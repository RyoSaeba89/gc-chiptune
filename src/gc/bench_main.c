/* ------------------------------------------------------------------------
 * GC-Chiptune: GameCube measurement bench.
 *
 * Goal: obtain the REAL CPU load of the Gekko.
 *
 * REAL HARDWARE CAMPAIGN. The figures in docs/STATUS.md 7 come from Dolphin,
 * which is not cycle-accurate and, above all, DOES NOT EMULATE THE CACHES
 * (7.3). The console should be SLOWER than those measurements, by an unknown
 * factor. That factor is what this bench goes looking for.
 *
 * Hence a reduced matrix:
 *   - the corpus's WORST CASES, one per backend, and nothing else;
 *   - both AI frequencies, 48 kHz and 32 kHz;
 *   - polyphony UNLIMITED first, for the raw number: the 64-voice ceiling (7.7)
 *     would mask the Dolphin/console gap we want to quantify;
 *   - then the two PRODUCTION settings, measured rather than extrapolated.
 *
 * The program runs the matrix (track x frequency) ON ITS OWN and displays a
 * cumulative table. No pad: on console the measurement has to run without
 * intervention, and an automatic measurement is reproducible.
 *
 * The results are copied to the SD card at the end (dump_results), for lack of
 * a debug console on real hardware.
 *
 * We measure min / mean / max, not just the instantaneous value: load
 * fluctuates with the density of the track. It is the MAX that causes
 * underruns, and the MEAN that says whether any margin is left.
 *
 * AND THAT IS NOT ENOUGH. This bench used to measure TWELVE SECONDS of the
 * start of a track. That is what led to the conclusion "budget closed" for the
 * V2M, whose load rose much later -- see docs/STATUS.md 13. The format has been
 * removed since, but the lesson holds for the two that remain: twelve seconds
 * of an opening do not describe a track.
 *
 * The tracks are embedded in the .dol (bin2s): we want to measure the synth,
 * not debug fatfs and SD emulation at the same time.
 *
 * THE SOUNDFONT IS EMBEDDED TOO. On real hardware the SD card would work, but
 * we keep it embedded for two reasons: the figures stay directly comparable
 * with Dolphin's (same .dol, same load path), and the bench does not depend on
 * an SD driver not yet validated on console (docs/STATUS.md 8.4). Cost: the
 * .dol goes from 1.0 to 8.1 MB.
 *
 * THAT IS THE ONLY DIFFERENCE BETWEEN THE TWO BINARIES. `make` produces the
 * PLAYER (gc-chiptune.dol, 1.0 MB) which reads everything from the SD card;
 * `make APP=bench` produces THIS bench, which embeds its two worst cases and
 * the soundfont. A 1 MB player is therefore not a crippled player -- it is the
 * other target.
 *
 * MEMORY -- the thing to watch. TinySoundFont does not play samples as s16: it
 * converts them to float at load time, i.e. DOUBLE. TimGM6mb's smpl chunk is
 * 5,764,336 bytes, so 11.0 MB once loaded, on top of the 5.7 MB of the blob in
 * .data -- 16.7 MB out of the console's 24. Hence a precaution: the MIDI phases
 * come LAST, and the soundfont is only loaded from the first of them.
 *
 * It is then RELOADED for every MIDI phase: a phase leaves behind a voiceNum
 * inflated to whatever the track demanded, and the next one would inherit it.
 * Every measurement point must start from the same state.
 *
 * The tracks are played straight out of .rodata, with no copy.
 * ------------------------------------------------------------------------ */

#include <gccore.h>
#include <ogc/lwp_watchdog.h>   /* gettime, ticks_to_millisecs */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "audio_gc.h"
#include "../player/backend.h"
#include "../player/sf2_endian.h"

#include "storage.h"

#include "worst_xm.h"
#include "worst_mid.h"
#include "soundfont_sf2.h"

/* ~43 ms per buffer. A player has no latency constraint, and large buffers
 * absorb render load peaks. */
#define FRAMES_PER_BUF 2048

/* A PHASE COVERS THE WHOLE TRACK, plus a margin.
 *
 * The previous version stopped at twelve seconds. That is what led to the
 * conclusion "budget closed" for the V2M, whose load rose at the two-minute
 * mark (docs/STATUS.md 13). The two embedded tracks are 90 s (XM) and 141 s
 * (MIDI); 200 s covers them, and the phase stops by itself as soon as the track
 * is over. */
