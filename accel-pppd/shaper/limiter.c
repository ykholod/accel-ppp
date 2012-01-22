#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/pkt_cls.h>
#include <linux/pkt_sched.h>
#include <linux/tc_act/tc_mirred.h>
#include <linux/tc_act/tc_skbedit.h>

#include "log.h"
#include "ppp.h"

#include "shaper.h"
#include "tc_core.h"
#include "libnetlink.h"

#define TCA_BUF_MAX 64*1024
#define MAX_MSG 16384

static __thread struct {
		struct nlmsghdr 	n;
		struct tcmsg 		t;
		char buf[TCA_BUF_MAX];
} req;

struct qdisc_opt
{
	char *kind;
	int handle;
	int parent;
	double latency;
	int rate;
	int buffer;
	int quantum;
	int defcls;
	int (*qdisc)(struct qdisc_opt *opt, struct nlmsghdr *n);
};

static int qdisc_tbf(struct qdisc_opt *qopt, struct nlmsghdr *n)
{
	struct tc_tbf_qopt opt;
	__u32 rtab[256];
	int mtu = 0;
	int Rcell_log = -1;
	unsigned int linklayer = LINKLAYER_ETHERNET; /* Assume ethernet */
	struct rtattr *tail;

	memset(&opt, 0, sizeof(opt));

	opt.rate.rate = qopt->rate;
	opt.limit = (double)qopt->rate * qopt->latency + qopt->buffer;
	opt.rate.mpu = conf_mpu;
	if (tc_calc_rtable(&opt.rate, rtab, Rcell_log, mtu, linklayer) < 0) {
		log_ppp_error("shaper: failed to calculate rate table.\n");
		return -1;
	}
	opt.buffer = tc_calc_xmittime(opt.rate.rate, qopt->buffer);

	tail = NLMSG_TAIL(n);
	addattr_l(n, 1024, TCA_OPTIONS, NULL, 0);
	addattr_l(n, 2024, TCA_TBF_PARMS, &opt, sizeof(opt));
	addattr_l(n, 3024, TCA_TBF_RTAB, rtab, 1024);
	tail->rta_len = (void *) NLMSG_TAIL(n) - (void *) tail;

	return 0;
}

static int qdisc_htb_root(struct qdisc_opt *qopt, struct nlmsghdr *n)
{
	struct tc_htb_glob opt;
	struct rtattr *tail;

	memset(&opt,0,sizeof(opt));

	opt.rate2quantum = qopt->quantum;
	opt.version = 3;
	opt.defcls = qopt->defcls;
	
	tail = NLMSG_TAIL(n);
	addattr_l(n, 1024, TCA_OPTIONS, NULL, 0);
	addattr_l(n, 2024, TCA_HTB_INIT, &opt, NLMSG_ALIGN(sizeof(opt)));
	tail->rta_len = (void *) NLMSG_TAIL(n) - (void *) tail;
	return 0;
}

static int qdisc_htb_class(struct qdisc_opt *qopt, struct nlmsghdr *n)
{
	struct tc_htb_opt opt;
	__u32 rtab[256],ctab[256];
	int cell_log=-1,ccell_log = -1;
	unsigned mtu = 1600;
	unsigned int linklayer  = LINKLAYER_ETHERNET; /* Assume ethernet */
	struct rtattr *tail;

	memset(&opt, 0, sizeof(opt));
	
	opt.rate.rate = qopt->rate;
	opt.rate.mpu = conf_mpu;
	opt.ceil.rate = qopt->rate;
	opt.ceil.mpu = conf_mpu;
	
	if (tc_calc_rtable(&opt.rate, rtab, cell_log, mtu, linklayer) < 0) {
		log_ppp_error("shaper: failed to calculate rate table.\n");
		return -1;
	}
	opt.buffer = tc_calc_xmittime(opt.rate.rate, qopt->buffer);

	if (tc_calc_rtable(&opt.ceil, ctab, ccell_log, mtu, linklayer) < 0) {
		log_ppp_error("shaper: failed to calculate ceil rate table.\n");
		return -1;
	}
	opt.cbuffer = tc_calc_xmittime(opt.ceil.rate, qopt->buffer);
	
	tail = NLMSG_TAIL(n);
	addattr_l(n, 1024, TCA_OPTIONS, NULL, 0);
	addattr_l(n, 2024, TCA_HTB_PARMS, &opt, sizeof(opt));
	addattr_l(n, 3024, TCA_HTB_RTAB, rtab, 1024);
	addattr_l(n, 4024, TCA_HTB_CTAB, ctab, 1024);
	tail->rta_len = (void *) NLMSG_TAIL(n) - (void *) tail;
	return 0;
}

