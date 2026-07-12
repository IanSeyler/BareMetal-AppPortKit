#ifndef _EXT4_H
#define _EXT4_H

#include <stddef.h>

// True if fd refers to an open ext4-backed file (as opposed to a std
// fd 0-2, which posix_shim.c handles itself).
int ext4_is_fd(long fd);

long ext4_open(const char *path, int flags, int mode);
long ext4_read(long fd, void *buf, size_t len);
long ext4_write(long fd, const void *buf, size_t len);
long ext4_close(long fd);
long ext4_lseek(long fd, long offset, int whence);
long ext4_fstat_fd(long fd, void *stbuf);
long ext4_unlink(const char *path);

// Fills a Linux struct kstat (see ext4.c) for path -- used to back
// the fstatat() syscall (aliased from SYS_newfstatat), which is what
// x86_64 musl's stat()/lstat()/fstatat() actually issue.
long ext4_fstatat(const char *path, void *kstbuf);

#endif
