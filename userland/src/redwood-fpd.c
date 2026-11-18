/*
 * redwood-fpd: the S20's fingerprint sensor behind iOS's native Touch ID.
 *
 * RedwoodMesa (iokit/Kernel/RedwoodMesa.cpp, a port of redfin's SweetMesa) plays the Secure Enclave's Touch ID part
 * for Apple's own biometrickitd, so Settings' enrolment screen, the lock screen and app prompts work unmodified. It
 * forwards the sensor work to this daemon over its bridge user client (ABI: sweetmesa_bridge.h / fpcd_bridge.h from
 * DenialOfGravity/ios-redfin, MIT). Here the work is done by our TEEGRIS fingerprint stack (bsd/dev/redwood_teegris.c),
 * driven with its "fp ..." commands through kern.redwood.tee, and its log lines become the bridge's FPCD events:
 *
 *   touch at X,Y                       FINGER_DOWN           finger lifted        FINGER_UP
 *   lift and press again               IMAGE (insufficient)  SAMPLE n ACCEPTED: progress v   ENROL_PROGRESS
 *   fp template: saved N bytes (0)     ENROL_DONE fid 1      SENSOR MATCH candidate (bio b score s)   MATCH
 *   NO MATCH                           NOMATCH
 *
 * A match is only ever the TEE matcher's own decision (its token is not verified: kern.sweet_mesa_match=2, the mode
 * redfin's owner chose). Nothing here turns a touch into a match. The keybag unlock rules (passcode since boot, 48 h,
 * 5 failures) are the kernel's (MiniAppleKeyStore redwood_bio_*).
 *
 * Runs only with the boot argument touchid=1 (com.redwood.fpd job); it then sets the kern.sweet_mesa* switches.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <spawn.h>
#include <IOKit/IOKitLib.h>
#include <notify.h>
#include "fpcd_bridge.h"

#define TEMPLATE        "/var/root/fp-template.bin"
#define FID             1u                      /* one finger: the TEE holds one template */
#define ENROL_TOUCHES   17u                     /* good touches to 100 % on this phone (FINGERPRINT-HANDOVER.md) */
#define STEP_MS         20000
#define RUN_MS          120000  /* longer than a round (fp run 3 1 45 4) */

static io_connect_t conn;
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static volatile uint32_t gen;                   /* operation generation: a new op or a cancel bumps it */
static volatile int op;                         /* 0 none, 1 enrol, 2 match */
static uint32_t evseq;
static uint32_t enrol_pct;                      /* the matcher's own enrolment percentage */
static volatile int enrolling, calibrated;                  /* EnrollMode .. template saved / Cancel / RemoveTemplate */

static void
lg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "redwood-fpd: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static long
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- the TEE backend (kern.redwood.tee) ---- */
static int
tee_status(char *b, size_t cap)
{
	size_t n = cap - 1;
	if (sysctlbyname("kern.redwood.tee", b, &n, NULL, 0)) return -1;
	b[n < cap ? n : cap - 1] = 0;
	return 0;
}

static int
tee_cmd(const char *c)
{
	return sysctlbyname("kern.redwood.tee", NULL, NULL, (void *)c, strlen(c));
}

static int
tee_idle(const char *s)
{
	return strstr(s, "TEEGRIS: up") && strstr(s, ", active 0");
}

