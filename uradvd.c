// SPDX-License-Identifier: BSD-2-Clause
/*
  Copyright (c) 2014, Matthias Schiffer <mschiffer@universe-factory.net>
  All rights reserved.
*/


#define _GNU_SOURCE

#include <errno.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <net/if.h>

#include <netinet/in.h>
#include <netinet/icmp6.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/stat.h>


#define MAX_PREFIXES 8
#define MAX_RDNSS 3

/* These are in seconds */
#define AdvValidLifetime 86400u
#define AdvPreferredLifetime 14400u
#define AdvDefaultLifetime 0u
#define AdvCurHopLimit 64u
#define AdvRDNSSLifetime 1200u

#define MinRtrAdvInterval 200u
#define MaxRtrAdvInterval 600u

/* And these in milliseconds */
#define MAX_RA_DELAY_TIME 500u
#define MIN_DELAY_BETWEEN_RAS 3000u


struct icmpv6_opt {
	uint8_t type;
	uint8_t length;
	uint8_t data[6];
};


struct iface {
	bool ok;
	unsigned int ifindex;
	struct in6_addr ifaddr;
	uint8_t mac[6];
};

struct __attribute__((__packed__)) nd_opt_rdnss {
	uint8_t nd_opt_rdnss_type;
	uint8_t nd_opt_rdnss_len;
	uint16_t nd_opt_rdnss_reserved;
	uint32_t nd_opt_rdnss_lifetime;
};

static struct global {
	struct iface iface;

	struct timespec time;
	struct timespec next_advert;
	struct timespec next_advert_earliest;

	int icmp_sock;
	int rtnl_sock;

	const char *ifname;

	uint16_t adv_default_lifetime;

	size_t n_prefixes;
	struct in6_addr prefixes[MAX_PREFIXES];
	bool prefixes_onlink[MAX_PREFIXES];

	size_t n_rdnss;
	struct in6_addr rdnss[MAX_RDNSS];
} G = {
	.rtnl_sock = -1,
	.icmp_sock = -1,
	.adv_default_lifetime = AdvDefaultLifetime,
};


static inline void print_error(const char *prefix, const char *message, int err) {
	if (err)
		fprintf(stderr, "uradvd: %s: %s: %s\n", prefix, message, strerror(err));
	else
		fprintf(stderr, "uradvd: %s: %s\n", prefix, message);
}

static inline void exit_error(const char *message, int err) {
	print_error("error", message, err);
	exit(1);
}

static inline void exit_errno(const char *message) {
	exit_error(message, errno);
}

static inline void warn_error(const char *message, int err) {
	print_error("error", message, err);
}

static inline void warn_errno(const char *message) {
	warn_error(message, errno);
}


static inline void update_time(void) {
	clock_gettime(CLOCK_MONOTONIC, &G.time);
}

/* Compares two timespecs and returns true if tp1 is after tp2 */
static inline bool timespec_after(const struct timespec *tp1, const struct timespec *tp2) {
	return (tp1->tv_sec > tp2->tv_sec ||
		(tp1->tv_sec == tp2->tv_sec && tp1->tv_nsec > tp2->tv_nsec));
}

/* Returns (tp1 - tp2) in milliseconds  */
static inline int timespec_diff(const struct timespec *tp1, const struct timespec *tp2) {
	return ((tp1->tv_sec - tp2->tv_sec))*1000 + (tp1->tv_nsec - tp2->tv_nsec)/1e6;
}

static inline void timespec_add(struct timespec *tp, unsigned int ms) {
	tp->tv_sec += ms/1000;
	tp->tv_nsec += (ms%1000) * 1e6;

	if (tp->tv_nsec >= 1e9) {
		tp->tv_nsec -= 1e9;
		tp->tv_sec++;
	}
}


static inline int setsockopt_int(int socket, int level, int option, int value) {
	return setsockopt(socket, level, option, &value, sizeof(value));
}


static void init_random(void) {
	unsigned int seed;
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		exit_errno("can't open /dev/urandom");

	if (read(fd, &seed, sizeof(seed)) != sizeof(seed))
		exit_errno("can't read from /dev/urandom");

	close(fd);

	srandom(seed);
}

