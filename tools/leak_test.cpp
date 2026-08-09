/* ------------------------------------------------------------------------
 * GC-Chiptune: does memory drain with use?
 *
 * Reported symptom: after a while of playback, no track starts any more,
 * "memory full". Two possible causes, which are fixed very differently:
 *
 *   LEAK           something is not released for every track. The allocated
 *                  total climbs without coming back down.
 *   FRAGMENTATION  everything is released, but the heap is too holed to fit a
 *                  large block. The total is stable, the LARGEST FREE BLOCK
 *                  collapses.
 *
 * The second case is this project's trap: "free memory: 10 000 kB" is perfectly
 * true while a 2.7 MB malloc fails. So we measure BOTH, and it is their
 * divergence that names the culprit.
 *
 * This bench replays exactly the player's sequence -- read the file, detect,
 * open, render, close, free -- over a list given as input, in a loop, reporting
 * both numbers.
 *
 * Host only: the console has neither psapi nor a comfortable way to print. A
 * leak in backend.cpp or libxmp shows here; fragmentation specific to libogc's
 * allocator does not -- for that, see the "contig" number added to the player.
 *
 * Build: see build-host.ps1
 * ------------------------------------------------------------------------ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <psapi.h>

#include "../src/player/backend.h"

/* Bytes actually committed by the process. Under MinGW, PrivateUsage is the
 * only counter that does not lie about blocks returned to the allocator but not
 * to the system -- which is precisely what we want to see. */
static size_t private_bytes(void)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
        return 0;
    return (size_t)pmc.PrivateUsage;
}

/* Largest block the allocator will still return, by bisection.
 *
 * THIS IS THE MEASUREMENT THAT COUNTS. A holed heap announces plenty of free
 * memory and still refuses the next module: the total says nothing, only the
 * largest contiguous block decides whether a 2.7 MB file loads.
 *
 * We probe without ever keeping: every attempt is returned immediately. */
static size_t largest_block(void)
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
    FILE  *f = fopen(path, "rb");
    long   n;
    void  *buf;

    *len = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    rewind(f);
    if (n <= 0) { fclose(f); return NULL; }

    buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* One track, start to finish, the way the player plays it. `render_frames` is
 * the number of frames to render before closing: 0 to only open it. */
static int play_once(const char *path, unsigned render_frames, short *sink)
{
    size_t       len = 0;
    void        *data = read_file(path, &len);
    gcc_backend *b = NULL;
    gcc_format   fmt;
    int          rc;

    if (!data) return -1;

    fmt = gcc_detect(path, (const unsigned char *)data, len);
    if (fmt == GCC_FMT_UNKNOWN) { free(data); return -1; }

    rc = gcc_open(data, len, 48000, fmt, &b);
    if (rc != GCC_OK) { free(data); return -1; }

    if (render_frames) {
        unsigned done = 0;
        while (done < render_frames) {
            unsigned n = render_frames - done;
            if (n > 2048) n = 2048;
            if (gcc_render(b, sink, n) == 0) break;
            done += n;
        }
    }

    gcc_close(b);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    char   **list = NULL;
    int      count = 0, cap = 0;
    int      passes = 20;
    unsigned frames = 48000;      /* 1 s of rendering per track */
    char     line[1024];
    short   *sink;
    size_t   base_priv, base_big;
    int      p, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc)      passes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-sf") && i + 1 < argc) {
            size_t sflen = 0;
            void  *sf    = read_file(argv[++i], &sflen);
            if (!sf) { fprintf(stderr, "soundfont unreadable\n"); return 1; }
            if (gcc_midi_set_soundfont(sf, sflen) != GCC_OK) {
                fprintf(stderr, "soundfont refused\n"); return 1;
            }
            /* Like the player: the source block is only needed for loading. */
            free(sf);
            gcc_midi_set_max_voices(GCC_MIDI_DEFAULT_VOICES);
        }
    }

    sink = (short *)malloc(2048 * 2 * sizeof(short));
    if (!sink) return 1;

    /* The track list arrives on stdin, one path per line. */
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 64;
            list = (char **)realloc(list, (size_t)cap * sizeof *list);
        }
        list[count++] = _strdup(line);
    }
    if (!count) {
        fprintf(stderr, "usage: leak_test [-n passes] [-f frames] [-sf font.sf2] < list\n");
        return 1;
    }

    printf("%d tracks, %d passes, %u frames rendered per track\n\n",
           count, passes, frames);

    /* One warm-up pass BEFORE the reference: the first round allocates the
     * internal buffers the libraries keep (libxmp's mixing tables, the MIDI
     * backend's float buffer). Counting those as a leak would blame a perfectly
     * normal start-up cost. */
    for (i = 0; i < count; i++) play_once(list[i], frames, sink);

    base_priv = private_bytes();
    base_big  = largest_block();

    printf("  %-6s  %12s  %12s  %12s\n",
           "pass", "committed kB", "delta kB", "contig kB");
    printf("  %-6s  %12lu  %12s  %12lu\n",
           "ref", (unsigned long)(base_priv / 1024), "-",
           (unsigned long)(base_big / 1024));

    for (p = 1; p <= passes; p++) {
        size_t priv, big;

        for (i = 0; i < count; i++) play_once(list[i], frames, sink);

        priv = private_bytes();
        big  = largest_block();

        printf("  %-6d  %12lu  %+12ld  %12lu\n", p,
               (unsigned long)(priv / 1024),
               (long)((long long)priv - (long long)base_priv) / 1024,
               (unsigned long)(big / 1024));
        fflush(stdout);
    }

    printf("\nHOW TO READ:\n"
           "  delta climbing pass after pass       -> LEAK\n"
           "  delta stable, contig collapsing      -> FRAGMENTATION\n"
           "  both stable                          -> neither, here\n");

    free(sink);
    for (i = 0; i < count; i++) free(list[i]);
    free(list);
    return 0;
}
