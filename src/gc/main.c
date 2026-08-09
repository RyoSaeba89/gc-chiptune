/* ------------------------------------------------------------------------
 * GC-Chiptune: the player.
 *
 * Chain of responsibility: SD card -> index -> scope -> playback.
 *
 *   storage.c   mounts the card
 *   index.c     caches the walk and the durations (so we do not relist 4937
 *               files on every power-on)
 *   playlist.c  recursive walk, used when the cache is missing
 *   library.c   scope, order, modes -- all the "what plays next" logic
 *   ui.c        the two panes, in GX
 *   state.c     resume after power-off
 *
 * ONE SINGLE LOOP. It services the audio, reads the pad and draws, in that
 * order, sixty times per second -- GRRLIB_Render() waits for the retrace.
 *
 * That is the structural change from the console-output version, which had two
 * screens and TWO loops: the browser loop closed the audio output on entry, so
 * browsing stopped the music. Both panes are now visible at once (see ui.h), so
 * there is no longer any reason to stop anything in order to look at the list.
 *
 * NO FIXED DURATION CAP. The old version cut everything at 3 minutes; measured
 * over the pack, that truncated 1212 tracks out of 5065, i.e. 24 %. Both
 * formats know where they stop -- gcc_render() returns 0 on the last useful
 * sample -- and gcc_duration_ms() gives the length as soon as the file opens.
 * The cap is therefore only a WATCHDOG scaled to the announced duration: it
 * truncates nothing legitimate and merely stops the list from wedging on a
 * pathological file.
 *
 * RESPONSIVENESS IS A HARD REQUIREMENT, NOT A NICETY. Everything the loop does
 * between two pad reads is latency the user feels in the list. Three rules
 * follow, and they are enforced in code rather than described in a comment:
 *
 *   1. the audio never renders more than AUDIO_BLOCKS_PER_PASS blocks in one
 *      pass, and the pad is read BETWEEN two blocks (see service_audio);
 *   2. nothing that touches the SD card runs while the ring is short;
 *   3. holding a direction scrolls (see nav_repeat) -- a 4937-entry library is
 *      unusable one press at a time.
 * ------------------------------------------------------------------------ */

#include <gccore.h>
#include <ogc/lwp_watchdog.h>   /* gettime, ticks_to_millisecs */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <dirent.h>

#include "audio_gc.h"
#include "storage.h"
#include "playlist.h"
#include "library.h"
#include "index.h"
#include "state.h"
#include "ui.h"
#include "../player/backend.h"
#include "../player/sf2_endian.h"

/* ~43 ms per buffer at 48 kHz. A player has no latency constraint, and large
 * buffers absorb the peaks of a dense module. */
#define FRAMES_PER_BUF 2048

/* Blocks rendered per pass through the main loop.
 *
 * ONE would be enough on paper -- 60 passes per second against 23 blocks
 * consumed -- but only while the loop actually runs at 60 Hz. It does not on a
 * heavy track, and at 20 passes per second one block per pass no longer keeps
 * up. TWO holds down to 12 passes per second and still bounds a single pass to
 * two renders.
 *
 * The bound is the point. The previous version filled the ring to the brim in
 * one go (`while (produce())`), which is fine in steady state and catastrophic
 * right after any hiccup: the ring was empty, so one pass rendered four blocks
 * back to back, and on a track heavier than real time it never got out of that
 * state. The loop then ran at four or five passes per second -- the pad was
 * read four times a second, and the list looked frozen. That is the reported
 * fault. */
#define AUDIO_BLOCKS_PER_PASS 2

/* Watchdog: announced duration + 25 % + 5 s. The margin covers the only known
 * discrepancy -- a MIDI plays one or two seconds past its last event, while the
 * releases die out. */
#define WATCHDOG_MS(d)  ((d) ? ((d) + (d) / 4u + 5000u) : 1800000u)

/* How long a rejection reason stays on screen before we move on. 1.2 s did not
 * leave time to read it. */
#define SKIP_NOTE_MS 3000

/* End-of-track drain. gcc_render() returns 0 on the last useful sample, but at
 * that moment the ring still holds several blocks of real music that the DSP
 * has not played yet. Cutting immediately clipped the last ~200 ms of every
 * track. We wait for the ring to empty, plus a margin for the stages libansnd
 * holds internally. */
#define DRAIN_MS 250

/* THE DRAWING MUST NEVER COST THE DECODER.
 *
 * First attempt: cap the redraw rate, then redraw only on change. Both failed
 * in use -- decoding still slowed down while scrolling the list, even on an XM
 * at 12 per mille -- and the second produced a screen that looked frozen, since
 * it stopped refreshing entirely as long as nothing was pressed.
 *
 * Both were knobs around the real problem: text was expensive.
 * GRRLIB_PrintfTTF rasterises every character on every call, with no cache at
 * all. The answer is in font_gx.c -- a glyph atlas built once -- after which a
 * frame is nothing but a pile of quads for the GPU.
 *
 * So we redraw EVERY FRAME, with no state condition. That is what keeps the
 * screen alive (the level meter moves continuously) and what removes the
 * "the picture stopped refreshing" class of fault.
 *
 * One guard remains: if the audio has no lead left, we skip the frame. But
 * never for longer than REDRAW_MAX_SKIP_MS -- a decoder that is permanently
 * slower than real time must not blank the screen, which would read as a
 * crash. Sound first, but the screen always comes back. */
#define REDRAW_MIN_LEAD      1     /* blocks of lead required, of GCAUDIO_LEAD_MAX */
#define REDRAW_MAX_SKIP_MS 250     /* never go longer than this without a frame  */

/* Auto-repeat on the d-pad. Without it, moving through a folder of two hundred
 * entries takes two hundred presses -- which is exactly what "the menu is
 * extremely slow" describes. */
#define REPEAT_DELAY_MS 350        /* hold this long before it starts repeating */
#define REPEAT_RATE_MS   55        /* then one step every                        */

/* How often the memory readout is refreshed. Probing the largest free block
 * costs a handful of allocations, and repeating that sixty times a second
 * would stir the very heap we are trying to observe. Once a second is plenty
 * to WATCH the memory go down over a session, which is all these two numbers
 * are for. */
#define RAM_POLL_MS 1000

