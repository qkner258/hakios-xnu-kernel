/*
 * XNU loader: a UEFI application that plays the role of iBoot for an
 * open-source arm64 XNU kernel on non-Apple hardware.
 *
 *   - finds the "XNUP" payload in RAM (placed there by the boot shim on the
 *     phone, by `-device loader` under QEMU) and the Mach-O kernel inside it
 *   - picks a block of free RAM to be XNU's [physBase, physBase + memSize)
 *   - copies the Mach-O segments there, builds an Apple-format device tree
 *     and a boot_args structure behind the kernel
 *   - exits boot services, turns the MMU off and jumps to the kernel entry
 */
#include "efi.h"
#include "identity.h"         /* who iOS shall think we are: from the iPhone's own device tree (mkidentity.py) */
#include "../uefi/files/Platforms/Xiaomi/umiPkg/Drivers/UmiDisplayProbeDxe/UmiDisplayProbe.h"

/* ------------------------------------------------------------------------ */
/* Fixed conventions shared with the boot shim, the packer and the kernel   */

#ifndef PAYLOAD_BASE
#define PAYLOAD_BASE        0xF0000000ULL
#endif
#define PAYLOAD_MAGIC       0x50554E58u /* "XNUP" */

struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL /* boot_args.virtBase */
#define KERNEL_LINK_OFFSET  0x03000000ULL         /* kernel text sits 48 MB above the base */
#define BASE_ALIGN          0x02000000ULL         /* 32 MB: L2 block size with 16K pages */
#define MAX_MEM_SIZE        0x180000000ULL        /* 6 GB; the kernel's bootstrap page tables cover 8 */
/*
 * A root image too big for the payload area is placed here, at a fixed
 * offset inside the kernel's memory window, once; it then survives any
 * number of kernel runs (nothing writes to it: the root is read-only).
 */
#define RESIDENT_RD_OFFSET  0x08000000ULL
#define RESIDENT_RD_RESERVE 0x30000000ULL         /* RAM that must remain for the kernel itself */
#define PAGE_SIZE_MAX       0x4000ULL

#define LOG_PARTITION       u"logdump"
#define CHECKPOINT_SELFTEST 999
#define PANIC_LOG_SIZE      0x80000u
#define NVRAM_BANK_SIZE     0x2000u
#define PANIC_LOG_BASE      (PAYLOAD_BASE + PAYLOAD_MAX_SIZE - 0x100000ULL)
#define PAYLOAD_MAX_SIZE    0x04000000ULL         /* RAM reserved for the payload in the UEFI memory map */

/* ------------------------------------------------------------------------ */
/* XNU structures (pexpert/pexpert/arm64/boot.h, mach-o/loader.h)           */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

#define MH_MAGIC_64      0xFEEDFACFu
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

/* ------------------------------------------------------------------------ */
/* Globals and small helpers                                                */

static efi_system_table *ST;
static efi_boot_services *BS;
static efi_handle IMAGE;

static char log_buf[32768];
static int budget_dumped;
static uint32_t log_len;

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n > 0; n--, x++, y++) {
		if (*x != *y) {
			return *x < *y ? -1 : 1;
		}
	}
	return 0;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	/*
	 * Word at a time when possible: USB transfer buffers are uncached, where
	 * a byte loop costs one bus access per byte (6 MB/s instead of 30+).
	 */
	if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
		uint64_t *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;

		for (; n >= 32; n -= 32, dw += 4, sw += 4) {
			uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
		}
		for (; n >= 8; n -= 8) {
			*dw++ = *sw++;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

static size_t
str_len(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/*
 * Set once the loader takes commands over USB: its log travels that way, and
 * the screen is left to the kernel (and to "oem screenshot").
 */
static int screen_quiet;
/* ... unless what is said is the last thing a person will see of a dead machine: then it is said
 * on the screen even so (the kernel is not drawing any more, and nothing else can be read). */
static int screen_forced;

static void
put_char(char c)
{
	char16 w[3] = { 0, 0, 0 };

	if (log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
	if (ST->con_out != NULL && (!screen_quiet || screen_forced)) {
		if (c == '\n') {
			w[0] = '\r', w[1] = '\n';
		} else {
			w[0] = (char16)c;
		}
		ST->con_out->output_string(ST->con_out, w);
	}
}

/* printf subset: %s %x %d %p (64-bit values), %% -- %p is 16 hex digits, so an address can be
 * typed back into atos as it stands. */
static void
print(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put_char(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) {
				put_char(*s++);
			}
		} else if (*fmt == 'p') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			for (int i = 15; i >= 0; i--) {
				put_char("0123456789abcdef"[(v >> (i * 4)) & 0xfu]);
			}
		} else if (*fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = (*fmt == 'x') ? 16 : 10;
			char tmp[24];
			int i = 0;
			do {
				tmp[i++] = "0123456789abcdef"[v % base];
				v /= base;
			} while (v);
			while (i) {
				put_char(tmp[--i]);
			}
		} else {
			put_char(*fmt);
		}
	}
	__builtin_va_end(ap);
}

#define READ_SYSREG(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r" (_v)); _v; })

/*
 * Write back and drop cached copies of a range. Required for memory the
 * kernel will touch with the MMU (and so the data cache) off: its early code
 * builds page tables that way, and a stale cached copy of those pages would
 * be what the table walker sees once caches are turned on again.
 */
static void
flush_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc civac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

static void
clean_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

/* ------------------------------------------------------------------------ */
/* Log persistence: raw write to a GPT partition (phone only)               */

static efi_block_io *
find_log_partition(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	static const char16 want[] = LOG_PARTITION;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		int match = 1;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
			if (info->name[k] != want[k]) {
				match = 0;
				break;
			}
		}
		if (match && !EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			/* (the handle array is pool memory the caller owns; this runs on every log write) */
			BS->free_pool(handles);
			return bio;
		}
	}
	BS->free_pool(handles);
	return NULL;
}

/*
 * The "staged" buffer holds the loader log followed by the kernel log of the
 * last run. It is what gets written to the log partition and what the host
 * fetches over USB (fastboot get_staged).
 */
static char out_buf[2 * 1024 * 1024];
/* the storage's reads for the kernel land here (block_poll), and nothing else may be put in it */
#define BLOCK_BUFFER_SIZE  (4u << 20)
static uint8_t block_buffer[BLOCK_BUFFER_SIZE] __attribute__((aligned(0x2000)));
/*
 * Where the log is read back to compare it (write_logs). Not block_buffer: that is the kernel's
 * mailbox buffer, and a read-back landing in it while the kernel runs on the other core would
 * overwrite data the kernel is still copying out of it.
 */
static uint8_t log_verify[sizeof(out_buf) + 8192] __attribute__((aligned(0x2000)));
static const char *staged_data = out_buf;   /* what "upload" sends: normally the log in out_buf */
static uint64_t staged_len;
static uint64_t completed_log_len;

static void
compose_logs(const char *klog, uint64_t klog_len)
{
	static const char sep[] = "\n== KERNEL LOG ==\n", end[] = "\n== END OF LOG ==\n";
	uint64_t n = log_len;

	staged_data = out_buf;
	memset(out_buf, 0, sizeof(out_buf));
	memcpy(out_buf, log_buf, log_len);
	if (klog != NULL) {
		if (klog_len > sizeof(out_buf) - n - 8192) {
			klog_len = sizeof(out_buf) - n - 8192;
		}
		memcpy(out_buf + n, sep, sizeof(sep) - 1);
		n += sizeof(sep) - 1;
		memcpy(out_buf + n, klog, klog_len);
		n += klog_len;
		memcpy(out_buf + n, end, sizeof(end) - 1);
		n += sizeof(end) - 1;
	}
	staged_len = n;
	completed_log_len = klog != NULL ? n : 0;
}

static int hc_list_is_uefis(void);
static int hc_log_skips;                /* logs not written this run (said twice at most) */
static void hc_print_state(const char *when);
static void hc_take_back(const char *when);

static void
write_logs(const char *klog, uint64_t klog_len)
{
	/* The UMI bring-up keeps logs in RAM/USB; no raw partition writes. */
#ifdef LOADER_UMI
	efi_block_io *bio = NULL;
#else
	efi_block_io *bio = find_log_partition();
#endif

	compose_logs(klog, klog_len);
	if (bio == NULL) {
		/* No log partition (QEMU): echo the returned kernel log on the console instead. */
		if (klog != NULL) {
			print("\n== KERNEL LOG ==\n");
			for (uint64_t i = 0; i < klog_len; i++) {
				put_char(klog[i]);
			}
			print("\n== END OF LOG ==\n");
		}
		return;
	}
	uint64_t bs = bio->media->block_size;
	uint64_t len = (staged_len + 1 + bs - 1) / bs * bs;
	efi_status ws, rs = 0;
	int same = 0;

	/* only through UEFI's own transfer list (see hc_take_back) */
	if (!hc_list_is_uefis()) {
		if (hc_log_skips++ < 2) {
			hc_print_state("log not written");
			print("log: NOT WRITTEN to logdump: UEFI's transfer list is not the one in the controller\n");
		}
		compose_logs(klog, klog_len);
		return;
	}
	ws = bio->write_blocks(bio, bio->media->media_id, 0, len, out_buf);

	if (!EFI_ERROR(ws)) {
		bio->flush_blocks(bio);
		/*
		 * And read back: that the log is in the partition is what a run of the kernel's own UFS
		 * driver must leave possible (the controller is shared with this one). The line goes into
		 * the copy the host fetches over USB, not into the partition, which already has the rest.
		 */
		if (len <= sizeof(log_verify)) {
			rs = bio->read_blocks(bio, bio->media->media_id, 0, len, log_verify);
			flush_dcache((uint64_t)(uintptr_t)log_verify, (uint64_t)(uintptr_t)log_verify + len);
			same = !EFI_ERROR(rs) && memcmp(log_verify, out_buf, len) == 0;
		}
	}
	print("log: %d bytes written to logdump (%x), read back %s (%x)\n", len, ws,
	    EFI_ERROR(ws) ? "not tried" : same ? "the same" : "DIFFERENT", rs);
	compose_logs(klog, klog_len);
}

static void
flush_log(void)
{
	write_logs(NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* Apple device tree writer                                                 */
/*                                                                          */
/* node := u32 nProperties, u32 nChildren, props[], children[]              */
/* prop := char name[32], u32 length, value padded to 4 bytes               */

struct dt_writer {
	uint8_t *base, *cur;
	uint8_t *end;         /* the first byte after the room reserved for the tree */
	int overflow;         /* something did not fit: nothing more is written, run_kernel refuses the tree */
	uint32_t scratch[2];  /* the counts of a node that was not written (after an overflow) */
	uint32_t *counts[16]; /* [depth] -> {nProperties, nChildren} of the open node */
	int depth;
};

static void
dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	uint64_t padded = ((uint64_t)len + 3) & ~3ULL;

	/*
	 * Not past the room reserved for the tree (boot_args follows it), and not a name longer than the
	 * 32-byte field: either would overwrite what is next to it with no message.
	 */
	if (w->overflow || str_len(name) > 32 ||(uint64_t)(w->end - w->cur) < 36 + padded) {
		w->overflow = 1;
		return;
	}
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, str_len(name));
	*(uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (size_t)padded);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + padded;
	w->counts[w->depth - 1][0]++;
}

static void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)str_len(value) + 1);
}

static void
dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

/* Seconds since 1970 by the firmware's real-time clock, or 0 when it has none or it is not set. */
static uint64_t
firmware_epoch(void)
{
	static const uint16_t before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	struct efi_time t;
	uint64_t days;

	if (ST->rt == NULL || ST->rt->get_time == NULL || EFI_ERROR(ST->rt->get_time(&t, NULL)) ||
	    t.year < 2024 || t.year > 2099 || t.month < 1 || t.month > 12) {
		return 0;
	}
	days = (uint64_t)(t.year - 1970) * 365 + (t.year - 1969) / 4 + before_month[t.month - 1] + (t.day - 1);
	if (t.month > 2 && t.year % 4 == 0) {
		days++;
	}
	uint64_t secs = days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
	if (t.timezone != 0x7FF) {
		secs += (int64_t)t.timezone * 60;       /* local time = UTC - timezone */
	}
	return secs;
}

/* A string in a field of fixed size, the rest zeros. */
static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];

	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}

/*
 * Properties copied from the iPhone's device tree (identity.h), but one: the iPhone X's display is a
 * wide-gamut one ("artwork-display-gamut" P3), and with it iOS takes the screen for one that shows
 * extended colour (MobileGestalt's HasExtendedColorDisplay): WebKit then draws a page's tiles into
 * surfaces of ten bits a colour ('w30r', 'b3a8'), which neither the kernel's surfaces nor the window
 * server's renderer here do (eight bits, BGRA), and every page comes out black. Here the screen is
 * what the framebuffer is: sRGB.
 */
static int gamut_p3;    /* boot-arg gamut=p3 (a test): the iPhone's own "P3" left in place */

static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, "artwork-display-gamut") && !gamut_p3) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

/* One property of those copied from the iPhone's device tree, by name. */
static void
dt_identity_one(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data, const char *name)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, name)) {
			dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
		}
	}
}

static void
dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	if (w->overflow || (uint64_t)(w->end - w->cur) < 8) {
		/* no room: the node is not written, the depth is kept so that dt_end still pairs with it */
		w->overflow = 1;
		w->counts[w->depth++] = w->scratch;
		return;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void
dt_end(struct dt_writer *w)
{
	w->depth--;
}

/* ------------------------------------------------------------------------ */
/* Platform description                                                     */

struct platform {
	const char *name;
	uint64_t io_base, io_size;       /* one window covering the SoC peripherals */
	uint64_t gic_dist, gic_redist;   /* GICv3 */
	uint64_t uart;                   /* PL011, 0 if none */
	int psci_hvc;                    /* the firmware's power interface is called with HVC, not SMC */
	int log_other_cpu;               /* keep notes in the log partition while a kernel starts on another core */
	int cores;                       /* at most this many processor cores, numbered from 0 ... */
	int core_shift;                  /* ... in this bit of MPIDR: the phone numbers them in the second affinity field */
	uint32_t timeout_scale;          /* a kernel on several cores gets this much longer to wait for a lock, 0: as it is */
	uint32_t small_cores;            /* a bit for each of the slower cores, by number: "small-core" in the device tree */
	uint64_t fb_base;                /* the framebuffer the firmware reports (GOP): it tells this device apart */
};

static const struct platform platform_qemu_virt = {
	.name = "qemu-virt", .io_base = 0x08000000, .io_size = 0x08000000,
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .uart = 0x09000000, .psci_hvc = 1,
	.cores = 8, .core_shift = 0,
	/*
	 * QEMU's emulated cores stop each other all the time (every flush of a page from all the
	 * TLBs is a rendezvous of all its CPU threads), and the kernel's patience with a spin lock
	 * is that of real hardware: an eighth of a second by a 24 MHz clock, a twentieth here.
	 */
	.timeout_scale = 20,
};

static const struct platform platform_redwood = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.name = "redwood", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	/*
	 * Cores 0-3 are Cortex-A55s (1.8 GHz at most), 4-7 Cortex-A78s (2.4 GHz): a big one is about
	 * 2.5 times as fast. The kernel uses the small ones only when the big ones are all busy
	 * (XNU's machine_routines.c, ml_choose_slow_core_partners()).
	 */
	.small_cores = 0x0f,
	.fb_base = 0xE1000000,
};

/* UMI: GIC and MPIDR from the supplied DTB. GOP supplies the actual scanout. */
static const struct platform platform_umi = {
	.name = "umi", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000,
	.cores = 8, .core_shift = 8, .small_cores = 0x0f,
	/* Continuous splash from the older UMI port; an explicit UMI build also
	 * accepts a relocated GOP buffer after checking its dimensions. */
	.fb_base = 0x9C000000,
};

/*
 * The devices the loader knows, told apart by the framebuffer base their UEFI reports. A port to another
 * device adds its description here (common/new-device.sh writes a starting point, docs/PORTING.md says
 * where each value comes from); a display that matches none is QEMU's ramfb.
 */
static const struct platform *const platforms[] = {
	&platform_redwood,
};

/* ------------------------------------------------------------------------ */

static void
reboot(void)
{
	hc_take_back("before the reset");
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

/*
 * Return-from-kernel support.
 *
 * Boot services are never exited: UEFI stays resident outside XNU's memory
 * window. loader_save_ctx() records the UEFI execution environment and
 * returns 0. When the kernel is done it jumps to loader_return_stub (mapped
 * 1:1 or with the MMU off) with x0 = PA of its log, x1 = length, x2 = reason
 * (0 = orderly, 1 = early exception, then x3..x7 = ESR, ELR, FAR, LR, SP).
 * The stub switches the MMU off, reinstates the saved environment and makes
 * loader_save_ctx() return a second time, with 1.
 *
 * The EL1 physical timer (CNTP_*) is deliberately left alone: the Qualcomm
 * hypervisor traps it. The CTX_CNTP_* slots are unused.
 */
enum {
	CTX_TTBR0, CTX_TCR, CTX_MAIR, CTX_SCTLR, CTX_VBAR, CTX_CPACR, CTX_SP, CTX_SPSEL, CTX_DAIF,
	CTX_CNTV_CTL, CTX_CNTV_CVAL, CTX_CNTP_CTL, CTX_CNTP_CVAL, CTX_CNTKCTL,
	CTX_X18, CTX_X19, /* ... x30 */
	CTX_LOG_PA = CTX_X19 + 12, CTX_LOG_LEN, CTX_REASON, CTX_ESR, CTX_ELR, CTX_FAR, CTX_LR, CTX_KSP,
	CTX_TPIDR_EL1, CTX_TPIDR_EL0, CTX_TPIDRRO_EL0, CTX_SP_EL0, CTX_CONTEXTIDR, CTX_MDSCR, CTX_FPCR, CTX_FPSR,
	CTX_ICC_PMR, CTX_ICC_IGRPEN1, CTX_ICC_BPR1, CTX_COUNT
};
uint64_t loader_ctx[CTX_COUNT];
int loader_save_ctx(void);
void loader_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl loader_save_ctx\n"
        "loader_save_ctx:\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  mrs x9, ttbr0_el1\n   str x9, [x10, #0]\n"
        "  mrs x9, tcr_el1\n     str x9, [x10, #8]\n"
        "  mrs x9, mair_el1\n    str x9, [x10, #16]\n"
        "  mrs x9, sctlr_el1\n   str x9, [x10, #24]\n"
        "  mrs x9, vbar_el1\n    str x9, [x10, #32]\n"
        "  mrs x9, cpacr_el1\n   str x9, [x10, #40]\n"
        "  mov x9, sp\n          str x9, [x10, #48]\n"
        "  mrs x9, spsel\n       str x9, [x10, #56]\n"
        "  mrs x9, daif\n        str x9, [x10, #64]\n"
        "  mrs x9, cntv_ctl_el0\n  str x9, [x10, #72]\n"
        "  mrs x9, cntv_cval_el0\n str x9, [x10, #80]\n"
        "  mrs x9, cntkctl_el1\n   str x9, [x10, #104]\n"
        /* Qualcomm's UEFI has its own thread scheduler and keeps its current-thread
         * pointer in TPIDR_EL1; the kernel overwrites it, so it must come back intact. */
        "  mrs x9, tpidr_el1\n     str x9, [x10, #280]\n"
        "  mrs x9, tpidr_el0\n     str x9, [x10, #288]\n"
        "  mrs x9, tpidrro_el0\n   str x9, [x10, #296]\n"
        "  mrs x9, contextidr_el1\n str x9, [x10, #312]\n"
        "  mrs x9, mdscr_el1\n     str x9, [x10, #320]\n"
        "  mrs x9, s3_3_c4_c4_0\n          str x9, [x10, #328]\n"
        "  mrs x9, s3_3_c4_c4_1\n          str x9, [x10, #336]\n"
        /* GICv3 CPU interface: the kernel reprograms it for its own interrupts */
        "  mrs x9, icc_pmr_el1\n     str x9, [x10, #344]\n"
        "  mrs x9, icc_igrpen1_el1\n str x9, [x10, #352]\n"
        "  mrs x9, icc_bpr1_el1\n    str x9, [x10, #360]\n"
        "  mrs x9, spsel\n"
        "  cbz x9, 1f\n"                  /* SP_EL0 is only accessible while running on SP_EL1 */
        "  mrs x9, sp_el0\n        str x9, [x10, #304]\n"
        "1:\n"
        "  str x18, [x10, #112]\n"
        "  stp x19, x20, [x10, #120]\n"
        "  stp x21, x22, [x10, #136]\n"
        "  stp x23, x24, [x10, #152]\n"
        "  stp x25, x26, [x10, #168]\n"
        "  stp x27, x28, [x10, #184]\n"
        "  stp x29, x30, [x10, #200]\n"
        "  mov x0, #0\n"
        "  ret\n"
        ".balign 64\n"
        ".globl loader_return_stub\n"
        "loader_return_stub:\n"
        "  msr daifset, #0xf\n"
        /* PSTATE bits the kernel may have left set. PAN in particular is fatal:
         * UEFI maps its memory as EL0-accessible, so with PAN every data access
         * (including the exception handler's own stack push) faults. */
        "  .inst 0xd500409f\n"          /* msr PAN, #0 */
        "  .inst 0xd500407f\n"          /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #0]\n   msr ttbr0_el1, x9\n"
        "  ldr x9, [x10, #8]\n   msr tcr_el1, x9\n"
        "  ldr x9, [x10, #16]\n  msr mair_el1, x9\n"
        "  ldr x9, [x10, #32]\n  msr vbar_el1, x9\n"
        "  ldr x9, [x10, #40]\n  msr cpacr_el1, x9\n"
        "  isb\n"
        "  tlbi vmalle1\n"
        "  dsb sy\n"
        "  ic iallu\n"
        "  isb\n"
        "  ldr x9, [x10, #24]\n  msr sctlr_el1, x9\n"
        "  isb\n"
        "  ldr x9, [x10, #56]\n"
        "  cbz x9, 1f\n"
        "  msr spsel, #1\n"
        "  ldr x9, [x10, #304]\n msr sp_el0, x9\n"
        "  b 2f\n"
        "1:\n"
        "  msr spsel, #0\n"
        "2:\n"
        "  ldr x9, [x10, #48]\n  mov sp, x9\n"
        "  ldr x9, [x10, #280]\n msr tpidr_el1, x9\n"
        "  ldr x9, [x10, #288]\n msr tpidr_el0, x9\n"
        "  ldr x9, [x10, #296]\n msr tpidrro_el0, x9\n"
        "  ldr x9, [x10, #312]\n msr contextidr_el1, x9\n"
        "  ldr x9, [x10, #320]\n msr mdscr_el1, x9\n"
        "  ldr x9, [x10, #344]\n msr icc_pmr_el1, x9\n"
        "  ldr x9, [x10, #360]\n msr icc_bpr1_el1, x9\n"
        "  ldr x9, [x10, #352]\n msr icc_igrpen1_el1, x9\n"
        "  ldr x9, [x10, #328]\n msr s3_3_c4_c4_0, x9\n"
        "  ldr x9, [x10, #336]\n msr s3_3_c4_c4_1, x9\n"
        "  ldr x9, [x10, #104]\n msr cntkctl_el1, x9\n"
        "  ldr x9, [x10, #80]\n  msr cntv_cval_el0, x9\n"
        "  ldr x9, [x10, #72]\n  msr cntv_ctl_el0, x9\n"
        "  ldr x18, [x10, #112]\n"
        "  ldp x19, x20, [x10, #120]\n"
        "  ldp x21, x22, [x10, #136]\n"
        "  ldp x23, x24, [x10, #152]\n"
        "  ldp x25, x26, [x10, #168]\n"
        "  ldp x27, x28, [x10, #184]\n"
        "  ldp x29, x30, [x10, #200]\n"
        "  stp x0, x1, [x10, #216]\n"   /* kernel log PA and length, stored with caches on */
        "  stp x2, x3, [x10, #232]\n"   /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"   /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"   /* LR, SP */
        "  ldr x9, [x10, #64]\n  msr daif, x9\n"
        "  mov x0, #1\n"
        "  ret\n"
        );

__attribute__((noreturn)) static void
jump_to_kernel(uint64_t entry, uint64_t boot_args_pa)
{
	__asm__ volatile (
                "msr daifset, #0xf\n"
                /* Last thing before the MMU goes off: write this stack back to RAM
                 * (dirty cache lines do not survive, and we come back to this frame). */
                "mov x2, sp\n"
                "sub x2, x2, #0x400\n"
                "bic x2, x2, #63\n"
                "add x3, x2, #0x4000\n"
                "1: dc cvac, x2\n"
                "add x2, x2, #64\n"
                "cmp x2, x3\n"
                "b.lo 1b\n"
                "dsb sy\n"
                "mrs x2, sctlr_el1\n"
                "bic x2, x2, #0x1\n"    /* M: MMU off */
                "bic x2, x2, #0x4\n"    /* C: data cache off */
                "bic x2, x2, #0x1000\n" /* I: instruction cache off */
                "msr sctlr_el1, x2\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, %1\n"
                "br %0\n"
                : : "r" (entry), "r" (boot_args_pa) : "x0", "x2", "x3");
	__builtin_unreachable();
}

/* ------------------------------------------------------------------------ */
/* The kernel on another core.                                               */
/*                                                                          */
/* The firmware runs on core 0, one of the four small ones (Cortex-A55); the */
/* kernel uses a single core, and it had better be a big one (the A78s are   */
/* about 2.5 times as fast). With "bootcpu=N" among the kernel's arguments   */
/* the loader does not jump into the kernel itself: it has the firmware      */
/* (PSCI) start core N at other_cpu_entry, which enters the kernel exactly   */
/* as core 0 would have, and waits. UEFI stays resident and lives on core 0, */
/* so the kernel's way back is different too: other_cpu_return_stub, on core */
/* N, leaves what the kernel returned where core 0 looks for it, wakes core  */
/* 0 and switches its own core off.                                         */

#define PSCI_CPU_OFF       0x84000002u
#define PSCI_CPU_ON        0xC4000003u
#define PSCI_AFFINITY_INFO 0xC4000004u      /* (MPIDR, 0): 0 = that core is on, 1 = off, 2 = being started */
#define PSCI_NOT_SUPPORTED (-1)

/* each alone in a cache line: one core reads them with its caches off */
static struct {
	uint64_t entry, boot_args;
} other_cpu_start __attribute__((aligned(64)));
volatile uint64_t other_cpu_stage __attribute__((aligned(64)));  /* 1: that core has reached other_cpu_entry */
volatile uint64_t other_cpu_done __attribute__((aligned(64)));   /* 1: the kernel is back (other_cpu_return_stub) */
uint64_t psci_by_hvc __attribute__((aligned(64)));
void other_cpu_entry(void);
void other_cpu_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl other_cpu_entry\n"
        "other_cpu_entry:\n"                    /* x0 = &other_cpu_start; MMU and caches are off */
        "  msr daifset, #0xf\n"
        /* until the kernel has vectors of its own, an exception must not go nowhere */
        "  adrp x9, other_cpu_vectors\n"
        "  add  x9, x9, :lo12:other_cpu_vectors\n"
        "  msr vbar_el1, x9\n"
        /* what the firmware had set up on the core the kernel used to start on, the kernel may take
         * for granted: floating point allowed (CPACR), the fixed bits of SCTLR */
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #40]\n"
        "  msr cpacr_el1, x9\n"
        "  ldr x9, [x10, #24]\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x9, other_cpu_stage\n"
        "  add  x9, x9, :lo12:other_cpu_stage\n"
        "  mov x10, #1\n"
        "  str x10, [x9]\n"
        "  ldp x9, x10, [x0]\n"
        "  mov x0, x10\n"
        "  br x9\n"
        "other_cpu_fault:\n"                    /* as an early exception in the kernel: reason 1 */
        "  mrs x3, esr_el1\n"
        "  mrs x4, elr_el1\n"
        "  mrs x5, far_el1\n"
        "  mov x6, x30\n"
        "  mov x7, sp\n"
        "  mov x0, #0\n"
        "  mov x1, #0\n"
        "  mov x2, #1\n"
        "  b other_cpu_return_stub\n"
        ".balign 0x800\n"
        "other_cpu_vectors:\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        ".balign 64\n"
        ".globl other_cpu_return_stub\n"
        "other_cpu_return_stub:\n"              /* x0..x7 as for loader_return_stub */
        "  msr daifset, #0xf\n"
        "  .inst 0xd500409f\n"                  /* msr PAN, #0 */
        "  .inst 0xd500407f\n"                  /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  stp x0, x1, [x10, #216]\n"           /* kernel log PA and length */
        "  stp x2, x3, [x10, #232]\n"           /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"           /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"           /* LR, SP */
        "  adrp x10, other_cpu_done\n"
        "  add  x10, x10, :lo12:other_cpu_done\n"
        "  mov x9, #1\n"
        "  str x9, [x10]\n"
        "  dsb sy\n"
        "  sev\n"
        "  adrp x10, psci_by_hvc\n"
        "  add  x10, x10, :lo12:psci_by_hvc\n"
        "  ldr x9, [x10]\n"
        "  mov x0, #0x0002\n"                   /* PSCI CPU_OFF: does not return */
        "  movk x0, #0x8400, lsl #16\n"
        "  cbnz x9, 2f\n"
        "  smc #0\n"
        "1: wfi\n"
        "  b 1b\n"
        "2: hvc #0\n"
        "  b 1b\n"
        );

static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2);
static uint64_t smc_x3;
static const struct platform *PLAT;
static void clean_dcache(uint64_t start, uint64_t end);
static void flush_dcache(uint64_t start, uint64_t end);

/* What the kernel shows a watching loader (struct loader_live in XNU's loader_return.c). */
struct kernel_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every 10 seconds while the kernel is alive */
	uint64_t log, log_len, log_ring_at, log_wrapped;        /* the kernel's virtual addresses: add the slide */
	uint64_t log_head, log_ring;    /* the log: this much from the start of the boot, then a ring this big */
	uint64_t recording;             /* (a virtual address too) { physical address, bytes } of a screen recording */
	uint64_t stop;                  /* ours to write: the host wants the run to end ("oem stop") */
	uint64_t magic2;                /* "XNULIVE2": the kernel is new enough to have what follows */
	uint64_t sound;                 /* (a virtual address too) the same for a recording of the sound */
	uint64_t magic3;                /* "XNULIVE3": ... and to have what follows too */
	uint64_t mmio;                  /* (a virtual address too) the radio's last accesses, or 0: struct lx_mmio_ring */
	uint64_t magic4;                /* "XNULIVE4": ... and to have what follows too */
	uint64_t cpu_pc;                /* (a virtual address too) where each processor was, or 0: struct lx_cpu_breadcrumbs */
};

/*
 * The radio's last accesses of the chip's registers, written by the driver's bus on the phone
 * (wifi/lx/src/lx_bus.c; the description is bsd/sys/hakios.lx.h, struct lx_mmio_ring). The kernel
 * offers it in kernel_live.mmio once the driver is started, and the loader prints it when the
 * kernel has been silent for SILENCE_SECONDS: the entry still marked pending is the access that
 * never came back (work/v29g/INVESTIGATION-hotspot3.md, P-C). This must stay identical to the
 * kernel's structure; the ring carries its own size so that a mismatch is refused.
 */
#define LX_MMIO_ACCESSES        16
#define LX_MMIO_VERSION         1
#define LX_MMIO_MAGIC           0x47524f494d4d584cULL   /* "LXMMIORG" */

struct lx_mmio_access {
	uint64_t seq, when_ns, caller, value, answer;
	uint32_t cpu, what, offset, at, window, pending;
};

struct lx_mmio_ring {
	uint64_t magic, seq, lost;
	uint32_t entries, size, version, unused;
	struct lx_mmio_access access[LX_MMIO_ACCESSES];
};

/*
 * Where each processor was when the machine stopped, one entry per processor written by that
 * processor's own interrupt, idle and spin-lock paths (osfmk/arm64/umi_cpu_pc.h describes the
 * entry points). The kernel offers the table in kernel_live.cpu_pc; the loader prints it when the
 * kernel has been silent, beside the MMIO ring, and both record the time with the same clock
 * (lx_os_nanoseconds()), so the ages can be read against each other. This must stay identical to
 * the kernel's structure; the table carries its version and size so that a mismatch is refused.
 */
#define LX_CPU_PC_CORES         8
#define LX_CPU_PC_VERSION       6       /* (what the kernel writes now: version 6 adds the panic's record) */
#define LX_CPU_PC_VERSION_MIN   1       /* (a version 1 table is printed as it was: no irq/act on its lines) */
#define LX_CPU_PC_VERSION_INTID 2       /* (the first version whose entries carry the two INTIDs) */
#define LX_CPU_PC_VERSION_TEARDOWN 3    /* (the first whose entries carry a pmap teardown, and which has the exit ring) */
#define LX_CPU_PC_VERSION_MMIO  4       /* (the first whose entries carry the radio's bus marker; the entry is still 192) */
#define LX_CPU_PC_VERSION_DEBUGGER 5    /* (the first whose table has the debugger records after the exit ring) */
#define LX_CPU_PC_VERSION_PANIC 6       /* (the first whose table has the panic's record after those) */
#define LX_CPU_PC_MAGIC         0x43425550434d584cULL   /* "LXMCPUBC" */
#define LX_CPU_STATE_IRQ        0x1u                    /* the entry was written by an interrupt */
#define LX_CPU_STATE_USER       0x2u                    /* ... which interrupted user code */
#define LX_CPU_FLAG_IDLE        0x1u                    /* the processor was inside machine_idle() */
#define LX_CPU_FLAG_SPIN        0x2u                    /* ... and in a spin past the kernel's threshold */
#define LX_CPU_INTID_NONE       0xffffffffu             /* no INTID is being handled (entry->intids, high half) */
#define LX_CPU_EXIT_RING        8                       /* (how many exits the table keeps, newest last) */
#define LX_PMAP_PHASE_NONE      0u      /* no pmap teardown has run on this processor */
#define LX_PMAP_PHASE_ENTER     1u      /* about to issue the invalidations of a dying address space */
#define LX_PMAP_PHASE_TLBI      2u      /* ... issued; the dsb that waits for them has not returned */
#define LX_PMAP_PHASE_DVM_DONE  3u      /* ... and it returned: every DVM participant answered (version 4) */
#define LX_PMAP_PHASE_DONE      4u      /* ... and the isb after it has run */
#define LX_DBG_KIND_NONE        0u      /* (version 5) no debugger entry on this processor now */
#define LX_DBG_KIND_TRAP        1u      /* handle_debugger_trap() */
#define LX_DBG_KIND_DEBUGGER    2u      /* Debugger() / DebuggerWithContext() */
#define LX_DBG_KIND_XCALL_ENTER 3u      /* DebuggerXCallEnter(): it sent the SIGPdebugs */
#define LX_DBG_KIND_XCALL_WAIT  4u      /* DebuggerXCall(): a receiving end, waiting for the sender */
#define LX_DBG_KIND_BRK         5u      /* a kernel BRK (sleh.c, DebuggerCall()) */
#define LX_DBG_ROLE_INITIATOR   0x1u    /* (roles) the debugger was entered on it */
#define LX_DBG_ROLE_RECEIVER    0x2u    /* ... it was signalled into it and is waiting */

struct lx_cpu_breadcrumb {
	uint64_t pc, lr, sp, when_ns, count, tid, state, mpidr;
	uint64_t idle_entries, idle_since_ns, lock_caller, lock, spin_when_ns, spins, flags;
	uint64_t intids;                /* [31:0] the last INTID acknowledged, [63:32] the one being handled */
} __attribute__((aligned(64)));

/* (version 3: what follows those 128 bytes on each entry: the last pmap teardown that processor ran;
 * version 4 adds the radio's bus marker, in the words that were padding before) */
struct lx_cpu_pmap_record {
	uint64_t phase, when_ns, pmap, ids;     /* ids: [15:0] hw_asid, [31:16] the pid of the process */
	char comm[16];
	uint64_t mmio_since_ns;                 /* (version 4) when the processor's radio access began; 0: none */
	uint64_t mmio_seq;                      /* ... and its number in the MMIO ring; 0: none */
} __attribute__((aligned(64)));

/* (one process that has exited; the table's ring keeps the last LX_CPU_EXIT_RING of them) */
struct lx_proc_exit {
	uint64_t seq, when_ns, pid, cpu;
	char comm[16];
};

/*
 * (version 5) One processor's last debugger entry, and the table's first and newest of them; they
 * sit after the exit ring, so a table of a version the loader does not know is still refused and a
 * version 1..4 table is printed as it was. The kernel's osfmk/arm64/umi_cpu_pc.h says what each is.
 */
struct lx_cpu_debugger {
	uint64_t kind, since_ns, roles, exc_code, elr, extra;
	uint64_t xcall_func, xcall_arg, xcall_when_ns;
	uint64_t reserved[7];
} __attribute__((aligned(64)));

struct lx_dbg_globals {
	uint64_t count;
	uint64_t first_cpu, first_kind, first_exc_code, first_elr, first_when_ns;
	uint64_t last_cpu, last_kind, last_exc_code, last_elr, last_when_ns;
	uint64_t reserved[5];
} __attribute__((aligned(64)));

/*
 * (version 6) The panic the dead kernel entered: the message formatted at its first instruction
 * (before any lock, console or debugger), the pc that called panic(), the return addresses above
 * it, and the last step of the way out it reached -- so that a machine that says nothing else can
 * still be read from another processor. The kernel's osfmk/arm64/umi_cpu_pc.h says what each field
 * is and what the stages are, and this must stay identical to its structure.
 */
