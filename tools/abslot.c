/*
 * abslot: read or mark the A/B boot slot flags that Chef's abl keeps in the
 * GPT entry attributes of boot_a, the job Android's QCOM boot_control HAL
 * (markBootSuccessful) did before Android was retired on this phone.
 *
 *   abslot status            boot_a/boot_b flags; both GPT copies validated
 *   abslot mark-successful   set the successful bit of boot_a (only writer)
 *
 * Why: `fastboot flash boot_a` clears boot_a's successful bit and sets its
 * retry count to 7, and abl then takes one retry per boot of a slot that is
 * not successful (both measured 2026-10-04). With boot_b already unbootable
 * an exhausted boot_a leaves no bootable slot. Bit layout of the entry
 * attributes as abl uses it: 48-49 priority, 50 active, 51-53 retry count,
 * 54 successful, 55 unbootable. Only bit 54 of boot_a is ever written; the
 * retry count, priority, active and every other entry stay as they are.
 *
 * mark-successful refuses, writing nothing, unless the kernel booted slot
 * _a, the disk is mmcblk0 with 512-byte sectors, both GPT copies are valid
 * (signature, header size, header CRC, own and alternate LBA, entry size
 * 128, entry-array CRC), agree on everything but their own position, have
 * byte-identical entry arrays, exactly one entry is named boot_a whose
 * number, first LBA and size match the kernel's PARTNAME=boot_a partition,
 * and boot_a reads active, not unbootable, nonzero priority (the slot we
 * run from). The whole run holds an flock of the disk. It then writes
 * exactly four 512-byte sectors: the sector holding the boot_a entry in the
 * backup array, the backup header, then the same two of the primary. All
 * device I/O is O_DIRECT (512-byte logical blocks, aligned bounce buffers)
 * and writes are O_SYNC: buffered 512-byte writes would read-modify-write
 * the block device's 4 KiB buffers and write back neighbouring sectors
 * (the protective MBR, other entries) too. abl's own attribute updates
 * touch the same bytes (measured on the 2026-10-04 flash: header CRC,
 * entry-array CRC, one attribute byte, nothing else). Both copies are then
 * re-read from the device and must differ from the pre-write read in
 * exactly header bytes 16-19 and 88-91 and bit 6 of boot_a's attribute
 * byte 6, per copy written. Already successful: no write, exit 0.
 *
 * Half-written state: an interruption between the two fsyncs leaves a
 * valid old primary and a valid new backup whose arrays differ in exactly
 * boot_a's successful bit. status reports it as "half-marked";
 * mark-successful completes the primary (the backup is not rewritten).
 * Any other difference between the copies, including the reverse (primary
 * marked, backup not), is refused. A torn copy (CRC mismatch) is refused.
 *
 * Every line goes to stdout, /dev/kmsg and /run/abslot.log (appended).
 * Overrides for host tests (logged as TEST OVERRIDE): -d DISK (image file;
 * buffered I/O if its filesystem refuses O_DIRECT, e.g. tmpfs), -s SYSFS
 * root, -c CMDLINE file, -K KMSG, -L LOG.
 */
#define _GNU_SOURCE	/* O_DIRECT */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>

#define SECTOR		512
#define ENTRY_SIZE	128
#define MAX_ARRAY	(32 * SECTOR)
#define ATTR_SUCCESS	(1ULL << 54)
#define ATTR_BYTE	6	/* byte of the 8-byte attribute word holding bit 54 */
#define ATTR_BIT	0x40
#define DISK_NAME	"mmcblk0"

struct gpt {
	uint8_t hdr[SECTOR];	/* the whole header sector, as read */
	uint64_t my_lba, alt_lba, first_usable, last_usable, array_lba;
	uint32_t count, size;
	uint8_t array[MAX_ARRAY];
};

struct part {
	unsigned partno;
	uint64_t start, sectors;
};

struct opts {
	const char *disk, *sysfs, *cmdline;
	bool test_disk;		/* -d given: a regular file may fall back to buffered I/O */
};

static FILE *g_kmsg, *g_log;