/* Minimum delay between two writes of the resume file. The C-stick used to
 * call save_state() on every volume step, i.e. an SD write (temp file, remove,
 * rename) every 150 ms while the stick was held. On libfat that is tens of
 * milliseconds of blocked main loop each time -- it starved the audio and
 * stalled the list. The state is now marked dirty and flushed when the ring is
 * full and enough time has passed. */
#define STATE_SAVE_MIN_MS 4000

static const char * const g_roots[] = {
    "sd:/chiptunes", "sd:/GC-Chiptune", "sd:/music", "sd:/", NULL
};

/* Directory the .dol was loaded from, derived from argv[0]. See note_dol_dir(). */
static char g_dol_dir[128];

static gcc_backend *g_backend  = NULL;
/* The soundfont is loaded ON DEMAND, on the first .mid encountered: 11 MB once
 * TinySoundFont has converted it to float, and there is no point tying that up
 * when the list has no MIDI in it. The source block is released as soon as the
 * load is done -- see ensure_soundfont(). */
static int          g_sf_loaded = 0;
static int          g_sf_tried  = 0;
static const char  *g_sf_why    = NULL;
/* Retry threshold after a refused load. See ensure_soundfont(). */
static unsigned long g_sf_retry_above = 0;

/* End of track. Rendering happens in gcaudio_service_step(), so outside
 * interrupt context: a counter is enough. Two empty buffers in a row mean the
 * end, not one -- gcc_render returns 0 as soon as the track is over but the DSP
 * is still playing what is queued. */
static volatile int g_silent_bufs = 0;

/* Peak of the last block, per channel, for the level meter. Computed here
 * because this is the only place the samples pass through -- a sweep over 2048
 * frames already in cache, negligible next to the render that just produced
 * them. */
static volatile unsigned char g_vu_l, g_vu_r;

static int fill_cb(void *user, short *dst, unsigned frames)
{
    int      produced;
    unsigned i;
    int      pl = 0, pr = 0;

    (void)user;
    if (!g_backend) { memset(dst, 0, frames * 4); g_vu_l = g_vu_r = 0; return 0; }

    produced = gcc_render(g_backend, dst, frames);
    if (produced == 0) g_silent_bufs++;
    else               g_silent_bufs = 0;

    for (i = 0; i < frames; i++) {
        int l = dst[2 * i], r = dst[2 * i + 1];
        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if (l > pl) pl = l;
        if (r > pr) pr = r;
    }
    /* 0..32768 -> 0..255. The clamp is not decorative: a full-scale negative
     * sample gives 32768 after the absolute value, and 32768 >> 7 is 256, which
     * wraps to 0 in a byte -- the meter would read silence on the loudest
     * possible block. */
    pl >>= 7; if (pl > 255) pl = 255;
    pr >>= 7; if (pr > 255) pr = 255;
    g_vu_l = (unsigned char)pl;
    g_vu_r = (unsigned char)pr;

    return produced;
}

/* Scan root, settled in main(). Declared here because the soundfont search
 * uses it. */
static const char *g_root = NULL;

/* Free memory in bytes. Defined further down; announced here because the
 * soundfont is the program's largest consumer and says so. */
static unsigned long free_ram(void);
static unsigned long largest_free_block(void);

/* Builds "<root>/<name>", inserting the separator only when the root does not
 * already end with one. Without this, a root of "sd:/" produced
 * "sd://gc-chiptune.idx". */
static void join_root(char *dst, size_t cap, const char *name)
{
    size_t n = g_root ? strlen(g_root) : 0;
    snprintf(dst, cap, "%s%s%s", g_root ? g_root : "sd:",
             (n && g_root[n - 1] == '/') ? "" : "/", name);
}

/* ------------------------------------------------------------- soundfont */

/* Remembers the directory the .dol was loaded from, so we can look for the
 * soundfont there: it is the natural place, and it avoids mixing a 5.7 MB file
 * in with the music.
 *
 * argv[0] only exists if the loader provides it -- Swiss does, a chip or a
 * minimal launcher may not, hence the fallback to the other locations. And its
 * DEVICE NAME is not ours: Swiss mounts its own volumes and may announce
 * "fat:/..." where we mount "sd:". Only the path interests us, so we re-anchor
 * it. */
static void note_dol_dir(int argc, char **argv)
{
    const char *p, *colon, *slash;

    g_dol_dir[0] = 0;
    if (argc < 1 || !argv || !argv[0]) return;

    p     = argv[0];
    colon = strchr(p, ':');
    if (colon) p = colon + 1;
    while (*p == '/') p++;

    slash = strrchr(p, '/');
    if (!slash) return;          /* .dol at the root: sd:/ is already tried */

    snprintf(g_dol_dir, sizeof g_dol_dir, "sd:/%.*s/", (int)(slash - p), p);
}

/* State of a soundfont candidate, read WITHOUT loading the file: the first
 * twelve bytes and the size are all sf2_check() needs. That is enough to tell
 * "absent", "raw" and "ready" apart -- no reason to tie up 5.7 MB to answer
 * that question. */
static int sf2_probe(const char *path, sf2_state *out)
{
    unsigned char head[12];
    long          size;
    FILE         *f = fopen(path, "rb");

    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    size = ftell(f);
    rewind(f);

    if (size <= 12 || fread(head, 1, sizeof head, f) != sizeof head) {
        fclose(f);
        return -1;
    }
    fclose(f);

    *out = sf2_check(head, (size_t)size);
    return 0;
}

