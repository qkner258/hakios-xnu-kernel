/* SM8250 experimental VIG0..3/LM composition. Included by hakios.display.c.
 * Two immutable source banks: upload before flush, reuse only after flip away.
 * A private SMMU context maps those banks below 4 GiB and keeps splash identity
 * mapped. No GPU context, page table or submission interface is changed.
 */
#define COMPOSE_IOVA 0x60000000u
#define COMPOSE_BANK_STEP 0x4000000u
#define COMPOSE_LAYER_STEP 0x1000000u
#define COMPOSE_TABLE_BYTES (69u * 4096u) /* root, four L2, 64 L3 */
static vm_offset_t compose_smmu, compose_cb, compose_tables, compose_sources[2][4];
static uint32_t compose_route, compose_old_route, compose_flush;
static bool compose_ready, compose_routed;
static uint32_t compose_lm[12], compose_ctl[5], compose_clk[4];
static struct dpu_geo compose_geo, compose_extra_geo[3];
static const uint32_t compose_pipe_regs[] = {
	0x014, 0x018, 0x01c, 0x020, 0x028, 0x030, 0x034, 0x038,
	0x044, 0x048, 0x060, 0x064, 0x068, 0x06c, 0x070, 0x074, 0x078,
	0x0b4, 0x134, 0x170, 0x1a00
};
static uint32_t compose_saved[4][sizeof(compose_pipe_regs) / sizeof(uint32_t)];

/* Hardware admission, before allocations or writes. Only the observed v28
 * single-LM VIG0 handoff is supported. Reject another owner's staged pipes. */
static int
compose_handoff(void)
{
	if (!dpu_ready || !dpu_scaled || dpu_pipe != sspp_off[0] || dpu_lm != 0 ||
	    rd(mdp, dpu_ctl + CTL_LAYER(0)) != 0x1000005 ||
	    rd(mdp, dpu_ctl + CTL_LAYER_EXT(0)) || rd(mdp, dpu_ctl + CTL_LAYER_EXT2(0)) ||
	    rd(mdp, dpu_ctl + CTL_LAYER_EXT3(0))) return 0;
	for (unsigned c = 0; c < 6; c++) {
		for (unsigned l = 0; l < 6; l++) {
			if (DPU_CTL(c) == dpu_ctl && l == 0) continue;
			if ((rd(mdp, DPU_CTL(c) + CTL_LAYER(l)) & ~CTL_MIXER_BORDER_OUT) ||
			    rd(mdp, DPU_CTL(c) + CTL_LAYER_EXT(l)) || rd(mdp, DPU_CTL(c) + CTL_LAYER_EXT2(l)) ||
			    rd(mdp, DPU_CTL(c) + CTL_LAYER_EXT3(l))) return 0;
		}
	}
	return 1;
}

