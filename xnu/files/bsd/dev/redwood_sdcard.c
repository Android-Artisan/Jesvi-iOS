/*
 * The S20's microSD slot (A, 2026-11-20; docs/devices/x1slte/sdcard.md): Synopsys DesignWare MMC 2.70a ("dwmmc2" at
 * 0x132E0000 in HSI1, the block of the embedded UFS and the Wi-Fi PCIe, so always powered while iOS runs).
 *
 * S-Boot leaves the slot initialised: card powered (PWREN 1), clocked (CLKENA 1), 4-bit bus (CTYPE 1), the card in
 * the "tran" state after its own CMD17. This first driver keeps that state and only reads: CMD18 (multiple blocks,
 * block addressing: SDHC/SDXC) with the controller's auto-stop, by PIO through the FIFO at +0x200 (CTRL 0: no
 * IDMAC yet). Read-only, and only with boot-arg sd=1 (default off).
 *
 *   /dev/rsdcard   raw, read-only; its size: the end of the last partition in the card's MBR (the card's own size,
 *                  CSD, needs its RCA, which only a re-identification gives -- next step)
 *   kern.redwood.sdcard   a line of state
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <sys/disk.h>
#include <sys/sysctl.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define SD_PHYS         0x132e0000
#define R_CTRL          0x00
#define R_PWREN         0x04
#define R_CLKENA        0x10
#define R_TMOUT         0x14
#define R_CTYPE         0x18
#define R_BLKSIZ        0x1c
#define R_BYTCNT        0x20
#define R_CMDARG        0x28
#define R_CMD           0x2c
#define R_RESP0         0x30
#define R_RINTSTS       0x44
#define R_STATUS        0x48
#define R_VERID         0x6c
#define R_FIFO          0x200

#define INT_CD          (1u << 2)       /* command done */
#define INT_DTO         (1u << 3)       /* data transfer over */
#define INT_RXDR        (1u << 5)       /* receive FIFO over its watermark */
#define INT_HTO         (1u << 10)      /* FIFO full, the card clock paused until it is drained: not an error */
#define INT_ERR         0xbbc2u         /* RE, RCRC, DCRC, RTO, DRTO, FRUN, HLE, SBE, EBE */

#define CMD_START       (1u << 31)
#define CMD_HOLD        (1u << 29)
#define CMD_WAIT_PREV   (1u << 13)
#define CMD_AUTO_STOP   (1u << 12)
#define CMD_DATA        (1u << 9)
#define CMD_CRC         (1u << 8)
#define CMD_RESP        (1u << 6)

#define SD_MAX_BLOCKS   128             /* 64 KB per command */

static volatile uint8_t *sd;
static uint32_t sd_mode;                /* boot-arg sd= */
static int sd_ready;
static uint64_t sd_blocks;              /* 512-byte blocks exposed */
static uint32_t sd_reads, sd_errors, sd_last_int;
LCK_GRP_DECLARE(sd_lck_grp, "redwood-sdcard");
LCK_MTX_DECLARE(sd_lock, &sd_lck_grp);

static inline uint32_t
rd(unsigned off)
{
	return *(volatile uint32_t *)(sd + off);
}

static inline void
wr(unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(sd + off) = v;
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

/* `count` blocks from block `lba` into `out`: CMD18 with auto-stop, PIO. 0 or an errno. sd_lock held. */
static int
sd_read_blocks(uint64_t lba, uint32_t count, uint8_t *out)
{
	uint32_t want = count * 128, got = 0, st;       /* 32-bit words */
	uint64_t deadline;

	if (count == 0 || count > SD_MAX_BLOCKS || lba > 0xffffffffull) {
		return EINVAL;
	}
	wr(R_RINTSTS, 0xffffffff);
	wr(R_BLKSIZ, 512);
	wr(R_BYTCNT, count * 512);
	wr(R_CMDARG, (uint32_t)lba);
	wr(R_CMD, CMD_START | CMD_HOLD | CMD_WAIT_PREV | (count > 1 ? CMD_AUTO_STOP : 0) | CMD_DATA | CMD_CRC | CMD_RESP |
	    (count > 1 ? 18u : 17u));
	deadline = now_us() + 500000 + (uint64_t)count * 2000;
	for (;;) {
		st = rd(R_RINTSTS);
		if (st & INT_ERR) {
			break;
		}
		/* drain the FIFO until it says empty (STATUS bit 2, FIFO_EMPTY) */
		while (got < want && !(rd(R_STATUS) & (1u << 2))) {
			((uint32_t *)out)[got++] = rd(R_FIFO);
		}
		if (st & (INT_RXDR | INT_HTO)) {
			wr(R_RINTSTS, st & (INT_RXDR | INT_HTO));
		}
		if ((st & INT_DTO) && got >= want) {
			break;
		}
		if (now_us() > deadline) {
			st |= 1u << 30;         /* (ours: timed out) */
			break;
		}
	}
	sd_last_int = st;
	wr(R_RINTSTS, 0xffffffff);
	if ((st & (INT_ERR | (1u << 30))) || got < want) {
		sd_errors++;
		return EIO;
	}
	sd_reads++;
	return 0;
}

static int
sd_copen(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (!sd_ready) {
		return ENXIO;
	}
	return (flags & FWRITE) ? EROFS : 0;
}

static int
sd_cclose(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
sd_cread(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint8_t *bounce;
	int error = 0;
	uint64_t size = sd_blocks * 512;

	if (!sd_ready) {
		return ENXIO;
	}
	if (uio_offset(uio) < 0 || uio_offset(uio) % 512 != 0 || uio_resid(uio) % 512 != 0) {
		return EINVAL;
	}
	bounce = kalloc_data(SD_MAX_BLOCKS * 512, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < size) {
		uint64_t at = (uint64_t)uio_offset(uio), want = (uint64_t)uio_resid(uio);

		if (want > SD_MAX_BLOCKS * 512) {
			want = SD_MAX_BLOCKS * 512;
		}
		if (want > size - at) {
			want = size - at;
		}
		lck_mtx_lock(&sd_lock);
		error = sd_read_blocks(at / 512, (uint32_t)(want / 512), bounce);
		lck_mtx_unlock(&sd_lock);
		if (error == 0) {
			error = uiomove((caddr_t)bounce, (int)want, uio);
		}
	}
	kfree_data(bounce, SD_MAX_BLOCKS * 512);
	return error;
}

static int
sd_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = 512;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = sd_blocks;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	default:
		return ENOTTY;
	}
}

