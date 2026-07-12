// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// ext4.c -- minimal ext4 reader/writer, used by posix_shim.c to back
// open/read/write/close/lseek/fstat/unlink. The kernel itself has no
// filesystem support (see libBareMetal.h's b_nvs_read/b_nvs_write --
// raw 4096-byte sector I/O only), so this parses the on-disk ext4
// layout directly (superblock, block group descriptors, inodes,
// extent trees, linear directory entries).
//
// This is not a general-purpose ext4 implementation -- it supports
// exactly the subset needed to serve as a POSIX file-I/O backend for
// a single flat root directory, and requires filesystem images built
// with a constrained feature set. See the "Required image layout" and
// "Limitations" comments below, and OPENISSUES.md.
//
// Required image layout (i.e. what `mkfs.ext4` must be told to
// produce -- this driver does not check every one of these and will
// silently misbehave if they don't hold):
//   - Block size 4096 (matches b_nvs_read/write's fixed 4096-byte
//     sector unit exactly, so "ext4 block" and "disk sector" are the
//     same unit -- no sub/multi-sector packing needed).
//   - Inode size 256 (the modern mkfs.ext4 default).
//   - `64bit`, `metadata_csum`, and `meta_bg` features disabled (this
//     driver has no checksum support and assumes 32-byte group
//     descriptors and a single contiguous group descriptor table).
//   - All regular files use extents (EXT4_EXTENTS_FL), never the
//     legacy indirect-block layout -- true for every file this driver
//     creates itself, and for anything written by a modern mkfs.ext4/
//     e2fsprogs.
//
// Example: mkfs.ext4 -b 4096 -I 256 -O ^64bit,^metadata_csum,^meta_bg disk.img
//
// Limitations (see OPENISSUES.md for the full list):
//   - Flat namespace only: paths are looked up directly in the root
//     directory (inode 2); embedded '/' is rejected. ext4 itself
//     supports real subdirectories, but nothing above this layer
//     (posix_shim.c) needs them yet.
//   - Directory entries are only ever scanned/inserted linearly.
//     htree-indexed directories are still read correctly (htree
//     index/root blocks are, by the on-disk format's own backward-
//     compatibility design, formatted as a single dirent with inode
//     0 spanning the whole block, so a linear scan safely skips them
//     and still finds every real entry in the leaf blocks) but this
//     driver never creates an htree, so a directory it grows stays
//     linear-only.
//   - Extent trees are only ever grown to depth 1 (an inode's 4
//     in-inode extents, or up to 4 index entries each pointing to one
//     4096-byte leaf block of up to 340 extents). No depth-2+ trees.
//     Plenty of range for this environment's expected file sizes, but
//     a hard cap.
//   - No journal replay and no checksums are written/verified --
//     every operation is expected to complete without a crash
//     partway through, same as BMFS before it.
// =============================================================================

#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

#include "libBareMetal.h"
#include "ext4.h"

#define EXT4_DRIVE       0
#define EXT4_BLOCK_BYTES 4096ULL
#define EXT4_INODE_SIZE  256U
#define EXT4_MAGIC       0xEF53
#define EXT4_ROOT_INO    2U
#define EXT4_MAX_GROUPS  128 /* 128 groups * 128MiB/group = 16GiB max image */

#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR      2

#define EXT4_EXTENTS_FL  0x00080000U
#define EXT4_INDEX_FL    0x00001000U

#define EXT4_EXTENT_MAGIC 0xF30A

#define EXT4_FD_BASE  3
#define EXT4_MAX_OPEN 8

// -----------------------------------------------------------------------
// On-disk structures (see Documentation/filesystems/ext4/ in the Linux
// tree for the authoritative layout this mirrors field-for-field).
// -----------------------------------------------------------------------

struct ext4_super_block {
	u32 s_inodes_count;
	u32 s_blocks_count_lo;
	u32 s_r_blocks_count_lo;
	u32 s_free_blocks_count_lo;
	u32 s_free_inodes_count;
	u32 s_first_data_block;
	u32 s_log_block_size;
	u32 s_log_cluster_size;
	u32 s_blocks_per_group;
	u32 s_clusters_per_group;
	u32 s_inodes_per_group;
	u32 s_mtime;
	u32 s_wtime;
	u16 s_mnt_count;
	u16 s_max_mnt_count;
	u16 s_magic;
	u16 s_state;
	u16 s_errors;
	u16 s_minor_rev_level;
	u32 s_lastcheck;
	u32 s_checkinterval;
	u32 s_creator_os;
	u32 s_rev_level;
	u16 s_def_resuid;
	u16 s_def_resgid;
	// EXT4_DYNAMIC_REV fields
	u32 s_first_ino;
	u16 s_inode_size;
	u16 s_block_group_nr;
	u32 s_feature_compat;
	u32 s_feature_incompat;
	u32 s_feature_ro_compat;
	u8  s_uuid[16];
	char s_volume_name[16];
	char s_last_mounted[64];
	u32 s_algorithm_usage_bitmap;
	u8  s_prealloc_blocks;
	u8  s_prealloc_dir_blocks;
	u16 s_reserved_gdt_blocks;
	u8  s_journal_uuid[16];
	u32 s_journal_inum;
	u32 s_journal_dev;
	u32 s_last_orphan;
	u32 s_hash_seed[4];
	u8  s_def_hash_version;
	u8  s_jnl_backup_type;
	u16 s_desc_size;
	u32 s_default_mount_opts;
	u32 s_first_meta_bg;
	u32 s_mkfs_time;
	u32 s_jnl_blocks[17];
	u32 s_blocks_count_hi;
	u32 s_r_blocks_count_hi;
	u32 s_free_blocks_count_hi;
	u16 s_min_extra_isize;
	u16 s_want_extra_isize;
	u32 s_flags;
	u16 s_raid_stride;
	u16 s_mmp_update_interval;
	u64 s_mmp_block;
	u32 s_raid_stripe_width;
	u8  s_log_groups_per_flex;
	u8  s_checksum_type;
	u16 s_reserved_pad;
	u64 s_kbytes_written;
	u32 s_snapshot_inum;
	u32 s_snapshot_id;
	u64 s_snapshot_r_blocks_count;
	u32 s_snapshot_list;
	u32 s_error_count;
	u32 s_first_error_time;
	u32 s_first_error_ino;
	u64 s_first_error_block;
	u8  s_first_error_func[32];
	u32 s_first_error_line;
	u32 s_last_error_time;
	u32 s_last_error_ino;
	u32 s_last_error_line;
	u64 s_last_error_block;
	u8  s_last_error_func[32];
	u8  s_mount_opts[64];
	u32 s_usr_quota_inum;
	u32 s_grp_quota_inum;
	u32 s_overhead_clusters;
	u32 s_backup_bgs[2];
	u8  s_encrypt_algos[4];
	u8  s_encrypt_pw_salt[16];
	u32 s_lpf_ino;
	u32 s_prj_quota_inum;
	u32 s_checksum_seed;
	u8  s_wtime_hi;
	u8  s_mtime_hi;
	u8  s_mkfs_time_hi;
	u8  s_lastcheck_hi;
	u8  s_first_error_time_hi;
	u8  s_last_error_time_hi;
	u8  s_first_error_errcode;
	u8  s_last_error_errcode;
	u16 s_encoding;
	u16 s_encoding_flags;
	u32 s_orphan_file_inum;
	u32 s_reserved[94];
	u32 s_checksum;
} __attribute__((packed));

