/*
 * vtdec_test: settle "green video" (H264SW's motion-compensated blocks read zero after the first
 * non-reference frame) on the phone, outside Safari and mediaserverd.
 *
 *   vtdec_test vm
 *       Checks the kernel VM calls libmalloc makes for decoder-sized blocks: malloc/calloc/free
 *       churn of frame-store-sized blocks (one thread and three at once), madvise REUSABLE/REUSE/
 *       DONTNEED next to live data (4K- and 16K-aligned), FREE (16K-aligned), purgeable volatile/nonvolatile,
 *       vm_copy / mach_vm_remap copy-on-write. Prints "vm: ... FAILED" for any lost or zeroed byte.
 *
 *   vtdec_test decode FILE.h264 [async] [attach] [threads=N] [realtime] [reduced=F] [sw] [forcesync|noforcesync] [pts] [flags=0xN] [hold=N] [dest=ca]
 *       Decodes an Annex-B H.264 file with VideoToolbox into 420v IOSurface buffers (the decoder is
 *       H264SW here, as for Safari) and prints, per sample in decode order, the NAL header byte and
 *       the number of zero bytes in the picture. Video-range output never has a zero luma byte.
 *       On the phone the session is remote: H264SW runs in mediaserverd, as for Safari (the hook there
 *       logs these samples too). H264SW has no setProperty, so threads=/realtime/reduced= never reach it,
 *       and forcesync sets the preference for this process's user, not for mediaserverd.
 *       async: flags 0x9 like Safari (asynchronous + temporal processing); flags=0xN: these flags
 *       (Safari: 0x9, then 0xd); attach: mark samples NotSync / DependsOnOthers / IsDependedOnByOthers
 *       from the NAL headers; pts: presentation times from the picture order count, reordered as a
 *       demuxer gives them (otherwise pts = dts); hold=N: keep the last N pictures out of the pool, as
 *       the consumer does; dest=ca: also ask for OpenGL ES and Metal compatible buffers;
 *       forcesync/noforcesync set or remove ForceSyncH264SWDecoder (com.apple.coremedia, this user)
 *       before the session is made; sw: no hardware decoder (for a control run on a Mac).
 *       Clips: vt.h264 (VideoToolbox, P/B/b), pyramid.h264 (x264, one reference B), jm-*.h264 (the JM
 *       reference encoder: Safari's stream's order I | P8 B4 B2 b1 b3 B6 b5 b7, two levels of reference B).
 *
 * Build (host): see build.sh next to this file.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <mach/mach.h>
/* (the iOS SDK's mach/mach_vm.h declares nothing; libsystem_kernel has these) */
extern kern_return_t mach_vm_allocate(vm_map_t, mach_vm_address_t *, mach_vm_size_t, int);
extern kern_return_t mach_vm_deallocate(vm_map_t, mach_vm_address_t, mach_vm_size_t);
extern kern_return_t mach_vm_purgable_control(vm_map_t, mach_vm_address_t, vm_purgable_t, int *);
extern kern_return_t mach_vm_remap(vm_map_t, mach_vm_address_t *, mach_vm_size_t, mach_vm_offset_t, int, vm_map_t,
    mach_vm_address_t, boolean_t, vm_prot_t *, vm_prot_t *, vm_inherit_t);
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------- vm */

static int vm_checks, vm_failed;

static void vm_result(const char *name, size_t bad, const char *detail)
{
	vm_checks++;
	if (bad) {
		vm_failed++;
		printf("vm: %s: FAILED, %zu bytes wrong%s%s\n", name, bad, detail ? ": " : "", detail ? detail : "");
	} else {
		printf("vm: %s: OK\n", name);
	}
}

static void fill(unsigned char *p, size_t n, unsigned seed)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (unsigned char)(((i >> 4) * 131 + seed * 29) | 1);	/* never zero */
}

