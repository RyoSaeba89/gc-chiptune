/* ------------------------------------------------------------------------
 * GC-Chiptune: playback scope, order and modes.
 *
 * This module touches NEITHER the SD card, NOR the screen, NOR the audio. All
 * it does is decide which track comes after which. That is what makes it
 * testable on a PC (tools/lib_test.c), and it is deliberate: shuffle-bag logic
 * is exactly the kind of thing you do not want to debug with a gamepad.
 *
 * THE IDEA THAT AVOIDS RE-READING THE CARD. playlist.c stores paths SORTED by
 * full path. The set of strings sharing a prefix is always contiguous in
 * lexicographic order, so any subtree is an INTERVAL of the array, findable by
 * bisection:
 *
 *     [0 .............................................. 4937]
 *            |<--- "tPORt/" --->|
 *            lo                 hi
 *
 * Changing scope -- going from "the whole pack" to "this artist" -- therefore
 * costs two binary searches. No SD access, no rescan.
 *
 * TWO ORTHOGONAL SETTINGS, not six modes:
 *
 *   order    SEQUENTIAL | SHUFFLE
 *   repeat   LIST | TRACK | ONCE
 *
 * Shuffle is a BAG, not a draw with replacement: order[] is a permutation of
 * the scope, so every track comes out exactly once before any repeats. Free and
 * important side effect -- "previous" becomes correct again, since we step back
 * through the same permutation.
 * ------------------------------------------------------------------------ */

#ifndef GC_LIBRARY_H_
#define GC_LIBRARY_H_

#include "playlist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length of a scope prefix. Same bound as playlist.c's paths, which
 * comes from PATH_MAX on the devoptab side. */
#define LIB_MAX_PREFIX 512

typedef enum { LIB_ORDER_SEQ = 0, LIB_ORDER_RANDOM } lib_order;

typedef enum {
    LIB_REPEAT_LIST = 0,   /* end of list -> start over (reshuffling if random) */
    LIB_REPEAT_TRACK,      /* the same track on a loop                          */
    LIB_REPEAT_OFF         /* end of list -> stop                               */
} lib_repeat;

typedef struct {
    const playlist *pl;

    char  prefix[LIB_MAX_PREFIX];  /* base folder, "" = the whole list       */
    int   recursive;               /* 0 = direct files only                  */

    int  *order;                   /* playlist indices, a permutation if random */
    int   n;                       /* tracks in scope                        */
    int   cap;
    int   pos;                     /* current position in order[], -1 = none */

    lib_order  order_mode;
    lib_repeat repeat;
    unsigned   seed;               /* generator state, see lib_set_seed()    */
} library;

void lib_init(library *lb);

/* Drops order[] and the position. KEEPS the modes (order, repeat, seed):
 * rebuilding the index is not a reason to lose the user's settings. */
void lib_free(library *lb);

/* Shuffle seed. Set it from a real entropy source (gettime() on the first
 * button press): without that, the same "bag" on every power-on. */
void lib_set_seed(library *lb, unsigned seed);

/* Defines the playback scope.
 *
 * prefix    : base folder. "" or NULL = the whole playlist. Must end with '/'
 *             if it names a folder, otherwise "tPORt" and "tPORtable" are not
 *             distinguished.
 * recursive : 1 = the folder AND everything under it (playing a whole artist,
 *             or the whole pack from the root);
 *             0 = only the files sitting directly in it.
 *
 * Rebuilds order[] and resets the position to the start. Returns the number of
 * tracks in scope, or -1 on allocation failure. */
int lib_set_scope(library *lb, const playlist *pl,
                  const char *prefix, int recursive);

/* Reshuffles. No effect in sequential mode. The CURRENT track is moved to the
 * head of the new permutation, so we do not cut ourselves off. */
void lib_reshuffle(library *lb);

void lib_set_order(library *lb, lib_order o);
void lib_set_repeat(library *lb, lib_repeat r);

/* Playlist index of the current track, -1 if the scope is empty. */
int  lib_current(const library *lb);
int  lib_count(const library *lb);
/* Human position in the scope, 1 to lib_count(). 0 if empty. */
int  lib_position(const library *lb);

/* Moves forward or back one track, applying the repeat mode.
 *
 * Returns 1 if there is a track to play, 0 if playback should stop
 * (LIB_REPEAT_OFF reaching the end). LIB_REPEAT_TRACK always returns 1 without
 * moving: it is up to the caller to restart the track. */
int  lib_next(library *lb);
int  lib_prev(library *lb);

/* Relative jump in the current order, clamped at the ends (it does not wrap: a
 * +10 jump at the end of the list must stop there, not go back to the start).
 * Ignores the repeat mode -- it is a navigation gesture, not the natural
 * continuation of playback. */
void lib_jump(library *lb, int delta);

/* Goes to playlist index `track`, if it is in scope. Returns 1 if found. Used
 * by resume-after-power-off. */
int  lib_goto_track(library *lb, int track);

/* Start of the next / previous folder in the SEQUENTIAL order of the scope,
 * whatever the current order mode. Returns 1 if we moved.
 *
 * At 4937 tracks it is the only jump with any musical meaning: you are not
 * looking for "ten further on", you are looking for the next artist. */
int  lib_next_folder(library *lb);
int  lib_prev_folder(library *lb);

#ifdef __cplusplus
}
#endif

#endif /* GC_LIBRARY_H_ */