struct ext4_group_desc {
	u32 bg_block_bitmap_lo;
	u32 bg_inode_bitmap_lo;
	u32 bg_inode_table_lo;
	u16 bg_free_blocks_count_lo;
	u16 bg_free_inodes_count_lo;
	u16 bg_used_dirs_count_lo;
	u16 bg_flags;
	u32 bg_exclude_bitmap_lo;
	u16 bg_block_bitmap_csum_lo;
	u16 bg_inode_bitmap_csum_lo;
	u16 bg_itable_unused_lo;
	u16 bg_checksum;
} __attribute__((packed));

// Base inode fields this driver interprets. The on-disk record is
// EXT4_INODE_SIZE (256) bytes; everything past this struct (extended
// attributes, high-precision timestamp fields we don't touch) is
// preserved by always reading/writing whole EXT4_INODE_SIZE-byte
// records rather than just sizeof(struct ext4_inode).
struct ext4_inode {
	u16 i_mode;
	u16 i_uid_lo;
	u32 i_size_lo;
	u32 i_atime;
	u32 i_ctime;
	u32 i_mtime;
	u32 i_dtime;
	u16 i_gid_lo;
	u16 i_links_count;
	u32 i_blocks_lo;
	u32 i_flags;
	u32 l_i_version;
	u32 i_block[15]; // extent header + entries live here
	u32 i_generation;
	u32 i_file_acl_lo;
	u32 i_size_high;
	u32 i_obso_faddr;
	u16 l_i_blocks_high;
	u16 l_i_file_acl_high;
	u16 l_i_uid_high;
	u16 l_i_gid_high;
	u16 l_i_checksum_lo;
	u16 l_i_reserved;
	u16 i_extra_isize;
	u16 i_checksum_hi;
	u32 i_ctime_extra;
	u32 i_mtime_extra;
	u32 i_atime_extra;
	u32 i_crtime;
	u32 i_crtime_extra;
	u32 i_version_hi;
	u32 i_projid;
} __attribute__((packed));

struct ext4_extent_header {
	u16 eh_magic;
	u16 eh_entries;
	u16 eh_max;
	u16 eh_depth;
	u32 eh_generation;
} __attribute__((packed));

struct ext4_extent {
	u32 ee_block;
	u16 ee_len;
	u16 ee_start_hi;
	u32 ee_start_lo;
} __attribute__((packed));

struct ext4_extent_idx {
	u32 ei_block;
	u32 ei_leaf_lo;
	u16 ei_leaf_hi;
	u16 ei_unused;
} __attribute__((packed));

#define EXT4_ROOT_MAX_ENTRIES 4U /* (60 - sizeof(header)) / sizeof(extent) */
#define EXT4_LEAF_MAX_ENTRIES ((u16)((EXT4_BLOCK_BYTES - 12) / 12))

struct ext4_dirent {
	u32 inode;
	u16 rec_len;
	u8  name_len;
	u8  file_type;
	char name[]; // no NUL terminator; name_len bytes follow
} __attribute__((packed));

// x86_64 musl has no SYS_stat in this build; stat()/lstat()/
// fstatat() all funnel through fstatat(), which musl's own
// src/internal/syscall.h aliases to SYS_newfstatat. That path fills a
// "struct kstat" (arch/x86_64/kstat.h), which is what this mirrors
// field-for-field -- there's no real kernel on the other end to be
// ABI-compatible with, just musl's own parsing of these bytes.
struct linux_kstat {
	unsigned long st_dev;
	unsigned long st_ino;
	unsigned long st_nlink;

	unsigned int st_mode;
	unsigned int st_uid;
	unsigned int st_gid;
	unsigned int __pad0;
	unsigned long st_rdev;
	long st_size;
	long st_blksize;
	long st_blocks;

	long st_atime_sec;
	long st_atime_nsec;
	long st_mtime_sec;
	long st_mtime_nsec;
	long st_ctime_sec;
	long st_ctime_nsec;
	long __unused[3];
};

// -----------------------------------------------------------------------
// Mount state
// -----------------------------------------------------------------------

struct ext4_file {
	int used;
	int append;
	u32 inum;
	u64 pos;
};

