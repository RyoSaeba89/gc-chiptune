/* ------------------------------------------------------------------------
 * GC-Chiptune: resume after power-off.
 *
 * Without it, you start again at track 1 of 4937 on every power-on. The file is
 * tiny and in TEXT, one `key=value` per line: it reads on a PC, it can be fixed
 * by hand, and an unknown or missing field simply falls back to its default
 * instead of making the file unreadable.
 *
 * WE REMEMBER THE TRACK'S PATH, NOT ITS INDEX. An index would name the wrong
 * file the moment a track is added to or removed from the card -- and the pack
 * has just been culled of 467 files. A path either stays correct or disappears
 * cleanly.
 *
 * The shuffle seed is saved too: the permutation is reproducible from it
 * (verified by tools/lib_test), so the shuffle bag resumes exactly where it was
 * instead of starting over.
 *
 * WRITING: at most once per track change, and never while the audio ring is
 * short -- an SD write blocks the main loop long enough to starve it. We write
 * to a temporary file and rename -- a power cut mid-write then leaves the OLD
 * state intact rather than a half-written file.
 * ------------------------------------------------------------------------ */

#ifndef GC_STATE_H_
#define GC_STATE_H_

#include "library.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_MAX_PATH 512

typedef struct {
    char       track[ST_MAX_PATH];   /* full path, "" if none          */
    char       scope[LIB_MAX_PREFIX];/* scope prefix                   */
    int        recursive;
    lib_order  order;
    lib_repeat repeat;
    unsigned   seed;
    unsigned   volume;               /* 0 to 100                       */
    int        valid;
} player_state;

/* Fills `st` with the defaults: the whole pack, sequential, repeat list. */
void st_defaults(player_state *st);

/* Reads the file. Returns 1 if a usable state was found, 0 otherwise (in which
 * case `st` carries the defaults). */
int  st_load(player_state *st, const char *file);

/* Writes the file. Returns 0 on success. Best effort: a failure must never
 * interrupt playback. */
int  st_save(const player_state *st, const char *file);

#ifdef __cplusplus
}
#endif

#endif /* GC_STATE_H_ */
