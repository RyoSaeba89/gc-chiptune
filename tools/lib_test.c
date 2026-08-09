/* ------------------------------------------------------------------------
 * GC-Chiptune: host bench for src/gc/library.c.
 *
 *   lib_test <folder>         replays the logic over a real corpus
 *   lib_test                  synthetic data set only
 *
 * The shuffle bag, the scope bounds and the repeat modes are exactly what you
 * do NOT want to debug with a gamepad on a console you power-cycle for every
 * attempt. library.c depends on neither the screen, nor the audio, nor the
 * card: it is proved here.
 *
 * The check that matters is the PERMUTATION one: over one complete pass in
 * shuffle mode, every track must come out once and only once. That is the
 * difference between a bag and a draw with replacement, and it is the property
 * that was asked for.
 * ------------------------------------------------------------------------ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gc/library.h"
#include "../src/gc/index.h"
#include "../src/gc/state.h"

static int g_fail = 0;

static void check(int cond, const char *what)
{
    printf("  %-58s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond) g_fail++;
}

/* ------------------------------------------------------------ synthetic set */

/* We build a playlist by hand rather than requiring a corpus: the bench has to
 * run anywhere, including with no card and no music folder. The paths are
 * ALREADY sorted, as pl_scan() guarantees. */
static const char *g_paths[] = {
    "sd:/m/AAA/a1.xm",
    "sd:/m/AAA/a2.xm",
    "sd:/m/AAA/sub/a3.xm",
    "sd:/m/AAA/sub/a4.xm",
    "sd:/m/BBB/b1.mod",
    "sd:/m/BBB/b2.mod",
    "sd:/m/BBB/b3.mod",
    "sd:/m/CCC/c1.it",
};
#define NPATHS ((int)(sizeof g_paths / sizeof g_paths[0]))

static void fake_playlist(playlist *pl)
{
    int i;
    size_t off = 0;

    pl_init(pl);
    for (i = 0; i < NPATHS; i++) off += strlen(g_paths[i]) + 1;

    pl->pool    = (char *)malloc(off);
    pl->offsets = (unsigned *)malloc(sizeof(unsigned) * NPATHS);
    pl->pool_cap = pl->pool_len = off;
    pl->count = pl->cap = NPATHS;

    off = 0;
    for (i = 0; i < NPATHS; i++) {
        pl->offsets[i] = (unsigned)off;
        memcpy(pl->pool + off, g_paths[i], strlen(g_paths[i]) + 1);
        off += strlen(g_paths[i]) + 1;
    }
}

/* --------------------------------------------------------------------- tests */

static void test_scope(playlist *pl)
{
    library lb;

    printf("\nscope\n");
    lib_init(&lb);

    check(lib_set_scope(&lb, pl, "", 1) == NPATHS,
          "root, recursive: every track");

    check(lib_set_scope(&lb, pl, "sd:/m/AAA/", 1) == 4,
          "one folder, recursive: it and its sub-folders");

    check(lib_set_scope(&lb, pl, "sd:/m/AAA/", 0) == 2,
          "one folder, NON-recursive: its direct files only");

    check(lib_set_scope(&lb, pl, "sd:/m/BBB/", 1) == 3,
          "folder with no sub-folder");

    check(lib_set_scope(&lb, pl, "sd:/m/ZZZ/", 1) == 0,
          "non-existent folder: empty scope");

    /* The trap of a prefix without '/': "sd:/m/A" would also catch "AAA" and
     * every folder starting with A. That is why lib_set_scope requires it. */
    check(lib_set_scope(&lb, pl, "sd:/m/AAA/sub/", 1) == 2,
          "deep sub-folder");

    lib_free(&lb);
}

static void test_sequential(playlist *pl)
{
    library lb;
    int i, ok = 1;

    printf("\nsequential order\n");
    lib_init(&lb);
    lib_set_scope(&lb, pl, "", 1);
    lib_set_repeat(&lb, LIB_REPEAT_OFF);

    for (i = 0; i < NPATHS; i++) {
        if (lib_current(&lb) != i) ok = 0;
        if (i < NPATHS - 1 && !lib_next(&lb)) ok = 0;
    }
    check(ok, "the list runs in playlist order");
    check(lib_next(&lb) == 0, "REPEAT_OFF: playback stops at the end of the list");

    lib_set_repeat(&lb, LIB_REPEAT_LIST);
    check(lib_next(&lb) == 1 && lib_current(&lb) == 0,
          "REPEAT_LIST: playback starts over");

    lib_set_repeat(&lb, LIB_REPEAT_TRACK);
    check(lib_next(&lb) == 1 && lib_current(&lb) == 0,
          "REPEAT_TRACK: next does not move");

    lib_free(&lb);
}