/* bytes that differ from fill(seed); *zeros counts those that are zero */
static size_t check(const unsigned char *p, size_t n, unsigned seed, size_t *zeros, size_t *first)
{
	size_t bad = 0;
	*zeros = 0;
	*first = (size_t)-1;
	for (size_t i = 0; i < n; i++) {
		unsigned char want = (unsigned char)(((i >> 4) * 131 + seed * 29) | 1);
		if (p[i] != want) {
			if (bad++ == 0)
				*first = i;
			if (p[i] == 0)
				(*zeros)++;
		}
	}
	return bad;
}

#define CHURN_BLOCKS 8
struct churn { unsigned id; size_t bad, zeros; char detail[160]; };

/* what a decoder with a growing DPB does: picture-sized blocks allocated, filled, freed, in turn */
static void *churn(void *arg)
{
	struct churn *c = arg;
	static const size_t sizes[] = { 282624, 401408, 311296, 98304, 45056, 1052672 };
	unsigned char *block[CHURN_BLOCKS] = { 0 };
	size_t size[CHURN_BLOCKS] = { 0 };
	unsigned seed[CHURN_BLOCKS] = { 0 };
	unsigned rnd = 12345 + c->id * 7919;

	for (unsigned it = 0; it < 3000 && !c->bad; it++) {
		rnd = rnd * 1103515245 + 12345;
		unsigned k = (rnd >> 16) % CHURN_BLOCKS;
		if (block[k])
			free(block[k]);
		size[k] = sizes[(rnd >> 8) % 6];
		block[k] = (it & 1) ? calloc(1, size[k]) : malloc(size[k]);
		if (!block[k]) {
			snprintf(c->detail, sizeof c->detail, "allocation of %zu failed at step %u", size[k], it);
			c->bad = 1;
			break;
		}
		if (it & 1) {
			size_t nz = 0;
			for (size_t i = 0; i < size[k]; i++)
				nz += block[k][i] != 0;
			if (nz) {
				snprintf(c->detail, sizeof c->detail, "calloc(%zu) at step %u returned %zu non-zero bytes", size[k], it, nz);
				c->bad = nz;
				break;
			}
		}
		seed[k] = it;
		fill(block[k], size[k], seed[k]);
		for (unsigned j = 0; j < CHURN_BLOCKS; j++) {
			size_t zeros, first;
			size_t bad = block[j] ? check(block[j], size[j], seed[j], &zeros, &first) : 0;
			if (bad) {
				snprintf(c->detail, sizeof c->detail, "step %u: live block %u (%zu bytes at %p) lost %zu bytes (%zu now zero) from +%zu",
				    it, j, size[j], (void *)block[j], bad, zeros, first);
				c->bad = bad;
				c->zeros = zeros;
				break;
			}
		}
	}
	for (unsigned j = 0; j < CHURN_BLOCKS; j++)
		free(block[j]);
	return NULL;
}

static void vm_madvise(const char *name, int advice, size_t pg)
{
	size_t n = 64 * 16384;
	unsigned char *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	char detail[160];
	size_t zeros, first, bad;
	size_t lo = 16 * 16384 + pg, hi = lo + 8 * pg;	/* pg-aligned, inside */

	fill(p, n, 7);
	if (madvise(p + lo, hi - lo, advice) != 0) {
		snprintf(detail, sizeof detail, "madvise errno %d", errno);
		vm_result(name, 1, detail);
		munmap(p, n);
		return;
	}
	if (advice == MADV_FREE_REUSABLE)
		madvise(p + lo, hi - lo, MADV_FREE_REUSE);
	bad = check(p, lo, 7, &zeros, &first) + check(p + hi, n - hi, 7, &zeros, &first);
	snprintf(detail, sizeof detail, "bytes outside the advised range [+%zu, +%zu) changed (%zu zero)", lo, hi, zeros);
	vm_result(name, bad, detail);
	munmap(p, n);
}