static int tc_qdisc_modify(struct rtnl_handle *rth, int ifindex, int cmd, unsigned flags, struct qdisc_opt *opt)
{
	memset(&req, 0, sizeof(req));

	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST|flags;
	req.n.nlmsg_type = cmd;
	req.t.tcm_family = AF_UNSPEC;

	req.t.tcm_ifindex = ifindex;

	if (opt->handle)
		req.t.tcm_handle = opt->handle;
	
	req.t.tcm_parent = opt->parent;
	
	if (opt->kind)
		addattr_l(&req.n, sizeof(req), TCA_KIND, opt->kind, strlen(opt->kind) + 1);

	if (opt->qdisc)
		opt->qdisc(opt, &req.n);

 	if (rtnl_talk(rth, &req.n, 0, 0, NULL, NULL, NULL, cmd == RTM_DELQDISC) < 0)
		return -1;

	return 0;
}

static int install_tbf(struct rtnl_handle *rth, int ifindex, int rate, int burst)
{
	struct qdisc_opt opt = {
		.kind = "tbf",
		.handle = 0x00010000,
		.parent = TC_H_ROOT,
		.rate = rate,
		.buffer = burst,
		.latency = conf_latency,
		.qdisc = qdisc_tbf,
	};

	return tc_qdisc_modify(rth, ifindex, RTM_NEWQDISC, NLM_F_EXCL|NLM_F_CREATE, &opt);
}

static int install_htb(struct rtnl_handle *rth, int ifindex, int rate, int burst)
{
	struct qdisc_opt opt1 = {
		.kind = "htb",
		.handle = 0x00010000,
		.parent = TC_H_ROOT,
		.quantum = conf_r2q,
		.defcls = 1,
		.qdisc = qdisc_htb_root,
	};
	
	struct qdisc_opt opt2 = {
		.kind = "htb",
		.handle = 0x00010001,
		.parent = 0x00010000,
		.rate = rate,
		.buffer = burst,
		.quantum = conf_quantum,
		.qdisc = qdisc_htb_class,
	};


	if (tc_qdisc_modify(rth, ifindex, RTM_NEWQDISC, NLM_F_EXCL|NLM_F_CREATE, &opt1))
		return -1;
	
	if (tc_qdisc_modify(rth, ifindex, RTM_NEWTCLASS, NLM_F_EXCL|NLM_F_CREATE, &opt2))
		return -1;
	
	return 0;
}