static inline int rand_range(int min, int max) {
	unsigned int r = (unsigned int)random();
	return (r%(max-min) + min);
}

static void init_icmp(void) {
	G.icmp_sock = socket(AF_INET6, SOCK_RAW|SOCK_NONBLOCK, IPPROTO_ICMPV6);
	if (G.icmp_sock < 0)
		exit_errno("can't open ICMP socket");

	setsockopt_int(G.icmp_sock, IPPROTO_RAW, IPV6_CHECKSUM, 2);

	setsockopt_int(G.icmp_sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, 255);
	setsockopt_int(G.icmp_sock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, 1);

	setsockopt_int(G.icmp_sock, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, 1);

	struct icmp6_filter filter;
	ICMP6_FILTER_SETBLOCKALL(&filter);
	ICMP6_FILTER_SETPASS(ND_ROUTER_SOLICIT, &filter);
	setsockopt(G.icmp_sock, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter));
}

static void init_rtnl(void) {
	G.rtnl_sock = socket(AF_NETLINK, SOCK_DGRAM|SOCK_NONBLOCK, NETLINK_ROUTE);
	if (G.rtnl_sock < 0)
		exit_errno("can't open RTNL socket");

	struct sockaddr_nl snl = {
		.nl_family = AF_NETLINK,
		.nl_groups = RTMGRP_LINK | RTMGRP_IPV6_IFADDR,
	};
	if (bind(G.rtnl_sock, (struct sockaddr *)&snl, sizeof(snl)) < 0)
		exit_errno("can't bind RTNL socket");
}


static void schedule_advert(bool nodelay) {
	struct timespec t = G.time;

	if (nodelay)
		timespec_add(&t, rand_range(0, MAX_RA_DELAY_TIME));
	else
		timespec_add(&t, rand_range(MinRtrAdvInterval*1000, MaxRtrAdvInterval*1000));

	if (timespec_after(&G.next_advert_earliest, &t))
		t = G.next_advert_earliest;

	if (!nodelay || timespec_after(&G.next_advert, &t))
		G.next_advert = t;
}