static void vm_tests(void)
{
	int64_t v64 = 0;
	int v = 0;
	size_t len;

	len = sizeof v64; sysctlbyname("hw.pagesize", &v64, &len, NULL, 0);
	printf("vm: getpagesize %d, vm_page_size %lu, vm_kernel_page_size %lu, hw.pagesize %lld\n",
	    getpagesize(), (unsigned long)vm_page_size, (unsigned long)vm_kernel_page_size, v64);
	len = sizeof v64; v64 = 0; sysctlbyname("hw.cachelinesize", &v64, &len, NULL, 0);
	len = sizeof v; sysctlbyname("hw.activecpu", &v, &len, NULL, 0);
	printf("vm: hw.cachelinesize %lld, hw.activecpu %d\n", v64, v);
	{	/* DEVELOPMENT kernels zero-fill (every other) page on madvise(MADV_FREE*) at once */
		int d = -1, ds = -1;
		len = sizeof d; sysctlbyname("vm.madvise_free_debug", &d, &len, NULL, 0);
		len = sizeof ds; sysctlbyname("vm.madvise_free_debug_sometimes", &ds, &len, NULL, 0);
		printf("vm: madvise_free_debug %d, madvise_free_debug_sometimes %d\n", d, ds);
	}

	vm_statistics64_data_t s0, s1;
	mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
	host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&s0, &cnt);

	struct churn one = { .id = 0 };
	churn(&one);
	vm_result("malloc/calloc/free churn, one thread", one.bad, one.detail[0] ? one.detail : NULL);

	struct churn three[3] = { { .id = 1 }, { .id = 2 }, { .id = 3 } };
	pthread_t t[3];
	for (int i = 0; i < 3; i++)
		pthread_create(&t[i], NULL, churn, &three[i]);
	for (int i = 0; i < 3; i++)
		pthread_join(t[i], NULL);
	for (int i = 0; i < 3; i++) {
		char name[64];
		snprintf(name, sizeof name, "malloc/calloc/free churn, thread %d of 3", i + 1);
		vm_result(name, three[i].bad, three[i].detail[0] ? three[i].detail : NULL);
	}

	vm_madvise("madvise REUSABLE+REUSE, 4K-aligned", MADV_FREE_REUSABLE, 4096);
	vm_madvise("madvise REUSABLE+REUSE, 16K-aligned", MADV_FREE_REUSABLE, 16384);
	/* MADV_FREE works on whole user pages (16K): XNU rounds the range out to VM_MAP_PAGE_MASK, also on
	 * a real A11, so a 4K-aligned FREE legitimately discards its 16K neighbours. Nothing does that here:
	 * libmalloc only uses REUSABLE (4K-exact), libdispatch FREEs whole 16K groups. */
	vm_madvise("madvise FREE, 16K-aligned", MADV_FREE, 16384);
	vm_madvise("madvise DONTNEED, 4K-aligned", MADV_DONTNEED, 4096);
	vm_madvise("madvise CAN_REUSE, 16K-aligned", MADV_CAN_REUSE, 16384);

	{	/* purgeable: volatile then nonvolatile must keep the data unless the old state says EMPTY */
		mach_vm_address_t a = 0;
		mach_vm_size_t n = 1 << 20;
		kern_return_t kr = mach_vm_allocate(mach_task_self(), &a, n, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE);
		char detail[160];
		if (kr != KERN_SUCCESS) {
			snprintf(detail, sizeof detail, "mach_vm_allocate purgeable: %d", kr);
			vm_result("purgeable volatile/nonvolatile", 1, detail);
		} else {
			int state = VM_PURGABLE_VOLATILE;
			size_t zeros = 0, first = 0, bad;
			fill((unsigned char *)a, n, 9);
			mach_vm_purgable_control(mach_task_self(), a, VM_PURGABLE_SET_STATE, &state);
			state = VM_PURGABLE_NONVOLATILE;
			mach_vm_purgable_control(mach_task_self(), a, VM_PURGABLE_SET_STATE, &state);
			bad = state == VM_PURGABLE_EMPTY ? 0 : check((unsigned char *)a, n, 9, &zeros, &first);
			snprintf(detail, sizeof detail, "old state %d, %zu zero, from +%zu", state, zeros, first);
			vm_result("purgeable volatile/nonvolatile", bad, detail);
			printf("vm: purgeable: the old state was %s\n", state == VM_PURGABLE_EMPTY ? "EMPTY (purged)" : "kept");
			mach_vm_deallocate(mach_task_self(), a, n);
		}
	}

	{	/* vm_copy and mach_vm_remap(copy) are copy-on-write: each side keeps its own bytes */
		size_t n = 2 << 20, zeros, first, bad;
		unsigned char *src = malloc(n), *dst = malloc(n);
		mach_vm_address_t r = 0;
		vm_prot_t cur, max;
		fill(src, n, 3);
		vm_copy(mach_task_self(), (vm_address_t)src, n, (vm_address_t)dst);
		mach_vm_remap(mach_task_self(), &r, n, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (mach_vm_address_t)src, TRUE,
		    &cur, &max, VM_INHERIT_DEFAULT);
		fill(src, n, 4);
		bad = check(dst, n, 3, &zeros, &first);
		vm_result("vm_copy copy-on-write", bad, NULL);
		bad = r ? check((unsigned char *)r, n, 3, &zeros, &first) : 1;
		vm_result("mach_vm_remap copy-on-write", bad, NULL);
		bad = check(src, n, 4, &zeros, &first);
		vm_result("source after both copies", bad, NULL);
		if (r)
			mach_vm_deallocate(mach_task_self(), r, n);
		free(src);
		free(dst);
	}

	cnt = HOST_VM_INFO64_COUNT;
	host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&s1, &cnt);
	printf("vm: during the checks: compressions %llu, decompressions %llu, purges %llu, swapins %llu, free pages %u, compressor pages %u\n",
	    s1.compressions - s0.compressions, s1.decompressions - s0.decompressions, s1.purges - s0.purges,
	    s1.swapins - s0.swapins, s1.free_count, s1.compressor_page_count);
	if (vm_failed)
		printf("vm: %d of %d checks FAILED\n", vm_failed, vm_checks);
	else
		printf("vm: all %d checks passed\n", vm_checks);
}

