/* ------------------------------------------------------------------------
 * GC-Chiptune: host test bench for the backends (src/player/backend.h).
 *
 *   gcc_render <file> [output.wav] [-r 48000] [-s 20] [-sf font.sf2]
 *   gcc_render -batch  [-sf font.sf2]     (list of paths on stdin)
 *   gcc_render -triage [-sf font.sf2]     (same, one TSV line per file)
 *
 * In batch mode it writes nothing to disk and summarises per format: refused
 * loads, silent tracks, NaNs, and CPU cost expressed as "real time x N" -- the
 * number used to budget the Gekko.
 *
 * Triage mode is for culling a pack: it emits ONE line per file,
 *
 *     verdict <TAB> backend <TAB> duration_ms <TAB> reason <TAB> path
 *
 * with verdict in OK / SILENT / NAN / REFUSED / UNSUPPORTED. Batch mode only
 * talks about rejects, which is enough for a summary but not for sorting: you
 * also need the list of what you keep.
 * ------------------------------------------------------------------------ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../src/player/backend.h"

#define BLOCK 1024

static unsigned char *slurp(const char *path, size_t *out_len, size_t pad)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n <= 4) { fclose(f); return NULL; }
    rewind(f);
    buf = (unsigned char *)calloc((size_t)n + pad, 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static void put32(FILE *f, unsigned long v)
{
    fputc((int)(v & 0xff), f);         fputc((int)((v >> 8) & 0xff), f);
    fputc((int)((v >> 16) & 0xff), f); fputc((int)((v >> 24) & 0xff), f);
}
static void put16(FILE *f, unsigned v)
{ fputc((int)(v & 0xff), f); fputc((int)((v >> 8) & 0xff), f); }

static FILE *wav_open(const char *path, unsigned rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    fwrite("RIFF", 1, 4, f); put32(f, 0);
    fwrite("WAVEfmt ", 1, 8, f); put32(f, 16);
    put16(f, 1); put16(f, 2);
    put32(f, rate); put32(f, rate * 4);
    put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, 0);
    return f;
}
static void wav_close(FILE *f, unsigned long databytes)
{
    fseek(f, 4, SEEK_SET);  put32(f, 36 + databytes);
    fseek(f, 40, SEEK_SET); put32(f, databytes);
    fclose(f);
}

typedef struct {
    gcc_format fmt;
    double     peak, cpu_seconds, audio_seconds;
    double     rms;
    unsigned long zero_runs;   /* zero samples (silence detection)             */
    unsigned long nans;        /* NaNs replaced by silence during the render   */
    unsigned long clipped;     /* samples pinned to the 16-bit rail            */
    unsigned long duration_ms; /* length announced by the format, 0 = unknown  */
    /* WORST WINDOW, not the mean over the track.
     *
     * A track builds up: the densest passage often comes well after the start.
     * A mean, or worse a measurement over the first twelve seconds, says
     * nothing about what falls over at the two-minute mark. This is the number
     * that decides -- the V2M was judged on twelve seconds for two days
     * (docs/STATUS.md 13). */
    double     worst_rt;       /* real-time factor of the worst window */
    double     worst_at;       /* where it falls, in seconds           */
    /* REQUIRED LEAD, in seconds of audio.
     *
     * This is the question that decides between "this track is unplayable" and
     * "this track needs a bigger buffer". A load peak condemns nothing if it is
     * preceded by a light passage: it is enough to have rendered enough lead
     * during the lull.
     *
     * So we integrate (CPU time consumed - audio time produced) along the
     * track, in CONSOLE seconds. The maximum of that integral is exactly the
     * number of seconds of audio you have to hold in reserve before the peak in
     * order not to be caught up. */
    double     need_lead;      /* worst cumulative deficit, in seconds */
    double     need_at;
    int        open_error;
} stats;

/* Speed ratio console / this machine.
 *
 * Used to project a host measurement into per mille of Gekko load. The value
 * was anchored on the one track for which we had both measurements; it depends
 * heavily on the backend and on the host machine. -K changes it.
 *
 * TREAT AS AN ORDER OF MAGNITUDE, never as a measurement. */
static double g_console_k = 25.7;

/* Length of a measurement window, in seconds of audio produced. Long enough
 * for clock() (1 ms resolution) to stay meaningful, short enough not to drown a
 * peak in an average. */
#define WINDOW_S 1.0

