/* ------------------------------------------------------------------------
 * GC-Chiptune: does the soundfont come back when we let it go?
 *
 * WHY THIS EXISTS. tools/leak_test answers "is something not released per
 * track", and its answer has been no for a while. It could not see the fault
 * that was actually reported -- "the soundfont never frees, memory saturates"
 * -- because it loads the soundfont ONCE, before the reference measurement, and
 * never touches it again. The 11.5 MB were therefore part of its baseline.
 *
 * That is the whole shape of the fault: not a leak, but a block the player
 * never asked to have back. §14.4 recorded it as an open point for weeks.
 *
 * WHAT IS MEASURED. The player's new sequence, in a loop:
 *
 *     module  ->  soundfont released before the read   (main.c, track_open)
 *     midi    ->  soundfont loaded on demand           (ensure_soundfont)
 *
 * and after each phase, the two numbers that decide anything on a GameCube:
 *
 *   COMMITTED   climbing pass after pass          -> a leak
 *   CONTIG      the largest block malloc will return. The soundfont wants
 *               ~11 MB IN ONE PIECE (tsf keeps its samples as one float array),
 *               so this is the number that says whether the reload will still
 *               be possible in an hour. A total of "10 000 kB free" has already
 *               been true and useless here twice.
 *
 * HOST ONLY, and the usual caveat applies with force: the host allocator is not
 * libogc's dlmalloc, and CONTIG on a PC is bounded by the address space, not by
 * a 24 MB arena. What this tool proves is that the RELEASE PATH RETURNS
 * EVERYTHING and that the reload survives being done a hundred times. Whether
 * the heap stays whole is a console question, and the player reports it on
 * screen (ui_now.ram_block).
 *
 * Build: see build-host.ps1
 *   sf_cycle -sf font.sf2 -mid a.mid -mod b.xm [-n passes]
 * ------------------------------------------------------------------------ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <psapi.h>

#include "../src/player/backend.h"

/* Bytes actually committed by the process. Same counter as leak_test: under
 * MinGW it is the only one that does not lie about blocks returned to the
 * allocator but not to the system. */
static size_t committed(void)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
        return 0;
    return (size_t)pmc.PrivateUsage;
}

/* Largest block still obtainable, by bisection, every attempt returned at once.
 * Capped: on a 64-bit host an uncapped probe measures the address space and
 * takes a very long time doing it. */
static size_t contig(void)
{
    size_t lo = 0, hi = 512u * 1024u * 1024u;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        void  *p   = malloc(mid);
        if (p) { free(p); lo = mid; }
        else   { hi = mid - 1; }
    }
    return lo;
}

static void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long  n;
    void *buf;

    *len = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    rewind(f);
    if (n <= 0) { fclose(f); return NULL; }
    buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* One track, opened and rendered the way the player renders it. */
static int play(const char *path, unsigned frames, short *sink)
{
    size_t       len = 0;
    void        *data = read_file(path, &len);
    gcc_backend *b = NULL;
    unsigned     done = 0;

    if (!data) return -1;
    if (gcc_open(data, len, 48000, GCC_FMT_UNKNOWN, &b) != GCC_OK) {
        free(data);
        return -1;
    }
    while (done < frames) {
        unsigned n = frames - done;
        if (n > 2048) n = 2048;
        if (gcc_render(b, sink, n) == 0) break;
        done += n;
    }
    gcc_close(b);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    const char *sf = NULL, *mid = NULL, *mod = NULL;
    int      passes = 20, p, i;
    unsigned frames = 48000;              /* 1 s per track */
    short   *sink;
    size_t   base_c, base_b;

    for (i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-sf")  && i + 1 < argc) sf  = argv[++i];
        else if (!strcmp(argv[i], "-mid") && i + 1 < argc) mid = argv[++i];
        else if (!strcmp(argv[i], "-mod") && i + 1 < argc) mod = argv[++i];
        else if (!strcmp(argv[i], "-n")   && i + 1 < argc) passes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f")   && i + 1 < argc) frames = (unsigned)atoi(argv[++i]);
    }
    if (!sf || !mid || !mod) {
        fprintf(stderr, "usage: sf_cycle -sf font.sf2 -mid a.mid -mod b.xm "
                        "[-n passes] [-f frames]\n");
        return 1;
    }

    sink = (short *)malloc(2048 * 2 * sizeof(short));
    if (!sink) return 1;

    gcc_midi_set_max_voices(GCC_MIDI_DEFAULT_VOICES);

    /* One full cycle before the reference, exactly as leak_test does: the first
     * round allocates the buffers the libraries keep for good (libxmp's mixing
     * tables, the backend's float buffer). Counting those would blame a normal
     * start-up cost. */
    if (gcc_midi_set_soundfont_file(sf) != GCC_OK) {
        fprintf(stderr, "soundfont refused: %s\n", sf);
        return 1;
    }
    play(mid, frames, sink);
    gcc_midi_free_soundfont();
    play(mod, frames, sink);

    base_c = committed();
    base_b = contig();

    printf("soundfont : %s\n", sf);
    printf("midi      : %s\n", mid);
    printf("module    : %s\n\n", mod);
    printf("  %-6s  %12s  %12s  %12s  %12s\n",
           "pass", "loaded kB", "released kB", "delta kB", "contig kB");
    printf("  %-6s  %12s  %12s  %12s  %12lu\n",
           "ref", "-", "-", "-", (unsigned long)(base_b / 1024));

    for (p = 1; p <= passes; p++) {
        size_t on, off, big;

        /* --- a MIDI: load on demand, then play it --- */
        if (gcc_midi_set_soundfont_file(sf) != GCC_OK) {
            printf("  %-6d  RELOAD REFUSED -- the heap no longer has "
                   "11 MB in one piece\n", p);
            return 1;
        }
        if (play(mid, frames, sink) != 0) { fprintf(stderr, "midi refused\n"); return 1; }
        on = committed();

        /* --- a module: the player releases the soundfont BEFORE reading it --- */
        gcc_midi_free_soundfont();
        if (play(mod, frames, sink) != 0) { fprintf(stderr, "module refused\n"); return 1; }
        off = committed();
        big = contig();

        printf("  %-6d  %12lu  %12lu  %+12ld  %12lu\n", p,
               (unsigned long)(on / 1024), (unsigned long)(off / 1024),
               (long)(((long long)off - (long long)base_c) / 1024),
               (unsigned long)(big / 1024));
        fflush(stdout);
    }

    printf("\nHOW TO READ:\n"
           "  loaded - released ~= the soundfont's weight (11.5 MB on TimGM6mb)\n"
           "  delta climbing pass after pass   -> the release does not release\n"
           "  RELOAD REFUSED                   -> the release fragments the heap,\n"
           "                                      which is worse than keeping it\n"
           "  delta flat, no refusal           -> the cycle is sound\n");

    free(sink);
    return 0;
}