/* ---------------------------------------------------------------- decode */

struct nal { const unsigned char *p; size_t n; };
struct au { struct nal nal[16]; int count; int vcl, ref_idc, idr, index, poc, frame; unsigned char first; };

/* ---- just enough of the SPS and the slice header for pic_order_cnt_type 0, to give the samples the
 * presentation times a demuxer gives them (B pictures: later than their decode times, reordered) */
struct bits { unsigned char b[512]; size_t n, pos; };

static void bits_init(struct bits *r, const struct nal *x)
{
	int zeros = 0;
	r->n = 0;
	r->pos = 8;	/* past the NAL header */
	for (size_t i = 0; i < x->n && r->n < sizeof r->b; i++) {
		if (zeros >= 2 && x->p[i] == 3) {
			zeros = 0;
			continue;
		}
		r->b[r->n++] = x->p[i];
		zeros = x->p[i] == 0 ? zeros + 1 : 0;
	}
}

static unsigned u(struct bits *r, int n)
{
	unsigned v = 0;
	while (n-- > 0) {
		v = v << 1 | (r->pos / 8 < r->n ? (r->b[r->pos / 8] >> (7 - r->pos % 8)) & 1 : 0);
		r->pos++;
	}
	return v;
}

static unsigned ue(struct bits *r)
{
	int z = 0;
	while (z < 32 && !u(r, 1))
		z++;
	return (1u << z) - 1 + u(r, z);
}

static struct { int frame_num_bits, poc_type, poc_lsb_bits, frame_mbs_only; } sps_info;