#define PHASE_MS 200000

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;
static gcc_backend *g_backend = NULL;

typedef struct {
    const char    *name;
    const void    *data;
    unsigned long  len;
} track;

/* ONLY the worst cases, one per backend, as identified by ranking the whole
 * corpus individually on the host (test/README.txt):
 *
 *   xm      DARKSiDERS - Royal Heroes intro  host x73.0
 *   midi    BACKLASH - Fighting Force        host x15.3
 *
 * The "typical" and "light" tracks were dropped from this campaign: they settle
 * nothing, and every phase costs measurement time on a console driven by hand.
 * The embedded XM is no longer data/test.xm ("typical", x34 real time measured
 * under Dolphin) but the corpus's actual worst case. */
static const track g_tracks[] = {
    { "XM worst",   worst_xm,  (unsigned long)worst_xm_size },
    { "MIDI worst", worst_mid, (unsigned long)worst_mid_size },
};

/* True if the track needs the soundfont. */
#define TRACK_IS_MIDI(i) ((i) == 1)

/* voices: MIDI polyphony limit, 0 = none. Ignored outside MIDI. */
typedef struct { int track; unsigned rate; int voices; } config;

/* Real hardware campaign: the worst cases x both AI frequencies, POLYPHONY
 * UNLIMITED throughout.
 *
 * No polyphony sweep here. Under Dolphin it served to find the useful ceiling
 * (64 voices, docs/STATUS.md 7.7); what we want from real hardware is the RAW
 * number, with no guard rail, to learn by how much the caveat of 7.3 (caches
 * not emulated) shifts the measurement. A voice limit would mask precisely what
 * we are trying to see.
 *
 * MIDI last: see the memory note at the top of the file. */
static const config g_configs[] = {
    { 0, 48000, 0 }, { 0, 32000, 0 },   /* XM   worst case */
    { 1, 48000, 0 }, { 1, 32000, 0 },   /* MIDI worst case, no voice limit */

    /* The two PRODUCTION settings, measured rather than extrapolated.
     * Extrapolation is precisely what has been expensive so far. */
    { 1, 48000, GCC_MIDI_DEFAULT_VOICES },
    { 1, 32000, GCC_MIDI_DEFAULT_VOICES },
};
#define NCONFIGS ((int)(sizeof g_configs / sizeof g_configs[0]))

typedef struct {
    unsigned      avg, max;
    unsigned      worst;      /* worst one-second window, per mille       */
    unsigned long under;
    unsigned long starved;    /* empty DSP cycles -- see gcaudio_starved  */
    int           peak;  /* observed MIDI voice peak          */
    unsigned      dsp;   /* DSP load peak, per mille          */
    int           stall; /* the DSP stalled at least once     */
    int           amp;   /* peak amplitude of the render, s16 */
    unsigned long nans;  /* non-numeric samples               */
    int           ok;
    const char   *err;   /* failure cause, NULL if ok         */
} result;

static result g_results[NCONFIGS];

static char g_sf_msg[48];

/* Peak amplitude of the render, as an absolute s16 value. */
static int g_peak_amp;

/* MEASURING RENDER TIME SAYS NOTHING ABOUT ITS VALIDITY.
 *
 * The V2M at 32 kHz was reported "ok, 756/896 per mille, 0 underruns" while it
 * was producing nothing but NaNs, converted to silence by the backend. A
 * plausible load on a silent render is the worst possible result: it reads as a
 * success.
 *
 * So we also record what COMES OUT. The cost is one integer comparison per
 * sample -- on the order of 0.5 per mille. The NaNs are counted by the backend
 * for free: the test was already there.
 *
 * End of track: two empty buffers in a row. Same rule as the player --
 * gcc_render returns 0 on the last useful sample, but the DSP is still playing
 * what is queued. */
static volatile int g_silent_bufs;

static int fill_cb(void *user, short *dst, unsigned frames)
{
    int rc, i, n2 = (int)frames * 2;

    (void)user;
    if (!g_backend) { memset(dst, 0, frames * 4); return 0; }

    rc = gcc_render(g_backend, dst, frames);
    if (rc == 0) g_silent_bufs++;
    else         g_silent_bufs = 0;

    for (i = 0; i < n2; i++) {
        int a = dst[i] < 0 ? -dst[i] : dst[i];
        if (a > g_peak_amp) g_peak_amp = a;
    }
    return rc;
}

