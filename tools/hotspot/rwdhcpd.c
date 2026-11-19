/*
 * rwdhcpd: the hotspot's DHCP server (A, 2026-11-19). iOS's bootpd exits at once when not started by Personal Hotspot,
 * so the S20's access point hands out addresses with this: 172.20.10.2-14 on IF (the phone is 172.20.10.1, as on an
 * iPhone hotspot), router 172.20.10.1, DNS as given. Leases live in memory (by MAC), 1 hour.
 *
 *   rwdhcpd IF [DNS]          e.g. rwdhcpd en0 172.20.10.1
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define BIOCSETIF _IOW('B', 108, struct ifreq)      /* net/bpf.h, absent from the iOS SDK */

#define NET     0xac140a00u     /* 172.20.10.0/28 */
#define SELF    (NET | 1)
#define FIRST   2
#define LAST    14
#define LEASE   3600

static uint8_t lease_mac[LAST + 1][6];

static int
pick(const uint8_t *mac)
{
	for (int i = FIRST; i <= LAST; i++) {
		if (!memcmp(lease_mac[i], mac, 6)) {
			return i;
		}
	}
	for (int i = FIRST; i <= LAST; i++) {
		static const uint8_t zero[6];
		if (!memcmp(lease_mac[i], zero, 6)) {
			memcpy(lease_mac[i], mac, 6);
			return i;
		}
	}
	return 0;
}

static uint8_t *
opt(uint8_t *o, uint8_t code, uint8_t len, const void *data)
{
	*o++ = code;
	*o++ = len;
	memcpy(o, data, len);
	return o + len;
}

/* replies go out as raw Ethernet frames through BPF, as iOS's bootpd does: a UDP socket gets EHOSTUNREACH for
 * 255.255.255.255 here, and clients ignore an offer to the subnet broadcast */
static int bpf = -1;
static uint8_t self_mac[6];

static int
bpf_open(const char *ifname)
{
	char dev[16];
	for (int i = 0; i < 64 && bpf < 0; i++) {
		snprintf(dev, sizeof(dev), "/dev/bpf%d", i);
		bpf = open(dev, O_WRONLY);
	}
	if (bpf < 0) {
		return -1;
	}
	struct ifreq ifr = { 0 };
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(bpf, BIOCSETIF, &ifr) < 0) {
		return -1;
	}
	struct ifaddrs *ia, *i;
	getifaddrs(&ia);
	for (i = ia; i; i = i->ifa_next) {
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_LINK && !strcmp(i->ifa_name, ifname)) {
			memcpy(self_mac, LLADDR((struct sockaddr_dl *)i->ifa_addr), 6);
		}
	}
	freeifaddrs(ia);
	return 0;
}

static uint16_t
csum(const uint8_t *d, size_t n)
{
	uint32_t s = 0;
	for (size_t i = 0; i + 1 < n; i += 2) {
		s += (uint32_t)(d[i] << 8 | d[i + 1]);
	}
	if (n & 1) {
		s += (uint32_t)d[n - 1] << 8;
	}
	while (s >> 16) {
		s = (s & 0xffff) + (s >> 16);
	}
	return (uint16_t)~s;
}

