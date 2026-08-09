/* ------------------------------------------------------------------------
 * GC-Chiptune: index cache on the SD card.
 *
 * TWO PROBLEMS, ONE FILE.
 *
 * 1. THE START-UP WALK. Listing 4937 files on an SD Gecko costs one EXI round
 *    trip per directory entry. The cache replaces that with a single sequential
 *    read.
 *
 * 2. THE DURATIONS. gcc_duration_ms() gives them for free -- but only after the
 *    file has been OPENED, and libxmp unrolls the whole module to find its
 *    loop. Computing them ahead of time for 4937 tracks would mean reading half
 *    a gigabyte of card at start-up: out of the question.
 *
 *    So we fill them in AS WE GO: every track played hands over its duration
 *    and we note it. The cache completes itself through use, and the browser
 *    shows what is already known.
 *
 * HENCE THE FILE LAYOUT. The durations are a fixed-size integer array, placed
 * AFTER the variable-length path pool:
 *
 *     [ header 64 B ][ paths, pool_len bytes ][ durations, count * 4 ]
 *                                             ^
 *                                             computable offset
 *
 * Noting a duration therefore does not rewrite the file: it is an fseek() and
 * FOUR BYTES, once per track, i.e. once every two minutes. The Gekko cost is
 * nil and the write happens when the track opens, never while the audio ring is
 * running.
 *
 * VALIDITY. We do not check that the card has not changed -- that would mean
 * redoing the walk we are trying to avoid. The cache is taken on trust; the
 * user rebuilds it on demand from the browser, like the "rescan" of any player.
 * The header does carry the root and the sizes, though, so a cache for a
 * DIFFERENT folder, or a truncated one, is rejected.
 * ------------------------------------------------------------------------ */

#ifndef GC_INDEX_H_
#define GC_INDEX_H_

#include "playlist.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IDX_MAX_PATH 128

typedef struct {
    char          file[IDX_MAX_PATH];  /* cache path on the card          */
    long          dur_offset;          /* where the duration array starts */
    int           count;
    unsigned long *dur;                /* count entries, 0 = not known yet */
    int           open;
} track_index;

void idx_init(track_index *ix);
void idx_free(track_index *ix);

/* Loads the cache into `pl` and prepares the duration table.
 *
 * `root` must be the expected root: a cache written for another folder is
 * refused rather than made to play paths that no longer exist.
 *
 * Returns the number of tracks, or -1 if the cache is absent, unreadable,
 * truncated, or written for another root. In that case `pl` is left empty and
 * the caller must run a pl_scan(). */
int idx_load(track_index *ix, playlist *pl, const char *file, const char *root);

/* Writes the cache from a freshly scanned playlist. Already known durations are
 * kept if `ix` carried any; otherwise everything starts at zero. Returns 0 on
 * success. */
int idx_save(track_index *ix, const playlist *pl, const char *file,
             const char *root);

/* Known duration of track `i`, in milliseconds. 0 = not played yet. */
unsigned long idx_duration(const track_index *ix, int i);

/* Notes the duration of track `i` and pushes it to the card -- fseek + 4 bytes.
 * No effect if the value is already that one, so we do not write on every
 * repeat of the same track. */
void idx_set_duration(track_index *ix, int i, unsigned long ms);

/* Number of durations already known, for display ("1247/4937 measured"). */
int idx_known(const track_index *ix);

#ifdef __cplusplus
}
#endif

#endif /* GC_INDEX_H_ */
