/* Read-only FAT16/FAT32 view of the Android modem UFS partition.
 * The X55 images are in NON-HLOS/modem on this board.  Keep this deliberately
 * small: no VFAT writes, no long-name creation, and no partition caching. */
extern int hakios_ufs_read_partition(const char *, uint64_t, uint8_t *, uint64_t, uint64_t *);
struct md_fat_volume {
	uint32_t bps, spc, reserved, fats, spf, root_entries;
	uint32_t root_cluster, root_start, data_start, clusters;
	uint64_t sectors, part_bytes;
	bool fat32;
};
struct md_fat_file {
	uint32_t first, size;
	bool directory;
	uint32_t *chain;
	uint32_t chain_count;
	uint64_t cluster_bytes;
};
static struct md_fat_volume md_fat;
static uint8_t md_fat_sector[4096];
static char md_fat_lfn[260];
static bool md_fat_lfn_valid;
static uint32_t md_fat_loaded_sector = UINT32_MAX;
/* Set while serving TFTP: hundreds of carrier-config opens, no per-path log. */
static bool md_fat_quiet;

static uint16_t md_fat_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}
static uint32_t md_fat_le32(const uint8_t *p)
{
	return md_fat_le16(p) | (uint32_t)md_fat_le16(p + 2) << 16;
}
static int md_fat_disk(uint64_t offset, void *dst, uint64_t length)
{
	int error;
	if (length == 0) return 0;
	if (offset > md_fat.part_bytes || length > md_fat.part_bytes - offset) {
		MD_LOG("FAT read range offset=%llu length=%llu part_bytes=%llu\n",offset,length,md_fat.part_bytes);
		return ERANGE;
	}
	/* The API's bytes result is the partition size, not this transfer count. */
	error = hakios_ufs_read_partition("modem", offset, dst, length, NULL);
	return error;
}
static int md_fat_init(void)
{
	uint32_t root_sectors, data_sectors;
	uint64_t fat_sectors;
	int error;
	if (md_fat.bps) return 0;
	memset(&md_fat, 0, sizeof(md_fat));
	md_fat_loaded_sector = UINT32_MAX;
	error = hakios_ufs_read_partition("modem", 0, md_fat_sector, sizeof(md_fat_sector),
	    &md_fat.part_bytes);
	if (error) return error;
	if (md_fat_sector[510] != 0x55 || md_fat_sector[511] != 0xaa) return EINVAL;
	md_fat.bps = md_fat_le16(md_fat_sector + 11);
	md_fat.spc = md_fat_sector[13];
	md_fat.reserved = md_fat_le16(md_fat_sector + 14);
	md_fat.fats = md_fat_sector[16];
	md_fat.root_entries = md_fat_le16(md_fat_sector + 17);
	MD_LOG("firmware FAT raw BPB bps=%u spc=%u reserved=%u fats=%u root=%u total16=%u spf16=%u total32=%u spf32=%u\n",
	    md_fat.bps,md_fat.spc,md_fat.reserved,md_fat.fats,md_fat.root_entries,
	    md_fat_le16(md_fat_sector+19),md_fat_le16(md_fat_sector+22),
	    md_fat_le32(md_fat_sector+32),md_fat_le32(md_fat_sector+36));
	if (md_fat.bps < 512 || md_fat.bps > sizeof(md_fat_sector) ||
	    (md_fat.bps & (md_fat.bps - 1)) || !md_fat.spc || !md_fat.reserved || !md_fat.fats)
		return EINVAL;
	md_fat.sectors = md_fat_le16(md_fat_sector + 19);
	if (!md_fat.sectors) md_fat.sectors = md_fat_le32(md_fat_sector + 32);
	md_fat.spf = md_fat_le16(md_fat_sector + 22);
	if (!md_fat.spf) md_fat.spf = md_fat_le32(md_fat_sector + 36);
	MD_LOG("firmware FAT BPB bps=%u spc=%u reserved=%u fats=%u spf=%u root=%u sectors=%llu\n",
	    md_fat.bps, md_fat.spc, md_fat.reserved, md_fat.fats, md_fat.spf,
	    md_fat.root_entries, md_fat.sectors);
	if (!md_fat.sectors || !md_fat.spf || md_fat.sectors > md_fat.part_bytes / md_fat.bps)
		return EINVAL;
	root_sectors = ((uint32_t)md_fat.root_entries * 32 + md_fat.bps - 1) / md_fat.bps;
	fat_sectors = (uint64_t)md_fat.fats * md_fat.spf;
	if ((uint64_t)md_fat.reserved + fat_sectors + root_sectors >= md_fat.sectors)
		return EINVAL;
	md_fat.data_start = md_fat.reserved + (uint32_t)fat_sectors + root_sectors;
	data_sectors = (uint32_t)(md_fat.sectors - md_fat.data_start);
	md_fat.clusters = data_sectors / md_fat.spc;
	if (md_fat.clusters < 1 || md_fat.clusters >= 0x0ffffff5) return EINVAL;
	md_fat.fat32 = md_fat.clusters >= 65525;
	if (md_fat.fat32) {
		md_fat.root_cluster = md_fat_le32(md_fat_sector + 44) & 0x0fffffff;
		md_fat.root_start = 0;
		if (md_fat.root_cluster < 2 || md_fat.root_cluster >= md_fat.clusters + 2) return EINVAL;
	} else {
		md_fat.root_cluster = 0;
		md_fat.root_start = md_fat.reserved + (uint32_t)fat_sectors;
	}
	/* Keep the byte size reported by the GPT/UFS reader.  FAT's sector count
	 * is validated above, but must not replace the partition size: UFS is
	 * exposed in 4K bytes on this phone while some FAT media describe 512B. */
	MD_LOG("firmware source=UFS modem FAT type=%s clusters=%u part_bytes=%llu\n",
	    md_fat.fat32 ? "FAT32" : "FAT16", md_fat.clusters, md_fat.part_bytes);
	return 0;
}
static int md_fat_next(uint32_t cluster, uint32_t *next)
{
	uint64_t byte, sector;
	uint32_t at;
	int error;
	if (cluster < 2 || cluster >= md_fat.clusters + 2) return EINVAL;
	byte = (uint64_t)cluster * (md_fat.fat32 ? 4 : 2);
	sector = md_fat.reserved + byte / md_fat.bps;
	at = (uint32_t)(byte % md_fat.bps);
	if (sector != md_fat_loaded_sector) {
		error = md_fat_disk(sector * md_fat.bps, md_fat_sector, md_fat.bps);
		if (error) return error;
		md_fat_loaded_sector = (uint32_t)sector;
	}
	*next = md_fat.fat32 ? md_fat_le32(md_fat_sector + at) & 0x0fffffff :
	    md_fat_le16(md_fat_sector + at);
	if (*next == 0 || (!md_fat.fat32 && *next >= 0xfff0) ||
	    (md_fat.fat32 && *next >= 0x0ffffff0)) return ENOENT;
	if (*next < 2 || *next >= md_fat.clusters + 2) return EINVAL;
	return 0;
}
static bool md_fat_name(const uint8_t *entry, const char *name)
{
	char shortname[13];
	unsigned n = 0, i, base = 8;
	while (n < 8 && entry[n] != ' ') n++;
	if (n == 0) return false;
	for (i = 0; i < n; i++) shortname[i] = entry[i] >= 'A' && entry[i] <= 'Z' ?
	    (char)(entry[i] + ('a' - 'A')) : (char)entry[i];
	if (entry[8] != ' ') {
		shortname[n++] = '.';
		while (base < 11 && entry[base] != ' ') {
			shortname[n++] = entry[base] >= 'A' && entry[base] <= 'Z' ?
			    (char)(entry[base] + ('a' - 'A')) : (char)entry[base];
			base++;
		}
	}
	shortname[n] = 0;
	if (strlen(name) != n) return false;
	for (i = 0; i < n; i++) {
		char c = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + ('a' - 'A')) : name[i];
		if (shortname[i] != c) return false;
	}
	return true;
}
static bool md_fat_long_name(const char *name)
{
	size_t n = strlen(name);
	if (!md_fat_lfn_valid || n != strlen(md_fat_lfn)) return false;
	for (size_t i = 0; i < n; i++) {
		char a = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + ('a' - 'A')) : name[i];
		char b = md_fat_lfn[i] >= 'A' && md_fat_lfn[i] <= 'Z' ?
		    (char)(md_fat_lfn[i] + ('a' - 'A')) : md_fat_lfn[i];
		if (a != b) return false;
	}
	return true;
}
static void md_fat_lfn_entry(const uint8_t *entry)
{
	unsigned ordinal = entry[0] & 0x1f;
	unsigned pos = ordinal ? (ordinal - 1) * 13 : 260;
	if (entry[0] & 0x40) {
		memset(md_fat_lfn, 0, sizeof(md_fat_lfn));
		md_fat_lfn_valid = true;
	}
	if (!ordinal || pos >= sizeof(md_fat_lfn) - 1 || !md_fat_lfn_valid) {
		md_fat_lfn_valid = false;
		return;
	}
	/* LFN is UTF-16. Signed modem filenames are ASCII; reject surrogate or
	 * non-ASCII characters rather than silently selecting another image. */
	for (unsigned i = 0; i < 13; i++) {
		unsigned at = i < 5 ? 1 + i * 2 : i < 11 ? 14 + (i - 5) * 2 : 28 + (i - 11) * 2;
		uint16_t c = md_fat_le16(entry + at);
		if (c == 0 || c == 0xffff) {
			if (pos + i < sizeof(md_fat_lfn)) md_fat_lfn[pos + i] = 0;
			continue;
		}
		if (c > 0x7f || pos + i >= sizeof(md_fat_lfn) - 1) {
			md_fat_lfn_valid = false; return;
		}
		md_fat_lfn[pos + i] = (char)c;
	}
	md_fat_lfn[sizeof(md_fat_lfn) - 1] = 0;
}
static int md_fat_scan_sector(uint32_t sector, const char *name, struct md_fat_file *out)
{
	int error = md_fat_disk((uint64_t)sector * md_fat.bps, md_fat_sector, md_fat.bps);
	if (error) return error;
	md_fat_loaded_sector = UINT32_MAX;
	for (unsigned off = 0; off < md_fat.bps; off += 32) {
		const uint8_t *e = md_fat_sector + off;
		uint32_t cluster;
		if (e[0] == 0) { md_fat_lfn_valid = false; return ENOENT; }
		if (e[0] == 0xe5) { md_fat_lfn_valid = false; continue; }
		if (e[11] == 0x0f) { md_fat_lfn_entry(e); continue; }
		if (e[11] & 0x08) { md_fat_lfn_valid = false; continue; }
		if (!md_fat_name(e, name) && !md_fat_long_name(name)) { md_fat_lfn_valid = false; continue; }
		cluster = md_fat_le16(e + 26);
		if (md_fat.fat32) cluster |= (uint32_t)md_fat_le16(e + 20) << 16;
		out->first = cluster;
		out->size = md_fat_le32(e + 28);
		out->directory = (e[11] & 0x10) != 0;
		return 0;
	}
	return ENOENT;
}
static int md_fat_find_dir(uint32_t cluster, const char *name, struct md_fat_file *out)
{
	uint32_t limit = 0;
	if (!md_fat.fat32 && cluster == 0) {
		md_fat_lfn_valid = false;
		uint32_t root_sectors = ((uint32_t)md_fat.root_entries * 32 + md_fat.bps - 1) / md_fat.bps;
		for (uint32_t i = 0; i < root_sectors; i++) {
			int e = md_fat_scan_sector(md_fat.root_start + i, name, out);
			if (!e) return 0;
			if (e != ENOENT) return e;
		}
		return ENOENT;
	}
	while (cluster >= 2 && cluster < md_fat.clusters + 2 && limit++ <= md_fat.clusters) {
		if (limit == 1) md_fat_lfn_valid = false;
		uint32_t first = md_fat.data_start + (cluster - 2) * md_fat.spc;
		for (uint32_t i = 0; i < md_fat.spc; i++) {
			int e = md_fat_scan_sector(first + i, name, out);
			if (!e) return 0;
			if (e != ENOENT) return e;
		}
		if (md_fat_next(cluster, &cluster)) break;
	}
	return ENOENT;
}
static int md_fat_lookup(const char *path, struct md_fat_file *out)
{
	char component[64];
	uint32_t dir = md_fat.fat32 ? md_fat.root_cluster : 0;
	const char *p = path;
	int error;
	if (!path || !path[0]) return EINVAL;
	for (;;) {
		const char *slash = strchr(p, '/');
		size_t n = slash ? (size_t)(slash - p) : strlen(p);
		if (!n || n >= sizeof(component)) return EINVAL;
		memcpy(component, p, n); component[n] = 0;
		if (!md_fat_quiet) MD_LOG("FAT resolve component=%s\n",component);
		error = md_fat_find_dir(dir, component, out);
		if (error) return error;
		if (!md_fat_quiet) MD_LOG("FAT resolved component=%s first=%u size=%u directory=%u\n",component,out->first,out->size,out->directory);
		if (!slash) return 0;
		if (!out->directory || !out->first) return ENOENT;
		dir = out->first;
		p = slash + 1;
	}
}
static int md_fat_open_path(const char *name, const char *path, struct md_fat_file *out)
{
	uint64_t cluster_bytes;
	uint32_t cluster;
	int error;
	if (!out || !name || !name[0] || strlen(name) >= 64) return EINVAL;
	memset(out, 0, sizeof(*out));
	error = md_fat_init();
	if (error) return error;
	error = md_fat_lookup(path, out);
	if (error) return error;
	cluster_bytes = (uint64_t)md_fat.bps * md_fat.spc;
	if (out->directory || !out->size || !out->first || !cluster_bytes) return ERANGE;
	if ((uint64_t)out->size + cluster_bytes - 1 > UINT32_MAX * cluster_bytes) return EFBIG;
	out->cluster_bytes = cluster_bytes;
	out->chain_count = (uint32_t)(((uint64_t)out->size + cluster_bytes - 1) / cluster_bytes);
	out->chain = kalloc_data((size_t)out->chain_count * sizeof(*out->chain), Z_WAITOK);
	if (!out->chain) return ENOMEM;
	cluster = out->first;
	for (uint32_t i = 0; i < out->chain_count; i++) {
		if (cluster < 2 || cluster >= md_fat.clusters + 2) return EINVAL;
		out->chain[i] = cluster;
		if (i + 1 < out->chain_count) MD_TRY(md_fat_next(cluster, &cluster));
	}
	if (!md_fat_quiet) MD_LOG("FAT image prepared name=%s first=%u size=%u clusters=%u\n",
	    name, out->first, out->size, out->chain_count);
	return 0;
}
static int md_fat_open(const char *name, struct md_fat_file *out)
{
	char path[96];
	if (!name || strlen(name) >= 64) return EINVAL;
	snprintf(path, sizeof(path), "image/sdx55m/%s", name);
	return md_fat_open_path(name, path, out);
}