/* Ethernet to ff:ff..., IPv4 SELF -> 255.255.255.255, UDP 67 -> 68 (UDP checksum 0: allowed for IPv4) */
static ssize_t
send_raw(const uint8_t *dhcp, size_t n)
{
	uint8_t f[1600];
	if (n + 42 > sizeof(f)) {
		return -1;
	}
	memset(f, 0xff, 6);
	memcpy(f + 6, self_mac, 6);
	f[12] = 0x08;
	f[13] = 0x00;
	uint8_t *ip = f + 14, *udp = ip + 20;
	uint16_t tl = (uint16_t)(20 + 8 + n), ul = (uint16_t)(8 + n);
	memset(ip, 0, 20);
	ip[0] = 0x45;
	ip[2] = tl >> 8;
	ip[3] = tl & 0xff;
	ip[8] = 64;
	ip[9] = 17;
	uint32_t src = htonl(SELF), dst = 0xffffffffu;
	memcpy(ip + 12, &src, 4);
	memcpy(ip + 16, &dst, 4);
	uint16_t c = csum(ip, 20);
	ip[10] = c >> 8;
	ip[11] = c & 0xff;
	udp[0] = 0; udp[1] = 67; udp[2] = 0; udp[3] = 68;
	udp[4] = ul >> 8; udp[5] = ul & 0xff; udp[6] = 0; udp[7] = 0;
	memcpy(udp + 8, dhcp, n);
	return write(bpf, f, 42 + n);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: rwdhcpd IF [DNS]\n");
		return 2;
	}
	uint32_t dns = htonl(SELF);
	if (argc > 2) {
		inet_pton(AF_INET, argv[2], &dns);
	}
	int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
	unsigned idx = if_nametoindex(argv[1]);
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	/* not IP_BOUND_IF: a socket pinned to IF looks 255.255.255.255 up in IF's scoped table and gets EHOSTUNREACH;
	 * the unscoped host route (route add -host 255.255.255.255 -interface IF) sends the offers out IF */
	struct sockaddr_in sa = { .sin_len = sizeof(sa), .sin_family = AF_INET, .sin_port = htons(67) };
	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("rwdhcpd: bind :67");
		return 1;
	}
	if (bpf_open(argv[1]) < 0) {
		perror("rwdhcpd: bpf (replies by UDP instead)");
	}
	printf("rwdhcpd: on %s (index %u), 172.20.10.%d-%d\n", argv[1], idx, FIRST, LAST);
	fflush(stdout);
	for (;;) {
		uint8_t p[1500];
		ssize_t n = recv(s, p, sizeof(p), 0);
		if (n < 240 || p[0] != 1 || p[236] != 99 || p[237] != 130 || p[238] != 83 || p[239] != 99) {
			continue;
		}
		uint8_t type = 0, *mac = p + 28;
		uint32_t want = 0;
		for (ssize_t i = 240; i + 1 < n && p[i] != 255;) {
			if (p[i] == 0) {
				i++;
				continue;
			}
			if (p[i] == 53 && p[i + 1] >= 1) {
				type = p[i + 2];
			}
			if (p[i] == 50 && p[i + 1] == 4) {
				memcpy(&want, p + i + 2, 4);
			}
			i += 2 + p[i + 1];
		}
		if (type != 1 && type != 3) {           /* DISCOVER, REQUEST */
			continue;
		}
		int host = pick(mac);
		if (!host) {
			printf("rwdhcpd: pool full\n");
			fflush(stdout);
			continue;
		}
		uint32_t yi = htonl(NET | (uint32_t)host), self = htonl(SELF), mask = htonl(0xfffffff0), lt = htonl(LEASE);
		bool nak = type == 3 && want && want != yi;
		p[0] = 2;                               /* BOOTREPLY; xid, flags, chaddr kept */
		memset(p + 12, 0, 4);                   /* ciaddr */
		memcpy(p + 16, nak ? (uint8_t[4]){ 0 } : (uint8_t *)&yi, 4);
		memcpy(p + 20, &self, 4);
		uint8_t *o = p + 240, t = nak ? 6 : type == 1 ? 2 : 5;  /* NAK, OFFER, ACK */
		o = opt(o, 53, 1, &t);
		o = opt(o, 54, 4, &self);
		if (!nak) {
			o = opt(o, 51, 4, &lt);
			o = opt(o, 1, 4, &mask);
			o = opt(o, 3, 4, &self);
			o = opt(o, 6, 4, &dns);
		}
		*o++ = 255;
		struct sockaddr_in to = { .sin_len = sizeof(to), .sin_family = AF_INET, .sin_port = htons(68),
			.sin_addr.s_addr = htonl(INADDR_BROADCAST) };   /* needs: route add -host 255.255.255.255 -interface IF (clients ignore a subnet-broadcast offer) */
		ssize_t w = bpf >= 0 ? send_raw(p, (size_t)(o - p)) : sendto(s, p, (size_t)(o - p), 0, (struct sockaddr *)&to, sizeof(to));
		printf("rwdhcpd: %s %02x:%02x:%02x:%02x:%02x:%02x -> %s 172.20.10.%d (%s)\n", type == 1 ? "DISCOVER" : "REQUEST",
		    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], nak ? "NAK" : type == 1 ? "OFFER" : "ACK", host,
		    w < 0 ? strerror(errno) : "sent");
		fflush(stdout);
	}
}