#define LX_PANIC_MSG            96
#define LX_PANIC_FPS            8
#define LX_PANIC_MAGIC          0x43494e41504d584cULL   /* "LXMPANIC" */
/* How much of the message its line keeps: the console is 80 columns and the caller's address takes
 * 24 of them (" caller " and 16 hex digits). Four return addresses are what fit after "PANIC BT: "
 * at 16 hex digits each; the record itself holds LX_PANIC_FPS of them. */
#define HUNG_PANIC_MSG_COLS     48
#define HUNG_PANIC_BT_COLS      4

#define LX_PANIC_STAGE_NONE             0u      /* no panic has reached this table */
#define LX_PANIC_STAGE_ENTRY            1u      /* the message is written */
#define LX_PANIC_STAGE_LOCK             2u      /* the debugger's lock is held */
#define LX_PANIC_STAGE_HALTED           3u      /* ... and the other processors were signalled */
#define LX_PANIC_STAGE_KDP_ENTER        4u      /* the KDP entry callouts have returned */
#define LX_PANIC_STAGE_PANIC_BEGIN      5u      /* the platform's panic actions are about to run */
#define LX_PANIC_STAGE_SAVEINFO         6u      /* SavePanicInfo(): backtraces and the panic stackshot */
#define LX_PANIC_STAGE_DIAG_DONE        7u      /* the diagnostics are done */
#define LX_PANIC_STAGE_REBOOT           8u      /* loader_return() is next */
#define LX_PANIC_STAGE_STOP_OTHERS      9u      /* psci_smp_stop_others() */
#define LX_PANIC_STAGE_QUIESCE_UFS      10u     /* ... and the driver cut off before the next one ... */
#define LX_PANIC_STAGE_QUIESCE_ADRENO   11u
#define LX_PANIC_STAGE_QUIESCE_WIFI     12u
#define LX_PANIC_STAGE_QUIESCE_ADSP     13u
#define LX_PANIC_STAGE_QUIESCE_MPSS     14u
#define LX_PANIC_STAGE_QUIESCE_DDR      15u
#define LX_PANIC_STAGE_QUIESCE_HAPTIC   16u
#define LX_PANIC_STAGE_QUIESCE_TORCH    17u
#define LX_PANIC_STAGE_QUIESCE_CAMERA   18u
#define LX_PANIC_STAGE_QUIESCE_MIC      19u
#define LX_PANIC_STAGE_JUMP             20u     /* loader_jump(): the way back itself */
#define LX_PANIC_STAGE_PSCI_BOOT_WANTED 21u     /* psci_smp.c: a processor that is not the boot one hands the return over */
#define LX_PANIC_STAGE_PSCI_MASTER_WAIT 22u     /* ... and waits for the boot processor to answer */
#define LX_PANIC_STAGE_PSCI_STOP_REST   23u     /* every other processor is told to switch itself off */
#define LX_PANIC_STAGE_PSCI_WAIT_CPU    24u     /* ... and each of them is waited for */
#define LX_PANIC_STAGE_PSCI_AFFINITY    25u     /* PSCI_AFFINITY_INFO: the firmware is asked whether one has gone */
#define LX_PANIC_STAGE_PSCI_CPU_OFF     26u     /* PSCI_CPU_OFF: this processor is switching itself off */
#define LX_PANIC_STAGE_PARKED           27u     /* (v34) the fast path stopped this processor on purpose */
#define LX_PANIC_STAGE_LAST             LX_PANIC_STAGE_PARKED

struct lx_panic_record {
	uint64_t magic, cpu, caller, when_ns, stage, stage_ns;
	uint64_t fps[LX_PANIC_FPS];
	uint64_t xcall_sync, xcall_sent, xcall_pending, ack_timeouts;
	uint32_t msg_len, stage_cpu;
	char msg[LX_PANIC_MSG];
} __attribute__((aligned(64)));

/*
 * The table itself: the two header lines and the entries. A version 3 kernel puts its exit ring
 * after the entries -- whose stride is the table's own `size`, not this structure's -- so the ring
 * is found at 64 + cores * size, its counter there and its entries 64 bytes further on.
 */
struct lx_cpu_breadcrumbs {
	uint64_t magic;
	uint32_t version, cores, size, unused;
	uint64_t gic_last_unhandled, gic_unhandled_count;
	uint64_t reserved[3];
	struct lx_cpu_breadcrumb core[LX_CPU_PC_CORES];
} __attribute__((aligned(64)));

#define LX_CPU_PC_CORE_OFFSET   64      /* (offsetof(struct lx_cpu_breadcrumbs, core), in the kernel too) */
#define LX_CPU_EXIT_RING_OFFSET 64      /* (the ring's counter is at its start; its entries follow in 64 bytes) */
#define LX_CPU_PC_DEBUG_OFFSET  (LX_CPU_EXIT_RING_OFFSET + LX_CPU_EXIT_RING * sizeof(struct lx_proc_exit))
#define LX_CPU_PC_PANIC_OFFSET  (LX_CPU_PC_DEBUG_OFFSET + LX_CPU_PC_CORES * sizeof(struct lx_cpu_debugger) + \
                                 sizeof(struct lx_dbg_globals))

/*
 * Battery state shared with XNU.  Qualcomm's charger firmware remains a UEFI
 * boot-service client on core 0 while XNU runs on another core.  The loader is
 * therefore the one safe place to call it; XNU only reads this cache.
 */
#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL /* "RDWBATT!" */
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent;
	int32_t voltage_mv;
	int32_t current_ma;
	int32_t temperature_c;
	uint32_t charger_action;
	uint32_t polls;
	uint32_t failures;
	uint32_t reserved;
};

/*
 * Layout returned by QcomChargerDxeLA's GetChargingAction, word offsets confirmed by dumping the
 * raw buffer on this phone: [2] state of charge, [3] charge current (signed mA), [4] battery
 * voltage (mV), [5] temperature (C). `rest` is oversized on purpose so a firmware that writes
 * more than the fields we name cannot smash the stack.
 */
struct qcom_battery_status {
	uint32_t status0, status1;
	int32_t state_of_charge;
	int32_t charge_current_ma;
	int32_t battery_voltage_mv;
	int32_t battery_temperature_c;
	uint32_t rest[26];
};

static struct loader_battery charger_shared __attribute__((aligned(0x2000))) = {
	.magic = LOADER_BATTERY_MAGIC,
	.version = LOADER_BATTERY_VERSION,
	.size = sizeof(struct loader_battery),
	.soc_percent = -1,
	.voltage_mv = -1,
	.current_ma = 0,
	.temperature_c = -273,
	.charger_action = 0xffffffffu,
};
static efi_charger_ex *CHARGER_EX;
static efi_qcom_charger *QCOM_CHARGER;
static int charger_enabled;

/* The screen recording the last kernel left in memory, if it made one ("oem recording" stages it). */
static const char *recording_data;
static uint64_t recording_len;
/* ... and the recording of the sound ("oem sound"). */
static const char *sound_data;
static uint64_t sound_len;
#define KERNEL_LIVE_MAGIC2      0x324556494c554e58ULL
#define KERNEL_LIVE_MAGIC3      0x334556494c554e58ULL
#define KERNEL_LIVE_MAGIC4      0x344556494c554e58ULL
#define KERNEL_LIVE_MAGIC       0x214556494c554e58ULL
#define OTHER_CPU_NOT_STARTED   1000    /* run_on_other_cpu(): the firmware said yes but the core never arrived */
#define OTHER_CPU_HUNG          1001    /* ... the kernel stopped giving signs of life */
#define START_SECONDS           5
#define SILENCE_SECONDS         50
#define PANIC_REPORT_SECONDS    2       /* ... or this long after a panic's record says it will not come back */
#define NOTE_SECONDS            150     /* hk: keep firmware write capacity until the budget dump */

static void wdt_pet(void);
static int wdt_armed;

/* (v34) How long the kernel had been silent when this report began: SILENCE_SECONDS in the
 * ordinary case, less when the kernel's own panic record ended the wait (run_on_other_cpu()). */
static uint64_t hung_silence_seconds = SILENCE_SECONDS;

/*
 * While a kernel runs on another core and this one watches it: where it shows its signs of life,
 * for the commands that the host sends meanwhile (usb_poll_command).
 */
static const struct kernel_live *running_live;
/*
 * Set for the whole of run_on_other_cpu(), whether the kernel has a live block or not.
 * running_live is NULL for a kernel without one, so it cannot say that a kernel is running.
 */
static int run_active;
static uint64_t running_slide;
/* The kernel image's physical range, and the physical RAM window it was loaded into (run_kernel()):
 * a dying kernel's live block is only read where these say it may be. */
static uint64_t kernel_image_lo, kernel_image_hi;
static uint64_t kernel_window_lo, kernel_window_hi;
static void usb_poll_command(void);
/* Device-tree mailbox: request/ack are uncached to XNU; flush on CPU0.
 * Isolated cache lines; PE/COFF permits at most 8 KiB static alignment. */
static struct {
	uint64_t magic;
	volatile uint64_t request, ack;
	uint8_t padding[0x4000 - 24];
} usb_handoff __attribute__((aligned(0x2000))) = { .magic = 0x32425355494d55ULL };
static int usb_kernel_requested;
/*
 * "hung_e2=1" in the kernel's own arguments: read the GIC/PCIe/SMMU state while a hang is reported.
 * Off by default: those are the only device registers the report reads, and on umi (not PROD) no
 * watchdog is armed, so a block whose clock died with the kernel would hang the bus for good.
 * Read by run_kernel(), used by report_hung_kernel() and hung_scanout_commit().
 */
static int hung_e2_requested;
static void usb_handoff_poll(uint64_t *, uint64_t *, int *);
static void usb_kernel_reset(void);

static uint64_t
seconds_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) / READ_SYSREG(CNTFRQ_EL0);
}

/* Reverse a stretch of memory in place. */
static void
reverse(char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n / 2; i++) {
		char c = p[i];

		p[i] = p[n - 1 - i];
		p[n - 1 - i] = c;
	}
}

/*
 * The log of a kernel that did not come back to hand it over: put its two parts in order
 * (the ring's oldest character first) and say where it is and how long.
 */
static void
log_of_hung_kernel(const struct kernel_live *live, uint64_t slide)
{
	char *log = (char *)(live->log + slide);
	uint32_t len, ring_at;

	loader_ctx[CTX_LOG_PA] = loader_ctx[CTX_LOG_LEN] = 0;
	len = *(volatile uint32_t *)(live->log_len + slide);
	ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	if (*(volatile uint8_t *)(live->log_wrapped + slide) && ring_at < live->log_ring) {
		/* rotate the ring left by ring_at: three reversals */
		reverse(log + live->log_head, ring_at);
		reverse(log + live->log_head + ring_at, live->log_ring - ring_at);
		reverse(log + live->log_head, live->log_ring);
		len = (uint32_t)(live->log_head + live->log_ring);
	}
	if (len <= live->log_head + live->log_ring) {
		loader_ctx[CTX_LOG_PA] = (uint64_t)(uintptr_t)log;
		loader_ctx[CTX_LOG_LEN] = len;
	}
}

/*
 * When the newest MMIO access was, once log_of_hung_mmio() has read the ring: the breadcrumbs
 * below are aged against it when it is newer than all of them, because a radio access and a
 * breadcrumb record the time with the same clock. Zero when there was no ring to read.
 */
static uint64_t hung_mmio_newest_ns;

/* The letter the record uses for an access: R, W, S (a window select) or ?. */
static const char *
hung_mmio_what(const struct lx_mmio_access *a)
{
	return a->what == 1 ? "R" : a->what == 2 ? "W" : a->what == 3 ? "S" : "?";
}

/*
 * The record of the radio's last accesses, found in the dead kernel's memory: NULL, and the reason
 * said with a line, when there is none this loader can read. Read-only; no device register.
 */
static const struct lx_mmio_ring *
hung_mmio_ring(const struct kernel_live *live, uint64_t slide)
{
	uint64_t beginning = (uint64_t)(uintptr_t)live;
	uint64_t through_sound = (uint64_t)(uintptr_t)&live->magic3 - beginning;   /* (where the old block ends) */
	uint64_t ring_va;
	const struct lx_mmio_ring *ring;

	/* (the kernel's stores are in its cache: look at memory, as note_kernel_progress() does). magic2 says the block
	 * goes as far as sound; magic3 and mmio are only read once it does -- a kernel of before v29h has neither, its
	 * object ends at sound, and the two words past it are whatever the rest of that image holds (A-3). */
	flush_dcache(beginning, beginning + through_sound);
	if (live->magic2 != KERNEL_LIVE_MAGIC2) {
		print("MMIO: this kernel's live block is too old to record the radio's accesses\n");
		return NULL;
	}
	flush_dcache(beginning + through_sound, beginning + sizeof(*live));
	if (live->magic3 != KERNEL_LIVE_MAGIC3 || live->mmio == 0) {
		print("MMIO: this kernel left no record of the radio's last accesses\n");
		return NULL;
	}
	/*
	 * The record's virtual address, translated with the same slide as the log, must land aligned and whole in the
	 * image the loader put in RAM: a dying kernel's corruption is exactly what this path runs for, and a load from
	 * an address that is not there is a data abort in firmware -- no report at all, then a watchdog reset (A-1).
	 */
	/* (no overflow test on mmio + slide: the slide takes a kernel virtual address to its physical one, so the sum
	 * wraps modulo 2^64 by design, exactly as live->log + slide does; the v29i hotspot hang was refused for
	 * fffffff007c90810 that way. The image/window range checks below are what keep a bad pointer out.) */
	if ((live->mmio & 7) != 0 || live->mmio < 0xfffffff000000000ULL) {
		print("MMIO: the record of the radio's accesses is not a pointer (%x) this kernel can have left\n", (uint64_t)live->mmio);
		return NULL;
	}
	ring_va = live->mmio + slide;
	if (ring_va < kernel_image_lo || ring_va >= kernel_image_hi || kernel_image_hi - ring_va < sizeof(*ring) ||
	    ring_va < kernel_window_lo || ring_va >= kernel_window_hi || kernel_window_hi - ring_va < sizeof(*ring)) {
		print("MMIO: the record of the radio's accesses at %x is outside the kernel image (%x..%x) in RAM\n",
		    ring_va, kernel_image_lo, kernel_image_hi);
		return NULL;
	}
	ring = (const struct lx_mmio_ring *)(uintptr_t)ring_va;
	flush_dcache((uint64_t)(uintptr_t)ring, (uint64_t)(uintptr_t)ring + sizeof(*ring));
	if (ring->magic != LX_MMIO_MAGIC || ring->version != LX_MMIO_VERSION ||
	    ring->entries != LX_MMIO_ACCESSES || ring->size != sizeof(struct lx_mmio_access)) {
		print("MMIO: the record of the radio's accesses is not one this loader knows (magic %x, version %x, size %x)\n",
		    ring->magic, (uint64_t)ring->version, (uint64_t)ring->size);
		return NULL;
	}
	return ring;
}

/*
 * The access that never came back: the newest entry still pending (the last one the driver began,
 * which is the one whose write or read the processor is stuck in). NULL when none is.
 */
static const struct lx_mmio_access *
hung_mmio_pending(const struct lx_mmio_ring *ring)
{
	const struct lx_mmio_access *stuck = NULL;
	uint64_t newest = ring->seq;
	uint64_t oldest = newest >= LX_MMIO_ACCESSES ? newest - LX_MMIO_ACCESSES + 1 : 1;
	uint64_t count = newest - oldest + 1;

	/* (at most the ring's entries, whatever seq and lost hold: a corrupted newest is a report, not a loop) (A-2) */
	if (count > LX_MMIO_ACCESSES) count = LX_MMIO_ACCESSES;
	for (uint64_t i = 0; i < count; i++) {
		const struct lx_mmio_access *a = &ring->access[(oldest + i) % LX_MMIO_ACCESSES];

		if (a->seq == oldest + i && a->pending) {
			stuck = a;
		}
	}
	return stuck;
}

/*
 * The radio's last register accesses, as the driver's bus wrote them down (struct lx_mmio_ring;
 * the kernel offers the pointer in loader_live.mmio). Printed oldest first, the entry still
 * pending marked with '*': that is the access that never came back, and with its offset, window
 * and caller (a kernel address: subtract the slide, then symbolicate with keepsyms) it says
 * whether it was a read or a write and which part of the driver asked for it. This is the whole
 * point of the record: a death without a panic and without a line leaves nothing else. The one
 * still pending is also said once, first of all, by report_hung_kernel(): these lines are the
 * first the screen loses, and three reports in a row lost its offset and cpu that way.
 */
static void
log_of_hung_mmio(const struct lx_mmio_ring *ring)
{
	uint64_t newest, oldest, count;
	uint64_t newest_ns = 0;

	hung_mmio_newest_ns = 0;
	if (ring == NULL) {
		return;
	}
	newest = ring->seq;
	oldest = newest >= LX_MMIO_ACCESSES ? newest - LX_MMIO_ACCESSES + 1 : 1;
	/* (at most the ring's entries, whatever seq and lost hold: a corrupted newest is a report, not a loop) (A-2) */
	count = newest - oldest + 1;
	if (count > LX_MMIO_ACCESSES) count = LX_MMIO_ACCESSES;
	print("MMIO: the radio's last register accesses, oldest first (* = never came back); %d recorded, %d not written down\n",
	    newest, ring->lost);
	for (uint64_t i = 0; i < count; i++) {
		const uint64_t seq = oldest + i;
		const struct lx_mmio_access *a = &ring->access[seq % LX_MMIO_ACCESSES];

		if (a->seq != seq) {
			continue;                       /* (an entry that was not written down: its number is the gap) */
		}
		newest_ns = a->when_ns;
		print("MMIO %s#%x cpu %x t %x: %s %x -> %x win %d val %x ans %x from %x\n",
		    a->pending ? "*" : " ", seq, (uint64_t)a->cpu, a->when_ns, hung_mmio_what(a), (uint64_t)a->offset,
		    (uint64_t)a->at, (uint64_t)a->window, a->value, a->answer, a->caller);
	}
	hung_mmio_newest_ns = newest_ns;
}

/*
 * Where each processor was when the machine stopped: one entry per processor, written by that
 * processor's own interrupt, idle and spin-lock paths (struct lx_cpu_breadcrumbs; the kernel
 * offers the pointer in loader_live.cpu_pc, osfmk/arm64/umi_cpu_pc.h describes the entry points).
 * One line per processor:
 *
 *   CPU 3 pc <pc> lr <lr> t <age>ms krn idle tid <tid>            (the last interrupt it took)
 *   CPU 3 pc <pc> t <age>ms krn spin <caller> lk <lock> s <age>ms  (and it was waiting for a lock)
 *
 * "krn"/"usr" say whether the interrupted pc is kernel or user code ("none": no entry yet). "t" is how long before the
 * newest record of either kind (the MMIO ring's newest access or the newest breadcrumb, the same
 * clock) that interrupt happened: t 0ms is a processor interrupted at the moment the machine
 * stopped, a large t a processor that has not run since. "idle" says it is
 * inside machine_idle(), asleep in WFI and not stopped. "spin" says it was waiting for the lock at
 * "lk", asked for from "caller", and that the wait had already passed the kernel's threshold
 * "s ms" before the newest record: that is the line that names a processor held up behind another
 * one. Both are kernel addresses, and this port has no kernel slide (the kernel runs at its link
 * address), so symbolicate any PC on these lines with:
 *
 *   atos -o kernel -arch arm64 -l 0xfffffff007004000 <pc>
 *
 * Read-only, and refused as a whole when the pointer, the table's magic, its version or its size
 * is not one this loader knows -- the same rules, and the same slide, as the MMIO ring above.
 *
 * A version 2 table (the kernel's own from v29j on) also says, per processor, which interrupt the
 * GIC gave it: "irq <n>" is the last INTID it acknowledged (its time is the "t" on the same line),
 * "act <n>" the one it was handling at the moment the machine stopped ("act -": none, the handler
 * had returned). SGI 0 is the signal between processors, 1 the stop, 27 the virtual timer; 32 and
 * above is a device's. All processors on the same "act" is one broadcast interrupt; all "act -"
 * with the same recent "irq" is a processor that came back from its handler, which puts the stop
 * elsewhere (a bus, the DVM, the power). A version 1 table has neither field and is printed as it
 * was, without them.
 *
 * A version 3 table (from v29v on, entries 192 bytes) also says how far that processor's last pmap
 * teardown got: "tlbi <phase> <age>ms <pmap> asid <n> pid <n> <comm>", where phase 1 is "about to
 * invalidate", 2 "the broadcast tlbi is issued and the dsb that waits for it has not returned",
 * 3 "it returned (every DVM participant answered) and the isb after it had not run" (from a version
 * 4 table; a version 3 kernel wrote 3 for the finished teardown), and 4 "the isb ran too" -- a
 * processor at 3 is one the v29z hang left on the isb, work/v29i/INVESTIGATION-hotspot-hang-v29z.md
 * §2.3 -- and the pmap, its ASID and the process that owned it are on the line. The same version
 * adds the exit ring: one line with the eight newest processes to exit (pid, name, how long before
 * the newest record, which processor ran the exit), so a teardown seen in flight can be named.
 *
 * A version 4 table (entries still 192 bytes: it takes the words that were padding) also says
 * whether the processor was inside one of the radio's register accesses: "mmio <age>ms #<n>", where
 * <n> is that access's own entry in the MMIO ring and <age> is how long before the newest record it
 * began. A processor marked there whose number is the pending entry of log_of_hung_mmio() is the
 * processor the access never came back to; a pending entry whose processor is not marked was
 * finished and the machine stopped elsewhere (INVESTIGATION-hotspot-hang-v29z §2.4).
 *
 * A version 5 table (entries still 192 bytes: the records are a block of their own after the exit
 * ring) also says whether the kernel's debugger was entered, on which processor and from where:
 * "xc <age>ms <func> <arg>" is the broadcast xcall that processor was about to run. The debugger's
 * own summary is the two LAST: lines log_of_hung_last() prints: how many entries there were, the
 * first and the newest (processor, hook, exception/code, pc, age), and "rx <mask>", the processors
 * still waiting in one when the machine stopped. That is what tells a machine parked in the
 * debugger's cross-call -- every processor on SGI 0, no panic, no line, no spin -- from hardware
 * that stopped (work/v29i/INVESTIGATION-hotspot-hang-v30.md §4.1).
 */
static const struct lx_cpu_breadcrumbs *hung_cpu_table;   /* (what hung_cpu_pc_open() accepted, for the dump below) */
static int hung_cpu_opened;                     /* (the report opens it before its title; the lines below reuse it) */

/*
 * The table of breadcrumbs the dead kernel left, read and checked: the same checks the report has
 * always made, and the same lines when it refuses one. A version this loader does not know, an
 * entry size its version did not have, or a table that reaches past the kernel image is refused
 * whole, before any of it is read as a structure.
 *
 * Opened once per report: report_hung_kernel() needs it for the panic's lines before it prints
 * anything else, and log_of_hung_cpu_pc() asks for it again -- the second ask is the same table,
 * and a refusal is said once.
 */
static const struct lx_cpu_breadcrumbs *
hung_cpu_pc_open(const struct kernel_live *live, uint64_t slide)
{
	const struct lx_cpu_breadcrumbs *table;
	uint64_t beginning = (uint64_t)(uintptr_t)live;
	uint64_t through_sound = (uint64_t)(uintptr_t)&live->magic3 - beginning;
	uint64_t table_va, through = sizeof(struct lx_cpu_breadcrumbs);
	uint64_t stride, cores_end;
	int teardown, debugger;

	if (hung_cpu_opened) {
		return hung_cpu_table;
	}
	hung_cpu_opened = 1;
	hung_cpu_table = NULL;

	/* (the kernel's stores are in its cache: look at memory, as log_of_hung_mmio() does. magic4 and
	 * cpu_pc are past the block magic3 covers, so they are read only once magic2 and magic3 have said
	 * the live block reaches as far as it did when this loader was built) */
	flush_dcache(beginning + through_sound, beginning + sizeof(*live));
	if (live->magic2 != KERNEL_LIVE_MAGIC2 || live->magic3 != KERNEL_LIVE_MAGIC3) {
		print("CPU: this kernel's live block is too old to leave breadcrumbs\n");
		return NULL;
	}
	if (live->magic4 != KERNEL_LIVE_MAGIC4 || live->cpu_pc == 0) {
		print("CPU: this kernel left no record of where its processors were\n");
		return NULL;
	}
	if ((live->cpu_pc & 63) != 0 || live->cpu_pc < 0xfffffff000000000ULL) {
		print("CPU: the record of where the processors were is not a pointer (%x) this kernel can have left\n",
		    live->cpu_pc);
		return NULL;
	}
	table_va = live->cpu_pc + slide;
	if (table_va < kernel_image_lo || table_va >= kernel_image_hi || kernel_image_hi - table_va < sizeof(*table) ||
	    table_va < kernel_window_lo || table_va >= kernel_window_hi || kernel_window_hi - table_va < sizeof(*table)) {
		print("CPU: the record of where the processors were at %x is outside the kernel image (%x..%x) in RAM\n",
		    table_va, kernel_image_lo, kernel_image_hi);
		return NULL;
	}
	table = (const struct lx_cpu_breadcrumbs *)(uintptr_t)table_va;
	/* (its first line says how big the entries are, and so how far the table reaches) */
	flush_dcache((uint64_t)(uintptr_t)table, (uint64_t)(uintptr_t)table + LX_CPU_PC_CORE_OFFSET);
	if (table->magic != LX_CPU_PC_MAGIC || table->version < LX_CPU_PC_VERSION_MIN ||
	    table->version > LX_CPU_PC_VERSION ||
	    (table->version < LX_CPU_PC_VERSION_TEARDOWN && table->size != sizeof(struct lx_cpu_breadcrumb)) ||
	    (table->version >= LX_CPU_PC_VERSION_TEARDOWN &&
	    table->size != sizeof(struct lx_cpu_breadcrumb) + sizeof(struct lx_cpu_pmap_record)) ||
	    table->cores == 0 || table->cores > LX_CPU_PC_CORES) {
		print("CPU: the record of where the processors were is not one this loader knows (magic %x version %x size %x cores %x)\n",
		    table->magic, (uint64_t)table->version, (uint64_t)table->size, (uint64_t)table->cores);
		return NULL;
	}
	teardown = table->version >= LX_CPU_PC_VERSION_TEARDOWN;
	stride = table->size;
	/* (the kernel's table always holds LX_CPU_PC_CORES entries -- `cores` only says how many of them
	 * to print -- so the exit ring is after all of them, at the same place in every table) */
	cores_end = LX_CPU_PC_CORE_OFFSET + (uint64_t)LX_CPU_PC_CORES * stride;
	if (cores_end > through) {
		through = cores_end;
	}
	if (teardown) {
		through = cores_end + LX_CPU_EXIT_RING_OFFSET + LX_CPU_EXIT_RING * sizeof(struct lx_proc_exit);
	}
	debugger = table->version >= LX_CPU_PC_VERSION_DEBUGGER;
	if (debugger) {
		/* (the debugger's records are the block after the ring: one per processor, then the table's) */
		through = cores_end + LX_CPU_PC_DEBUG_OFFSET + LX_CPU_PC_CORES * sizeof(struct lx_cpu_debugger) +
		    sizeof(struct lx_dbg_globals);
	}
	if (table->version >= LX_CPU_PC_VERSION_PANIC) {
		/* (version 6: and the panic's record after those) */
		through += sizeof(struct lx_panic_record);
	}
	if (table_va + through > kernel_image_hi || table_va + through > kernel_window_hi) {
		print("CPU: the record of where the processors were at %x reaches past the kernel image (%x..%x) in RAM\n",
		    table_va, kernel_image_lo, kernel_image_hi);
		return NULL;
	}
	hung_cpu_table = table;
	flush_dcache((uint64_t)(uintptr_t)table + LX_CPU_PC_CORE_OFFSET, (uint64_t)(uintptr_t)table + through);
	return table;
}

/*
 * (version 6) The panic's record of a table, or NULL when this table has none (a version 5 one):
 * it is at the same place in every table that has it, after the debugger's block.
 */
static const struct lx_panic_record *
hung_cpu_pc_panic(const struct lx_cpu_breadcrumbs *table)
{
	const char *base;

	if (table == NULL || table->version < LX_CPU_PC_VERSION_PANIC) {
		return NULL;
	}
	base = (const char *)table + LX_CPU_PC_CORE_OFFSET + (uint64_t)LX_CPU_PC_CORES * table->size;
	return (const struct lx_panic_record *)(const void *)(base + LX_CPU_PC_PANIC_OFFSET);
}

/*
 * (v34) Is a panic in the table that will not come back? The kernel takes the same decision
 * itself before it parks the panicking processor (osfmk/arm64/umi_cpu_pc.inc,
 * umi_cpu_pc_panic_fast()): a panic whose debugger cross-call reached every processor it
 * signalled, or timed out on one, never takes the way back to the loader. A panic that has not
 * run that cross-call yet might still return, so this says no and the ordinary 50 s of silence
 * decides. Read from the dead kernel's memory, like everything else the report reads.
 * (the debug loader alone watches for a hang; the production loader has the watchdog.)
 */
#ifndef HAKIOS_LOADER_PROD
static int
hung_panic_parked(const struct kernel_live *live, uint64_t slide)
{
	const struct lx_panic_record *panic = hung_cpu_pc_panic(hung_cpu_pc_open(live, slide));

	if (panic == NULL) {
		return 0;
	}
	/* (the kernel writes the message last, and its own stores are in its cache: look at memory) */
	flush_dcache((uint64_t)(uintptr_t)panic, (uint64_t)(uintptr_t)panic + sizeof(*panic));
	if (panic->magic != LX_PANIC_MAGIC) {
		return 0;
	}
	return panic->ack_timeouts != 0 || (panic->xcall_sent != 0 && panic->xcall_sync == 0);
}
#endif

static void
log_of_hung_cpu_pc(const struct kernel_live *live, uint64_t slide)
{
	const struct lx_cpu_breadcrumbs *table = hung_cpu_pc_open(live, slide);
	const char *core_base;
	const struct lx_proc_exit *exits = NULL;
	uint64_t newest = hung_mmio_newest_ns;
	uint64_t stride, cores_end;
	uint64_t exits_total = 0;
	uint32_t cores;
	int intids, teardown, mmio_mark, debugger;

	if (table == NULL) {
		return;
	}
	teardown = table->version >= LX_CPU_PC_VERSION_TEARDOWN;
	stride = table->size;
	cores = table->cores;
	core_base = (const char *)table + LX_CPU_PC_CORE_OFFSET;
	cores_end = LX_CPU_PC_CORE_OFFSET + (uint64_t)LX_CPU_PC_CORES * stride;
	intids = table->version >= LX_CPU_PC_VERSION_INTID;
	mmio_mark = table->version >= LX_CPU_PC_VERSION_MMIO;
	debugger = table->version >= LX_CPU_PC_VERSION_DEBUGGER;
	for (uint32_t i = 0; i < cores; i++) {
		const struct lx_cpu_breadcrumb *entry =
		    (const struct lx_cpu_breadcrumb *)(const void *)(core_base + (uint64_t)i * stride);

		if (entry->when_ns > newest) {
			newest = entry->when_ns;
		}
		if (mmio_mark) {
			/* (a processor inside one of the radio's accesses: that access began when the
			 * record says, on the same clock, so it is the newest record when it is) */
			const struct lx_cpu_pmap_record *record = (const struct lx_cpu_pmap_record *)(const void *)
			    ((const char *)entry + sizeof(struct lx_cpu_breadcrumb));

			if (record->mmio_seq != 0 && record->mmio_since_ns > newest) {
				newest = record->mmio_since_ns;
			}
		}
	}
	if (teardown) {
		/* (the ring's counter first: how many exits there have been, and the newest of them) */
		exits_total = *(const uint64_t *)(const void *)((const char *)table + cores_end);
		exits = (const struct lx_proc_exit *)(const void *)((const char *)table + cores_end + LX_CPU_EXIT_RING_OFFSET);
		for (uint32_t i = 0; i < LX_CPU_EXIT_RING; i++) {
			if (exits[i].seq != 0 && exits[i].when_ns > newest) {
				newest = exits[i].when_ns;
			}
		}
	}
	if (debugger) {
		/* (a debugger entry, if the newest record is one, ages everything else: the same clock) */
		const struct lx_dbg_globals *globals = (const struct lx_dbg_globals *)(const void *)
		    ((const char *)table + cores_end + LX_CPU_PC_DEBUG_OFFSET +
		    LX_CPU_PC_CORES * sizeof(struct lx_cpu_debugger));

		if (globals->count != 0 && globals->last_when_ns > newest) {
			newest = globals->last_when_ns;
		}
	}
	if (table->version >= LX_CPU_PC_VERSION_PANIC) {
		/* (version 6: the last step the panic reached is a record like the others, on the same
		 * clock -- and often the newest of them all: the machine stopped inside that step) */
		const struct lx_panic_record *panic = hung_cpu_pc_panic(table);

		if (panic->magic == LX_PANIC_MAGIC && panic->stage_ns > newest) {
			newest = panic->stage_ns;
		}
	}
	print("CPU: each processor's last interrupt; t = ms before the newest record (atos -l 0xfffffff007004000)\n");
	for (uint32_t i = 0; i < cores; i++) {
		const struct lx_cpu_breadcrumb *entry =
		    (const struct lx_cpu_breadcrumb *)(const void *)(core_base + (uint64_t)i * stride);
		const uint64_t age = newest >= entry->when_ns ? (newest - entry->when_ns) / 1000000 : 0;
		const char *state = (entry->state & LX_CPU_STATE_USER) ? "usr" :
		    (entry->state & LX_CPU_STATE_IRQ) ? "krn" : "none";
		const uint32_t last_intid = (uint32_t)entry->intids;
		const uint32_t active_intid = (uint32_t)(entry->intids >> 32);
		const int seen = (entry->state & LX_CPU_STATE_IRQ) != 0;

		if (entry->flags & LX_CPU_FLAG_SPIN) {
			const uint64_t spent = newest >= entry->spin_when_ns ? (newest - entry->spin_when_ns) / 1000000 : 0;

			print("CPU %d pc %p t %dms %s spin %p lk %p s %dms",
			    (uint64_t)i, entry->pc, age, state, entry->lock_caller, entry->lock, spent);
		} else {
			print("CPU %d pc %p lr %p t %dms %s%s tid %x",
			    (uint64_t)i, entry->pc, entry->lr, age, state,
			    (entry->flags & LX_CPU_FLAG_IDLE) ? " idle" : "", entry->tid);
		}
		if (intids) {
			/* (the kernel's GIC handler is the only writer of these; "none" is not 0, which is SGI 0) */
			if (!seen) {
				print(" irq - act -");
			} else if (active_intid == LX_CPU_INTID_NONE) {
				print(" irq %d act -", (uint64_t)last_intid);
			} else {
				print(" irq %d act %d", (uint64_t)last_intid, (uint64_t)active_intid);
			}
		}
		if (teardown) {
			/* (how far this processor's last pmap teardown got: "tlbi 2" is a processor waiting for
			 * the dsb that has to see every DVM participant answer its broadcast invalidation) */
			const struct lx_cpu_pmap_record *record = (const struct lx_cpu_pmap_record *)(const void *)
			    ((const char *)entry + sizeof(struct lx_cpu_breadcrumb));

			if (record->phase != LX_PMAP_PHASE_NONE) {
				const uint64_t spent = newest >= record->when_ns ? (newest - record->when_ns) / 1000000 : 0;
				const uint32_t asid = (uint32_t)(record->ids & 0xffffu);
				const uint32_t pid = (uint32_t)(record->ids >> 16);
				char comm[17];

				for (unsigned int k = 0; k < 16; k++) {
					comm[k] = record->comm[k];
				}
				comm[16] = '\0';
				print(" tlbi %d %dms %p asid %x pid %x %s", (uint64_t)record->phase, spent, record->pmap,
				    (uint64_t)asid, (uint64_t)pid, comm[0] != '\0' ? comm : "-");
			}
		}
		if (mmio_mark) {
			/* (inside one of the radio's register accesses: its number is the ring entry's, so a
			 * processor whose number is the ring's pending one is the processor it never returned to) */
			const struct lx_cpu_pmap_record *record = (const struct lx_cpu_pmap_record *)(const void *)
			    ((const char *)entry + sizeof(struct lx_cpu_breadcrumb));

			if (record->mmio_seq != 0) {
				const uint64_t spent = newest >= record->mmio_since_ns ?
				    (newest - record->mmio_since_ns) / 1000000 : 0;

				print(" mmio %dms #%x", spent, record->mmio_seq);
			}
		}
		if (debugger) {
			/* (the last broadcast xcall it was about to run: the function, its argument, and how long
			 * before the newest record -- an xcall still running when the machine stopped is one way
			 * every processor ends up on SGI 0 without having entered the debugger) */
			const struct lx_cpu_debugger *record = (const struct lx_cpu_debugger *)(const void *)
			    ((const char *)table + cores_end + LX_CPU_PC_DEBUG_OFFSET) + i;

			if (record->xcall_when_ns != 0) {
				const uint64_t spent = newest >= record->xcall_when_ns ?
				    (newest - record->xcall_when_ns) / 1000000 : 0;

				print(" xc %dms %x %x", spent, record->xcall_func, record->xcall_arg);
			}
			if (record->roles & LX_DBG_ROLE_RECEIVER) {
				print(" dbg-wait %dms", newest >= record->since_ns ?
				    (newest - record->since_ns) / 1000000 : 0);
			}
		}
		print("\n");
	}
	if (intids) {
		if (table->gic_unhandled_count != 0) {
			print("GIC: unhandled %d, the last %x (masked by the handler)\n",
			    table->gic_unhandled_count, table->gic_last_unhandled);
		} else {
			print("GIC: unhandled 0 (every interrupt had a handler)\n");
		}
	}
	if (teardown) {
		/* (one line: the newest exits, so that a teardown named above can be given a process) */
		if (exits_total == 0) {
			print("EXITS 0: no process has exited since boot\n");
		} else {
			print("EXITS %x (newest first):", exits_total);
			for (uint64_t k = 0; k < LX_CPU_EXIT_RING && k < exits_total; k++) {
				const uint64_t want = exits_total - k;
				const struct lx_proc_exit *entry = &exits[want % LX_CPU_EXIT_RING];
				char comm[17];

				if (entry->seq != want) {
					continue;               /* (that slot has been used again: its number is the gap) */
				}
				for (unsigned int j = 0; j < 16; j++) {
					comm[j] = entry->comm[j];
				}
				comm[16] = '\0';
				print(" %x %s %dms c%d |", entry->pid, comm[0] != '\0' ? comm : "-",
				    newest >= entry->when_ns ? (newest - entry->when_ns) / 1000000 : 0, entry->cpu);
			}
			print("\n");
		}
	}
	/* (the same clock for both records: log_of_hung_last() ages the debugger's entries against it,
	 * and an entry of the kernel's own is as much the newest record as a register access is) */
	hung_mmio_newest_ns = newest;
}