static void test_prev(playlist *pl)
{
    library lb;

    printf("\nprevious\n");
    lib_init(&lb);
    lib_set_scope(&lb, pl, "", 1);
    lib_set_repeat(&lb, LIB_REPEAT_OFF);

    lib_next(&lb); lib_next(&lb);
    check(lib_current(&lb) == 2, "moved forward by two");
    lib_prev(&lb);
    check(lib_current(&lb) == 1, "moved back by one");

    lib_jump(&lb, -10);
    check(lib_current(&lb) == 0, "negative jump clamped to the start, no wrap");
    lib_jump(&lb, 1000);
    check(lib_current(&lb) == NPATHS - 1, "positive jump clamped to the end");

    check(lib_prev(&lb) == 1, "previous from the end");
    lib_jump(&lb, -1000);
    check(lib_prev(&lb) == 0, "REPEAT_OFF: no previous before the first");

    lib_free(&lb);
}

/* THE check: a bag, not a draw with replacement. */
static void test_shuffle_bag(playlist *pl)
{
    library lb;
    int     seen[NPATHS];
    int     i, run, ok_all = 1, ok_full = 1;

    printf("\nshuffle bag\n");
    lib_init(&lb);
    lib_set_order(&lb, LIB_ORDER_RANDOM);
    lib_set_scope(&lb, pl, "", 1);
    lib_set_repeat(&lb, LIB_REPEAT_LIST);

    /* Three complete passes in a row: each must be a permutation. */
    for (run = 0; run < 3; run++) {
        memset(seen, 0, sizeof seen);
        for (i = 0; i < NPATHS; i++) {
            int t = lib_current(&lb);
            if (t < 0 || t >= NPATHS || seen[t]) ok_all = 0;
            seen[t] = 1;
            lib_next(&lb);
        }
        for (i = 0; i < NPATHS; i++) if (!seen[i]) ok_full = 0;
    }
    check(ok_all,  "no track repeats before the end of the pass");
    check(ok_full, "every track comes out on every pass");

    /* Going back must return the track that was ACTUALLY played before, not a
     * fresh draw. That is free with a permutation, impossible without. */
    {
        int a, b;
        lib_set_scope(&lb, pl, "", 1);
        a = lib_current(&lb);
        lib_next(&lb);
        b = lib_current(&lb);
        lib_prev(&lb);
        check(lib_current(&lb) == a && a != b,
              "previous returns to the track actually played before");
    }

    lib_free(&lb);
}

/* Two different seeds must give two different orders, or the console would
 * replay the same "bag" on every power-on. */
static void test_seed(playlist *pl)
{
    library a, b;
    int i, differ = 0;

    printf("\nseed\n");
    lib_init(&a); lib_init(&b);
    lib_set_order(&a, LIB_ORDER_RANDOM);
    lib_set_order(&b, LIB_ORDER_RANDOM);
    lib_set_seed(&a, 12345u);
    lib_set_seed(&b, 99999u);
    lib_set_scope(&a, pl, "", 1);
    lib_set_scope(&b, pl, "", 1);

    for (i = 0; i < NPATHS; i++)
        if (a.order[i] != b.order[i]) differ = 1;
    check(differ, "two seeds give two different orders");

    /* ... and the same seed must give the same order back, or resume after
     * power-off could not land on its feet. */
    {
        library c;
        int same = 1;
        lib_init(&c);
        lib_set_order(&c, LIB_ORDER_RANDOM);
        lib_set_seed(&c, 12345u);
        lib_set_scope(&c, pl, "", 1);
        for (i = 0; i < NPATHS; i++) if (a.order[i] != c.order[i]) same = 0;
        check(same, "the same seed gives the same order back");
        lib_free(&c);
    }

    lib_free(&a); lib_free(&b);
}