/* The "voices" column: requested limit / peak actually observed. "-" outside
 * MIDI. The two numbers together say the essential -- a limit above the peak is
 * useless, a limit below it is paid for in cut notes. */
static void voices_label(char *buf, size_t n, const config *c,
                         int peak, int known)
{
    if (!TRACK_IS_MIDI(c->track)) { snprintf(buf, n, "%s", "-"); return; }
    if (!known)                   { snprintf(buf, n, "%d/?", c->voices); return; }
    if (c->voices == 0)             snprintf(buf, n, "inf/%d", peak);
    else                            snprintf(buf, n, "%d/%d", c->voices, peak);
}

/* THE VERDICT SAYS FIRST WHETHER THE RENDER IS VALID, THEN WHETHER IT HOLDS
 * REAL TIME. The order is not cosmetic: it is exactly the reverse ordering that
 * let the V2M through at 32 kHz, where a perfectly plausible load (756/896,
 * zero underruns) crowned a render that was entirely NaN. A CPU number over
 * silence is not a result, it is a trap. */
static const char *verdict(const result *r)
{
    if (!r->ok)         return r->err ? r->err : "FAILED";
    if (r->nans)        return "NaN, RENDER WRONG";
    if (r->amp < 256)   return "SILENT";        /* -42 dBFS */
    /* Same logic as the two lines above: a stalled DSP returns a load of 0 per
     * mille, i.e. the best value in the table. Say it BEFORE talking about real
     * time. */
    if (r->stall)       return "DSP STALLED";
    if (r->max >= 1000) return "over real time";
    if (r->under)       return "starved";
    if (r->avg >= 800)  return "marginal";
    return "ok";
}

static void draw(int current)
{
    int  i;
    char v[12];

    printf("\x1b[1;1H");
    printf("GC-Chiptune -- CPU load, REAL HARDWARE\n");
    printf("corpus worst cases, polyphony NOT limited\n");
    printf("=====================================================================\n");
    printf("%-14s %4s %7s %5s %5s %5s %6s %5s\n",
           "track", "kHz", "voices", "avg", "worst", "max", "dry", "dsp");
    printf("---------------------------------------------------------------------\n");

    for (i = 0; i < NCONFIGS; i++) {
        const config *c = &g_configs[i];
        /* Fixed width is mandatory: libogc's console does not clear to end of
         * line on redraw, so a shorter label would leave the tail of the
         * previous one behind ("ok" over "<== running" gave "ok= running"). */
        const char *status;
        if (i > current) {
            voices_label(v, sizeof v, c, 0, 0);
            printf("%-14s %4u %7s %5s %5s %5s %6s %5s  %-14s\n",
                   g_tracks[c->track].name, c->rate / 1000, v,
                   "-", "-", "-", "-", "-", "");
            continue;
        }
        if (i == current) {
            status = "<== running";
            voices_label(v, sizeof v, c, gcc_midi_voices(), 1);
            printf("%-14s %4u %7s %5u %5s %5u %6lu %5u  %-14s\n",
                   g_tracks[c->track].name, c->rate / 1000, v,
                   gcaudio_load_avg(), "-", gcaudio_load_max(),
                   gcaudio_starved(), gcaudio_dsp_permille(), status);
        } else {
            const result *r = &g_results[i];
            status = verdict(r);
            voices_label(v, sizeof v, c, r->peak, r->ok);
            printf("%-14s %4u %7s %5u %5u %5u %6lu %5u  %-14s\n",
                   g_tracks[c->track].name, c->rate / 1000, v,
                   r->avg, r->worst, r->max, r->starved, r->dsp, status);
        }
    }
    printf("---------------------------------------------------------------------\n");
    printf("load in per mille of real time; 1000 = hard limit\n");
    printf("worst = worst ONE-SECOND window, not the phase mean\n");
    printf("dry = DSP cycles with no block ready: THIS is what sees a dropout\n");
}

/* Copies the final table to the SD card.
 *
 * On real hardware there is neither a debug console nor a Dolphin dump: the
 * only way to record the numbers would be to photograph the screen. A text file
 * makes them usable as they are.
 *
 * Best effort, and AFTER all the measurements: a mount failure then has no
 * effect on them. Incidentally it is the first real exercise of the SD driver
 * (docs/STATUS.md 8.4 -- not emulated by Dolphin), hence displaying the carrier
 * that was selected.
 *
 * Returns a label to display, never NULL. */