/*
 * What the interrupt controller, the SMMU and the two PCIe links say at the moment the machine
 * stopped: read-only registers, after the lines above and before the screen hold. This is the
 * half of the report that the breadcrumbs cannot give -- whether an interrupt is still active in
 * the distributor, whether the SMMU saw a fault (the "TLBI was never answered" of the hotspot hang
 * would show in sGFSR or in a context bank's FSR/TLBSTATUS), and whether a PCIe link fell out of
 * L0. The values are one short line each; what would not fit is written to the loader's log only
 * (the host fetches that over USB: "oem log"), where a line more costs nothing.
 *
 * Every read is to a block known to be clocked at that moment:
 *   - the GIC distributor and redistributors are in the always-on domain and this loader is using
 *     its timer through them while it holds the screen;
 *   - GCC (0x00100000) is always on; this file already reads it for the display (umi_display_clock_snapshot);
 *   - a PCIe controller's PARF is read only after its GDSC in GCC has said the power domain is on,
 *     which is exactly the order umi_pcie.c and umi_modem_pcie.inc bring the links up in;
 *   - apps_smmu (0x15000000) is enabled and translating for the whole session (USB, display,
 *     audio), and no code of ours turns it off while the kernel runs; if it were clocked off, this
 *     read itself would hang, which is why it is last and why the whole block has a build switch.
 * The GIC-600 error frame would be the place a DVM/synchronization error is recorded, but its
 * address is not in this SoC's device tree (only the distributor and the redistributors are), so
 * it is not read -- and the log says so.
 *
 * HUNG_MMIO_DUMP: a build may define it to 0 to leave the whole block out.
 */
#ifndef HUNG_MMIO_DUMP
#define HUNG_MMIO_DUMP  1
#endif

#if HUNG_MMIO_DUMP

#define HUNG_GICD_CTLR          0x0000ULL
#define HUNG_GICD_TYPER         0x0004ULL
#define HUNG_GICD_ISPENDR(w)    (0x0200ULL + 4ULL * (w))    /* (SPIs; words 1..: INTID 32 and above) */
#define HUNG_GICD_ISACTIVER(w)  (0x0300ULL + 4ULL * (w))
#define HUNG_GICR_FRAME         0x20000ULL
#define HUNG_GICR_TYPER         0x0008ULL
#define HUNG_GICR_TYPER_LAST    (1ULL << 4)
#define HUNG_GICR_WAKER         0x0014ULL
#define HUNG_GICR_PEND_SGI      0x10200ULL                  /* (SGI 0..15, PPI 16..31) */
#define HUNG_GICR_ACT_SGI       0x10300ULL
#define HUNG_SMMU_PHYS          0x15000000ULL
#define HUNG_SMMU_SCR0          0x0000ULL
#define HUNG_SMMU_IDR0          0x0020ULL
#define HUNG_SMMU_IDR1          0x0024ULL
#define HUNG_SMMU_SGFSR         0x0048ULL
#define HUNG_SMMU_SGFSYNR1      0x0054ULL
#define HUNG_SMMU_STLBGSTATUS   0x0074ULL                   /* (the global TLB sync status) */
#define HUNG_SMMU_SMR(i)        (0x0800ULL + 4ULL * (i))
#define HUNG_SMMU_S2CR(i)       (0x0c00ULL + 4ULL * (i))
#define HUNG_SMMU_CB_SCTLR      0x0000ULL
#define HUNG_SMMU_CB_TTBR0      0x0020ULL
#define HUNG_SMMU_CB_TTBR1      0x0028ULL
#define HUNG_SMMU_CB_TCR        0x0030ULL
#define HUNG_SMMU_CB_FSR        0x0058ULL
#define HUNG_SMMU_CB_FAR        0x0060ULL
#define HUNG_SMMU_CB_FSYNR0     0x0068ULL
#define HUNG_SMMU_CB_TLBSTATUS  0x07f4ULL
#define HUNG_SMMU_SID_DISPLAY   0x0820u                    /* (hakios.display.c: DISPLAY_SID) */
#define HUNG_SMMU_SID_USB       0x0000u                    /* (hakios.usb.c: stream 0, context bank 5) */
#define HUNG_GCC_PHYS           0x00100000ULL               /* (always on: the display's clock checks read it) */
#define HUNG_GCC_PCIE0_GDSCR    0x06b004ULL                 /* (umi_pcie.c: bit 31 = the domain is on) */
#define HUNG_GCC_PCIE2_GDSCR    0x006004ULL                 /* (umi_modem_pcie.inc: the same bit) */
#define HUNG_GDSC_ON            0x80000000u
#define HUNG_PCIE0_PARF         0x01c00000ULL
#define HUNG_PCIE2_PARF         0x01c10000ULL
#define HUNG_PCIE_SYS_CTRL      0x0000ULL
#define HUNG_PCIE_PHY_CTRL      0x0040ULL
#define HUNG_PCIE_LTSSM         0x01b0ULL
#define HUNG_PCIE_BDF2SID       0x2c00ULL

static uint32_t
hung_mmio_rd32(uint64_t pa)
{
	return *(volatile uint32_t *)(uintptr_t)pa;
}

static uint64_t
hung_mmio_rd64(uint64_t pa)
{
	return *(volatile uint64_t *)(uintptr_t)pa;
}

/* The INTIDs of one 32-bit SPI word, first at most eight, and how many more ("+n"): one line. */
static void
hung_spi_list(uint64_t gicd, uint32_t words, int active)
{
	unsigned shown = 0, total = 0;

	for (uint32_t w = 1; w < words && w < 32; w++) {
		uint32_t v = hung_mmio_rd32(gicd + (active ? HUNG_GICD_ISACTIVER(w) : HUNG_GICD_ISPENDR(w)));

		for (; v != 0; v &= v - 1) {
			unsigned bit = __builtin_ctz(v);

			if (shown < 8) {
				print(" %d", (uint64_t)(32u * w + bit));
				shown++;
			}
			total++;
		}
	}
	if (total == 0) {
		print(" none");
	} else if (total > shown) {
		print(" +%d", (uint64_t)(total - shown));
	}
}

static void
hung_gic_dump(uint64_t gicd, uint64_t gicr)
{
	int was_quiet = screen_quiet, was_forced = screen_forced;
	uint64_t typer_core[LX_CPU_PC_CORES];
	uint32_t pend[LX_CPU_PC_CORES], act[LX_CPU_PC_CORES], waker[LX_CPU_PC_CORES];
	int have[LX_CPU_PC_CORES];
	uint32_t ctlr, typer, lines;

	if (gicd == 0 || gicr == 0) {
		return;
	}
	ctlr = hung_mmio_rd32(gicd + HUNG_GICD_CTLR);
	typer = hung_mmio_rd32(gicd + HUNG_GICD_TYPER);
	if (ctlr == 0xffffffffu || typer == 0xffffffffu) {
		print("GIC: the distributor at %x does not answer (%x): skipped\n", gicd, (uint64_t)ctlr);
		return;
	}
	lines = (typer & 0x1fu) + 1u;
	print("GIC: ctlr %x typer %x", (uint64_t)ctlr, (uint64_t)typer);
	if (hung_cpu_table != NULL && hung_cpu_table->version >= LX_CPU_PC_VERSION_INTID) {
		print(" unhandled %x", hung_cpu_table->gic_unhandled_count);
		if (hung_cpu_table->gic_unhandled_count != 0) {
			print(" last %x", hung_cpu_table->gic_last_unhandled);
		}
	}
	print("\n");

	memset(have, 0, sizeof(have));
	for (uint32_t k = 0; k < LX_CPU_PC_CORES; k++) {
		uint64_t frame = gicr + (uint64_t)k * HUNG_GICR_FRAME;
		uint64_t t = hung_mmio_rd64(frame + HUNG_GICR_TYPER);
		uint32_t core = (uint32_t)((t >> 40) & 0xffu);       /* (TYPER[63:32]: Aff3.Aff2.Aff1.Aff0) */
		int last = (t & HUNG_GICR_TYPER_LAST) != 0;

		if (t != ~0ULL && core < LX_CPU_PC_CORES && !have[core]) {
			uint32_t w = hung_mmio_rd32(frame + HUNG_GICR_WAKER);
			uint32_t p = hung_mmio_rd32(frame + HUNG_GICR_PEND_SGI);
			uint32_t a = hung_mmio_rd32(frame + HUNG_GICR_ACT_SGI);

			/* (a frame the bus did not answer for reads as all ones: do not show it as empty) */
			if (w != 0xffffffffu && p != 0xffffffffu && a != 0xffffffffu) {
				typer_core[core] = t;
				waker[core] = w;
				pend[core] = p;
				act[core] = a;
				have[core] = 1;
			}
		}
		if (last) {
			break;
		}
	}
	for (uint32_t half = 0; half < 2; half++) {
		print("GICR pend/act");
		for (uint32_t c = half * 4u; c < half * 4u + 4u; c++) {
			if (have[c]) {
				print(" %d %x/%x", (uint64_t)c, (uint64_t)pend[c], (uint64_t)act[c]);
			} else {
				print(" %d -", (uint64_t)c);
			}
		}
		print("\n");
	}
	print("GICD spi pend");
	hung_spi_list(gicd, lines, 0);
	print(" act");
	hung_spi_list(gicd, lines, 1);
	print("\n");

	/* (the log's copy: the bitmaps above are pending/active for SGI 0..15 and PPI 16..31, per core) */
	screen_quiet = 1;
	screen_forced = 0;
	print("GIC detail: ctlr %x typer %x, %d SPI words; no GIC-600 error frame address is known on this SoC (skipped)\n",
	    (uint64_t)ctlr, (uint64_t)typer, (uint64_t)lines);
	for (uint32_t c = 0; c < LX_CPU_PC_CORES; c++) {
		if (have[c]) {
			print("GIC core %d typer %p waker %x pend %x act %x\n",
			    (uint64_t)c, typer_core[c], (uint64_t)waker[c], (uint64_t)pend[c], (uint64_t)act[c]);
		}
	}
	for (uint32_t w = 1; w < lines && w < 32; w++) {
		uint32_t p = hung_mmio_rd32(gicd + HUNG_GICD_ISPENDR(w));
		uint32_t a = hung_mmio_rd32(gicd + HUNG_GICD_ISACTIVER(w));

		if (p != 0 || a != 0) {
			print("GIC spi INTID %d..%d pend %x act %x\n",
			    (uint64_t)(32u * w), (uint64_t)(32u * w + 31u), (uint64_t)p, (uint64_t)a);
		}
	}
	screen_quiet = was_quiet;
	screen_forced = was_forced;
}

/* One stream's context bank in apps_smmu, found the way hakios.display.c's dpu_smmu_probe() does. */
struct hung_smmu_cb {
	uint32_t found;                 /* (a stream match entry covers the SID) */
	uint32_t smr, s2cr;
	uint32_t cb;                    /* 0xffffffff: translated by no context bank (bypassed or faulting) */
	uint32_t cbar, cba2r;
	uint32_t sctlr, tcr, ttbr0, ttbr1, fsr, far, fsynr, tlb;
};

static void
hung_smmu_lookup(uint64_t smmu, uint32_t idr0, uint32_t idr1, uint32_t sid, struct hung_smmu_cb *out)
{
	uint32_t nsmr = idr0 & 0xffu;
	uint32_t ncb = idr1 & 0xffu;
	uint32_t pgshift = (idr1 & (1u << 31)) ? 16u : 12u;
	uint32_t numpage = 1u << (((idr1 >> 28) & 7u) + 1u);
	uint64_t gr1 = smmu + (1ULL << pgshift);          /* (CBAR/CBA2R live on the second page) */

	memset(out, 0, sizeof(*out));
	out->cb = 0xffffffffu;
	for (uint32_t i = 0; i < nsmr && i < 256u; i++) {
		uint32_t smr = hung_mmio_rd32(smmu + HUNG_SMMU_SMR(i));
		uint32_t id = smr & 0x7fffu, mask = (smr >> 16) & 0x7fffu;

		if (!(smr & (1u << 31)) || ((id ^ sid) & ~mask & 0x7fffu) != 0) {
			continue;
		}
		uint32_t s2cr = hung_mmio_rd32(smmu + HUNG_SMMU_S2CR(i));
		uint32_t type = (s2cr >> 16) & 3u, cb = s2cr & 0xffu;

		out->found = 1;
		out->smr = smr;
		out->s2cr = s2cr;
		if (type != 0 || cb >= ncb) {
			return;              /* (1 bypass, 2/3 fault: no context bank of its own to read) */
		}
		uint64_t bank = smmu + ((uint64_t)numpage << pgshift) + ((uint64_t)cb << pgshift);

		/* (the block is 1 MiB in this SoC's device tree: never read past it, whatever IDR1 says) */
		if (bank < smmu || bank + HUNG_SMMU_CB_TLBSTATUS + 4 > smmu + 0x100000) {
			return;
		}
		out->cb = cb;
		out->cbar = hung_mmio_rd32(gr1 + 4u * cb);
		out->cba2r = hung_mmio_rd32(gr1 + 0x800u + 4u * cb);
		out->sctlr = hung_mmio_rd32(bank + HUNG_SMMU_CB_SCTLR);
		out->tcr = hung_mmio_rd32(bank + HUNG_SMMU_CB_TCR);
		out->ttbr0 = hung_mmio_rd32(bank + HUNG_SMMU_CB_TTBR0);
		out->ttbr1 = hung_mmio_rd32(bank + HUNG_SMMU_CB_TTBR1);
		out->fsr = hung_mmio_rd32(bank + HUNG_SMMU_CB_FSR);
		out->far = hung_mmio_rd32(bank + HUNG_SMMU_CB_FAR);
		out->fsynr = hung_mmio_rd32(bank + HUNG_SMMU_CB_FSYNR0);
		out->tlb = hung_mmio_rd32(bank + HUNG_SMMU_CB_TLBSTATUS);
		return;
	}
}

static void
hung_smmu_cb_print(const struct hung_smmu_cb *cb, uint32_t sid)
{
	if (!cb->found) {
		print("sid%x no-match", (uint64_t)sid);
	} else if (cb->cb == 0xffffffffu) {
		print("sid%x s2cr %x no-cb", (uint64_t)sid, (uint64_t)cb->s2cr);
	} else {
		print("sid%x cb%d sctlr %x fsr %x tlb %x", (uint64_t)sid, (uint64_t)cb->cb,
		    (uint64_t)cb->sctlr, (uint64_t)cb->fsr, (uint64_t)cb->tlb);
	}
}

static void
hung_smmu_dump(void)
{
	int was_quiet = screen_quiet, was_forced = screen_forced;
	uint64_t smmu = HUNG_SMMU_PHYS;
	uint32_t scr0 = hung_mmio_rd32(smmu + HUNG_SMMU_SCR0);
	uint32_t idr0 = hung_mmio_rd32(smmu + HUNG_SMMU_IDR0);
	uint32_t idr1 = hung_mmio_rd32(smmu + HUNG_SMMU_IDR1);
	struct hung_smmu_cb display, usb;

	if (scr0 == 0xffffffffu || idr0 == 0xffffffffu) {
		print("SMMU: %x does not answer (cr0 %x idr0 %x): skipped\n", smmu, (uint64_t)scr0, (uint64_t)idr0);
		return;
	}
	print("SMMU: cr0 %x idr0 %x gfsr %x gfsynr1 %x tlbgstatus %x\n",
	    (uint64_t)scr0, (uint64_t)idr0, (uint64_t)hung_mmio_rd32(smmu + HUNG_SMMU_SGFSR),
	    (uint64_t)hung_mmio_rd32(smmu + HUNG_SMMU_SGFSYNR1),
	    (uint64_t)hung_mmio_rd32(smmu + HUNG_SMMU_STLBGSTATUS));
	hung_smmu_lookup(smmu, idr0, idr1, HUNG_SMMU_SID_DISPLAY, &display);
	hung_smmu_lookup(smmu, idr0, idr1, HUNG_SMMU_SID_USB, &usb);
	print("SMMU: ");
	hung_smmu_cb_print(&display, HUNG_SMMU_SID_DISPLAY);
	print(" | ");
	hung_smmu_cb_print(&usb, HUNG_SMMU_SID_USB);
	print("\n");

	screen_quiet = 1;
	screen_forced = 0;
	print("SMMU detail: idr0 %x idr1 %x: %d stream match registers, %d context banks, %d-byte pages\n",
	    (uint64_t)idr0, (uint64_t)idr1, (uint64_t)(idr0 & 0xffu), (uint64_t)(idr1 & 0xffu),
	    (uint64_t)((idr1 & (1u << 31)) ? 0x10000u : 0x1000u));
	for (int which = 0; which < 2; which++) {
		const struct hung_smmu_cb *cb = which == 0 ? &display : &usb;
		uint32_t sid = which == 0 ? HUNG_SMMU_SID_DISPLAY : HUNG_SMMU_SID_USB;

		if (!cb->found || cb->cb == 0xffffffffu) {
			continue;               /* (the summary line already said "no-match"/"s2cr … no-cb") */
		}
		print("SMMU sid %x smr %x s2cr %x cb %d cbar %x cba2r %x sctlr %x tcr %x ttbr0 %x ttbr1 %x fsr %x far %x fsynr %x tlb %x\n",
		    (uint64_t)sid, (uint64_t)cb->smr, (uint64_t)cb->s2cr, (uint64_t)cb->cb,
		    (uint64_t)cb->cbar, (uint64_t)cb->cba2r, (uint64_t)cb->sctlr, (uint64_t)cb->tcr,
		    (uint64_t)cb->ttbr0, (uint64_t)cb->ttbr1, (uint64_t)cb->fsr, (uint64_t)cb->far,
		    (uint64_t)cb->fsynr, (uint64_t)cb->tlb);
	}
	screen_quiet = was_quiet;
	screen_forced = was_forced;
}

static void
hung_pcie_dump(void)
{
	int was_quiet = screen_quiet, was_forced = screen_forced;
	uint32_t gdsc0 = hung_mmio_rd32(HUNG_GCC_PHYS + HUNG_GCC_PCIE0_GDSCR);
	uint32_t gdsc2 = hung_mmio_rd32(HUNG_GCC_PHYS + HUNG_GCC_PCIE2_GDSCR);
	int on0 = (gdsc0 & HUNG_GDSC_ON) != 0;
	int on2 = (gdsc2 & HUNG_GDSC_ON) != 0;

	print("PCIE:");
	if (on0) {
		print(" p0 ltssm %x bdf2sid %x", (uint64_t)hung_mmio_rd32(HUNG_PCIE0_PARF + HUNG_PCIE_LTSSM),
		    (uint64_t)hung_mmio_rd32(HUNG_PCIE0_PARF + HUNG_PCIE_BDF2SID));
	} else {
		print(" p0 off gdsc %x", (uint64_t)gdsc0);
	}
	if (on2) {
		print(" | p2 ltssm %x bdf2sid %x", (uint64_t)hung_mmio_rd32(HUNG_PCIE2_PARF + HUNG_PCIE_LTSSM),
		    (uint64_t)hung_mmio_rd32(HUNG_PCIE2_PARF + HUNG_PCIE_BDF2SID));
	} else {
		print(" | p2 off gdsc %x", (uint64_t)gdsc2);
	}
	print("\n");

	screen_quiet = 1;
	screen_forced = 0;
	print("PCIE detail: gdsc0 %x gdsc2 %x\n", (uint64_t)gdsc0, (uint64_t)gdsc2);
	if (on0) {
		print("PCIE p0 sys %x phy %x (LTSSM 0x11 = L0)\n",
		    (uint64_t)hung_mmio_rd32(HUNG_PCIE0_PARF + HUNG_PCIE_SYS_CTRL),
		    (uint64_t)hung_mmio_rd32(HUNG_PCIE0_PARF + HUNG_PCIE_PHY_CTRL));
	}
	if (on2) {
		print("PCIE p2 sys %x phy %x (LTSSM 0x11 = L0)\n",
		    (uint64_t)hung_mmio_rd32(HUNG_PCIE2_PARF + HUNG_PCIE_SYS_CTRL),
		    (uint64_t)hung_mmio_rd32(HUNG_PCIE2_PARF + HUNG_PCIE_PHY_CTRL));
	}
	screen_quiet = was_quiet;
	screen_forced = was_forced;
}

static void
log_of_hung_mmio_state(void)
{
	if (PLAT != &platform_umi) {
		return;                 /* (the addresses above are this phone's) */
	}
	hung_gic_dump(PLAT->gic_dist, PLAT->gic_redist);
	hung_pcie_dump();
	hung_smmu_dump();               /* (last: the one block with no power-domain check to make first) */
}

#endif /* HUNG_MMIO_DUMP */

/* (version 5) The hook a debugger entry names, as the report prints it, in three or four letters. */
static const char *
hung_dbg_kind(uint64_t kind)
{
	switch (kind) {
	case LX_DBG_KIND_TRAP:
		return "trap";
	case LX_DBG_KIND_DEBUGGER:
		return "dbg";
	case LX_DBG_KIND_XCALL_ENTER:
		return "xin";
	case LX_DBG_KIND_XCALL_WAIT:
		return "xrx";
	case LX_DBG_KIND_BRK:
		return "brk";
	default:
		return "none";
	}
}

/* (version 6) The step of the panic's way out a stage number names, as the report prints it. */
static const char *
hung_panic_stage(uint64_t stage)
{
	switch (stage) {
	case LX_PANIC_STAGE_ENTRY:
		return "ENTRY";
	case LX_PANIC_STAGE_LOCK:
		return "LOCK";
	case LX_PANIC_STAGE_HALTED:
		return "HALTED";
	case LX_PANIC_STAGE_KDP_ENTER:
		return "KDP_ENTER";
	case LX_PANIC_STAGE_PANIC_BEGIN:
		return "PANIC_BEGIN";
	case LX_PANIC_STAGE_SAVEINFO:
		return "SAVEINFO";
	case LX_PANIC_STAGE_DIAG_DONE:
		return "DIAG_DONE";
	case LX_PANIC_STAGE_REBOOT:
		return "REBOOT";
	case LX_PANIC_STAGE_STOP_OTHERS:
		return "STOP_OTHERS";
	case LX_PANIC_STAGE_QUIESCE_UFS:
		return "QUIESCE_UFS";
	case LX_PANIC_STAGE_QUIESCE_ADRENO:
		return "QUIESCE_ADRENO";
	case LX_PANIC_STAGE_QUIESCE_WIFI:
		return "QUIESCE_WIFI";
	case LX_PANIC_STAGE_QUIESCE_ADSP:
		return "QUIESCE_ADSP";
	case LX_PANIC_STAGE_QUIESCE_MPSS:
		return "QUIESCE_MPSS";
	case LX_PANIC_STAGE_QUIESCE_DDR:
		return "QUIESCE_DDR";
	case LX_PANIC_STAGE_QUIESCE_HAPTIC:
		return "QUIESCE_HAPTIC";
	case LX_PANIC_STAGE_QUIESCE_TORCH:
		return "QUIESCE_TORCH";
	case LX_PANIC_STAGE_QUIESCE_CAMERA:
		return "QUIESCE_CAMERA";
	case LX_PANIC_STAGE_QUIESCE_MIC:
		return "QUIESCE_MIC";
	case LX_PANIC_STAGE_JUMP:
		return "JUMP";
	case LX_PANIC_STAGE_PSCI_BOOT_WANTED:
		return "PSCI_BOOT_WANTED";
	case LX_PANIC_STAGE_PSCI_MASTER_WAIT:
		return "PSCI_MASTER_WAIT";
	case LX_PANIC_STAGE_PSCI_STOP_REST:
		return "PSCI_STOP_REST";
	case LX_PANIC_STAGE_PSCI_WAIT_CPU:
		return "PSCI_WAIT_CPU";
	case LX_PANIC_STAGE_PSCI_AFFINITY:
		return "PSCI_AFFINITY";
	case LX_PANIC_STAGE_PSCI_CPU_OFF:
		return "PSCI_CPU_OFF";
	case LX_PANIC_STAGE_PARKED:
		return "PARKED";
	default:
		return "none";
	}
}

/*
 * The panic the dead kernel entered, as the lines the report keeps: the text it formatted at the
 * panic's first instruction and the pc it is blamed on; the return addresses above that (the frame
 * chain, as far as they fit on a line); and the last step of the way out it reached, on which
 * processor and how long before the newest record of any kind. Nothing is printed for a table
 * without a panic in it -- a version 5 kernel's report is what it always was.
 *
 * Printed at the top of the report and again with the last lines, because the screen keeps its last
 * screenful: whichever end of the report is read, the panic's text is there
 * (work/v29i/INVESTIGATION-panic-xcall-hang.md §5.1).
 */
static void
hung_log_panic(const struct lx_mmio_ring *ring, const struct lx_cpu_breadcrumbs *table)
{
	const struct lx_panic_record *panic = hung_cpu_pc_panic(table);
	char msg[HUNG_PANIC_MSG_COLS + 1];
	uint64_t newest;
	unsigned int n = 0, bt = 0;

	if (panic == NULL || panic->magic != LX_PANIC_MAGIC) {
		return;
	}
	/* (what fits before "caller", and anything that is not printable -- a newline in the panic's
	 * own text -- shown as a dot) */
	for (unsigned int i = 0; i < HUNG_PANIC_MSG_COLS && i < panic->msg_len && i < LX_PANIC_MSG; i++) {
		const char c = panic->msg[i];

		msg[n++] = (c >= ' ' && c <= '~') ? c : '.';
	}
	msg[n] = '\0';
	print("PANIC: %s caller %x\n", msg, panic->caller);
	for (unsigned int i = 0; i < HUNG_PANIC_BT_COLS && i < LX_PANIC_FPS && panic->fps[i] != 0; i++) {
		print(bt == 0 ? "PANIC BT: %x" : " %x", panic->fps[i]);
		bt++;
	}
	if (bt != 0) {
		print("\n");
	}
	/*
	 * (the age is against the newest record the report can see on the same clock -- the register
	 * accesses, the processors' breadcrumbs, the debugger's and the panic's own stage, which is the
	 * newest of them when the machine stopped inside it)
	 */
	newest = panic->stage_ns;
	if (ring != NULL) {
		for (uint32_t i = 0; i < LX_MMIO_ACCESSES; i++) {
			if (ring->access[i].seq != 0 && ring->access[i].when_ns > newest) {
				newest = ring->access[i].when_ns;
			}
		}
	}
	for (uint32_t i = 0; i < table->cores; i++) {
		const struct lx_cpu_breadcrumb *entry = (const struct lx_cpu_breadcrumb *)(const void *)
		    ((const char *)table + LX_CPU_PC_CORE_OFFSET + (uint64_t)i * table->size);

		if (entry->when_ns > newest) {
			newest = entry->when_ns;
		}
	}
	if (table->version >= LX_CPU_PC_VERSION_DEBUGGER) {
		const struct lx_dbg_globals *globals = (const struct lx_dbg_globals *)(const void *)
		    ((const char *)table + LX_CPU_PC_CORE_OFFSET + (uint64_t)LX_CPU_PC_CORES * table->size +
		    LX_CPU_PC_DEBUG_OFFSET + LX_CPU_PC_CORES * sizeof(struct lx_cpu_debugger));

		if (globals->last_when_ns > newest) {
			newest = globals->last_when_ns;
		}
	}
	print("LAST: panic stage %d (%s) c%x t %dms\n", (uint64_t)panic->stage, hung_panic_stage(panic->stage),
	    (uint64_t)panic->stage_cpu,
	    panic->stage_ns != 0 && newest >= panic->stage_ns ? (newest - panic->stage_ns) / 1000000 : 0);
}

/*
 * The summary the last photograph is sure to hold. The console shows its last screenful -- about 24
 * rows of 80 columns on this phone, where the v29z report (some 60 lines) left only the tail, so
 * even "STUCK", printed first of all, was scrolled away -- and the screen copy that follows the
 * reset line takes that same tail. These lines are printed after everything else and just before
 * the reset line, so they are the last of the report: the access that never came back (with
 * the time it began, the caller and its number in the ring), the ring's own counters (a "lost"
 * above zero says an access had already been long unfinished when the machine stopped), how
 * far each processor's last pmap teardown got with the ASID and the process it belonged to, and
 * (version 5) whether the debugger was entered, on which processor, from which hook and from which
 * pc, and who is still waiting in it. Pure printing of what the report above has already read: no
 * device register, nothing new from the dead kernel (work/v29i/INVESTIGATION-hotspot-hang-v29z.md
 * §4.2, 修 2 and A1/A2; ...-hang-v30.md §4.2 for the debugger's two lines).
 */
static void
log_of_hung_last(const struct lx_mmio_ring *ring, uint64_t core)
{
	const struct lx_cpu_breadcrumbs *table = hung_cpu_table;

	/* (version 6: the panic's three lines again, first of these, so a photograph of the tail has them) */
	hung_log_panic(ring, table);
	if (ring != NULL) {
		const struct lx_mmio_access *stuck = hung_mmio_pending(ring);

		print("LAST: ring seq %x lost %x; hung core %x\n", (uint64_t)ring->seq, (uint64_t)ring->lost, core);
		if (stuck != NULL) {
			/* (no "val": a pending entry's value is only written when it comes back, so it is 0
			 * here always; the line is kept under the console's 80 columns so it costs one row) */
			print("LAST: STUCK %s#%x off %x win %d cpu %x t %x from %x\n", hung_mmio_what(stuck),
			    stuck->seq, (uint64_t)stuck->offset, (uint64_t)stuck->window,
			    (uint64_t)stuck->cpu, stuck->when_ns, stuck->caller);
		} else {
			print("LAST: no register access is left pending\n");
		}
	} else {
		print("LAST: no record of the radio's accesses; hung core %x\n", core);
	}
	if (table == NULL) {
		print("LAST: no record of where the processors were\n");
		return;
	}
	if (table->version < LX_CPU_PC_VERSION_TEARDOWN) {
		print("LAST: this kernel recorded no pmap teardowns\n");
		return;
	}
	print("LAST: pmap");
	for (uint32_t i = 0; i < table->cores; i++) {
		const struct lx_cpu_breadcrumb *entry = (const struct lx_cpu_breadcrumb *)(const void *)
		    ((const char *)table + LX_CPU_PC_CORE_OFFSET + (uint64_t)i * table->size);
		const struct lx_cpu_pmap_record *record = (const struct lx_cpu_pmap_record *)(const void *)
		    ((const char *)entry + sizeof(struct lx_cpu_breadcrumb));

		if (record->phase == LX_PMAP_PHASE_NONE) {
			print(" %d:-", (uint64_t)i);
		} else {
			print(" %d:%d/%x/%x", (uint64_t)i, (uint64_t)record->phase,
			    (uint64_t)(record->ids & 0xffffu), (uint64_t)(record->ids >> 16));
		}
	}
	print("\n");
	if (table->version < LX_CPU_PC_VERSION_DEBUGGER) {
		print("LAST: dbg none (a version %x table has no debugger record)\n", (uint64_t)table->version);
	} else {
		/*
		 * Whether the kernel's debugger was entered at all, where and from which hook: the two
		 * lines that tell a machine parked in the debugger's SIGPdebug cross-call -- every
		 * processor on SGI 0, no panic, no line, no spin -- from hardware that stopped, and
		 * "rx <mask>" names the processors still waiting in one (work/v29i/
		 * INVESTIGATION-hotspot-hang-v30.md §4.1). The first line is the entry that began it;
		 * the second is the newest, and the two are the same line when there was only one.
		 * Two rows of the panel's 80 columns, and the only rows this record costs.
		 */
		const char *base = (const char *)table + LX_CPU_PC_CORE_OFFSET + (uint64_t)LX_CPU_PC_CORES * table->size;
		const struct lx_cpu_debugger *records = (const struct lx_cpu_debugger *)(const void *)
		    (base + LX_CPU_PC_DEBUG_OFFSET);
		const struct lx_dbg_globals *globals = (const struct lx_dbg_globals *)(const void *)
		    (base + LX_CPU_PC_DEBUG_OFFSET + LX_CPU_PC_CORES * sizeof(struct lx_cpu_debugger));
		uint64_t rx = 0;

		for (uint32_t i = 0; i < LX_CPU_PC_CORES; i++) {
			if (records[i].roles & LX_DBG_ROLE_RECEIVER) {
				rx |= 1u << i;
			}
		}
		if (globals->count == 0) {
			print("LAST: dbg none (no processor entered the debugger since boot)\n");
		} else {
			print("LAST: dbg %x first c%x %s e%x/%x pc %x t %dms\n", globals->count,
			    globals->first_cpu, hung_dbg_kind(globals->first_kind),
			    (uint64_t)(uint32_t)globals->first_exc_code, globals->first_exc_code >> 32, globals->first_elr,
			    hung_mmio_newest_ns >= globals->first_when_ns ?
			    (hung_mmio_newest_ns - globals->first_when_ns) / 1000000 : 0);
			print("LAST: dbg last c%x %s e%x/%x pc %x t %dms rx %x\n", globals->last_cpu,
			    hung_dbg_kind(globals->last_kind),
			    (uint64_t)(uint32_t)globals->last_exc_code, globals->last_exc_code >> 32, globals->last_elr,
			    hung_mmio_newest_ns >= globals->last_when_ns ?
			    (hung_mmio_newest_ns - globals->last_when_ns) / 1000000 : 0, rx);
		}
	}
}

/* How long the screen keeps the report of a dead kernel before the machine is reset: long enough
 * for a person to photograph it, and no longer than the firmware watchdog's 30 s. */
#define HUNG_SCREEN_SECONDS     30

static void hung_screen_copy_report(void);      /* (below: it needs SCREEN) */
static void hung_screen_begin_report(void);     /* (below: it needs SCREEN) */

/*
 * What a kernel that stopped answering leaves: its log, the radio's last accesses, and the screen
 * for a while. Said here, and not where run_kernel() handles OTHER_CPU_HUNG, because a run that
 * was started over USB does not get back there: run_on_other_cpu() resets the machine itself
 * (usb_kernel_reset()), so anything that is to be seen has to be said before that point.
 */
/* ------------------------------------------------------------------------ *
 * hk: after a hang there is no partition to write the log to and the USB is
 * the kernel's: the screen is what is left.  Print the lines of the kernel's
 * message buffer that say what the user space did, newest last.
 */
static void
hk_key_lines(const struct kernel_live *live, uint64_t slide)
{
	static const char *const keys[] = {
		"SpringBoard", "backboardd", "runningboardd", "BoardServices", "xpcproxy",
		"persona", "exited due to", "failed to", "SIGABRT", "SIGILL", "SIGSEGV",
		"Unable to find", "service has crashed", "failed activation",
	};
	static uint32_t start[48], size[48];
	const char *log = (const char *)(live->log + slide);
	uint32_t len, i = 0, n = 0;

	flush_dcache((uint64_t)(uintptr_t)live->log_len + slide, (uint64_t)(uintptr_t)live->log_len + slide + 4);
	len = *(volatile uint32_t *)(live->log_len + slide);
	if (len > live->log_head + live->log_ring) {
		return;
	}
	flush_dcache((uint64_t)(uintptr_t)log, (uint64_t)(uintptr_t)log + len);
	while (i < len) {
		uint32_t j = i, t, q;
		int hit = 0;

		while (j < len && log[j] != '\n') {
			j++;
		}
		for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]) && !hit; k++) {
			for (t = i; t + 1 < j && !hit; t++) {
				for (q = 0; keys[k][q] != 0 && t + q < j && log[t + q] == keys[k][q]; q++) {
				}
				if (keys[k][q] == 0) {
					hit = 1;
				}
			}
		}
		if (hit) {
			start[n % 48] = i;
			size[n % 48] = j - i;
			n++;
		}
		i = j + 1;
	}
	print("\n== KEY LINES OF THE KERNEL LOG (last %u of %u) ==\n", n > 30 ? (uint64_t)30 : (uint64_t)n, (uint64_t)n);
	for (uint32_t x = (n > 30 ? n - 30 : 0); x < n; x++) {
		uint32_t id = x % 48;
		for (uint32_t t = 0; t < size[id] && t < 150; t++) {
			char c = log[start[id] + t];
			put_char(c >= 0x20 && c < 0x7f ? c : '.');
		}
		put_char('\n');
	}
	print("== END OF KEY LINES ==\n");
}


