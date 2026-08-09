/* ------------------------------------------------------------------------
 * GC-Chiptune: recursive walk of a folder and construction of the playlist.
 *
 * Paths are stored in a single concatenated buffer rather than an array of
 * pointers: the reference corpus has 5533 files with long names (~70 bytes on
 * average), i.e. ~400 kB of strings. One char* per entry would add 22 kB of
 * pointers and, more to the point, that many malloc headers, on a console with
 * only 24 MB. So we store OFFSETS, which also leaves us free to realloc() the
 * buffer.
 * ------------------------------------------------------------------------ */

#ifndef GC_PLAYLIST_H_
#define GC_PLAYLIST_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard bounds. Once exceeded, the walk stops cleanly and marks `truncated`: an
 * incomplete list beats running out of memory halfway through the scan.
 *
 * Redefinable at compile time: that is how tools/pl_scan_test exercises the
 * "truncated list" path, which no real corpus reaches. */
#ifndef PL_MAX_TRACKS
#define PL_MAX_TRACKS   8192
#endif
#ifndef PL_MAX_DEPTH
#define PL_MAX_DEPTH      12
#endif

/* Maximum size of an accepted track. The largest module in the corpus is
 * 1.6 MB; beyond that we are looking at something else (a soundfont, an .ogg)
 * and loading it whole might not fit. */
#define PL_MAX_TRACK_BYTES (6u * 1024u * 1024u)

typedef struct {
    char     *pool;
    size_t    pool_len, pool_cap;
    unsigned *offsets;
    int       count, cap;

    int       dirs;       /* folders visited                          */
    int       skipped;    /* files rejected (extension or size)       */
    int       truncated;  /* a bound was reached                      */
} playlist;

void pl_init(playlist *pl);
void pl_free(playlist *pl);

/* Walks `root` depth-first and adds every file whose extension one of the
 * backends knows. Then sorts by path.
 * Returns the number of tracks found, or -1 if `root` is unreadable. */
int pl_scan(playlist *pl, const char *root);

int         pl_count(const playlist *pl);
/* NULL for an out-of-range index. Callers do check -- a re-index can leave a
 * stale index behind for one frame. */
const char *pl_path(const playlist *pl, int i);

/* File name alone (after the last '/'), for display. */
const char *pl_basename(const playlist *pl, int i);

/* True if the extension is playable by one of the backends. Exposed so the
 * caller can filter other sources (a soundfont, for instance). */
int pl_is_playable(const char *name);

/* True if the extension announces a MIDI (mid / midi / rmi), i.e. a track that
 * will need the soundfont.
 *
 * THE EXTENSION, AND NOT gcc_detect(), BECAUSE OF WHEN THE ANSWER IS NEEDED.
 * The player has to decide whether to release the 11.5 MB of soundfont BEFORE
 * reading the file -- that is the whole point, the module needs the room to be
 * read at all -- and detection wants the bytes. A wrong guess costs nothing:
 * gcc_detect() still has the last word on content, and the soundfont is loaded
 * on demand right after. */
int pl_is_midi(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* GC_PLAYLIST_H_ */
