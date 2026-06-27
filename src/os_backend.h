/*
 * os_backend.h - the two filesystem concerns that cannot be portable. os.c calls
 * these; a platform that needs to route reads (Android = AAssetManager) provides
 * real implementations, everyone else gets the no-op defaults in os.c.
 */
#ifndef OS_BACKEND_H
#define OS_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* Read a shipped asset. Returns a malloc'd, NUL-terminated buffer (size excludes
   the terminator) or NULL when this platform does not route assets, in which case
   os.c falls back to reading a real file under the asset root. */
uint8_t *os_backend_asset_read(const char *path, size_t *out_size);

/* 1 exists, 0 missing, -1 not routed (os.c then checks the filesystem). */
int os_backend_asset_exists(const char *path);

/* Writable per-app directory, or NULL when not provided (os.c uses dir_app). */
const char *os_backend_data_dir(void);

#endif /* OS_BACKEND_H */
