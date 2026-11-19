/* Private, bounded DMA domain. No global SMMU reset or bypass. Ownership is
 * reserved under the same lock as Wi-Fi, camera and the integrated drivers.
 * Keep this domain and every allocation until reboot, including on failure. */
static vm_offset_t md_pool_cached,md_pool,md_tables;
static size_t md_used;
static uint32_t md_cb;
static unsigned md_cb_index;
static unsigned md_smr=UINT32_MAX;
static bool md_dma_ready;
struct md_buf { uint8_t *p; uint64_t a; size_t size; };
/* MHI requires every ring base aligned to its complete ring length.  Page
 * alignment is insufficient for ER4 (16 KiB) and ER5 (32 KiB). */
static struct md_buf md_alloc_aligned(size_t size,size_t alignment)
{
	struct md_buf b={0};
	size=(size+4095)&~(size_t)4095;
	if (alignment<4096 || (alignment&(alignment-1))) alignment=4096;
	size_t used=(md_used+alignment-1)&~(alignment-1);
	if (!size || used>MD_DMA_SIZE || size>MD_DMA_SIZE-used) return b;
	b.p=(uint8_t *)(md_pool+used); b.a=MD_IOVA+used; b.size=size;
	md_used=used+size;
	return b;
}
static struct md_buf md_alloc(size_t size) { return md_alloc_aligned(size,4096); }
static void md_publish(void) { __asm__ volatile("dsb osh" ::: "memory"); }
static uint64_t *md_table(unsigned n) { return (uint64_t *)(md_tables+n*4096); }
static void md_clean(vm_offset_t va,size_t len)
{
	for (vm_offset_t p=va;p<va+len;p+=64) __asm__ volatile("dc civac, %0" :: "r"(p):"memory");
	__asm__ volatile("dsb sy" ::: "memory");
}
/* A restart keeps the one private domain: install refuses a second time (the
 * endpoint SID is already ours), so the same context bank is re-armed instead.
 * Order matters: acknowledge the context and global faults (both write-1-to-
 * clear), drop the dead link's translations, re-assert SMR/S2CR, and only then
 * let translation and finally bus mastering come back. The page tables and the
 * pool are kept untouched; a translation fault left set would stop the stream
 * again as soon as the new link's first transfer arrives. */
