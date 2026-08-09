/* ------------------------------------------------------------------------
 * GC-Chiptune: host bench for the SCOPE, replayed over a real corpus.
 *
 * Exactly what the player's A button does, on a file:
 *
 *     lib_set_scope(&lb, &pl, current_folder, 0);   -- non-recursive
 *     lib_goto_track(&lb, file_index);
 *
 * and if the scope comes back empty, the screen shows "nothing playing" with no
 * further explanation. That happened on console in SOME folders, across all
 * formats -- so not a backend matter.
 *
 * This bench replays the call for EVERY folder of the corpus and compares the
 * count returned by lib_set_scope against the number of files actually sitting
 * in that folder. The two must agree everywhere.
 *
 *   scope_test <folder>
 * ------------------------------------------------------------------------ */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../src/gc/playlist.h"
#include "../src/gc/library.h"
#include "../src/gc/index.h"

/* Prefix of the folder containing `path`, trailing '/' included. */
static int parent_of(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    size_t      n;

    if (!slash) return 0;
    n = (size_t)(slash - path) + 1;
    if (n >= cap) return 0;
    memcpy(out, path, n);
    out[n] = 0;
    return 1;
}

int main(int argc, char **argv)
{
    playlist pl;
    library  lb;
    int      i, n, bad = 0, dirs = 0;
    char     prev[LIB_MAX_PREFIX];

    if (argc < 2) { fprintf(stderr, "usage: scope_test <folder>\n"); return 1; }

    pl_init(&pl);
    lib_init(&lb);

    n = pl_scan(&pl, argv[1]);
    if (n <= 0) { fprintf(stderr, "no tracks under %s\n", argv[1]); return 2; }
    printf("%d tracks\n\n", n);

    prev[0] = 0;

    for (i = 0; i < n; i++) {
        char dir[LIB_MAX_PREFIX];
        int  direct = 0, got, j;

        if (!parent_of(pl_path(&pl, i), dir, sizeof dir)) continue;
        if (!strcmp(dir, prev)) continue;          /* already handled */
        snprintf(prev, sizeof prev, "%s", dir);
        dirs++;

        /* Reference count: a linear sweep, with no bisection and no sorting
         * assumption. This is the judge. */
        for (j = 0; j < n; j++) {
            const char *p = pl_path(&pl, j);
            size_t      dl = strlen(dir);
            if (strncmp(p, dir, dl) == 0 && strchr(p + dl, '/') == NULL) direct++;
        }

        got = lib_set_scope(&lb, &pl, dir, 0);

        if (got != direct) {
            bad++;
            printf("MISMATCH  expected %4d, got %4d   %s\n", direct, got, dir);
            if (bad > 30) { printf("...\n"); break; }
        }
    }

    printf("\n%d folders examined, %d mismatch(es)\n", dirs, bad);

    /* Same check, recursive: that is what START does. */
    {
        int rbad = 0;
        prev[0] = 0;
        for (i = 0; i < n; i++) {
            char dir[LIB_MAX_PREFIX];
            int  under = 0, got, j;

            if (!parent_of(pl_path(&pl, i), dir, sizeof dir)) continue;
            if (!strcmp(dir, prev)) continue;
            snprintf(prev, sizeof prev, "%s", dir);

            for (j = 0; j < n; j++)
                if (strncmp(pl_path(&pl, j), dir, strlen(dir)) == 0) under++;

            got = lib_set_scope(&lb, &pl, dir, 1);
            if (got != under) {
                rbad++;
                printf("MISMATCH REC  expected %4d, got %4d   %s\n", under, got, dir);
                if (rbad > 30) { printf("...\n"); break; }
            }
        }
        printf("%d mismatch(es) in recursive mode\n", rbad);
        bad += rbad;
    }

    /* THE SAME CHECK AFTER A ROUND TRIP THROUGH THE INDEX CACHE.
     *
     * That is the path the console takes from the SECOND power-on onwards: the
     * list no longer comes from pl_scan but from idx_load. lib_set_scope does a
     * BISECTION there, so it requires a sorted list -- whereas the browser
     * sweeps linearly and would still show the files even if the order were
     * broken. A disagreement between the two would give exactly the observed
     * symptom: you see the track, and playing it answers "nothing playing". */
    {
        playlist    pl2;
        track_index ix2;
        int         cbad = 0, m;

        idx_init(&ix2);
        if (idx_save(&ix2, &pl, "build-host/scope_test.idx", argv[1]) != 0) {
            printf("\n(cache not written: cache check skipped)\n");
        } else {
            pl_init(&pl2);
            idx_free(&ix2); idx_init(&ix2);
            m = idx_load(&ix2, &pl2, "build-host/scope_test.idx", argv[1]);
            printf("\ncache re-read: %d tracks\n", m);

            if (m != n) { printf("COUNT MISMATCH: %d against %d\n", m, n); cbad++; }

            for (i = 1; i < m; i++)
                if (strcmp(pl_path(&pl2, i - 1), pl_path(&pl2, i)) > 0) {
                    printf("CACHE NOT SORTED at %d: \"%s\" then \"%s\"\n",
                           i, pl_path(&pl2, i - 1), pl_path(&pl2, i));
                    cbad++;
                    break;
                }

            prev[0] = 0;
            for (i = 0; i < m; i++) {
                char dir[LIB_MAX_PREFIX];
                int  direct = 0, got, j;

                if (!parent_of(pl_path(&pl2, i), dir, sizeof dir)) continue;
                if (!strcmp(dir, prev)) continue;
                snprintf(prev, sizeof prev, "%s", dir);

                for (j = 0; j < m; j++) {
                    const char *p = pl_path(&pl2, j);
                    size_t      dl = strlen(dir);
                    if (strncmp(p, dir, dl) == 0 && strchr(p + dl, '/') == NULL)
                        direct++;
                }
                got = lib_set_scope(&lb, &pl2, dir, 0);
                if (got != direct) {
                    cbad++;
                    printf("MISMATCH CACHE  expected %4d, got %4d   %s\n",
                           direct, got, dir);
                    if (cbad > 30) { printf("...\n"); break; }
                }
            }
            printf("%d mismatch(es) after the cache round trip\n", cbad);
            bad += cbad;
            pl_free(&pl2);
        }
        idx_free(&ix2);
    }

    lib_free(&lb);
    pl_free(&pl);
    return bad ? 1 : 0;
}
