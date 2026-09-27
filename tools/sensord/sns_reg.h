/* sns_reg: the sensors registry sensord serves as REG2 (0x10f), held in
 * RAM. See sns-reg-map.py for the file layout and where the map comes
 * from. The backing file is a copy of persist's sns.reg that sensors-up
 * made under /run; writes the DSP sends land in the RAM copy, and
 * sns_reg_save() can write that back over the /run copy (only ever onto
 * a RAM filesystem), so the copy keeps what the DSP wrote for the rest
 * of the boot. persist itself is never written. */
#ifndef CHEF_CYCLO_SNS_REG_H
#define CHEF_CYCLO_SNS_REG_H

#include <stddef.h>
#include <stdint.h>

struct sns_reg_span {
	uint16_t id;
	uint16_t size;
	uint32_t offset;
};

struct sns_reg {
	uint8_t *data;
	size_t size;			/* from the map's "size" line */
	struct sns_reg_span *groups;	/* sorted by id */
	size_t ngroups;
	struct sns_reg_span *items;	/* sorted by id */
	size_t nitems;
	unsigned long reads, writes, misses;
};

/* Parse a map file (sns-reg-map.py output). 0 or -errno; *line gets the
 * offending line number on -EINVAL. */
int sns_reg_load_map(struct sns_reg *r, const char *path, int *line);

/* Load the registry bytes. The file must be exactly the map's size unless
 * allow_short (then it is zero-padded and the caller logs it). 0/-errno. */
int sns_reg_load_data(struct sns_reg *r, const char *path, int allow_short, size_t *got);

void sns_reg_free(struct sns_reg *r);

const struct sns_reg_span *sns_reg_find(const struct sns_reg_span *v, size_t n, uint16_t id);

/* Copy out an item/group. Returns its size, or -1 (unknown id). */
int sns_reg_read(struct sns_reg *r, int group, uint16_t id, uint8_t *out, size_t outsz);

/* Write the RAM registry back to path: path.tmp (created with path's
 * mode), fsync, rename over path. Refuses (-EXDEV) unless path and the
 * temp file are both on tmpfs or ramfs, so it can never reach persist or any other disk, and
 * (-EINVAL) unless path is an existing regular file. 0 or -errno; a
 * failure leaves path as it was. */
int sns_reg_save(const struct sns_reg *r, const char *path);

/* Overwrite an item/group (or its first len bytes) in RAM. Returns 0,
 * -ENOENT (unknown id) or -EINVAL (empty, or longer than the entry). */
int sns_reg_write(struct sns_reg *r, int group, uint16_t id, const uint8_t *in, size_t len);

#endif