static const struct cdevsw sd_cdevsw = {
	.d_open       = sd_copen,
	.d_close      = sd_cclose,
	.d_read       = sd_cread,
	.d_write      = eno_rdwrt,
	.d_ioctl      = sd_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

static char sd_state[160] = "off (boot-arg sd=1)";
SYSCTL_DECL(_kern_redwood);
SYSCTL_STRING(_kern_redwood, OID_AUTO, sdcard, CTLFLAG_RD | CTLFLAG_LOCKED, sd_state, 0, "the microSD slot");
SYSCTL_UINT(_kern_redwood, OID_AUTO, sdcard_reads, CTLFLAG_RD | CTLFLAG_LOCKED, &sd_reads, 0, "reads done");
SYSCTL_UINT(_kern_redwood, OID_AUTO, sdcard_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &sd_errors, 0, "reads failed");
SYSCTL_UINT(_kern_redwood, OID_AUTO, sdcard_last_int, CTLFLAG_RD | CTLFLAG_LOCKED, &sd_last_int, 0, "last RINTSTS");

void redwood_sdcard_init(void);
void
redwood_sdcard_init(void)
{
	uint32_t verid, resp;
	uint8_t *mbr;
	int cmaj;

	if (!PE_parse_boot_argn("sd", &sd_mode, sizeof(sd_mode)) || sd_mode == 0) {
		return;
	}
	sd = (volatile uint8_t *)ml_io_map(SD_PHYS, 0x1000);
	if (sd == NULL) {
		snprintf(sd_state, sizeof(sd_state), "no mapping");
		return;
	}
	verid = rd(R_VERID);
	resp = rd(R_RESP0);
	if ((verid & 0xffff0000) != 0x53420000 || rd(R_PWREN) != 1 || rd(R_CLKENA) != 1) {
		snprintf(sd_state, sizeof(sd_state), "not as S-Boot leaves it: VERID %#x PWREN %u CLKENA %u", verid,
		    rd(R_PWREN), rd(R_CLKENA));
		printf("sdcard: %s\n", sd_state);
		return;
	}
	mbr = kalloc_data(512, Z_WAITOK);
	if (mbr == NULL) {
		return;
	}
	lck_mtx_lock(&sd_lock);
	int error = sd_read_blocks(0, 1, mbr);
	lck_mtx_unlock(&sd_lock);
	if (error != 0 || mbr[510] != 0x55 || mbr[511] != 0xaa) {
		snprintf(sd_state, sizeof(sd_state), "no card or no MBR: read %d, RINTSTS %#x, RESP0 %#x", error, sd_last_int, resp);
		printf("sdcard: %s\n", sd_state);
		kfree_data(mbr, 512);
		return;
	}
	for (unsigned i = 0; i < 4; i++) {          /* the end of the furthest partition */
		const uint8_t *e = mbr + 446 + 16 * i;
		uint64_t start = (uint64_t)e[8] | (uint64_t)e[9] << 8 | (uint64_t)e[10] << 16 | (uint64_t)e[11] << 24;
		uint64_t len = (uint64_t)e[12] | (uint64_t)e[13] << 8 | (uint64_t)e[14] << 16 | (uint64_t)e[15] << 24;

		if (e[4] != 0 && start + len > sd_blocks) {
			sd_blocks = start + len;
		}
	}
	kfree_data(mbr, 512);
	cmaj = cdevsw_add(-1, &sd_cdevsw);
	if (cmaj < 0 || sd_blocks == 0) {
		snprintf(sd_state, sizeof(sd_state), "no device (major %d, %llu blocks)", cmaj, sd_blocks);
		return;
	}
	(void)devfs_make_node(makedev(cmaj, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rsdcard");
	sd_ready = 1;
	snprintf(sd_state, sizeof(sd_state), "ready, read-only: %llu MB (to the last partition), DW MMC %#x, 4-bit, S-Boot's setup",
	    sd_blocks / 2048, verid);
	printf("sdcard: %s\n", sd_state);
}