int
hakios_display_compose_start(void)
{
	if (!display_switch_file("dpu-compose")) return 0;
	const char *why = "handoff";
	uint32_t page, pages, ncb, nsmr, cb = UINT32_MAX, match = UINT32_MAX;
	vm_offset_t s;
	if (!compose_handoff()) goto fail;
	why = "smmu-layout";
	s = ml_io_map_unmappable(APPS_SMMU_PHYS, 0x1000, DPU_WIMG_IO);
	if (!s) goto fail;
	uint32_t idr1 = rd(s, 0x24);
	page = (idr1 >> 31) ? 0x10000 : 0x1000;
	pages = 2u << ((idr1 >> 28) & 7);
	ncb = idr1 & 0xff;
	nsmr = rd(s, 0x20) & 0xff;
	if (!ncb || ncb > 128 || !nsmr || pages * page * 2u > 0x2000000) goto fail;
	s = ml_io_map_unmappable(APPS_SMMU_PHYS, pages * page * 2u, DPU_WIMG_IO);
	if (!s) goto fail;
	why = "smmu-stream";
	for (unsigned i = 0; i < nsmr; i++) {
		uint32_t smr = rd(s, 0x800 + 4 * i), mask = (smr >> 16) & 0x7fff;
		if (!(smr & (1u << 31)) || ((smr ^ DISPLAY_SID) & ~mask & 0x7fff)) continue;
		/* DT's 0x820/0x402 is display-only. Never replace a broader stream. */
		if (mask != 0x402 || (smr & ~mask & 0x7fff) != DISPLAY_SID || match != UINT32_MAX) goto fail;
		match = i;
	}
	if (match == UINT32_MAX) goto fail;
	why = "smmu-context";
	for (unsigned c = 0; c < ncb && cb == UINT32_MAX; c++) {
		bool used = false;
		for (unsigned i = 0; i < nsmr; i++) {
			uint32_t route = rd(s, 0xc00 + 4 * i);
			used |= (rd(s, 0x800 + 4 * i) & (1u << 31)) && !(route & (3u << 16)) && (route & 0xff) == c;
		}
		if (!used && !(rd(s, pages * page + c * page) & 1)) cb = c;
	}
	if (cb == UINT32_MAX) goto fail;
	why = "allocation";
	if (kmem_alloc(kernel_map, &compose_tables, round_page(COMPOSE_TABLE_BYTES),
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) goto fail;
	for (unsigned b = 0; b < 2; b++) {
		for (unsigned l = 0; l < 4; l++) {
			if (kmem_alloc(kernel_map, &compose_sources[b][l], UMI_DPU_SOURCE_BYTES,
			    KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) goto fail;
		}
	}
	uint64_t *root = (uint64_t *)compose_tables;
	/* 39-bit VA, 4K granule, read-only outer-shared normal NC leaf mappings.
	 * Keep 0..4 GiB identity mapped except our reserved snapshot IOVA window. */
	for (unsigned g = 0; g < 4; g++) {
		uint64_t *l2 = (uint64_t *)(compose_tables + (1 + g) * 4096);
		root[g] = kvtophys((vm_offset_t)l2) | 3;
		for (unsigned n = 0; n < 512; n++) l2[n] = ((uint64_t)g << 30 | (uint64_t)n << 21) | 0xec1;
	}
	for (unsigned n = 0; n < 64; n++) {
		uint32_t va = COMPOSE_IOVA + n * 0x200000u;
		uint64_t *l2 = (uint64_t *)(compose_tables + (1 + (va >> 30)) * 4096);
		uint64_t *l3 = (uint64_t *)(compose_tables + (5 + n) * 4096);
		l2[(va >> 21) & 511] = kvtophys((vm_offset_t)l3) | 3;
		unsigned b = n / 32, l = (n % 32) / 8, chunk = n % 8;
		if (chunk >= UMI_DPU_SOURCE_BYTES / 0x200000u) continue; /* unmapped guard after each source */
		for (unsigned p = 0; p < 512; p++) {
			l3[p] = kvtophys(compose_sources[b][l] + chunk * 0x200000u + p * 4096u) | 0xec3;
		}
	}
	flush_dcache64(compose_tables, COMPOSE_TABLE_BYTES, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	why = "idle-window";
	if (!dpu_wait(100000000) || !hakios_display_wait_copy(100000, 100000000)) goto fail;
	compose_smmu = s;
	compose_cb = s + pages * page + cb * page;
	compose_route = 0xc00 + 4 * match;
	compose_old_route = rd(s, compose_route);
	/* Same ARM SMMU v2 stage-1 setup as the established GPU driver, separate CB. */
	wr(s, page + 4 * cb, 0x1f300);
	wr(s, page + 0x800 + 4 * cb, 1);
	wr(compose_cb, 0x10, 0x38011);
	wr(compose_cb, 0x30, 0x803519);
	uint64_t ttbr = kvtophys(compose_tables) | ((uint64_t)(cb + 1) << 48);
	wr(compose_cb, 0x20, (uint32_t)ttbr);
	wr(compose_cb, 0x24, (uint32_t)(ttbr >> 32));
	wr(compose_cb, 0x28, 0);
	wr(compose_cb, 0x2c, 0);
	wr(compose_cb, 0x38, 0xf404ff44);
	wr(compose_cb, 0x3c, 0);
	wr(compose_cb, 0x58, 0xc00001fe);
	wr(compose_cb, 0x610, cb + 1);
	wr(compose_cb, 0x7f0, 1);
	uint64_t tlb_start = mach_absolute_time(), tlb_ns;
	why = "smmu-tlb";
	while (rd(compose_cb, 0x7f4) & 1) {
		absolutetime_to_nanoseconds(mach_absolute_time() - tlb_start, &tlb_ns);
		if (tlb_ns >= 1000000) goto fail; /* disabled context is not routed yet */
		delay_for_interval(2, NSEC_PER_USEC);
	}
	wr(compose_cb, 0x00, 0x1027);
	/* Preserve privilege configuration; replace only TYPE and CBNDX. */
	wr(s, compose_route, (compose_old_route & ~0x300ffu) | cb);
	compose_routed = true;
	dpu_geo_read(&compose_geo);
	compose_ctl[0] = rd(mdp, dpu_ctl + CTL_LAYER(0));
	compose_ctl[1] = rd(mdp, dpu_ctl + CTL_LAYER_EXT(0));
	compose_ctl[2] = rd(mdp, dpu_ctl + CTL_LAYER_EXT2(0));
	compose_ctl[3] = rd(mdp, dpu_ctl + CTL_LAYER_EXT3(0));
	compose_ctl[4] = rd(mdp, dpu_ctl + CTL_FETCH_PIPE_ACTIVE);
	compose_lm[0] = rd(mdp, DPU_LM(0));
	for (unsigned l = 0; l < 3; l++) {
		compose_lm[1 + l * 2] = rd(mdp, DPU_LM(0) + 0x20 + 0x18 * l);
		compose_lm[2 + l * 2] = rd(mdp, DPU_LM(0) + 0x24 + 0x18 * l);
	}
	for (unsigned p = 0; p < 4; p++) {
		compose_clk[p] = rd(mdp, 0x2ac + 8 * p);
		if (p) dpu_geo_read_pipe(&compose_extra_geo[p - 1], sspp_off[p]);
		for (unsigned n = 0; n < sizeof(compose_pipe_regs) / sizeof(uint32_t); n++)
			compose_saved[p][n] = rd(mdp, sspp_off[p] + compose_pipe_regs[n]);
	}
	compose_flush = dpu_flush;
	for (unsigned p = 1; p < 4; p++) compose_flush |= sspp_flush[p];
	compose_ready = true;
	printf("fbcompose-init: enabled=1 banks=2 layers=4 source_bytes=%u cb=%u stream=%u\n", UMI_DPU_SOURCE_BYTES, cb, match);
	return 1;
fail:
	for (unsigned b = 0; b < 2; b++) for (unsigned l = 0; l < 4; l++) {
		if (compose_sources[b][l]) kmem_free(kernel_map, compose_sources[b][l], UMI_DPU_SOURCE_BYTES);
		compose_sources[b][l] = 0;
	}
	if (compose_tables) kmem_free(kernel_map, compose_tables, round_page(COMPOSE_TABLE_BYTES));
	compose_tables = 0;
	printf("fbcompose-init: enabled=0 reason=%s\n", why);
	return 0;
}

int
hakios_display_compose_begin(uint64_t limit_ns)
{
	if (!compose_ready || !dpu_ready || !dpu_wait(limit_ns) || rd(compose_cb, 0x58)) {
		compose_ready = false;
		compose_failed = true; /* next single-pipe flip retires the banks before route rollback */
		return -1;
	}
	return dpu_shown == DPU_NOT_OURS + 1 ? 1 : 0;
}

uintptr_t
hakios_display_compose_source(unsigned bank, unsigned layer)
{
	return compose_sources[bank][layer];
}

/* Pure geometry/QSEED programming shared with host tests. Admission rejects
 * cropped/offscreen destination rectangles; those keep the CPU path's clipping. */
static int
compose_geometry(const struct umi_dpu_layer *l, struct dpu_geo *g)
{
	uint32_t sw = l->src[2], sh = l->src[3], dw = l->dst[2], dh = l->dst[3];
	if (!l->width || !l->height || l->width > 4096 || l->height > 4096 || l->rgba > 1 ||
	    l->stride < l->width * 4 || l->stride > 65535 || (l->stride & 3) ||
	    (uint64_t)l->stride * l->height > UMI_DPU_SOURCE_BYTES ||
	    l->src[0] < 0 || l->src[1] < 0 || !sw || !sh ||
	    (uint64_t)l->src[0] + sw > l->width || (uint64_t)l->src[1] + sh > l->height ||
	    l->dst[0] < 0 || l->dst[1] < 0 || !dw || !dh ||
	    (uint64_t)l->dst[0] + dw > 1080 || (uint64_t)l->dst[1] + dh > 2340 ||
	    sw > dw * 4 || sh > dh * 4 || dw > sw * 20 || dh > sh * 20) return 0;
	*g = compose_geo;
	g->src_size = sh << 16 | sw;
	g->src_xy = (uint32_t)l->src[1] << 16 | l->src[0];
	g->out_size = dh << 16 | dw;
	g->out_xy = (uint32_t)l->dst[1] << 16 | l->dst[0];
	g->ystride0 = l->stride;
	g->op = SSPP_OP_PE_OVERRIDE;
	for (unsigned c = 0; c < 3; c++) {
		g->pe[3 * c] = g->pe[3 * c + 1] = 0;
		g->pe[3 * c + 2] = sh << 16 | sw;
	}
	g->qseed[0] = 1u | 3u << 16 | 1u << 31 | 1u << 14 | 1u << 10 | 1u << 29;
	g->qseed[1] = g->qseed[3] = (uint32_t)(((uint64_t)sw << 21) / dw);
	g->qseed[2] = g->qseed[4] = (uint32_t)(((uint64_t)sh << 21) / dh);
	g->qseed[5] = 4u | 2u << 8 | 4u << 16 | 2u << 24;
	g->qseed[6] = g->qseed[7] = sh << 16 | sw;
	g->qseed[8] = dh << 16 | dw;
	g->qseed[9] = g->qseed[10] = g->qseed[11] = g->qseed[12] = 0;
	return 1;
}

int
hakios_display_compose_validate(const struct umi_dpu_layer *layers, unsigned count)
{
	struct dpu_geo geo;
	if (count < 2 || count > 4) return 0;
	for (unsigned p = 0; p < count; p++) if (!compose_geometry(layers + p, &geo)) return 0;
	return !layers[0].dst[0] && !layers[0].dst[1] && layers[0].dst[2] == 1080 && layers[0].dst[3] == 2340;
}

int
hakios_display_compose(unsigned bank, const struct umi_dpu_layer *layers, unsigned count)
{
	struct dpu_geo geo[4];
	static const unsigned shift[4] = {0, 3, 6, 26};
	if (!compose_ready || !dpu_ready || bank > 1 || count < 2 || count > 4) return 0;
	for (unsigned p = 0; p < count; p++) if (!compose_geometry(layers + p, geo + p)) return 0;
	if (layers[0].dst[0] || layers[0].dst[1] || layers[0].dst[2] != 1080 || layers[0].dst[3] != 2340) return 0;
	uint32_t staged = CTL_MIXER_BORDER_OUT;
	lck_mtx_lock(&dpu_submit_mtx);
	for (unsigned p = 0; p < count; p++) {
		uint32_t pipe = sspp_off[p];
		/* Clone the proven linear BGRA fetch/QoS policy onto the extra VIGs. */
		for (unsigned n = 0; n < sizeof(compose_pipe_regs) / sizeof(uint32_t); n++)
			wr(mdp, pipe + compose_pipe_regs[n], compose_saved[0][n]);
		wr(mdp, 0x2ac + 8 * p, compose_clk[p] | 1); /* force this VIG's core clock */
		wr(mdp, pipe + SSPP_SRC_FORMAT, 0x237ff); /* tight 4x8-bit interleaved, alpha enabled */
		wr(mdp, pipe + 0x034, layers[p].rgba ? 0x03010002 : 0x03020001);
		wr(mdp, pipe + 0x170, 0);
		wr(mdp, pipe + 0x1a00, 0); /* RGB: CSC10 bypass */
		dpu_geo_write_pipe(geo + p, pipe);
		wr(mdp, pipe + SSPP_SRC0_ADDR, COMPOSE_IOVA + bank * COMPOSE_BANK_STEP + p * COMPOSE_LAYER_STEP);
		staged |= (p + 1) << shift[p]; /* BASE, STAGE0, STAGE1, STAGE2 */
		if (p) {
			uint32_t blend = DPU_LM(0) + 0x20 + 0x18 * (p - 1);
			wr(mdp, blend, (2u << 8) | (1u << 10)); /* premultiplied: src + dst*(1-src.a) */
			wr(mdp, blend + 4, 0x00ff0000);        /* FG constant=255, BG=0 */
		}
	}
	wr(mdp, DPU_LM(0), (compose_lm[0] & 0xc0000000) | ((1u << count) - 1));
	wr(mdp, dpu_ctl + CTL_LAYER(0), staged);
	wr(mdp, dpu_ctl + CTL_LAYER_EXT(0), 0);
	wr(mdp, dpu_ctl + CTL_LAYER_EXT2(0), 0);
	wr(mdp, dpu_ctl + CTL_LAYER_EXT3(0), 0);
	wr(mdp, dpu_ctl + CTL_FETCH_PIPE_ACTIVE, compose_ctl[4] | (((1u << count) - 1) << 16));
	compose_active = true;
	dpu_flush = compose_flush;
	wr(mdp, dpu_ctl + CTL_FLUSH, dpu_flush);
	dpu_programmed_at = mach_absolute_time();
	dpu_pending = DPU_NOT_OURS + 1 + bank;
	lck_mtx_unlock(&dpu_submit_mtx);
	return 1;
}

/* The next ordinary flip atomically retires all extra sources and restores
 * single-pipe scaling. Keep the wider flush mask until those changes latch. */
static void
compose_single(void)
{
	if (!compose_active) return;
	wr(mdp, dpu_ctl + CTL_LAYER(0), compose_ctl[0]);
	wr(mdp, dpu_ctl + CTL_LAYER_EXT(0), compose_ctl[1]);
	wr(mdp, dpu_ctl + CTL_LAYER_EXT2(0), compose_ctl[2]);
	wr(mdp, dpu_ctl + CTL_LAYER_EXT3(0), compose_ctl[3]);
	wr(mdp, dpu_ctl + CTL_FETCH_PIPE_ACTIVE, compose_ctl[4]);
	wr(mdp, DPU_LM(0), compose_lm[0]);
	for (unsigned l = 0; l < 3; l++) {
		wr(mdp, DPU_LM(0) + 0x20 + 0x18 * l, compose_lm[1 + l * 2]);
		wr(mdp, DPU_LM(0) + 0x24 + 0x18 * l, compose_lm[2 + l * 2]);
	}
	for (unsigned n = 0; n < sizeof(compose_pipe_regs) / sizeof(uint32_t); n++)
		wr(mdp, sspp_off[0] + compose_pipe_regs[n], compose_saved[0][n]);
	dpu_geo_write(&compose_geo);
	compose_active = false;
}

static void
compose_restore(void)
{
	if (!compose_routed) return;
	compose_failed = false;
	/* Only restore firmware routing once the single GOP source has latched.
	 * On failure keep our tables/banks pinned: never revoke a live DMA mapping. */
	if (!dpu_wait(100000000)) {
		printf("fbcompose-restore: restored=0 reason=latch\n");
		return;
	}
	wr(compose_smmu, compose_route, compose_old_route);
	for (unsigned p = 0; p < 4; p++) {
		if (p) dpu_geo_write_pipe(&compose_extra_geo[p - 1], sspp_off[p]);
		if (p) for (unsigned n = 0; n < sizeof(compose_pipe_regs) / sizeof(uint32_t); n++)
			wr(mdp, sspp_off[p] + compose_pipe_regs[n], compose_saved[p][n]);
		wr(mdp, 0x2ac + 8 * p, compose_clk[p]);
	}
	compose_routed = compose_ready = false;
	printf("fbcompose-restore: restored=1\n");
}