static void test_folders(playlist *pl)
{
    library lb;

    printf("\nfolder jump\n");
    lib_init(&lb);
    lib_set_scope(&lb, pl, "", 1);

    check(lib_next_folder(&lb) && lib_current(&lb) == 2,
          "next folder from AAA -> AAA/sub, first track");
    check(lib_next_folder(&lb) && lib_current(&lb) == 4,
          "then BBB, first track");
    check(lib_prev_folder(&lb) && lib_current(&lb) == 2,
          "previous folder: FIRST track of the folder, not the last");

    lib_jump(&lb, 1000);
    check(lib_next_folder(&lb) == 0, "no folder after the last one");

    /* Folder jumps must keep their meaning in shuffle mode too, where order[]
     * is no longer sorted. */
    lib_set_order(&lb, LIB_ORDER_RANDOM);
    lib_goto_track(&lb, 0);
    check(lib_next_folder(&lb) && lib_current(&lb) == 2,
          "in shuffle mode too, the jump reasons about the tree");

    lib_free(&lb);
}

static void test_switch_order(playlist *pl)
{
    library lb;
    int cur;

    printf("\norder switch during playback\n");
    lib_init(&lb);
    lib_set_scope(&lb, pl, "", 1);
    lib_jump(&lb, 5);
    cur = lib_current(&lb);

    lib_set_order(&lb, LIB_ORDER_RANDOM);
    check(lib_current(&lb) == cur, "switching to shuffle: the track does not change");
    lib_set_order(&lb, LIB_ORDER_SEQ);
    check(lib_current(&lb) == cur, "back to sequential: the track does not change");

    lib_free(&lb);
}

/* -------------------------------------------------------------- index cache */

/* The cache is the riskiest piece of the lot: binary format, computed offsets,
 * and a spot write by fseek in the middle of the file. An offset error there
 * would play the wrong file or corrupt the card, and would only show on the
 * next start-up. */
static void test_index(playlist *pl)
{
    const char  *file = "build-host/test.idx";
    track_index  ix;
    playlist     back;
    int          i, ok;

    printf("\nindex cache\n");
    idx_init(&ix);

    check(idx_save(&ix, pl, file, "sd:/m") == 0, "writing the cache");

    /* Round trip: the paths must come back identical, in the same order. This
     * is the same check that proved the SF2 path without a GameCube
     * (STATUS 7.6). */
    pl_init(&back);
    check(idx_load(&ix, &back, file, "sd:/m") == NPATHS,
          "re-read: the right number of tracks");
    ok = 1;
    for (i = 0; i < NPATHS; i++)
        if (strcmp(pl_path(&back, i), g_paths[i]) != 0) ok = 0;
    check(ok, "round trip: bit-identical paths");

    /* A different root must make the cache be REFUSED: otherwise we would play
     * paths that no longer exist. */
    {
        playlist other;
        pl_init(&other);
        check(idx_load(&ix, &other, file, "sd:/other") == -1,
              "cache of another root: refused");
        pl_free(&other);
        /* idx_load released the state: reload it for what follows. */
        idx_load(&ix, &back, file, "sd:/m");
    }

    /* Filling in as we go. */
    check(idx_duration(&ix, 3) == 0, "duration unknown to begin with");
    idx_set_duration(&ix, 3, 187000u);
    idx_set_duration(&ix, 0, 42u);
    check(idx_duration(&ix, 3) == 187000u, "duration noted in memory");
    check(idx_known(&ix) == 2, "count of known durations");

    {
        playlist again;
        track_index ix2;
        pl_init(&again);
        idx_init(&ix2);
        check(idx_load(&ix2, &again, file, "sd:/m") == NPATHS,
              "re-read after the spot write");
        check(idx_duration(&ix2, 3) == 187000u && idx_duration(&ix2, 0) == 42u,
              "the noted durations did land in the file");
        check(idx_duration(&ix2, 5) == 0,
              "the other durations were not touched");
        /* The pool must have survived the write in the middle of the file. */
        ok = 1;
        for (i = 0; i < NPATHS; i++)
            if (strcmp(pl_path(&again, i), g_paths[i]) != 0) ok = 0;
        check(ok, "the paths were not damaged by the fseek");
        idx_free(&ix2);
        pl_free(&again);
    }

    idx_free(&ix);
    pl_free(&back);
    remove(file);
}

