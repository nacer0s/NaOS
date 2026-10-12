/* NaOS updater phase 1: manifest check + verified downloads.
 * Manifest (key=value text): version=, note=, then per file
 *   fN_name= (local FAT path), fN_url= (server path), fN_size=, fN_sha256=
 * Downloads stream in 2KB chunks straight to FAT (fat_append), hashed
 * on the fly. Nothing is auto-applied in phase 1: the human reviews. */
#ifndef NAOS_UPDATE_H
#define NAOS_UPDATE_H

#include <stdint.h>

/* Fetch manifest from host:port + path, compare with our VERSION. */
int update_check(const char *host, uint16_t port, const char *mpath);
/* Download remotepath -> local FAT path, optional hex sha256 ("" skips). */
int update_get(const char *host, uint16_t port, const char *remotepath,
               const char *localpath, const char *sha_hex);

#endif /* NAOS_UPDATE_H */