/* Read from a prepared entry. Bounds and chain integrity are checked by the
 * caller once before a Sahara request; this loop performs no path or FAT I/O. */
static int md_fat_read_prepared(const struct md_fat_file *file, uint64_t off, void *data, size_t length)
{
	if (!file || !file->chain || !file->cluster_bytes || !data) return EINVAL;
	uint64_t index = off / file->cluster_bytes, in_cluster = off % file->cluster_bytes, remain = length;
	if (index >= file->chain_count) return EINVAL;
	while (remain) {
		uint64_t available = file->cluster_bytes - in_cluster;
		uint64_t count = remain < available ? remain : available;
		uint64_t disk = ((uint64_t)md_fat.data_start + (file->chain[index] - 2) * md_fat.spc) *
		    md_fat.bps + in_cluster;
		MD_TRY(md_fat_disk(disk, data, count));
		data = (uint8_t *)data + count; remain -= count; in_cluster = 0;
		if (remain && ++index >= file->chain_count) return EINVAL;
	}
	return 0;
}

/* Compatibility wrapper used only by the host fixture tests and the initial
 * metadata/SBL path. Production Sahara reads use md_fat_open once per image. */
static int md_fat_file(const char *name, uint64_t off, void *data, size_t *length, bool whole)
{
	struct md_fat_file file;
	int error;
	if (!length) return EINVAL;
	error = md_fat_open(name, &file);
	if (error) {
		/* Legacy fixture/helper API only. The production Sahara path above
		 * uses the single known image/sdx55m/ location. */
		char path[96];
		snprintf(path, sizeof(path), "%s", name);
		error = md_fat_open_path(name, path, &file);
	}
	if (error) return error;
	if (off > file.size) return ERANGE;
	if (whole) {
		if (file.size > *length) return EFBIG;
		*length = file.size;
	} else if (*length > file.size - off) return ERANGE;
	if (data && *length) return md_fat_read_prepared(&file, off, data, *length);
	return 0;
}
