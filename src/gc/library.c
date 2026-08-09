/* ------------------------------------------------------------------------
 * GC-Chiptune: playback scope, order and modes. See library.h.
 * ------------------------------------------------------------------------ */

#include "library.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------- tools */

/* 32-bit linear congruential generator (Numerical Recipes). We deliberately do
 * NOT use the libc rand(): its quality and period vary between
 * implementations, and above all a reproducible shuffle from a known seed is
 * indispensable if tools/lib_test is to verify the permutation. */
static unsigned lib_rand(library *lb)
{
    lb->seed = lb->seed * 1664525u + 1013904223u;
    return lb->seed >> 8;          /* the low bits are poor */
}

/* First index whose path is >= prefix. Bisection: playlist.c sorted by full
 * path. */
static int lower_bound(const playlist *pl, const char *prefix, size_t plen)
{
    int lo = 0, hi = pl_count(pl);
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (strncmp(pl_path(pl, mid), prefix, plen) < 0) lo = mid + 1;
        else                                             hi = mid;
    }
    return lo;
}

/* First index whose path no longer starts with prefix. */
static int upper_bound(const playlist *pl, const char *prefix, size_t plen)
{
    int lo = 0, hi = pl_count(pl);
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (strncmp(pl_path(pl, mid), prefix, plen) <= 0) lo = mid + 1;
        else                                             hi = mid;
    }
    return lo;
}

/* True if the path, once the prefix is removed, contains no more '/' -- i.e. if
 * the file sits IN the folder and not in a sub-folder. */
static int is_direct_child(const char *path, size_t plen)
{
    return strchr(path + plen, '/') == NULL;
}

/* ------------------------------------------------------------------- life cycle */

void lib_init(library *lb)
{
    memset(lb, 0, sizeof *lb);
    lb->pos  = -1;
    lb->seed = 1u;
}

void lib_free(library *lb)
{
    free(lb->order);
    lb->order = NULL;
    lb->n = lb->cap = 0;
    lb->pos = -1;
    /* pl is left dangling on purpose: every accessor guards on n == 0 or
     * pos < 0, and the caller is about to hand us a new playlist. Zeroing the
     * modes here would silently reset the user's settings on a re-index. */
    lb->pl = NULL;
}

void lib_set_seed(library *lb, unsigned seed)
{
    /* Zero is a fixed point for many generators; not for this one, but there is
     * no reason to depend on that detail. */
    lb->seed = seed ? seed : 1u;
}

/* ---------------------------------------------------------------- scope */

int lib_set_scope(library *lb, const playlist *pl,
                  const char *prefix, int recursive)
{
    char   own[LIB_MAX_PREFIX];
    size_t plen;
    int    lo, hi, i, n = 0;

    if (!lb || !pl) return -1;

    if (!prefix) prefix = "";
    plen = strlen(prefix);
    if (plen >= LIB_MAX_PREFIX) return -1;

    /* COPY FIRST. lib_set_order() calls us with lb->prefix as the argument, so
     * without this the memcpy below would be a self-copy -- undefined
     * behaviour, harmless in practice today and a trap waiting for a
     * memcpy that runs backwards. */
    memcpy(own, prefix, plen + 1);
    memcpy(lb->prefix, own, plen + 1);
    lb->recursive = recursive;
    lb->pl        = pl;

    if (plen == 0) {
        lo = 0;
        hi = pl_count(pl);
    } else {
        lo = lower_bound(pl, own, plen);
        hi = upper_bound(pl, own, plen);
    }

    if (hi - lo > lb->cap) {
        int *p = (int *)realloc(lb->order, (size_t)(hi - lo) * sizeof(int));
        if (!p) return -1;
        lb->order = p;
        lb->cap   = hi - lo;
    }

    for (i = lo; i < hi; i++) {
        if (!recursive && !is_direct_child(pl_path(pl, i), plen)) continue;
        lb->order[n++] = i;
    }

    lb->n   = n;
    lb->pos = n ? 0 : -1;

    /* order[] leaves here in sequential order. In shuffle mode it has to be
     * shuffled right away, or the first track of the new scope would always be
     * the same one. */
    if (lb->order_mode == LIB_ORDER_RANDOM) lib_reshuffle(lb);

    return n;
}

void lib_reshuffle(library *lb)
{
    int i, keep = -1;

    if (!lb || lb->n < 2) return;

    /* Preserve the current track: it goes back to the head of the new
     * permutation. Without that, reshuffling would cut playback off to jump
     * somewhere else -- which is never what anyone wants, neither at the end of
     * a bag nor when the user switches to shuffle. */
    if (lb->pos >= 0 && lb->pos < lb->n) keep = lb->order[lb->pos];

    /* Fisher-Yates, last to first: every permutation is equally likely, and
     * every track appears EXACTLY once. That is the difference from drawing at
     * random for each track, which would give the same song twice in a row one
     * time in n. */
    for (i = lb->n - 1; i > 0; i--) {
        int j = (int)(lib_rand(lb) % (unsigned)(i + 1));
        int t = lb->order[i];
        lb->order[i] = lb->order[j];
        lb->order[j] = t;
    }

    if (keep >= 0) {
        for (i = 0; i < lb->n; i++) {
            if (lb->order[i] == keep) {
                lb->order[i] = lb->order[0];
                lb->order[0] = keep;
                break;
            }
        }
        lb->pos = 0;
    } else {
        lb->pos = lb->n ? 0 : -1;
    }
}