static int install_police(struct rtnl_handle *rth, int ifindex, int rate, int burst)
{
	__u32 rtab[256];
	struct rtattr *tail, *tail1, *tail2, *tail3;
	int Rcell_log = -1;
	int mtu = 0, flowid = 1;
	unsigned int linklayer  = LINKLAYER_ETHERNET; /* Assume ethernet */

	struct qdisc_opt opt1 = {
		.kind = "ingress",
		.handle = 0xffff0000,
		.parent = TC_H_INGRESS,
	};
	
	struct sel {
		struct tc_u32_sel sel;
		struct tc_u32_key key;
	} sel = {
		.sel.nkeys = 1,
		.sel.flags = TC_U32_TERMINAL,
		.key.off = 12,
	};
	
	struct tc_police police = {
		.action = TC_POLICE_SHOT,
		.rate.rate = rate,
		.rate.mpu = conf_mpu,
		.limit = (double)rate * conf_latency + burst,
		.burst = tc_calc_xmittime(rate, burst),
	};

	if (tc_qdisc_modify(rth, ifindex, RTM_NEWQDISC, NLM_F_EXCL|NLM_F_CREATE, &opt1))
		return -1;
	
	if (tc_calc_rtable(&police.rate, rtab, Rcell_log, mtu, linklayer) < 0) {
		log_ppp_error("shaper: failed to calculate ceil rate table.\n");
		return -1;
	}

	memset(&req, 0, sizeof(req));

	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST|NLM_F_EXCL|NLM_F_CREATE;
	req.n.nlmsg_type = RTM_NEWTFILTER;
	req.t.tcm_family = AF_UNSPEC;
	req.t.tcm_ifindex = ifindex;
	req.t.tcm_handle = 1;
	req.t.tcm_parent = 0xffff0000;
	req.t.tcm_info = TC_H_MAKE(1 << 16, ntohs(ETH_P_IP));
	
	addattr_l(&req.n, sizeof(req), TCA_KIND, "u32", 4);

	tail = NLMSG_TAIL(&req.n);
	addattr_l(&req.n, MAX_MSG, TCA_OPTIONS, NULL, 0);

	tail1 = NLMSG_TAIL(&req.n);
	addattr_l(&req.n, MAX_MSG, TCA_U32_ACT, NULL, 0);

	tail2 = NLMSG_TAIL(&req.n);
	addattr_l(&req.n, MAX_MSG, 1, NULL, 0);
	addattr_l(&req.n, MAX_MSG, TCA_ACT_KIND, "police", 7);

	tail3 = NLMSG_TAIL(&req.n);
	addattr_l(&req.n, MAX_MSG, TCA_ACT_OPTIONS, NULL, 0);
	addattr_l(&req.n, MAX_MSG, TCA_POLICE_TBF, &police, sizeof(police));
	addattr_l(&req.n, MAX_MSG, TCA_POLICE_RATE, rtab, 1024);
	tail3->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail3;
	
	tail2->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail2;
	
	tail1->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail1;

	addattr_l(&req.n, MAX_MSG, TCA_U32_CLASSID, &flowid, 4);
	addattr_l(&req.n, MAX_MSG, TCA_U32_SEL, &sel, sizeof(sel));
	tail->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail;
 	
	if (rtnl_talk(rth, &req.n, 0, 0, NULL, NULL, NULL, 0) < 0)
		return -1;

	return 0;
}

