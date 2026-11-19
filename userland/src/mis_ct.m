/*
 * libredwood-mis: the carrier side of iOS's own Personal Hotspot, inside misd (A, 2026-11-19).
 *
 * misd (MobileInternetSharing) runs the hotspot itself -- DHCP, NAT, NAT64 for an IPv6-only carrier, the Wi-Fi
 * access point through wifid -- and Settings' Personal Hotspot pane and Control Center's switch drive it. It asks
 * CommCenter (CoreTelephony) whether tethering is allowed, whether cellular data is on and which interface carries
 * it; the S20 has no CommCenter (its modem is redwood-cellulard's), so those answers come from here:
 *   -[misCTClientSharedInstance getTetheringStatus:]        allowed by the carrier, the user, available, 5 hosts,
 *                                                            the connection active on the cellular data utun
 *   -[misCTClientSharedInstance isDataPlanEnabled:]         yes
 *   -[misCTClientSharedInstance getTetheringInterfaceName:] the cellular data utun
 *   -[misCTClientSharedInstance activateTethering:]         done (the data connection is always up here)
 * The struct (misd's own type encoding): { bool carrierEnabled, userAuthenticated, connectionAvailable;
 * unsigned maxHosts; { int state (CT state + 1), int, char ifname[16] } }.
 * kern.redwood ... none: the cellular utun is the one redwood-cellulard published (its log), else the first utun
 * with a global IPv6 address. Loaded into misd by kern.exec_inject; RW_MIS_STATE overrides the state (bring-up).
 */
#import <Foundation/Foundation.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

struct mis_conn { int state; int pdp; char ifname[16]; };
struct mis_status { bool carrierEnabled, userAuthenticated, connectionAvailable; unsigned maxHosts; struct mis_conn conn; };

static os_log_t lg;

/* the cellular data utun: what redwood-cellulard last published, else a utun with a global, non-link-local IPv6 address */
static void
cell_ifname(char out[16])
{
	char line[512], last[32] = "";
	FILE *f = fopen("/var/tmp/redwood-cellular.log", "r");

	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			char *p = strstr(line, "configd service redwood-cell published");
			char *on = p != NULL ? strstr(p, " on utun") : NULL;
			if (on != NULL) {
				sscanf(on + 4, "%15[a-z0-9]", last);
			}
		}
		fclose(f);
	}
	if (last[0] == 0) {
		struct ifaddrs *ia, *i;
		if (getifaddrs(&ia) == 0) {
			for (i = ia; i != NULL && last[0] == 0; i = i->ifa_next) {
				if (i->ifa_addr != NULL && i->ifa_addr->sa_family == AF_INET6 && strncmp(i->ifa_name, "utun", 4) == 0) {
					const struct in6_addr *a = &((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr;
					if (!IN6_IS_ADDR_LINKLOCAL(a) && (a->s6_addr[0] & 0xe0) == 0x20) {
						strlcpy(last, i->ifa_name, sizeof(last));
					}
				}
			}
			freeifaddrs(ia);
		}
	}
	strlcpy(out, last[0] ? last : "utun1", 16);
}

static int
active_state(void)
{
	const char *e = getenv("RW_MIS_STATE");
	return e != NULL ? atoi(e) : 3;
}

static int
mis_getTetheringStatus(id self, SEL _cmd, struct mis_status *s)
{
	if (s == NULL) {
		return -1;
	}
	memset(s, 0, sizeof(*s));
	s->carrierEnabled = s->userAuthenticated = s->connectionAvailable = true;
	s->maxHosts = 5;
	s->conn.state = active_state();
	cell_ifname(s->conn.ifname);
	os_log(lg, "redwood-mis: getTetheringStatus -> allowed, available, state %d on %{public}s", s->conn.state, s->conn.ifname);
	return 0;
}

static int
mis_isDataPlanEnabled(id self, SEL _cmd, bool *on)
{
	if (on != NULL) {
		*on = true;
	}
	return 0;
}

static int
mis_getTetheringInterfaceName(id self, SEL _cmd, char *name)
{
	char n[16];

	cell_ifname(n);
	if (name != NULL) {
		strlcpy(name, n, 16);
	}
	os_log(lg, "redwood-mis: getTetheringInterfaceName -> %{public}s", n);
	return 0;
}

static int
mis_activateTethering(id self, SEL _cmd, bool on)
{
	os_log(lg, "redwood-mis: activateTethering(%d) -> done", on);
	return 0;
}

/*
 * The network behind the access point (B, 2026-11-19). On an iPhone wifid makes a separate interface (ap1) and misd
 * builds the bridge, 172.20.10.1, bootpd and NAT on it; here the access point is en0 itself, so misd never sees an
 * internal interface. Watch the driver (kern.redwood.wifi_ap reads "ON THE AIR..." while it runs) and bring the
 * network up/down with tools/hotspot/hotspot-up.sh net-start/net-stop (addresses, rwdhcpd, NAT over CLAT46).
 */
extern char **environ;

static void
hotspot_net(const char *what)
{
	char *argv[] = { "zsh", "/var/root/hotspot-up.sh", (char *)what, NULL };
	pid_t pid;
	int st;

	posix_spawn_file_actions_t fa;
	int e;

	posix_spawn_file_actions_init(&fa);	/* its output: /var/tmp/redwood-hotspot.log */
	posix_spawn_file_actions_addopen(&fa, 1, "/var/tmp/redwood-hotspot.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	posix_spawn_file_actions_adddup2(&fa, 1, 2);
	e = posix_spawn(&pid, "/var/jb/bin/zsh", &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);

	if (e == 0) {
		waitpid(pid, &st, 0);
		os_log(lg, "redwood-mis: hotspot %{public}s: exit %d", what, WEXITSTATUS(st));
	} else {
		os_log(lg, "redwood-mis: hotspot %{public}s: cannot start zsh: %d", what, e);
	}
}

static void *
ap_watch(void *arg)
{
	bool up = false;

	(void)arg;
	os_log(lg, "redwood-mis: watching the access point");
	for (;;) {
		char s[160] = "";
		size_t n = sizeof(s) - 1;
		bool on = sysctlbyname("kern.redwood.wifi_ap", s, &n, NULL, 0) == 0 && strncmp(s, "ON THE AIR", 10) == 0;

		if (on != up) {
			os_log(lg, "redwood-mis: access point %{public}s (%{public}.20s)", on ? "on" : "off", s);
			hotspot_net(on ? "net-start" : "net-stop");
			up = on;
		}
		sleep(2);
	}
	return NULL;
}

static void
swap(Class c, const char *sel, IMP imp)
{
	Method m = class_getInstanceMethod(c, sel_registerName(sel));
	if (m != NULL) {
		method_setImplementation(m, imp);
	} else {
		os_log(lg, "redwood-mis: no %{public}s", sel);
	}
}

__attribute__((constructor)) static void
mis_init(void)
{
	Class c = objc_getClass("misCTClientSharedInstance");

	lg = os_log_create("com.redwood.mis", "ct");
	if (c == Nil) {
		return;                 /* not misd */
	}
	swap(c, "getTetheringStatus:", (IMP)mis_getTetheringStatus);
	swap(c, "isDataPlanEnabled:", (IMP)mis_isDataPlanEnabled);
	swap(c, "getTetheringInterfaceName:", (IMP)mis_getTetheringInterfaceName);
	swap(c, "activateTethering:", (IMP)mis_activateTethering);
	{
		pthread_t th;
		pthread_create(&th, NULL, ap_watch, NULL);
		pthread_detach(th);
	}
	os_log(lg, "redwood-mis: misd's CoreTelephony answers come from the S20's modem service");
}