void lib_set_order(library *lb, lib_order o)
{
    if (!lb || lb->order_mode == o) return;
    lb->order_mode = o;

    if (o == LIB_ORDER_RANDOM) {
        lib_reshuffle(lb);
    } else {
        /* Back to sequential: order[] becomes the playlist order again. We put
         * ourselves back on the current track so as not to jump elsewhere. */
        int cur = lib_current(lb), i;
        if (!lb->pl) return;
        lib_set_scope(lb, lb->pl, lb->prefix, lb->recursive);
        if (cur >= 0) {
            for (i = 0; i < lb->n; i++)
                if (lb->order[i] == cur) { lb->pos = i; break; }
        }
    }
}

void lib_set_repeat(library *lb, lib_repeat r) { if (lb) lb->repeat = r; }

/* --------------------------------------------------------------- queries */

int lib_count(const library *lb) { return lb ? lb->n : 0; }

int lib_current(const library *lb)
{
    if (!lb || !lb->order || lb->pos < 0 || lb->pos >= lb->n) return -1;
    return lb->order[lb->pos];
}

int lib_position(const library *lb)
{
    if (!lb || lb->pos < 0 || lb->n == 0) return 0;
    return lb->pos + 1;
}

/* -------------------------------------------------------------- movement */

int lib_next(library *lb)
{
    if (!lb || lb->n == 0) return 0;

    /* LIB_REPEAT_TRACK does not move: it is the caller that reopens the same
     * file. Saying so here rather than leaving every caller to remember it. */
    if (lb->repeat == LIB_REPEAT_TRACK) return 1;

    if (lb->pos + 1 < lb->n) { lb->pos++; return 1; }

    /* End of list. */
    switch (lb->repeat) {
        case LIB_REPEAT_LIST:
            /* In shuffle mode the bag is empty: reshuffle BEFORE starting over,
             * or the second pass would replay the first one's order. */
            if (lb->order_mode == LIB_ORDER_RANDOM) {
                lb->pos = -1;          /* nothing to preserve, the bag is done */
                lib_reshuffle(lb);
            }
            lb->pos = 0;
            return 1;
        case LIB_REPEAT_OFF:
        default:
            return 0;
    }
}

int lib_prev(library *lb)
{
    if (!lb || lb->n == 0) return 0;
    if (lb->repeat == LIB_REPEAT_TRACK) return 1;

    if (lb->pos > 0) { lb->pos--; return 1; }

    if (lb->repeat == LIB_REPEAT_LIST) { lb->pos = lb->n - 1; return 1; }
    return 0;
}

void lib_jump(library *lb, int delta)
{
    int p;

    if (!lb || lb->n == 0) return;
    p = lb->pos + delta;
    if (p < 0)       p = 0;
    if (p >= lb->n)  p = lb->n - 1;
    lb->pos = p;
}

int lib_goto_track(library *lb, int track)
{
    int i;

    if (!lb || !lb->order) return 0;
    for (i = 0; i < lb->n; i++)
        if (lb->order[i] == track) { lb->pos = i; return 1; }
    return 0;
}

/* ------------------------------------------------------------ by folder */

/* Length of the path up to and including the last '/'. Two tracks in the same
 * folder share exactly that prefix. */
static size_t dir_len(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? (size_t)(slash - path) + 1 : 0;
}

/* True if both tracks are in the same folder. */
static int same_folder(const library *lb, int a, int b)
{
    const char *pa = pl_path(lb->pl, a);
    const char *pb = pl_path(lb->pl, b);
    size_t      la;

    if (!pa || !pb) return 0;
    la = dir_len(pa);
    return la == dir_len(pb) && strncmp(pa, pb, la) == 0;
}

/* Folder jumps reason about PLAYLIST order, never about order[]: in shuffle
 * mode the tracks of one folder are scattered, and "next folder" would mean
 * nothing there. So we look for the target among the scope's playlist indices,
 * then reposition onto it in the current order.
 *
 * Two passes, each O(n), and NEITHER touches lb->pos: searching must not move
 * playback. */
static int move_folder(library *lb, int dir)
{
    int cur, i, best = -1, first;

    if (!lb || lb->n == 0 || !lb->pl) return 0;
    cur = lib_current(lb);
    if (cur < 0) return 0;

    /* Pass 1: the track closest to `cur`, in the wanted direction, that is in a
     * DIFFERENT folder. */
    for (i = 0; i < lb->n; i++) {
        int t = lb->order[i];
        if (dir > 0 ? (t <= cur) : (t >= cur)) continue;
        if (same_folder(lb, t, cur)) continue;
        if (best < 0 || (dir > 0 ? t < best : t > best)) best = t;
    }
    if (best < 0) return 0;

    /* Pass 2: we want the FIRST track of that folder, not the one we happened
     * to land on -- going backwards, pass 1 returned the last. */
    first = best;
    for (i = 0; i < lb->n; i++) {
        int t = lb->order[i];
        if (t < first && same_folder(lb, t, best)) first = t;
    }

    return lib_goto_track(lb, first);
}

int lib_next_folder(library *lb) { return move_folder(lb,  1); }
int lib_prev_folder(library *lb) { return move_folder(lb, -1); }