static void
report_hung_kernel(const struct kernel_live *live, uint64_t slide, uint64_t core)
{
	int was_quiet = screen_quiet;
	uint64_t began, last_pet;
	const struct lx_mmio_ring *mmio_ring = NULL;

	/*
	 * The USB command loop left the screen to the kernel, and the kernel is dead. In the v29f and
	 * v29g deaths the host's bulk reads stopped with the kernel, so the screen is the one place
	 * this can be read: say it there (and in the loader's log, for a host that can still reach it).
	 */
	screen_quiet = 0;
	screen_forced = 1;
	hung_cpu_opened = 0;            /* (this report opens the table of breadcrumbs exactly once) */
	/*
	 * The console's own frame is where the report is drawn, and the kernel may have been drawing into
	 * that same frame (fb_dpu=3/4 lays the carve-out out as its three frames, the first of them the GOP
	 * frame): its rows hold the picture the kernel died on, at the kernel's row stride. Clear it before
	 * the first line, so what the panel is given below is the report on black and nothing of that
	 * picture reads as part of it.
	 */
	hung_screen_begin_report();
	print("THE KERNEL HUNG on core %x: no sign of life for %d seconds; its log follows\n", core,
	    hung_silence_seconds);
	/*
	 * The access that never came back, first of all: it is one line, and the ring's own sixteen below are
	 * the first the screen loses when something later scrolls it (three reports in a row lost its offset,
	 * window and cpu that way; work/v29i/INVESTIGATION-wifi-srng-hang.md §0.11 and §6.1). Read from the
	 * dead kernel's own memory, like everything below: no device register.
	 */
	if (live != NULL) {
		mmio_ring = hung_mmio_ring(live, slide);
	}
	/*
	 * UMI (version 6): the panic the dead kernel entered, first of all after the title (the ring
	 * above is only read, not printed). It is the one thing a photograph of this screen has to
	 * have -- the text the kernel formatted at the panic's first instruction, before anything in
	 * that path could wait -- and it is said here because the report scrolls its own top away.
	 * The table (and the ring) are opened once; log_of_hung_cpu_pc() reuses it.
	 */
	if (live != NULL) {
		hung_log_panic(mmio_ring, hung_cpu_pc_open(live, slide));
	}
	if (mmio_ring != NULL) {
		const struct lx_mmio_access *stuck = hung_mmio_pending(mmio_ring);

		if (stuck != NULL) {
			/* (one line that answers "which access, when, from where" without the ring's sixteen lines
			 * below it: the v29z report lost its offset and cpu to the screen's scroll, and even when the
			 * line survived it had no time and no caller -- work/v29i/INVESTIGATION-hotspot-hang-v29z.md
			 * §4.2, 修 1. "t" is when the access began, in the clock the breadcrumbs use, in hex.) */
			print("STUCK: %s off %x win %d val %x cpu %x t %x from %x seq %x\n", hung_mmio_what(stuck),
			    (uint64_t)stuck->offset, (uint64_t)stuck->window, stuck->value, (uint64_t)stuck->cpu,
			    stuck->when_ns, stuck->caller, stuck->seq);
		} else {
			print("STUCK: no register access is left pending\n");
		}
	} else {
		print("STUCK: no record of the radio's accesses\n");
	}
	/*
	 * The report reaches the panel first. Everything below draws into the firmware's frame, which a
	 * kernel with fb_dpu scans only after the copy at the end (and the loader's log lives in RAM,
	 * which the reset erases), so the lines that say what happened are copied before anything else
	 * is done. Each later stage copies again: whatever stage stops answering, the screen already
	 * holds the stages before it.
	 */
	hung_screen_copy_report();
	if (live != NULL) {
		/* (the ring and the CPU record are read from the dead kernel's own memory: no device register) */
		log_of_hung_mmio(mmio_ring);
		/*
		 * Where each processor was, after the radio's accesses: the two records share a clock, so
		 * this one is aged against the newest of them (and says how long before it each processor
		 * last interrupted -- or that it was asleep, or waiting for a lock).
		 */
		log_of_hung_cpu_pc(live, slide);
	} else {
		print("MMIO: this kernel has no live block, so no record of the radio's accesses\n");
	}
	hung_screen_copy_report();
#if HUNG_MMIO_DUMP
	/*
	 * ... and the live state of the interrupt controller, the SMMU and the PCIe links, read-only:
	 * the lines above say where the processors were, these say what was still holding an interrupt
	 * (or a power domain, or a link) at that moment. These are the report's only device-register
	 * reads, so they come after the screen holds the lines above, and only with "hung_e2=1" (see
	 * hung_e2_requested): a read of a block whose clock died with the kernel hangs the bus, and the
	 * umi build has no watchdog to recover from that.
	 */
	if (hung_e2_requested) {
		print("MMIO state dump: GIC, then PCIe, then SMMU (hung_e2=1)\n");
		hung_screen_copy_report();
		log_of_hung_mmio_state();
		hung_screen_copy_report();
	}
#endif
	/*
	 * The kernel's log goes into the loader's copy of it (what a host fetches; on this phone there
	 * is no partition to write it to) but not to the screen: it would push the lines above away.
	 */
	screen_quiet = 1;
	screen_forced = 0;
	if (live != NULL) {
		hk_key_lines(live, slide);      /* hk: the screen is the only way out */
		write_logs((const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
	} else {
		write_logs(NULL, 0);
	}
	screen_quiet = was_quiet;
	screen_forced = 1;
	/*
	 * The last lines before the reset line, and so the ones the final screen copy keeps even when
	 * everything above has scrolled: the stuck access, the ring's counters and the pmap phases.
	 */
	log_of_hung_last(mmio_ring, core);
	print("\n(the machine resets in %d seconds: photograph this screen)\n", (uint64_t)HUNG_SCREEN_SECONDS);
	/*
	 * The console drew into the firmware's frame, and a kernel with fb_dpu scans one of the carve-out's
	 * other frames: put the report in the frame the panel actually fetches, then hold. Whatever comes of
	 * this (it gives up and says why in the loader's log if the display is not reachable), the hold and
	 * the reset below still happen.
	 */
	hung_screen_copy_report();
	/*
	 * Keep the firmware watchdog fed (the loader arms 30 s before the kernel runs): this is the one
	 * pause in the loader that is meant to outlast it.
	 */
	began = last_pet = READ_SYSREG(CNTVCT_EL0);
	while (seconds_since(began) < HUNG_SCREEN_SECONDS) {
		if (READ_SYSREG(CNTVCT_EL0) - last_pet >= READ_SYSREG(CNTFRQ_EL0)) {
			last_pet = READ_SYSREG(CNTVCT_EL0);
			wdt_pet();
		}
	}
	/*
	 * The report is what stays on the screen: the caller's next words -- hc_take_back()'s dozen UFS state
	 * lines on the path that is not a USB run -- go to the loader's log instead of over it (A-4).
	 */
	screen_forced = 0;
}

/*
 * The log of a kernel that is alive and is merely being asked how it is doing ("oem log" during
 * a run): a copy in the right order after the loader's own log, with nothing moved under the
 * kernel's feet.
 */
static void
compose_live_log(const struct kernel_live *live, uint64_t slide)
{
	static const char sep[] = "\n== KERNEL LOG (the kernel is still running) ==\n";
	const char *log = (const char *)(live->log + slide);
	uint32_t len = *(volatile uint32_t *)(live->log_len + slide);
	uint32_t ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	int wrapped = *(volatile uint8_t *)(live->log_wrapped + slide);
	uint64_t n, room = sizeof(out_buf) - 8192;

	compose_logs(NULL, 0);
	n = staged_len;
#define PUT(from, size) do { \
	uint64_t take = (size); \
	if (n + take > room) { \
	        take = room - n; \
	} \
	memcpy(out_buf + n, (from), take); \
	n += take; \
} while (0)
	PUT(sep, sizeof(sep) - 1);
	if (!wrapped || ring_at >= live->log_ring) {
		PUT(log, len <= live->log_head + live->log_ring ? len : live->log_head + live->log_ring);
	} else {
		PUT(log, live->log_head);
		PUT(log + live->log_head + ring_at, live->log_ring - ring_at);
		PUT(log + live->log_head, ring_at);
	}
#undef PUT
	staged_len = n;
}

static void print(const char *fmt, ...);
static void flush_log(void);
static void put_char(char c);

/* A voltage-only fallback is deliberately conservative; the real QG SOC wins whenever available. */
static int
soc_from_voltage(uint32_t mv)
{
	if (mv <= 3300) return 0;
	if (mv < 3700) return (int)((mv - 3300) * 10 / 400);
	if (mv < 3900) return 10 + (int)((mv - 3700) * 40 / 200);
	if (mv < 4200) return 50 + (int)((mv - 3900) * 40 / 300);
	if (mv < 4400) return 90 + (int)((mv - 4200) * 10 / 200);
	return 100;
}

/* Read-only status query.  No TakeAction call and no charger-limit writes are made here. */
static void
charger_poll(void)
{
	struct qcom_battery_status status;
	uint32_t action = 0xffffffffu, flags = 0, voltage = 0;
	uint8_t present = 0, plugged = 0;
	efi_status s;
	int have_soc = 0, have_voltage = 0;

	if (!charger_enabled) {
		return;
	}
	memset(&status, 0, sizeof(status));

	charger_shared.sequence++;
	__asm__ volatile ("dmb ish" ::: "memory");
	if (CHARGER_EX != NULL) {
		s = CHARGER_EX->get_battery_presence(&present);
		if (!EFI_ERROR(s) && present) flags |= LOADER_BATTERY_PRESENT;
		s = CHARGER_EX->get_charger_presence(&plugged);
		if (!EFI_ERROR(s) && plugged) flags |= LOADER_BATTERY_CHARGER_PRESENT;
		s = CHARGER_EX->get_battery_voltage(&voltage);
		if (!EFI_ERROR(s) && voltage >= 2500 && voltage <= 5000) {
			have_voltage = 1;
			flags |= LOADER_BATTERY_VOLTAGE_VALID;
			charger_shared.voltage_mv = (int32_t)voltage;
		}
	}
	if (QCOM_CHARGER != NULL) {
		s = QCOM_CHARGER->get_charging_action(&action, &status, 0);
		if (!EFI_ERROR(s)) {
			charger_shared.charger_action = action;
			if (status.battery_voltage_mv >= 2500 && status.battery_voltage_mv <= 5000) {
				charger_shared.voltage_mv = status.battery_voltage_mv;
				have_voltage = 1;
				flags |= LOADER_BATTERY_VOLTAGE_VALID;
			}
			/*
			 * GetChargingAction returns the firmware's state of charge in word [2] of its
			 * output (confirmed by dumping the raw buffer on this device). We use it as-is
			 * EXCEPT when the fuel gauge is clearly not ready yet: right after initialize()
			 * it can report a near-empty SOC while the cell voltage says otherwise, which is
			 * where the old "shows 1% on a half-full battery" came from. A Li-ion cell above
			 * ~3.6 V is never under 5%, so in that case we distrust the gauge and fall back to
			 * the voltage estimate below until it settles.
			 */
			if (status.state_of_charge >= 0 && status.state_of_charge <= 100) {
				int vmv = have_voltage ? charger_shared.voltage_mv : 0;
				if (status.state_of_charge < 5 && vmv > 3600) {
					/* fuel gauge not settled; leave it to the voltage fallback */
				} else {
					charger_shared.soc_percent = status.state_of_charge;
					have_soc = 1;
					flags |= LOADER_BATTERY_VALID;
				}
			}
			charger_shared.current_ma = status.charge_current_ma;
			charger_shared.temperature_c = status.battery_temperature_c;
		}
	}
	if (!have_soc && have_voltage) {
		charger_shared.soc_percent = soc_from_voltage((uint32_t)charger_shared.voltage_mv);
		flags |= LOADER_BATTERY_VALID | LOADER_BATTERY_SOC_ESTIMATED;
	}
	if (plugged && charger_shared.soc_percent < 100) {
		flags |= LOADER_BATTERY_CHARGING;
	}
	charger_shared.flags = flags;
	charger_shared.polls++;
	if (!(flags & LOADER_BATTERY_VALID)) charger_shared.failures++;
	__asm__ volatile ("dmb ish" ::: "memory");
	charger_shared.sequence++;
	clean_dcache((uint64_t)(uintptr_t)&charger_shared,
	    (uint64_t)(uintptr_t)&charger_shared + sizeof(charger_shared));
}

/* ------------------------------------------------------------------------ */
/* UFS survey (ufs=1): what the phone's storage holds, read only            */

/*
 * The phone's storage as Qualcomm's own UFS driver in this UEFI sees it (UFSDxe): every raw
 * logical unit, every GPT partition with its name, place and size, and for the partitions there
 * are reference images of -- firmware-update/ in the ROM's fw.zip -- an FNV-1a hash of exactly
 * as many bytes as the image has, which tools/ufs-survey.py computes from the images on the host.
 * Where the two agree, reading through this driver gives the bytes that are on the flash, and that
 * is the ground a block device for the kernel is to stand on.
 *
 * Only read_blocks is ever called here: nothing is written, flushed or reset. It runs before the
 * kernel is started, with UEFI's boot services whole and interrupts on.
 */
static const struct {
	const char *name;
	uint64_t bytes;
} ufs_references[] = {
	{ "bluetooth", 503808 }, { "abl", 208896 }, { "aop", 208896 }, { "xbl", 3706880 },
	{ "tz", 3710976 }, { "hyp", 3653632 }, { "dsp", 67108864 }, { "modem", 214265856 },
};

static uint8_t ufs_buffer[1024 * 1024] __attribute__((aligned(4096)));

static uint64_t
ufs_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

/* the partition's name in ASCII (GPT names are UTF-16; these are all plain letters) */
static void
ufs_name(const efi_partition_info *info, char *out, unsigned size)
{
	unsigned i = 0;

	for (; i + 1 < size && i < 36 && info->name[i] != 0; i++) {
		out[i] = info->name[i] < 0x80 ? (char)info->name[i] : '?';
	}
	out[i] = 0;
}

/* "bluetooth_a" and "bluetooth_b" are both "bluetooth": the image is the same for either slot */
static int
ufs_reference_of(const char *name)
{
	for (unsigned r = 0; r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
		const char *a = ufs_references[r].name, *b = name;

		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a == 0 && (*b == 0 || ((b[0] == '_') && (b[1] == 'a' || b[1] == 'b') && b[2] == 0))) {
			return (int)r;
		}
	}
	return -1;
}

static void
ufs_hash_partition(efi_block_io *bio, const char *name, uint64_t bytes)
{
	uint64_t bs = bio->media->block_size, h = 0xcbf29ce484222325ULL, done = 0, started = READ_SYSREG(CNTVCT_EL0);
	uint64_t chunk = sizeof(ufs_buffer) / bs * bs, lba = 0;

	if (bs == 0 || bs > sizeof(ufs_buffer) || (bio->media->last_block + 1) * bs < bytes) {
		print("ufs: %s: %d bytes asked for, the partition is smaller\n", name, bytes);
		return;
	}
	while (done < bytes) {
		uint64_t want = bytes - done < chunk ? (bytes - done + bs - 1) / bs * bs : chunk;
		efi_status s = bio->read_blocks(bio, bio->media->media_id, lba, want, ufs_buffer);

		if (EFI_ERROR(s)) {
			print("ufs: %s: reading at block %d: error %x\n", name, lba, (uint64_t)s);
			return;
		}
		if (done == 0) {
			print("ufs: %s: begins", name);
			for (int k = 0; k < 16; k++) {
				print(" %x", (uint64_t)ufs_buffer[k]);
			}
			print("\n");
		}
		h = ufs_fnv(h, ufs_buffer, bytes - done < want ? bytes - done : want);
		done += bytes - done < want ? bytes - done : want;
		lba += want / bs;
		if (wdt_armed) {
			wdt_pet();
		}
	}
	print("ufs: %s: fnv %x over %d bytes, %d ms\n", name, h, bytes,
	    (READ_SYSREG(CNTVCT_EL0) - started) * 1000 / READ_SYSREG(CNTFRQ_EL0));
}

/* CRC-32 as GPT uses it (IEEE 802.3, reflected, 0xedb88320) */
static uint32_t
ufs_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;

	for (uint64_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

/*
 * A unit's GPT checks itself: its header carries a CRC-32 of the header and one of the table of
 * partitions. Whatever firmware is on the phone, the bytes read are the bytes on the flash only if
 * both come out right -- the one check here that does not depend on having the same images.
 */
static void
ufs_check_gpt(efi_block_io *bio, uint64_t unit)
{
	uint64_t bs = bio->media->block_size, entries_lba, count, size, bytes, read;
	uint32_t header_size, header_crc, entries_crc, got_header, got_entries;
	uint8_t header[512];

	if (bs < 512 || bs > sizeof(ufs_buffer) ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, ufs_buffer))) {
		print("ufs: unit %d: the GPT header could not be read\n", unit);
		return;
	}
	if (ufs_buffer[0] != 'E' || ufs_buffer[1] != 'F' || ufs_buffer[2] != 'I' || ufs_buffer[3] != ' ' ||
	    ufs_buffer[4] != 'P' || ufs_buffer[5] != 'A' || ufs_buffer[6] != 'R' || ufs_buffer[7] != 'T') {
		print("ufs: unit %d: no GPT header in block 1\n", unit);
		return;
	}
	memcpy(header, ufs_buffer, sizeof(header));
	header_size = *(uint32_t *)(header + 12);
	header_crc = *(uint32_t *)(header + 16);
	entries_lba = *(uint64_t *)(header + 72);
	count = *(uint32_t *)(header + 80);
	size = *(uint32_t *)(header + 84);
	entries_crc = *(uint32_t *)(header + 88);
	if (header_size < 92 || header_size > sizeof(header)) {
		print("ufs: unit %d: GPT header size %d\n", unit, (uint64_t)header_size);
		return;
	}
	*(uint32_t *)(header + 16) = 0;
	got_header = ufs_crc32(header, header_size);
	bytes = count * size;
	if (bytes == 0 || bytes > sizeof(ufs_buffer)) {
		print("ufs: unit %d: GPT header crc %s, a table of %d bytes not checked\n", unit,
		    got_header == header_crc ? "right" : "WRONG", bytes);
		return;
	}
	read = (bytes + bs - 1) / bs * bs;
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, entries_lba, read, ufs_buffer))) {
		print("ufs: unit %d: the GPT table could not be read\n", unit);
		return;
	}
	got_entries = ufs_crc32(ufs_buffer, bytes);
	print("ufs: unit %d: GPT header crc %s (%x), table of %d entries crc %s (%x)\n", unit,
	    got_header == header_crc ? "right" : "WRONG", (uint64_t)got_header, count,
	    got_entries == entries_crc ? "right" : "WRONG", (uint64_t)got_entries);
}

/* one line per 32 bytes: "ufs: PART WHAT +OFFSET: hex" */
static void
ufs_dump_of(const char *part, const char *what, const uint8_t *p, uint64_t n)
{
	for (uint64_t at = 0; at < n; at += 32) {
		char line[72];
		unsigned k = 0;

		for (uint64_t i = at; i < n && i < at + 32; i++) {
			line[k++] = "0123456789abcdef"[p[i] >> 4];
			line[k++] = "0123456789abcdef"[p[i] & 15];
		}
		line[k] = 0;
		print("ufs: %s %s +%x: %s\n", part, what, at, line);
	}
}

static void
ufs_dump(const char *what, const uint8_t *p, uint64_t n)
{
	ufs_dump_of("super", what, p, n);
}

/*
 * The start of userdata: where an f2fs (or ext4) of Android's would have its superblock, at 1024
 * and, for f2fs, its copy at 4096 + 1024. Plain text there says the filesystem itself lies on the
 * flash as it is; anything else says it lies under an encryption of the whole partition
 * (Android's metadata encryption, dm-default-key), and only Android can see into it.
 */
static void
ufs_dump_userdata(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 8192;

	if (bs == 0 || want % bs || EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: userdata: its start could not be read\n");
		return;
	}
	ufs_dump_of("userdata", "sb", ufs_buffer + 1024, 1024);
	ufs_dump_of("userdata", "sb2", ufs_buffer + 4096 + 1024, 64);
}

/*
 * Android's own system (system, vendor, product, ...) is not in GPT partitions but in logical ones
 * inside "super", which liblp's metadata at its start describes: a geometry at 4096 (and its copy
 * at 8192), then its metadata slots from 12288 on -- each a header and its tables of partitions,
 * extents, groups and block devices. Printed raw for tools/ufs-super.py, which checks their SHA-256 sums and
 * says where in super nothing lies: a place a volume of this port's own could have without
 * anything of Android's being moved.
 */
static void
ufs_dump_super(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 256 * 1024;
	const uint8_t *geometry = ufs_buffer + 4096;
	uint32_t max_size, slots;

	if (bs == 0 || want % bs || (bio->media->last_block + 1) * bs < want ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: super: its start could not be read\n");
		return;
	}
	if (*(const uint32_t *)geometry != 0x616c4467) {
		print("ufs: super: no liblp geometry (magic %x)\n", (uint64_t)*(const uint32_t *)geometry);
		return;
	}
	ufs_dump("geometry", geometry, 52);
	max_size = *(const uint32_t *)(geometry + 40);
	slots = *(const uint32_t *)(geometry + 44);
	/* a slot per A/B slot: the first describes super as slot _a sees it, the second as _b does */
	for (uint32_t slot = 0; slot < slots && slot < 2; slot++) {
		const uint8_t *header = ufs_buffer + 3 * 4096 + (uint64_t)slot * max_size;
		uint32_t header_size, tables_size;
		char what[8] = "header0", tables[8] = "tables0";

		if (3 * 4096 + (uint64_t)(slot + 1) * max_size > want || *(const uint32_t *)header != 0x414c5030) {
			print("ufs: super: slot %d: no metadata header within the first %d KB\n", (uint64_t)slot, want >> 10);
			continue;
		}
		header_size = *(const uint32_t *)(header + 8);
		tables_size = *(const uint32_t *)(header + 44);
		if (header_size < 128 || header_size > 256 || tables_size > max_size - header_size || tables_size > 16384) {
			print("ufs: super: slot %d: header of %d bytes, tables of %d: not dumped\n", (uint64_t)slot,
			    (uint64_t)header_size, (uint64_t)tables_size);
			continue;
		}
		what[6] = tables[6] = (char)('0' + slot);
		ufs_dump(what, header, header_size);
		ufs_dump(tables, header + header_size, tables_size);
	}
}

/* GCC is accessible even when the display bus is stopped. Do not extend
 * this snapshot to DISPCC/MDSS/DSI without first checking their bus clocks.
 * The UMI ClockDxe domain shared by display and UFS AHB is at GCC + 0x5050.
 */
static void
umi_display_probe_dump(void)
{
	static const efi_guid guid = UMI_DISPLAY_PROBE_GUID;
	static const char *const stages[] = UMI_DISPLAY_PROBE_STAGES;
	if (PLAT != &platform_umi) return;
	const UMI_DISPLAY_PROBE_TABLE *table = NULL;
	for (uint64_t i = 0; i < ST->nr_tables; ++i) {
		if (!memcmp(&ST->tables[i].guid, &guid, sizeof(guid))) {
			table = ST->tables[i].table;
			break;
		}
	}
	if (!table) {
		print("uefi-probe: absent\n");
		return;
	}
	if (table->Magic != UMI_DISPLAY_PROBE_MAGIC || table->Version != UMI_DISPLAY_PROBE_VERSION ||
	    table->Count > UMI_DISPLAY_PROBE_CAPACITY) {
		print("uefi-probe: invalid table\n");
		return;
	}
	print("uefi-probe: version=%d count=%d event-errors=%x vote-status=%x\n",
	    (uint64_t)table->Version, (uint64_t)table->Count, (uint64_t)table->EventErrors,
	    (uint64_t)table->VoteStatus);
	for (unsigned i = 0; i < table->Count; ++i) {
		const UMI_DISPLAY_PROBE_RECORD *r = &table->Records[i];
		const char *stage = r->Stage < sizeof(stages) / sizeof(stages[0]) ? stages[r->Stage] : "unknown";
		print("uefi-probe: %s: parent-cmd=%x parent-cfg=%x disp-ahb=%x ufs-ahb=%x disp-hf=%x disp-sf=%x disp-xo=%x mmss-bcr=%x ufs-bcr=%x\n",
		    stage, (uint64_t)r->Gcc[0], (uint64_t)r->Gcc[1], (uint64_t)r->Gcc[2],
		    (uint64_t)r->Gcc[3], (uint64_t)r->Gcc[4], (uint64_t)r->Gcc[5],
		    (uint64_t)r->Gcc[6], (uint64_t)r->Gcc[7], (uint64_t)r->Gcc[8]);
	}
}

static void
umi_display_clock_snapshot(const char *stage)
{
	if (PLAT != &platform_umi) return;
	volatile uint32_t *gcc = (volatile uint32_t *)(uintptr_t)0x00100000;
	print("display-clock: %s: parent-cmd=%x parent-cfg=%x disp-ahb=%x ufs-ahb=%x disp-hf=%x disp-sf=%x\n",
	    stage, (uint64_t)gcc[0x5050 / 4], (uint64_t)gcc[0x5054 / 4],
	    (uint64_t)gcc[0xb00c / 4], (uint64_t)gcc[0x77018 / 4],
	    (uint64_t)gcc[0xb034 / 4], (uint64_t)gcc[0xb038 / 4]);
}

static void
ufs_survey(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count, units = 0, partitions = 0;

	/* the raw logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count; i++) {
			efi_block_io *bio;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical) {
				continue;
			}
			print("ufs: unit %d: media %d, %d blocks of %d (%d MB), %s, align %d\n", units++,
			    (uint64_t)bio->media->media_id, bio->media->last_block + 1, (uint64_t)bio->media->block_size,
			    (bio->media->last_block + 1) * bio->media->block_size >> 20,
			    bio->media->read_only ? "read only" : "writable", (uint64_t)bio->media->io_align);
			ufs_check_gpt(bio, units - 1);
		}
		/* (the handle array is pool memory this function owns) */
		BS->free_pool(handles);
	}
	/* the GPT partitions, with the hash of those there are images of */
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information in this UEFI\n");
		return;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];
		int r;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		partitions++;
		print("ufs: part %s: lba %d..%d, %d KB\n", name, info->start_lba, info->end_lba,
		    (bio->media->last_block + 1) * bio->media->block_size >> 10);
		if ((r = ufs_reference_of(name)) >= 0) {
			ufs_hash_partition(bio, name, ufs_references[r].bytes);
		}
		if (name[0] == 's' && name[1] == 'u' && name[2] == 'p' && name[3] == 'e' && name[4] == 'r' && name[5] == 0) {
			ufs_dump_super(bio);
		}
		if (name[0] == 'u' && name[1] == 's' && name[2] == 'e' && name[3] == 'r' && name[4] == 'd' &&
		    name[5] == 'a' && name[6] == 't' && name[7] == 'a' && name[8] == 0) {
			ufs_dump_userdata(bio);
		}
	}
	BS->free_pool(handles);
	print("ufs: %d raw units, %d partitions\n", units, partitions);
}

/* ------------------------------------------------------------------------ */
/* UFS block service (ufs=2): the kernel reads the phone's storage through  */
/* this loader, and this loader through Qualcomm's own UFS driver           */

/*
 * The kernel has no driver for the phone's storage, and does not need one: UEFI never exits here,
 * Qualcomm's UFS driver (UFSDxe) has the controller, its PHY, clocks and power up and stays
 * resident, and this loader already serves the kernel from core 0 while the kernel runs on another
 * (the battery). So the kernel asks for blocks through a page both can see, and core 0 reads them
 * with that driver's ReadBlocks into a buffer both can see. The kernel's side is
 * drivers/storage/hakios.ufs.c; it gets the page's address as ufs_shared_pa= on its command line.
 *
 * Reads, and -- with ufs_write=1 on the command line -- writes into one partition and nowhere
 * else: the port's own darwin, proved by its GPT entry (name, both GUIDs, first and last LBA, its
 * logical unit), and in it only past everything the build writes from the computer, so that no
 * answer to the kernel can reach Android's partitions, the iOS volume or the cryptexes. The
 * refusals are here, in the kernel (drivers/storage/hakios.ufs.c) and in UEFI's own driver, which gives
 * out a partition's BlockIo and bounds it: a request has to pass all three.
 *
 * Each side writes its own cache lines only: the kernel the request, core 0 the answer (and the
 * table, once, before the kernel starts). A line one side cleans out of its cache therefore never
 * carries a stale copy of what the other side wrote into it. Into the data buffer core 0 writes
 * only what it has read back after a write of its own, so after the controller's DMA there is
 * nothing of its own in its cache to write back over the data; the kernel, which fills the buffer
 * for a write, cleans its own lines of it before it asks.
 */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_PART_UNIT    1u                          /* the entry is a whole logical unit, not a partition */
#define BLOCK_PART_VIRTUAL 2u                          /* darwin as a file of userdata (darwinfile.c): on no unit of its own */
#define BLOCK_OP_READ      0u
#define BLOCK_OP_WRITE     1u

/*
 * The one partition a write may reach, and from which of its blocks. darwin is the port's own
 * 20 GiB at the end of logical unit 0 (README, "The port's partition"); what the build puts there
 * from the computer (tools/darwin-write.py) is the iOS volume at its start and the two cryptexes
 * behind it, and nothing of this port reads or writes the rest -- so the data of iOS lives there
 * and a write is refused before that block. The identity is the whole of darwin-write.py's: the
 * same partition, named the same, of the same size, in the same place, on logical unit 0.
 */
#define BLOCK_WRITE_FIRST  2042368ULL                  /* darwin's blocks of 4096: past both cryptexes */
/*
 * Its first and last LBA of unit 0 as the GPT has them, and its unique GUID: this phone's, from its identity
 * file (phones/<serial>.darwin, made when its darwin was cut: common/flash/darwinid.py), which xnuloader/build.sh
 * turns into darwin_identity.h (DARWIN_ID=file); without one, the phone the port was brought up on.
 */
#include "darwin_identity.h"
#define BLOCK_DARWIN_TYPE  { 0x7c3457ef, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } }

/*
 * The partitions first, then the logical units they are on (BLOCK_PART_UNIT, named unit0, unit1,
 * ... in the order UEFI lists them). Each partition says which of those it is on and where it
 * begins there, so that a reader of the units themselves -- the kernel's own UFS driver -- finds
 * it; a unit says its logical unit number if its device path does.
 */
struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks (0 for a unit) */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	/* written by the loader, once, before the kernel starts */
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	/*
	 * Which partition takes writes this run, as its index among part[] plus one (0: none), and
	 * from which of its blocks. They are here, in what was padding before ask, so that the page
	 * stays the size and the version it was: one phone is shared between working copies, and a
	 * kernel built before writes existed must go on reading through a loader built after them,
	 * and the other way about -- a kernel that does not know these fields leaves ask.op zero,
	 * which is a read, and a loader that does not know them leaves these zero, which is no write.
	 */
	uint32_t write_part_plus_one;
	uint32_t write_reserved;
	uint64_t write_first;
	/* written by the kernel: what it wants, then request is advanced */
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;           /* in the partition's own blocks */
		uint32_t op;                   /* BLOCK_OP_READ, BLOCK_OP_WRITE */
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	/* written by the loader: answered becomes request when the data is in the buffer */
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;                /* the EFI status of the read, 0 when it went well */
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	/*
	 * Written by the loader before the kernel starts, with ufs_native= on the command line: where
	 * the kernel may put a transfer request list of its own into the UFS host controller for the
	 * run (hc_area, below), and UEFI's, which the loader puts back. hc_pa 0: it may not.
	 */
	struct {
		uint64_t hc_pa;                /* the controller's registers */
		uint64_t area_pa;              /* the list (1 KB, at the start) and command descriptors */
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
	} native __attribute__((aligned(128)));
	/* written by the loader during a run: it takes the controller back (a hung kernel); send nothing more */
	struct {
		volatile uint32_t take_back;
	} loader_native __attribute__((aligned(128)));
	/* written by the kernel: its list is in the controller (for the loader's log) */
	struct {
		volatile uint32_t list_in;
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;       /* UTRLCNR when the kernel's list went in */
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128,
    "the page must stay the size it was: the fields before ask have to fit in what was padding");

/* (8 KB is as far as this target aligns; the kernel maps whole 16 KB pages around both) */
static struct loader_block block_shared __attribute__((aligned(0x2000)));
static void block_io2_probe(efi_handle h, efi_block_io *bio);
static int block_probe_requested;       /* ufs_probe=1: block_io2_probe() at block_start() */
static efi_block_io *block_bios[BLOCK_MAX_PARTS];
static int block_enabled;
static int block_write_requested;       /* ufs_write=1 on the command line */
static int block_write_part = -1;       /* the only partition a write may reach; -1: none, writes refused */
static uint64_t block_writes, block_writes_refused, block_write_bytes;

/* the bytes of a device path before its end node */
static uint64_t
device_path_bytes(const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0;

	for (int nodes = 0; nodes < 64; nodes++) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		if (p[n] == 0x7f || len < 4) {
			break;
		}
		n += len;
	}
	return n;
}

/*
 * A unit's device path, a node at a time ("ufs: unit N: path TYPE.SUBTYPE[data] ..."), and its
 * logical unit number if a UFS node (messaging, subtype 0x19: target, then LUN) says it.
 */
static uint8_t
device_path_print(uint64_t unit, const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0, total = device_path_bytes(dp);
	uint8_t lun = 0xff;

	print("ufs: unit %d: path", unit);
	while (n < total) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		print(" %x.%x[", (uint64_t)p[n], (uint64_t)p[n + 1]);
		for (uint64_t i = 4; i < len && i < 4 + 24; i++) {
			print("%s%x", (uint64_t)p[n + i] < 16 ? "0" : "", (uint64_t)p[n + i]);
		}
		print("%s]", len > 4 + 24 ? "..." : "");
		if (p[n] == 3 && p[n + 1] == 0x19 && len >= 6) {
			lun = p[n + 5];
		}
		n += len;
	}
	print(lun == 0xff ? "\n" : " (LUN %d)\n", (uint64_t)lun);
	return lun;
}

/*
 * The UFS host controller, for the kernel's own driver (ufs_native= on the command line; without it
 * this loader never touches the controller's registers). The kernel may, for the length of a run,
 * stop the controller's transfer request list, point it at a list of its own in hc_area (memory of
 * this loader, so that putting UEFI's back never depends on the kernel's memory), and start it
 * again; UEFI's list is one slot and its command descriptor, with no room for another request.
 * This loader keeps UEFI's list address (UTRLBA/UTRLBAU) from before the first run and checks it,
 * and puts it back if need be, on every way back from a run (hc_take_back) before anything here
 * touches the storage; it reads for the kernel (block_poll) and writes the log (write_logs) only
 * while UEFI's list is the one in the controller.
 */
#define HC_PA              0x1d84000ULL        /* ufshc@1d84000, "ufs_mem", in this phone's Android device tree */
#define HC_CAP             0x00
#define HC_VER             0x08
#define HC_IS              0x20
#define HC_IE              0x24
#define HC_HCS             0x30
#define HC_HCE             0x34
#define HC_UTRLBA          0x50
#define HC_UTRLBAU         0x54
#define HC_UTRLDBR         0x58
#define HC_UTRLCLR         0x5c
#define HC_UTRLRSR         0x60
#define HC_UTRLCNR         0x64
#define HC_UTMRLDBR        0x78
#define HC_SLOTS           32
#define HC_UCD_OFFSET      0x1000
#define HC_UCD_SIZE        0x800
#define HC_AREA_SIZE       (HC_UCD_OFFSET + HC_SLOTS * HC_UCD_SIZE)

static uint8_t hc_area[HC_AREA_SIZE] __attribute__((aligned(0x2000)));
static int hc_requested;                /* ufs_native= on the command line */
static int hc_known;                    /* UEFI's list address is kept here: check it, put it back */
static uint32_t hc_uefi_lo, hc_uefi_hi;
static int hc_refused_reads;

static inline uint32_t
hc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(HC_PA + off);
}

static inline void
hc_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)(HC_PA + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
hc_ms_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) * 1000 / READ_SYSREG(CNTFRQ_EL0);
}

/* UEFI's list in, and running: what UEFI's driver needs to read or write */
static int
hc_list_is_uefis(void)
{
	return !hc_known || (hc_rd(HC_UTRLBA) == hc_uefi_lo && hc_rd(HC_UTRLBAU) == hc_uefi_hi &&
	       (hc_rd(HC_UTRLRSR) & 1) != 0);
}

static void
hc_print_state(const char *when)
{
	print("ufs: %s: list %x:%x (UEFI's %x:%x), run %x, doorbell %x, HCS %x, IS %x, IE %x, completion %x, "
	    "task doorbell %x\n", when, (uint64_t)hc_rd(HC_UTRLBAU), (uint64_t)hc_rd(HC_UTRLBA), (uint64_t)hc_uefi_hi,
	    (uint64_t)hc_uefi_lo, (uint64_t)hc_rd(HC_UTRLRSR), (uint64_t)hc_rd(HC_UTRLDBR), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hc_rd(HC_UTRLCNR), (uint64_t)hc_rd(HC_UTMRLDBR));
}

/*
 * Whatever the kernel left: UEFI's list back in the controller. The kernel is told to send nothing
 * more (it looks before every doorbell, with interrupts off), given 50 ms, then what is in flight
 * is waited for (1 s), cleared if it does not finish (UTRLCLR: every slot of that list is the
 * kernel's), the list stopped, UEFI's address written back, the list started, all read back.
 */