/* the same completion rules as tools/fingerprint/ui/broker.m */
static int
tee_ok(const char *cmd, const char *s)
{
	const char *last = strstr(s, "\nlast: ");
	last = last ? last + 7 : "";
	if (!strcmp(cmd, "fp power")) return !strncmp(last, "fp power: configured, error 0", 29);
	if (!strcmp(cmd, "fp cbge")) return !strncmp(last, "fp cbge: ", 9);
	if (!strncmp(cmd, "fp 12:", 6)) return !strncmp(last, "fp 12 (", 7);
	if (!strcmp(cmd, "fp prepare")) return !strncmp(last, "fp prepare: ", 12) && strstr(last, ": error 0") != NULL;
	if (!strcmp(cmd, "fp cbgeefs")) return strstr(last, "2 factory slots read, load error 0") != NULL;
	if (!strcmp(cmd, "fp idinit")) return !strncmp(last, "fp idinit: error 0 op ", 22);
	if (!strcmp(cmd, "fp einit 1")) return !strncmp(last, "fp einit 1: result 0 error 0 op ", 32);
	if (!strncmp(cmd, "fp run ", 7)) return !strncmp(last, "fp run ", 7);
	return strstr(last, ": result 0 origin ") && strstr(last, " error 0 timeout ");
}

/* ---- the kernel log (kern.redwood_dmesg: u64 total bytes ever written, then the ring's tail) ---- */
static uint64_t log_total;
static char partial[8192];
static size_t partial_len;

static void
log_baseline(void)
{
	size_t n = 0;
	static char buf[1 << 20];
	if (sysctlbyname("kern.redwood_dmesg", NULL, &n, NULL, 0) || n < 8 || n > sizeof(buf)) return;
	if (sysctlbyname("kern.redwood_dmesg", buf, &n, NULL, 0) || n < 8) return;
	memcpy(&log_total, buf, 8);
	partial_len = 0;
}

typedef void (*line_fn)(const char *line);

static void
log_read(line_fn fn)
{
	static char buf[1 << 20];
	size_t n = 0;
	if (sysctlbyname("kern.redwood_dmesg", NULL, &n, NULL, 0) || n < 8 || n > sizeof(buf)) return;
	if (sysctlbyname("kern.redwood_dmesg", buf, &n, NULL, 0) || n < 8) return;
	uint64_t total;
	memcpy(&total, buf, 8);
	size_t have = n - 8;
	if (total < log_total || total - log_total > have) {    /* lost lines: start over from here */
		log_total = total;
		partial_len = 0;
		return;
	}
	size_t delta = (size_t)(total - log_total);
	log_total = total;
	const char *p = buf + n - delta;
	for (size_t i = 0; i < delta; i++) {
		if (p[i] == '\n' || partial_len == sizeof(partial) - 1) {
			partial[partial_len] = 0;
			const char *t = strstr(partial, "redwood-tee: ");
			if (t) fn(t + 13);
			partial_len = 0;
		} else {
			partial[partial_len++] = p[i];
		}
	}
}

/* ---- the bridge ---- */
static int
bridge_open(void)
{
	io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(SMB_SERVICE));
	if (!s) return -1;
	kern_return_t k = IOServiceOpen(s, mach_task_self(), SMB_UC_TYPE, &conn);
	IOObjectRelease(s);
	return k == KERN_SUCCESS ? 0 : -1;
}

static void
ev_send(unsigned code, int32_t status, uint32_t fid, int32_t score, uint32_t remaining, uint32_t samples, uint32_t flags)
{
	static struct smb_event e;
	struct fpcd_event v = {
		.magic = FPCD_EV_MAGIC, .version = FPCD_ABI_VERSION, .code = (uint16_t)code, .evseq = ++evseq,
		.op_gen = gen, .status = status, .fid = fid, .score = score, .remaining = remaining, .samples = samples,
		.flags = flags, .op = (uint32_t)op, .hat_status = FPCD_HAT_NO_VERIFIER,
	};
	if (code == FPCD_EV_ENROL_PROGRESS || code == FPCD_EV_ENROL_DONE) {
		v.percent = code == FPCD_EV_ENROL_DONE ? 100 : enrol_pct;
	}
	if (code == FPCD_EV_TEMPLATES || code == FPCD_EV_ENROL_DONE) {
		v.n = access(TEMPLATE, F_OK) == 0;
		v.ids[0] = FID;
	}
	memset(&e, 0, sizeof(e));
	e.subtype = fpcd_event_subtype(code);
	e.len = sizeof(v);
	memcpy(e.data, &v, sizeof(v));
	kern_return_t k = IOConnectCallStructMethod(conn, SMB_SEL_EVENT, &e, sizeof(e), NULL, NULL);
	lg("event %u status %d fid %u score %d remaining %u samples %u -> %#x", code, status, fid, score, remaining,
	    samples, k);
}