static struct ext4_super_block sb;
static struct ext4_group_desc gd[EXT4_MAX_GROUPS];
static int mounted = 0;
static u32 groups_count;
static struct ext4_file files[EXT4_MAX_OPEN];

// Two scratch block buffers. bmp_buf is used only within the
// allocate/free bitmap helpers, which never need a live block held by
// their caller to survive the call. blk_buf is used everywhere else
// (superblock block, group descriptor table, inode table blocks,
// directory data blocks, extent leaf blocks); every user of blk_buf
// finishes reading whatever it needs from it (or writes it back)
// before making any call that might reuse it -- see the ordering
// notes next to each call site below.
static unsigned char blk_buf[EXT4_BLOCK_BYTES];
static unsigned char bmp_buf[EXT4_BLOCK_BYTES];

static void sb_flush(void)
{
	b_nvs_read(blk_buf, 0, 1, EXT4_DRIVE);
	memcpy(blk_buf + 1024, &sb, sizeof(sb));
	b_nvs_write(blk_buf, 0, 1, EXT4_DRIVE);
}

static void gd_flush(void)
{
	memset(blk_buf, 0, sizeof(blk_buf));
	memcpy(blk_buf, gd, groups_count * sizeof(gd[0]));
	b_nvs_write(blk_buf, sb.s_first_data_block + 1, 1, EXT4_DRIVE);
}

// Mounts on first use. Returns 0 on success, -EIO if the image
// doesn't satisfy this driver's required layout (see the file-level
// comment above).
static long ext4_mount(void)
{
	if (mounted)
		return 0;

	b_nvs_read(blk_buf, 0, 1, EXT4_DRIVE);
	memcpy(&sb, blk_buf + 1024, sizeof(sb));

	if (sb.s_magic != EXT4_MAGIC)
		return -EIO;
	if ((1024ULL << sb.s_log_block_size) != EXT4_BLOCK_BYTES)
		return -EIO;
	if (sb.s_inode_size != EXT4_INODE_SIZE)
		return -EIO;

	groups_count = (sb.s_blocks_count_lo + sb.s_blocks_per_group - 1) / sb.s_blocks_per_group;
	if (groups_count == 0 || groups_count > EXT4_MAX_GROUPS)
		return -EIO;

	b_nvs_read(blk_buf, sb.s_first_data_block + 1, 1, EXT4_DRIVE);
	memcpy(gd, blk_buf, groups_count * sizeof(gd[0]));

	mounted = 1;
	return 0;
}

// -----------------------------------------------------------------------
// Inode table I/O
// -----------------------------------------------------------------------

static void inode_location(u32 inum, u32 *block, u32 *off)
{
	u32 idx0 = inum - 1;
	u32 group = idx0 / sb.s_inodes_per_group;
	u32 index = idx0 % sb.s_inodes_per_group;
	u64 byte = (u64)index * EXT4_INODE_SIZE;

	*block = gd[group].bg_inode_table_lo + (u32)(byte / EXT4_BLOCK_BYTES);
	*off = (u32)(byte % EXT4_BLOCK_BYTES);
}

static void inode_read(u32 inum, unsigned char *out)
{
	u32 block, off;

	inode_location(inum, &block, &off);
	b_nvs_read(blk_buf, block, 1, EXT4_DRIVE);
	memcpy(out, blk_buf + off, EXT4_INODE_SIZE);
}

static void inode_write(u32 inum, const unsigned char *in)
{
	u32 block, off;

	inode_location(inum, &block, &off);
	b_nvs_read(blk_buf, block, 1, EXT4_DRIVE);
	memcpy(blk_buf + off, in, EXT4_INODE_SIZE);
	b_nvs_write(blk_buf, block, 1, EXT4_DRIVE);
}

static u64 inode_size(const struct ext4_inode *ino)
{
	return ((u64)ino->i_size_high << 32) | ino->i_size_lo;
}

static void inode_set_size(struct ext4_inode *ino, u64 size)
{
	ino->i_size_lo = (u32)size;
	ino->i_size_high = (u32)(size >> 32);
}

// -----------------------------------------------------------------------
// Block/inode bitmap allocation
// -----------------------------------------------------------------------

static long alloc_block(u64 *out)
{
	for (u32 g = 0; g < groups_count; g++) {
		if (gd[g].bg_free_blocks_count_lo == 0)
			continue;

		b_nvs_read(bmp_buf, gd[g].bg_block_bitmap_lo, 1, EXT4_DRIVE);
		for (u32 i = 0; i < EXT4_BLOCK_BYTES * 8; i++) {
			if (bmp_buf[i / 8] & (1 << (i % 8)))
				continue;

			bmp_buf[i / 8] |= (1 << (i % 8));
			b_nvs_write(bmp_buf, gd[g].bg_block_bitmap_lo, 1, EXT4_DRIVE);

			gd[g].bg_free_blocks_count_lo--;
			sb.s_free_blocks_count_lo--;
			gd_flush();
			sb_flush();

			*out = sb.s_first_data_block + (u64)g * sb.s_blocks_per_group + i;
			return 0;
		}
	}

	return -ENOSPC;
}

static void free_block(u64 block)
{
	u64 rel = block - sb.s_first_data_block;
	u32 g = (u32)(rel / sb.s_blocks_per_group);
	u32 i = (u32)(rel % sb.s_blocks_per_group);

	b_nvs_read(bmp_buf, gd[g].bg_block_bitmap_lo, 1, EXT4_DRIVE);
	bmp_buf[i / 8] &= ~(1 << (i % 8));
	b_nvs_write(bmp_buf, gd[g].bg_block_bitmap_lo, 1, EXT4_DRIVE);

	gd[g].bg_free_blocks_count_lo++;
	sb.s_free_blocks_count_lo++;
	gd_flush();
	sb_flush();
}