static void
hc_take_back(const char *when)
{
	uint32_t lo, hi, rsr, db;
	uint64_t t;

	if (!hc_known) {
		return;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	rsr = hc_rd(HC_UTRLRSR);
	db = hc_rd(HC_UTRLDBR);
	if (lo == hc_uefi_lo && hi == hc_uefi_hi && (rsr & 1)) {
		print("ufs: %s: UEFI's transfer list is in the controller (%x), running, doorbell %x\n", when, (uint64_t)lo,
		    (uint64_t)db);
		return;
	}
	print("ufs: %s: the controller has list %x:%x (run %x, doorbell %x), not UEFI's %x:%x: taking it back\n", when,
	    (uint64_t)hi, (uint64_t)lo, (uint64_t)rsr, (uint64_t)db, (uint64_t)hc_uefi_hi, (uint64_t)hc_uefi_lo);
	block_shared.loader_native.take_back = 1;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.loader_native,
	    (uint64_t)(uintptr_t)&block_shared.loader_native + sizeof(block_shared.loader_native));
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
	}
	if (db != 0) {
		print("ufs: %s: doorbell %x still set after 1 s\n", when, (uint64_t)db);
		if (hc_rd(HC_UTRLBA) != hc_uefi_lo || hc_rd(HC_UTRLBAU) != hc_uefi_hi) {
			print("ufs: %s: clearing the kernel's slots (UTRLCLR)\n", when);
			hc_wr(HC_UTRLCLR, 0);
			for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
			}
			print("ufs: %s: doorbell %x after the clear\n", when, (uint64_t)db);
		}
	}
	/* the completion notifications the kernel's requests left (it says what there was when its list went in) */
	flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
	    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
	if (block_shared.kernel_native.switched_in != 0) {
		uint32_t cnr = hc_rd(HC_UTRLCNR), ours = cnr & ~block_shared.kernel_native.cnr_at_switch;

		if (ours != 0) {
			hc_wr(HC_UTRLCNR, ours);
		}
		print("ufs: %s: completion %x, at the kernel's switch %x: cleared %x, now %x\n", when, (uint64_t)cnr,
		    (uint64_t)block_shared.kernel_native.cnr_at_switch, (uint64_t)ours, (uint64_t)hc_rd(HC_UTRLCNR));
	}
	hc_wr(HC_UTRLRSR, 0);
	for (t = READ_SYSREG(CNTVCT_EL0); (hc_rd(HC_UTRLRSR) & 1) && hc_ms_since(t) < 100;) {
	}
	hc_wr(HC_UTRLBA, hc_uefi_lo);
	hc_wr(HC_UTRLBAU, hc_uefi_hi);
	hc_wr(HC_UTRLRSR, 1);
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	hc_print_state(when);
	print("ufs: %s: UEFI's list %s\n", when, hc_list_is_uefis() ? "put back" : "NOT PUT BACK: the storage is not used");
}

/*
 * Before a run (ufs_native=): what the controller looks like, and UEFI's list address kept -- the
 * first time; later it must be the same (or this loader's own area, which is then taken back).
 */
static int
hc_init(void)
{
	uint32_t lo, hi, area = (uint32_t)(uintptr_t)hc_area;

	if (!hc_requested) {
		return 0;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	print("ufs: controller at %x: CAP %x, VER %x, HCE %x, HCS %x, IS %x, IE %x, list %x:%x, run %x, doorbell %x\n",
	    HC_PA, (uint64_t)hc_rd(HC_CAP), (uint64_t)hc_rd(HC_VER), (uint64_t)hc_rd(HC_HCE), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hi, (uint64_t)lo, (uint64_t)hc_rd(HC_UTRLRSR),
	    (uint64_t)hc_rd(HC_UTRLDBR));
	if (!hc_known) {
		if (hi == 0 && lo == area) {
			print("ufs: the controller has this loader's own area for its list before any run: left alone\n");
			return 0;
		}
		hc_uefi_lo = lo;
		hc_uefi_hi = hi;
		hc_known = 1;
		print("ufs: UEFI's transfer list is at %x:%x; it is put back after every run\n", (uint64_t)hi, (uint64_t)lo);
	} else if (lo != hc_uefi_lo || hi != hc_uefi_hi) {
		if (hi == 0 && lo == area) {
			hc_take_back("before the run");
		} else {
			print("ufs: UEFI's list moved from %x:%x to %x:%x: that is UEFI's now\n", (uint64_t)hc_uefi_hi,
			    (uint64_t)hc_uefi_lo, (uint64_t)hi, (uint64_t)lo);
			hc_uefi_lo = lo;
			hc_uefi_hi = hi;
		}
	}
	return hc_list_is_uefis();
}

/*
 * The firmware's configuration tables, by GUID (an ACPI or device tree table among them would say
 * where the storage controller is).
 */
#include "darwinfile.c"

static void
block_tables(void)
{
	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables && i < 32; i++) {
		const efi_guid *g = &ST->tables[i].guid;

		print("ufs: firmware table %d: %x-%x-%x-%x%x-%x%x%x%x%x%x at %x\n", i, (uint64_t)g->a, (uint64_t)g->b,
		    (uint64_t)g->c, (uint64_t)g->d[0], (uint64_t)g->d[1], (uint64_t)g->d[2], (uint64_t)g->d[3],
		    (uint64_t)g->d[4], (uint64_t)g->d[5], (uint64_t)g->d[6], (uint64_t)g->d[7],
		    (uint64_t)(uintptr_t)ST->tables[i].table);
	}
}

/*
 * Is this partition the one a write may reach? Everything about it has to be what it was when the
 * port was set up; one field out of place and nothing is written this run. The name UEFI reports
 * is already in bp->name (ufs_name), in ASCII.
 */
static int
block_is_darwin(const efi_partition_info *info, const efi_block_io *bio, const struct loader_block_part *bp)
{
	static const efi_guid type = BLOCK_DARWIN_TYPE, uniq = BLOCK_DARWIN_UNIQ;
	static const char name[] = "darwin";
	const char *why = NULL;

	for (unsigned i = 0; i < sizeof(name); i++) {
		if (bp->name[i] != name[i]) {
			return 0;               /* another partition: nothing to say about it */
		}
	}
	if (memcmp(&info->type_guid, &type, sizeof(type)) != 0) {
		why = "its type GUID is not the one it had";
	} else if (memcmp(&info->unique_guid, &uniq, sizeof(uniq)) != 0) {
		why = "its unique GUID is not the one it had";
	} else if (info->start_lba != BLOCK_DARWIN_FIRST || info->end_lba != BLOCK_DARWIN_LAST) {
		why = "it does not begin and end where it did";
	} else if (bp->unit != 0) {
		/* the first unit UEFI lists: this firmware's device paths say no logical unit number
		 * (device_path_print finds no 3.19 node), so bp->lun is 0xff for every unit */
		why = "it is not on the first logical unit";
	} else if (bp->block_size != 4096 || bp->blocks != BLOCK_DARWIN_LAST - BLOCK_DARWIN_FIRST + 1) {
		why = "its blocks are not what they were";
	} else if (bio->media->read_only) {
		why = "UEFI gives it out read only";
	}
	if (why != NULL) {
		print("ufs: darwin: no write will be served: %s\n", why);
		return 0;
	}
	print("ufs: darwin (LBA %d..%d of unit 0): its blocks from %d on may be written this run\n",
	    info->start_lba, info->end_lba, BLOCK_WRITE_FIRST);
	return 1;
}

static void
block_start(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	static const efi_guid dp_guid = EFI_DEVICE_PATH_GUID;
	static struct {
		efi_block_io *bio;
		const efi_device_path *path;
		uint64_t path_bytes;
		uint8_t lun;
	} units[BLOCK_MAX_UNITS];
	uint64_t nunits = 0;

	memset(&block_shared, 0, sizeof(block_shared));
	block_tables();
	/* the logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count && nunits < BLOCK_MAX_UNITS; i++) {
			efi_block_io *bio;
			efi_device_path *dp;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical ||
			    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE ||
			    EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
				continue;
			}
			units[nunits].bio = bio;
			units[nunits].path = dp;
			units[nunits].path_bytes = device_path_bytes(dp);
			units[nunits].lun = device_path_print(nunits, dp);
			nunits++;
		}
		/* (the handle array is pool memory this function owns; units[] holds the protocols, not the array) */
		BS->free_pool(handles);
	}
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information: the block service is off\n");
		return;
	}
	for (uint64_t i = 0; i < count && block_shared.parts + nunits < BLOCK_MAX_PARTS; i++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		efi_partition_info *info;
		efi_block_io *bio;
		efi_device_path *dp;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) ||
		    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE) {
			continue;
		}
		ufs_name(info, bp->name, sizeof(bp->name));
		bp->block_size = bio->media->block_size;
		bp->blocks = bio->media->last_block + 1;
		bp->start_lba = info->start_lba;
		bp->unit = 0xffff;
		bp->lun = 0xff;
		/* its unit: the one whose device path its own begins with */
		if (!EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
			uint64_t bytes = device_path_bytes(dp);

			for (uint64_t u = 0; u < nunits; u++) {
				if (units[u].path_bytes < bytes &&
				    memcmp(units[u].path, dp, units[u].path_bytes) == 0) {
					bp->unit = (uint16_t)u;
					bp->lun = units[u].lun;
				}
			}
		}
		if (block_write_requested && block_is_darwin(info, bio, bp)) {
			block_write_part = (int)block_shared.parts;
		}
		block_bios[block_shared.parts++] = bio;
		if (block_probe_requested && bp->name[0] == 'd' && bp->name[1] == 'a' && bp->name[2] == 'r' &&
		    bp->name[3] == 'w' && bp->name[4] == 'i' && bp->name[5] == 'n' && bp->name[6] == 0) {
			block_io2_probe(handles[i], bio);
		}
	}
	BS->free_pool(handles);
	/* darwin as a file of userdata: a partition of its own blocks, on no unit (the kernel reads it through here) */
	if (darwin_file_bio() != NULL && block_shared.parts + nunits < BLOCK_MAX_PARTS) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		static const char darwin_name[] = "darwin";

		memcpy(bp->name, darwin_name, sizeof(darwin_name));
		bp->block_size = 4096;
		bp->blocks = DARWIN_FILE_BLOCKS;
		bp->start_lba = 0;
		bp->unit = 0xffff;
		bp->lun = 0xff;
		bp->flags = BLOCK_PART_VIRTUAL;
		if (block_write_requested) {
			block_write_part = (int)block_shared.parts;
			print("ufs: darwin (a file of %s): its blocks from %d on may be written this run\n", DARWIN_FILE_HOST,
			    BLOCK_WRITE_FIRST);
		}
		block_bios[block_shared.parts++] = darwin_file_bio();
		/* the kernel's copy of the map: the host is userdata's entry in this table */
		for (uint32_t k = 0; k < block_shared.parts; k++) {
			if (str_eq(block_shared.part[k].name, DARWIN_FILE_HOST) && !(block_shared.part[k].flags & BLOCK_PART_UNIT)) {
				darwin_file_publish(k);
				break;
			}
		}
	}
	for (uint64_t u = 0; u < nunits; u++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		char *n = bp->name;

		n[0] = 'u', n[1] = 'n', n[2] = 'i', n[3] = 't';
		n[4] = (char)('0' + u / 10 % 10), n[5] = (char)('0' + u % 10), n[6] = 0;
		if (u < 10) {
			n[4] = n[5], n[5] = 0;
		}
		bp->block_size = units[u].bio->media->block_size;
		bp->blocks = units[u].bio->media->last_block + 1;
		bp->unit = (uint16_t)u;
		bp->lun = units[u].lun;
		bp->flags = BLOCK_PART_UNIT;
		block_bios[block_shared.parts++] = units[u].bio;
	}
	/* the kernel's own transfer list: this loader's area, if the controller is as UEFI keeps it */
	if (hc_known && hc_list_is_uefis() && hc_rd(HC_UTRLDBR) == 0 && (hc_rd(HC_HCE) & 1)) {
		memset(hc_area, 0, sizeof(hc_area));
		flush_dcache((uint64_t)(uintptr_t)hc_area, (uint64_t)(uintptr_t)hc_area + sizeof(hc_area));
		block_shared.native.hc_pa = HC_PA;
		block_shared.native.area_pa = (uint64_t)(uintptr_t)hc_area;
		block_shared.native.area_size = sizeof(hc_area);
		block_shared.native.ucd_offset = HC_UCD_OFFSET;
		block_shared.native.ucd_size = HC_UCD_SIZE;
		block_shared.native.slots = HC_SLOTS;
		block_shared.native.uefi_utrlba = hc_uefi_lo;
		block_shared.native.uefi_utrlbau = hc_uefi_hi;
		print("ufs: the kernel may put its own transfer list in, at %x (this loader's), for the run\n",
		    (uint64_t)(uintptr_t)hc_area);
	} else if (hc_requested) {
		print("ufs: the kernel's own transfer list is not offered: the controller is not as UEFI keeps it\n");
	}
	hc_refused_reads = 0;
	hc_log_skips = 0;
	/* per run, like the two above: this loader stays resident across runs, and a tally carried
	 * over from the run before is read as this one's */
	block_writes = block_writes_refused = block_write_bytes = 0;
	if (block_write_part >= 0) {
		block_shared.write_part_plus_one = (uint32_t)block_write_part + 1;
		block_shared.write_first = BLOCK_WRITE_FIRST;
	}
	block_shared.magic = BLOCK_MAGIC;
	block_shared.version = BLOCK_VERSION;
	block_shared.size = sizeof(block_shared);
	block_shared.buffer_size = BLOCK_BUFFER_SIZE;
	block_shared.buffer_pa = (uint64_t)(uintptr_t)block_buffer;
	clean_dcache((uint64_t)(uintptr_t)&block_shared, (uint64_t)(uintptr_t)&block_shared + sizeof(block_shared));
	block_enabled = 1;
	print("ufs: block service: %d partitions and %d units, page %x, buffer %x of %d KB\n",
	    (uint64_t)block_shared.parts - nunits, nunits, (uint64_t)(uintptr_t)&block_shared, block_shared.buffer_pa,
	    (uint64_t)(BLOCK_BUFFER_SIZE >> 10));
}

/*
 * Whether the storage could have more than one request in flight (ufs_probe=1): Block I/O 2
 * on the partitions, and whether its reads come back before they are done.  Eight 4 KB
 * reads at places 64 MB apart in "darwin", one after another through Block I/O and then all
 * eight at once through Block I/O 2, each into its own part of the buffer.  Before the kernel
 * runs (the UEFI timer still ticks), and only to say what there is.
 *
 * What it says on this phone: Block I/O 2 is there and the calls come back at once (eight in
 * 160 us, none done), but the eight are done only after 61.5 ms, against 1.0 ms one at a time
 * through Block I/O -- Qualcomm's driver finishes them from the UEFI timer, which besides does
 * not tick on core 0 while the kernel runs.  So one request at a time, synchronously.
 */
static void
block_io2_probe(efi_handle h, efi_block_io *bio)
{
	static const efi_guid blk2_guid = EFI_BLOCK_IO2_GUID;
	efi_status (EFIAPI *create_event)(uint32_t, uint64_t, void *, void *, void **) =
	    (efi_status (EFIAPI *)(uint32_t, uint64_t, void *, void *, void **))BS->create_event;
	efi_status (EFIAPI *check_event)(void *) = (efi_status (EFIAPI *)(void *))BS->check_event;
	static efi_block_io2_token tokens[8];
	efi_block_io2 *bio2 = NULL;
	uint64_t bs = bio->media->block_size, t0, t_sync, t_issue = 0, t_all, freq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t returned_done = 0, pending, polls = 0;
	efi_status s;

	if (EFI_ERROR(BS->handle_protocol(h, &blk2_guid, (void **)&bio2)) || bio2 == NULL) {
		print("ufs: darwin: no Block I/O 2: one request at a time it is\n");
		return;
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		s = bio->read_blocks(bio, bio->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 7, bs,
		    block_buffer + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: Block I/O read failed %x\n", s);
			return;
		}
	}
	t_sync = READ_SYSREG(CNTVCT_EL0) - t0;
	for (int i = 0; i < 8; i++) {
		tokens[i].transaction_status = 0;
		if (EFI_ERROR(create_event(0, 8 /* TPL_CALLBACK */, NULL, NULL, &tokens[i].event))) {
			print("ufs: probe: no event\n");
			return;
		}
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		/* other places than the synchronous reads, so that nothing the storage kept is read again */
		s = bio2->read_blocks_ex(bio2, bio2->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 1031,
		    &tokens[i], bs, block_buffer + 1048576 + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: ReadBlocksEx %d failed %x\n", (uint64_t)i, s);
			return;
		}
		if (check_event(tokens[i].event) == 0) {
			returned_done++;        /* already signalled when the call came back */
		}
	}
	t_issue = READ_SYSREG(CNTVCT_EL0) - t0;
	pending = 8 - returned_done;
	while (pending > 0 && polls < 50000000) {
		pending = 0;
		for (int i = 0; i < 8; i++) {
			if (tokens[i].event != NULL && check_event(tokens[i].event) != 0) {
				pending++;
			} else if (tokens[i].event != NULL) {
				BS->close_event(tokens[i].event);
				tokens[i].event = NULL;
			}
		}
		polls++;
	}
	t_all = READ_SYSREG(CNTVCT_EL0) - t0;
	print("ufs: darwin: Block I/O 2 there. 8 random 4 KB reads: one at a time %d us; at once: the calls came back "
	    "after %d us (%d of them done by then), all done after %d us, %d still pending\n",
	    t_sync * 1000000 / freq, t_issue * 1000000 / freq, returned_done, t_all * 1000000 / freq, pending);
}

/* one request, if there is one: called from core 0's watch loop while the kernel runs */
static void
block_poll(void)
{
	uint64_t ask = (uint64_t)(uintptr_t)&block_shared.ask;
	uint32_t request, part, op;
	uint64_t lba, count, bytes, started;
	efi_status s = 0x8000000000000002ULL;     /* EFI_INVALID_PARAMETER: a request out of bounds */

	/* the kernel writes the request into memory past this core's cache: look at memory */
	flush_dcache(ask, ask + sizeof(block_shared.ask));
	request = block_shared.ask.request;
	if (request == block_shared.answer.answered) {
		return;
	}
	part = block_shared.ask.part;
	lba = block_shared.ask.lba;
	count = block_shared.ask.count;
	op = block_shared.ask.op;
	started = READ_SYSREG(CNTVCT_EL0);
	bytes = 0;
	if (part < block_shared.parts && count != 0 && count <= BLOCK_BUFFER_SIZE / block_shared.part[part].block_size &&
	    lba < block_shared.part[part].blocks && count <= block_shared.part[part].blocks - lba) {
		efi_block_io *bio = block_bios[part];

		bytes = count * block_shared.part[part].block_size;
		if (!hc_list_is_uefis()) {
			/* the kernel's own list is in: UEFI's driver must not ring now (the kernel should not ask) */
			if (hc_refused_reads++ < 4) {
				print("ufs: a read asked for while the kernel's own transfer list is in the controller: refused\n");
			}
			s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
			bytes = 0;
		} else if (op == BLOCK_OP_WRITE) {
			if (block_write_part < 0 || part != (uint32_t)block_write_part || lba < BLOCK_WRITE_FIRST) {
				/* not darwin, or not past what the build writes from the computer: never */
				if (block_writes_refused++ < 8) {
					print("ufs: a write of %d blocks from %d of %s: REFUSED\n", count, lba,
					    block_shared.part[part].name);
				}
				s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
				bytes = 0;
			} else {
				/*
				 * The kernel filled the buffer and cleaned its lines of it; drop whatever this
				 * core holds, write, and read back -- the answer says the blocks hold what the
				 * kernel gave, or the write is no good. The read back overwrites the buffer,
				 * which the kernel does not need: it has the data.
				 */
				uint32_t want;

				flush_dcache((uint64_t)(uintptr_t)block_buffer,
				    (uint64_t)(uintptr_t)block_buffer + bytes);
				want = ufs_crc32(block_buffer, bytes);
				s = bio->write_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
				if (!EFI_ERROR(s)) {
					bio->flush_blocks(bio);
					s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
					flush_dcache((uint64_t)(uintptr_t)block_buffer,
					    (uint64_t)(uintptr_t)block_buffer + bytes);
					if (!EFI_ERROR(s) && ufs_crc32(block_buffer, bytes) != want) {
						print("ufs: %d blocks from %d of darwin read back DIFFERENT\n",
						    count, lba);
						s = 0x800000000000000AULL;      /* EFI_VOLUME_CORRUPTED */
					}
				}
				if (!EFI_ERROR(s)) {
					block_writes++;
					block_write_bytes += bytes;
				}
			}
		} else if (op == BLOCK_OP_READ) {
			s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
			/* what the controller wrote is in memory; drop whatever this core held of the buffer */
			flush_dcache((uint64_t)(uintptr_t)block_buffer, (uint64_t)(uintptr_t)block_buffer + bytes);
		} else {
			bytes = 0;              /* an operation this loader does not know: nothing is touched */
		}
	}
	block_shared.answer.status = (int64_t)s;
	block_shared.answer.ticks += READ_SYSREG(CNTVCT_EL0) - started;
	if (EFI_ERROR(s)) {
		block_shared.answer.failed++;
	} else {
		block_shared.answer.served++;
		block_shared.answer.bytes += bytes;
	}
	__asm__ volatile ("dmb sy");
	block_shared.answer.answered = request;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.answer,
	    (uint64_t)(uintptr_t)&block_shared.answer + sizeof(block_shared.answer));
}

static void
charger_start(void)
{
	static const efi_guid charger_ex_guid = QCOM_CHARGER_EX_GUID;
	static const efi_guid qcom_charger_guid = QCOM_CHARGER_GUID;
	efi_status ex_status, qcom_status, init_status = 0;

	if (charger_enabled || (PLAT != &platform_redwood && PLAT != &platform_umi)) {
		return;
	}
	ex_status = BS->locate_protocol(&charger_ex_guid, NULL, (void **)&CHARGER_EX);
	qcom_status = BS->locate_protocol(&qcom_charger_guid, NULL, (void **)&QCOM_CHARGER);
	if (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL) {
		/* Re-open the driver's status path after QcomChargerApp handed off to the OS. */
		init_status = QCOM_CHARGER->initialize(1);
	}
	charger_enabled = (!EFI_ERROR(ex_status) && CHARGER_EX != NULL) ||
	    (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL);
	print("battery: ChargerEx=%x QcomCharger=%x init=%x shared=%x\n", ex_status, qcom_status, init_status,
	    (uint64_t)(uintptr_t)&charger_shared);
	charger_poll();
	print("battery: flags=%x SOC=%d%% V=%d mV I=%d mA T=%d C action=%d\n", (uint64_t)charger_shared.flags,
	    (uint64_t)(int64_t)charger_shared.soc_percent, (uint64_t)(int64_t)charger_shared.voltage_mv,
	    (uint64_t)(int64_t)charger_shared.current_ma, (uint64_t)(int64_t)charger_shared.temperature_c,
	    (uint64_t)charger_shared.charger_action);
}

/* For the record: how much the kernel on the other core has logged so far, and the end of it. */
static void
note_kernel_progress(const struct kernel_live *live, uint64_t slide, uint64_t seconds)
{
	const char *log;
	uint32_t len, from;

	if (live == NULL) {
		print("%d s\n", seconds);
		return;
	}
	log = (const char *)(live->log + slide);
	/* its first words are written with the caches off: look at memory */
	flush_dcache(live->log_len + slide, live->log_len + slide + 4);
	len = *(volatile uint32_t *)(live->log_len + slide);
	if (len > live->log_head + live->log_ring) {
		len = 0;
	}
	from = len > 240 ? len - 240 : 0;
	flush_dcache((uint64_t)(uintptr_t)log + from, (uint64_t)(uintptr_t)log + len);
	print("%d s: the kernel has logged %d bytes, heartbeat %d; the last of it:\n", seconds, (uint64_t)len, live->heartbeat);
	for (uint32_t i = from; i < len; i++) {
		put_char(log[i] >= 0x20 && log[i] < 0x7f ? log[i] : log[i] == '\n' ? '\n' : '.');
	}
	print("\n");
}

/* A call of the firmware's power interface, the way this platform makes them. */
static int64_t
psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int64_t status;

	if (PLAT->psci_hvc) {
		register uint64_t x0 __asm__("x0") = function;
		register uint64_t x1 __asm__("x1") = a1;
		register uint64_t x2 __asm__("x2") = a2;
		register uint64_t x3 __asm__("x3") = a3;

		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                        : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
		status = (int64_t)x0;
	} else {
		smc_x3 = a3;
		status = (int64_t)smc_call(function, a1, a2, NULL, NULL);
		smc_x3 = 0;
	}
	return status;
}

/*
 * Run the kernel on the core with that MPIDR and wait until it is back. Returns 0 when it has
 * been and gone (the caller goes on as after loader_return_stub), a negative number if the
 * firmware refused to start the core, OTHER_CPU_NOT_STARTED, or OTHER_CPU_HUNG: then the kernel
 * is still sitting on that core, but what it logged is there to be read (loader_ctx, as after a
 * return). `live` is where the kernel shows its signs of life, if it has such a place;
 * `budget` is how many seconds it has said it will run.
 */
/* Same polling-only mode used after NOTE_SECONDS, now also required before
 * USB ownership is acknowledged. Stop() does not remove other DXEs' timers:
 * USB power/config callbacks can consume events or reinitialize DWC3 later.
 * UFS Block I/O is synchronous/polled; watchdog and deadline use CNTVCT.
 */
static void
firmware_watch_only(uint64_t *daif, uint64_t *group1, int *watching_only)
{
	if (*watching_only) return;
	*daif = READ_SYSREG(DAIF);
	__asm__ volatile ("msr daifset, #0xf" ::: "memory");
	*group1 = READ_SYSREG(S3_0_C12_C12_7);
	__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" ((uint64_t)0) : "memory");
	*watching_only = 1;
}

