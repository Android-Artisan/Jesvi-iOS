/*
 * The debug port (A, 2026-11-06): one sysctl, kern.redwood.poke, that takes a command and answers it, so a register
 * can be read or tried on the running phone instead of a rebuild and a reboot. From the Mac: tools/poke 'r 0x18c51000 4'
 * (ssh s20 rwsys kern.redwood.poke '...'). Root only (sysctl writes need root).
 *
 * Built in:
 *   r PA [N]          N 32-bit words (at most 64) at physical PA       w PA V        one word, read back
 *   m PA AND OR       read-modify-write: (old & AND) | OR
 *   sr PA / sw PA V   the same through EL3 (Samsung's SMC_CMD_REG -101: SFRs the secure world owns, e.g. ABOX's GIC)
 *   help              every command, the drivers' too
 * Drivers add commands with redwood_dbgport_add() (sys/redwood_dbgport.h), e.g. the sound driver's snd.send.
 * Physical pages are mapped once and kept (64 at most, 16 KB each). Nothing here runs unless a command is written.
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/redwood_dbgport.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <kern/locks.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define SMC_CMD_REG     0xFFFFFFFFFFFFFF9BULL

static char out_buf[8192];
static struct { uint64_t page; volatile uint8_t *va; } maps[64];
static unsigned nmaps;
static struct { char name[24]; const char *help; redwood_dbgport_fn fn; } cmds[32];
static unsigned ncmds;
static LCK_GRP_DECLARE(dbgport_grp, "redwood_dbgport");
static LCK_MTX_DECLARE(dbgport_mtx, &dbgport_grp);

int
redwood_dbgport_add(const char *name, const char *help, redwood_dbgport_fn fn)
{
	if (ncmds >= sizeof(cmds) / sizeof(cmds[0]) || name == NULL || fn == NULL) {
		return -1;
	}
	strlcpy(cmds[ncmds].name, name, sizeof(cmds[ncmds].name));
	cmds[ncmds].help = help;
	cmds[ncmds].fn = fn;
	ncmds++;
	return 0;
}

static volatile uint8_t *
pmap_pa(uint64_t pa)
{
	uint64_t page = pa & ~0x3fffULL;

	for (unsigned i = 0; i < nmaps; i++) {
		if (maps[i].page == page) {
			return maps[i].va + (pa - page);
		}
	}
	if (nmaps >= sizeof(maps) / sizeof(maps[0])) {
		return NULL;
	}
	maps[nmaps].va = (volatile uint8_t *)ml_io_map((vm_offset_t)page, 0x4000);
	if (maps[nmaps].va == NULL) {
		return NULL;
	}
	maps[nmaps].page = page;
	return maps[nmaps++].va + (pa - page);
}

static uint64_t
smc(uint64_t f, uint64_t a1, uint64_t a2, uint64_t *x2out)
{
	register uint64_t x0 __asm__("x0") = f;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile ("dsb sy\n\tsmc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	if (x2out != NULL) {
		*x2out = x2;
	}
	return x0;
}

static unsigned
parse(const char *s, char *cmd, size_t cmdlen, uint64_t *v, unsigned max)
{
	unsigned n = 0, c = 0;

	while (*s == ' ') {
		s++;
	}
	while (*s && *s != ' ' && c + 1 < cmdlen) {
		cmd[c++] = *s++;
	}
	cmd[c] = 0;
	while (*s && n < max) {
		uint64_t x = 0;
		int base = 10;

		while (*s == ' ') {
			s++;
		}
		if (!*s) {
			break;
		}
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			base = 16;
			s += 2;
		}
		for (; *s && *s != ' '; s++) {
			int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 :
			    (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : 99;

			if (d >= base) {
				break;
			}
			x = x * (uint64_t)base + (uint64_t)d;
		}
		v[n++] = x;
	}
	return n;
}

static void
run(const char *line)
{
	char cmd[24];
	uint64_t v[16] = { 0 };
	unsigned n = parse(line, cmd, sizeof(cmd), v, 16);
	size_t cap = sizeof(out_buf), o = 0;

	out_buf[0] = 0;
	if (strcmp(cmd, "r") == 0 && n >= 1) {
		unsigned cnt = n >= 2 ? (unsigned)v[1] : 1;

		cnt = cnt > 64 ? 64 : cnt == 0 ? 1 : cnt;
		for (unsigned i = 0; i < cnt && o < cap; i++) {
			volatile uint8_t *va = pmap_pa(v[0] + 4 * i);

			if (va == NULL) {
				o += (size_t)snprintf(out_buf + o, cap - o, "\nunmappable %#llx", v[0] + 4 * i);
				break;
			}
			if (i % 4 == 0) {
				o += (size_t)snprintf(out_buf + o, cap - o, "%s%#llx:", i ? "\n" : "", v[0] + 4 * i);
			}
			o += (size_t)snprintf(out_buf + o, cap - o, " %08x", *(volatile uint32_t *)va);
		}
		snprintf(out_buf + o, cap - o, "\n");
	} else if ((strcmp(cmd, "w") == 0 && n >= 2) || (strcmp(cmd, "m") == 0 && n >= 3)) {
		volatile uint8_t *va = pmap_pa(v[0]);

		if (va == NULL) {
			snprintf(out_buf, cap, "unmappable %#llx\n", v[0]);
		} else {
			uint32_t old = *(volatile uint32_t *)va, nv = cmd[0] == 'w' ? (uint32_t)v[1] : ((old & (uint32_t)v[1]) | (uint32_t)v[2]);

			*(volatile uint32_t *)va = nv;
			__asm__ volatile ("dsb sy" ::: "memory");
			snprintf(out_buf, cap, "%#llx: %08x -> %08x, reads %08x\n", v[0], old, nv, *(volatile uint32_t *)va);
		}
	} else if (strcmp(cmd, "sr") == 0 && n >= 1) {
		uint64_t x = 0, r = smc(SMC_CMD_REG & 0xffffffffULL, (3U << 30) | ((uint32_t)v[0] >> 2), 0, &x);

		snprintf(out_buf, cap, "%#llx: %08x%s\n", v[0], (uint32_t)x, r == 0 ? "" : " (EL3 refused)");
	} else if (strcmp(cmd, "sw") == 0 && n >= 2) {
		uint64_t r = smc(SMC_CMD_REG, (1U << 30) | ((uint32_t)v[0] >> 2), (uint32_t)v[1], NULL);

		snprintf(out_buf, cap, "%#llx <- %08x through EL3: %lld\n", v[0], (uint32_t)v[1], (long long)r);
	} else {
		for (unsigned i = 0; i < ncmds; i++) {
			if (strcmp(cmd, cmds[i].name) == 0) {
				cmds[i].fn(v, n, out_buf, cap);
				return;
			}
		}
		o = (size_t)snprintf(out_buf, cap, "r PA [N] | w PA V | m PA AND OR | sr PA | sw PA V | help\n");
		for (unsigned i = 0; i < ncmds && o < cap; i++) {
			o += (size_t)snprintf(out_buf + o, cap - o, "%s %s\n", cmds[i].name, cmds[i].help ? cmds[i].help : "");
		}
	}
}

static int
poke_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char in[256];
	int error = 0;

	lck_mtx_lock(&dbgport_mtx);
	if (req->newptr != USER_ADDR_NULL) {
		size_t len = req->newlen < sizeof(in) - 1 ? req->newlen : sizeof(in) - 1;

		if ((error = SYSCTL_IN(req, in, len)) == 0) {
			in[len] = 0;
			run(in);
		}
	}
	if (error == 0) {
		error = SYSCTL_OUT(req, out_buf, strlen(out_buf) + 1);
	}
	lck_mtx_unlock(&dbgport_mtx);
	return error;
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, poke, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, poke_sysctl, "A",
    "the debug port: r/w/m/sr/sw and the drivers' commands (help)");