static long alloc_inode(u32 *out)
{
	for (u32 g = 0; g < groups_count; g++) {
		if (gd[g].bg_free_inodes_count_lo == 0)
			continue;

		b_nvs_read(bmp_buf, gd[g].bg_inode_bitmap_lo, 1, EXT4_DRIVE);
		for (u32 i = 0; i < sb.s_inodes_per_group; i++) {
			if (bmp_buf[i / 8] & (1 << (i % 8)))
				continue;

			bmp_buf[i / 8] |= (1 << (i % 8));
			b_nvs_write(bmp_buf, gd[g].bg_inode_bitmap_lo, 1, EXT4_DRIVE);

			gd[g].bg_free_inodes_count_lo--;
			sb.s_free_inodes_count--;
			gd_flush();
			sb_flush();

			*out = g * sb.s_inodes_per_group + i + 1;
			return 0;
		}
	}

	return -ENOSPC;
}

static void free_inode(u32 inum)
{
	u32 idx0 = inum - 1;
	u32 g = idx0 / sb.s_inodes_per_group;
	u32 i = idx0 % sb.s_inodes_per_group;

	b_nvs_read(bmp_buf, gd[g].bg_inode_bitmap_lo, 1, EXT4_DRIVE);
	bmp_buf[i / 8] &= ~(1 << (i % 8));
	b_nvs_write(bmp_buf, gd[g].bg_inode_bitmap_lo, 1, EXT4_DRIVE);

	gd[g].bg_free_inodes_count_lo++;
	sb.s_free_inodes_count++;
	gd_flush();
	sb_flush();
}

// -----------------------------------------------------------------------
// Extent tree: maps an inode's logical blocks to physical blocks.
// Supports depth 0 (up to 4 extents directly in the inode) and depth 1
// (up to 4 index entries, each pointing to one 4096-byte leaf block of
// up to EXT4_LEAF_MAX_ENTRIES extents) -- see the file-level comment.
// -----------------------------------------------------------------------

static long leaf_find(unsigned char *block_buf, u64 logical, u64 *phys)
{
	struct ext4_extent_header *eh = (struct ext4_extent_header *)block_buf;
	struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);

	for (u16 i = 0; i < eh->eh_entries; i++) {
		u32 len = ex[i].ee_len & 0x7FFF;
		if (logical >= ex[i].ee_block && logical < (u64)ex[i].ee_block + len) {
			u64 start = ((u64)ex[i].ee_start_hi << 32) | ex[i].ee_start_lo;
			*phys = start + (logical - ex[i].ee_block);
			return 0;
		}
	}
	return -1;
}

// blk_buf ordering: only used here (for a depth-1 leaf) before
// returning; caller is free to reuse blk_buf immediately after.
static long ext_map(struct ext4_inode *ino, u64 logical, u64 *phys)
{
	struct ext4_extent_header *eh = (struct ext4_extent_header *)ino->i_block;

	if (eh->eh_magic != EXT4_EXTENT_MAGIC)
		return -1;

	if (eh->eh_depth == 0)
		return leaf_find((unsigned char *)ino->i_block, logical, phys);

	struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);
	long which = -1;
	for (u16 i = 0; i < eh->eh_entries; i++) {
		if (idx[i].ei_block <= logical)
			which = i;
	}
	if (which < 0)
		return -1;

	u64 leaf = ((u64)idx[which].ei_leaf_hi << 32) | idx[which].ei_leaf_lo;
	b_nvs_read(blk_buf, leaf, 1, EXT4_DRIVE);
	return leaf_find(blk_buf, logical, phys);
}

// Tries to merge (logical,phys) into the last extent of the entries
// array, else appends a new entry if there's room. Returns 0 if
// merged/appended, -1 if the array is full.
static long entries_insert(struct ext4_extent *ex, u16 *count, u16 max, u64 logical, u64 phys)
{
	if (*count > 0) {
		struct ext4_extent *last = &ex[*count - 1];
		u32 last_len = last->ee_len & 0x7FFF;
		u64 last_start = ((u64)last->ee_start_hi << 32) | last->ee_start_lo;

		if ((u64)last->ee_block + last_len == logical &&
		    last_start + last_len == phys && last_len + 1 < 32768) {
			last->ee_len = (u16)(last_len + 1);
			return 0;
		}
	}

	if (*count >= max)
		return -1;

	ex[*count].ee_block = (u32)logical;
	ex[*count].ee_len = 1;
	ex[*count].ee_start_hi = (u16)(phys >> 32);
	ex[*count].ee_start_lo = (u32)phys;
	(*count)++;
	return 0;
}