static int md_dma_recover(void)
{
	md_stage="SMMU-recover";
	if (!md_dma_ready || !md_cb || md_smr==UINT32_MAX) return EIO;
	lck_mtx_lock(&hakios_smmu_lock);
	const uint32_t smmu=MD_SMMU;
	uint32_t fsr_before=md_read(md_cb+0x58);
	if (fsr_before!=UINT32_MAX) md_write(md_cb+0x58,fsr_before);
	uint32_t gfsr=md_read(smmu+0x48);
	if (gfsr!=UINT32_MAX) md_write(smmu+0x48,gfsr);
	md_write(smmu+0xc00+4*md_smr,(2u<<16)|md_cb_index);
	md_write(smmu+0x800+4*md_smr,0x80001d01);
	md_write(md_cb,0);
	md_write(md_cb+0x610,md_cb_index);
	md_write(md_cb+0x7f0,1);
	int error=md_wait(md_cb+0x7f4,1,0,100000);
	if (!error) {
		md_write(smmu+0xc00+4*md_smr,md_cb_index);
		md_write(md_cb,0x1127);
		md_write(md_cb+0x610,md_cb_index);
		md_write(md_cb+0x7f0,1);
		error=md_wait(md_cb+0x7f4,1,0,100000);
	}
	/* Anything the PBL raised while translation was off is acknowledged here:
	 * bus mastering is still off (md_enable_dma), and a genuinely broken
	 * mapping faults again on the first transfer, where md_fault() sees it. */
	uint32_t fsr_after=md_read(md_cb+0x58);
	if (fsr_after!=UINT32_MAX) md_write(md_cb+0x58,fsr_after);
	uint32_t sctlr=md_read(md_cb),s2cr=md_read(smmu+0xc00+4*md_smr);
	lck_mtx_unlock(&hakios_smmu_lock);
	if (!error && (!(sctlr&1) || s2cr!=md_cb_index)) error=EIO;
	MD_LOG("SMMU recovered CB=%u SMR=%u S2CR=%u FSR=%#x->%#x SCTLR=%#x errno=%d\n",
	    md_cb_index,md_smr,s2cr,fsr_before,fsr_after,sctlr,error);
	return error;
}
static int md_dma_install(void)
{
	md_stage="DMA-allocation";
	if (md_dma_ready) return md_dma_recover();

	if (kmem_alloc_contig(kernel_map,&md_pool_cached,MD_DMA_SIZE,PAGE_MASK,0,0,
	    KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS) return ENOMEM;
	md_clean(md_pool_cached,MD_DMA_SIZE);
	md_pool=ml_io_map_unmappable(kvtophys(md_pool_cached),MD_DMA_SIZE,VM_WIMG_WCOMB);
	if (!md_pool) return ENOMEM;
	/* root + L2 + eight L3 for the 16 MiB pool; L2/L3 for the MSI page. */
	const size_t table_bytes=round_page(12*4096);
	if (kmem_alloc(kernel_map,&md_tables,table_bytes,KMA_KOBJECT|KMA_ZERO|KMA_DATA,
	    VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS) return ENOMEM;
	md_table(0)[MD_IOVA>>30]=kvtophys((vm_offset_t)md_table(1))|3;
	for (unsigned n=0;n<8;n++) {
		md_table(1)[((MD_IOVA>>21)&511)+n]=kvtophys((vm_offset_t)md_table(2+n))|3;
		for (unsigned j=0;j<512;j++) md_table(2+n)[j]=
		    (kvtophys(md_pool_cached)+(uint64_t)n*0x200000+j*4096)|0x0000000000000e43ull;
	}
	md_table(0)[0]=kvtophys((vm_offset_t)md_table(10))|3;
	md_table(10)[0x17a10000>>21]=kvtophys((vm_offset_t)md_table(11))|3;
	md_table(11)[(0x17a10000>>12)&511]=0x17a10000ull|0x0060000000000e4bull;
	md_clean(md_tables,table_bytes);
	md_stage="SMMU";
	lck_mtx_lock(&hakios_smmu_lock);
	int error=EBUSY;
	const uint32_t smmu=MD_SMMU;
	uint32_t id0=md_read(smmu+0x20),id1=md_read(smmu+0x24);
	unsigned page=(id1>>31)?0x10000:0x1000, pages=2u<<((id1>>28)&7);
	unsigned count=id0&255,contexts=id1&255,match_ep=UINT32_MAX,cb=UINT32_MAX;
	bool used[256]={false};
	bool programmed=false;
	uint32_t smr_before=0,s2cr_before=0;
	MD_LOG("SMMU caps ID0=%#x ID1=%#x page=%#x pages=%u SMRs=%u CBs=%u ctrl=%#x\n",
	    id0,id1,page,pages,count,contexts,md_read(smmu));
	if (id0==UINT32_MAX || id1==UINT32_MAX || (md_read(smmu)&9) ||
	    2*page*pages!=0x100000 || contexts>pages || !count || !contexts) { error=ENOTSUP; goto out; }
	for (unsigned n=0;n<count;n++) {
		uint32_t smr=md_read(smmu+0x800+4*n),s2cr=md_read(smmu+0xc00+4*n);
		if (smr&MD_BIT(31)) {
			uint32_t sid=smr&0x7fff;
			if (sid==0x1d00 || sid==0x1d01)
				MD_LOG("SMMU active SMR%u=%#x SID=%#x S2CR=%#x CB=%u\n",n,smr,sid,s2cr,s2cr&255);
			used[s2cr&255]=true;
			if (sid==0x1d01) match_ep=n;
		}
	}
	if (match_ep!=UINT32_MAX) {
		MD_LOG("SMMU endpoint SID 0x1d01 already owned by SMR%u; left intact\n",match_ep);
		error=EBUSY; goto out;
	}
	unsigned free_smr=UINT32_MAX;
	for (unsigned n=0;n<count;n++) if (!(md_read(smmu+0x800+4*n)&MD_BIT(31))) { free_smr=n; break; }
	for (unsigned n=0;n<contexts;n++) {
		uint32_t base=smmu+(pages+n)*page;
		uint32_t cbar=md_read(base),fsr=md_read(base+0x58);
		if (!used[n] && !(cbar&1) && !(fsr&0xc00001fe)) { cb=n; break; }
	}
	MD_LOG("SMMU allocation endpoint-SMR=%u CB=%u\n",free_smr,cb);
	if (free_smr==UINT32_MAX || cb==UINT32_MAX) goto out;
	md_cb=smmu+(pages+cb)*page;
	md_cb_index=cb;
	/* What the context held before we armed it. This SoC's SMMU leaves FSR=0x400 (FORMAT only,
	 * no fault bit: 0xc00001fe is the mask) and a stale FAR/FSYNR0 from the bootloader stage in
	 * contexts that never translated; printing the initial values keeps a later fault dump from
	 * being read as our stream being rejected, and keeps it out of the fault line itself. */
	MD_LOG("SMMU CB%u initial FSR=%#x FAR=%#x:%#x FSYNR0=%#x\n",cb,md_read(md_cb+0x58),
	    md_read(md_cb+0x64),md_read(md_cb+0x60),md_read(md_cb+0x68));
	/* Mark the stream faulting before installing the context. This reserves
	 * ownership even if a readback/timeout fails, without enabling DMA. */
	smr_before=md_read(smmu+0x800+4*free_smr); s2cr_before=md_read(smmu+0xc00+4*free_smr);
	MD_LOG("SMMU endpoint SMR%u before=%#x S2CR=%#x\n",free_smr,smr_before,s2cr_before);
	md_write(smmu+0xc00+4*free_smr,(2u<<16)|cb);
	md_write(smmu+0x800+4*free_smr,0x80001d01);
	programmed=true;
	md_write(smmu+page+0x800+4*cb,1);
	md_write(smmu+page+4*cb,0x1f300);
	md_write(md_cb,0);
	md_write(md_cb+0x10,0x38011);
	md_write(md_cb+0x30,0x803519);
	uint64_t ttbr=kvtophys(md_tables)|((uint64_t)cb<<48);
	md_address(md_cb+0x20,ttbr); md_address(md_cb+0x28,0);
	md_write(md_cb+0x38,0xf404ff44); md_write(md_cb+0x3c,0);
	md_write(md_cb+0x610,cb); md_write(md_cb+0x7f0,1);
	error=md_wait(md_cb+0x7f4,1,0,100000);
	if (error) goto out;
	md_write(md_cb,0x1127);
	/* The temporary fault S2CR prevents traffic while the CB is assembled.
	 * Switch it to translation only after the first TLBSYNC, then sync again. */
	md_write(smmu+0xc00+4*free_smr,cb);
	md_write(md_cb+0x7f0,1);
	error=md_wait(md_cb+0x7f4,1,0,100000);
	if (error) goto out;
	uint32_t smr_after=md_read(smmu+0x800+4*free_smr),s2cr_after=md_read(smmu+0xc00+4*free_smr);
	uint32_t cbar_after=md_read(smmu+page+4*cb),cba2r_after=md_read(smmu+page+0x800+4*cb),sctlr_after=md_read(md_cb),ttbr0_lo=md_read(md_cb+0x20),ttbr0_hi=md_read(md_cb+0x24);
	MD_LOG("SMMU endpoint SMR%u after=%#x S2CR=%#x\n",free_smr,smr_after,s2cr_after);
	MD_LOG("SMMU CB%u CBAR=%#x CBA2R=%#x SCTLR=%#x TTBR0=%#x:%#x\n",cb,cbar_after,cba2r_after,sctlr_after,ttbr0_hi,ttbr0_lo);
	if (smr_after!=0x80001d01 || s2cr_after!=cb || !(sctlr_after&1) ||
	    (cbar_after&0x30000)!=0x10000 || !(cba2r_after&1) || ttbr0_lo!=(uint32_t)ttbr ||
	    ttbr0_hi!=(uint32_t)(ttbr>>32)) { error=EIO; goto out; }
	MD_LOG("SMMU SID=0x1d01 CB=%u IOVA=0xb0000000 size=0x1000000\n",cb);
	md_smr=free_smr;md_dma_ready=true;
	error=0;
out:
	if (programmed && error) {
		/* Never leave a faulting or translating stream pointed at freed tables. */
		md_write(smmu+0xc00+4*free_smr,s2cr_before);
		md_write(smmu+0x800+4*free_smr,smr_before);
		md_write(md_cb,0);
		md_write(md_cb+0x7f0,1);
		(void)md_wait(md_cb+0x7f4,1,0,100000);
		MD_LOG("SMMU restore SMR%u=%#x S2CR=%#x\n",free_smr,md_read(smmu+0x800+4*free_smr),md_read(smmu+0xc00+4*free_smr));
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	return error;
}
static int md_enable_dma(void)
{
	md_stage="MSI/SID";
	/* Poll event rings; all MSI vectors remain masked at the endpoint. Map
	 * the GIC MBI alias used by Linux, including SBL's direct MSI writes. */
	unsigned cap=md_read(MD_CONF+0x34)&255;
	uint64_t seen=0;
	bool found=false;
	while (cap) {
		if (cap<0x40 || cap>0xfc || (cap&3) || (seen&(1ull<<(cap/4)))) return EPROTO;
		seen|=1ull<<(cap/4);
		uint32_t header=md_read(MD_CONF+cap);
		if (header==UINT32_MAX) return EIO;
		if ((header&255)==5) {
			unsigned ctrl=header>>16;
			if (!(ctrl&0x80) || !(ctrl&0x100) || ((ctrl>>1)&7)<3 || cap>0xe8) return ENOTSUP;
			md_write(MD_CONF+cap,header&~MD_BIT(16));
			md_write(MD_CONF+cap+16,UINT32_MAX);
			md_address(MD_CONF+cap+4,0x17a10040);
			md_write(MD_CONF+cap+12,864);
			md_write(MD_CONF+cap,(header&~(0x71u<<16))|(0x31u<<16));
			found=true; break;
		}
		cap=(header>>8)&255;
	}
	if (!found) return ENODEV;
	for (unsigned i=0;i<256;i++) md_write(MD_PARF+0x2000+4*i,0);
	unsigned hash=0;
	for (unsigned byte=0;byte<2;byte++) {
		hash^=byte?0:1;
		for (unsigned bit=0;bit<8;bit++) hash=((hash<<1)^((hash&128)?7:0))&255;
	}
	md_write(MD_PARF+0x2000+4*hash,0x01000100);
	if (md_read(MD_PARF+0x2000+4*hash)!=0x01000100) return EIO;
	MD_TRY(md_update(MD_PARF+0x2c00,1,0));
	if (md_read(MD_PARF+0x2c00)&1) return EIO;
	md_write(MD_DBI+4,6);
	md_write(MD_CONF+4,(md_read(MD_CONF+4)&0xffff)|4);
	md_dma_on=true;
	if ((md_read(MD_CONF+4)&7)!=6) return EIO;
	MD_LOG("protected DMA enabled; MSI alias=0x17a10040 INTID=864 masked polling\n");
	return 0;
}