static int64_t
run_on_other_cpu(uint64_t mpidr, uint64_t entry, uint64_t boot_args_pa, const struct kernel_live *live, uint64_t slide,
    uint64_t budget)
{
	uint64_t daif = 0, group1 = 0, started, last_pet, last_battery, last_beat_at, last_beat = 0, noted = 0;
#ifndef HAKIOS_LOADER_PROD
	uint64_t last_panic_check, panic_seen_at = 0;
#endif
	int64_t status;
	int arrived = 0, watching_only = 0;

	running_live = live;
	run_active = 1;
	running_slide = slide;
	other_cpu_start.entry = entry;
	other_cpu_start.boot_args = boot_args_pa;
	other_cpu_stage = 0;
	other_cpu_done = 0;
	psci_by_hvc = PLAT->psci_hvc;
	/* that core starts with its caches off: all it reads must be in memory, not in this core's cache */
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_start, (uint64_t)(uintptr_t)&other_cpu_start + sizeof(other_cpu_start));
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
	clean_dcache((uint64_t)(uintptr_t)&psci_by_hvc, (uint64_t)(uintptr_t)&psci_by_hvc + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_entry + 0x1000);
	clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));

	/* the last argument arrives in that core's x0 */
	status = psci_call(PSCI_CPU_ON, mpidr, (uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_start);

	started = last_pet = last_battery = last_beat_at = READ_SYSREG(CNTVCT_EL0);
#ifndef HAKIOS_LOADER_PROD
	last_panic_check = started;
#endif
	print("core %x: the firmware answers %d to the request to start it\n", mpidr, (uint64_t)status);
	if (PLAT->log_other_cpu) {
		flush_log();
	}
#ifdef HAKIOS_LOADER_PROD
	(void)budget;
	(void)noted;
	/* No live USB or firmware log I/O after handoff; retain the UFS/charger broker. */
	firmware_watch_only(&daif, &group1, &watching_only);
#endif
	while (status == 0) {
#ifndef HAKIOS_LOADER_PROD
		if (usb_kernel_requested) {
			usb_handoff_poll(&daif, &group1, &watching_only);
			/* Independent of USB and of the kernel's scheduler/return timer. */
			if (seconds_since(started) >= budget + 20) usb_kernel_reset();
		}
		/*
		 * The first seconds are the ones in which a new arrangement goes wrong, and a machine
		 * that resets takes this loader's memory with it: where there is storage for the log,
		 * what the kernel is seen to do goes there as it happens. That needs UEFI's services,
		 * so until NOTE_SECONDS are over this core keeps its interrupts.
		 */
		if (!watching_only && seconds_since(started) >= noted + 2) {
			noted = seconds_since(started);
			if (PLAT->log_other_cpu) {
				if (!arrived) {
					print("%d s: the core has not arrived\n", noted);
				} else {
					note_kernel_progress(live, slide, noted);
				}
				flush_log();
			}
		}
		/*
		 * After that this core only watches: no interrupts (the kernel reprograms the interrupt
		 * controller's shared half; UEFI's handlers had better not run on top of that for long).
		 */
		/* hk: the budget is up -- take the kernel's whole message ring before the watchdog resets us */
		if (!budget_dumped && seconds_since(started) >= budget) {
			budget_dumped = 1;
			if (arrived && live != NULL) {
				log_of_hung_kernel(live, slide);
				write_logs((const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
			}
			status = OTHER_CPU_HUNG;
		}
		if (!watching_only && seconds_since(started) >= NOTE_SECONDS) {
			firmware_watch_only(&daif, &group1, &watching_only);
		}
#endif
		/* both are written by a core with its caches off: look at memory, not at our copy */
		flush_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
		if (other_cpu_done) {
			/* what the kernel returned, written the same way */
			flush_dcache((uint64_t)(uintptr_t)&loader_ctx[CTX_LOG_PA], (uint64_t)(uintptr_t)&loader_ctx[CTX_KSP + 1]);
			break;
		}
		if (!arrived) {
			flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
			arrived = other_cpu_stage != 0;
			if (!arrived && seconds_since(started) >= START_SECONDS) {
				status = OTHER_CPU_NOT_STARTED;
			}
		}
		/*
		 * The watchdog is this core's to feed for as long as the kernel is expected to run:
		 * the kernel does it too, but whether that counts from another core is not known.
		 * After that a kernel that neither returns nor hangs visibly meets the watchdog.
		 */
#ifdef HAKIOS_LOADER_PROD
		/* Acquire the kernel's shared progress counter on every pass. */
		uint64_t beat = __atomic_load_n(&live->heartbeat, __ATOMIC_ACQUIRE);
		/* A new heartbeat every 10 s; allow 25 s scheduling latency, never total uptime. */
		if (beat != last_beat) {
			last_beat = beat;
			last_beat_at = READ_SYSREG(CNTVCT_EL0);
		}
		if (wdt_armed && seconds_since(last_pet) >= 5 &&
		    ((last_beat != 0 && seconds_since(last_beat_at) < 25) ||
		     (last_beat == 0 && seconds_since(started) < SILENCE_SECONDS))) {
#else
		if (wdt_armed && seconds_since(last_pet) >= 5 && seconds_since(started) < budget + 60) {
#endif
			wdt_pet();
			last_pet = READ_SYSREG(CNTVCT_EL0);
		}
		/* Qualcomm's UEFI charger stack owns the PMIC/GLINK side; keep XNU's cache live. */
		if (charger_enabled && usb_handoff.ack != 1 && seconds_since(last_battery) >= 5 && hc_list_is_uefis()) {
			/* (not while the kernel's own transfer list is in: Qualcomm's DXEs are not to touch the storage) */
			charger_poll();
			last_battery = READ_SYSREG(CNTVCT_EL0);
		}
#ifndef HAKIOS_LOADER_PROD
		if (arrived && live != NULL) {
			if (live->heartbeat != last_beat) {
				last_beat = live->heartbeat;
				last_beat_at = READ_SYSREG(CNTVCT_EL0);
			} else if (seconds_since(last_panic_check) >= 1) {
				/* (once a second: reading the kernel's table is a cache flush, not a loop step) */
				uint64_t silent;

				last_panic_check = READ_SYSREG(CNTVCT_EL0);
				/*
				 * UMI (v34): a panic whose record says the kernel will not come back ends the
				 * wait before the 50 s below. Its processor says stage PARKED and stops there
				 * (osfmk/arm64/umi_cpu_pc.h), so no heartbeat follows; the panic's own lines in
				 * the table are what the report prints. The ordinary silence is still the
				 * backstop for every other way a kernel stops.
				 */
				if (hung_panic_parked(live, slide)) {
					if (panic_seen_at == 0) {
						panic_seen_at = READ_SYSREG(CNTVCT_EL0);
					} else if (seconds_since(panic_seen_at) >= PANIC_REPORT_SECONDS) {
						silent = seconds_since(last_beat_at);
						hung_silence_seconds = silent != 0 ? silent : 1;
						log_of_hung_kernel(live, slide);
						status = OTHER_CPU_HUNG;
					}
				} else if (seconds_since(last_beat_at) >= SILENCE_SECONDS) {
					hung_silence_seconds = SILENCE_SECONDS;
					log_of_hung_kernel(live, slide);
					status = OTHER_CPU_HUNG;
				}
			}
		}
#endif
		if (block_enabled) {
			block_poll();
		}
#ifndef HAKIOS_LOADER_PROD
		usb_poll_command();
#endif
		if (block_enabled) {
			/*
			 * The pause is spent looking at the kernel's mailbox: with one look a pass, a
			 * request waited 60-75 us on average before this core noticed it, more than a
			 * 4 KB ReadBlocks takes (27 us). Same length of pause as below.
			 */
			for (int k = 0; k < 64; k++) {
				block_poll();
				for (volatile int i = 0; i < 300; i++) {
				}
			}
		} else {
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
	}
#ifdef HAKIOS_LOADER_PROD
	/* CPU start failure or an impossible return: no firmware/storage recovery or petting. */
	for (;;) { __asm__ volatile ("wfe"); }
#endif
	running_live = NULL;
	run_active = 0;

	/*
	 * A kernel that stopped answering: what it left (its log, the radio's last accesses, the
	 * screen for a while) is said here, before the reset below -- a run started over USB is
	 * reset from here and never returns to run_kernel(), so this is the only place it is seen.
	 */
	if (status == OTHER_CPU_HUNG) {
		report_hung_kernel(live, slide, mpidr);
	}
	/* Reset before restoring IRQs: queued firmware callbacks must never regain
	 * controller access between kernel return and Android reset. */
	if (usb_kernel_requested) usb_kernel_reset();
	if (watching_only) {
		__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" (group1));
		__asm__ volatile ("msr daif, %0" : : "r" (daif));
	}
	return status;
}

/* ------------------------------------------------------------------------ */
/* Hypervisor watchdog (Qualcomm "Haven" virtual watchdog, SMC interface as  */
/* used by Linux drivers/virt/haven/hh_virt_wdt.c). Armed for the whole stay */
/* in the loader and the kernel: any hang ends in a reset without a human.   */

#define VIRT_WDT_CONTROL   0x86000005u
#define VIRT_WDT_STATUS    0x86000006u
#define VIRT_WDT_PET       0x86000007u
#define VIRT_WDT_SET_TIME  0x86000008u
#define WDT_BITE_MS        30000u

static const struct platform *PLAT;
static int wdt_armed;

static uint64_t smc_x3;             /* the fourth argument register, for the one call that has one */

static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
	register uint64_t x0 __asm__("x0") = id;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = smc_x3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	if (r1 != NULL) {
		*r1 = x1;
	}
	if (r2 != NULL) {
		*r2 = x2;
	}
	return x0;
}

static void
wdt_pet(void)
{
	if (wdt_armed) {
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

/* Change the time to the bite, and start counting again. The hypervisor takes 16 bits of milliseconds. */
static void
wdt_set_ms(uint64_t ms)
{
	if (ms > 65000) {
		ms = 65000;
	}
	if (wdt_armed) {
		smc_call(VIRT_WDT_SET_TIME, ms, ms, NULL, NULL);
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

static void
wdt_arm(void)
{
	uint64_t ctl = 0, since = 0;

#ifdef HAKIOS_LOADER_PROD
	if (PLAT != &platform_umi) {
#else
	if (PLAT != &platform_redwood) {
#endif
		return; /* SMC is not routed anywhere useful on QEMU virt */
	}
	uint64_t st = smc_call(VIRT_WDT_STATUS, 0, 0, &ctl, &since);
	uint64_t r_time = smc_call(VIRT_WDT_SET_TIME, WDT_BITE_MS, WDT_BITE_MS, NULL, NULL);
	uint64_t r_ctl = smc_call(VIRT_WDT_CONTROL, 3, 0, NULL, NULL);
	uint64_t r_pet = smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	print("watchdog: status ret=%x ctl=%x since_pet=%x; set_time=%x enable=%x pet=%x\n", st, ctl, since, r_time,
	    r_ctl, r_pet);
#ifdef HAKIOS_LOADER_PROD
	wdt_armed = (r_time == 0 && r_ctl == 0 && r_pet == 0);
#else
	wdt_armed = (r_pet == 0);
#endif
}

/* ------------------------------------------------------------------------ */
/* UEFI exception hook: put the fault into the log instead of on the screen  */

extern char __ImageBase;

static void EFIAPI
exception_handler(int64_t type, uint64_t *ctx)
{
	print("\nUEFI EXCEPTION type=%d elr=%x esr=%x far=%x lr=%x sp=%x spsr=%x\n", (uint64_t)type, ctx[EXC_CTX_ELR],
	    ctx[EXC_CTX_ESR], ctx[EXC_CTX_FAR], ctx[EXC_CTX_LR], ctx[EXC_CTX_SP], ctx[EXC_CTX_SPSR]);
	print("loader image base %x (elr offset %x)\n", (uint64_t)(uintptr_t)&__ImageBase,
	    ctx[EXC_CTX_ELR] - (uint64_t)(uintptr_t)&__ImageBase);
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

static void
install_exception_hook(void)
{
	static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
	efi_cpu_arch *cpu;

	if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu))) {
		return;
	}
	for (int64_t type = 0; type <= 3; type += 3) { /* synchronous, SError */
		cpu->register_interrupt_handler(cpu, type, NULL);
		efi_status s = cpu->register_interrupt_handler(cpu, type, exception_handler);
		if (EFI_ERROR(s)) {
			print("exception hook %d: status %x\n", (uint64_t)type, s);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Payload                                                                  */

struct payload_info {
	const uint8_t *kernel;
	uint32_t kernel_size;           /* the kernel entry's size: what its Mach-O header may point into */
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	const uint8_t *resident;        /* "resident": an image to put at the resident address (a bundle of the flash) */
	uint32_t resident_size;
	const char *cmdline;
	uint64_t checkpoint;
	uint64_t forced_base;
	uint64_t resident_rd;           /* size of a root image the host put at the resident address */
	const char *kernel_version;     /* kernel.ver: the version of the kernel in this payload (tools/kernel-version.py) */
	int usb_mode;
};

static uint64_t
parse_number(const char *c, unsigned base)
{
	uint64_t v = 0;
	for (; *c; c++) {
		uint64_t d = (*c >= '0' && *c <= '9') ? (uint64_t)(*c - '0') : (*c >= 'a' && *c <= 'f') ? (uint64_t)(*c - 'a' + 10) : 16;
		if (d >= base) {
			break;
		}
		v = v * base + d;
	}
	return v;
}

/* How many bytes after the header the walk over a payload's entries may cover: the header's size, but never more than fits */
static uint64_t
payload_walk_size(uint32_t header_size)
{
	uint64_t most = PAYLOAD_MAX_SIZE - 0x100000ULL - sizeof(struct payload_header);

	return header_size < most ? header_size : most;
}

static int
parse_payload(struct payload_info *pi, int verbose)
{
	struct payload_header *ph = (struct payload_header *)PAYLOAD_BASE;

	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	if (ph->magic != PAYLOAD_MAGIC) {
		print("no payload at %x (magic %x)\n", PAYLOAD_BASE, (uint64_t)ph->magic);
		return 0;
	}
	/*
	 * The size in the header is not trusted past what the payload region can hold (the file's size is not
	 * known here), so a wild value cannot send the walk through memory that is not the payload.
	 */
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + payload_walk_size(ph->size);
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		const char *text = (const char *)(p + sizeof(*e));
		if (e->name[0] == 0) {
			break;
		}
		if (verbose) {
			print("payload entry '%s' size %x\n", e->name, (uint64_t)e->size);
		}
		/*
		 * An entry that claims more bytes than are left is a torn or made-up payload: the ramdisk copy would read
		 * past it, and the 32-bit rounding below would wrap. Not booted, as with a payload of the wrong magic.
		 */
		if ((uint64_t)e->size > (uint64_t)(pend - (p + sizeof(*e)))) {
			print("payload entry is larger than the payload (size %x): not used\n", (uint64_t)e->size);
			return 0;
		}
		if (str_eq(e->name, "resident")) {
			/* what `xl ramdisk` uploads over USB, carried by the payload instead: an installed phone's boot
			 * bundle has no computer to upload it (common/flash/mkbundle.py --resident) */
			pi->resident = p + sizeof(*e);
			pi->resident_size = e->size;
		} else if (str_eq(e->name, "ramdisk")) {
			pi->ramdisk = p + sizeof(*e);             /* becomes /dev/md0, the root device */
			pi->ramdisk_size = e->size;
		} else if (str_eq(e->name, "kernel")) {
			pi->kernel = p + sizeof(*e);
			pi->kernel_size = e->size;
		} else if (str_eq(e->name, "cmdline")) {
			pi->cmdline = text;
		} else if (str_eq(e->name, "physbase")) {
			pi->forced_base = parse_number(text, 16); /* testing aid; must be free RAM */
		} else if (str_eq(e->name, "rdres")) {
			pi->resident_rd = parse_number(text, 16); /* e.g. loaded there by QEMU; see RESIDENT_RD_OFFSET */
		} else if (str_eq(e->name, "kernel.ver")) {
			pi->kernel_version = text;                /* printed under the banner; the kernel gets it as hakios_kver */
		} else if (str_eq(e->name, "checkpoint")) {
			pi->checkpoint = parse_number(text, 10);  /* where the kernel should stop and come back */
		} else if (str_eq(e->name, "usb")) {
			pi->usb_mode = 1;                         /* stay in the loader and take commands over USB */
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* NVRAM image                                                              */
/*                                                                          */
/* iBoot hands the kernel a copy of the NVRAM contents in CHRP format       */
/* (iokit/Kernel/IONVRAMCHRPHandler.cpp). There is no NVRAM here, so build  */
/* an image with a valid header and one empty "common" partition.           */

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	/* checksum over signature, length and name, folded to 8 bits */
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");                           /* Apple header: 32 bytes */
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common"); /* the rest: common partition */
	memcpy(image + 20, &generation, 4);
	/* Adler-32 over everything after the checksum field itself */
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ------------------------------------------------------------------------ */
/* Running the kernel                                                       */

static efi_gop *GOP;
/* The display handed to the kernel: the firmware's, or one we set up ourselves (QEMU). base 0 = none. */
static struct {
	uint64_t base;
	uint32_t width, height, stride;
} SCREEN;

/* ------------------------------------------------------------------------ */
/* A dead kernel's report, on the frame the panel is actually scanning      */
/*
 * report_hung_kernel() draws with the firmware console into the firmware's frame (SCREEN.base), and that
 * frame is the start of the splash carve-out. A kernel with fb_dpu flips the source pipe to one of the
 * carve-out's other frames at every swap and never flips back when it dies, so a report drawn into the
 * firmware's frame is drawn where nothing fetches. So after the drawing, copy it into a frame the pipe can
 * be pointed at, at the width it is drawn with (the row stride is its own: the pipe is given exactly that,
 * never the padded stride of whatever CoreAnimation surface the kernel last drew into the frame) -- the
 * image centred and the rest of every row blacked -- and commit it with one CTL_FLUSH: the next autorefresh
 * transfer shows it. The destination is chosen not to overlap the console's own frame, so the copy never
 * eats its source and the report can be re-copied after every stage. The pipe's SRC_SIZE (its geometry,
 * and with it the QSEED3 setup the kernel left) is left as it is; only SRC0_ADDR, SRC_YSTRIDE0 and the
 * flush are written.
 *
 * The display's registers may only be read while its power switch, its bus clocks and the DPU's core
 * clock are on: on these SoCs a read of a block whose clock is off does not fail, it hangs the whole bus,
 * and on umi (not PROD) the watchdog is not armed, so that would be a permanent hang. Everything below is
 * behind that check -- the bus clocks and power switch exactly as the kernel asks them, the core clock and
 * a constant of the block itself as extra proof -- nothing is polled, and a failed check changes nothing
 * (the report then stays in the firmware's frame, which is what an older kernel -- one that never moved
 * the scanout -- shows anyway).
 *
 * Where the register offsets come from: DISPCC is clock-controller@af00000 (sm8250.dtsi); upstream
 * drivers/clk/qcom/dispcc-sm8250.c names disp_cc_mdss_ahb_clk by halt/enable register 0x2080 and
 * disp_cc_mdss_mdp_clk by 0x200c, both BRANCH_HALT with enable bit 0 and CLK_OFF bit 31 (the same layout
 * the kernel's clock_on() uses). MDSS is display-subsystem@ae00000, its HW_VERSION at offset 0 (freedreno
 * registers/display/mdss.xml: MAJOR bits 31:28; upstream msm_mdss.c reads it as MDSS_HW_VER(major, minor,
 * step) and treats >= 6.0.0 as this generation), and sm8250's DPU catalog is dpu_6_0_sm8250.
 */
#define HUNG_MDSS_HW_VERSION    0x0ae00000ULL   /* MDSS's own version register: a constant while it runs */
#define HUNG_MDP_PHYS           0x0ae01000ULL   /* MDSS + 0x1000: the DPU's block offsets are from here */
#define HUNG_GCC_DISP_AHB_CBCR  0x0010b00cULL   /* DISPCC's AHB clock on GCC: always answers */
#define HUNG_DISPCC_GDSCR       0x0af03000ULL   /* MDSS core power switch */
#define HUNG_DISPCC_AHB_CBCR    0x0af02080ULL   /* MDSS AHB clock */
#define HUNG_DISPCC_MDP_CBCR    0x0af0200cULL   /* DISP_CC_MDSS_MDP_CLK: the DPU's core clock */
#define HUNG_SPLASH_PHYS        0x9c000000ULL   /* cont_splash_region: the GOP frame and the DPU's frames */
#define HUNG_SPLASH_SIZE        0x2300000ULL
#define HUNG_CTL_INTF_ACTIVE    0x0f4u
#define HUNG_CTL_INTF_1         2u              /* INTF_1 is DSI0, the panel */
#define HUNG_CTL_FLUSH          0x018u
#define HUNG_CTL_LAYER(lm)      ((lm) == 5 ? 0x024u : 4u * (lm))
#define HUNG_CTL_LAYER_EXT(lm)  (0x040u + 4u * (lm))
#define HUNG_CTL_LAYER_EXT2(lm) (0x070u + 4u * (lm))
#define HUNG_CTL_LAYER_EXT3(lm) (0x0a0u + 4u * (lm))
#define HUNG_CTL_MIXER_BORDER_OUT (1u << 24)
#define HUNG_CTL_FLUSH_CTL      (1u << 17)
#define HUNG_SSPP_SRC_SIZE      0x000u
#define HUNG_SSPP_SRC_ADDR      0x014u          /* SRC0..SRC3, one every 4 bytes */
#define HUNG_SSPP_SRC_YSTRIDE0  0x024u
#define HUNG_SSPP_SRC_FORMAT    0x030u
#define HUNG_FMT_RGBA8888       0x237ffu        /* what UEFI leaves in the pipe: 8 bits a pixel, no tiling */
#define HUNG_ROW_MAX            16384u          /* the widest row this copies through (16 KiB) */
#define HUNG_FRAME_SPINS        (32u * 1024u * 1024u)  /* the one-frame wait's cap: far past 33 ms */
/* Where the report goes when the frame the pipe fetches is the console's own: past it, still in the
 * carve-out (0x2300000 long), so that neither the copy nor the console's next line can land on the other. */
#define HUNG_REPORT_ALT         0x1000000u

#define HUNG_REG(pa)            (*(volatile uint32_t *)(uintptr_t)(pa))

static const uint32_t hung_sspp_off[8] = { 0x4000, 0x6000, 0x8000, 0xa000, 0x24000, 0x26000, 0x28000, 0x2a000 };
static const uint32_t hung_sspp_flush[8] = { 1u << 0, 1u << 1, 1u << 2, 1u << 18, 1u << 11, 1u << 12, 1u << 24, 1u << 25 };
static const uint32_t hung_lm_flush[6] = { 1u << 6, 1u << 7, 1u << 8, 1u << 9, 1u << 10, 1u << 20 };

/*
 * Where a CTL stages each pipe's first rect, from the kernel's own table (dpu_hw_ctl.c's blendstage
 * setup, ctl_blend_config[]): VIG0..3 and DMA0..1 get a 3-bit stage field in CTL_LAYER plus one overflow
 * bit in CTL_LAYER_EXT; DMA2..3 get a 4-bit field in CTL_LAYER_EXT2. The table is in the same order as
 * hung_sspp_off[] (dpu_6_0_sm8250's sspp list).
 */
static const struct {
	uint8_t ext2;   /* 1: the field is in CTL_LAYER_EXT2 (4 bits); 0: CTL_LAYER (3 bits) + EXT bit */
	uint8_t shift;  /* the field's position in that register */
	uint8_t ext;    /* for ext2 == 0: the overflow bit's position in CTL_LAYER_EXT */
} hung_stage[8] = {
	{ 0, 0, 0 },    /* VIG0 */
	{ 0, 3, 2 },    /* VIG1 */
	{ 0, 6, 4 },    /* VIG2 */
	{ 0, 26, 6 },   /* VIG3 */
	{ 0, 18, 16 },  /* DMA0 */
	{ 0, 21, 18 },  /* DMA1 */
	{ 1, 0, 0 },    /* DMA2 */
	{ 1, 4, 0 },    /* DMA3 */
};

/*
 * Whether the CTL has the pipe mixed in on any of its layer mixers. A DPU pipe has no enable bit of its
 * own -- being staged in a mixer of an active CTL is what makes it fetch -- so this is the "the pipe is
 * enabled" test: a pipe whose address register points at a carve-out frame but that this CTL does not
 * stage is some other composer's pipe, and committing to it would change nothing the panel shows.
 */
static int
hung_pipe_staged(uint32_t ctl, unsigned p)
{
	for (unsigned lm = 0; lm < 6; lm++) {
		if (hung_stage[p].ext2) {
			if ((HUNG_REG(ctl + HUNG_CTL_LAYER_EXT2(lm)) >> hung_stage[p].shift) & 0xfu) {
				return 1;
			}
		} else if (((HUNG_REG(ctl + HUNG_CTL_LAYER(lm)) >> hung_stage[p].shift) & 7u) ||
		    (HUNG_REG(ctl + HUNG_CTL_LAYER_EXT(lm)) & (1u << hung_stage[p].ext))) {
			return 1;
		}
	}
	return 0;
}

/*
 * Whether the display's registers can be read at all: its power switch on and its bus clocks running. The
 * clock controllers themselves answer (DISPCC only once GCC's clock for it runs, hence the order). The
 * first three are exactly the kernel's regs_alive(); the last one is extra, and needed: those three only
 * prove the bus clock and the power switch, which can be on while the DPU's own core clock is gated (the
 * kernel asks for DISP_CC_MDSS_MDP_CLK separately), and then the first MDP register read hangs the bus
 * for good -- on umi the watchdog is not armed, so that means a long press.
 */
static int
hung_display_alive(void)
{
	uint32_t v = HUNG_REG(HUNG_GCC_DISP_AHB_CBCR);
	if (v == 0xffffffffu || !(v & 1u) || (v & 0x80000000u)) {
		return 0;
	}
	v = HUNG_REG(HUNG_DISPCC_GDSCR);
	if (v == 0xffffffffu || !(v & 0x80000000u)) {
		return 0;
	}
	v = HUNG_REG(HUNG_DISPCC_AHB_CBCR);
	if (v == 0xffffffffu || !(v & 1u) || (v & 0x80000000u)) {
		return 0;
	}
	v = HUNG_REG(HUNG_DISPCC_MDP_CBCR);
	return v != 0xffffffffu && (v & 1u) && !(v & 0x80000000u);
}

/* What the DPU is fetching, read off its registers. Nothing is written until hung_scanout_commit(). */
struct hung_scanout {
	uint32_t ctl;                   /* the CTL that drives DSI0, absolute */
	uint32_t pipe;                  /* the SSPP that fetches the frame, absolute */
	uint32_t addr_reg[4];           /* its SRC0..SRC3 address registers */
	uint32_t addr[4];               /* ... and what they hold now (the frame it fetches) */
	unsigned addrs;
	uint32_t stride;                /* SRC_YSTRIDE0: the row stride it fetches with */
	uint32_t ystride;               /* ... and the whole register, so a commit leaves its other bits alone */
	uint32_t width, height;         /* SRC_SIZE: the frame as it reads it (left as it is) */
	uint32_t dst;                   /* the frame the report is staged in (not the console's own) */
	uint32_t dst_stride;            /* ... at the width it is drawn with, no padding */
	int copy;                       /* 0: the pipe already fetches the console's frame as it is drawn */
	uint32_t flush;                 /* the CTL_FLUSH value that commits a frame */
};

/* 1 when a report of `bytes` fits in the carve-out at `at` -- aligned, inside, and not on the console's
 * own frame (whose rows the console keeps writing as the report goes on, and which the copy reads). */
static int
hung_report_frame_ok(uint64_t at, uint64_t bytes)
{
	return at >= HUNG_SPLASH_PHYS && at + bytes <= HUNG_SPLASH_PHYS + HUNG_SPLASH_SIZE && (at & 63u) == 0 &&
	    (at + bytes <= SCREEN.base || at >= SCREEN.base + (uint64_t)SCREEN.stride * SCREEN.height);
}

/* 1 when the scanout is one frame of the carve-out that a 32-bit RGBA copy of the console fits; 0 (and
 * nothing touched) otherwise, with *why saying what was not as expected. */
static int
hung_scanout_probe(struct hung_scanout *s, const char **why)
{
	unsigned pipe = 0, pipes = 0, ignored = 0;
	uint32_t size;
	uint64_t bytes;

	memset(s, 0, sizeof(*s));
	*why = "not the phone's panel";
	if (PLAT != &platform_umi || SCREEN.base != HUNG_SPLASH_PHYS || SCREEN.width != 1080 ||
	    SCREEN.height != 2340 || SCREEN.stride < SCREEN.width * 4) {
		return 0;
	}
	*why = "the display's power or bus clocks are off";
	if (!hung_display_alive()) {
		return 0;
	}
	/*
	 * One constant of the block itself, before any CTL/SSPP read: on this generation the MDSS version
	 * reads 0x6xxxxxxx (DPU 6.0), so anything else -- all ones from a bus that did not answer, zero from
	 * a block that did not come up -- means the clock checks above were not enough and the reads below
	 * could hang the bus.
	 */
	*why = "the display's core does not answer with its version";
	size = HUNG_REG(HUNG_MDSS_HW_VERSION);
	if (size == 0xffffffffu || (size >> 28) != 6u) {
		return 0;
	}
	/* (from here on the display's registers answer: every read below follows the checks above) */
	for (uint32_t n = 0; n < 6; n++) {
		uint32_t ctl = (uint32_t)HUNG_MDP_PHYS + 0x1000u + 0x200u * n;
		if (HUNG_REG(ctl + HUNG_CTL_INTF_ACTIVE) & HUNG_CTL_INTF_1) {
			s->ctl = ctl;
			break;
		}
	}
	*why = "no CTL drives the panel";
	if (s->ctl == 0) {
		return 0;
	}
	for (uint32_t p = 0; p < 8; p++) {
		uint32_t found = 0, reg = 0, at = 0;
		for (uint32_t k = 0; k < 4; k++) {
			uint32_t r = (uint32_t)HUNG_MDP_PHYS + hung_sspp_off[p] + HUNG_SSPP_SRC_ADDR + 4u * k;
			uint32_t a = HUNG_REG(r);
			if (a < HUNG_SPLASH_PHYS || a >= HUNG_SPLASH_PHYS + HUNG_SPLASH_SIZE) {
				continue;
			}
			if (found) {
				*why = "more than one pipe, or one pipe in pieces, fetches the carve-out";
				return 0;
			}
			found = 1;
			reg = r;
			at = a;
		}
		if (!found) {
			continue;
		}
		if (!hung_pipe_staged(s->ctl, p)) {
			ignored = 1;
			continue;
		}
		if (pipes) {
			*why = "more than one pipe, or one pipe in pieces, fetches the carve-out";
			return 0;
		}
		pipes = 1;
		pipe = p;
		s->pipe = (uint32_t)HUNG_MDP_PHYS + hung_sspp_off[p];
		s->addr_reg[0] = reg;
		s->addr[0] = at;
		s->addrs = 1;
	}
	*why = ignored ? "the pipe that fetches the carve-out is not staged on the CTL driving the panel" :
	    "no single pipe fetches a frame of the splash carve-out";
	if (pipes != 1 || s->addrs != 1) {
		return 0;
	}
	size = HUNG_REG(s->pipe + HUNG_SSPP_SRC_SIZE);
	s->width = size & 0xffffu;
	s->height = size >> 16;
	s->ystride = HUNG_REG(s->pipe + HUNG_SSPP_SRC_YSTRIDE0);
	s->stride = s->ystride & 0xffffu;
	*why = "the frame's pixels are not a 32-bit RGBA image";
	if (HUNG_REG(s->pipe + HUNG_SSPP_SRC_FORMAT) != HUNG_FMT_RGBA8888) {
		return 0;
	}
	*why = "the frame's geometry is not one the report fits";
	if (s->width < SCREEN.width || s->height < SCREEN.height || s->width > 4096 || s->height > 4096 ||
	    s->width * 4u > HUNG_ROW_MAX) {
		return 0;
	}
	/*
	 * The report is staged at the width it is drawn with, no padding, so the stride it is committed with
	 * is the console's own row length: the panel then shows the console's rows as they are, whatever
	 * padded stride (and whatever CoreAnimation surface) the kernel last left on this pipe -- a surface
	 * there carries its own row stride, and copying at that stride is what smeared the report of the
	 * v29u hang across the screen. A frame that overlaps the console's own is skipped for one past it:
	 * the copy must not write over the console's frame, or the next stage would read a moved picture.
	 */
	s->dst_stride = s->width * 4u;
	bytes = (uint64_t)s->dst_stride * s->height;
	s->copy = 1;
	if (s->addr[0] == SCREEN.base && s->stride == SCREEN.stride && s->width == SCREEN.width &&
	    s->height == SCREEN.height) {
		/* (the panel already fetches the console's frame as the console draws it: nothing to copy) */
		s->dst = s->addr[0];
		s->dst_stride = s->stride;
		s->copy = 0;
	} else {
		s->dst = s->addr[0];
		*why = "no frame apart from the console's own can hold the report";
		if (!hung_report_frame_ok(s->addr[0], bytes)) {
			if (!hung_report_frame_ok(SCREEN.base + HUNG_REPORT_ALT, bytes)) {
				return 0;
			}
			s->dst = (uint32_t)(SCREEN.base + HUNG_REPORT_ALT);
		}
	}
	s->flush = hung_sspp_flush[pipe];
	for (uint32_t lm = 0; lm < 6; lm++) {
		uint32_t staged = (HUNG_REG(s->ctl + HUNG_CTL_LAYER(lm)) & ~HUNG_CTL_MIXER_BORDER_OUT) |
		    HUNG_REG(s->ctl + HUNG_CTL_LAYER_EXT(lm)) | HUNG_REG(s->ctl + HUNG_CTL_LAYER_EXT2(lm)) |
		    HUNG_REG(s->ctl + HUNG_CTL_LAYER_EXT3(lm));
		if (staged) {
			s->flush |= hung_lm_flush[lm];
		}
	}
	s->flush |= HUNG_CTL_FLUSH_CTL;
	return 1;
}

/*
 * Copy the firmware's picture -- the console's report, SCREEN.width wide, at SCREEN's stride -- into the
 * report frame at the frame's own width (dst_stride = width * 4, the stride it is committed with):
 * centred, the rest of every row black so that no half of an old screen reads as part of the report. The
 * destination never overlaps the console's frame (the probe chooses one that does not), so the copy is a
 * plain top-down walk and leaves its source intact for the next stage. The DPU reads the point of
 * coherency, not this core's cache, so the console's pixels are written back before they are copied and
 * the copy is cleaned after it is written.
 */
static void
hung_screen_push(const struct hung_scanout *s)
{
	static uint8_t row[HUNG_ROW_MAX];
	const uint64_t src = SCREEN.base, src_bytes = (uint64_t)SCREEN.stride * SCREEN.height;
	const uint32_t src_row = SCREEN.width * 4u;
	const uint32_t xoff = (s->width > SCREEN.width) ? ((s->width - SCREEN.width) / 2u) & ~3u : 0;
	const uint32_t yoff = (s->height > SCREEN.height) ? (s->height - SCREEN.height) / 2u : 0;

	flush_dcache(src, src + src_bytes);
	for (uint32_t r = 0; r < s->height; r++) {
		uint8_t *dst = (uint8_t *)(uintptr_t)(s->dst + (uint64_t)r * s->dst_stride);
		if (r < yoff || r - yoff >= SCREEN.height) {
			memset(dst, 0, s->dst_stride);
			continue;
		}
		memcpy(row + xoff * 4u, (const void *)(uintptr_t)(src + (uint64_t)(r - yoff) * SCREEN.stride), src_row);
		memset(row, 0, xoff * 4u);
		if (s->dst_stride > xoff * 4u + src_row) {
			memset(row + xoff * 4u + src_row, 0, s->dst_stride - xoff * 4u - src_row);
		}
		memcpy(dst, row, s->dst_stride);
	}
	clean_dcache(s->dst, s->dst + (uint64_t)s->dst_stride * s->height);
}

/*
 * One commit: the report frame's address and the console's row length, written so that they are staged,
 * and one CTL_FLUSH, which the next autorefresh frame start latches (what the kernel's flip does). By
 * default nothing is read from the DPU afterwards -- a power collapse between here and the reset would
 * hang the bus on a read -- and the wait is one frame, counted, not a status bit. The wait is bounded by
 * an iteration count as well as by CNTVCT, so a counter that stopped cannot trap the loader here.
 * With "hung_e2=1" (hung_e2_requested) the flush is read back once after that frame: a commit whose
 * flush bit is still set did not reach the DPU, which a photograph of a screen still showing no report
 * cannot distinguish from a DPU that latched it and stopped fetching. It is the same opt-in as the rest
 * of the MMIO reads, and the printed line is the only thing it adds.
 */
static void
hung_scanout_commit(const struct hung_scanout *s)
{
	uint64_t began;

	for (unsigned i = 0; i < s->addrs; i++) {
		HUNG_REG(s->addr_reg[i]) = s->dst;
	}
	HUNG_REG(s->pipe + HUNG_SSPP_SRC_YSTRIDE0) = (s->ystride & 0xffff0000u) | s->dst_stride;
	__asm__ volatile ("dsb sy" ::: "memory");
	HUNG_REG(s->ctl + HUNG_CTL_FLUSH) = s->flush;
	began = READ_SYSREG(CNTVCT_EL0);
	for (uint64_t n = 0; n < HUNG_FRAME_SPINS; n++) {
		if (READ_SYSREG(CNTVCT_EL0) - began >= READ_SYSREG(CNTFRQ_EL0) / 30u) {
			break;
		}
	}
	if (hung_e2_requested) {
		uint32_t flushed = HUNG_REG(s->ctl + HUNG_CTL_FLUSH);

		print("screen: CTL_FLUSH %x read back as %x: %s\n", (uint64_t)s->flush, (uint64_t)flushed,
		    (flushed & HUNG_CTL_FLUSH_CTL) ? "not cleared, the DPU did not latch the commit" :
		    "cleared, the DPU took the commit");
	}
}

/*
 * Called after each stage of the report (report_hung_kernel(): the hang line first, then the ring and
 * the CPU lines, then the state dump if it was asked for, and once at the end before the hold) so that
 * whatever stage stops answering leaves the stages before it on the panel: it keeps no state between
 * calls and can be called again. Its own words go into the loader's log and not onto the screen: what is
 * on the screen is the report itself.
 */
static void
hung_screen_copy_report(void)
{
	int was_quiet = screen_quiet, was_forced = screen_forced;
	struct hung_scanout s;
	const char *why = "no reason";

	screen_quiet = 1;
	screen_forced = 0;
	if (!hung_scanout_probe(&s, &why)) {
		print("screen: %s; the report stays in the firmware's frame\n", why);
		goto out;
	}
	if (!s.copy) {
		clean_dcache(SCREEN.base, SCREEN.base + (uint64_t)SCREEN.stride * SCREEN.height);
		hung_scanout_commit(&s);
		print("screen: the panel fetches the firmware's frame itself; the report was committed to it (%x)\n",
		    (uint64_t)s.dst);
		goto out;
	}
	hung_screen_push(&s);
	hung_scanout_commit(&s);
	print("screen: the report was copied into %x (%d x %d, %d bytes a row; the pipe fetched %x at %d) and "
	    "committed to it\n", (uint64_t)s.dst, (uint64_t)s.width, (uint64_t)s.height, (uint64_t)s.dst_stride,
	    (uint64_t)s.addr[0], (uint64_t)s.stride);
out:
	screen_quiet = was_quiet;
	screen_forced = was_forced;
}

/*
 * Before the report is drawn, once, while the loader's own log is not being written to the screen: the
 * console's frame is the first frame of the splash carve-out, and the kernel draws into that same frame
 * (fb_dpu's frames start at the GOP frame), so what is under the console's text is the picture the
 * kernel died on -- at the kernel's row stride, which is not the console's. Clear the whole frame and the
 * console with it, so that the panel gets the report on black and nothing of that picture survives beside
 * it, and set the console to white on black: a photograph of the panel at arm's length is all a person
 * has of this.
 */
static void
hung_screen_begin_report(void)
{
	uint64_t bytes = (uint64_t)SCREEN.stride * SCREEN.height;

	if (PLAT != &platform_umi || SCREEN.base != HUNG_SPLASH_PHYS || bytes == 0) {
		return;
	}
	if (ST->con_out != NULL) {
		if (ST->con_out->set_attribute != NULL) {
			(void)ST->con_out->set_attribute(ST->con_out, EFI_ATTR(EfiBlack, EfiWhite));
		}
		if (ST->con_out->clear_screen != NULL) {
			(void)ST->con_out->clear_screen(ST->con_out);
		}
	}
	memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)bytes);
}

static uint64_t win_base, win_size; /* XNU's physical memory window, claimed once */
static uint64_t resident_rd_size;   /* root image kept at win_base + RESIDENT_RD_OFFSET, 0 if none */
static char last_result[64] = "idle";

static void
set_result(const char *what, uint64_t a, uint64_t b)
{
	uint32_t saved = log_len;
	uint32_t start = log_len;
	print("%s %d %x", what, a, b);
	uint32_t n = log_len - start;
	if (n > sizeof(last_result) - 1) {
		n = sizeof(last_result) - 1;
	}
	memcpy(last_result, log_buf + start, n);
	last_result[n] = 0;
	log_len = saved;
}

/* Every range of the firmware's memory map of 1 MB or more, and the total of each type. */
static void
print_memory_map(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, totals[16];
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return;
	}
	memset(totals, 0, sizeof(totals));
	print("memory map (type 7 = free, 1/2 = loader, 3/4 = boot services, 5/6 = runtime, 0 = reserved):\n");
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (d->type < 16) {
			totals[d->type] += d->pages << 12;
		}
		if (d->pages >= 256) {
			print("  %x - %x  type %d  %d MB\n", d->phys_start, d->phys_start + (d->pages << 12), (uint64_t)d->type,
			    d->pages >> 8);
		}
	}
	for (int t = 0; t < 16; t++) {
		if (totals[t] != 0) {
			print("  total of type %d: %d MB\n", (uint64_t)t, totals[t] >> 20);
		}
	}
}

static int starts_with(const char *s, const char *prefix);

#ifdef HAKIOS_LOADER_PROD
/* ABI1: paired with umi_loader_prod.inc. No command-line space is consumed. */
#define UMI_PROD_MAGIC 0x31444f5250494d55ULL /* "UMIPROD1" */
#define UMI_PROD_SLOT  0x50524f4400000001ULL
#endif

/* UMI boot-argument gate: keep device-specific tests explicit and protect
 * the unchanged prebuilt kernel from Redwood MMIO. */
static int
umi_bootargs_ok(const struct payload_info *pi)
{
	static const char *const allowed[] = {
		"umi_modem=1", "umi_modem=2", "umi_modem_probe=1", /* external X55 only; default off (2: CommCenter bridge, /baseband) */
		"umi_vops=", /* NAS 0x4E/0x4D TLV 0x21 -> 1: report LTE VoPS to CommCenter (default off, dev-only) */
		"umi_rtpok=", /* 1: answer the IMSRTP 0x28 session lifecycle, so the MO INVITE goes out (default off; no audio) */
		"hung_e2=", /* 1: read GIC/PCIe/SMMU state in a hang report (default off: those reads can hang the bus) */
		"-v", "debug=", "keepsyms=", "serial=", "cpus=", "bootcpu=",
		"loader_return_after=", "epoch=", "sb_delay=", "systrace=", "autounlock=0", "umi_touch=", "umi_keys=", "umifts_scan=",
		/* Narrowly allow the existing frame-latency diagnostics on UMI. */
		"touch_lat=1", "cpuload=1000", "thermal=2",
		"torch=1", "torch=2", "torch=3", "cam=1", "cam=2", "cam=3", "cam=4", "cam=5", "cam=6", "cam=7", "cam_fake=1",
		"umi_haptic=", "haptic_effect=", "haptic_gain=", "adsp=", "sound=", "adsp_umi=", "adsp_step=", "adsp_return=", "umi_amp_probe=", "umi_mic=1", "umi_mic=2", "umi_mic=3", "umi_mic=4",
		"gpu=", "gpu_fc=1", "gpu_exact=", "metal=", "metal_all=1", "metal_fps=", "metal_defer=", "metal_sync=", "metal_shadow_refresh=", "metal_nocopy_refresh=", "metal_nospec", "metal_big=1", "metal_m2v=", "metal_gmem_draws=", "metal_gmem_off=", "metal_pass_wfi=", "metal_rs=1", "ca_metallib=", "gpu_recover=", "anim_max=", "cpufreq=", "cpugov=1", "cpugov_log=1", "amp=1", "al=1", "bl=1", "fb_scan=", "fb_filter=", "fb_threads=", "fb_hz=", "fb_measure=", "fb_beam=", "fbscale=", "fb_diff=1", "fb_dirty=1", "fb_dpu=1", "fb_dpu=2", "fb_dpu=3", "fb_dpu=4", "fb_stats=1", "surf_clear=0", "wallpaper_freeze=", "touchscreen=", "touch_poll_us=", "adreno=", "adreno_pause=", "adreno_fine=", "adreno_return=",
		"umi_wifi_step=", "umi_wifi_return=", "wifi_mac=", "wifi=1", "wifi_scan=1", "umi_bt_probe=", "umi_bt=",
		"battery=1", "umi_charge_diag=1",
		"idle=0", "idle=1", "screenoff_pm=0", "screenoff_pm=1", "screenoff_cpu_mhz=", "screenoff_gpu=0", "screenoff_gpu=1", "screenoff_wifi=0", "screenoff_wifi=1",
		"ufs=1", "ufs=2", "ufs_write=0", "ufs_write=1", "ufs_native=0", "usb=1", "usb_ncm=1", /* no var_store=1: its raw slots overlap the HFS data volume on darwin */
		/* darwin flash root (devices/xiaomi-mi-10/flash/FLASH-umi.md): read check, HFS /var, Apple root */
		"apfs_check=", "apfs_dev=", "apfs_mnt=", "apfs_subtree=", "apfs_max=", "var_hfs=", "root_apfs=",
		"glcopy=1",
		"gltargetcopy=1",
		"glfix=0", "glfix=1", /* purple diagnostics; optional framebuffer-fetch serialization */
		"sbx=0", "sbx=1", /* kernel: 0 nobody, 1 daemons only reported sandboxed (devices/xiaomi-mi-10/SANDBOX-umi.md) */
		"location=0", /* launchd leaves locationd out: /hakios-no-location (devices/xiaomi-mi-10/LOCATION-umi.md) */
	};
	int adsp_umi = 0, adsp_step = 0, adsp_return = 0, umi_amp_probe = 0, adsp_on = 0, sound = 0;
	int gpu = 0, metal = 0, cpufreq = 0, cpugov = 0, cpugov_log = 0, amp = 0, fb_scan = 0, fb_filter = 0, fb_threads = 0, fbscale = 0, fb_diff = 0, fb_dirty = 0, adreno = 0, adreno_fine = 0, adreno_return = 0;
	int umi_wifi_step = 0, umi_wifi_return = 0, wifi_mac = 0, wifi_on = 0;
	if (PLAT != &platform_umi) return 1;
	if (pi->forced_base != 0) {
		print("UMI: forced physical memory windows are disabled\n");
		return 0;
	}
	for (const char *c = pi->cmdline; *c;) {
		while (*c == ' ') c++;
		if (!*c) break;
		const char *end = c;
		while (*end && *end != ' ') end++;
#ifdef HAKIOS_LOADER_PROD
		/* Reject debug timeouts/USB ownership rather than silently honouring them. */
		if (starts_with(c, "loader_return_after=") || starts_with(c, "usb=") ||
		    starts_with(c, "usb_ncm=")) {
			print("production: timeout and USB debug arguments are disabled\n");
			return 0;
		}
#endif
		int ok = 0;
		for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
			uint64_t len = str_len(allowed[i]);
			if (starts_with(c, allowed[i]) &&
			    (allowed[i][len - 1] == '=' || c + len == end)) ok = 1;
		}
		if (starts_with(c, "adsp=")) {
			if (adsp_on || end - c != 6 || c[5] != '1') ok = 0;
			adsp_on = 1;
		} else if (starts_with(c, "sound=")) {
			if (sound || end - c != 7 || c[6] < '1' || c[6] > '4') ok = 0;
			sound = 1;
		} else if (starts_with(c, "adsp_umi=")) {
			if (adsp_umi || end - c != 10 || c[9] != '1') ok = 0;
			adsp_umi = 1;
		} else if (starts_with(c, "adsp_step=")) {
			uint32_t n = 0;
			if (adsp_step || end - c <= 10) ok = 0;
			for (const char *v = c + 10; v < end && ok; v++) {
				if (*v < '0' || *v > '9' || n > 11) {
					ok = 0;
					break;
				}
				n = n * 10 + (uint32_t)(*v - '0');
			}
			if (n == 0 || n > 11) ok = 0;
			adsp_step = 1;
		} else if (starts_with(c, "adsp_return=")) {
			if (adsp_return || end - c != 13 || c[12] != '1') ok = 0;
			adsp_return = 1;
		} else if (starts_with(c, "umi_bt=")) {
			if (end - c != 8 || c[7] != '1') ok = 0;
		} else if (starts_with(c, "umi_bt_probe=")) {
			if (end - c != 14 || c[13] < '1' || c[13] > '5') ok = 0;
		} else if (starts_with(c, "umi_amp_probe=")) {
			if (umi_amp_probe || end - c != 15 || c[14] < '1' || c[14] > '5') ok = 0;
			umi_amp_probe = 1;
		} else if (starts_with(c, "umi_wifi_step=")) {
			uint32_t n = 0;
			if (umi_wifi_step || end - c <= 14) ok = 0;
			for (const char *v = c + 14; v < end && ok; v++) {
				if (*v < '0' || *v > '9' || n > 10) { ok = 0; break; }
				n = n * 10 + (uint32_t)(*v - '0');
			}
			if (n == 0 || n > 10) ok = 0;
			umi_wifi_step = 1;
		} else if (starts_with(c, "umi_wifi_return=")) {
			if (umi_wifi_return || end - c != 17 || c[16] != '1') ok = 0;
			umi_wifi_return = 1;
		} else if (starts_with(c, "wifi=")) {
			if (wifi_on || end - c != 6 || c[5] != '1') ok = 0;
			wifi_on = 1;
		} else if (starts_with(c, "wifi_mac=")) {
			if (wifi_mac || end - c != 26) ok = 0;
			for (unsigned i = 0; i < 17 && ok; i++) {
				char h = c[9 + i];
				if (i % 3 == 2 ? h != ':' : !((h >= '0' && h <= '9') ||
				    (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) ok = 0;
			}
			wifi_mac = 1;
		} else if (starts_with(c, "gpu=")) {
			if (gpu || end - c != 5 || c[4] != '1') ok = 0;
			gpu = 1;
		} else if (starts_with(c, "metal=")) {
			if (metal || end - c != 7 || c[6] < '1' || c[6] > '3') ok = 0;
			metal = 1;
		} else if (starts_with(c, "cpufreq=")) {
			if (cpufreq || end - c != 9 || c[8] != '1') ok = 0;
			cpufreq = 1;
		} else if (starts_with(c, "cpugov=")) {
			if (cpugov || end - c != 8 || c[7] != '1') ok = 0;
			cpugov = 1;
		} else if (starts_with(c, "cpugov_log=")) {
			if (cpugov_log || end - c != 12 || c[11] != '1') ok = 0;
			cpugov_log = 1;
		} else if (starts_with(c, "amp=")) {
			if (amp || end - c != 5 || c[4] != '1') ok = 0;
			amp = 1;
		} else if (starts_with(c, "fb_scan=")) {
			if (fb_scan || end - c != 9 || c[8] != '1') ok = 0;
			fb_scan = 1;
		} else if (starts_with(c, "fb_filter=")) {
			if (fb_filter || end - c != 11 || (c[10] != '0' && c[10] != '1')) ok = 0;
			fb_filter = 1;
		} else if (starts_with(c, "fb_threads=")) {
			uint32_t n = 0;
			if (fb_threads || end - c <= 11) ok = 0;
			for (const char *v = c + 11; v < end && ok; v++) {
				if (*v < '0' || *v > '9' || n > 8) {
					ok = 0;
					break;
				}
				n = n * 10 + (uint32_t)(*v - '0');
			}
			if (n == 0 || n > 8) ok = 0;
			fb_threads = 1;
		} else if (starts_with(c, "fbscale=")) {
			if (fbscale || end - c != 9 || c[8] != '2') ok = 0;
			fbscale = 1;
		} else if (starts_with(c, "fb_diff=")) {
			if (fb_diff || end - c != 9 || c[8] != '1') ok = 0;
			fb_diff = 1;
		} else if (starts_with(c, "fb_dirty=")) {
			if (fb_dirty || end - c != 10 || c[9] != '1') ok = 0;
			fb_dirty = 1;
		} else if (starts_with(c, "adreno=")) {
			uint32_t n = 0;
			if (adreno || end - c <= 7) ok = 0;
			for (const char *v = c + 7; v < end && ok; v++) {
				if (*v < '0' || *v > '9' || n > 22) {
					ok = 0;
					break;
				}
				n = n * 10 + (uint32_t)(*v - '0');
			}
			if (n == 0 || n > 22) ok = 0;
			adreno = 1;
		} else if (starts_with(c, "adreno_pause=")) {
			uint32_t n = 0;
			if (end - c <= 13) ok = 0;
			for (const char *v = c + 13; v < end && ok; v++) {
				if (*v < '0' || *v > '9' || n > 5000) {
					ok = 0;
					break;
				}
				n = n * 10 + (uint32_t)(*v - '0');
			}
			if (n > 5000) ok = 0;
		} else if (starts_with(c, "adreno_fine=")) {
			if (adreno_fine || end - c != 13 || c[12] != '1') ok = 0;
			adreno_fine = 1;
		} else if (starts_with(c, "adreno_return=")) {
			if (adreno_return || end - c != 15 || c[14] != '1') ok = 0;
			adreno_return = 1;
		}
		if (!ok) {
			print("UMI: unsupported boot argument at: %s\n", c);
			return 0;
		}
		c = end;
	}
	if ((adsp_on && !adsp_umi) || (sound && (!adsp_on || !adsp_umi)) || (adsp_on && adsp_step)) {
		print("UMI: audio tests require adsp=1 adsp_umi=1, without diagnostic steps\n");
		return 0;
	}
	if (adsp_step && (!adsp_umi || !adsp_return)) {
		print("UMI: ADSP diagnostics require adsp_umi=1 and adsp_return=1\n");
		return 0;
	}
	if (adreno && !adreno_return) {
		print("UMI: early GPU diagnostics require adreno_return=1\n");
		return 0;
	}
	if (gpu && adreno) {
		print("UMI: choose gpu=1 for iOS or adreno=N for a diagnostic\n");
		return 0;
	}
	if (metal && !gpu) {
		print("UMI: metal requires gpu=1\n");
		return 0;
	}
	if (cpugov && !cpufreq) {
		print("UMI: cpugov requires cpufreq=1\n");
		return 0;
	}
	if (cpugov_log && !cpugov) {
		print("UMI: cpugov_log requires cpugov=1\n");
		return 0;
	}
	if (fb_dirty && (!fb_scan || !fb_diff)) {
		print("UMI: fb_dirty requires fb_scan=1 and fb_diff=1\n");
		return 0;
	}
	if (umi_wifi_step != umi_wifi_return || (umi_wifi_step && wifi_on)) {
		print("UMI: Wi-Fi diagnostics require umi_wifi_step=1..10 umi_wifi_return=1\n");
		return 0;
	}
	return 1;
}

static int
choose_window(const struct payload_info *pi)
{
	if (!umi_bootargs_ok(pi)) return 0;
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size;
	uint32_t desc_ver;
	uint64_t best_base = 0, best_size = 0;
	uint64_t pay_lo = PAYLOAD_BASE, pay_hi = PAYLOAD_BASE + PAYLOAD_MAX_SIZE;

	if (win_size != 0) {
		return 1;
	}
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return 0;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		if (d->type != EfiConventionalMemory) {
			continue;
		}
		uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);
		/* the payload sits in RAM that UEFI may consider free: stay clear of it */
		if (lo < pay_hi && hi > pay_lo) {
			if (pay_lo - lo > hi - pay_hi && pay_lo > lo) {
				hi = pay_lo;
			} else {
				lo = pay_hi;
			}
		}
		lo = (lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
		hi &= ~(BASE_ALIGN - 1);
		if (hi > lo && hi - lo > best_size) {
			best_base = lo;
			best_size = hi - lo;
		}
	}
	print("largest free block %x size %x\n", best_base, best_size);
	if (pi->forced_base != 0) {
		best_base = pi->forced_base;
		best_size = MAX_MEM_SIZE;
	}
	if (best_size > MAX_MEM_SIZE) {
		best_size = MAX_MEM_SIZE;
	}
	/* UEFI stays alive while the kernel runs, so the kernel must own all of its window. */
	uint64_t claim = best_base;
	efi_status s = BS->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &claim);
	print("phys window %x size %x, claim status %x\n", best_base, best_size, s);
	if (EFI_ERROR(s)) {
		return 0;
	}
	win_base = best_base;
	win_size = best_size;
	return 1;
}

/*
 * Load and run the kernel from the payload. Returns when the kernel comes
 * back (panic, checkpoint, early exception); the combined log is staged and
 * written to the log partition. Returns 0 if the kernel could not be started.
 */
static int
run_kernel(int have_checkpoint, uint64_t checkpoint)
{
	static uint8_t map_buf[65536];
	uint64_t map_size, map_key, desc_size;
	uint32_t desc_ver;
	struct payload_info pi;

	if (!parse_payload(&pi, 1) || pi.kernel == NULL) {
		print("no kernel in payload\n");
		return 0;
	}
	if (!have_checkpoint) {
		checkpoint = pi.checkpoint;
	}
	const uint8_t *kernel = pi.kernel;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	if (pi.kernel_size < sizeof(*mh) || mh->magic != MH_MAGIC_64) {
		print("kernel is not a 64-bit Mach-O\n");
		return 0;
	}
	/* the load commands must lie inside the kernel entry, or the walk below reads whatever follows it */
	if (mh->sizeofcmds > pi.kernel_size - sizeof(*mh)) {
		print("kernel: the load commands are longer than the kernel\n");
		return 0;
	}

	/* --- kernel extent -------------------------------------------------- */
	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		/*
		 * Every command must fit in the commands the header counts, and be at least its own header long: a
		 * cmdsize of 0 would keep the walk on one command for up to 2^32 rounds, a huge one would leave the kernel.
		 */
		uint64_t at = (uint64_t)((const uint8_t *)lc - (const uint8_t *)(mh + 1));

		if (at > mh->sizeofcmds || sizeof(*lc) > mh->sizeofcmds - at || lc->cmdsize < sizeof(*lc) ||
		    lc->cmdsize > mh->sizeofcmds - at) {
			print("kernel: load command %d does not fit in the load commands\n", (uint64_t)i);
			return 0;
		}
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (lc->cmdsize < sizeof(*sg)) {
				print("kernel: segment command %d is too short\n", (uint64_t)i);
				return 0;
			}
			if (sg->vmsize == 0) {
				continue;
			}
			/*
			 * What is loaded must be inside the kernel (file) and inside its own extent: a vmaddr + vmsize that
			 * wraps, a filesize above vmsize, or a file range past the entry would write over, or read from, memory
			 * that is not the kernel's.
			 */
			if (sg->vmaddr + sg->vmsize < sg->vmaddr || sg->filesize > sg->vmsize || sg->fileoff > pi.kernel_size ||
			    sg->filesize > pi.kernel_size - sg->fileoff) {
				print("kernel: segment %d is outside the kernel or wraps\n", (uint64_t)i);
				return 0;
			}
			if (sg->vmaddr < vm_lo) {
				vm_lo = sg->vmaddr;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
		} else if (lc->cmd == LC_UNIXTHREAD && lc->cmdsize >= 16 + 33 * 8) {
			/* cmd, cmdsize, flavor, count, x0..x28, fp, lr, sp, pc (a command too short to hold pc gives no entry) */
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	print("kernel va %x..%x entry %x\n", vm_lo, vm_hi, entry_va);
	/* (a kernel that reaches more than the largest window past the virtual base is no kernel of this loader's) */
	if (vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || entry_va == 0 || vm_hi < vm_lo ||
	    vm_hi - KERNEL_VIRT_BASE > MAX_MEM_SIZE) {
		print("unexpected kernel layout\n");
		return 0;
	}
	if (!choose_window(&pi)) {
		return 0;
	}

	uint64_t phys_base = win_base;
	uint64_t slide = phys_base - KERNEL_VIRT_BASE; /* pa = va + slide */
	uint64_t kern_lo = vm_lo + slide, kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	/* (where a hung kernel's live block may point: report_hung_kernel reads only inside these) */
	kernel_image_lo = kern_lo;
	kernel_image_hi = kern_hi;
	kernel_window_lo = win_base;
	kernel_window_hi = win_base + win_size;
	/* The ramdisk must sit inside the kernel's static mapping, i.e. below topOfKernelData. */
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	int rd_resident = 0;
	/* A resident image next to a payload ramdisk: two disks (which is which is decided where the device tree is written). */
	uint64_t rd1_pa = 0, rd1_size = 0;

	if (pi.resident_rd != 0) {
		/*
		 * The size is the payload's word: one that does not even fit above the resident address would wrap
		 * the page rounding or the device tree address below, and the room check after them would pass.
		 * (Whatever fits is still held to the room check as before.)
		 */
		if (win_size <= RESIDENT_RD_OFFSET || pi.resident_rd > win_size - RESIDENT_RD_OFFSET) {
			print("rdres %x does not fit in the memory window\n", pi.resident_rd);
			return 0;
		}
		resident_rd_size = pi.resident_rd;
	}
	if (pi.resident != NULL && pi.resident_size != 0) {
		uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET, n = pi.resident_size;

		/* the payload holding it must not lie where it goes (in QEMU both are ordinary RAM) */
		if (res_pa < PAYLOAD_BASE + PAYLOAD_MAX_SIZE && PAYLOAD_BASE < res_pa + n) {
			print("the resident image's place overlaps the payload: not booted\n");
			return 0;
		}
		memcpy((void *)res_pa, pi.resident, n);
		clean_dcache(res_pa, res_pa + n);
		resident_rd_size = n;
		print("resident image from the payload: %d bytes at pa %x\n", n, res_pa);
	}
	if (resident_rd_size != 0) {
		uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET;
		uint64_t res_size = (resident_rd_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

		if (kern_hi + rd_size > res_pa) {
			print("kernel and ramdisk overlap the resident image\n");
			return 0;
		}
		if (rd_size == 0) {
			rd_pa = res_pa;
			rd_size = res_size;
			rd_resident = 1;
		} else {
			rd1_pa = res_pa;
			rd1_size = res_size;
		}
	}
	/* everything up to topOfKernelData is the kernel's static region: it must cover both images */
	uint64_t dt_pa = rd1_size != 0 ? rd1_pa + rd1_size : rd_pa + rd_size;
	uint64_t dt_max = 0x10000;
	uint64_t ba_pa = dt_pa + dt_max;
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	if (win_size < top - phys_base + 0x4000000ULL) {
		print("not enough contiguous RAM\n");
		return 0;
	}

	/* --- load segments -------------------------------------------------- */
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		const struct segment_command_64 *sg = (const void *)lc;
		if (lc->cmd != LC_SEGMENT_64 || sg->vmsize == 0) {
			continue;
		}
		memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
		memcpy((void *)(sg->vmaddr + slide), kernel + sg->fileoff, sg->filesize);
	}
	print("kernel loaded at pa %x..%x\n", kern_lo, kern_hi);
	if (rd_resident) {
		print("resident ramdisk at pa %x size %x\n", rd_pa, rd_size);
	} else if (rd_size != 0) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
		print("ramdisk at pa %x size %x\n", rd_pa, rd_size);
	}
	if (rd1_size != 0) {
		print("resident image at pa %x size %x: it is the root (md0), the payload ramdisk is md1\n", rd1_pa, rd1_size);
	}

	/* "bootcpu=N" among the kernel's arguments: run it on core N (see run_on_other_cpu) */
	uint64_t this_mpidr = READ_SYSREG(MPIDR_EL1) & 0xFFFFFF, kernel_mpidr = this_mpidr;
	/* "cpus=N" or "cpumask=M" there too: the kernel may have more cores than the one it starts on */
	int more_cores = 0, battery_requested = 0, ufs_requested = 0;
	/* "torch=N" (not 0) there too: the phone's flashlight is the kernel's (drivers/camera/hakios.torch.c) */
	int torch_requested = 0;
	/* "cam=5" there too: the phone's wide camera is iOS's (drivers/camera/hakios.camera.c) */
	int camera_requested = 0;
	/* "umi_modem=2" there too: iOS's own CommCenter drives the X55 (COMMCENTER-umi.md); launchd loads it only with /baseband */
	int baseband_requested = 0;
	/* "location=0" there too: the kill switch of iOS's locationd (_LimitLoadFromDeviceTree = hakios-no-location) */
	int location_off = 0;
	usb_kernel_requested = 0;
	usb_handoff.request = usb_handoff.ack = 0;
	/* (nor what the last run asked the hang report to read: "hung_e2=1" is per-run) */
	hung_e2_requested = 0;
	/* (the loader stays for run after run: what the last run's arguments said must not hold for this one) */
	gamut_p3 = 0;
	/* a run that does not ask for writes does not get them, whatever an earlier run of this
	 * loader asked for (the loader stays resident across runs) */
	block_write_requested = 0;
	block_write_part = -1;
	/*
	 * the same for the other storage requests (they are only ever OR-ed in below): without this a run
	 * that did not ask for ufs=2 is still offered a stale page and given its ufs_shared_pa
	 */
	hc_requested = 0;
	block_probe_requested = 0;
	block_enabled = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '0' && c[8] <= '7') {
			kernel_mpidr = (uint64_t)(c[8] - '0') << PLAT->core_shift;
		}
		usb_kernel_requested |= starts_with(c, "usb=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[5] == 0 || c[5] == ' ');
		hung_e2_requested |= starts_with(c, "hung_e2=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[9] == 0 || c[9] == ' ');
		more_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == ' ');
		battery_requested |= starts_with(c, "battery=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[9] == 0 || c[9] == ' ');
		torch_requested |= starts_with(c, "torch=") && (c == pi.cmdline || c[-1] == ' ') && c[6] >= '1' && c[6] <= '9';
		camera_requested |= starts_with(c, "cam=5") && (c == pi.cmdline || c[-1] == ' ') && (c[5] == 0 || c[5] == ' ');
		baseband_requested |= starts_with(c, "umi_modem=2") && (c == pi.cmdline || c[-1] == ' ') && (c[11] == 0 || c[11] == ' ');
		location_off |= starts_with(c, "location=0") && (c == pi.cmdline || c[-1] == ' ') && (c[10] == 0 || c[10] == ' ');
		gamut_p3 |= starts_with(c, "gamut=p3") && (c == pi.cmdline || c[-1] == ' ');
		if (starts_with(c, "ufs=") && (c == pi.cmdline || c[-1] == ' ') && c[4] >= '1' && c[4] <= '2' &&
		    (c[5] == 0 || c[5] == ' ')) {
			ufs_requested = c[4] - '0';
		}
		block_probe_requested |= starts_with(c, "ufs_probe=1") && (c == pi.cmdline || c[-1] == ' ');
		block_write_requested |= starts_with(c, "ufs_write=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[11] == 0 || c[11] == ' ');
		hc_requested |= starts_with(c, "ufs_native=") && (c == pi.cmdline || c[-1] == ' ') && c[11] >= '1' &&
		    c[11] <= '9';
	}
	if (battery_requested) {
		charger_start();
	}
	if (ufs_requested >= 2) {
		hc_init();              /* (ufs_native=: UEFI's transfer list kept, before anything reads) */
	}
	umi_display_probe_dump(); // Repeat the DXE history in each supervisor live-run log.
	umi_display_clock_snapshot("before UFS survey");
	if (ufs_requested >= 1) {
		ufs_survey();
	}
	if (ufs_requested >= 2) {
		block_start();
	}
	umi_display_clock_snapshot("after UFS setup");
	if (checkpoint == CHECKPOINT_SELFTEST) {
		kernel_mpidr = this_mpidr;
	}

	/* where the kernel will show its signs of life, and how long it has said it will run */
	const struct kernel_live *kernel_live = NULL;
	uint64_t kernel_budget = 0;
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == KERNEL_LIVE_MAGIC) {
			kernel_live = (const struct kernel_live *)q;
			break;
		}
	}
#ifdef HAKIOS_LOADER_PROD
	int capable = 0, boot7 = 0, cpus7 = 0, return_slot = 0;
	for (uint64_t *q = (uint64_t *)kern_lo; q + 4 <= (uint64_t *)kern_hi; q++) {
		if (q[0] == UMI_PROD_MAGIC && q[1] == 1) capable = 1;
		if (q[0] == 0x545352444C554E58ULL) return_slot = 1;
	}
	for (const char *c = pi.cmdline; *c; c++) {
		if (c != pi.cmdline && c[-1] != ' ') continue;
		if (starts_with(c, "bootcpu=")) {
			if (!starts_with(c, "bootcpu=7") || (c[9] != 0 && c[9] != ' ')) return 0;
			boot7++;
		}
		if (starts_with(c, "cpus=")) {
			if (!starts_with(c, "cpus=7") || (c[6] != 0 && c[6] != ' ')) return 0;
			cpus7++;
		}
	}
	if (!capable || !return_slot || kernel_live == NULL || !wdt_armed || checkpoint != 0 ||
	    kernel_mpidr == this_mpidr || boot7 != 1 || cpus7 != 1) {
		print("production: requires ABI1 kernel/live/slot, watchdog, bootcpu=7 cpus=7, no checkpoint\n");
		return 0;
	}
#else
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "loader_return_after=")) {
			for (c += 20; *c >= '0' && *c <= '9'; c++) {
				kernel_budget = kernel_budget * 10 + (uint64_t)(*c - '0');
			}
			break;
		}
	}

#endif

	if (usb_kernel_requested && (kernel_mpidr == this_mpidr || kernel_budget == 0 || kernel_budget > 3600)) {
		print("usb=1 requires a separate bootcpu and loader_return_after=1..3600; not started\n");
		return 0;
	}

	/* Give the kernel the address of our return stub and the checkpoint to stop at. */
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == 0x545352444C554E58ULL /* "XNULDRST" */) {
#ifdef HAKIOS_LOADER_PROD
			q[1] = 0; /* No return stub, including early-exception assembly. */
			q[2] = UMI_PROD_SLOT;
#else
			q[1] = kernel_mpidr != this_mpidr ? (uint64_t)(uintptr_t)&other_cpu_return_stub :
			    (uint64_t)(uintptr_t)&loader_return_stub;
			q[2] = checkpoint;
#endif
			q[3] = slide;
			print("return slot patched at %x, checkpoint %d\n", (uint64_t)(uintptr_t)q, checkpoint);
			break;
		}
	}

	/* --- boot_args ------------------------------------------------------ */
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	/* any display the firmware has set up: the phone's panel, or QEMU's ramfb */
	if (SCREEN.base != 0) {
		/*
		 * v_display is how iBoot says "graphics mode": the kernel then keeps
		 * its text off the screen. A verbose boot (-v) wants the text.
		 */
		int verbose = 0;
		for (const char *c = pi.cmdline; *c; c++) {
			verbose |= starts_with(c, "-v") && (c[2] == ' ' || c[2] == 0) && (c == pi.cmdline || c[-1] == ' ');
		}
		ba->video.v_baseAddr = SCREEN.base;
		ba->video.v_display = verbose ? 0 : 1;
		ba->video.v_rowBytes = SCREEN.stride;
		ba->video.v_width = SCREEN.width;
		ba->video.v_height = SCREEN.height;
		ba->video.v_depth = 32;
	}
	/*
	 * No "-v" by default: on the phone the verbose console is a text
	 * framebuffer that scrolls 10 MB per line and slows boot to a crawl.
	 * The log comes back through the loader instead. Add -v via the
	 * payload's cmdline entry to watch the boot on screen.
	 */
	/*
	 * serial=3 sends the console to the UART. Where there is none it would
	 * only silence the screen; and a "serial=" in the payload's own
	 * arguments must win, which means ours must not come first.
	 */
	int own_serial = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_serial |= starts_with(c, "serial=");
	}
	const char *base_cmdline = (PLAT->uart != 0 && !own_serial) ? "debug=0x8 serial=3 " : "debug=0x8 ";
	uint32_t n = 0;
	for (const char *c = base_cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}
	/* one processor unless the payload asks for more (of two "cpus=" the kernel takes the first) */
	if (!more_cores) {
		for (const char *c = "cpus=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	/* the root is the first RAM disk unless the payload says otherwise */
	int own_root = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_root |= starts_with(c, "rd=") && (c == pi.cmdline || c[-1] == ' ');
	}
	if (rd_size != 0 && !own_root) {
		for (const char *c = "rd=md0 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
#ifndef HAKIOS_LOADER_PROD
	if (wdt_armed) {
		for (const char *c = "hv_wdt_pet=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
#endif
	if (block_enabled) {
		static const char prefix[] = "ufs_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&block_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	if (block_enabled && df_state > 0 && df_map.magic != 0) {
		/* darwin as a file: where its map is, for the kernel's own UFS driver (darwinfile.c) */
		static const char prefix[] = "darwin_map_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&df_map;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	if (battery_requested) {
		static const char prefix[] = "battery_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&charger_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	/* the version of this kernel, for sysctl kern.hakios.version and the kernel's log (no spaces in it) */
	if (pi.kernel_version != NULL && pi.kernel_version[0] != 0) {
		static const char prefix[] = "hakios_kver=";
		uint32_t before = n, need = (uint32_t)(sizeof(prefix) - 1 + str_len(pi.kernel_version) + 1);

		if (n + need < BOOT_LINE_LENGTH - 1) {
			for (const char *c = prefix; *c; c++) {
				ba->commandLine[n++] = *c;
			}
			for (const char *c = pi.kernel_version; *c && *c != ' '; c++) {
				ba->commandLine[n++] = *c;
			}
			ba->commandLine[n++] = ' ';
		} else {
			print("WARNING: hakios_kver= does not fit in the kernel command line (%d of %d used)\n", (uint64_t)before,
			    (uint64_t)(BOOT_LINE_LENGTH - 1));
		}
	}
	/*
	 * The payload's own command line, last. The kernel's boot_args holds BOOT_LINE_LENGTH characters and the
	 * copy used to stop at the end without a word: a long line lost its tail (the last argument arrived
	 * shortened and the kernel took it as that). Now it is said, with how much was lost.
	 */
	const char *rest = pi.cmdline;
	for (; *rest && n < BOOT_LINE_LENGTH - 1; rest++) {
		ba->commandLine[n++] = *rest;
	}
	if (*rest != 0) {
		print("WARNING: the kernel command line is TRUNCATED: %d characters of the payload's cmdline did not fit in the "
		    "%d the kernel has; the last argument is cut\n", (uint64_t)str_len(rest), (uint64_t)(BOOT_LINE_LENGTH - 1));
#ifdef HAKIOS_LOADER_PROD
		return 0; /* Never boot with a truncated storage, battery or CPU argument. */
#endif
	}
	print("cmdline: %d of %d characters\n", (uint64_t)n, (uint64_t)(BOOT_LINE_LENGTH - 1));

	/* --- device tree ---------------------------------------------------- */
	const struct platform *plat = PLAT;
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .end = (uint8_t *)(dt_pa + dt_max), .depth = 0 };
	uint64_t cntfrq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t ranges[3] = { 0, plat->io_base, plat->io_size };
	uint64_t reg2[2];
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(READ_SYSREG(CNTVCT_EL0) >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	/*
	 * The identity is the iPhone's (model, compatible, target-type ...): iOS user space looks
	 * its device up by these, and its files -- the dyld shared cache to begin with -- are
	 * those of that phone. XNU itself does not care; the hardware is described further down.
	 */
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	/* what iBoot takes from the factory configuration of a real phone; fixed size fields */
	const char *serial = PLAT == &platform_umi ? "UMI0XIAOMI10" : "RDW00POCOX5P";
	const char *mlb_serial = PLAT == &platform_umi ? "UMI0XIAOMI10MLB00" : "RDW00POCOX5PMLB00";
	dt_prop_padded(&w, "serial-number", serial, 32);
	dt_prop_padded(&w, "mlb-serial-number", mlb_serial, 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	if (usb_kernel_requested)
		dt_prop_u64(&w, "umi-usb-handoff", (uint64_t)(uintptr_t)&usb_handoff);
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	dt_prop_u32(&w, "debug-enabled", 1);
	/* what MobileGestalt answers "main screen scale" from: 3 device pixels per point */
	dt_prop_str(&w, "hakios-board", plat->name);
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	/* how the kernel is to ask the firmware for more cores (PSCI) */
	dt_prop_str(&w, "psci-method", plat->psci_hvc ? "hvc" : "smc");
	/* the time now, by the firmware's clock (the kernel has no real-time clock driver) */
	uint64_t epoch = firmware_epoch();
	if (epoch != 0) {
		dt_prop_u64(&w, "boot-epoch", epoch);
	}
	/*
	 * More of what iBoot reports about the phone. MobileGestalt builds the device identifier
	 * (UDID) from the serial number, the chip ID and the two addresses; when one is missing it
	 * asks its helper for the factory data, is refused and waits forever (and lsd with it).
	 */
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	/* (its %02X form names the personalized baseband folder in flashboot: overlays/userland/build.sh BB_HASH) */
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	/* An empty but well-formed NVRAM image: IOKit refuses to start without it. */
	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	dt_begin(&w, "memory-map");
	/*
	 * "RAMDisk" becomes /dev/md0, the root (the kernel and launchd treat
	 * only md0 as a RAM disk boot); "RAMDisk1" becomes /dev/md1. With both
	 * images present the big resident one is the root and the payload's
	 * small one is the second disk.
	 */
	if (rd_size != 0 || rd1_size != 0) {
		uint64_t first[2] = { rd1_size != 0 ? rd1_pa : rd_pa, rd1_size != 0 ? rd1_size : rd_size };
		dt_prop(&w, "RAMDisk", first, sizeof(first));
	}
	if (rd_size != 0 && rd1_size != 0) {
		uint64_t second[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk1", second, sizeof(second));
	}
	dt_end(&w);
	if (more_cores && plat->timeout_scale != 0) {
		/* multiplies every "machine timeout" of the kernel (machine_timeout_init_with_suffix()) */
		dt_begin(&w, "machine-timeouts");
		dt_prop_u32(&w, "global-scale", plat->timeout_scale);
		dt_end(&w);
	}
	dt_end(&w);

	/*
	 * What iBoot's device tree tells the kernel to do unless told otherwise. The compressor: an
	 * embedded kernel left to itself keeps it for the freezer alone, and with nothing being
	 * compressed the memory is gone twice as fast (2 = VM_PAGER_COMPRESSOR_NO_SWAP, as on a phone).
	 */
	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	/* the product: screen class, artwork scale, name ... as on the real phone */
	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	/* the iPhone X's Qualcomm baseband (D22AP device tree): which firmware and which CommCenter drivers ABM picks */
	if (baseband_requested) {
		dt_prop_str(&w, "baseband-chipset", "mav17");
	}
	/*
	 * and its haptics: "closed-loop" (MobileGestalt's DeviceSupportsClosedLoopHaptics) makes iOS play
	 * its vibrations and feedbacks as waveforms on the "Actuator" audio device (the audio plug-in
	 * hands them to the kernel's motor driver); without it iOS looks for an IOKit vibration motor
	 * that the iPhone X does not have, and nothing vibrates.
	 */
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	/*
	 * and of its camera what is here: with torch=N the flash (MobileGestalt's camera-flash; without
	 * it Control Center has no flashlight); with cam=5 the node itself, for a camera there is (with it
	 * MobileGestalt's video-camera). The rest of the iPhone's camera is not here, nor said to be.
	 */
	if (torch_requested || camera_requested) {
		dt_begin(&w, "camera");
		if (torch_requested) {
			dt_identity_one(&w, identity_camera, sizeof(identity_camera) / sizeof(identity_camera[0]), identity_camera_data, "flash");
		}
		dt_end(&w);
	}
	dt_end(&w);

	/*
	 * umi_modem=2: the node that makes launchd load the cellular CommCenter (_LimitLoadToDeviceTree = baseband)
	 * and MobileGestalt report a baseband. Identity as on D22AP; the kernel's bridge (hakios.umi_baseband.cpp)
	 * is the driver behind it, so no function-* lines are given.
	 */
	if (baseband_requested) {
		/*
		 * D22AP's own values. "class" is read by lockdownd and ABM (baseband_class), "config" by
		 * libTelephonyBaseband's _TelephonyBasebandControllerLoadConfigOnce (at least 24 bytes: its power and reset
		 * timings, in ms), from the IOService named "baseband" (the bridge's AppleBaseband copies this node).
		 */
		static const uint32_t baseband_config[10] = { 250, 500, 18500, 1000, 100, 100, 131, 37, 20, 100 };
		dt_begin(&w, "baseband");
		dt_prop_str(&w, "device_type", "baseband");
		dt_prop_str(&w, "compatible", "baseband,n41");
		dt_prop_u32(&w, "class", 3);
		dt_prop(&w, "config", baseband_config, sizeof(baseband_config));
		dt_end(&w);
	}

	/*
	 * location=0: the node that keeps launchd from loading iOS's locationd, whose job (overlays/userland/mkjobs.py,
	 * KILL_SWITCH) says _LimitLoadFromDeviceTree = hakios-no-location -- the stock mechanism by which
	 * CommCenter-noncellular stays out when /baseband is here. Without the argument there is no node and locationd
	 * loads, on demand. For a boot that hangs on locationd: the same payload, plus this (LOCATION-umi.md).
	 */
	if (location_off) {
		dt_begin(&w, "hakios-no-location");
		dt_prop_str(&w, "device_type", "hakios-no-location");
		dt_prop_str(&w, "compatible", "hakios-no-location");
		dt_end(&w);
	}

	/* IOKit publishes its NVRAM object on this node; without it IOKit asserts. */
	dt_begin(&w, "options");
	dt_end(&w);

	/* Panic log region: RAM outside XNU's window (tail of the payload reservation). */
	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/*
	 * The processors. The first is the one the kernel starts on. With "cpus=N" in its
	 * arguments the kernel takes that one and the next N-1 of this list and starts them
	 * itself (PSCI CPU_ON); "cpumask=M" picks by position, bit 0 being the first. So the
	 * order matters: after the boot core come those whose numbers are nearest to its own
	 * (on the phone, where the kernel boots on the biggest core, 7: the other big ones
	 * 6, 5, 4, then the small ones 3, 2, 1). Only cores that are off are listed: the one
	 * this loader stays on while it watches is not the kernel's, nor any the firmware runs.
	 */
	uint64_t cpu_mpidr[8] = { kernel_mpidr };
	int cpu_count = 1, boot_core = (int)(kernel_mpidr >> plat->core_shift);

	if (more_cores) {
		for (int distance = 1; distance < plat->cores; distance++) {
			for (int side = 0; side < 2; side++) {
				int core = side == 0 ? boot_core + distance : boot_core - distance;
				uint64_t mpidr = (uint64_t)core << plat->core_shift;
				int64_t state;

				if (core < 0 || core >= plat->cores || mpidr == this_mpidr) {
					continue;
				}
				state = psci_call(PSCI_AFFINITY_INFO, mpidr, 0, 0);
				if (state == 1 || state == PSCI_NOT_SUPPORTED) {
					cpu_mpidr[cpu_count++] = mpidr;
				} else if (state >= 0) {
					print("core %x is not off (the firmware says %d): not the kernel's to take\n", mpidr, (uint64_t)state);
				}
			}
		}
		print("cores for the kernel, in the order it takes them:");
		for (int i = 0; i < cpu_count; i++) {
			print(" %x", cpu_mpidr[i]);
		}
		print("\n");
	}

	dt_begin(&w, "cpus");
	for (int i = 0; i < cpu_count; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)cpu_mpidr[i]);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (plat->small_cores & (1u << ((cpu_mpidr[i] >> plat->core_shift) & 31))) {
			dt_prop_u32(&w, "small-core", 1);
		}
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", plat->name);
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	/*
	 * With cam=5, where MobileGestalt looks for the cameras (IODeviceTree:/arm-io/isp, its still-camera and
	 * rear-facing-camera, by which SpringBoard shows Camera.app): the iPhone's ISP node, of its
	 * properties only the camera at the back. Neither a compatible nor a device_type: no driver matches it.
	 */
	if (camera_requested) {
		dt_begin(&w, "isp");
		dt_prop_u32(&w, "camera-rear", 1);
		dt_end(&w);
	}
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x10000, plat->gic_redist - plat->io_base, 0x100000 };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);
	if (plat->uart != 0) {
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		reg2[0] = plat->uart - plat->io_base;
		reg2[1] = 0x1000;
		dt_prop(&w, "reg", reg2, sizeof(reg2));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);

	dt_end(&w);

	/* a tree that did not fit in the room before boot_args was cut short by dt_prop: not handed to the kernel */
	if (w.overflow) {
		print("device tree does not fit in %x bytes: not started\n", dt_max);
		return 0;
	}
	uint32_t dt_len = (uint32_t)(w.cur - w.base);
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = dt_len;
	print("device tree %x len %x, boot_args %x, top %x\n", dt_pa, (uint64_t)dt_len, ba_pa, top);
	print("cmdline: %s\n", ba->commandLine);
	if (kernel_mpidr != this_mpidr) {
		print("the kernel will run on core %x (this is core %x)\n", kernel_mpidr, this_mpidr);
	}
	umi_display_clock_snapshot("before XNU handoff");
	print("jumping to %x\n", entry_va + slide);
	flush_log();
	/* From here on the kernel feeds the watchdog (boot-arg hv_wdt_pet, see loader_return.c in XNU). */
	wdt_set_ms(60000);

	/* --- hand off (UEFI is left resident, see loader_save_ctx) ------------ */
	if (loader_save_ctx() == 0) {
		/*
		 * On real hardware (under the Qualcomm hypervisor) dirty data-cache
		 * lines do not survive the MMU-off round trip. UEFI stays resident,
		 * so everything it owns must be written back to RAM first: clean
		 * every allocated region in the memory map, then this stack again.
		 */
		map_size = sizeof(map_buf);
		if (!EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
			for (uint64_t off = 0; off < map_size; off += desc_size) {
				efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
				/* loader/boot-services/runtime code and data, ACPI tables; XNU's own window is handled below */
				if (d->phys_start >= win_base && d->phys_start < win_base + win_size) {
					continue;
				}
				if ((d->type >= 1 && d->type <= 6) || d->type == 9 || d->type == 10) {
					clean_dcache(d->phys_start, d->phys_start + (d->pages << 12));
				}
			}
		}
		uint64_t sp_now = (uint64_t)(uintptr_t)__builtin_frame_address(0);
		clean_dcache(sp_now - 0x2000, sp_now + 0x2000);
		flush_dcache(phys_base, top);
		if (checkpoint == CHECKPOINT_SELFTEST) {
			/* Do not enter the kernel at all: exercise only the return path. */
			jump_to_kernel((uint64_t)(uintptr_t)&loader_return_stub, 0);
		}
		if (kernel_mpidr != this_mpidr) {
			int64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);

			wdt_set_ms(WDT_BITE_MS);
			if (status != 0) {
				/* a kernel that hung may have left its own transfer list in the controller */
				hc_take_back(status == OTHER_CPU_HUNG ? "the kernel hung" : "the kernel did not start");
			}
			if (status == OTHER_CPU_HUNG) {
				/*
				 * The kernel still has that core; what it left was said -- on the screen, in the
				 * log, and the radio's last accesses -- by report_hung_kernel(), in
				 * run_on_other_cpu(), before it returned (a USB run is reset from there).
				 */
				set_result("hung", loader_ctx[CTX_LOG_LEN], 0);
				return 1;
			} else if (status != 0) {
				/* the kernel was told it would be on that core (return path, device tree): no second try */
				if (status == OTHER_CPU_NOT_STARTED) {
					print("core %x was started but never arrived\n", kernel_mpidr);
				} else {
					print("core %x did not start: PSCI error %d\n", kernel_mpidr, (uint64_t)-status);
				}
				set_result("no-such-core", (uint64_t)(status < 0 ? -status : status), 0);
				write_logs(NULL, 0);
				return 1;
			}
		} else {
			jump_to_kernel(entry_va + slide, ba_pa);
		}
	}

#ifdef HAKIOS_LOADER_PROD
	for (;;) { __asm__ volatile ("wfe"); } /* watchdog; never resume the debug menu */
#endif
	/* --- the kernel came back ------------------------------------------- */
	wdt_set_ms(WDT_BITE_MS);
	hc_take_back("the kernel came back");   /* before anything here touches the storage */
	if (hc_known) {
		flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
		    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
		print("ufs: the kernel put its own list in %d times and took it out %d times\n",
		    (uint64_t)block_shared.kernel_native.switched_in, (uint64_t)block_shared.kernel_native.switched_out);
	}
	if (block_write_requested) {
		print("ufs: %d writes into darwin, %d bytes, read back the same; %d refused\n", block_writes,
		    block_write_bytes, block_writes_refused);
	}
	/* ... and its other cores should all be off again: the next kernel cannot start one that is not */
	for (int i = 1; i < cpu_count; i++) {
		int64_t state = psci_call(PSCI_AFFINITY_INFO, cpu_mpidr[i], 0, 0);

		if (state != 1 && state != PSCI_NOT_SUPPORTED) {
			print("THE KERNEL LEFT CORE %x RUNNING (the firmware says %d): restart before running another\n", cpu_mpidr[i],
			    (uint64_t)state);
		}
	}
	recording_data = NULL;
	recording_len = 0;
	if (kernel_live != NULL && kernel_live->recording != 0 && checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *recording = (const uint64_t *)(kernel_live->recording + slide);

		if (recording[0] != 0 && recording[1] > 24 && recording[1] <= (1ULL << 30)) {
			recording_data = (const char *)recording[0];
			recording_len = recording[1];
			print("the kernel left a screen recording: %d bytes at %x\n", recording_len, recording[0]);
		}
	}
	sound_data = NULL;
	sound_len = 0;
	if (kernel_live != NULL && kernel_live->magic2 == KERNEL_LIVE_MAGIC2 && kernel_live->sound != 0 &&
	    checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *sound = (const uint64_t *)(kernel_live->sound + slide);

		if (sound[0] != 0 && sound[1] > 24 && sound[1] <= (1ULL << 30)) {
			sound_data = (const char *)sound[0];
			sound_len = sound[1];
			print("the kernel left a sound recording: %d bytes at %x\n", sound_len, sound[0]);
		}
	}
	const char *klog = (const char *)loader_ctx[CTX_LOG_PA];
	uint64_t klog_len = loader_ctx[CTX_LOG_LEN];
	uint64_t reason = loader_ctx[CTX_REASON];
	if (checkpoint == CHECKPOINT_SELFTEST) {
		print("return-path self test passed\n");
		set_result("selftest-ok", 0, 0);
		klog = NULL;
		klog_len = 0;
	} else {
		print("kernel returned: log at %x, %d bytes\n", (uint64_t)(uintptr_t)klog, klog_len);
		if (reason == 2) {
			print("STOPPED AT CHECKPOINT %d\n", loader_ctx[CTX_ESR]);
			set_result("checkpoint", loader_ctx[CTX_ESR], 0);
		} else if (reason == 1) {
			print("EARLY KERNEL EXCEPTION: esr=%x elr=%x far=%x lr=%x sp=%x\n", loader_ctx[CTX_ESR], loader_ctx[CTX_ELR],
			    loader_ctx[CTX_FAR], loader_ctx[CTX_LR], loader_ctx[CTX_KSP]);
			print("  (pa-va slide %x: subtract it from physical addresses to symbolicate)\n", slide);
			set_result("early-exception", 0, loader_ctx[CTX_ELR]);
			klog_len = klog_len > (1u << 20) ? 0 : klog_len;
		} else {
			set_result("returned", klog_len, 0);
		}
	}
	write_logs(klog, klog_len);
	return 1;
}

/* ------------------------------------------------------------------------ */
/* USB command channel: a small fastboot-protocol server.                    */
/*                                                                          */
/* Host side (stock `fastboot` tool):                                        */
/*   fastboot stage payload.bin    download a new "XNUP" payload            */
/*   fastboot oem run [N]          run its kernel (optionally stop at N)    */
/*   fastboot getvar result        blocks until the kernel is back          */
/*   fastboot get_staged log.txt   fetch loader + kernel log                */
/*   fastboot oem hang             stop petting the watchdog (test)         */
/*   fastboot reboot               cold reset                               */

#define USB_EP_OUT   0x01
#define USB_EP_IN    0x81
#define USB_CHUNK    (1024 * 1024)

static usb_device_protocol *USB;
static int download_to_ramdisk;     /* set by "oem ramdisk" for the one download that follows */
static uint8_t *usb_rx_buf, *usb_tx_buf;
static int usb_connected;

static void
usb_kernel_reset(void)
{
	print("usb: autonomous Android reset (kernel return/deadline)\n");
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	/* If firmware unexpectedly returns, stop petting the watchdog. */
	for (;;) {}
}

static void
usb_handoff_poll(uint64_t *daif, uint64_t *group1, int *watching_only)
{
	flush_dcache((uint64_t)(uintptr_t)&usb_handoff, (uint64_t)(uintptr_t)&usb_handoff + 64);
	if (!usb_handoff.request || usb_handoff.ack) return;
	efi_status s = USB ? USB->stop() : (EFI_ERROR_BIT | 14);
	if (!EFI_ERROR(s)) firmware_watch_only(daif, group1, watching_only);
	/* ABI2: ack=1 guarantees Stop plus CPU0 firmware interrupt quiescence. */
	usb_handoff.ack = EFI_ERROR(s) ? 2 : 1;
	clean_dcache((uint64_t)(uintptr_t)&usb_handoff, (uint64_t)(uintptr_t)&usb_handoff + 64);
	print("usb: firmware Stop=%x ownership ABI2 ack=%d; CPU0 polling only, autonomous reset armed\n", s, usb_handoff.ack);
}

static const uint8_t usb_dev_desc[18] = { 18, 1, 0x10, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_ss_dev_desc[18] = { 18, 1, 0x00, 0x03, 0, 0, 0, 9, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 0x50,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,   /* fastboot interface */
	7, 5, USB_EP_IN, 2, 0x00, 0x02, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x02, 0,
};
static const uint8_t usb_ss_cfg_desc[44] = {
	9, 2, 44, 0, 1, 1, 0, 0x80, 0x10,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,
	7, 5, USB_EP_IN, 2, 0x00, 0x04, 0,   6, 0x30, 4, 0, 0, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x04, 0,  6, 0x30, 4, 0, 0, 0,
};
static const uint8_t usb_qualifier[10] = { 10, 6, 0x00, 0x02, 0xff, 0xff, 0xff, 64, 1, 0 };
static const uint8_t usb_bos[42] = {
	5, 0x0f, 42, 0, 3,
	7, 0x10, 0x02, 0x06, 0, 0, 0,
	10, 0x10, 0x03, 0x00, 0x0e, 0x00, 0x01, 0x07, 0x65, 0x00,
	20, 0x10, 0x0a, 0x00, 0x01, 0, 0, 0, 0x00, 0x11, 0, 0, 0x30, 0x40, 0x0a, 0x00, 0xb0, 0x40, 0x0a, 0x00,
};
static const uint8_t usb_str0[4] = { 4, 3, 0x09, 0x04 };
static const uint8_t usb_str_mfr[8] = { 8, 3, 'X', 0, 'N', 0, 'U', 0 };
static const uint8_t usb_str_prod[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_serial[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_iface[18] = { 18, 3, 'f', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0 };

static void EFIAPI
usb_dummy_notify(void *event, void *ctx)
{
}

static int
usb_start(void)
{
	static const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
	static const efi_guid init_guid = QCOM_USB_INIT_EVENT_GUID;
	static const void *cfg = usb_cfg_desc, *ss_cfg = usb_ss_cfg_desc;
	static const void *strings[5] = { usb_str0, usb_str_mfr, usb_str_prod, usb_str_serial, usb_str_iface };
	static usb_device_descriptor_set set;
	void *event;

	/* Ask the platform to bring the controller up in peripheral mode (as the stock fastboot does). */
	if (!EFI_ERROR(BS->create_event_ex(0x200 /* EVT_NOTIFY_SIGNAL */, 8 /* TPL_CALLBACK */, usb_dummy_notify, NULL,
	    &init_guid, &event))) {
		BS->signal_event(event);
		BS->close_event(event);
	}
	if (EFI_ERROR(BS->locate_protocol(&usb_guid, NULL, (void **)&USB))) {
		print("usb: device protocol not found\n");
		return 0;
	}
	set.device_descriptor = usb_dev_desc;
	set.descriptors = &cfg;
	set.ss_device_descriptor = usb_ss_dev_desc;
	set.ss_descriptors = &ss_cfg;
	set.device_qualifier = usb_qualifier;
	set.bos = usb_bos;
	set.string_count = 5;
	set.strings = strings;
	efi_status s = USB->start_ex(&set);
	if (EFI_ERROR(s)) {
		print("usb: start failed %x\n", s);
		return 0;
	}
	if (EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_rx_buf)) ||
	    EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_tx_buf))) {
		print("usb: no transfer buffers\n");
		return 0;
	}
	print("usb: started\n");
	return 1;
}

/*
 * Pump controller events until a transfer on `endpoint` finishes.
 * Returns the byte count, or -1 if the transfer was cancelled or the cable
 * state changed. Keeps the watchdog fed while idle.
 */
static int64_t
usb_wait(uint8_t endpoint)
{
	/* static: a long transfer is many short waits, and the watchdog must be fed across them */
	static uint64_t last_pet;
	uint64_t freq = READ_SYSREG(CNTFRQ_EL0), began = READ_SYSREG(CNTVCT_EL0);

	for (;;) {
		uint32_t msg = UsbDeviceEventNoEvent;
		uint64_t size = 0;
		usb_device_event_data data;

		USB->handle_event(&msg, &size, &data);
		if (msg == UsbDeviceEventDeviceStateChange) {
			usb_connected = (data.device_state == UsbDeviceStateConnected);
			return -1;
		}
		/* with a kernel running, a host that went away must not keep this core from watching it */
		if (run_active && READ_SYSREG(CNTVCT_EL0) - began > 3 * freq) {
			return -1;
		}
		if (msg == UsbDeviceEventTransferNotification && data.transfer.endpoint_index == endpoint) {
			return data.transfer.status == UsbDeviceTransferStatusCompleteOK ? (int64_t)data.transfer.bytes_completed : -1;
		}
		if (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {
			last_pet = READ_SYSREG(CNTVCT_EL0);
			wdt_pet();
		}
	}
}

static int usb_rx_queued;       /* usb_poll_command() has left the receive for the next command with the controller */

static int64_t
usb_rx(uint8_t *buf, uint64_t len)
{
	if (usb_rx_queued) {
		usb_rx_queued = 0;      /* (every command is received the same way, into the same buffer) */
	} else if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {
		return -1;
	}
	return usb_wait(USB_EP_OUT);
}

static int64_t
usb_tx(const uint8_t *data, uint64_t len)
{
	if (data != usb_tx_buf) {
		memcpy(usb_tx_buf, data, len);
	}
	if (EFI_ERROR(USB->send(USB_EP_IN, len, usb_tx_buf))) {
		return -1;
	}
	return usb_wait(USB_EP_IN);
}

static void
fb_reply(const char *code, const char *text)
{
	uint32_t n = 0;
	for (; *code; code++) {
		usb_tx_buf[n++] = (uint8_t)*code;
	}
	for (; *text && n < 63; text++) {
		usb_tx_buf[n++] = (uint8_t)*text;
	}
	usb_tx(usb_tx_buf, n);
}

static void
fb_reply_data(uint64_t len)
{
	char msg[13] = "DATA00000000";
	for (int i = 0; i < 8; i++) {
		msg[4 + i] = "0123456789abcdef"[(len >> (28 - 4 * i)) & 0xf];
	}
	usb_tx((const uint8_t *)msg, 12);
}

static int
starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++) {
			return 0;
		}
	}
	return 1;
}