// Adds a mapping for a newly-written logical block. May allocate
// metadata blocks (leaf/index blocks) of its own via alloc_block().
// blk_buf ordering: any blk_buf use here (building/reading a leaf)
// completes and is written back before the next nested call that
// might reuse blk_buf (alloc_block only touches bmp_buf).
static long extent_insert(struct ext4_inode *ino, u64 logical, u64 phys)
{
	struct ext4_extent_header *eh = (struct ext4_extent_header *)ino->i_block;

	if (eh->eh_magic != EXT4_EXTENT_MAGIC) {
		eh->eh_magic = EXT4_EXTENT_MAGIC;
		eh->eh_entries = 0;
		eh->eh_max = EXT4_ROOT_MAX_ENTRIES;
		eh->eh_depth = 0;
		eh->eh_generation = 0;
	}

	if (eh->eh_depth == 0) {
		struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);
		u16 count = eh->eh_entries;

		if (entries_insert(ex, &count, EXT4_ROOT_MAX_ENTRIES, logical, phys) == 0) {
			eh->eh_entries = count;
			return 0;
		}

		// Root's 4 slots are full: promote to depth 1. Move the
		// existing 4 extents into a new leaf block, then insert
		// the new mapping into that leaf (which has much more
		// room), then rewrite the root as a depth-1 index.
		u64 leaf_blk;
		long rc = alloc_block(&leaf_blk);
		if (rc < 0)
			return rc;

		struct ext4_extent_header *leh = (struct ext4_extent_header *)blk_buf;
		leh->eh_magic = EXT4_EXTENT_MAGIC;
		leh->eh_entries = EXT4_ROOT_MAX_ENTRIES;
		leh->eh_max = EXT4_LEAF_MAX_ENTRIES;
		leh->eh_depth = 0;
		leh->eh_generation = 0;
		memcpy(leh + 1, ex, EXT4_ROOT_MAX_ENTRIES * sizeof(struct ext4_extent));

		u16 lcount = EXT4_ROOT_MAX_ENTRIES;
		struct ext4_extent *lex = (struct ext4_extent *)(leh + 1);
		if (entries_insert(lex, &lcount, EXT4_LEAF_MAX_ENTRIES, logical, phys) < 0)
			return -ENOSPC; // unreachable: leaf has far more room than 5
		leh->eh_entries = lcount;
		b_nvs_write(blk_buf, leaf_blk, 1, EXT4_DRIVE);
		ino->i_blocks_lo += (u32)(EXT4_BLOCK_BYTES / 512); // the leaf block itself counts

		eh->eh_magic = EXT4_EXTENT_MAGIC;
		eh->eh_entries = 1;
		eh->eh_max = EXT4_ROOT_MAX_ENTRIES;
		eh->eh_depth = 1;
		eh->eh_generation = 0;
		struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);
		idx[0].ei_block = 0;
		idx[0].ei_leaf_lo = (u32)leaf_blk;
		idx[0].ei_leaf_hi = (u16)(leaf_blk >> 32);
		idx[0].ei_unused = 0;
		return 0;
	}

	// Depth 1: try the last leaf first.
	struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);
	u16 n = eh->eh_entries;
	u64 last_leaf = ((u64)idx[n - 1].ei_leaf_hi << 32) | idx[n - 1].ei_leaf_lo;

	b_nvs_read(blk_buf, last_leaf, 1, EXT4_DRIVE);
	struct ext4_extent_header *leh = (struct ext4_extent_header *)blk_buf;
	struct ext4_extent *lex = (struct ext4_extent *)(leh + 1);
	u16 lcount = leh->eh_entries;

	if (entries_insert(lex, &lcount, leh->eh_max, logical, phys) == 0) {
		leh->eh_entries = lcount;
		b_nvs_write(blk_buf, last_leaf, 1, EXT4_DRIVE);
		return 0;
	}

	// That leaf is full: need a new one, which needs a free root
	// index slot.
	if (n >= EXT4_ROOT_MAX_ENTRIES)
		return -EFBIG; // hit this driver's depth-1 size cap

	u64 new_leaf;
	long rc = alloc_block(&new_leaf);
	if (rc < 0)
		return rc;

	struct ext4_extent_header *nleh = (struct ext4_extent_header *)blk_buf;
	nleh->eh_magic = EXT4_EXTENT_MAGIC;
	nleh->eh_entries = 1;
	nleh->eh_max = EXT4_LEAF_MAX_ENTRIES;
	nleh->eh_depth = 0;
	nleh->eh_generation = 0;
	struct ext4_extent *nex = (struct ext4_extent *)(nleh + 1);
	nex[0].ee_block = (u32)logical;
	nex[0].ee_len = 1;
	nex[0].ee_start_hi = (u16)(phys >> 32);
	nex[0].ee_start_lo = (u32)phys;
	b_nvs_write(blk_buf, new_leaf, 1, EXT4_DRIVE);
	ino->i_blocks_lo += (u32)(EXT4_BLOCK_BYTES / 512); // the leaf block itself counts

	idx[n].ei_block = (u32)logical;
	idx[n].ei_leaf_lo = (u32)new_leaf;
	idx[n].ei_leaf_hi = (u16)(new_leaf >> 32);
	idx[n].ei_unused = 0;
	eh->eh_entries = n + 1;
	return 0;
}

// Frees every physical block mapped by ino's extent tree (data blocks
// and, for depth 1, the leaf metadata blocks themselves), then resets
// the tree to an empty depth-0 leaf.
static void extent_free_all(struct ext4_inode *ino)
{
	struct ext4_extent_header *eh = (struct ext4_extent_header *)ino->i_block;

	if (eh->eh_magic == EXT4_EXTENT_MAGIC) {
		if (eh->eh_depth == 0) {
			struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);
			for (u16 i = 0; i < eh->eh_entries; i++) {
				u32 len = ex[i].ee_len & 0x7FFF;
				u64 start = ((u64)ex[i].ee_start_hi << 32) | ex[i].ee_start_lo;
				for (u32 j = 0; j < len; j++)
					free_block(start + j);
			}
		} else {
			struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);
			for (u16 i = 0; i < eh->eh_entries; i++) {
				u64 leaf = ((u64)idx[i].ei_leaf_hi << 32) | idx[i].ei_leaf_lo;
				b_nvs_read(blk_buf, leaf, 1, EXT4_DRIVE);
				struct ext4_extent_header *leh = (struct ext4_extent_header *)blk_buf;
				struct ext4_extent *lex = (struct ext4_extent *)(leh + 1);
				for (u16 j = 0; j < leh->eh_entries; j++) {
					u32 len = lex[j].ee_len & 0x7FFF;
					u64 start = ((u64)lex[j].ee_start_hi << 32) | lex[j].ee_start_lo;
					for (u32 k = 0; k < len; k++)
						free_block(start + k);
				}
				free_block(leaf);
			}
		}
	}

	memset(ino->i_block, 0, sizeof(ino->i_block));
	eh->eh_magic = EXT4_EXTENT_MAGIC;
	eh->eh_entries = 0;
	eh->eh_max = EXT4_ROOT_MAX_ENTRIES;
	eh->eh_depth = 0;
	eh->eh_generation = 0;
}

