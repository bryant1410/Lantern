/* vi: set sw=4 ts=4: */
/*
 * iplink.c "ip link".
 *
 * Authors: Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 *
 * Licensed under GPLv2 or later, see file LICENSE in this tarball for details.
 */

//#include <sys/ioctl.h>
//#include <sys/socket.h>
#include <net/if.h>
#include <net/if_packet.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>

#include "ip_common.h"  /* #include "libbb.h" is inside */
#include "rt_names.h"
#include "utils.h"

/* taken from linux/sockios.h */
#define SIOCSIFNAME	0x8923		/* set interface name */

static void on_off(const char *msg) ATTRIBUTE_NORETURN;
static void on_off(const char *msg)
{
	bb_error_msg_and_die("error: argument of \"%s\" must be \"on\" or \"off\"", msg);
}

/* Exits on error */
static int get_ctl_fd(void)
{
	int fd;

	fd = socket(PF_INET, SOCK_DGRAM, 0);
	if (fd >= 0)
		return fd;
	fd = socket(PF_PACKET, SOCK_DGRAM, 0);
	if (fd >= 0)
		return fd;
	return xsocket(PF_INET6, SOCK_DGRAM, 0);
}

/* Exits on error */
static void do_chflags(char *dev, uint32_t flags, uint32_t mask)
{
	struct ifreq ifr;
	int fd;

	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
	fd = get_ctl_fd();
	if (ioctl(fd, SIOCGIFFLAGS, &ifr)) {
		bb_perror_msg_and_die("SIOCGIFFLAGS");
	}
	if ((ifr.ifr_flags ^ flags) & mask) {
		ifr.ifr_flags &= ~mask;
		ifr.ifr_flags |= mask & flags;
		if (ioctl(fd, SIOCSIFFLAGS, &ifr))
			bb_perror_msg_and_die("SIOCSIFFLAGS");
	}
	close(fd);
}

/* Exits on error */
static void do_changename(char *dev, char *newdev)
{
	struct ifreq ifr;
	int fd;
	int err;

	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
	strncpy(ifr.ifr_newname, newdev, sizeof(ifr.ifr_newname));
	fd = get_ctl_fd();
	err = ioctl(fd, SIOCSIFNAME, &ifr);
	if (err) {
		bb_perror_msg_and_die("SIOCSIFNAME");
	}
	close(fd);
}

/* Exits on error */
static void set_qlen(char *dev, int qlen)
{
	struct ifreq ifr;
	int s;

	s = get_ctl_fd();
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
	ifr.ifr_qlen = qlen;
	if (ioctl(s, SIOCSIFTXQLEN, &ifr) < 0) {
		bb_perror_msg_and_die("SIOCSIFXQLEN");
	}
	close(s);
}

/* Exits on error */
static void set_mtu(char *dev, int mtu)
{
	struct ifreq ifr;
	int s;

	s = get_ctl_fd();
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
	ifr.ifr_mtu = mtu;
	if (ioctl(s, SIOCSIFMTU, &ifr) < 0) {
		bb_perror_msg_and_die("SIOCSIFMTU");
	}
	close(s);
}

#ifndef RTM_SETLINK
#define RTM_SETLINK (RTM_BASE+3)
#endif

/*Support ip link set vrf xxx*/
static int set_vrf(char *dev, int vrf)
{
	struct rtnl_handle rth;
	struct {
		struct nlmsghdr		n;
		struct ifinfomsg	r;
		char			buf[16];
	} req;

	memset(&req, 0, sizeof(req));

	req.n.nlmsg_type = RTM_SETLINK;
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.n.nlmsg_flags = NLM_F_REQUEST;

	xrtnl_open(&rth);

	ll_init_map(&rth);
#if 0 /*m by zhuj at 2010-11-27 for _NEW_VERSION_LINUX_*/
	req.r.ifi_family = AF_UNSPEC;
	req.r.ifi_index = xll_name_to_index(dev);
	addattr32(&req.n, sizeof(req), IFLA_VRF, vrf);
#else
    req.r.ifi_family = AF_UNSPEC;
    req.r.ifi_index = xll_name_to_index(dev);
    addattr_l(&req.n, sizeof(req), IFLA_NS_ID, &vrf, 4);
#endif

	if (rtnl_talk(&rth, &req.n, 0, 0, NULL, NULL, NULL) < 0)
		return 2;

	return (0);
}

/* Exits on error */
static int get_address(char *dev, int *htype)
{
	struct ifreq ifr;
	struct sockaddr_ll me;
	socklen_t alen;
	int s;

	s = xsocket(PF_PACKET, SOCK_DGRAM, 0);

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
		bb_perror_msg_and_die("SIOCGIFINDEX");
	}

	memset(&me, 0, sizeof(me));
	me.sll_family = AF_PACKET;
	me.sll_ifindex = ifr.ifr_ifindex;
	me.sll_protocol = htons(ETH_P_LOOP);
	xbind(s, (struct sockaddr*)&me, sizeof(me));

	alen = sizeof(me);
	if (getsockname(s, (struct sockaddr*)&me, &alen) == -1) {
		bb_perror_msg_and_die("getsockname");
	}
	close(s);
	*htype = me.sll_hatype;
	return me.sll_halen;
}