static int ensure_soundfont(void)
{
    /* g_sf_why points at this: it has to survive the return. */
    static char why[160];
    char        at_dol[192], at_root[192];
    const char *cand[5];
    unsigned long before;
    int         i, n = 0;

    if (g_sf_loaded) return 0;
    /* g_sf_tried is only set on DEFINITIVE failures: file absent, raw,
     * unreadable. The previous version set it before even trying, so any
     * failure -- including a transient memory shortage, the one case that can
     * clear up -- condemned MIDI for the whole session. */
    if (g_sf_tried)  return -1;

    before = free_ram();
    if (g_sf_retry_above && before <= g_sf_retry_above) return -1;

    /* The .dol's directory first: that is where the user expects to drop a file
     * that belongs to the program rather than to their music. Then the usual
     * locations. */
    if (g_dol_dir[0]) {
        snprintf(at_dol, sizeof at_dol, "%ssoundfont.sf2", g_dol_dir);
        cand[n++] = at_dol;
    }
    cand[n++] = "sd:/soundfont.sf2";
    join_root(at_root, sizeof at_root, "soundfont.sf2");
    cand[n++] = at_root;
    cand[n++] = "sd:/chiptunes/soundfont.sf2";
    cand[n++] = "sd:/GC-Chiptune/soundfont.sf2";

    snprintf(why, sizeof why, "MIDI skipped: no soundfont.sf2 in %s", cand[0]);
    g_sf_why = why;

    for (i = 0; i < n; i++) {
        sf2_state state;
        int       rc;

        if (sf2_probe(cand[i], &state) != 0) continue;

        /* Neither of these will ever clear up on its own: the file is at fault,
         * not the state of the machine. Latch permanently. */
        switch (state) {
        case SF2_STATE_RAW:
            g_sf_why   = "soundfont not prepared: run it through tools/sf2_prep";
            g_sf_tried = 1;
            return -1;
        case SF2_STATE_UNKNOWN:
            g_sf_why   = "soundfont unreadable";
            g_sf_tried = 1;
            return -1;
        default: break;
        }

        /* READ STRAIGHT OFF THE CARD, never loaded whole.
         *
         * The previous version read the 5.7 MB file into RAM and then called
         * tsf_load_memory(), which allocates a further 11.0 MB of float
         * samples on top: a 16.7 MB peak, 11.0 of it contiguous, on an arena of
         * about 21.5 MB already eaten into by GX, the glyph atlases and the
         * index. It worked or it did not depending on heap fragmentation --
         * hence a "soundfont refused (memory)" that reproduced on some cards
         * and not on others.
         *
         * tsf_load() only reads its input forwards: the file has no reason to
         * be in memory at all. The peak drops back to 11.0 MB. */
        rc = gcc_midi_set_soundfont_file(cand[i]);
        if (rc == GCC_OK) {
            g_sf_loaded = 1;
            /* The threshold belonged to a shortage that is now over. Left
             * standing it would gate the NEXT load -- the one after a
             * release -- on a number measured in another era of the heap. */
            g_sf_retry_above = 0;
            snprintf(why, sizeof why, "soundfont loaded: %lu -> %lu kB free",
                     before / 1024u, free_ram() / 1024u);
            g_sf_why = why;
            return 0;
        }

        /* SAY WHAT FAILED, AND WITH WHAT NUMBERS.
         *
         * The old message asserted "out of memory?" whatever the reason --
         * including a malformed file, where tsf_load returns NULL without
         * having allocated anything. Asserting a cause you have not measured is
         * exactly the habit this project has already paid for three times
         * (docs/STATUS.md 12.18). So we report the return code AND the free
         * memory, and let the reader judge. */
        snprintf(why, sizeof why, "soundfont refused (%s, %lu kB free)",
                 rc == GCC_ERR_MEMORY ? "voices" : "load",
                 free_ram() / 1024u);
        g_sf_why  = why;

        /* WE DO NOT CONDEMN MIDI FOR THE SESSION. tsf_load() returns NULL both
         * on a malformed file -- definitive -- and on a refused malloc --
         * transient, since a large module frees several MB later. We do not
         * know which, so we remember the free memory at that moment and only
         * retry once it has risen: a broken file is retried only now and then,
         * and a genuine shortage recovers by itself. */
        g_sf_retry_above = free_ram();
        return -1;
    }

    /* None of the five locations exists: nothing to expect from a retry. */
    g_sf_tried = 1;
    return -1;
}

/* GIVES THE SOUNDFONT'S 11.5 MB BACK. The counterpart to ensure_soundfont(),
 * and for a long time the missing half of it.
 *
 * THE FAULT IT FIXES. The soundfont was taken on the first `.mid` and kept
 * until the console was switched off -- 11.5 MB of an arena of about 21.5,
 * planted wherever the heap happened to have room at that instant. It is what
 * §14.4 recorded as an open point and what §16.2 named as the second source of
 * fragmentation, after the per-track buffer. On a pack that is 97.8 % modules
 * (110 `.mid` out of 4937) the player therefore spent almost its whole session
 * carrying a soundfont it was not using, and the largest free block collapsed
 * around it.
 *
 * IT REALLY DOES COME BACK -- measured rather than assumed, since this project
 * has already paid three times for the other habit. On the host, five
 * load/close cycles over TimGM6mb: +11832 kB on load, back to within 48-124 kB
 * of the baseline after the close. tsf_close() returns the presets, the regions
 * and the float samples.
 *
 * WHAT IT COSTS. The next `.mid` pays for a reload: 5.7 MB read from the card
 * plus the conversion to float. Several seconds on an SD Gecko, and the loop is
 * blocked throughout -- hence the frame drawn before it in track_open(), so the
 * wait says what it is instead of looking like the freeze of §19.
 *
 * WHY THAT TRADE IS THE RIGHT WAY ROUND. Consecutive MIDIs pay nothing: the
 * release only happens when a non-MIDI track comes along. What it costs is one
 * load per ISOLATED `.mid`, which is the very case where the soundfont would
 * otherwise sit unread for the next forty-five tracks.
 *
 * `g_sf_tried` is NOT cleared: it only latches on faults that belong to the
 * file (absent, raw, unreadable), and none of those clear up because we freed
 * some memory. */
static void soundfont_release(void)
{
    if (!g_sf_loaded) return;
    gcc_midi_free_soundfont();
    g_sf_loaded      = 0;
    g_sf_retry_above = 0;
}

/* ------------------------------------------------------------ global state */

static playlist     g_pl;
static library      g_lb;
static track_index  g_ix;
static ui_browser   g_br;
static player_state g_st;

static char        g_idx_file[128];
static char        g_state_file[128];

/* The current track. `g_data` is the file we read. It is not referenced after
 * gcc_open() -- libxmp and TinyMidiLoader both copy what they need -- but it is
 * kept anyway, because it is the one buffer we reuse.
 *
 * ONE BUFFER, WHICH NEVER SHRINKS. This is the fix for the "after a while no
 * song starts any more, memory full" fault.
 *
 * The previous version took a fresh block per track (memalign) and returned it
 * at the end. Over this pack the sizes run from 10 kB to 2.7 MB, and
 * alternating small and large blocks is precisely the recipe for a holed heap:
 * libogc's allocator returns nothing to the system, so after a few dozen tracks
 * there can be ten megabytes free without a single contiguous three. The total
 * on display stayed reassuring -- hence a "cannot read (N kB free)" with a
 * comfortable N.
 *
 * With a single buffer that only ever grows, the peak is the same (that of the
 * largest track played) but there is only ONE block, taken once and never
 * returned: no churn, so no fragmentation possible from this side. Same
 * reasoning as for the ARAM (audio_gc.c) and for the libansnd voice -- remove
 * the unstable state instead of managing it. */