/* the sensor's ring on screen (libredwood-touchid-ui in SpringBoard) while a capture waits for a finger */
static void
ring(int on)
{
	static int token = -1;
	if (token == -1 && notify_register_check("com.redwood.touchid.sensor", &token) != NOTIFY_STATUS_OK) {
		token = -2;
	}
	if (token >= 0) {
		notify_set_state(token, (uint64_t)on);
		notify_post("com.redwood.touchid.sensor");
	}
}

/* ---- operations ---- */
static uint32_t samples_seen;

static void
on_line(const char *l)
{
	unsigned n, v, b, sc, x, y;
	if (strstr(l, ": touch at ") && strstr(l, "ON THE SENSOR") &&
	    sscanf(strstr(l, ": touch at "), ": touch at %u,%u", &x, &y) == 2) {   /* a tap elsewhere is not a finger */
		ev_send(FPCD_EV_FINGER_DOWN, 0, 0, 0, 0, 0, 0);
	} else if (strstr(l, ": finger lifted")) {
		ev_send(FPCD_EV_FINGER_UP, 0, 0, 0, 0, 0, 0);
	} else if (strstr(l, "lift and press again")) {
		ev_send(FPCD_EV_IMAGE, 3, 0, 0, 0, 0, 0);
	} else if (op == 1 && sscanf(l, "fp run 3: SAMPLE %u ACCEPTED: progress %u", &n, &v) == 2 && v <= 100) {
		samples_seen++;                 /* the driver counts per run; the enrolment runs on across runs */
		n = samples_seen;
		/* the TEE says how far, not how many are left: estimate from the touches so far */
		/* Apple's screen follows the matcher's own percentage: phase 1 ends when Samsung commits the template (100),
		 * not after a touch count (it ended at 75 % before, and phase 2's MatchMode cut the enrolment short) */
		enrol_pct = v;
		uint32_t rem = v >= 100 ? 0 : (100 - v + 5) / 6;
		ev_send(FPCD_EV_ENROL_PROGRESS, v >= 100 ? 0 : 1, 0, 0, rem ? rem : (v < 100), n, 0);
	} else if (op == 1 && !strncmp(l, "fp template: saved ", 19)) {
		unsigned bytes;
		int err;
		if (sscanf(l, "fp template: saved %u bytes to " TEMPLATE " (error %d)", &bytes, &err) == 2 && !err &&
		    bytes > 0) {
			enrolling = 0;
			ev_send(FPCD_EV_ENROL_DONE, 0, FID, 0, 0, samples_seen, 0);
			ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0);
		} else {
			ev_send(FPCD_EV_ENROL_FAILED, FPCD_E_IO, 0, 0, 0, samples_seen, 0);
		}
		op = 0;
	} else if (op == 2 && sscanf(l, "fp run 6: SENSOR MATCH candidate (bio %u score %u", &b, &sc) == 2) {
		ev_send(FPCD_EV_MATCH, 0, FID, (int32_t)sc, 0, 1, 0);
		op = 0;
	} else if (op == 2 && !strcmp(l, "fp run 6: NO MATCH")) {
		ev_send(FPCD_EV_NOMATCH, 0x20, 0, 0, 0, 1, 0);
	} else if (strstr(l, "invalid response") || strstr(l, "re-arm failed") || strstr(l, "capture error 0x44") ||
	    strstr(l, ": stop")) {
		if (op) ev_send(op == 1 ? FPCD_EV_ENROL_FAILED : FPCD_EV_ERROR, FPCD_E_IO, 0, 0, 0, samples_seen, 0);
		op = 0;
	}
}