static int render_one(const char *inpath, const char *outpath,
                      unsigned rate, double seconds, stats *st, int quiet)
{
    size_t len;
    unsigned char *data;
    gcc_backend *b = NULL;
    FILE *wav = NULL;
    unsigned long total = 0, want, nonzero = 0;
    double sumsq = 0.0;
    static short pcm[BLOCK * 2];
    clock_t t0;
    int rc;

    memset(st, 0, sizeof *st);

    data = slurp(inpath, &len, 0);
    if (!data) { st->open_error = GCC_ERR_LOAD; if (!quiet) fprintf(stderr, "%s: unreadable\n", inpath); return 2; }

    st->fmt = gcc_detect(inpath, data, len);
    rc = gcc_open(data, len, rate, st->fmt, &b);
    if (rc != GCC_OK) {
        st->open_error = rc;
        if (!quiet) fprintf(stderr, "%s: %s\n", inpath, gcc_strerror(rc));
        free(data);
        return 3;
    }

    st->duration_ms = gcc_duration_ms(b);

    if (outpath) {
        wav = wav_open(outpath, rate);
        if (!wav) { gcc_close(b); free(data); return 5; }
    }

    want = (unsigned long)(seconds * rate);
    st->nans = gcc_render_nans();   /* cumulative counter: we take the delta */
    st->worst_rt = 1e9;
    t0 = clock();
    {
    unsigned long win_frames = (unsigned long)(WINDOW_S * rate);
    unsigned long win_done   = 0;
    clock_t       win_t0     = t0;
    double        deficit    = 0.0;

    while (total < want) {
        unsigned long n = want - total; int i, alive;
        if (n > BLOCK) n = BLOCK;
        alive = gcc_render(b, pcm, (unsigned)n);

        for (i = 0; i < (int)(n * 2); i++) {
            double v = pcm[i] / 32768.0;
            if (pcm[i]) nonzero++;
            /* Samples pinned to the rail: that is CLIPPING, and that is what
             * is heard as a background fizz. A peak of 1.000 does not say
             * whether there is one of them or a million. */
            if (pcm[i] >= 32767 || pcm[i] <= -32768) st->clipped++;
            if (fabs(v) > st->peak) st->peak = fabs(v);
            sumsq += v * v;
        }
        if (wav) fwrite(pcm, sizeof(short), n * 2, wav);
        total += n;

        /* End of window: keep the worst, and track the cumulative deficit. */
        win_done += n;
        if (win_done >= win_frames) {
            clock_t now = clock();
            double  cpu = (double)(now - win_t0) / CLOCKS_PER_SEC;
            double  aud = (double)win_done / rate;
            double  rt  = (cpu > 0.0) ? aud / cpu : 1e9;
            if (rt < st->worst_rt) { st->worst_rt = rt; st->worst_at = (double)total / rate; }

            /* Deficit: what the console would have taken beyond real time. It
             * cannot go below zero -- lead only accumulates up to the buffer
             * size, and we are looking for the worst dip to cover, not an
             * unlimited credit. */
            deficit += g_console_k * cpu - aud;
            if (deficit < 0.0) deficit = 0.0;
            if (deficit > st->need_lead) {
                st->need_lead = deficit;
                st->need_at   = (double)total / rate;
            }

            win_t0   = now;
            win_done = 0;
        }

        if (!alive && total > rate / 2) break;   /* track finished */
    }
    }
    if (st->worst_rt > 1e8) st->worst_rt = 0.0;   /* track shorter than one window */
    st->cpu_seconds   = (double)(clock() - t0) / CLOCKS_PER_SEC;
    st->nans          = gcc_render_nans() - st->nans;
    st->audio_seconds = (double)total / rate;
    st->rms = total ? sqrt(sumsq / (total * 2)) : 0.0;
    st->zero_runs = total * 2 - nonzero;

    if (wav) wav_close(wav, total * 4);

    if (!quiet) {
        printf("%s\n", inpath);
        printf("   backend=%-7s title=\"%s\"\n", gcc_backend_name(b), gcc_title(b));
        printf("   rendered %.1fs @%uHz in %.2fs CPU  ->  real time x%.1f\n",
               st->audio_seconds, rate, st->cpu_seconds,
               st->cpu_seconds > 0.0 ? st->audio_seconds / st->cpu_seconds : 0.0);
        printf("   peak=%.3f  rms=%.4f%s\n", st->peak, st->rms,
               st->rms < 1e-6 ? "   << SILENCE" : "");
    }

    gcc_close(b);
    free(data);
    return 0;
}