static int           g_playing    = 0;
static void         *g_data       = NULL;
static size_t        g_data_cap   = 0;
static unsigned long g_watchdog   = 0;
static u64           g_last_tick  = 0;
static ui_now        g_now;
static char          g_note[96];
static u64           g_note_since = 0;

/* Start of the end-of-track drain, 0 while the track is still producing. */
static u64           g_drain_since = 0;

/* Resume file, written lazily. See STATE_SAVE_MIN_MS. */
static int           g_state_dirty = 0;
static u64           g_state_saved = 0;

/* Entropy seed. Without it, the same random bag on every power-on: the draw
 * would be reproducible, hence not a draw. gettime() on the user's first input
 * is the only source available here. */
static void seed_from_user(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    lib_set_seed(&g_lb, (unsigned)(gettime() & 0xFFFFFFFFu));
}

static void save_state_now(void)
{
    int cur = lib_current(&g_lb);
    const char *path = (cur >= 0) ? pl_path(&g_pl, cur) : NULL;

    snprintf(g_st.track, sizeof g_st.track, "%s", path ? path : "");
    snprintf(g_st.scope, sizeof g_st.scope, "%s", g_lb.prefix);
    g_st.recursive = g_lb.recursive;
    g_st.order     = g_lb.order_mode;
    g_st.repeat    = g_lb.repeat;
    g_st.seed      = g_lb.seed;
    g_st.volume    = gcaudio_volume();

    st_save(&g_st, g_state_file);
    g_state_dirty = 0;
    g_state_saved = gettime();
}

/* Marks the resume file as needing a write. Never writes here: this is called
 * from the input handlers, and an SD write in that path blocks the loop long
 * enough to starve the audio ring. */
static void save_state_later(void) { g_state_dirty = 1; }

/* Writes the resume file if it is dirty, enough time has passed, and the audio
 * has a full ring to survive the write. */
static void save_state_maybe(u64 nowt)
{
    if (!g_state_dirty) return;
    if (g_state_saved &&
        ticks_to_millisecs(nowt - g_state_saved) < STATE_SAVE_MIN_MS) return;
    if (g_playing && gcaudio_lead() < GCAUDIO_LEAD_MAX) return;
    save_state_now();
}

/* FREE MEMORY, in bytes.
 *
 * Two sources to add up on libogc: what malloc keeps free in its heap
 * (fordblks) and what it has not yet taken from the arena (Hi - Lo).
 *
 * Why this counter exists: the soundfont weighs 5.7 MB on disk AND 11.0 MB once
 * loaded -- TinySoundFont converts every sample to float (tsf.h,
 * tsf_load_samples). With the .dol resident, that leaves little room on a 24 MB
 * machine, and the rest has to cover GX, the glyph atlases, the index and the
 * current file -- the largest module in the pack is 2.7 MB.
 *
 * Too tight to estimate: we measure it. */
static unsigned long free_ram(void)
{
    struct mallinfo mi = mallinfo();
    unsigned long   arena = (unsigned long)((char *)SYS_GetArena1Hi() -
                                            (char *)SYS_GetArena1Lo());
    return (unsigned long)mi.fordblks + arena;
}

/* LARGEST CONTIGUOUS BLOCK, in bytes. THIS is what decides whether the next
 * track loads, not free_ram().
 *
 * The player answered "cannot read (N kB free)" with a comfortable N while no
 * track would start any more. Both facts are compatible, and that is the whole
 * problem: track_read() asks for the file in ONE PIECE (memalign), and a heap
 * holed by alternating 10 kB and 2.7 MB blocks -- plus the 11 MB of soundfont
 * planted in the middle -- can have ten megabytes free without a single
 * contiguous three.
 *
 * mallinfo() does not publish that value, so we probe for it: a bisection over
 * what malloc accepts, each attempt returned immediately.
 *
 * THE GRAIN IS 64 kB, NOT ONE BYTE. Byte resolution meant twenty-four
 * multi-megabyte allocations per probe; the answer is displayed in kilobytes
 * and nobody reads its last digit. Eight allocations is enough. */
static unsigned long largest_free_block(void)
{
    /* BOUND TAKEN FROM free_ram(), NOT FROM A CONSTANT.
     *
     * The first version capped at 32 MB "to be safe". On console it returned
     * exactly 32768 kB -- its own bound -- while the free total was 13193 kB.
     * In other words malloc() accepted 32 MB on a machine that has 24: the
     * probe measured nothing, it echoed its own ceiling.
     *
     * An instrument that returns its own bound is worse than no instrument: it
     * reads as "all is well". So we cap at what is genuinely free; beyond that
     * the answer cannot be true anyway. And if the bisection comes back stuck
     * to the bound, it shows -- the two displayed numbers will be equal. */
    const unsigned long grain = 64u * 1024u;
    unsigned long lo = 0, hi = free_ram() / grain;

    while (lo < hi) {
        unsigned long mid = lo + (hi - lo + 1) / 2;
        void         *p   = malloc(mid * grain);
        if (p) { free(p); lo = mid; }
        else   { hi = mid - 1; }
    }
    return lo * grain;
}

static void pause_ms(unsigned ms)
{
    u64 t = gettime();
    while (ticks_to_millisecs(gettime() - t) < ms) VIDEO_WaitVSync();
}

/* --------------------------------------------------------------- playback */

static void track_close(void)
{
    if (!g_playing) return;
    gcaudio_stop();
    gcaudio_shutdown();
    gcc_close(g_backend);
    g_backend = NULL;
    /* g_data IS NOT FREED: it is the buffer reused from one track to the next.
     * See its declaration. The backend has just been closed, nobody references
     * it any more. */
    g_playing = 0;
    g_drain_since = 0;

    memset(&g_now, 0, sizeof g_now);
}