static uint32_t crc32_le(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;

	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & -(c & 1));
	}
	return ~c;
}

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void say(const char *fmt, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("abslot: %s\n", line);
	if (g_kmsg) {
		fprintf(g_kmsg, "abslot: %s\n", line);
		fflush(g_kmsg);
	}
	if (g_log) {
		fprintf(g_log, "%s\n", line);
		fflush(g_log);
	}
}

/* Header CRC over hsize bytes with the CRC field zeroed. */
static uint32_t header_crc(const uint8_t *hdr)
{
	uint8_t tmp[SECTOR];
	uint32_t hsize = get32(hdr + 12);

	memcpy(tmp, hdr, hsize);
	put32(tmp + 16, 0);
	return crc32_le(tmp, hsize);
}

/* O_DIRECT needs sector-multiple lengths and aligned buffers: go through
 * one page-aligned bounce buffer. */
static uint8_t *bounce(void)
{
	static uint8_t *buf;

	if (!buf && posix_memalign((void **)&buf, 4096, MAX_ARRAY + SECTOR))
		buf = NULL;
	return buf;
}

static int read_at(int fd, void *dst, size_t n, uint64_t lba)
{
	uint8_t *buf = bounce();
	size_t len = (n + SECTOR - 1) / SECTOR * SECTOR;

	if (!buf || len > MAX_ARRAY + SECTOR ||
	    pread(fd, buf, len, (off_t)(lba * SECTOR)) != (ssize_t)len)
		return -1;
	memcpy(dst, buf, n);
	return 0;
}

/* Parse and validate one GPT copy whose header is at my_lba. */
static int load_copy(int fd, uint64_t my_lba, uint64_t want_alt, uint64_t disk_sectors,
		     struct gpt *g, const char *what)
{
	uint32_t hsize;

	if (read_at(fd, g->hdr, SECTOR, my_lba)) {
		say("%s header: read of LBA %llu failed", what, (unsigned long long)my_lba);
		return -1;
	}
	if (memcmp(g->hdr, "EFI PART", 8)) {
		say("%s header: no GPT signature at LBA %llu", what, (unsigned long long)my_lba);
		return -1;
	}
	hsize = get32(g->hdr + 12);
	if (hsize < 92 || hsize > SECTOR) {
		say("%s header: size %u", what, hsize);
		return -1;
	}
	if (header_crc(g->hdr) != get32(g->hdr + 16)) {
		say("%s header: CRC mismatch", what);
		return -1;
	}
	g->my_lba = get64(g->hdr + 24);
	g->alt_lba = get64(g->hdr + 32);
	g->first_usable = get64(g->hdr + 40);
	g->last_usable = get64(g->hdr + 48);
	g->array_lba = get64(g->hdr + 72);
	g->count = get32(g->hdr + 80);
	g->size = get32(g->hdr + 84);
	if (g->my_lba != my_lba || g->alt_lba != want_alt) {
		say("%s header: at LBA %llu claims %llu, alternate %llu (want %llu)", what,
		    (unsigned long long)my_lba, (unsigned long long)g->my_lba,
		    (unsigned long long)g->alt_lba, (unsigned long long)want_alt);
		return -1;
	}
	if (g->size != ENTRY_SIZE || g->count == 0 || (uint64_t)g->count * g->size > MAX_ARRAY) {
		say("%s header: %u entries of %u bytes", what, g->count, g->size);
		return -1;
	}
	if (g->array_lba < 2 || g->array_lba + MAX_ARRAY / SECTOR > disk_sectors) {
		say("%s header: entry array at LBA %llu", what, (unsigned long long)g->array_lba);
		return -1;
	}
	if (read_at(fd, g->array, (size_t)g->count * g->size, g->array_lba)) {
		say("%s entries: read failed", what);
		return -1;
	}
	if (crc32_le(g->array, (size_t)g->count * g->size) != get32(g->hdr + 88)) {
		say("%s entries: CRC mismatch", what);
		return -1;
	}
	return 0;
}

/* Both copies valid and consistent. Returns 0 with identical entry arrays,
 * -1 otherwise; the caller decides whether a difference is the known
 * half-written state (half_marked()). */