// -----------------------------------------------------------------------
// Linear directory operations (root directory only -- see the
// file-level comment on the flat-namespace limitation).
// -----------------------------------------------------------------------

static u32 dirent_ideal_len(u8 name_len)
{
	return (8 + name_len + 3) & ~3U;
}

// Scans dir's data blocks for `name`. Returns the inode number, or 0
// if not found.
static u32 dir_find(struct ext4_inode *dir, const char *name, size_t name_len)
{
	u64 nblocks = (inode_size(dir) + EXT4_BLOCK_BYTES - 1) / EXT4_BLOCK_BYTES;

	for (u64 b = 0; b < nblocks; b++) {
		u64 phys;
		if (ext_map(dir, b, &phys) < 0)
			continue;

		// blk_buf ordering: ext_map's own blk_buf use (if any,
		// for a depth-1 leaf) is done by the time it returns; we
		// now reuse blk_buf for the actual directory block.
		b_nvs_read(blk_buf, phys, 1, EXT4_DRIVE);

		u32 pos = 0;
		while (pos < EXT4_BLOCK_BYTES) {
			struct ext4_dirent *de = (struct ext4_dirent *)(blk_buf + pos);
			if (de->rec_len == 0)
				break;
			if (de->inode != 0 && de->name_len == name_len &&
			    memcmp(de->name, name, name_len) == 0)
				return de->inode;
			pos += de->rec_len;
		}
	}

	return 0;
}

// Inserts a new (name -> inum) entry into dir, growing it by one
// block if no existing block has room. `dir_inum` is dir's own inode
// number (needed to flush dir's inode if it grows).
static long dir_insert(struct ext4_inode *dir, u32 dir_inum, const char *name, size_t name_len, u32 inum, u8 file_type)
{
	u32 needed = dirent_ideal_len((u8)name_len);
	u64 nblocks = (inode_size(dir) + EXT4_BLOCK_BYTES - 1) / EXT4_BLOCK_BYTES;

	for (u64 b = 0; b < nblocks; b++) {
		u64 phys;
		if (ext_map(dir, b, &phys) < 0)
			continue;

		b_nvs_read(blk_buf, phys, 1, EXT4_DRIVE);

		u32 pos = 0;
		while (pos < EXT4_BLOCK_BYTES) {
			struct ext4_dirent *de = (struct ext4_dirent *)(blk_buf + pos);
			if (de->rec_len == 0)
				break;

			u32 ideal = de->inode != 0 ? dirent_ideal_len(de->name_len) : 0;
			u32 slack = de->rec_len - ideal;

			if (slack >= needed) {
				struct ext4_dirent *nde;

				if (ideal == 0) {
					// Reusing a fully-free (tombstone)
					// slot.
					nde = de;
				} else {
					de->rec_len = (u16)ideal;
					nde = (struct ext4_dirent *)(blk_buf + pos + ideal);
					nde->rec_len = (u16)slack;
				}

				nde->inode = inum;
				nde->name_len = (u8)name_len;
				nde->file_type = file_type;
				memcpy(nde->name, name, name_len);

				b_nvs_write(blk_buf, phys, 1, EXT4_DRIVE);
				return 0;
			}

			pos += de->rec_len;
		}
	}

	// No room in any existing block: grow the directory by one
	// block, formatted as a single dirent spanning it.
	u64 new_phys;
	long rc = alloc_block(&new_phys);
	if (rc < 0)
		return rc;

	// blk_buf ordering: extent_insert() finishes all of its own
	// blk_buf use (leaf construction) and returns before we touch
	// blk_buf again below.
	rc = extent_insert(dir, nblocks, new_phys);
	if (rc < 0) {
		free_block(new_phys);
		return rc;
	}

	memset(blk_buf, 0, EXT4_BLOCK_BYTES);
	struct ext4_dirent *de = (struct ext4_dirent *)blk_buf;
	de->inode = inum;
	de->rec_len = (u16)EXT4_BLOCK_BYTES;
	de->name_len = (u8)name_len;
	de->file_type = file_type;
	memcpy(de->name, name, name_len);
	b_nvs_write(blk_buf, new_phys, 1, EXT4_DRIVE);

	inode_set_size(dir, (nblocks + 1) * EXT4_BLOCK_BYTES);
	dir->i_blocks_lo += (u32)(EXT4_BLOCK_BYTES / 512);
	// This driver never builds an htree index; clear the flag so a
	// real ext4 mount never trusts a (nonexistent) index over the
	// linear entries we just added.
	dir->i_flags &= ~EXT4_INDEX_FL;
	unsigned char raw[EXT4_INODE_SIZE];
	inode_read(dir_inum, raw);
	memcpy(raw, dir, sizeof(*dir));
	inode_write(dir_inum, raw);

	return 0;
}

// Removes the (name) entry from dir. Merges its space into the
// preceding entry in the same block when possible, else leaves a
// tombstone (inode 0) in place for future reuse by dir_insert().
static long dir_remove(struct ext4_inode *dir, const char *name, size_t name_len)
{
	u64 nblocks = (inode_size(dir) + EXT4_BLOCK_BYTES - 1) / EXT4_BLOCK_BYTES;

	for (u64 b = 0; b < nblocks; b++) {
		u64 phys;
		if (ext_map(dir, b, &phys) < 0)
			continue;

		b_nvs_read(blk_buf, phys, 1, EXT4_DRIVE);

		u32 pos = 0, prev = (u32)-1;
		while (pos < EXT4_BLOCK_BYTES) {
			struct ext4_dirent *de = (struct ext4_dirent *)(blk_buf + pos);
			if (de->rec_len == 0)
				break;

			if (de->inode != 0 && de->name_len == name_len &&
			    memcmp(de->name, name, name_len) == 0) {
				if (prev != (u32)-1) {
					struct ext4_dirent *pde = (struct ext4_dirent *)(blk_buf + prev);
					pde->rec_len += de->rec_len;
				} else {
					de->inode = 0;
				}
				b_nvs_write(blk_buf, phys, 1, EXT4_DRIVE);
				return 0;
			}

			prev = pos;
			pos += de->rec_len;
		}
	}

	return -ENOENT;
}