static void parse_sps(const struct nal *x)
{
	struct bits r;
	bits_init(&r, x);
	unsigned profile = u(&r, 8);
	u(&r, 16);
	ue(&r);
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
	    profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134) {
		unsigned chroma = ue(&r);
		if (chroma == 3)
			u(&r, 1);
		ue(&r);
		ue(&r);
		u(&r, 1);
		if (u(&r, 1))	/* scaling matrices */
			for (int i = 0; i < (chroma == 3 ? 12 : 8); i++)
				if (u(&r, 1))
					for (int j = 0, last = 8, next = 8; j < (i < 6 ? 16 : 64) && next; j++) {
						unsigned v = ue(&r);
						int delta = v & 1 ? (int)(v + 1) / 2 : -(int)(v / 2);
						next = (last + delta + 256) % 256;
						last = next ? next : last;
					}
	}
	sps_info.frame_num_bits = (int)ue(&r) + 4;
	sps_info.poc_type = (int)ue(&r);
	if (sps_info.poc_type == 0)
		sps_info.poc_lsb_bits = (int)ue(&r) + 4;
	else
		return;
	ue(&r);
	u(&r, 1);
	ue(&r);
	ue(&r);
	sps_info.frame_mbs_only = (int)u(&r, 1);
}

/* the picture order count of a slice (8.2.1.1; no memory_management_control_operation 5) */
static int slice_poc(const struct nal *x, int idr, int ref)
{
	static int prev_msb, prev_lsb;
	struct bits r;
	if (sps_info.poc_type != 0 || !sps_info.poc_lsb_bits)
		return 0;
	bits_init(&r, x);
	ue(&r);
	ue(&r);
	ue(&r);
	u(&r, sps_info.frame_num_bits);
	if (!sps_info.frame_mbs_only && u(&r, 1))
		u(&r, 1);
	if (idr) {
		ue(&r);
		prev_msb = prev_lsb = 0;
	}
	int lsb = (int)u(&r, sps_info.poc_lsb_bits), max = 1 << sps_info.poc_lsb_bits, msb = prev_msb;
	if (lsb < prev_lsb && prev_lsb - lsb >= max / 2)
		msb += max;
	else if (lsb > prev_lsb && lsb - prev_lsb > max / 2)
		msb -= max;
	if (ref) {
		prev_msb = msb;
		prev_lsb = lsb;
	}
	return msb + lsb;
}

static struct nal next_nal(const unsigned char *d, size_t len, size_t *pos)
{
	struct nal r = { 0 };
	size_t i = *pos;
	while (i + 3 <= len && !(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1))
		i++;
	if (i + 3 > len) {
		*pos = len;
		return r;
	}
	i += 3;
	size_t j = i;
	while (j + 3 <= len && !(d[j] == 0 && d[j + 1] == 0 && (d[j + 2] == 1 || (d[j + 2] == 0 && j + 3 < len && d[j + 3] == 1))))
		j++;
	if (j + 3 > len)
		j = len;
	r.p = d + i;
	r.n = j - i;
	*pos = j;
	return r;
}

static volatile int frames_out, frames_zero, first_zero = -1;
/* hold=N: the last N pictures are kept, as a consumer (FigVideoQueue, CoreAnimation) keeps the ones it shows */
static CVPixelBufferRef held[64];
static int hold, held_next;