/* one command: wait for the backend to be idle, send, wait until it finishes; lines are read meanwhile */
static int
step(const char *cmd, uint32_t g)
{
	char s[2048];
	long until = now_ms() + STEP_MS;
	while (tee_status(s, sizeof(s)) == 0 && !tee_idle(s)) {
		if (now_ms() > until || g != gen) return -1;
		usleep(100000);
	}
	if (tee_cmd(cmd)) {
		lg("%s refused (%d)", cmd, errno);
		return -1;
	}
	until = now_ms() + (strncmp(cmd, "fp run ", 7) ? STEP_MS : RUN_MS);
	usleep(100000);
	for (;;) {
		log_read(on_line);
		if (tee_status(s, sizeof(s))) return -1;
		if (tee_idle(s)) return tee_ok(cmd, s) ? 0 : -1;
		if (g != gen) {                 /* cancelled: stop the backend's run and let it drain */
			tee_cmd("fp stop");
			g = gen;
		}
		if (now_ms() > until) {
			tee_cmd("fp stop");
			ev_send(FPCD_EV_OP_TIMEOUT, 0, 0, 0, 0, 0, 0);
			return -1;
		}
		usleep(100000);
	}
}


/* the opcode the TEE asks for next, from kern.redwood.tee's "last:" line ("... opcode 0x44 ..."), or -1 */
static int
tee_next_op(void)
{
	char s[2048];
	if (tee_status(s, sizeof(s))) return -1;
	const char *l = strstr(s, "\nlast: ");
	const char *o = l ? strstr(l, " opcode ") : NULL;
	return o ? (int)strtol(o + 8, NULL, 0) : -1;
}

/*
 * Background calibration as Samsung's FPBAuthService::new_cbge_mode (libbauthserver 0x15abc0) does when the sensor
 * raises its IPC line (CBGE_REQUIRED): CONTROL 69, then whatever opcode the TEE returns (68, 70 with its 10 ms
 * timeout, 67), then CONTROL 71 until it answers 0. Without it every capture ran with "IPC line low" and the matcher
 * rejected most images (PARTIAL / DUPLICATED / LIGHT_TOUCH); on the phone (2026-11-19) this chain set the line high.
 */
static void __attribute__((unused))
cbge_chain(uint32_t g)
{
	char c[32];
	int op = 69;
	for (int n = 0; n < 40 && op > 0 && op != 71 && g == gen; n++) {
		snprintf(c, sizeof(c), "fp 12:%d", op);
		if (step(c, g) && op != 67) break;              /* 67 answers 0x33 without slot data: go on */
		int next = tee_next_op();
		if (op == 70) usleep(10000);
		if (next == op && op == 67) break;
		op = next;
	}
	for (int n = 0; n < 8 && g == gen; n++) {
		step("fp 12:71", g);
		if (tee_next_op() == 0) break;
	}
	lg("background calibration done");
}

