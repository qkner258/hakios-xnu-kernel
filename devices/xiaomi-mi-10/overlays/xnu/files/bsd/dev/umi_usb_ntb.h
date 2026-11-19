/* NCM16 framing, host-testable; no allocation, unaligned access or DMA here. */
#define NCM_NTB_MAX 16384U
#define NCM_FRAME_MAX 1514U
#define NCM_DATAGRAMS 16U
struct ncm_datagram { unsigned offset, length; };
static uint16_t ncm_le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t ncm_le32(const uint8_t *p) { return ncm_le16(p) | (uint32_t)ncm_le16(p + 2) << 16; }
static void ncm_put16(uint8_t *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void ncm_put32(uint8_t *p, uint32_t v) { ncm_put16(p, v); ncm_put16(p + 2, v >> 16); }
static bool ncm_overlap(unsigned a, unsigned n, unsigned b, unsigned m) { return a < b + m && b < a + n; }
/* Negative return values are stable diagnostic categories, not errno. */
enum ncm_error {
    NCM_OK, NCM_SIZE, NCM_NTH32, NCM_NTH_SIGNATURE, NCM_NTH_LENGTH,
    NCM_BLOCK_LENGTH, NCM_NDP_INDEX, NCM_NDP_CRC, NCM_NDP_SIGNATURE,
    NCM_NDP_LENGTH, NCM_NDP_CHAIN, NCM_FRAME_LIMIT, NCM_FRAME_INDEX,
    NCM_FRAME_LENGTH, NCM_FRAME_OVERLAP, NCM_TABLE_OVERLAP,
    NCM_TERMINATOR, NCM_RESIDUE, NCM_DMA, NCM_ERRORS
};
static const char *const ncm_error_name[NCM_ERRORS] = {
    "ok", "size", "nth32", "nth-signature", "nth-length", "block-length",
    "ndp-index", "ndp-crc", "ndp-signature", "ndp-length", "ndp-chain",
    "frame-limit", "frame-index", "frame-length", "frame-overlap",
    "table-overlap", "terminator", "trb-residue", "dma"
};
struct ncm_decode_info { unsigned blocks, zero_block, pad, unaligned, half_null; };
/* Linux f_ncm accepts zero wBlockLength (USB transfer delimits the NTB),
 * successive NTBs and a trailing zero byte. Frame offsets need not be aligned:
 * we only use byte accesses, just as Linux copies unaligned Ethernet frames.
 * Validate ALL blocks before the caller delivers any frame. No reassembly:
 * one 16KiB OUT TRB spans USB packets up to the advertised maximum NTB size. */
static int ncm_decode(const uint8_t *p, unsigned size, struct ncm_datagram *frames,
                      struct ncm_decode_info *info)
{
    unsigned base = 0, nf = 0;
    memset(info, 0, sizeof(*info));
    if (size < 12 || size > NCM_NTB_MAX) return -NCM_SIZE;
    while (base < size) {
        const uint8_t *q = p + base;
        unsigned available = size - base;
        if (base && available == 1 && !q[0]) { ++info->pad; break; }
        if (available < 12) return -NCM_SIZE;
        if (ncm_le32(q) == 0x686d636e) return -NCM_NTH32;
        if (ncm_le32(q) != 0x484d434e) return -NCM_NTH_SIGNATURE;
        if (ncm_le16(q + 4) != 12) return -NCM_NTH_LENGTH;
        unsigned block = ncm_le16(q + 8);
        if (!block) { block = available; ++info->zero_block; }
        if (block < 24 || block > available) return -NCM_BLOCK_LENGTH;
        unsigned tables[NCM_DATAGRAMS], lengths[NCM_DATAGRAMS], nt = 0, first = nf;
        unsigned at = ncm_le16(q + 10);
        if (!at) return -NCM_NDP_INDEX;
        while (at) {
            if (nt == NCM_DATAGRAMS) return -NCM_NDP_CHAIN;
            if (at < 12 || (at & 3) || at > block - 8) return -NCM_NDP_INDEX;
            for (unsigned i = 0; i < nt; ++i) if (at == tables[i]) return -NCM_NDP_CHAIN;
            if (ncm_le32(q + at) == 0x314d434e) return -NCM_NDP_CRC;
            if (ncm_le32(q + at) != 0x304d434e) return -NCM_NDP_SIGNATURE;
            unsigned n = ncm_le16(q + at + 4);
            if (n < 12 || (n & 3) || n > block - at) return -NCM_NDP_LENGTH;
            for (unsigned i = 0; i < nt; ++i)
                if (ncm_overlap(at, n, tables[i], lengths[i])) return -NCM_TABLE_OVERLAP;
            tables[nt] = at; lengths[nt++] = n;
            bool end = false;
            for (unsigned i = 8; i < n; i += 4) {
                unsigned off = ncm_le16(q + at + i), len = ncm_le16(q + at + i + 2);
                /* Linux terminates when EITHER member of the next DPE is zero. */
                if (!off || !len) { end = true; info->half_null += !!(off || len); break; }
                if (nf == NCM_DATAGRAMS) return -NCM_FRAME_LIMIT;
                if (off < 12 || off > block) return -NCM_FRAME_INDEX;
                if (len < 14 || len > NCM_FRAME_MAX || len > block - off) return -NCM_FRAME_LENGTH;
                for (unsigned j = first; j < nf; ++j)
                    if (ncm_overlap(base + off, len, frames[j].offset, frames[j].length)) return -NCM_FRAME_OVERLAP;
                info->unaligned += !!(off & 3);
                frames[nf++] = (struct ncm_datagram){base + off, len};
            }
            if (!end) return -NCM_TERMINATOR;
            at = ncm_le16(q + at + 6);
        }
        for (unsigned i = first; i < nf; ++i)
            for (unsigned j = 0; j < nt; ++j)
                if (ncm_overlap(frames[i].offset, frames[i].length, base + tables[j], lengths[j])) return -NCM_TABLE_OVERLAP;
        ++info->blocks;
        base += block;
    }
    return (int)nf;
}
static unsigned ncm_encode(uint8_t *p, const uint8_t *frame, unsigned len, unsigned sequence)
{
    if (len < 14 || len > NCM_FRAME_MAX) return 0;
    unsigned size = 28 + len;
    /* A short USB packet terminates even when the host queues a larger NTB. */
    if (!(size % 512)) ++size;
    memset(p, 0, 28);
    ncm_put32(p, 0x484d434e); ncm_put16(p + 4, 12); ncm_put16(p + 6, sequence);
    ncm_put16(p + 8, size); ncm_put16(p + 10, 12);
    ncm_put32(p + 12, 0x304d434e); ncm_put16(p + 16, 16);
    ncm_put16(p + 20, 28); ncm_put16(p + 22, len);
    memcpy(p + 28, frame, len);
    if (size > 28 + len) p[size - 1] = 0;
    return size;
}