static int join_multicast(void) {
	struct ipv6_mreq mreq = {
		.ipv6mr_multiaddr = {
			.s6_addr = {
				/* all-routers address */
				0xff, 0x02, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x02,
			}
		},
		.ipv6mr_interface = G.iface.ifindex,
	};

	if (setsockopt(G.icmp_sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0) {
		return 2;
	}
	else if (errno != EADDRINUSE) {
		warn_errno("can't join multicast group");
		return 0;
	}

	return 1;
}

static void update_interface(void) {
	struct iface old;

	memcpy(&old, &G.iface, sizeof(struct iface));
	memset(&G.iface, 0, sizeof(struct iface));

	/* Update ifindex */
	G.iface.ifindex = if_nametoindex(G.ifname);
	if (!G.iface.ifindex)
		return;

	/* Update MAC address */
	struct ifreq ifr = {};
	strncpy(ifr.ifr_name, G.ifname, sizeof(ifr.ifr_name)-1);
	if (ioctl(G.icmp_sock, SIOCGIFHWADDR, &ifr) < 0)
		return;

	memcpy(G.iface.mac, ifr.ifr_hwaddr.sa_data, sizeof(G.iface.mac));

	struct ifaddrs *addrs, *addr;
	if (getifaddrs(&addrs) < 0) {
		warn_errno("getifaddrs");
		return;
	}

	memset(&G.iface.ifaddr, 0, sizeof(G.iface.ifaddr));

	for (addr = addrs; addr; addr = addr->ifa_next) {
		if (!addr->ifa_addr || addr->ifa_addr->sa_family != AF_INET6)
			continue;

		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr->ifa_addr;
		if (!IN6_IS_ADDR_LINKLOCAL(&in6->sin6_addr))
			continue;

		if (strncmp(addr->ifa_name, G.ifname, IFNAMSIZ-1) != 0)
			continue;

		G.iface.ifaddr = in6->sin6_addr;
	}

	freeifaddrs(addrs);

	if (IN6_IS_ADDR_UNSPECIFIED(&G.iface.ifaddr))
		return;

	int joined = join_multicast();
	if (!joined)
		return;

	setsockopt(G.icmp_sock, SOL_SOCKET, SO_BINDTODEVICE, G.ifname, strnlen(G.ifname, IFNAMSIZ-1));

	G.iface.ok = true;

	if (memcmp(&old, &G.iface, sizeof(struct iface)) != 0 || joined == 2)
		schedule_advert(true);
}


static bool handle_rtnl_link(uint16_t type, const struct ifinfomsg *msg) {
	switch (type) {
	case RTM_NEWLINK:
		if (!G.iface.ok)
			return true;

		break;

	case RTM_SETLINK:
		if ((unsigned)msg->ifi_index == G.iface.ifindex)
			return true;

		if (!G.iface.ok)
			return true;

		break;

	case RTM_DELLINK:
		if (G.iface.ok && (unsigned)msg->ifi_index == G.iface.ifindex)
			return true;
	}

	return false;
}

static bool handle_rtnl_addr(uint16_t type, const struct ifaddrmsg *msg) {
	switch (type) {
	case RTM_NEWADDR:
		if (!G.iface.ok && (unsigned)msg->ifa_index == G.iface.ifindex)
			return true;

		break;

	case RTM_DELADDR:
		if (G.iface.ok && (unsigned)msg->ifa_index == G.iface.ifindex)
			return true;
	}

	return false;
}

static bool handle_rtnl_msg(uint16_t type, const void *data) {
	switch (type) {
	case RTM_NEWLINK:
	case RTM_DELLINK:
	case RTM_SETLINK:
		return handle_rtnl_link(type, data);

	case RTM_NEWADDR:
	case RTM_DELADDR:
		return handle_rtnl_addr(type, data);

	default:
		return false;
	}
}

static void handle_rtnl(void) {
	char buffer[4096];

	ssize_t len = recv(G.rtnl_sock, buffer, sizeof(buffer), 0);
	if (len < 0) {
		warn_errno("recv");
		return;
	}

	const struct nlmsghdr *nh;
	for (nh = (struct nlmsghdr *)buffer; NLMSG_OK(nh, len); nh = NLMSG_NEXT(nh, len)) {
		switch (nh->nlmsg_type) {
		case NLMSG_DONE:
			return;

		case NLMSG_ERROR:
			exit_error("netlink error", 0);

		default:
			if (handle_rtnl_msg(nh->nlmsg_type, NLMSG_DATA(nh))) {
				update_interface();
				return;
			}
		}
	}
}

static void add_pktinfo(struct msghdr *msg) {
	struct cmsghdr *cmsg = (struct cmsghdr*)((char*)msg->msg_control + msg->msg_controllen);

	cmsg->cmsg_level = IPPROTO_IPV6;
	cmsg->cmsg_type = IPV6_PKTINFO;
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));

	msg->msg_controllen += cmsg->cmsg_len;

	struct in6_pktinfo pktinfo = {
		.ipi6_addr = G.iface.ifaddr,
		.ipi6_ifindex = G.iface.ifindex,
	};

	memcpy(CMSG_DATA(cmsg), &pktinfo, sizeof(pktinfo));
}


static void handle_solicit(void) {
	struct sockaddr_in6 addr;

	uint8_t buffer[1500] __attribute__((aligned(8)));
	struct iovec vec = { .iov_base = buffer, .iov_len = sizeof(buffer) };

	uint8_t cbuf[1024] __attribute__((aligned(8)));


	struct msghdr msg = {
		.msg_name = &addr,
		.msg_namelen = sizeof(addr),
		.msg_iov = &vec,
		.msg_iovlen = 1,
		.msg_control = cbuf,
		.msg_controllen = sizeof(cbuf),
	};

	ssize_t len = recvmsg(G.icmp_sock, &msg, 0);
	if (len < (ssize_t)sizeof(struct nd_router_solicit)) {
		if (len < 0)
			warn_errno("recvmsg");

		return;
	}

	struct cmsghdr *cmsg;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level != IPPROTO_IPV6)
			continue;

		if (cmsg->cmsg_type != IPV6_HOPLIMIT)
			continue;

		if (*(int*)CMSG_DATA(cmsg) != 255)
			return;

		break;
	}

	const struct nd_router_solicit *s = (struct nd_router_solicit *)buffer;
	if (s->nd_rs_hdr.icmp6_type != ND_ROUTER_SOLICIT || s->nd_rs_hdr.icmp6_code != 0)
		return;

	const struct icmpv6_opt *opt = (struct icmpv6_opt *)(buffer + sizeof(struct nd_router_solicit)), *end = (struct icmpv6_opt *)(buffer+len);

	for (; opt < end; opt += opt->length) {
		if (opt+1 < end)
			return;

		if (!opt->length)
			return;

		if (opt+opt->length < end)
			return;

		if (opt->type == ND_OPT_SOURCE_LINKADDR && IN6_IS_ADDR_UNSPECIFIED(&addr.sin6_addr))
			return;
	}

	if (opt != end)
		return;

	schedule_advert(true);
}