static int install_htb_ifb(struct rtnl_handle *rth, int ifindex, __u32 priority, int rate, int burst)
{
	struct rtattr *tail, *tail1, *tail2, *tail3;

	struct qdisc_opt opt1 = {
		.kind = "htb",
		.handle = 0x00010001 + priority,
		.parent = 0x00010000,
		.rate = rate,
		.buffer = burst,
		.quantum = conf_quantum,
		.qdisc = qdisc_htb_class,
	};
	
	struct qdisc_opt opt2 = {
		.kind = "ingress",
		.handle = 0xffff0000,
		.parent = TC_H_INGRESS,
	};
	
	struct sel {
		struct tc_u32_sel sel;
		struct tc_u32_key key;
	} sel = {
		.sel.nkeys = 1,
		.sel.flags = TC_U32_TERMINAL,
		.key.off = 0,
	};
	
	struct tc_skbedit p1 = {
		.action = TC_ACT_PIPE,
	};
	
	struct tc_mirred p2 = {
		.eaction = TCA_EGRESS_REDIR,
		.action = TC_ACT_STOLEN,
		.ifindex = conf_ifb_ifindex,
	};

	if (tc_qdisc_modify(rth, conf_ifb_ifindex, RTM_NEWTCLASS, NLM_F_EXCL|NLM_F_CREATE, &opt1))
		return -1;
	
	if (tc_qdisc_modify(rth, ifindex, RTM_NEWQDISC, NLM_F_EXCL|NLM_F_CREATE, &opt2))
		return -1;
	
	memset(&req, 0, sizeof(req));

	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST|NLM_F_EXCL|NLM_F_CREATE;
	req.n.nlmsg_type = RTM_NEWTFILTER;
	req.t.tcm_family = AF_UNSPEC;
	req.t.tcm_ifindex = ifindex;
	req.t.tcm_handle = 1;
	req.t.tcm_parent = 0xffff0000;
	req.t.tcm_info = TC_H_MAKE(1 << 16, ntohs(ETH_P_IP));
	
	addattr_l(&req.n, sizeof(req), TCA_KIND, "u32", 4);

	tail = NLMSG_TAIL(&req.n);
	addattr_l(&req.n, MAX_MSG, TCA_OPTIONS, NULL, 0);

	tail1 = NLMSG_TAIL(&req.n);
	addattr_l(&req.n, MAX_MSG, TCA_U32_ACT, NULL, 0);

	// action skbedit priority X pipe 
	tail2 = NLMSG_TAIL(&req.n);
	addattr_l(&req.n, MAX_MSG, 1, NULL, 0);
	addattr_l(&req.n, MAX_MSG, TCA_ACT_KIND, "skbedit", 8);

	tail3 = NLMSG_TAIL(&req.n);
	addattr_l(&req.n, MAX_MSG, TCA_ACT_OPTIONS, NULL, 0);
	addattr_l(&req.n, MAX_MSG, TCA_SKBEDIT_PARMS, &p1, sizeof(p1));
	addattr_l(&req.n, MAX_MSG, TCA_SKBEDIT_PRIORITY, &priority, sizeof(priority));
	tail3->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail3;
	
	tail2->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail2;
	
	tail1->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail1;

	// action mirred egress redirect dev ifb0
	tail2 = NLMSG_TAIL(&req.n);
	addattr_l(&req.n, MAX_MSG, 2, NULL, 0);
	addattr_l(&req.n, MAX_MSG, TCA_ACT_KIND, "mirred", 7);

	tail3 = NLMSG_TAIL(&req.n);
	addattr_l(&req.n, MAX_MSG, TCA_ACT_OPTIONS, NULL, 0);
	addattr_l(&req.n, MAX_MSG, TCA_MIRRED_PARMS, &p2, sizeof(p2));
	tail3->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail3;
	
	tail2->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail2;
	
	tail1->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail1;
  //

	addattr32(&req.n, 4096, TCA_U32_CLASSID, 1);
	addattr_l(&req.n, MAX_MSG, TCA_U32_SEL, &sel, sizeof(sel));
	tail->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail;
 	
	if (rtnl_talk(rth, &req.n, 0, 0, NULL, NULL, NULL, 0) < 0)
		return -1;

	return 0;
}

static int remove_root(struct rtnl_handle *rth, int ifindex)
{
	struct qdisc_opt opt = {
		.handle = 0x00010000,
		.parent = TC_H_ROOT,
	};
	
	return tc_qdisc_modify(rth, ifindex, RTM_DELQDISC, 0, &opt);
}

static int remove_ingress(struct rtnl_handle *rth, int ifindex)
{
	struct qdisc_opt opt = {
		.handle = 0xffff0000,
		.parent = TC_H_INGRESS,
	};
	
	return tc_qdisc_modify(rth, ifindex, RTM_DELQDISC, 0, &opt);
}

static int remove_htb_ifb(struct rtnl_handle *rth, int ifindex, int priority)
{
	struct qdisc_opt opt = {
		.handle = 0x00010001 + priority,
		.parent = 0x00010000,
	};
	
	return tc_qdisc_modify(rth, conf_ifb_ifindex, RTM_DELTCLASS, 0, &opt);
}