static const char *dump_results(void)
{
    static char msg[64];
    char        path[64];
    char        src[32];
    FILE       *f;
    int         i;

    if (gcs_mount() != 0) return "SD card: none (numbers on screen)";

    /* gcs_unmount() clears the carrier label: copy it first. */
    snprintf(src, sizeof src, "%s", gcs_source_name());

    snprintf(path, sizeof path, "%s/gc-chiptune-bench.txt", gcs_device());
    f = fopen(path, "w");
    if (!f) {
        gcs_unmount();
        snprintf(msg, sizeof msg, "SD %s: write refused", src);
        return msg;
    }

    fprintf(f, "GC-Chiptune -- CPU load, REAL HARDWARE\n");
    fprintf(f, "corpus worst cases, polyphony NOT limited, %d s per phase\n",
            PHASE_MS / 1000);
    fprintf(f, "load in per mille of real time; 1000 = hard limit\n");
    fprintf(f, "libansnd output: AI pinned at 48 kHz, samples read from ARAM,\n");
    fprintf(f, "mixing and (sinc) resampling on the DSP\n");
    fprintf(f, "worst = worst ONE-SECOND window, not the phase mean\n");
    fprintf(f, "dry = DSP cycles where the ring was empty (gcaudio_starved) --\n");
    fprintf(f, "      THIS is the counter that sees a dropout, not 'end'\n");
    fprintf(f, "end = the voice stopped (FINISHED); dsp = DSP load\n");
    fprintf(f, "peak = s16 amplitude of the render (32767 max); NaN = wrong render\n");
    fprintf(f, "a load verdict is only worth anything if peak > 0 and NaN = 0\n");
    fprintf(f, "SD carrier: %s\n\n", src);
    fprintf(f, "%-14s %5s %6s %6s %6s %6s %7s %6s %5s %6s %9s  %s\n",
            "track", "kHz", "voices", "avg", "worst", "max", "dry", "end",
            "dsp", "peak", "NaN", "verdict");

    for (i = 0; i < NCONFIGS; i++) {
        const config *c = &g_configs[i];
        const result *r = &g_results[i];
        char          v[12];

        voices_label(v, sizeof v, c, r->peak, r->ok);

        fprintf(f, "%-14s %5u %6s %6u %6u %6u %7lu %6lu %5u %6d %9lu  %s\n",
                g_tracks[c->track].name, c->rate, v,
                r->avg, r->worst, r->max, r->starved, r->under, r->dsp,
                r->amp, r->nans, verdict(r));
    }

    fclose(f);
    gcs_unmount();

    snprintf(msg, sizeof msg, "written to SD (%s)", src);
    return msg;
}