/*
 * One command from the host. While a kernel runs on another core (running_live) only what does
 * not disturb it: questions, the screen, the log so far, and "oem stop".
 */
static void
fb_command(char *cmd)
{
	/* a kernel runs on another core, with a live block or without one (running_live alone is NULL without) */
	int busy = run_active;

	{
		if (busy && (starts_with(cmd, "oem ramdisk") || starts_with(cmd, "download:") || starts_with(cmd, "oem run") ||
		    starts_with(cmd, "oem selftest") || starts_with(cmd, "oem hang") || starts_with(cmd, "oem recording") ||
		    starts_with(cmd, "oem sound") || starts_with(cmd, "oem clearscreen") || starts_with(cmd, "oem memmap"))) {
			fb_reply("FAIL", "a kernel is running (oem stop ends it)");
		} else if (starts_with(cmd, "oem stop")) {
			/* the kernel looks at this once a second and comes back as it does when its time is up */
			if (busy && running_live != NULL) {
				((struct kernel_live *)(uintptr_t)running_live)->stop = 1;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "getvar:")) {
			const char *var = cmd + 7;
			if (str_eq(var, "product")) {
				fb_reply("OKAY", "xnuloader");
			} else if (str_eq(var, "version")) {
				fb_reply("OKAY", "0.5");
			} else if (str_eq(var, "umi-usb-handoff")) {
				fb_reply("OKAY", "2");
			} else if (str_eq(var, "max-download-size")) {
				fb_reply("OKAY", download_to_ramdisk ? "0xffffffff" : "0x03f00000");
			} else if (str_eq(var, "result")) {
				fb_reply("OKAY", last_result);
			} else if (str_eq(var, "screen") && SCREEN.base != 0) {
				/* width height bytes-per-row */
				uint32_t mark = log_len;
				print("%d %d %d", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, (uint64_t)SCREEN.stride);
				log_buf[log_len] = 0;
				fb_reply("OKAY", log_buf + mark);
				log_len = mark;
			} else {
				fb_reply("OKAY", "");
			}
		} else if (starts_with(cmd, "oem ramdisk")) {
			/* "oem ramdisk": the next download is a root image for the resident area; "oem ramdisk none" drops it */
			struct payload_info pi;

			if (starts_with(cmd, "oem ramdisk none")) {
				resident_rd_size = 0;
				/* (an "oem ramdisk" before this one must not leave the next download aimed at the resident area) */
				download_to_ramdisk = 0;
				fb_reply("OKAY", "");
				return;
			}
			parse_payload(&pi, 0);
			log_len = 0;
			if (!choose_window(&pi) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE) {
				fb_reply("FAIL", "no memory window");
				return;
			}
			download_to_ramdisk = 1;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "download:")) {
			uint64_t total = parse_number(cmd + 9, 16), got = 0;
			uint64_t limit = PAYLOAD_MAX_SIZE - 0x100000;
			uint8_t *dest = (uint8_t *)PAYLOAD_BASE;
			int to_ramdisk = download_to_ramdisk;

			download_to_ramdisk = 0;
			if (to_ramdisk) {
				limit = win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE;
				/* the answer to "download:" carries the size in 32 bits: a larger one would be answered as another size */
				if (limit > 0xffffffffULL) {
					limit = 0xffffffffULL;
				}
				dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);
				resident_rd_size = 0;
			}
			if (total == 0 || total > limit) {
				fb_reply("FAIL", "bad size");
				return;
			}
			/*
			 * A download over the payload makes the payload no payload until it is whole: the XNUP magic
			 * is taken out when it arrives and put back at the end, so that a transfer that stops half-way
			 * leaves something "oem run" refuses, not a mix of the new and the old.
			 */
			if (!to_ramdisk) {
				*(volatile uint32_t *)dest = 0;
			}
			uint32_t magic_held = 0;
			int magic_taken = 0;
			fb_reply_data(total);
			while (got < total) {
				uint64_t want = total - got > USB_CHUNK ? USB_CHUNK : total - got;
				int64_t r = usb_rx(usb_rx_buf, want);
				if (r <= 0) {
					break;
				}
				/* (a controller that reports more than was asked for must not carry the copy past the end) */
				if (r > (int64_t)want) {
					r = (int64_t)want;
				}
				memcpy(dest + got, usb_rx_buf, (size_t)r);
				if (!to_ramdisk && !magic_taken && got + (uint64_t)r >= 4) {
					magic_held = *(volatile uint32_t *)dest;
					*(volatile uint32_t *)dest = 0;
					magic_taken = 1;
				}
				got += (uint64_t)r;
			}
			if (got == total) {
				if (magic_taken) {
					*(volatile uint32_t *)dest = magic_held;
				}
				if (to_ramdisk) {
					resident_rd_size = total;
				}
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "short download");
			}
		} else if (starts_with(cmd, "upload")) {
			uint64_t sent = 0;
			fb_reply_data(staged_len);
			while (sent < staged_len) {
				uint64_t chunk = staged_len - sent > USB_CHUNK ? USB_CHUNK : staged_len - sent;
				if (usb_tx((const uint8_t *)staged_data + sent, chunk) < 0) {
					break;
				}
				sent += chunk;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem screenshot")) {
			/* stage the raw framebuffer; "getvar screen" gives its geometry */
			if (SCREEN.base == 0) {
				fb_reply("FAIL", "no display");
				return;
			}
			staged_data = (const char *)(uintptr_t)SCREEN.base;
			staged_len = (uint64_t)SCREEN.stride * SCREEN.height;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem recording")) {
			/* stage what the kernel's display driver recorded (boot-arg rec_secs) */
			if (recording_data == NULL) {
				fb_reply("FAIL", "no recording");
				return;
			}
			staged_data = recording_data;
			staged_len = recording_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem sound")) {
			/* stage what the kernel recorded of the sound (boot-arg rec_sound) */
			if (sound_data == NULL) {
				fb_reply("FAIL", "no sound recording");
				return;
			}
			staged_data = sound_data;
			staged_len = sound_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem clearscreen")) {
			if (SCREEN.base != 0) {
				memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)SCREEN.stride * SCREEN.height);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem run") || starts_with(cmd, "oem selftest")) {
			int selftest = starts_with(cmd, "oem selftest");
			const char *arg = cmd + 7;
			while (*arg == ' ') {
				arg++;
			}
			int have_cp = selftest || (*arg >= '0' && *arg <= '9');
			uint64_t cp = selftest ? CHECKPOINT_SELFTEST : parse_number(arg, 10);

			set_result("running", cp, 0);
			completed_log_len = 0;
			fb_reply("OKAY", "");
			log_len = 0;
			print("== XNULOADER run (usb) ==\n");
			if (!run_kernel(have_cp, cp)) {
				set_result("not-started", 0, 0);
				compose_logs(NULL, 0);
			}
		} else if (starts_with(cmd, "oem hang")) {
			fb_reply("OKAY", "watchdog test: not petting any more");
			for (;;) {
			}
		} else if (starts_with(cmd, "oem memmap")) {
			/* the firmware's memory map into the log (then "oem log" and fetch it) */
			completed_log_len = 0;
			print_memory_map();
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem log")) {
			if (busy && running_live != NULL) {
				compose_live_log(running_live, running_slide);
			} else if (completed_log_len) {
				/* Keep the full completed kernel log: the console echo can
				 * exceed log_buf, and a final live poll must not replace it. */
				staged_data = out_buf;
				staged_len = completed_log_len;
			} else {
				compose_logs(NULL, 0);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "reboot") || starts_with(cmd, "oem reboot")) {
			fb_reply("OKAY", "");
			BS->stall(200000);
			ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
		} else {
			fb_reply("FAIL", "unknown command");
		}
	}
}