int install_limiter(struct ppp_t *ppp, int down_speed, int down_burst, int up_speed, int up_burst)
{
	struct rtnl_handle rth;
	int r;
	
	if (rtnl_open(&rth, 0)) {
		log_ppp_error("shaper: cannot open rtnetlink\n");
		return -1;
	}

	down_speed = down_speed * 1000 / 8;
	down_burst = down_burst ? down_burst : conf_down_burst_factor * down_speed;
	up_speed = up_speed * 1000 / 8;
	up_burst = up_burst ? up_burst : conf_up_burst_factor * up_speed;

	if (conf_down_limiter == LIM_TBF)
		r = install_tbf(&rth, ppp->ifindex, down_speed, down_burst);
	else
		r = install_htb(&rth, ppp->ifindex, down_speed, down_burst);

	if (conf_up_limiter == LIM_POLICE)
		r = install_police(&rth, ppp->ifindex, up_speed, up_burst);
	else
		r = install_htb_ifb(&rth, ppp->ifindex, ppp->unit_idx + 1, down_speed, down_burst);
	
	rtnl_close(&rth);

	return r;
}

int remove_limiter(struct ppp_t *ppp)
{
	struct rtnl_handle rth;

	if (rtnl_open(&rth, 0)) {
		log_ppp_error("shaper: cannot open rtnetlink\n");
		return -1;
	}

	remove_root(&rth, ppp->ifindex);
	remove_ingress(&rth, ppp->ifindex);
	
	if (conf_up_limiter == LIM_HTB)
		remove_htb_ifb(&rth, ppp->ifindex, ppp->unit_idx + 1);

	return 0;
}

int init_ifb(const char *name)
{
	struct rtnl_handle rth;
	struct rtattr *tail;
	struct ifreq ifr;
	int r;
	
	struct qdisc_opt opt = {
		.kind = "htb",
		.handle = 0x00010000,
		.parent = TC_H_ROOT,
		.quantum = conf_r2q,
		.qdisc = qdisc_htb_root,
	};
	
	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, name);

	if (ioctl(sock_fd, SIOCGIFINDEX, &ifr)) {
		log_emerg("shaper: ioctl(SIOCGIFINDEX): %s\n", strerror(errno));
		return -1;
	}

	conf_ifb_ifindex = ifr.ifr_ifindex;
	
	ifr.ifr_flags |= IFF_UP;

	if (ioctl(sock_fd, SIOCSIFFLAGS, &ifr)) {
		log_emerg("shaper: ioctl(SIOCSIFINDEX): %s\n", strerror(errno));
		return -1;
	}

	if (rtnl_open(&rth, 0)) {
		log_emerg("shaper: cannot open rtnetlink\n");
		return -1;
	}

	tc_qdisc_modify(&rth, conf_ifb_ifindex, RTM_DELQDISC, 0, &opt);

	r = tc_qdisc_modify(&rth, conf_ifb_ifindex, RTM_NEWQDISC, NLM_F_CREATE | NLM_F_REPLACE, &opt);
	if (r)
		goto out;

	memset(&req, 0, sizeof(req));

	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST|NLM_F_EXCL|NLM_F_CREATE;
	req.n.nlmsg_type = RTM_NEWTFILTER;
	req.t.tcm_family = AF_UNSPEC;
	req.t.tcm_ifindex = conf_ifb_ifindex;
	req.t.tcm_handle = 1;
	req.t.tcm_parent = 0x00010000;
	req.t.tcm_info = TC_H_MAKE(1 << 16, ntohs(ETH_P_IP));
	
	addattr_l(&req.n, sizeof(req), TCA_KIND, "flow", 5);

	tail = NLMSG_TAIL(&req.n);
	addattr_l(&req.n, 4096, TCA_OPTIONS, NULL, 0);
	addattr32(&req.n, 4096, TCA_FLOW_KEYS, 1 << FLOW_KEY_PRIORITY);
	addattr32(&req.n, 4096, TCA_FLOW_MODE, FLOW_MODE_MAP);
	tail->rta_len = (void *)NLMSG_TAIL(&req.n) - (void *)tail;

	r = rtnl_talk(&rth, &req.n, 0, 0, NULL, NULL, NULL, 0);

out:
	rtnl_close(&rth);

	return r;
}