static void send_advert(void) {
	if (!G.iface.ok)
		return;

	struct nd_router_advert advert = {
		.nd_ra_hdr = {
			.icmp6_type = ND_ROUTER_ADVERT,
			.icmp6_dataun.icmp6_un_data8 = {AdvCurHopLimit, 0 /* Flags */, (G.adv_default_lifetime>>8) & 0xff, G.adv_default_lifetime & 0xff },
		},
	};

	struct icmpv6_opt lladdr = {ND_OPT_SOURCE_LINKADDR, 1, {}};
	memcpy(lladdr.data, G.iface.mac, sizeof(G.iface.mac));

	struct nd_opt_prefix_info prefixes[G.n_prefixes];

	size_t i;
	for (i = 0; i < G.n_prefixes; i++) {
		uint8_t flags = ND_OPT_PI_FLAG_AUTO;

		if (G.prefixes_onlink[i])
			flags |= ND_OPT_PI_FLAG_ONLINK;

		prefixes[i] = (struct nd_opt_prefix_info){
			.nd_opt_pi_type = ND_OPT_PREFIX_INFORMATION,
			.nd_opt_pi_len = 4,
			.nd_opt_pi_prefix_len = 64,
			.nd_opt_pi_flags_reserved = flags,
			.nd_opt_pi_valid_time = htonl(AdvValidLifetime),
			.nd_opt_pi_preferred_time = htonl(AdvPreferredLifetime),
			.nd_opt_pi_prefix = G.prefixes[i],
		};
	}

	struct nd_opt_rdnss rdnss = {};
	uint8_t rdnss_ips[G.n_rdnss][16];

	if (G.n_rdnss > 0) {
		rdnss.nd_opt_rdnss_type = 25;
		rdnss.nd_opt_rdnss_len = 1 + 2 * G.n_rdnss;
		rdnss.nd_opt_rdnss_lifetime = htonl(AdvRDNSSLifetime);

		for (i = 0; i < G.n_rdnss; i++)
			memcpy(rdnss_ips[i], G.rdnss[i].s6_addr, 16);
	}

	struct iovec vec[5] = {
		{ .iov_base = &advert, .iov_len = sizeof(advert) },
		{ .iov_base = &lladdr, .iov_len = sizeof(lladdr) },
		{ .iov_base = prefixes, .iov_len = sizeof(prefixes) },
		{ .iov_base = &rdnss, .iov_len = sizeof(rdnss) },
		{ .iov_base = rdnss_ips, .iov_len = sizeof(rdnss_ips) }
	};

	struct sockaddr_in6 addr = {
		.sin6_family = AF_INET6,
		.sin6_addr = {
			.s6_addr = {
				/* all-nodes address */
				0xff, 0x02, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x01,
			}
		},
		.sin6_scope_id = G.iface.ifindex,
	};

	uint8_t cbuf[1024] __attribute__((aligned(8))) = {};

	struct msghdr msg = {
		.msg_name = &addr,
		.msg_namelen = sizeof(addr),
		.msg_iov = vec,
		.msg_iovlen = G.n_rdnss > 0 ? 5 : 3,
		.msg_control = cbuf,
		.msg_controllen = 0,
		.msg_flags = 0,
	};

	add_pktinfo(&msg);

	if (sendmsg(G.icmp_sock, &msg, 0) < 0) {
		G.iface.ok = false;
		return;
	}

	G.next_advert_earliest = G.time;
	timespec_add(&G.next_advert_earliest, MIN_DELAY_BETWEEN_RAS);

	schedule_advert(false);
}