/*
 * A kernel is running on another core and this one watches it: see whether the host has said
 * something meanwhile, without waiting for it to. This is how a run can be asked to end, and
 * the screen and the log be had while it lasts.
 */
static void
usb_poll_command(void)
{
	uint32_t msg = UsbDeviceEventNoEvent;
	uint64_t size = 0, n;
	usb_device_event_data data;
	char cmd[128];

	if (USB == NULL || (usb_kernel_requested && usb_handoff.ack)) {
		return;         /* this run was not started over USB */
	}
	if (!usb_rx_queued && usb_connected && !EFI_ERROR(USB->send(USB_EP_OUT, 511, usb_rx_buf))) {
		usb_rx_queued = 1;
	}
	USB->handle_event(&msg, &size, &data);
	if (msg == UsbDeviceEventDeviceStateChange) {
		usb_connected = (data.device_state == UsbDeviceStateConnected);
		usb_rx_queued = 0;
		return;
	}
	if (msg != UsbDeviceEventTransferNotification || data.transfer.endpoint_index != USB_EP_OUT) {
		return;
	}
	usb_rx_queued = 0;
	if (data.transfer.status != UsbDeviceTransferStatusCompleteOK || data.transfer.bytes_completed == 0) {
		return;
	}
	n = data.transfer.bytes_completed < sizeof(cmd) - 1 ? data.transfer.bytes_completed : sizeof(cmd) - 1;
	memcpy(cmd, usb_rx_buf, (size_t)n);
	cmd[n] = 0;
	fb_command(cmd);
}

static void
usb_command_loop(void)
{
	screen_quiet = 1;
	for (;;) {
		char cmd[128];
		int64_t n;

		if (!usb_connected) {
			usb_wait(0xff); /* returns on the next cable state change */
			continue;
		}
		n = usb_rx(usb_rx_buf, 511);
		if (n <= 0) {
			continue;
		}
		if (n > (int64_t)sizeof(cmd) - 1) {
			n = sizeof(cmd) - 1;
		}
		memcpy(cmd, usb_rx_buf, (size_t)n);
		cmd[n] = 0;
		fb_command(cmd);
	}
}

/* ------------------------------------------------------------------------ */

/*
 * QEMU's "ramfb" display shows whatever guest memory it is told to, in any size; the firmware
 * only offers a few small landscape modes of it. So we point it at memory of our own with the
 * shape of the iPhone's screen, through the fw_cfg file "etc/ramfb" (the firmware's own mode
 * is simply replaced). In QEMU the display is then exactly what iOS expects; on the real phone
 * the kernel's display driver scales that picture to the panel.
 */
#define PHONE_WIDTH     1125            /* the screen of the iPhone X, whose identity we carry */
#define PHONE_HEIGHT    2436
#define FW_CFG_BASE     0x09020000ULL   /* QEMU virt: data at +0, selector at +8, DMA address at +16 */
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_DMA_SELECT 0x08
#define FW_CFG_DMA_WRITE  0x10

static uint32_t
be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

static void
fw_cfg_read(void *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *)buf)[i] = *(volatile uint8_t *)FW_CFG_BASE;
	}
}

static void
ramfb_phone_screen(void)
{
	static struct {
		uint64_t addr;
		uint32_t fourcc, flags, width, height, stride;
	} __attribute__((packed)) cfg;
	/*
	 * volatile: QEMU writes `control' back (to 0) when it has done the request, behind the compiler's back. Without the
	 * qualifier a build is free to keep the value written a few lines below and never look again, and the loader then
	 * says the request failed, leaves ramfb at the firmware's 800x600 and iOS draws its 1125x2436 into a corner: a black window
	 * (it did, with a newer build of this very file; the older one happened to reload).
	 */
	static volatile struct {
		uint32_t control, length;
		uint64_t address;
	} dma;
	struct {
		uint32_t size;
		uint16_t select, reserved;
		char name[56];
	} file;
	uint32_t count, select = 0;
	uint64_t fb = 0, bytes = (uint64_t)PHONE_WIDTH * 4 * PHONE_HEIGHT;

	*(volatile uint16_t *)(FW_CFG_BASE + 8) = __builtin_bswap16(FW_CFG_FILE_DIR);
	fw_cfg_read(&count, sizeof(count));
	count = be32(count);
	for (uint32_t i = 0; i < count && i < 256 && select == 0; i++) {
		fw_cfg_read(&file, sizeof(file));
		if (str_eq(file.name, "etc/ramfb")) {
			select = __builtin_bswap16(file.select);
		}
	}
	if (select == 0 || EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (bytes + 0xFFF) >> 12, &fb))) {
		return;
	}
	memset((void *)(uintptr_t)fb, 0, (size_t)bytes);
	cfg.addr = __builtin_bswap64(fb);
	cfg.fourcc = be32(0x34325258);          /* 'XR24': blue, green, red, unused */
	cfg.flags = 0;
	cfg.width = be32(PHONE_WIDTH);
	cfg.height = be32(PHONE_HEIGHT);
	cfg.stride = be32(PHONE_WIDTH * 4);
	dma.control = be32(select << 16 | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
	dma.length = be32(sizeof(cfg));
	dma.address = __builtin_bswap64((uint64_t)(uintptr_t)&cfg);
	__asm__ volatile ("dsb sy" ::: "memory");       /* (cfg and dma are in memory before the device is told to read them) */
	*(volatile uint64_t *)(FW_CFG_BASE + 16) = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
	/* the device does it at once; allow it some time all the same instead of reading once */
	for (uint32_t spin = 0; dma.control != 0 && spin < 1000000; spin++) {
		__asm__ volatile ("" ::: "memory");
	}
	if (dma.control != 0) {
		print("ramfb: control %x after the request\n", (uint64_t)be32(dma.control));
		return;
	}
	SCREEN.base = fb;
	SCREEN.width = PHONE_WIDTH;
	SCREEN.height = PHONE_HEIGHT;
	SCREEN.stride = PHONE_WIDTH * 4;
}

#include "flashboot.c"

/*
 * UEFI-native boot: a payload from the volume the loader was started from.
 *
 * On the phone the payload (the kernel, the small root image, the command line) is put in memory at PAYLOAD_BASE
 * before the loader runs: by the boot shim behind Project Silicium's image (fastboot boot), or by QEMU's loader
 * device. This is the other way, the one every UEFI boot loader has: when there is no payload in memory, look for
 * \EFI\hakios\payload.bin on the volume this image was loaded from (the EFI system partition, where
 * \EFI\BOOT\BOOTAA64.EFI is the loader itself), read it to PAYLOAD_BASE and go on as if it had been there all along.
 * That is what lets the kernel start from a boot menu, a USB stick or an SD card, without fastboot.
 *
 * It prints nothing (the banner has not been drawn yet): volume_payload_bytes and volume_payload_note say what
 * happened, and efi_main says it under the banner. The file has to fit in the payload region (less the last MiB,
 * which holds the panic log), and the region has to be free: where a firmware keeps it reserved, the allocation is
 * refused and the loader goes on without a payload, as it always did.
 * Only the kernel and a root image that fits in the payload come this way; the big iOS system image does not
 * (docs/uefi-boot.md says what does).
 */
static uint64_t volume_payload_bytes;           /* 0: nothing was read; else the payload's size */
static const char *volume_payload_note;         /* why not, if it was looked for */

static int
load_payload_from_volume(void)
{
	static const efi_guid image_guid = EFI_LOADED_IMAGE_GUID, fs_guid = EFI_SIMPLE_FILE_SYSTEM_GUID,
	    info_guid = EFI_FILE_INFO_GUID;
	static const char16 path[] = u"\\EFI\\hakios\\payload.bin";
	efi_loaded_image *image = NULL;
	efi_simple_fs *fs = NULL;
	efi_file *root = NULL, *file = NULL;
	uint8_t info[512] __attribute__((aligned(8)));
	uint64_t info_size = sizeof(info), size, done = 0, pages;
	uint64_t address = PAYLOAD_BASE;

	if (EFI_ERROR(BS->handle_protocol(IMAGE, &image_guid, (void **)&image)) || image == NULL || image->device == NULL ||
	    EFI_ERROR(BS->handle_protocol(image->device, &fs_guid, (void **)&fs)) || fs == NULL) {
		volume_payload_note = "the loader was not started from a volume with a file system";
		return 0;
	}
	if (EFI_ERROR(fs->open_volume(fs, &root)) || root == NULL) {
		volume_payload_note = "the volume cannot be opened";
		return 0;
	}
	if (EFI_ERROR(root->open(root, &file, path, EFI_FILE_MODE_READ, 0)) || file == NULL) {
		volume_payload_note = "there is no \\EFI\\hakios\\payload.bin on the volume";
		root->close(root);
		return 0;
	}
	if (EFI_ERROR(file->get_info(file, &info_guid, &info_size, info))) {
		volume_payload_note = "the size of \\EFI\\hakios\\payload.bin cannot be read";
		goto close;
	}
	size = *(uint64_t *)(info + EFI_FILE_INFO_FILE_SIZE_OFFSET);
	if (size < sizeof(struct payload_header) || size > PAYLOAD_MAX_SIZE - 0x100000ULL) {
		volume_payload_note = "\\EFI\\hakios\\payload.bin is empty or does not fit in the payload region (63 MiB)";
		goto close;
	}
	/*
	 * Take the whole payload region, not only the pages the file fills: the rest of it is written later (a payload
	 * sent over USB with `download:` goes up to the 63 MiB limit, and the panic log lives in the last MiB), and
	 * memory the firmware has not given to us must not be written.
	 */
	pages = PAYLOAD_MAX_SIZE / 0x1000;
	if (EFI_ERROR(BS->allocate_pages(2 /* AllocateAddress */, 2 /* EfiLoaderData */, pages, &address))) {
		volume_payload_note = "the payload region is not free (the firmware keeps it reserved)";
		goto close;
	}
	while (done < size) {
		uint64_t chunk = size - done < 0x100000ULL ? size - done : 0x100000ULL;       /* 1 MiB at a time */

		if (EFI_ERROR(file->read(file, &chunk, (void *)(uintptr_t)(PAYLOAD_BASE + done))) || chunk == 0) {
			volume_payload_note = "\\EFI\\hakios\\payload.bin could not be read to its end";
			/* what was read stays in memory after the pages are given back: without this, the first MiB's XNUP
			 * header would still be there and a half-read payload would be taken for a whole one */
			((volatile struct payload_header *)PAYLOAD_BASE)->magic = 0;
			BS->free_pages(PAYLOAD_BASE, pages);
			goto close;
		}
		done += chunk;
	}
	if (((const struct payload_header *)PAYLOAD_BASE)->magic != PAYLOAD_MAGIC) {
		volume_payload_note = "\\EFI\\hakios\\payload.bin is not a payload (no XNUP at its start)";
		BS->free_pages(PAYLOAD_BASE, pages);
		goto close;
	}
	volume_payload_bytes = size;
close:
	file->close(file);
	root->close(root);
	return volume_payload_bytes != 0;
}

/*
 * The version of the kernel in the payload (the entry kernel.ver, made by xnuloader/mkpayload.py from
 * tools/kernel-version.py), or NULL if the payload has none. A look at the payload that changes
 * nothing and prints nothing, so that the banner can use it before the rest of the loader runs.
 */
static const char *
payload_kernel_version(void)
{
	const struct payload_header *ph = (const struct payload_header *)PAYLOAD_BASE;

	if (ph->magic != PAYLOAD_MAGIC) {
		return NULL;
	}
	/* (the same limits as parse_payload: a header size or an entry size that reaches past the payload ends the walk) */
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + payload_walk_size(ph->size);
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;

		if (e->name[0] == 0) {
			break;
		}
		if ((uint64_t)e->size > (uint64_t)(pend - (p + sizeof(*e)))) {
			break;
		}
		if (str_eq(e->name, "kernel.ver")) {
			return (const char *)(p + sizeof(*e));
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return NULL;
}

/*
 * The greeting. Printed from efi_main, while the UEFI text console is still ours (on the phone that is
 * Project Silicium's UEFI): the USB command loop that follows sets screen_quiet and leaves the screen
 * alone, so what is on it now stays there. It also goes into the log, which "xl log" fetches.
 * The two lines under the letters are the ones the project's banner has: a greeting from the kernel,
 * and its version, kernel.ver=, as tools/kernel-version.py makes it.
 */
static void
print_banner(void)
{
	const char *version = payload_kernel_version();

	print("\n");
	print("   #   #   ###   #   #  #####   ###    ####\n");
	print("   #   #  #   #  #  #     #    #   #  #\n");
	print("   #####  #####  ###      #    #   #   ###\n");
	print("   #   #  #   #  #  #     #    #   #      #\n");
	print("   #   #  #   #  #   #  #####   ###   ####\n");
	print("\n");
	print("   hello hakios-kernel!\n");
	print("   kernel.ver=%s\n", version ? version : "unknown");
	print("\n");
}

efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
	static const efi_guid gop_guid = EFI_GOP_GUID;
	struct payload_info pi;

	ST = st;
	BS = st->bs;
	IMAGE = image;

	BS->set_watchdog_timer(0, 0, 0, NULL);
	/* no payload in memory (nobody put one there): the volume this loader came from may have one, as a UEFI boot loader's does */
	if (((const struct payload_header *)PAYLOAD_BASE)->magic != PAYLOAD_MAGIC) {
		load_payload_from_volume();
	}
	print("== XNULOADER v3 ==\n");
#ifdef HAKIOS_LOADER_PROD
	print("UMI PRODUCTION ABI1: no timed return, no host logging while XNU runs\n");
#endif
	print_banner();
	if (volume_payload_bytes != 0) {
		print("payload: %d bytes from \\EFI\\hakios\\payload.bin on the volume this loader was started from\n",
		    volume_payload_bytes);
	} else if (volume_payload_note != NULL) {
		print("payload: none in memory, and %s\n", volume_payload_note);
	}
	print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d image=%x\n", READ_SYSREG(CurrentEL) >> 2, READ_SYSREG(MIDR_EL1),
	    READ_SYSREG(ID_AA64MMFR0_EL1), READ_SYSREG(CNTFRQ_EL0), (uint64_t)(uintptr_t)&__ImageBase);

	PLAT = &platform_qemu_virt;
	if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
	    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
		GOP = NULL;                     /* no display (QEMU without -device ramfb) */
	} else {
		SCREEN.base = GOP->mode->fb_base;
		SCREEN.width = GOP->mode->info->width;
		SCREEN.height = GOP->mode->info->height;
		SCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
		for (unsigned i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++)
			if (platforms[i]->fb_base == SCREEN.base)
				PLAT = platforms[i];
		#ifndef LOADER_UMI
		if (PLAT == &platform_qemu_virt)
			ramfb_phone_screen();
#endif
		print("display %d x %d at %x\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
	}
#ifdef LOADER_UMI
	/* Do not turn an unknown phone framebuffer into QEMU's MMIO platform. */
	if (GOP == NULL || SCREEN.width != 1080 || SCREEN.height != 2340) {
		print("UMI: expected GOP 1080 x 2340; refusing kernel handoff\n");
		return 0x8000000000000003ULL; /* EFI_UNSUPPORTED */
	}
	PLAT = &platform_umi;
#endif
	print("platform %s\n", PLAT->name);
	umi_display_probe_dump();
	umi_display_clock_snapshot("loader entry");

	install_exception_hook();
	wdt_arm();

	int have_payload = parse_payload(&pi, 0);
	if (have_payload && pi.usb_mode && usb_start()) {
		flush_log();
		usb_command_loop();
	}

	/*
	 * One-shot mode: run the kernel that came with the image, then reset. A payload with the loader alone
	 * (an installed device's boot partition: common/flash/install.sh) has no kernel: that is the menu's.
	 */
	if (have_payload && pi.kernel != NULL) {
		run_kernel(0, 0);
		reboot();
	}
	/* No kernel: an installed device booting on its own -- the menu, the bundle from darwin, Android */
	fb_standalone();
	reboot();
	return 0;
}