static int load_gpt(int fd, uint64_t disk_sectors, struct gpt *pri, struct gpt *bak)
{
	uint64_t last = disk_sectors - 1;

	if (load_copy(fd, 1, last, disk_sectors, pri, "primary") ||
	    load_copy(fd, last, 1, disk_sectors, bak, "backup"))
		return -2;
	if (pri->count != bak->count || pri->first_usable != bak->first_usable ||
	    pri->last_usable != bak->last_usable ||
	    memcmp(pri->hdr + 56, bak->hdr + 56, 16) /* disk GUID */ ||
	    memcmp(pri->hdr, bak->hdr, 8 + 4 + 4) /* sig, revision, size */) {
		say("primary and backup headers disagree");
		return -2;
	}
	return memcmp(pri->array, bak->array, (size_t)pri->count * pri->size) ? -1 : 0;
}

/* Index of the only entry named `name`, or -1. */
static int find_entry(const struct gpt *g, const char *name)
{
	int found = -1;

	for (uint32_t i = 0; i < g->count; i++) {
		const uint8_t *e = g->array + (size_t)i * g->size;
		char ascii[37];
		size_t k;

		for (k = 0; k < 36; k++) {
			uint16_t ch = (uint16_t)(e[56 + 2 * k] | e[57 + 2 * k] << 8);

			if (ch == 0)
				break;
			ascii[k] = ch < 0x80 ? (char)ch : '?';
		}
		ascii[k] = 0;
		if (strcmp(ascii, name))
			continue;
		if (found >= 0)
			return -2;
		found = (int)i;
	}
	return found;
}

static uint64_t entry_attr(const struct gpt *g, int i)
{
	return get64(g->array + (size_t)i * g->size + 48);
}

static int read_text(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "r");
	size_t r;

	if (!f)
		return -1;
	r = fread(buf, 1, n - 1, f);
	fclose(f);
	buf[r] = 0;
	return 0;
}

static int read_u64(const char *path, uint64_t *v)
{
	char buf[64], *end;

	if (read_text(path, buf, sizeof(buf)))
		return -1;
	errno = 0;
	*v = strtoull(buf, &end, 10);
	return (errno || end == buf || (*end && *end != '\n')) ? -1 : 0;
}

/* The kernel's PARTNAME=name partition of DISK_NAME, through class/block. */
static int sysfs_part(const char *sysfs, const char *name, struct part *p)
{
	char path[512], buf[1024], want_dev[64];
	DIR *d;
	struct dirent *de;
	int found = 0;

	snprintf(path, sizeof(path), "%s/class/block", sysfs);
	d = opendir(path);
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		char *line, *save;
		const char *devname = NULL, *partname = NULL, *partn = NULL;

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/class/block/%s/uevent", sysfs, de->d_name);
		if (read_text(path, buf, sizeof(buf)))
			continue;
		for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			if (!strncmp(line, "DEVNAME=", 8)) devname = line + 8;
			else if (!strncmp(line, "PARTNAME=", 9)) partname = line + 9;
			else if (!strncmp(line, "PARTN=", 6)) partn = line + 6;
		}
		if (!partname || strcmp(partname, name))
			continue;
		if (found++ || !devname || !partn) {
			found = 2;
			break;
		}
		p->partno = (unsigned)strtoul(partn, NULL, 10);
		snprintf(want_dev, sizeof(want_dev), DISK_NAME "p%u", p->partno);
		if (strcmp(devname, want_dev) || strcmp(de->d_name, want_dev)) {
			found = 2;
			break;
		}
		snprintf(path, sizeof(path), "%s/class/block/%s/start", sysfs, de->d_name);
		if (read_u64(path, &p->start)) { found = 2; break; }
		snprintf(path, sizeof(path), "%s/class/block/%s/size", sysfs, de->d_name);
		if (read_u64(path, &p->sectors)) { found = 2; break; }
	}
	closedir(d);
	return found == 1 ? 0 : -1;
}

static int booted_slot_a(const char *cmdline)
{
	char buf[4096], *tok, *save;

	if (read_text(cmdline, buf, sizeof(buf)))
		return 0;
	for (tok = strtok_r(buf, " \n", &save); tok; tok = strtok_r(NULL, " \n", &save))
		if (!strcmp(tok, "androidboot.slot_suffix=_a"))
			return 1;
	return 0;
}