static void *
run(void *arg)
{
	int kind = (int)(intptr_t)arg;
	uint32_t g = gen;
	const char *setup[] = { "fp power", "fp prepare", "fp cbgeefs", "fp 10:1", "fp prepare" };
	pthread_mutex_lock(&mx);
	ring(1);
	log_baseline();
	if (kind == 1 && enrol_pct == 0) samples_seen = 0;
	int bad = 0;
	/* a match keeps the background calibration the enrolment ran with (stock calibrates when the sensor asks and
	 * keeps it); reloading the factory slots and PREPARE op 2 before identify gave NO MATCH on the same finger */
	if (kind == 2 && calibrated) bad = step("fp power", g);
	else for (unsigned i = 0; i < sizeof(setup) / sizeof(setup[0]) && !bad; i++) bad = step(setup[i], g);
	/* the kernel's fp cbge runs the same chain with CONTROL 67 per slot (slot byte + 0x96000 output), as Samsung's
	 * update_cbge (0x14c794); the generic chain above left 67 without data and the next capture failed with 0x44 */
	/* only before an enrolment: a calibration taken while a finger rests on the sensor (Apple's phase 2 asks for the
	 * next touch at once) becomes the "background", and identify's contamination check then fails every image (29) */
	if (!bad && kind == 1) { step("fp cbge", g); calibrated = 1; lg("background calibration: fp cbge done"); }
	if (kind == 2) {
		if (!bad) bad = step("fp idinit", g);
		if (!bad) bad = step("fp run 6 1 15 0", g);
	} else {
		/* a run ends after its step budget; enrolment goes on in the next one until the template is saved (op 0),
		 * as tools/fingerprint/ui/broker.m did (17 touches to 100 % on this phone) */
		for (int rounds = 0; !bad && op == 1 && g == gen && rounds < 200; rounds++) {
			bad = step("fp einit 1", g);
			if (!bad) bad = step("fp run 3 1 45 4", g);
		}
	}
	if (bad && op == kind && g == gen) {
		ev_send(kind == 1 ? FPCD_EV_ENROL_FAILED : FPCD_EV_ERROR, FPCD_E_IO, 0, 0, 0, samples_seen, 0);
	}
	if (g == gen) op = 0;
	ring(0);
	pthread_mutex_unlock(&mx);
	return NULL;
}

static int
start(int kind)
{
	gen++;
	op = kind;
	pthread_t t;
	if (pthread_create(&t, NULL, run, (void *)(intptr_t)kind)) return FPCD_E_BUSY;
	pthread_detach(t);
	return FPCD_OK;
}

/* ---- requests from the kernel ---- */
static int
rep_u32(struct smb_reply *p, uint32_t v)
{
	memcpy(p->data, &v, 4);
	p->len = 4;
	return FPCD_OK;
}

static int
handle(const struct smb_req *q, struct smb_reply *p)
{
	int has = access(TEMPLATE, F_OK) == 0;
	switch (q->cmd) {
	case 1: return rep_u32(p, 2);                           /* GetCmdProtocolVersion */
	case 83: return rep_u32(p, 1);                          /* IsSensorReady */
	case 15: return rep_u32(p, 1);                          /* RequestMaxIdentityCount: one finger */
	case 65: return rep_u32(p, has ? 0 : 1);                /* GetFreeIdentityCount */
	case 66: {                                              /* GetIdentitiesList */
		struct fpcd_ids ids = { .count = (uint32_t)has, .ids = { FID } };
		memcpy(p->data, &ids, sizeof(ids));
		p->len = sizeof(ids);
		return FPCD_OK;
	}
	case 3: enrolling = 1; return start(1);                 /* EnrollMode */
	case 4:                                                 /* MatchMode */
		if (enrolling) {                                /* phase 2 before the template was committed: go on */
			if (op != 1) return start(1);
			return FPCD_OK;
		}
		return has ? start(2) : FPCD_E_NOENT;
	case 12:                                                /* Cancel (Apple also sends it between its two phases) */
		gen++;
		op = 0;
		return FPCD_OK;
	case 38: case 69: return FPCD_OK;                       /* finger detect: the runs detect by themselves */
	case 2:                                                 /* ResetSensor: end any run */
		if (op) {
			gen++;
			op = 0;
		}
		return FPCD_OK;
	case 14: return FPCD_OK;                                /* EnrollContinue: the enrol run goes on by itself */
	case 49: return FPCD_OK;                                /* NoCatacomb: the template file stays (nothing to load) */
	case 56: {                                              /* GetCatacombId: a stable id for the template db */
		uint64_t id = has ? 0x4a45535649544944ull : 0;  /* "JESVITID" */
		memcpy(p->data, &id, 8);
		p->len = 8;
		return FPCD_OK;
	}
	case 78: p->len = 0; return FPCD_OK;                    /* GetLastMatchEvent: none kept here */
	case 72: {                                              /* RemoveUserData: every finger (kept aside, not deleted) */
		char bak[96];
		snprintf(bak, sizeof(bak), TEMPLATE ".removed.%ld", (long)time(NULL));
		if (has && rename(TEMPLATE, bak)) return FPCD_E_IO;
		ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0);
		return FPCD_OK;
	}
	case 13: {                                              /* RemoveTemplate */
		char bak[96];
		snprintf(bak, sizeof(bak), TEMPLATE ".removed.%ld", (long)time(NULL));
		if (has && rename(TEMPLATE, bak)) return FPCD_E_IO;    /* kept aside, never deleted */
		ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0);
		return FPCD_OK;
	}
	case FPCD_CMD_PING:
		p->len = (uint32_t)snprintf((char *)p->data, SMB_DATA_MAX, "redwood-fpd 1") + 1;
		return FPCD_OK;
	default:
		return FPCD_E_UNSUPPORTED;
	}
}