/* Exits on error */
static void parse_address(char *dev, int hatype, int halen, char *lla, struct ifreq *ifr)
{
	int alen;

	memset(ifr, 0, sizeof(*ifr));
	strncpy(ifr->ifr_name, dev, sizeof(ifr->ifr_name));
	ifr->ifr_hwaddr.sa_family = hatype;
	alen = ll_addr_a2n((unsigned char *)(ifr->ifr_hwaddr.sa_data), 14, lla);
	if (alen < 0)
		exit(1);
	if (alen != halen) {
		bb_error_msg_and_die("wrong address (%s) length: expected %d bytes", lla, halen);
	}
}

/* Exits on error */
static void set_address(struct ifreq *ifr, int brd)
{
	int s;

	s = get_ctl_fd();
	if (ioctl(s, brd ? SIOCSIFHWBROADCAST  :SIOCSIFHWADDR, ifr) < 0) {
		bb_perror_msg_and_die(brd ? "SIOCSIFHWBROADCAST" : "SIOCSIFHWADDR");
	}
	close(s);
}


/* Return value becomes exitcode. It's okay to not return at all */
static int do_set(int argc, char **argv)
{
	char *dev = NULL;
	int vrf = -1;
	uint32_t mask = 0;
	uint32_t flags = 0;
	int qlen = -1;
	int mtu = -1;
	char *newaddr = NULL;
	char *newbrd = NULL;
	struct ifreq ifr0, ifr1;
	char *newname = NULL;
	int htype, halen;
	static const char * const keywords[] = {
		"up", "down", "name", "mtu", "multicast", "arp", "addr", "dev",
		"on", "off", "vrf", NULL
	};
	enum { ARG_up = 1, ARG_down, ARG_name, ARG_mtu, ARG_multicast, ARG_arp,
		ARG_addr, ARG_dev, PARM_on, PARM_off, ARG_vrf };
	smalluint key;

	while (argc > 0) {
		key = index_in_str_array(keywords, *argv) + 1;
		if (key == ARG_up) {
			mask |= IFF_UP;
			flags |= IFF_UP;
		} else if (key == ARG_down) {
			mask |= IFF_UP;
			flags &= ~IFF_UP;
		} else if (key == ARG_name) {
			NEXT_ARG();
			newname = *argv;
		} else if (key == ARG_mtu) {
			NEXT_ARG();
			if (mtu != -1)
				duparg("mtu", *argv);
			if (get_integer(&mtu, *argv, 0))
				invarg(*argv, "mtu");
		} else if (key == ARG_multicast) {
			NEXT_ARG();
			mask |= IFF_MULTICAST;
			key = index_in_str_array(keywords, *argv) + 1;
			if (key == PARM_on) {
				flags |= IFF_MULTICAST;
			} else if (key == PARM_off) {
				flags &= ~IFF_MULTICAST;
			} else
				on_off("multicast");
		} else if (key == ARG_arp) {
			NEXT_ARG();
			mask |= IFF_NOARP;
			key = index_in_str_array(keywords, *argv) + 1;
			if (key == PARM_on) {
				flags &= ~IFF_NOARP;
			} else if (key == PARM_off) {
				flags |= IFF_NOARP;
			} else
				on_off("arp");
		} else if (key == ARG_vrf) {
			NEXT_ARG();
			if (vrf != -1)
				duparg("vrf", *argv);
			if (get_integer(&vrf, *argv, 0))
				invarg("Invalid \"vrf\" value\n", *argv);
		}else if (key == ARG_addr) {
			NEXT_ARG();
			newaddr = *argv;
		} else {
			if (key == ARG_dev) {
				NEXT_ARG();
			}
			if (dev)
				duparg2("dev", *argv);
			dev = *argv;
		}
		argc--; argv++;
	}

	if (!dev) {
		bb_error_msg_and_die(bb_msg_requires_arg, "\"dev\"");
	}

	if (newaddr || newbrd) {
		halen = get_address(dev, &htype);
		if (newaddr) {
			parse_address(dev, htype, halen, newaddr, &ifr0);
		}
		if (newbrd) {
			parse_address(dev, htype, halen, newbrd, &ifr1);
		}
	}

	if (newname && strcmp(dev, newname)) {
		do_changename(dev, newname);
		dev = newname;
	}
	if (qlen != -1) {
		set_qlen(dev, qlen);
	}
	if (mtu != -1) {
		set_mtu(dev, mtu);
	}
	if (vrf != -1) { 
		if (set_vrf(dev, vrf) < 0)
			return -1; 
	}
	if (newaddr || newbrd) {
		if (newbrd) {
			set_address(&ifr1, 1);
		}
		if (newaddr) {
			set_address(&ifr0, 0);
		}
	}
	if (mask)
		do_chflags(dev, flags, mask);
	return 0;
}

static int ipaddr_list_link(int argc, char **argv)
{
	preferred_family = AF_PACKET;
	return ipaddr_list_or_flush(argc, argv, 0);
}

/* Return value becomes exitcode. It's okay to not return at all */
int do_iplink(int argc, char **argv)
{
	static const char * const keywords[] = {
		"set", "show", "lst", "list", NULL
	};
	smalluint key;
	if (argc <= 0)
		return ipaddr_list_link(0, NULL);
	key = index_in_substr_array(keywords, *argv) + 1;
	if (key == 0)
		bb_error_msg_and_die(bb_msg_invalid_arg, *argv, applet_name);
	argc--; argv++;
	if (key == 1) /* set */
		return do_set(argc, argv);
	else /* show, lst, list */
		return ipaddr_list_link(argc, argv);
}