static void print_flags(const char *name, int i, uint64_t attr)
{
	unsigned a = (unsigned)(attr >> 48);

	say("%s p%d attr=0x%016llx priority=%u active=%u retry=%u successful=%u unbootable=%u",
	    name, i + 1, (unsigned long long)attr, a & 3, (a >> 2) & 1, (a >> 3) & 7,
	    (a >> 6) & 1, (a >> 7) & 1);
}

static int write_sector(int fd, const uint8_t *src, uint64_t lba)
{
	uint8_t *buf = bounce();

	if (!buf)
		return -1;
	memcpy(buf, src, SECTOR);
	return pwrite(fd, buf, SECTOR, (off_t)(lba * SECTOR)) == SECTOR ? 0 : -1;
}

/* The disk, O_DIRECT (and O_SYNC for writing). Only a -d test image on a
 * filesystem without O_DIRECT support falls back to buffered I/O. */
static int open_disk(const struct opts *o, bool mark)
{
	int flags = mark ? O_RDWR | O_SYNC : O_RDONLY;
	int fd = open(o->disk, flags | O_DIRECT);
	struct stat st;

	if (fd < 0 && errno == EINVAL && o->test_disk &&
	    !stat(o->disk, &st) && S_ISREG(st.st_mode)) {
		say("TEST OVERRIDE: %s refuses O_DIRECT, using buffered I/O", o->disk);
		fd = open(o->disk, flags);
	}
	return fd;
}

/* Count differing bits between two buffers. */
static unsigned bitdiff(const uint8_t *a, const uint8_t *b, size_t n)
{
	unsigned bits = 0;

	for (size_t i = 0; i < n; i++)
		bits += (unsigned)__builtin_popcount((unsigned)(a[i] ^ b[i]));
	return bits;
}

/* Offset of boot_a's successful-bit byte within the entry array. */
static size_t success_byte(const struct gpt *g, int i)
{
	return (size_t)i * g->size + 48 + ATTR_BYTE;
}

/* The arrays differ in exactly one bit, boot_a's successful bit, and
 * `marked` has it set: the state an interrupted mark-successful leaves. */
static bool only_success_bit(const struct gpt *marked, const struct gpt *other, int i)
{
	size_t len = (size_t)marked->count * marked->size, at = success_byte(marked, i);

	return bitdiff(marked->array, other->array, len) == 1 &&
	       (marked->array[at] ^ other->array[at]) == ATTR_BIT &&
	       (marked->array[at] & ATTR_BIT);
}

/* After a write: `after` differs from `before` in exactly header bytes
 * 16-19 and 88-91 and boot_a's successful bit (or not at all if !written). */
static bool expected_change(const struct gpt *before, const struct gpt *after, int i, bool written)
{
	size_t len = (size_t)before->count * before->size, at = success_byte(before, i);

	if (!written)
		return !memcmp(before->hdr, after->hdr, SECTOR) &&
		       !memcmp(before->array, after->array, len);
	for (size_t k = 0; k < SECTOR; k++)
		if (before->hdr[k] != after->hdr[k] && !(k >= 16 && k < 20) && !(k >= 88 && k < 92))
			return false;
	return bitdiff(before->array, after->array, len) == 1 &&
	       (before->array[at] ^ after->array[at]) == ATTR_BIT && (after->array[at] & ATTR_BIT);
}