// -----------------------------------------------------------------------
// Path handling: strip one leading '/', reject anything with an
// embedded '/' or too long for a dirent's 8-bit name_len (see the
// flat-namespace limitation in the file-level comment).
// -----------------------------------------------------------------------

static const char *ext4_name(const char *path)
{
	if (path[0] == '/')
		path++;
	return path;
}

int ext4_is_fd(long fd)
{
	return fd >= EXT4_FD_BASE && fd < EXT4_FD_BASE + EXT4_MAX_OPEN
		&& files[fd - EXT4_FD_BASE].used;
}

long ext4_open(const char *path, int flags, int mode)
{
	(void)mode; // no permission model

	long rc = ext4_mount();
	if (rc < 0)
		return rc;

	const char *name = ext4_name(path);
	size_t len = strnlen(name, 256);
	if (len == 0 || len >= 256 || strchr(name, '/'))
		return -ENOENT;

	int slot = -1;
	for (int i = 0; i < EXT4_MAX_OPEN; i++) {
		if (!files[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return -EMFILE;

	unsigned char root_raw[EXT4_INODE_SIZE];
	inode_read(EXT4_ROOT_INO, root_raw);
	struct ext4_inode *root = (struct ext4_inode *)root_raw;

	u32 inum = dir_find(root, name, len);
	unsigned char inode_raw[EXT4_INODE_SIZE];

	if (inum == 0) {
		if (!(flags & O_CREAT))
			return -ENOENT;

		rc = alloc_inode(&inum);
		if (rc < 0)
			return rc;

		memset(inode_raw, 0, sizeof(inode_raw));
		struct ext4_inode *ni = (struct ext4_inode *)inode_raw;
		ni->i_mode = 0100644; // S_IFREG | 0644
		ni->i_links_count = 1;
		ni->i_flags = EXT4_EXTENTS_FL;
		struct ext4_extent_header *eh = (struct ext4_extent_header *)ni->i_block;
		eh->eh_magic = EXT4_EXTENT_MAGIC;
		eh->eh_max = EXT4_ROOT_MAX_ENTRIES;
		u64 now = b_system(WALLCLOCK, 0, 0);
		ni->i_atime = ni->i_ctime = ni->i_mtime = (u32)now;
		inode_write(inum, inode_raw);

		rc = dir_insert(root, EXT4_ROOT_INO, name, len, inum, EXT4_FT_REG_FILE);
		if (rc < 0) {
			free_inode(inum);
			return rc;
		}
	} else if ((flags & O_CREAT) && (flags & O_EXCL)) {
		return -EEXIST;
	} else {
		inode_read(inum, inode_raw);
		struct ext4_inode *ei = (struct ext4_inode *)inode_raw;
		if ((ei->i_mode & 0170000) != 0100000) // S_IFMT / S_IFREG
			return -EISDIR;

		if (flags & O_TRUNC) {
			extent_free_all(ei);
			inode_set_size(ei, 0);
			ei->i_blocks_lo = 0;
			inode_write(inum, inode_raw);
		}
	}

	struct ext4_file *f = &files[slot];
	f->used = 1;
	f->append = (flags & O_APPEND) != 0;
	f->inum = inum;

	inode_read(inum, inode_raw);
	u64 size = inode_size((struct ext4_inode *)inode_raw);
	f->pos = f->append ? size : 0;

	return EXT4_FD_BASE + slot;
}

// Reads/writes are done one block at a time through a scratch buffer
// so callers can use arbitrary byte offsets/lengths without the
// caller's buffer needing to be block-aligned.

static long ext4_pread(struct ext4_inode *ino, void *buf, size_t len, u64 off)
{
	u64 size = inode_size(ino);
	if (off >= size)
		return 0;
	if (off + len > size)
		len = size - off;

	unsigned char *dst = buf;
	size_t remaining = len;
	u64 pos = off;

	while (remaining) {
		u64 lblock = pos / EXT4_BLOCK_BYTES;
		u32 boff = (u32)(pos % EXT4_BLOCK_BYTES);
		size_t chunk = EXT4_BLOCK_BYTES - boff;
		if (chunk > remaining)
			chunk = remaining;

		u64 phys;
		if (ext_map(ino, lblock, &phys) == 0) {
			b_nvs_read(blk_buf, phys, 1, EXT4_DRIVE);
			memcpy(dst, blk_buf + boff, chunk);
		} else {
			memset(dst, 0, chunk); // sparse hole
		}

		dst += chunk;
		pos += chunk;
		remaining -= chunk;
	}

	return (long)len;
}

static long ext4_pwrite(u32 inum, struct ext4_inode *ino, const void *buf, size_t len, u64 off)
{
	const unsigned char *src = buf;
	size_t remaining = len;
	u64 pos = off;
	u64 size = inode_size(ino);

	while (remaining) {
		u64 lblock = pos / EXT4_BLOCK_BYTES;
		u32 boff = (u32)(pos % EXT4_BLOCK_BYTES);
		size_t chunk = EXT4_BLOCK_BYTES - boff;
		if (chunk > remaining)
			chunk = remaining;

		u64 phys;
		if (ext_map(ino, lblock, &phys) == 0) {
			// blk_buf ordering: ext_map's own use (depth-1
			// leaf lookup) is done by the time it returns.
			if (chunk < EXT4_BLOCK_BYTES)
				b_nvs_read(blk_buf, phys, 1, EXT4_DRIVE);
			memcpy(blk_buf + boff, src, chunk);
			b_nvs_write(blk_buf, phys, 1, EXT4_DRIVE);
		} else {
			long rc = alloc_block(&phys);
			if (rc < 0)
				return remaining == len ? rc : (long)(len - remaining);

			if (chunk < EXT4_BLOCK_BYTES)
				memset(blk_buf, 0, EXT4_BLOCK_BYTES);
			memcpy(blk_buf + boff, src, chunk);
			b_nvs_write(blk_buf, phys, 1, EXT4_DRIVE);

			// extent_insert() finishes its own blk_buf use
			// (if any) and returns before blk_buf is touched
			// again on the next loop iteration.
			rc = extent_insert(ino, lblock, phys);
			if (rc < 0) {
				free_block(phys);
				return remaining == len ? rc : (long)(len - remaining);
			}
			ino->i_blocks_lo += (u32)(EXT4_BLOCK_BYTES / 512);
		}

		src += chunk;
		pos += chunk;
		remaining -= chunk;
	}

	if (pos > size)
		inode_set_size(ino, pos);

	unsigned char raw[EXT4_INODE_SIZE];
	inode_read(inum, raw);
	memcpy(raw, ino, sizeof(*ino));
	inode_write(inum, raw);

	return (long)len;
}

long ext4_read(long fd, void *buf, size_t len)
{
	struct ext4_file *f = &files[fd - EXT4_FD_BASE];
	unsigned char raw[EXT4_INODE_SIZE];
	inode_read(f->inum, raw);

	long n = ext4_pread((struct ext4_inode *)raw, buf, len, f->pos);
	if (n > 0)
		f->pos += (u64)n;
	return n;
}

long ext4_write(long fd, const void *buf, size_t len)
{
	struct ext4_file *f = &files[fd - EXT4_FD_BASE];
	unsigned char raw[EXT4_INODE_SIZE];
	inode_read(f->inum, raw);
	struct ext4_inode *ino = (struct ext4_inode *)raw;

	// O_APPEND: every write goes to the current end of file,
	// regardless of the fd's seek position -- otherwise a write
	// following an lseek() would land at the wrong offset.
	if (f->append)
		f->pos = inode_size(ino);

	long n = ext4_pwrite(f->inum, ino, buf, len, f->pos);
	if (n > 0)
		f->pos += (u64)n;
	return n;
}

long ext4_close(long fd)
{
	struct ext4_file *f = &files[fd - EXT4_FD_BASE];
	f->used = 0;
	return 0;
}

long ext4_lseek(long fd, long offset, int whence)
{
	struct ext4_file *f = &files[fd - EXT4_FD_BASE];
	unsigned char raw[EXT4_INODE_SIZE];
	long base;

	switch (whence) {
	case 0: base = 0; break;              /* SEEK_SET */
	case 1: base = (long)f->pos; break;   /* SEEK_CUR */
	case 2:
		inode_read(f->inum, raw);
		base = (long)inode_size((struct ext4_inode *)raw);
		break;                         /* SEEK_END */
	default: return -EINVAL;
	}

	long np = base + offset;
	if (np < 0)
		return -EINVAL;

	f->pos = (u64)np;
	return np;
}

long ext4_fstat_fd(long fd, void *stbuf)
{
	struct ext4_file *f = &files[fd - EXT4_FD_BASE];
	unsigned char raw[EXT4_INODE_SIZE];
	inode_read(f->inum, raw);
	struct ext4_inode *ino = (struct ext4_inode *)raw;
	struct stat *st = stbuf;

	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFREG | 0644;
	st->st_size = (off_t)inode_size(ino);
	st->st_blksize = EXT4_BLOCK_BYTES;
	st->st_blocks = ino->i_blocks_lo;

	return 0;
}

long ext4_fstatat(const char *path, void *kstbuf)
{
	long rc = ext4_mount();
	if (rc < 0)
		return rc;

	const char *name = ext4_name(path);
	size_t len = strnlen(name, 256);
	if (len == 0 || len >= 256 || strchr(name, '/'))
		return -ENOENT;

	unsigned char root_raw[EXT4_INODE_SIZE];
	inode_read(EXT4_ROOT_INO, root_raw);
	u32 inum = dir_find((struct ext4_inode *)root_raw, name, len);
	if (inum == 0)
		return -ENOENT;

	unsigned char raw[EXT4_INODE_SIZE];
	inode_read(inum, raw);
	struct ext4_inode *ino = (struct ext4_inode *)raw;

	struct linux_kstat *st = kstbuf;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFREG | 0644;
	st->st_nlink = 1;
	st->st_ino = inum;
	st->st_size = (long)inode_size(ino);
	st->st_blksize = EXT4_BLOCK_BYTES;
	st->st_blocks = (long)ino->i_blocks_lo;
	st->st_atime_sec = (long)ino->i_atime;
	st->st_mtime_sec = (long)ino->i_mtime;
	st->st_ctime_sec = (long)ino->i_ctime;

	return 0;
}

long ext4_unlink(const char *path)
{
	long rc = ext4_mount();
	if (rc < 0)
		return rc;

	const char *name = ext4_name(path);
	size_t len = strnlen(name, 256);
	if (len == 0 || len >= 256 || strchr(name, '/'))
		return -ENOENT;

	unsigned char root_raw[EXT4_INODE_SIZE];
	inode_read(EXT4_ROOT_INO, root_raw);
	struct ext4_inode *root = (struct ext4_inode *)root_raw;

	u32 inum = dir_find(root, name, len);
	if (inum == 0)
		return -ENOENT;

	unsigned char raw[EXT4_INODE_SIZE];
	inode_read(inum, raw);
	struct ext4_inode *ino = (struct ext4_inode *)raw;

	extent_free_all(ino);
	ino->i_links_count = 0;
	ino->i_dtime = (u32)b_system(WALLCLOCK, 0, 0);
	inode_write(inum, raw);
	free_inode(inum);

	return dir_remove(root, name, len);
}

// =============================================================================
// EOF