/* Loads the track into the shared buffer, growing it if needed. Returns 0 and
 * sets *len, or -1.
 *
 * We never return the buffer and never shrink it: the only case where it grows
 * is a track larger than every previous one, which happens a handful of times
 * per session at most. */
static int track_read(const char *path, size_t *len)
{
    FILE  *f;
    long   size;

    *len = 0;
    if (!path) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    size = ftell(f);
    rewind(f);
    if (size <= 0) { fclose(f); return -1; }

    if ((size_t)size > g_data_cap) {
        /* memalign rather than malloc: the block goes to the backends as-is,
         * and 32 bytes is the Gekko cache line.
         *
         * The old buffer is released BEFORE asking for the new one. Asking
         * first would be safer in principle -- we would keep the old one if the
         * new one failed -- but it would require both at once, which is exactly
         * the peak we are trying to remove. */
        void *nb;
        free(g_data);
        g_data = NULL; g_data_cap = 0;

        nb = memalign(32, (size_t)size);
        if (!nb) { fclose(f); return -1; }
        g_data     = nb;
        g_data_cap = (size_t)size;
    }

    if (fread(g_data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return -1;
    }
    fclose(f);

    *len = (size_t)size;
    return 0;
}

/* Posts a rejection reason, shown for SKIP_NOTE_MS before we move on. */
static void set_note(const char *why)
{
    /* Explicit truncation: the source can be 127 bytes, the displayed reason
     * only holds 96, and fx_draw_fit would cut at the pane width anyway. Saying
     * so here avoids a legitimate compiler warning. */
    snprintf(g_note, sizeof g_note, "%.*s", (int)sizeof g_note - 1,
             why ? why : "refused");
    g_now.note    = g_note;
    g_now.playing = 0;
    g_note_since  = gettime();
}

/* Opens the current track and starts playback. Returns 0 if it is playing. */
static int track_open(void)
{
    const char *path;
    size_t      len  = 0;
    unsigned    rate = 48000;
    gcc_format  fmt;
    int         rc, cur;

    track_close();

    cur = lib_current(&g_lb);
    if (cur < 0) return -1;
    path = pl_path(&g_pl, cur);
    if (!path) return -1;

    memset(&g_now, 0, sizeof g_now);
    g_now.path = path;
    g_now.rate = rate;

    /* THE SOUNDFONT GOES BACK BEFORE THE FILE IS READ, NOT AFTER.
     *
     * This is the whole fix for "memory saturates", and the ordering is the
     * fix. track_read() asks for the track in ONE PIECE, and the block that
     * decides is the largest contiguous one -- so the 11.5 MB have to be gone
     * BEFORE the read, or a large module still fails while eleven megabytes sit
     * unused a few tracks after the last `.mid`.
     *
     * Which means the decision has to be taken without the bytes, hence
     * pl_is_midi() on the extension rather than gcc_detect() on the content.
     * Being wrong is free: a `.mid` that is not one merely loads a soundfont
     * nobody uses, and a MIDI hiding under a module extension is detected below
     * and loads it then. */
    if (!pl_is_midi(path)) soundfont_release();

    if (track_read(path, &len) != 0) {
        char msg[96];
        /* Say HOW MUCH was missing, and IN WHAT FORM.
         *
         * The old message gave only the free total, and that is what made the
         * fault unreadable: it announced comfortable memory while nothing would
         * load any more. The file is read in one piece -- the largest block
         * decides, not the sum of the crumbs. The two numbers side by side say
         * which of them is missing. */
        snprintf(msg, sizeof msg,
                 "cannot read: %lu kB free, largest block %lu kB",
                 free_ram() / 1024u, largest_free_block() / 1024u);
        set_note(msg);
        return -1;
    }

    /* The error paths no longer free g_data: it is the reused buffer, not an
     * allocation belonging to this track. */
    fmt = gcc_detect(path, (const unsigned char *)g_data, len);
    if (fmt == GCC_FMT_MIDI) {
        /* SAY THAT WE ARE READING 5.7 MB OFF THE CARD, BEFORE BLOCKING ON IT.
         *
         * ensure_soundfont() does not return for several seconds on an SD
         * Gecko, and the loop draws nothing while it runs. A player that stops
         * refreshing for four seconds is indistinguishable from the frozen
         * interface of §19 -- the fault reported the week before this one. One
         * frame beforehand costs nothing and turns a suspected crash into a
         * stated wait.
         *
         * Only when a load is really about to happen: g_sf_loaded means there
         * is nothing to do, g_sf_tried means it has already failed for good. */
        if (!g_sf_loaded && !g_sf_tried)
            ui_draw_scan(g_root, pl_count(&g_pl),
                         "loading soundfont, 5.7 MB from the card...");

        if (ensure_soundfont() != 0) {
            set_note(g_sf_why);
            return -1;
        }
    }

    if (gcaudio_init(rate, FRAMES_PER_BUF, fill_cb, NULL) != 0) {
        set_note("audio output unavailable");
        return -1;
    }

    rc = gcc_open(g_data, len, rate, fmt, &g_backend);
    if (rc != GCC_OK) {
        char        msg[96];
        const char *detail = gcc_last_detail();
        snprintf(msg, sizeof msg, "refused: %s%s%s", gcc_strerror(rc),
                 *detail ? " -- " : "", detail);
        gcaudio_shutdown();
        set_note(msg);
        return -1;
    }

    g_now.title       = gcc_title(g_backend);
    g_now.backend     = gcc_backend_name(g_backend);
    g_now.duration_ms = gcc_duration_ms(g_backend);
    g_now.playing     = 1;
    g_now.note        = NULL;
    g_watchdog        = WATCHDOG_MS(g_now.duration_ms);

    /* The duration has just been learned: note it in the cache. Four bytes at a
     * known offset, once per track. That is how the cache fills up -- without
     * ever reading a file for its duration alone.
     *
     * Both this and the resume file are written HERE, before gcaudio_start():
     * the ring is not running yet, so an SD write cannot starve it. */
    idx_set_duration(&g_ix, cur, g_now.duration_ms);
    save_state_now();

    g_silent_bufs = 0;
    g_drain_since = 0;
    gcaudio_stats_reset();
    gcaudio_start();

    g_last_tick = gettime();
    g_playing   = 1;
    return 0;
}

/* Moves forward (or back) one track and starts it. Rejections chain by
 * themselves: track_open() posts its reason and the loop calls advance() again
 * once it has been displayed. */
static void advance(int back)
{
    track_close();
    if (back) { if (!lib_prev(&g_lb)) return; }
    else      { if (!lib_next(&g_lb)) return; }
    track_open();
}

/* ------------------------------------------------------------- browser */

/* Opens the browser on the current track's folder, cursor on it. Failing a
 * current track, on the scan root. */
static void browser_to_current(void)
{
    char        dir[LIB_MAX_PREFIX];
    const char *path;
    char       *slash;
    int         cur = lib_current(&g_lb), i;

    if (cur < 0) { ui_browser_open(&g_br, &g_pl, g_root); return; }

    path = pl_path(&g_pl, cur);
    if (!path) { ui_browser_open(&g_br, &g_pl, g_root); return; }

    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '/');
    if (!slash) { ui_browser_open(&g_br, &g_pl, g_root); return; }
    slash[1] = 0;                       /* keep the trailing '/' */

    ui_browser_open(&g_br, &g_pl, dir);

    for (i = 0; i < g_br.n; i++) {
        if (!g_br.item[i].is_dir && g_br.item[i].track == cur) {
            g_br.sel = i;
            g_br.top = (i >= UI_ROWS) ? i - UI_ROWS / 2 : 0;
            break;
        }
    }
}