static void output(void *ctx, void *ref, OSStatus status, VTDecodeInfoFlags info, CVImageBufferRef img, CMTime pts, CMTime dur)
{
	struct au *a = ref;
	(void)ctx; (void)pts; (void)dur;
	if (!img) {
		printf("decode: sample %d nal %02x ref_idc %d: status %d, info 0x%x, no picture\n", a->index, a->first, a->ref_idc, (int)status, (unsigned)info);
		frames_out++;
		return;
	}
	CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
	size_t w = CVPixelBufferGetWidthOfPlane(img, 0), h = CVPixelBufferGetHeightOfPlane(img, 0);
	size_t yb = CVPixelBufferGetBytesPerRowOfPlane(img, 0), cb = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
	size_t cw = CVPixelBufferGetWidthOfPlane(img, 1), ch = CVPixelBufferGetHeightOfPlane(img, 1);
	const unsigned char *y = CVPixelBufferGetBaseAddressOfPlane(img, 0), *c = CVPixelBufferGetBaseAddressOfPlane(img, 1);
	size_t yz = 0, cz = 0;
	unsigned long long sum = 0;
	for (size_t r = 0; r < h; r++)
		for (size_t x = 0; x < w; x++) {
			yz += y[r * yb + x] == 0;
			sum += y[r * yb + x];
		}
	for (size_t r = 0; r < ch; r++)
		for (size_t x = 0; x < cw * 2; x++)
			cz += c[r * cb + x] == 0;
	IOSurfaceRef s = CVPixelBufferGetIOSurface(img);
	CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
	if (yz > w * h / 100 && frames_zero++ == 0)
		first_zero = a->index;
	if (hold) {
		if (held[held_next])
			CVPixelBufferRelease(held[held_next]);
		held[held_next] = CVPixelBufferRetain(img);
		held_next = (held_next + 1) % hold;
	}
	printf("decode: sample %d nal %02x ref_idc %d poc %d: status %d, info 0x%x, %zux%zu surface %u: Y %zu of %zu zero, CbCr %zu of %zu zero, Y mean %.1f\n",
	    a->index, a->first, a->ref_idc, a->poc, (int)status, (unsigned)info, w, h, s ? IOSurfaceGetID(s) : 0, yz, w * h, cz, cw * ch * 2,
	    w * h ? (double)sum / (double)(w * h) : 0.0);
	frames_out++;
}