static const char *fmtname(gcc_format f)
{
    switch (f) {
        case GCC_FMT_MODULE: return "module";
        case GCC_FMT_MIDI:   return "midi";
        default:             return "unknown";
    }
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *sfpath = NULL;
    unsigned rate = 48000;
    double seconds = 20.0;
    int batch = 0, triage = 0, i;
    stats st;
    unsigned char *sf = NULL; size_t sflen = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-sf") && i + 1 < argc) sfpath = argv[++i];
        else if (!strcmp(argv[i], "-batch")) batch = 1;
        else if (!strcmp(argv[i], "-triage")) { triage = 1; batch = 1; }
        else if (!strcmp(argv[i], "-K") && i + 1 < argc) g_console_k = atof(argv[++i]);
        else if (!in) in = argv[i];
        else if (!out) out = argv[i];
    }

    if (sfpath) {
        sf = slurp(sfpath, &sflen, 0);
        if (!sf) { fprintf(stderr, "soundfont unreadable: %s\n", sfpath); return 1; }
        if (gcc_midi_set_soundfont(sf, sflen) != GCC_OK) {
            fprintf(stderr, "soundfont refused by TinySoundFont: %s\n", sfpath);
            return 1;
        }
        if (!batch) printf("soundfont: %s (%.2f MB)\n\n", sfpath, sflen / 1048576.0);
    }

    if (batch) {
        char line[4096];
        unsigned long n = 0;
        struct { unsigned long files, bad, silent; double cpu, audio, worst; char worstname[512]; } acc[4];
        int k;
        for (k = 0; k < 4; k++) {
            acc[k].files = acc[k].bad = acc[k].silent = 0;
            acc[k].cpu = acc[k].audio = 0.0; acc[k].worst = 1e9; acc[k].worstname[0] = 0;
        }

        while (fgets(line, sizeof line, stdin)) {
            char *p = line + strlen(line);
            int idx;
            while (p > line && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' ')) *--p = 0;
            if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF) memmove(line, line + 3, strlen(line + 3) + 1);
            if (!line[0]) continue;
            n++;

            render_one(line, NULL, rate, seconds, &st, 1);
            idx = (st.fmt >= GCC_FMT_MODULE && st.fmt <= GCC_FMT_MIDI) ? (int)st.fmt : 0;
            acc[idx].files++;

            /* Triage mode: ONE line per file, verdict first, fields separated
             * by tabs. The original -batch mode only talks about refusals and
             * silences -- handy for a summary, useless for culling a pack,
             * where you also need to know what you keep. */
            if (triage) {
                const char *verdict;
                char reason[256];
                reason[0] = 0;
                if (st.fmt == GCC_FMT_UNKNOWN && st.open_error == GCC_ERR_FORMAT) {
                    verdict = "UNSUPPORTED";
                    snprintf(reason, sizeof reason, "no backend for this format");
                } else if (st.open_error) {
                    verdict = "REFUSED";
                    snprintf(reason, sizeof reason, "%s%s%s",
                             gcc_strerror(st.open_error),
                             gcc_last_detail()[0] ? " -- " : "", gcc_last_detail());
                } else if (st.rms < 1e-6) {
                    verdict = "SILENT";
                    snprintf(reason, sizeof reason, "loads but produces no sound");
                } else if (st.nans) {
                    verdict = "NAN";
                    snprintf(reason, sizeof reason, "%lu NaN samples", st.nans);
                } else {
                    verdict = "OK";
                    snprintf(reason, sizeof reason,
                             "peak %.3f  rms %.4f  clipped %lu  "
                             "avg x%.1f  WORST x%.1f at %.0fs  LEAD %.1fs at %.0fs",
                             st.peak, st.rms, st.clipped,
                             st.cpu_seconds > 0.0 ? st.audio_seconds / st.cpu_seconds : 0.0,
                             st.worst_rt, st.worst_at, st.need_lead, st.need_at);
                }
                printf("%s\t%s\t%lu\t%s\t%s\n", verdict, fmtname(st.fmt),
                       st.duration_ms, reason, line);
                fflush(stdout);
                continue;
            }

            if (st.open_error) {
                acc[idx].bad++;
                printf("[%s] %s : %s%s%s\n", fmtname(st.fmt), line,
                       gcc_strerror(st.open_error),
                       gcc_last_detail()[0] ? " -- " : "", gcc_last_detail());
            } else {
                double rt;
                if (st.rms < 1e-6) { acc[idx].silent++; printf("[SILENCE] %s\n", line); }
                acc[idx].cpu += st.cpu_seconds; acc[idx].audio += st.audio_seconds;
                rt = st.cpu_seconds > 0.0 ? st.audio_seconds / st.cpu_seconds : 1e9;
                if (rt < acc[idx].worst) {
                    acc[idx].worst = rt;
                    snprintf(acc[idx].worstname, sizeof acc[idx].worstname, "%s", line);
                }
            }
        }

        if (triage) return 0;

        printf("\n==================== summary (%lu files) ====================\n", n);
        for (k = 0; k < 4; k++) {
            if (!acc[k].files) continue;
            printf("\n%-8s : %lu files, %lu refused, %lu silent\n",
                   fmtname((gcc_format)k), acc[k].files, acc[k].bad, acc[k].silent);
            if (acc[k].cpu > 0.0) {
                printf("           mean       real time x%.1f\n", acc[k].audio / acc[k].cpu);
                printf("           worst case real time x%.1f  (%s)\n",
                       acc[k].worst, acc[k].worstname);
            }
        }
        return 0;
    }

    if (!in) {
        fprintf(stderr, "usage: gcc_render <file> [output.wav] [-r rate] [-s sec] [-sf font.sf2]\n"
                        "       gcc_render -batch  [-sf font.sf2]  (list on stdin)\n"
                        "       gcc_render -triage [-sf font.sf2]  (list on stdin, TSV)\n");
        return 1;
    }
    return render_one(in, out, rate, seconds, &st, 0);
}
