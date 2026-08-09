/* ------------------------------------------------------------------------
 * GC-Chiptune: storage access (SD card) on GameCube.
 *
 * Mounts an SD card through libfat and provides a way to read a whole file into
 * memory, the only form of I/O the backends need (backend.h: the backends do no
 * I/O themselves).
 *
 * Three possible carriers, tried in order:
 *   1. SD Gecko in memory card slot A  (__io_gcsda) -- the one Dolphin emulates
 *   2. SD Gecko in memory card slot B  (__io_gcsdb)
 *   3. SD2SP2 on serial port 2         (__io_gcsd2)
 *
 * The volume is mounted as "sd", so paths look like "sd:/...".
 * ------------------------------------------------------------------------ */

#ifndef GC_STORAGE_H_
#define GC_STORAGE_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mounts the first SD card found. Returns 0 on success.
 * No effect (and success) if a volume is already mounted. */
int gcs_mount(void);

void gcs_unmount(void);

/* Prefix to use in paths, "sd:" -- or "" if nothing is mounted. */
const char *gcs_device(void);

/* Readable label of the mounted carrier ("slot A", "SD2SP2"...), "" otherwise. */
const char *gcs_source_name(void);

/* Reads a whole file.
 *
 * Allocates `size + pad` bytes aligned to 32 (Gekko cache line, and what the
 * audio DMA requires) and zeroes the last `pad` bytes. That satisfies a
 * trailing margin with no special case at the caller's level.
 *
 * `*out_len` receives the REAL file size, without the padding: that is what
 * gcc_open() expects.
 *
 * Returns NULL on failure. Release with free(). */
void *gcs_read_file(const char *path, size_t *out_len, size_t pad);

#ifdef __cplusplus
}
#endif

#endif /* GC_STORAGE_H_ */