/* Rebuilds the index from scratch and puts every consumer back in a consistent
 * state.
 *
 * ORDER MATTERS HERE, AND THE PREVIOUS VERSION GOT IT WRONG. It freed the
 * playlist, rescanned, and only reset the library and the browser IF the scan
 * succeeded. On failure -- an unreadable card, a card pulled out -- the browser
 * kept item[].track indices into a pool that had just been freed, and the very
 * next frame dereferenced them. The library kept an order[] full of indices
 * past the end of an empty playlist, so lib_current() handed a bad index to
 * pl_path(). Both paths are now unconditional. */
static void rebuild_index(void)
{
    track_close();
    /* The walk rebuilds a ~400 kB path pool and the offset table by successive
     * realloc(); doing that around a resident 11.5 MB block is asking for the
     * scan to fail on a card it walked without trouble at start-up. Nothing is
     * playing here, so the soundfont has no reader -- and the next `.mid` will
     * fetch it again. */
    soundfont_release();
    ui_draw_scan(g_root, 0, "rebuilding index...");

    /* Everything that points into the playlist goes first. lib_free() keeps the
     * modes (order, repeat, seed) and only drops order[], which is what we
     * want: rebuilding the index is not a reason to lose the user's settings. */
    idx_free(&g_ix);
    lib_free(&g_lb);
    memset(&g_br, 0, sizeof g_br);
    pl_free(&g_pl);
    pl_init(&g_pl);

    if (pl_scan(&g_pl, g_root) > 0) {
        idx_save(&g_ix, &g_pl, g_idx_file, g_root);
        lib_set_scope(&g_lb, &g_pl, "", 1);
    }
    ui_browser_open(&g_br, &g_pl, g_root);
    save_state_later();
}

/* --------------------------------------------------------------- input */

/* ONE BUTTON, ONE ROLE. No focus, no mode, nothing that depends on invisible
 * state: the d-pad belongs to the list, the C-stick to the player, and the four
 * right-hand buttons are toggles with no homonym.
 *
 * The version before that moved a focus with START and gave two meanings to
 * every button. In use, "START, up, down, everything seems to blur together" --
 * which was accurate, and no amount of on-screen state would have fixed it.
 *
 * THE TWO DIAGNOSTIC BINDINGS ARE GONE. d-pad LEFT swapped the decoder for a
 * 440 Hz sine, and d-pad RIGHT stopped drawing for eight seconds. Both were
 * built to split a crackle in two, both did their job, and both sat on the two
 * buttons a user presses first when a list is on screen: pressing RIGHT froze
 * the picture for eight seconds with no way to tell why. They now do what a
 * file browser's left and right have always done -- go up, go in. */
static void handle_keys(u32 down, u32 nav)
{
    /* L+R together: rebuild the index. Two buttons because it is rare and
     * destructive of the cache -- and because L and R alone already do
     * something. Checked before the individual triggers so the combo does not
     * also scroll the list. */
    if ((down & (PAD_TRIGGER_L | PAD_TRIGGER_R)) &&
        (PAD_ButtonsHeld(0) & (PAD_TRIGGER_L | PAD_TRIGGER_R))
            == (PAD_TRIGGER_L | PAD_TRIGGER_R)) {
        rebuild_index();
        return;
    }

    /* --- the d-pad and the triggers: the list, always --- */
    if (nav & PAD_BUTTON_UP)    ui_browser_move(&g_br, -1);
    if (nav & PAD_BUTTON_DOWN)  ui_browser_move(&g_br,  1);
    if (nav & PAD_TRIGGER_L)    ui_browser_move(&g_br, -UI_JUMP);
    if (nav & PAD_TRIGGER_R)    ui_browser_move(&g_br,  UI_JUMP);

    /* LEFT = out, RIGHT = in. Same as B and A, which stay as they are: on a
     * pad, both habits exist and neither costs anything. */
    if (nav & PAD_BUTTON_LEFT)  ui_browser_up(&g_br, &g_pl, g_root);
    if (nav & PAD_BUTTON_RIGHT) {
        const char *sub = ui_browser_sel_dir(&g_br, &g_pl);
        if (sub) ui_browser_open(&g_br, &g_pl, sub);
    }

    if (down & PAD_BUTTON_B) ui_browser_up(&g_br, &g_pl, g_root);

    if (down & PAD_BUTTON_A) {
        const char *sub = ui_browser_sel_dir(&g_br, &g_pl);
        if (sub) {
            ui_browser_open(&g_br, &g_pl, sub);
        } else if (g_br.n) {
            /* A file: we play the CURRENT folder, without descending. */
            lib_set_scope(&g_lb, &g_pl, g_br.dir, 0);
            lib_goto_track(&g_lb, g_br.item[g_br.sel].track);
            track_open();
        }
    }

    /* START = the whole tree under the targeted folder -- or under the current
     * folder if the cursor is on a file. From the root, that is the whole
     * pack. */
    if (down & PAD_BUTTON_START) {
        const char *sub = ui_browser_sel_dir(&g_br, &g_pl);
        lib_set_scope(&g_lb, &g_pl, sub ? sub : g_br.dir, 1);
        track_open();
    }

    /* --- the toggles: the player, always --- */
    if (down & PAD_BUTTON_X) gcaudio_set_paused(!gcaudio_paused());

    if (down & PAD_BUTTON_Y) {
        lib_set_repeat(&g_lb, (lib_repeat)((g_lb.repeat + 1) % 3));
        save_state_later();
    }

    if (down & PAD_TRIGGER_Z) {
        lib_set_order(&g_lb, g_lb.order_mode == LIB_ORDER_RANDOM
                             ? LIB_ORDER_SEQ : LIB_ORDER_RANDOM);
        save_state_later();
    }
}