static void test_state(void)
{
    const char  *file = "build-host/test.state";
    player_state a, b;

    printf("\nresume after power-off\n");

    st_defaults(&a);
    snprintf(a.track, sizeof a.track, "%s", "sd:/m/BBB/b2.mod");
    snprintf(a.scope, sizeof a.scope, "%s", "sd:/m/BBB/");
    a.recursive = 0;
    a.order     = LIB_ORDER_RANDOM;
    a.repeat    = LIB_REPEAT_TRACK;
    a.seed      = 987654321u;
    a.volume    = 65u;

    check(st_save(&a, file) == 0, "writing the state");
    check(st_load(&b, file) == 1, "re-reading");
    check(!strcmp(a.track, b.track) && !strcmp(a.scope, b.scope) &&
          a.recursive == b.recursive && a.order == b.order &&
          a.repeat == b.repeat && a.seed == b.seed && a.volume == b.volume,
          "every field survives the round trip");

    check(st_load(&b, "build-host/no-such-file.state") == 0,
          "missing file: defaults, not a failure");
    check(b.recursive == 1 && b.repeat == LIB_REPEAT_LIST && b.volume == 100,
          "the defaults are the whole pack, repeat list, full volume");

    remove(file);
}

/* --------------------------------------------------------------- real corpus */

static void test_corpus(const char *root)
{
    playlist pl;
    library  lb;
    int      n, i, ok = 1;
    int     *seen;

    printf("\nreal corpus: %s\n", root);
    pl_init(&pl);
    n = pl_scan(&pl, root);
    if (n <= 0) { printf("  (no tracks, skipped)\n"); pl_free(&pl); return; }
    printf("  %d tracks, %d folders\n", n, pl.dirs);

    lib_init(&lb);
    check(lib_set_scope(&lb, &pl, "", 1) == n, "recursive root = the whole list");

    /* The permutation check, but over the real corpus's 5000 tracks: that is
     * the size at which an index error shows. */
    lib_set_order(&lb, LIB_ORDER_RANDOM);
    lib_set_scope(&lb, &pl, "", 1);
    seen = (int *)calloc((size_t)n, sizeof(int));
    for (i = 0; i < n; i++) {
        int t = lib_current(&lb);
        if (t < 0 || t >= n || seen[t]) ok = 0;
        seen[t] = 1;
        lib_next(&lb);
    }
    for (i = 0; i < n; i++) if (!seen[i]) ok = 0;
    check(ok, "exact permutation over the whole corpus");
    free(seen);

    /* One scope per top-level folder, to exercise the bounds on real names --
     * the pack's contain spaces, dots and dashes. */
    {
        char   prefix[LIB_MAX_PREFIX];
        int    total = 0, scopes = 0;
        size_t rootlen = strlen(root);
        const char *last = NULL;

        for (i = 0; i < n; i++) {
            const char *p = pl_path(&pl, i);
            const char *slash;
            size_t      len;

            if (strncmp(p, root, rootlen) != 0) continue;
            slash = strchr(p + rootlen + 1, '/');
            if (!slash) continue;
            len = (size_t)(slash - p) + 1;
            if (len >= sizeof prefix) continue;
            if (last && strncmp(last, p, len) == 0) continue;

            memcpy(prefix, p, len); prefix[len] = 0;
            total += lib_set_scope(&lb, &pl, prefix, 1);
            scopes++;
            last = p;
        }
        printf("  %d top-level folders, %d tracks covered\n",
               scopes, total);
        check(total <= n, "no track counted twice by the scopes");
    }

    lib_free(&lb);
    pl_free(&pl);
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    playlist pl;

    printf("=== bench for src/gc/library.c ===\n");

    fake_playlist(&pl);
    test_scope(&pl);
    test_sequential(&pl);
    test_prev(&pl);
    test_shuffle_bag(&pl);
    test_seed(&pl);
    test_folders(&pl);
    test_switch_order(&pl);
    test_index(&pl);
    test_state();
    pl_free(&pl);

    if (argc > 1) test_corpus(argv[1]);

    printf("\n%s\n", g_fail ? "SOME TESTS FAILED" : "all tests pass");
    return g_fail ? 1 : 0;
}