static int decode(int argc, char **argv)
{
	const char *file = argv[2];
	int async = 0, attach = 0, threads = 0, realtime = 0, sw = 0, pts = 0, ca = 0, flags = -1;
	double reduced = 0;
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "async")) async = 1;
		else if (!strcmp(argv[i], "pts")) pts = 1;
		else if (!strncmp(argv[i], "flags=", 6)) flags = (int)strtol(argv[i] + 6, NULL, 0);
		else if (!strncmp(argv[i], "hold=", 5)) hold = atoi(argv[i] + 5) > 64 ? 64 : atoi(argv[i] + 5);
		else if (!strcmp(argv[i], "dest=ca")) ca = 1;
		else if (!strcmp(argv[i], "attach")) attach = 1;
		else if (!strncmp(argv[i], "threads=", 8)) threads = atoi(argv[i] + 8);
		else if (!strcmp(argv[i], "realtime")) realtime = 1;
		else if (!strcmp(argv[i], "sw")) sw = 1;
		else if (!strncmp(argv[i], "reduced=", 8)) reduced = atof(argv[i] + 8);
		else if (!strcmp(argv[i], "forcesync") || !strcmp(argv[i], "noforcesync")) {
			CFPreferencesSetAppValue(CFSTR("ForceSyncH264SWDecoder"), argv[i][0] == 'f' ? kCFBooleanTrue : NULL, CFSTR("com.apple.coremedia"));
			CFPreferencesAppSynchronize(CFSTR("com.apple.coremedia"));
		} else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}
	Boolean valid = false;
	Boolean force = CFPreferencesGetAppBooleanValue(CFSTR("ForceSyncH264SWDecoder"), CFSTR("com.apple.coremedia"), &valid);
	printf("decode: %s: async %d, attach %d, threads %d, realtime %d, reduced %.2f, pts %d, flags %#x, hold %d, dest %s; "
	    "ForceSyncH264SWDecoder as this process reads it: %s\n", file, async, attach, threads, realtime, reduced, pts,
	    flags >= 0 ? (unsigned)flags : async ? 9u : 0u, hold, ca ? "ca" : "plain", valid ? (force ? "YES" : "NO") : "not set");

	FILE *f = fopen(file, "rb");
	if (!f) { perror(file); return 1; }
	fseek(f, 0, SEEK_END);
	size_t len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *d = malloc(len);
	if (fread(d, 1, len, f) != len) { perror("read"); return 1; }
	fclose(f);

	struct au *aus = calloc(4096, sizeof *aus);
	int n = 0;
	struct nal sps = { 0 }, pps = { 0 };
	size_t pos = 0;
	for (;;) {
		struct nal x = next_nal(d, len, &pos);
		if (!x.p)
			break;
		if (!x.n)
			continue;
		int type = x.p[0] & 0x1f;
		int vcl = type == 1 || type == 5;
		if (type == 7 && !sps.p) { sps = x; parse_sps(&x); }
		if (type == 8 && !pps.p) pps = x;
		/* a new access unit: a non-VCL unit after the picture, or the first slice of the next one */
		if (n == 0 || (aus[n - 1].vcl && (!vcl || (x.n > 1 && (x.p[1] & 0x80)))))
			n++;
		if (n > 4096)
			break;
		struct au *a = &aus[n - 1];
		if (type == 9)
			continue;
		if (a->count < 16)
			a->nal[a->count++] = x;
		if (vcl) {
			if (!a->vcl) {
				a->first = x.p[0];
				a->poc = slice_poc(&x, type == 5, (x.p[0] >> 5) & 3);
			}
			a->vcl = 1;
			if (((x.p[0] >> 5) & 3) > a->ref_idc)
				a->ref_idc = (x.p[0] >> 5) & 3;
			a->idr |= type == 5;
		}
	}
	if (!sps.p || !pps.p) { fprintf(stderr, "no SPS/PPS\n"); return 1; }
	/* pts: display frame numbers from the picture order count (2 per frame), each IDR after the last shown;
	 * delayed so that no picture is shown before it is decoded */
	int base = 0, top = -1, delay = 0;
	for (int i = 0, k = 0; i < n; i++) {
		struct au *a = &aus[i];
		if (!a->vcl)
			continue;
		if (a->idr)
			base = top + 1;
		a->frame = base + a->poc / 2;
		top = a->frame > top ? a->frame : top;
		delay = k - a->frame > delay ? k - a->frame : delay;
		k++;
	}
	if (pts)
		printf("decode: presentation times from the picture order count (poc type %d, %d bits), %d frames after decode\n",
		    sps_info.poc_type, sps_info.poc_lsb_bits, delay);

	CMVideoFormatDescriptionRef fmt = NULL;
	const uint8_t *ps[2] = { sps.p, pps.p };
	size_t psn[2] = { sps.n, pps.n };
	OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, ps, psn, 4, &fmt);
	if (st) { fprintf(stderr, "format description: %d\n", (int)st); return 1; }

	int pf = '420v';
	CFNumberRef pfn = CFNumberCreate(NULL, kCFNumberIntType, &pf);
	CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	/* dest=ca: also what a picture for CoreAnimation / Metal / GL asks for */
	const void *k[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey,
#if TARGET_OS_IPHONE
	    kCVPixelBufferOpenGLESCompatibilityKey,
#else
	    kCVPixelBufferIOSurfaceOpenGLTextureCompatibilityKey,
#endif
	    kCVPixelBufferMetalCompatibilityKey };
	const void *vv[] = { pfn, empty, kCFBooleanTrue, kCFBooleanTrue };
	CFDictionaryRef dst = CFDictionaryCreate(NULL, k, vv, ca ? 4 : 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	VTDecompressionOutputCallbackRecord cb = { output, NULL };
	VTDecompressionSessionRef sess = NULL;
	/* sw: no hardware decoder (EnableHardwareAcceleratedVideoDecoder = NO): Apple's software H.264 on a Mac */
	const void *swk[] = { CFSTR("EnableHardwareAcceleratedVideoDecoder") }, *swv[] = { kCFBooleanFalse };
	CFDictionaryRef spec = sw ? CFDictionaryCreate(NULL, swk, swv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
	st = VTDecompressionSessionCreate(NULL, fmt, spec, dst, &cb, &sess);
	if (st) { fprintf(stderr, "VTDecompressionSessionCreate: %d\n", (int)st); return 1; }
	if (threads) {
		CFNumberRef t = CFNumberCreate(NULL, kCFNumberIntType, &threads);
		printf("decode: ThreadCount %d: %d\n", threads, (int)VTSessionSetProperty(sess, kVTDecompressionPropertyKey_ThreadCount, t));
	}
	if (realtime)
		printf("decode: RealTime: %d\n", (int)VTSessionSetProperty(sess, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue));
	if (reduced > 0) {
		CFNumberRef r = CFNumberCreate(NULL, kCFNumberDoubleType, &reduced);
		printf("decode: ReducedFrameDelivery %.2f: %d\n", reduced, (int)VTSessionSetProperty(sess, kVTDecompressionPropertyKey_ReducedFrameDelivery, r));
	}

	int submitted = 0;
	for (int i = 0; i < n; i++) {
		struct au *a = &aus[i];
		if (!a->vcl)
			continue;
		size_t total = 0;
		for (int j = 0; j < a->count; j++)
			total += 4 + a->nal[j].n;
		unsigned char *buf = malloc(total), *w = buf;
		for (int j = 0; j < a->count; j++) {
			uint32_t l = (uint32_t)a->nal[j].n;
			w[0] = l >> 24; w[1] = l >> 16; w[2] = l >> 8; w[3] = l;
			memcpy(w + 4, a->nal[j].p, l);
			w += 4 + l;
		}
		CMBlockBufferRef bb = NULL;
		CMBlockBufferCreateWithMemoryBlock(NULL, buf, total, kCFAllocatorMalloc, NULL, 0, total, 0, &bb);
		CMSampleTimingInfo tim = { CMTimeMake(1, 30), CMTimeMake(pts ? a->frame + delay : submitted, 30), CMTimeMake(submitted, 30) };
		CMSampleBufferRef sb = NULL;
		CMSampleBufferCreateReady(NULL, bb, fmt, 1, 1, &tim, 1, &total, &sb);
		if (attach) {
			CFArrayRef arr = CMSampleBufferGetSampleAttachmentsArray(sb, true);
			CFMutableDictionaryRef at = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(arr, 0);
			if (!a->idr)
				CFDictionarySetValue(at, kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
			CFDictionarySetValue(at, kCMSampleAttachmentKey_DependsOnOthers, a->idr ? kCFBooleanFalse : kCFBooleanTrue);
			CFDictionarySetValue(at, kCMSampleAttachmentKey_IsDependedOnByOthers, a->ref_idc ? kCFBooleanTrue : kCFBooleanFalse);
		}
		a->index = submitted;
		VTDecodeInfoFlags info = 0;
		VTDecodeFrameFlags fl = flags >= 0 ? (VTDecodeFrameFlags)flags :
		    async ? (kVTDecodeFrame_EnableAsynchronousDecompression | kVTDecodeFrame_EnableTemporalProcessing) : 0;
		st = VTDecompressionSessionDecodeFrame(sess, sb, fl, a, &info);
		if (st)
			printf("decode: sample %d (nal %02x): DecodeFrame %d\n", submitted, a->first, (int)st);
		CFRelease(sb);
		CFRelease(bb);
		submitted++;
		if (!(fl & kVTDecodeFrame_EnableAsynchronousDecompression))
			VTDecompressionSessionWaitForAsynchronousFrames(sess);
	}
	VTDecompressionSessionFinishDelayedFrames(sess);
	VTDecompressionSessionWaitForAsynchronousFrames(sess);
	printf("decode: %d samples in, %d pictures out, %d more than 1%% zero (the first: sample %d)\n", submitted, frames_out, frames_zero, first_zero);
	VTDecompressionSessionInvalidate(sess);
	for (int i = 0; i < hold; i++)
		if (held[i])
			CVPixelBufferRelease(held[i]);
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc >= 2 && !strcmp(argv[1], "vm")) {
		vm_tests();
		return vm_failed != 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "decode"))
		return decode(argc, argv);
	fprintf(stderr, "usage: %s vm | decode FILE.h264 [async] [attach] [threads=N] [realtime] [reduced=F] [sw] [forcesync|noforcesync] [pts] [flags=0xN] [hold=N] [dest=ca]\n", argv[0]);
	return 2;
}