/* TEEGRIS up, as tools/fingerprint/tee-up.sh: the storage daemon, the secure OS, then this phone's driver TAs
 * (/var/root/tee/driver/: Samsung's, pulled from the phone itself, never in the image) */
extern char **environ;
static void
tee_up(void)
{
	char s[2048];
	if (tee_status(s, sizeof(s)) == 0 && strstr(s, "TEEGRIS: up")) return;
	pid_t pid;
	char *argv[] = { "/redwood/bin/teetsd", NULL };
	if (access(argv[0], X_OK) == 0 && posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) == 0) lg("teetsd started");
	tee_cmd("start");
	sleep(6);
	DIR *d = opendir("/var/root/tee/driver");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char c[320];
		snprintf(c, sizeof(c), "openkeep /var/root/tee/driver/%s", e->d_name);
		tee_cmd(c);
		sleep(3);
	}
	if (d) closedir(d);
	lg("TEEGRIS %s", tee_status(s, sizeof(s)) == 0 && strstr(s, "TEEGRIS: up") ? "up" : "did not come up");
}

static void
set(const char *name, int v)
{
	if (sysctlbyname(name, NULL, NULL, &v, sizeof(v))) lg("%s=%d: errno %d", name, v, errno);
}

int
main(void)
{
	char args[1024] = "";
	size_t n = sizeof(args) - 1;
	sysctlbyname("kern.bootargs", args, &n, NULL, 0);
	if (!strstr(args, "touchid=1")) return 0;               /* off unless asked for at boot */
	if (getuid() != 0) return 1;
	umask(0077);
	tee_up();
	set("kern.sweet_mesa_match", 2);                        /* the TEE matcher's decision, token unverified */
	set("kern.sweet_mesa_offline", 1);
	set("kern.sweet_mesa", 3);                              /* publish the persona + translate our events */
	for (int i = 0; bridge_open(); i++) {
		if (i == 50) {
			lg("no %s", SMB_SERVICE);
			return 1;
		}
		usleep(200000);
	}
	lg("bridge open; template %s", access(TEMPLATE, F_OK) ? "none" : "present");
	static struct smb_req q;
	static struct smb_reply p;
	for (;;) {
		uint64_t timeout = 1000;
		size_t qn = sizeof(q);
		kern_return_t k = IOConnectCallMethod(conn, SMB_SEL_WAIT, &timeout, 1, NULL, 0, NULL, NULL, &q, &qn);
		if (k == (kern_return_t)0xe00002d6) continue;          /* kIOReturnTimeout */
		if (k != KERN_SUCCESS) {
			lg("wait: %#x", k);
			sleep(1);
			continue;
		}
		memset(&p, 0, offsetof(struct smb_reply, data));
		p.seq = q.seq;
		p.status = handle(&q, &p);
		IOConnectCallStructMethod(conn, SMB_SEL_REPLY, &p, offsetof(struct smb_reply, data) + p.len, NULL, NULL);
	}
}
