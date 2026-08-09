/* ------------------------------------------------------------------------
 * GC-Chiptune: storage access (SD card). See storage.h.
 * ------------------------------------------------------------------------ */

#include <fat.h>
#include <sdcard/gcsd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "storage.h"

#define GCS_VOLUME "sd"

static int         g_mounted = 0;
static const char *g_source  = "";

typedef struct {
    const char      *label;
    DISC_INTERFACE *(*get)(void);
} candidate;

/* Order of attempts: slot A first, it is the only carrier Dolphin emulates
 * (Options > Configuration > GameCube > slot A = "SD Gecko"), so the only one
 * testable there. The other two are for real hardware. */
static const candidate g_candidates[] = {
    { "SD Gecko slot A", get_io_gcsda },
    { "SD Gecko slot B", get_io_gcsdb },
    { "SD2SP2 (port 2)", get_io_gcsd2 },
};
#define NCANDIDATES ((int)(sizeof g_candidates / sizeof g_candidates[0]))

int gcs_mount(void)
{
    int i;

    if (g_mounted) return 0;

    for (i = 0; i < NCANDIDATES; i++) {
        DISC_INTERFACE *io = g_candidates[i].get();
        if (!io) continue;

        /* startup() before isInserted(): on an empty slot, libsdcard's
         * initialisation fails immediately, whereas isInserted() on an
         * interface that was never started has no reliable answer. */
        if (!io->startup(io)) continue;
        if (!io->isInserted(io)) { io->shutdown(io); continue; }

        /* fatMountSimple starts the interface itself (unlike fatMount); the
         * startup above is only a probe, and it is idempotent. */
        if (fatMountSimple(GCS_VOLUME, io)) {
            g_mounted = 1;
            g_source  = g_candidates[i].label;
            return 0;
        }

        io->shutdown(io);
    }

    return -1;
}

void gcs_unmount(void)
{
    if (!g_mounted) return;
    fatUnmount(GCS_VOLUME);
    g_mounted = 0;
    g_source  = "";
}

const char *gcs_device(void)      { return g_mounted ? GCS_VOLUME ":" : ""; }
const char *gcs_source_name(void) { return g_source; }

void *gcs_read_file(const char *path, size_t *out_len, size_t pad)
{
    FILE  *f;
    long   size;
    void  *buf;

    if (out_len) *out_len = 0;
    if (!path) return NULL;

    f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    size = ftell(f);
    if (size <= 0) { fclose(f); return NULL; }
    rewind(f);

    /* memalign rather than malloc: the block is handed to the backends as-is,
     * and 32-byte alignment is a Gekko cache line. */
    buf = memalign(32, (size_t)size + pad);
    if (!buf) { fclose(f); return NULL; }

    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    if (pad) memset((char *)buf + size, 0, pad);

    if (out_len) *out_len = (size_t)size;
    return buf;
}