/* Turns "held" into "pressed, then repeating".
 *
 * Only the four d-pad directions repeat. The triggers deliberately do not: they
 * jump ten rows, and a ten-row jump repeating at 18 Hz lands nowhere. The L+R
 * combo is checked before any of this, so a repeat can never trigger a
 * rebuild. */
static u32 nav_repeat(u32 held, u32 down, u64 nowt)
{
    static u32 latched   = 0;
    static u64 next_fire = 0;

    const u32 mask = PAD_BUTTON_UP | PAD_BUTTON_DOWN |
                     PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;
    u32 dir  = held & mask;
    u32 edge = down & mask;
    u32 out  = edge | (down & (PAD_TRIGGER_L | PAD_TRIGGER_R));

    if (edge) {
        latched   = edge;
        next_fire = nowt + millisecs_to_ticks(REPEAT_DELAY_MS);
    } else if (!dir) {
        latched   = 0;
        next_fire = 0;
    } else if (latched && (latched & dir) && nowt >= next_fire) {
        out      |= latched & dir;
        next_fire = nowt + millisecs_to_ticks(REPEAT_RATE_MS);
    }

    return out;
}

/* Main analog stick: same job as the d-pad, because half the people who pick up
 * a GameCube pad reach for it first. Digital thresholds with the same repeat,
 * so the two are indistinguishable in use. */
static u32 stick_nav(u64 nowt)
{
    static int prev = 0;
    static u64 next_fire = 0;
    s8  y   = PAD_StickY(0);
    int now = (y > 45) ? -1 : (y < -45) ? 1 : 0;   /* up = towards the top */
    u32 bit = (now < 0) ? PAD_BUTTON_UP : PAD_BUTTON_DOWN;

    if (!now) { prev = 0; next_fire = 0; return 0; }

    if (now != prev) {
        prev      = now;
        next_fire = nowt + millisecs_to_ticks(REPEAT_DELAY_MS);
        return bit;
    }
    if (nowt >= next_fire) {
        next_fire = nowt + millisecs_to_ticks(REPEAT_RATE_MS);
        return bit;
    }
    return 0;
}

/* C-stick: volume horizontally, previous/next track vertically.
 *
 * An analog stick has no edge, hence the minimum time step -- without it, a
 * held stick would sweep the volume from 0 to 100 in half a second.
 *
 * THE DEAD ZONE IS DELIBERATELY WIDE, and the track skip requires a return to
 * centre. A worn or badly calibrated C-stick rests off-centre; with the old
 * 40/50 thresholds that meant an SD write every 150 ms and a track skip every
 * 400 ms, forever, with nothing touching the pad. */
static void handle_cstick(u64 nowt)
{
    static u64 last_vol = 0;
    static int trk_armed = 1;
    s8 cx = PAD_SubStickX(0);
    s8 cy = PAD_SubStickY(0);

    if ((cx > 58 || cx < -58) && ticks_to_millisecs(nowt - last_vol) > 150) {
        unsigned v = gcaudio_volume();
        gcaudio_set_volume(cx > 0 ? v + 5 : (v >= 5 ? v - 5 : 0));
        last_vol = nowt;
        save_state_later();
    }

    if (cy > -30 && cy < 30) trk_armed = 1;
    else if (trk_armed && (cy > 62 || cy < -62)) {
        trk_armed = 0;
        advance(cy < 0);            /* down = previous, up = next */
    }
}

/* Reads the pad and applies it. Called MORE THAN ONCE per pass through the main
 * loop -- see service_audio(). Calling it twice in the same video frame is
 * harmless: libogc derives "just pressed" from the previous scan, so the second
 * call sees no new edge. */
static void pump_input(void)
{
    u64 nowt = gettime();
    u32 down, held, nav;

    PAD_ScanPads();
    down = PAD_ButtonsDown(0);
    held = PAD_ButtonsHeld(0);
    if (down) seed_from_user();

    nav = nav_repeat(held, down, nowt) | stick_nav(nowt);

    handle_keys(down, nav);
    handle_cstick(nowt);
}

/* Renders at most AUDIO_BLOCKS_PER_PASS blocks, READING THE PAD BETWEEN TWO.
 *
 * That interleaving is the whole point. A block is 43 ms of audio; producing
 * one costs a fraction of that on a light module and most of it on a dense
 * one. Rendering two back to back without looking at the pad is up to eighty
 * milliseconds of input latency, and a button pressed and released inside that
 * window is lost outright -- which is what "the menu freezes" looks like. */