static void usage(void) {
	fprintf(stderr, "Usage: uradvd [-h] -i <interface> -a/-p <prefix> [ -a/-p <prefix> ... ] [ --default-lifetime <seconds> ] [ --rdnss <ip> ... ]\n");
}

static void add_rdnss(const char *ip) {
	if (G.n_rdnss == MAX_RDNSS) {
		fprintf(stderr, "uradvd: error: maximum number of RDNSS IPs is %i.\n", MAX_RDNSS);
		exit(1);
	}

	if (inet_pton(AF_INET6, ip, &G.rdnss[G.n_rdnss]) != 1) {
		fprintf(stderr, "uradvd: error: invalid RDNSS IP address %s.\n", ip);
		exit(1);
	}

	G.n_rdnss++;
}

static void add_prefix(const char *prefix, bool adv_onlink) {
	if (G.n_prefixes == MAX_PREFIXES) {
		fprintf(stderr, "uradvd: error: maximum number of prefixes is %i.\n", MAX_PREFIXES);
		exit(1);
	}

	const size_t len = strlen(prefix)+1;
	char prefix2[len];
	memcpy(prefix2, prefix, len);

	char *slash = strchr(prefix2, '/');
	if (slash) {
		*slash = 0;
		if (strcmp(slash+1, "64") != 0)
			goto error;
	}

	if (inet_pton(AF_INET6, prefix2, &G.prefixes[G.n_prefixes]) != 1)
		goto error;

	static const uint8_t zero[8] = {};
	if (memcmp(G.prefixes[G.n_prefixes].s6_addr + 8, zero, 8) != 0)
		goto error;

	G.prefixes_onlink[G.n_prefixes] = adv_onlink;

	G.n_prefixes++;
	return;

error:
	fprintf(stderr, "uradvd: error: invalid prefix %s (only prefixes of length 64 are supported).\n", prefix);
	exit(1);
}

static void parse_cmdline(int argc, char *argv[]) {
	int c;
	char *endptr;
	unsigned long val;

	static struct option long_options[] =
	{
		{"default-lifetime", required_argument, 0, 0},
		{"rdnss", required_argument, 0, 1},
		{0, 0, 0, 0}
	};

	int option_index = 0;

	while ((c = getopt_long(argc, argv, "i:a:p:h", long_options, &option_index)) != -1) {
		switch(c) {
		case 0: // --default-lifetime
			val = strtoul(optarg, &endptr, 0);

			if (!*optarg || *endptr || val > UINT16_MAX)
				exit_error("invalid default lifetime\n", 0);

			G.adv_default_lifetime = val;

			break;

		case 1: // --rdnss
			add_rdnss(optarg);
			break;

		case 'i':
			if (G.ifname)
				exit_error("multiple interfaces are not supported.\n", 0);

			G.ifname = optarg;

			break;

		case 'a':
			add_prefix(optarg, false);
			break;

		case 'p':
			add_prefix(optarg, true);
			break;

		case 'h':
			usage();
			exit(0);

		default:
			usage();
			exit(1);
		}
	}
}

int main(int argc, char *argv[]) {
	parse_cmdline(argc, argv);

	if (!G.ifname || !G.n_prefixes)
		exit_error("interface and prefix arguments are required.\n", 0);

	init_random();
	init_icmp();
	init_rtnl();

	update_time();
	G.next_advert = G.next_advert_earliest = G.time;

	update_interface();

	while (true) {
		struct pollfd fds[2] = {
			{ .fd = G.icmp_sock, .events = POLLIN },
			{ .fd = G.rtnl_sock, .events = POLLIN },
		};

		int timeout = -1;

		if (G.iface.ok) {
			timeout = timespec_diff(&G.next_advert, &G.time);

			if (timeout < 0)
				timeout = 0;
		}

		int ret = poll(fds, 2, timeout);
		if (ret < 0)
			exit_errno("poll");

		update_time();

		if (fds[0].revents & POLLIN)
			handle_solicit();
		if (fds[1].revents & POLLIN)
			handle_rtnl();

		if (timespec_after(&G.time, &G.next_advert))
			send_advert();
	}
}
