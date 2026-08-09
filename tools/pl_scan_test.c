/* ------------------------------------------------------------------------
 * GC-Chiptune: host bench for the folder walk (src/gc/playlist.c).
 *
 * Why a separate bench: Dolphin DOES NOT EMULATE the SD Gecko or the SD2SP2
 * (checked on Dolphin 2606: no SD option in the GameCube slots, only the Wii
 * has an SD card). The libfat mount is therefore only testable on real
 * hardware. Everything else -- recursive walk, extension filter, bounds,
 * sorting -- is portable code: we replay it here over the full corpus, which
 * covers most of the new logic.
 *
 *   pl_scan_test <folder>
 * ------------------------------------------------------------------------ */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../src/gc/playlist.h"

/* Breakdown by extension, to compare against the corpus census. */
#define MAX_EXTS 64

typedef struct { char ext[12]; int n; } extcount;

static extcount g_exts[MAX_EXTS];
static int      g_nexts;

static void tally(const char *path)
{
    const char *dot = strrchr(path, '.');
    char        e[12];
    size_t      i;
    int         k;

    if (!dot) return;
    dot++;
    for (i = 0; i < sizeof e - 1 && dot[i]; i++)
        e[i] = (char)((dot[i] >= 'A' && dot[i] <= 'Z') ? dot[i] + 32 : dot[i]);
    e[i] = '\0';

    for (k = 0; k < g_nexts; k++)
        if (strcmp(g_exts[k].ext, e) == 0) { g_exts[k].n++; return; }

    if (g_nexts < MAX_EXTS) {
        strcpy(g_exts[g_nexts].ext, e);
        g_exts[g_nexts].n = 1;
        g_nexts++;
    }
}

int main(int argc, char **argv)
{
    playlist pl;
    int      n, i;
    clock_t  t0;

    if (argc < 2) {
        fprintf(stderr, "usage: pl_scan_test <folder>\n");
        return 2;
    }

    pl_init(&pl);
    t0 = clock();
    n  = pl_scan(&pl, argv[1]);

    if (n < 0) {
        fprintf(stderr, "FAILED: %s unreadable\n", argv[1]);
        return 1;
    }

    printf("root      : %s\n", argv[1]);
    printf("tracks    : %d\n", n);
    printf("folders   : %d\n", pl.dirs);
    printf("skipped   : %d\n", pl.skipped);
    printf("truncated : %s\n", pl.truncated ? "YES" : "no");
    printf("pool      : %lu bytes for %d paths (%lu B/path)\n",
           (unsigned long)pl.pool_len, n,
           n ? (unsigned long)pl.pool_len / (unsigned long)n : 0UL);
    printf("duration  : %.2f s\n\n", (double)(clock() - t0) / CLOCKS_PER_SEC);

    for (i = 0; i < n; i++) tally(pl_path(&pl, i));
    printf("breakdown:\n");
    for (i = 0; i < g_nexts; i++)
        printf("  %-6s %5d\n", g_exts[i].ext, g_exts[i].n);

    /* Sorting drives navigation: check it explicitly. */
    {
        int bad = 0;
        for (i = 1; i < n; i++)
            if (strcmp(pl_path(&pl, i - 1), pl_path(&pl, i)) > 0) bad++;
        printf("\nsorting   : %s (%d inversions)\n", bad ? "BROKEN" : "ok", bad);
    }

    if (n > 0) {
        printf("first     : %s\n", pl_path(&pl, 0));
        printf("last      : %s\n", pl_path(&pl, n - 1));
        printf("basename  : %s\n", pl_basename(&pl, n - 1));
    }

    pl_free(&pl);
    return 0;
}
