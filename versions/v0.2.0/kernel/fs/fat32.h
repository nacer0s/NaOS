/* NaOS FAT32 driver: read/write, subdirs, long filenames. Own BPB parsing. */
#ifndef NAOS_FAT32_H
#define NAOS_FAT32_H

#include <stdint.h>

int fat_init(void);              /* 0 ok: parses BPB from sector 0 */
int fat_is_ready(void);
/* List directory: path "" or "/" = root, else "EFI", "NaOS", ... Prints via kprintf. */
int fat_ls(const char *path);
/* 1 if path exists and is a directory, else 0. */
int fat_is_dir(const char *path);
/* Read file into buf (cap bytes). Returns bytes read, <0 on error. */
int fat_read(const char *path, void *buf, uint32_t cap, uint32_t *size_out);
/* Create-or-overwrite a file. Returns 0 ok, <0 on error. */
int fat_write(const char *path, const void *buf, uint32_t len);
/* Append bytes (create when missing). One consistent update. 0 ok. */
int fat_append(const char *path, const void *buf, uint32_t len);
/* Same, by parent cluster + 8.3 leaf (for file-descriptor flushes).
 * up_full is the uppercased full leaf for LFN-aware matching (NULL ok,
 * legacy exact-8.3 match only). */
int fat_write_parts(uint32_t parent, const uint8_t leaf83[11],
                    const char *up_full, const void *buf, uint32_t len);
/* Open a file for reading: cluster + size. Files only (no dirs). */
int fat_fopen_r(const char *path, uint32_t *clus_out, uint32_t *size_out);
/* Resolve a parent dir + 8.3 leaf for creation (create-ok check).
 * up_out (char[256] or NULL) also gets the uppercased full leaf. */
int fat_parent_leaf(const char *path, uint32_t *parent_out,
                    uint8_t leaf83_out[11], char *up_out);
/* Positional read of an open file. Returns bytes read, <0 on error. */
int fat_read_at(uint32_t clus, uint32_t size, uint32_t pos, void *buf,
                uint32_t len);
/* Create a directory. Returns 0 ok, <0 on error. */
int fat_mkdir(const char *path);
/* Delete a file or an empty directory. Returns 0 ok, <0 on error. */
int fat_delete(const char *path);
/* Rename within one folder (files and folders, even full ones).
 * Across folders: files are copied+deleted, folders are refused (-8).
 * Returns 0 ok, <0 on error. */
int fat_rename(const char *oldpath, const char *newpath);
/* Copy a file or a whole folder tree. Returns bytes copied, <0 on error. */
int fat_copy(const char *src, const char *dst);
/* Walk a directory tree pre-order, cb(full path, is_dir, ctx) per entry
 * (. and .. skipped, depth cap 8). The callback must not call back into
 * fat_* (it shares the scan state): collect paths, act afterwards. */
typedef void (*fat_walk_cb)(const char *path, int is_dir, void *ctx);
int fat_walk(const char *path, fat_walk_cb cb, void *ctx);

#endif /* NAOS_FAT32_H */