static int run(const struct opts *o, bool mark)
{
	static struct gpt pri, bak, chk_pri, chk_bak;
	struct part part;
	char path[512];
	uint64_t disk_sectors, lbs, attr;
	int fd, ia, ib, same;
	bool half = false;

	if (!booted_slot_a(o->cmdline)) {
		say("refusing: the kernel did not boot slot _a (%s)", o->cmdline);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/block/" DISK_NAME "/size", o->sysfs);
	if (read_u64(path, &disk_sectors) || disk_sectors < 68) {
		say("refusing: no usable %s", path);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/block/" DISK_NAME "/queue/logical_block_size", o->sysfs);
	if (read_u64(path, &lbs) || lbs != SECTOR) {
		say("refusing: logical block size is not %d", SECTOR);
		return 1;
	}
	if (sysfs_part(o->sysfs, "boot_a", &part)) {
		say("refusing: no single PARTNAME=boot_a partition of " DISK_NAME " in sysfs");
		return 1;
	}
	fd = open_disk(o, mark);
	if (fd < 0) {
		say("refusing: open %s: %s", o->disk, strerror(errno));
		return 1;
	}
	if (flock(fd, mark ? LOCK_EX : LOCK_SH)) {
		say("refusing: flock %s: %s", o->disk, strerror(errno));
		goto refuse;
	}
	same = load_gpt(fd, disk_sectors, &pri, &bak);
	if (same == -2)
		goto refuse;
	ia = find_entry(&pri, "boot_a");
	ib = find_entry(&pri, "boot_b");
	if (ia < 0) {
		say("refusing: %s boot_a entry", ia == -2 ? "more than one" : "no");
		goto refuse;
	}
	{
		const uint8_t *e = pri.array + (size_t)ia * pri.size;
		uint64_t first = get64(e + 32), last = get64(e + 40);

		if ((unsigned)ia + 1 != part.partno || first != part.start ||
		    last < first || last - first + 1 != part.sectors) {
			say("refusing: GPT boot_a p%d %llu+%llu does not match the kernel's p%u %llu+%llu",
			    ia + 1, (unsigned long long)first, (unsigned long long)(last - first + 1),
			    part.partno, (unsigned long long)part.start,
			    (unsigned long long)part.sectors);
			goto refuse;
		}
	}
	if (same == -1) {
		if (!only_success_bit(&bak, &pri, ia)) {
			say("refusing: primary and backup entry arrays differ%s",
			    only_success_bit(&pri, &bak, ia) ?
			    " (primary marked successful, backup not)" : "");
			goto refuse;
		}
		half = true;
		say("half-marked: backup has boot_a successful, primary does not");
	}
	attr = entry_attr(&pri, ia);
	print_flags("boot_a", ia, attr);
	if (ib >= 0)
		print_flags("boot_b", ib, entry_attr(&pri, ib));
	if (!mark) {
		close(fd);
		return 0;
	}
	if (attr & ATTR_SUCCESS) {
		say("boot_a already marked successful, nothing written");
		close(fd);
		return 0;
	}
	if (!(attr >> 50 & 1) || attr >> 55 & 1 || !(attr >> 48 & 3)) {
		say("refusing: boot_a is not the active bootable slot");
		goto refuse;
	}
	{
		size_t len = (size_t)pri.count * pri.size;
		size_t at = success_byte(&pri, ia);
		uint64_t rel = at / SECTOR;	/* sector of the byte within the array */
		uint8_t new_array[MAX_ARRAY], sec_pri[SECTOR], sec_bak[SECTOR];
		uint8_t hdr_pri[SECTOR], hdr_bak[SECTOR];
		uint64_t new_attr = attr | ATTR_SUCCESS;

		memset(new_array, 0, sizeof(new_array));
		memcpy(new_array, pri.array, len);
		new_array[at] |= ATTR_BIT;
		if (bitdiff(new_array, pri.array, len) != 1 ||
		    get64(new_array + (size_t)ia * pri.size + 48) != new_attr ||
		    (half && memcmp(new_array, bak.array, len))) {
			say("refusing: internal: new entry array is not the one-bit change");
			goto refuse;
		}
		/* The sector holding the byte: read whole (the array may end
		 * mid-sector), patch only that byte. */
		if (read_at(fd, sec_pri, SECTOR, pri.array_lba + rel) ||
		    read_at(fd, sec_bak, SECTOR, bak.array_lba + rel)) {
			say("refusing: re-read of the entry sector failed");
			goto refuse;
		}
		/* They must still hold what was validated (the part of the
		 * sector inside the array). */
		{
			size_t from = rel * SECTOR, n = len - from < SECTOR ? len - from : SECTOR;

			if (memcmp(sec_pri, pri.array + from, n) || memcmp(sec_bak, bak.array + from, n)) {
				say("refusing: entry sector changed since it was validated");
				goto refuse;
			}
		}
		sec_pri[at % SECTOR] |= ATTR_BIT;
		sec_bak[at % SECTOR] |= ATTR_BIT;
		memcpy(hdr_pri, pri.hdr, SECTOR);
		memcpy(hdr_bak, bak.hdr, SECTOR);
		put32(hdr_pri + 88, crc32_le(new_array, len));
		put32(hdr_bak + 88, crc32_le(new_array, len));
		put32(hdr_pri + 16, header_crc(hdr_pri));
		put32(hdr_bak + 16, header_crc(hdr_bak));

		say("boot_a attr 0x%016llx -> 0x%016llx%s", (unsigned long long)attr,
		    (unsigned long long)new_attr, half ? " (completing the primary)" : "");
		if ((!half && (write_sector(fd, sec_bak, bak.array_lba + rel) ||
			       write_sector(fd, hdr_bak, bak.my_lba) || fsync(fd))) ||
		    write_sector(fd, sec_pri, pri.array_lba + rel) ||
		    write_sector(fd, hdr_pri, pri.my_lba) || fsync(fd)) {
			say("WRITE FAILED: %s; check both GPT copies (abslot status)", strerror(errno));
			close(fd);
			return 2;
		}
		/* The re-read below is O_DIRECT, from the device; also drop any
		 * stale buffered copies other readers (dd) may hold. */
		if (ioctl(fd, BLKFLSBUF, 0) && errno != ENOTTY && errno != EINVAL) {
			say("BLKFLSBUF: %s", strerror(errno));
			close(fd);
			return 2;
		}
		if (load_gpt(fd, disk_sectors, &chk_pri, &chk_bak) != 0 ||
		    !expected_change(&pri, &chk_pri, ia, true) ||
		    !expected_change(&bak, &chk_bak, ia, !half) ||
		    memcmp(chk_pri.array, new_array, len) ||
		    memcmp(chk_pri.hdr, hdr_pri, SECTOR) || memcmp(chk_bak.hdr, hdr_bak, SECTOR)) {
			say("VERIFY FAILED after write; check both GPT copies (abslot status)");
			close(fd);
			return 2;
		}
		print_flags("boot_a", ia, entry_attr(&chk_pri, ia));
		if (half)
			say("boot_a marked successful (primary LBAs %llu, %llu written and verified)",
			    (unsigned long long)(pri.array_lba + rel), (unsigned long long)pri.my_lba);
		else
			say("boot_a marked successful (LBAs %llu, %llu, %llu, %llu written and verified)",
			    (unsigned long long)(bak.array_lba + rel), (unsigned long long)bak.my_lba,
			    (unsigned long long)(pri.array_lba + rel), (unsigned long long)pri.my_lba);
	}
	close(fd);
	return 0;
refuse:
	close(fd);
	return 1;
}

static void usage(void)
{
	fprintf(stderr, "usage: abslot [-d DISK] [-s SYSFS] [-c CMDLINE] [-K KMSG] [-L LOG] status|mark-successful\n");
}

#ifndef ABSLOT_NO_MAIN
int main(int argc, char **argv)
{
	struct opts o = { "/dev/" DISK_NAME, "/sys", "/proc/cmdline", false };
	const char *kmsg = "/dev/kmsg", *log = "/run/abslot.log";
	int c;

	setvbuf(stdout, NULL, _IOLBF, 0);
	opterr = 0;
	while ((c = getopt(argc, argv, "d:s:c:K:L:")) != -1)
		if (c == 'K') kmsg = optarg;
		else if (c == 'L') log = optarg;
	g_kmsg = fopen(kmsg, "w");
	g_log = fopen(log, "a");
	optind = 1;
	opterr = 1;
	while ((c = getopt(argc, argv, "d:s:c:K:L:")) != -1) {
		switch (c) {
		case 'K': case 'L': break;
		case 'd': o.disk = optarg; o.test_disk = true; say("TEST OVERRIDE: disk %s", optarg); break;
		case 's': o.sysfs = optarg; say("TEST OVERRIDE: sysfs %s", optarg); break;
		case 'c': o.cmdline = optarg; say("TEST OVERRIDE: cmdline %s", optarg); break;
		default: usage(); return 64;
		}
	}
	if (optind + 1 != argc) {
		usage();
		return 64;
	}
	if (!strcmp(argv[optind], "status"))
		return run(&o, false);
	if (!strcmp(argv[optind], "mark-successful"))
		return run(&o, true);
	usage();
	return 64;
}
#endif
