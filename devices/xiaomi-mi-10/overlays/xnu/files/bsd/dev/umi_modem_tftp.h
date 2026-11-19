/* TFTP over QRTR (service 4096) as Linux tqftpserv serves it to the SDX55:
 * RRQ/WRQ parsing with the blksize/timeoutms/tsize/rsize/wsize/seek options
 * and the OACK it answers with. Shared by the kernel (umi_modem_mhi.inc) and
 * the host tests (tools/modem-tests). No transport dependency here. */
#ifndef UMI_MODEM_TFTP_H
#define UMI_MODEM_TFTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { UMI_TFTP_RRQ = 1, UMI_TFTP_WRQ, UMI_TFTP_DATA, UMI_TFTP_ACK, UMI_TFTP_ERROR, UMI_TFTP_OACK };

struct umi_tftp_request {
	unsigned op;
	const char *path;
	bool options, tsize;
	uint32_t blksize, timeoutms, wsize;
	uint64_t rsize, seek;
};

static inline bool umi_tftp_number(const char *s, uint64_t min, uint64_t max, uint64_t *out)
{
	uint64_t v = 0;
	if (!*s) return false;
	for (; *s; s++) {
		if (*s < '0' || *s > '9' || v > (max - (uint64_t)(*s - '0')) / 10) return false;
		v = v * 10 + (uint64_t)(*s - '0');
	}
	if (v < min) return false;
	*out = v; return true;
}
static inline const char *umi_tftp_string(const char **s, const char *end)
{
	const char *start = *s; size_t n = strnlen(start, (size_t)(end - start));
	if (start + n >= end) return NULL;
	*s = start + n + 1; return start;
}
/* 0 or -1. Ranges as tqftpserv: blksize 8..65464, timeoutms 1..255000,
 * wsize 1..65535, rsize 1..100 MiB; unknown options are ignored. */
static inline int umi_tftp_parse(const uint8_t *p, size_t n, struct umi_tftp_request *r)
{
	const char *s, *end, *mode, *opt, *val; uint64_t v;
	if (!p || !r || n < 4 || p[0] || (p[1] != UMI_TFTP_RRQ && p[1] != UMI_TFTP_WRQ)) return -1;
	s = (const char *)p + 2; end = (const char *)p + n;
	memset(r, 0, sizeof(*r)); r->op = p[1]; r->blksize = 512; r->timeoutms = 1000; r->wsize = 1;
	if (!(r->path = umi_tftp_string(&s, end)) || !r->path[0] || !(mode = umi_tftp_string(&s, end))) return -1;
	for (const char *o = "octet"; *o || *mode; o++, mode++) if ((*mode | 0x20) != *o) return -1;
	while (s < end) {
		if (!(opt = umi_tftp_string(&s, end)) || !(val = umi_tftp_string(&s, end))) return -1;
		r->options = true;
		if (!strcmp(opt, "blksize")) { if (!umi_tftp_number(val, 8, 65464, &v)) return -1; r->blksize = (uint32_t)v; }
		else if (!strcmp(opt, "timeoutms")) { if (!umi_tftp_number(val, 1, 255000, &v)) return -1; r->timeoutms = (uint32_t)v; }
		else if (!strcmp(opt, "tsize")) { if (!umi_tftp_number(val, 0, UINT32_MAX, &v)) return -1; r->tsize = true; }
		else if (!strcmp(opt, "rsize")) { if (!umi_tftp_number(val, 1, 100u << 20, &r->rsize)) return -1; }
		else if (!strcmp(opt, "wsize")) { if (!umi_tftp_number(val, 1, 65535, &v)) return -1; r->wsize = (uint32_t)v; }
		else if (!strcmp(opt, "seek")) { if (!umi_tftp_number(val, 0, UINT32_MAX, &r->seek)) return -1; }
	}
	return 0;
}
static inline bool umi_tftp_put(uint8_t *out, size_t cap, size_t *at, const char *name, uint64_t value)
{
	char digits[21]; size_t d = 0, n = strlen(name) + 1;
	do digits[d++] = (char)('0' + value % 10); while ((value /= 10) != 0);
	if (*at + n + d + 1 > cap) return false;
	memcpy(out + *at, name, n); *at += n;
	while (d) out[(*at)++] = (uint8_t)digits[--d];
	out[(*at)++] = 0; return true;
}
/* tqftpserv's OACK: blksize, timeoutms, tsize (if asked, whole file size),
 * wsize, rsize and seek (if non-zero), in that order. 0 if it does not fit. */
static inline size_t umi_tftp_oack(uint8_t *out, size_t cap, const struct umi_tftp_request *r, uint32_t blksize, uint64_t tsize)
{
	size_t at = 2;
	if (!out || !r || cap < 2) return 0;
	out[0] = 0; out[1] = UMI_TFTP_OACK;
	if (!umi_tftp_put(out, cap, &at, "blksize", blksize) || !umi_tftp_put(out, cap, &at, "timeoutms", r->timeoutms) ||
	    (r->tsize && !umi_tftp_put(out, cap, &at, "tsize", tsize)) || !umi_tftp_put(out, cap, &at, "wsize", r->wsize) ||
	    (r->rsize && !umi_tftp_put(out, cap, &at, "rsize", r->rsize)) || (r->seek && !umi_tftp_put(out, cap, &at, "seek", r->seek)))
		return 0;
	return at;
}
/* A transfer of `limit` bytes is limit/blksize + 1 DATA blocks; the last is
 * short (empty when limit is a multiple of blksize), which ends it. */
static inline uint32_t umi_tftp_blocks(uint64_t limit, uint32_t blksize) { return (uint32_t)(limit / blksize + 1); }
static inline uint32_t umi_tftp_block_length(uint64_t limit, uint32_t blksize, uint32_t block)
{
	uint64_t offset = (uint64_t)(block - 1) * blksize;
	return offset >= limit ? 0 : (uint32_t)(limit - offset < blksize ? limit - offset : blksize);
}

#endif