int main(int argc, char **argv)
{
    int ci;

    (void)argc; (void)argv;

    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb   = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    /* Non-IEEE mode: denormals treated as zero. Resonant filters produce them
     * at the end of a release, and handling them in software is a classic
     * performance killer on the PPC 750. Set before any rendering. */
    gcaudio_flush_denormals();

    printf("\x1b[2J");

    for (ci = 0; ci < NCONFIGS; ci++) {
        const config *c = &g_configs[ci];
        u64 t_start, t_draw;
        int rc;

        /* The soundfont is (re)loaded for EVERY MIDI phase, not once for all.
         * Two reasons:
         *
         *  - tsf_set_max_voices can only RAISE the limit on an already loaded
         *    soundfont (tsf.h:1543). Without a reload, a descending 64 -> 16
         *    sweep would measure a single value.
         *  - even without that, a phase with no limit leaves voiceNum at
         *    whatever the track demanded; the next phase would inherit it.
         *    Every measurement point must start from the same state.
         *
         * It therefore only exists in memory during the MIDI phases, which are
         * the last ones -- see the memory note at the top of the file. */
        if (TRACK_IS_MIDI(c->track)) {
            gcc_midi_set_max_voices(c->voices);
            /* Two possible causes of failure, and they must be separated:
             *
             *  - ENDIANNESS: an .sf2 is a RIFF container, hence little-endian,
             *    and tsf.h reads its fields by raw copy. A soundfont that has
             *    not been through tools/sf2_prep cannot work here
             *    (docs/STATUS.md 7.5);
             *  - memory: it asks for 11.0 MB in one piece.
             *
             * So we name the cause instead of displaying "FAILED". */
            switch (sf2_check(soundfont_sf2, (size_t)soundfont_sf2_size)) {
            case SF2_STATE_RAW:
                snprintf(g_sf_msg, sizeof g_sf_msg, "sf2 not prepared");
                break;
            case SF2_STATE_UNKNOWN:
                snprintf(g_sf_msg, sizeof g_sf_msg, "sf2 unreadable");
                break;
            default:
                snprintf(g_sf_msg, sizeof g_sf_msg, "sf2 failed, %lu kB free",
                         (unsigned long)SYS_GetArena1Size() / 1024);
                break;
            }
            if (gcc_midi_set_soundfont(soundfont_sf2,
                                       (size_t)soundfont_sf2_size) != GCC_OK) {
                g_results[ci].ok  = 0;
                g_results[ci].err = g_sf_msg;
                continue;
            }
        }

        if (gcaudio_init(c->rate, FRAMES_PER_BUF, fill_cb, NULL) != 0) {
            g_results[ci].ok  = 0;
            g_results[ci].err = "audio failed";
            continue;
        }

        rc = gcc_open(g_tracks[c->track].data, g_tracks[c->track].len,
                      c->rate, GCC_FMT_UNKNOWN, &g_backend);
        if (rc != GCC_OK) {
            g_results[ci].ok  = 0;
            g_results[ci].err = gcc_strerror(rc);
            gcaudio_shutdown();
            continue;
        }

        gcaudio_stats_reset();
        gcc_render_stats_reset();
        g_peak_amp    = 0;
        g_silent_bufs = 0;
        gcaudio_start();

        g_results[ci].peak  = 0;
        g_results[ci].dsp   = 0;
        g_results[ci].stall = 0;

        t_start = gettime();
        t_draw  = 0;
        g_results[ci].worst = 0;
        {
        u64      t_win  = t_start;
        unsigned win_hi = 0;

        while (ticks_to_millisecs(gettime() - t_start) < PHASE_MS) {
            int      nv;
            unsigned dsp, inst;
            u64      now;

            gcaudio_service();   /* rendering happens here, not in the ISR */

            /* WORST ONE-SECOND WINDOW, not just the phase mean. A low mean can
             * hide ten seconds above real time; that is exactly what
             * happened. */
            inst = gcaudio_load_permille();
            if (inst > win_hi) win_hi = inst;
            if (ticks_to_millisecs(gettime() - t_win) >= 1000) {
                if (win_hi > g_results[ci].worst) g_results[ci].worst = win_hi;
                win_hi = 0;
                t_win  = gettime();
            }

            /* The track is over: no point measuring silence until PHASE_MS. */
            if (g_silent_bufs >= 2) break;

            nv = gcc_midi_voices();
            if (nv > g_results[ci].peak) g_results[ci].peak = nv;

            if (gcaudio_dsp_stalled()) g_results[ci].stall = 1;
            dsp = gcaudio_dsp_permille();
            if (dsp > g_results[ci].dsp) g_results[ci].dsp = dsp;

            /* THE BENCH MUST NOT COMPETE WITH WHAT IT MEASURES.
             *
             * The previous version redrew the whole table -- about twenty
             * console printfs, with glyph rendering into the XFB -- then waited
             * for vsync, ON EVERY FRAME, between two gcaudio_service() calls.
             * On console some of the underruns recorded came from that, not
             * from the synths: the measured load maximum stayed under 1000, so
             * no block had exceeded real time.
             *
             * Four refreshes per second are enough to follow a measurement, and
             * the vsync is gone: nothing must block the loop. */
            now = gettime();
            if (ticks_to_millisecs(now - t_draw) >= 250) {
                draw(ci);
                t_draw = now;
            }
        }
        if (win_hi > g_results[ci].worst) g_results[ci].worst = win_hi;
        }

        g_results[ci].avg     = gcaudio_load_avg();
        g_results[ci].max     = gcaudio_load_max();
        g_results[ci].under   = gcaudio_underruns();
        g_results[ci].starved = gcaudio_starved();
        g_results[ci].amp     = g_peak_amp;
        g_results[ci].nans  = gcc_render_nans();
        g_results[ci].ok    = 1;

        gcaudio_stop();
        gcaudio_shutdown();
        gcc_close(g_backend);
        g_backend = NULL;
    }

    draw(NCONFIGS);
    printf("\nMeasurement complete. %-40s\n", dump_results());
    printf("Screen frozen: record the numbers, then switch off.\n");

    while (1) VIDEO_WaitVSync();
    return 0;
}