static void service_audio(void)
{
    int i;

    for (i = 0; i < AUDIO_BLOCKS_PER_PASS; i++) {
        if (!gcaudio_service_step()) break;
        pump_input();
    }
    gcaudio_service_arm();
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int i, n = 0, from_cache = 0, resumed = 0;
    u64 t0, last_draw = 0;
    unsigned long scan_ms;

    note_dol_dir(argc, argv);

    /* ui_init() FIRST: GRRLIB takes over the display and does not give it back,
     * so anything printed before would be lost -- including the explanation of
     * a failure. Start-up narrates itself through ui_boot(). */
    if (ui_init() != 0) {
        printf("\nFAILED: GX did not start. Nothing to draw on.\n");
        for (;;) VIDEO_WaitVSync();
    }

    PAD_Init();
    gcaudio_flush_denormals();
    gcc_midi_set_max_voices(GCC_MIDI_DEFAULT_VOICES);

    pl_init(&g_pl);
    lib_init(&g_lb);
    idx_init(&g_ix);
    memset(&g_now, 0, sizeof g_now);

    ui_boot("Mounting SD card...");
    if (gcs_mount() != 0)
        ui_boot_fatal("FAILED: no SD card (slot A, slot B, SD2SP2).");
    ui_boot("  %s", gcs_source_name());

    ui_boot("free memory: %lu kB", free_ram() / 1024u);
    /* The soundfont is only loaded on the first .mid, so much later: say WHERE
     * we will look for it while there is still a screen to say it on. */
    ui_boot("soundfont expected at: %ssoundfont.sf2",
            g_dol_dir[0] ? g_dol_dir : "sd:/");

    /* Find the root. We are not scanning yet: the cache may spare us
     * entirely. */
    for (i = 0; g_roots[i]; i++) {
        DIR *d = opendir(g_roots[i]);
        if (d) { closedir(d); g_root = g_roots[i]; break; }
    }
    if (!g_root)
        ui_boot_fatal("FAILED: no music folder. Try sd:/chiptunes.");

    join_root(g_idx_file,   sizeof g_idx_file,   "gc-chiptune.idx");
    join_root(g_state_file, sizeof g_state_file, "gc-chiptune.state");

    t0 = gettime();
    n  = idx_load(&g_ix, &g_pl, g_idx_file, g_root);
    if (n > 0) {
        from_cache = 1;
    } else {
        ui_draw_scan(g_root, 0, "first run: walking the card...");
        n = pl_scan(&g_pl, g_root);
        if (n > 0) idx_save(&g_ix, &g_pl, g_idx_file, g_root);
    }
    scan_ms = (unsigned long)ticks_to_millisecs(gettime() - t0);

    if (n <= 0) ui_boot_fatal("FAILED: no tracks under %s.", g_root);

    ui_boot("%d tracks %s in %lu.%02lu s", n,
            from_cache ? "read from the index" : "walked and indexed",
            scan_ms / 1000, (scan_ms % 1000) / 10);
    ui_boot("%d durations known", idx_known(&g_ix));
    if (!from_cache)
        ui_boot("index written: later start-ups will be immediate");

    /* Resume. Scope and modes first, position second. */
    st_load(&g_st, g_state_file);
    lib_set_seed(&g_lb, g_st.seed);
    lib_set_repeat(&g_lb, g_st.repeat);
    lib_set_order(&g_lb, g_st.order);
    gcaudio_set_volume(g_st.volume);

    if (lib_set_scope(&g_lb, &g_pl, g_st.scope, g_st.recursive) <= 0)
        lib_set_scope(&g_lb, &g_pl, "", 1);

    if (g_st.valid) {
        /* The track is found BY PATH: an index would point at the wrong file as
         * soon as a track had been added or removed. */
        for (i = 0; i < pl_count(&g_pl); i++) {
            const char *p = pl_path(&g_pl, i);
            if (p && !strcmp(p, g_st.track)) {
                if (lib_goto_track(&g_lb, i)) {
                    resumed = 1;
                    ui_boot("resuming at track %d", lib_position(&g_lb));
                }
                break;
            }
        }
    }

    /* On a resume, put the browser on the resumed track's folder rather than at
     * the root. Without a resume there is nothing to aim at. */
    if (resumed) browser_to_current();
    else         ui_browser_open(&g_br, &g_pl, g_root);

    pause_ms(1500);

    /* A resume must put playback back where it was left. */
    if (resumed) track_open();

    /* --- the loop: audio, pad, picture --- */
    for (;;) {
        u64 nowt = gettime();

        /* 1. The pad, before anything else. Whatever the rest of the pass
         *    costs, the input has already been read for this frame. */
        pump_input();

        /* 2. The audio: the only work with a deadline. service_audio() bounds
         *    itself and reads the pad again between two blocks. */
        if (g_playing) {
            service_audio();

            /* Playback clock accumulated rather than read from a t0: a pause
             * must not advance the counter. */
            nowt = gettime();
            if (!gcaudio_paused())
                g_now.elapsed_ms += (unsigned long)ticks_to_millisecs(nowt - g_last_tick);
            g_last_tick  = nowt;
            g_now.paused = gcaudio_paused();

            if (g_now.elapsed_ms >= g_watchdog) {
                advance(0);                          /* watchdog */
            } else if (g_silent_bufs >= 2) {
                /* Natural end. DO NOT CUT HERE: the decoder has finished, but
                 * the ring still holds several blocks of real music and
                 * libansnd a couple more. Stopping the voice now clipped the
                 * last fifth of a second off every single track. We wait for
                 * the ring to drain, then a fixed margin for the stages we
                 * cannot see. */
                if (!g_drain_since) g_drain_since = nowt;
                if (gcaudio_lead() == 0 &&
                    ticks_to_millisecs(nowt - g_drain_since) >= DRAIN_MS)
                    advance(0);
            }
        } else if (g_now.note && ticks_to_millisecs(nowt - g_note_since) >= SKIP_NOTE_MS) {
            /* The rejection reason has been read: move on. */
            g_now.note = NULL;
            advance(0);
        }

        /* 3. Deferred SD work, only when the ring can afford it. */
        save_state_maybe(nowt);

        /* 4. The picture. */
        g_now.vu_l = g_vu_l;
        g_now.vu_r = g_vu_r;

        /* Memory, once a second, and never while the audio is short: probing
         * the largest free block costs a handful of allocations. */
        {
            static u64 last_ram = 0;
            if ((!last_ram || ticks_to_millisecs(nowt - last_ram) >= RAM_POLL_MS) &&
                (!g_playing || gcaudio_lead() >= GCAUDIO_LEAD_MAX)) {
                g_now.ram_free  = free_ram() / 1024u;
                g_now.ram_block = largest_free_block() / 1024u;
                last_ram = nowt;
            }
        }

        /* Skip the frame if the audio has no lead -- but never for longer than
         * REDRAW_MAX_SKIP_MS. And when we do skip, we go straight back to the
         * audio instead of waiting for the retrace: the old code burned a whole
         * 16 ms vsync in the very branch whose purpose was to give that time
         * back to the decoder. */
        if (gcaudio_lead() >= REDRAW_MIN_LEAD ||
            (last_draw && ticks_to_millisecs(nowt - last_draw) >= REDRAW_MAX_SKIP_MS)) {
            ui_draw(&g_br, &g_pl, &g_ix, &g_lb, &g_now, g_root,
                    gcs_source_name());
            ui_present();
            last_draw = gettime();
        }
    }

    /* No exit: the console is switched off at the button. The only state that
     * must survive is written on every change (save_state_now). */
}
