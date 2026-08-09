/* ------------------------------------------------------------------------
 * GC-Chiptune: GameCube audio output (libogc2). See audio_gc.h.
 *
 * BUILT ON LIBANSND. This file changed foundation twice, both times for a
 * reason that only showed on real hardware:
 *
 *  1. Raw AUDIO_ API (original). TWO buffers, DMA REARMED FROM THE
 *     END-OF-TRANSFER INTERRUPT: between the end of one block and the arming of
 *     the next, the DAC had nothing -- a gap at every boundary, ~23 per second
 *     at 48 kHz. And the AI frequency was switched between tracks, which
 *     triggers __AISRCINIT() on Gekko: busy waits with interrupts disabled.
 *     Dolphin shows neither.
 *
 *  2. ASND (libasnd). Fixed both: AI pinned at 48 kHz once and for all, ring
 *     primed ahead, mixing on the DSP.
 *
 *  3. ANSND (libansnd), here. On the advice of the libogc2 maintainer: "Use
 *     libansnd for best performance."
 *
 * WHAT ANSND CHANGES OVER ASND, concretely:
 *
 *   - SAMPLES ARE READ BY THE DSP ACCELERATOR FROM ARAM. ASND copied every
 *     block into main memory by hand. Here we stage the block into ARAM by DMA
 *     (ARQ) and the DSP fetches it itself: not one copy left on the Gekko in
 *     the audio path, and the main memory bus is no longer used for audio.
 *
 *   - WINDOWED-SINC RESAMPLING, where ASND does linear. The render comes out at
 *     48000 Hz and the GameCube AI runs at 54000000/1124, i.e. ~48043 Hz: there
 *     is always a ratio to make up, so it may as well be made up properly.
 *     Measured cost upstream: 5.56 % of the DSP for one resampled voice, on a
 *     machine that holds 18.
 *
 *   - DSP LOAD IS REPORTED PLAINLY (ansnd_get_dsp_usage_percent), without the
 *     unit bug in ASND_GetDSP_PercentUse that returned 2110 to 3410 per mille
 *     -- impossible values that had to be recomputed by hand.
 *
 * ARAM IS THE REAL STRUCTURAL CHANGE. libansnd refuses a pointer that is not in
 * it (ANSND_ERROR_INVALID_MEMORY, ansndlib.c). The path becomes:
 *
 *     gcc_render -> MRAM buffer -> DCFlushRange -> ARQ (DMA) -> ARAM block
 *                                                                    |
 *                          stream_callback (DSP interrupt) ----------+
 *
 * The 16 MB of ARAM served no purpose until now. They are now the queue: SIX
 * blocks, of which at most two are held by the DSP (the one playing and the one
 * already armed). Four blocks of lead at 2048 samples is ~256 ms of margin,
 * twice what ASND held. A player has no latency constraint and that margin
 * absorbs load peaks instead of turning them into gaps.
 *
 * RENDERING STAYS IN gcaudio_service_step(), i.e. in the main loop. The
 * stream_callback runs in the DSP interrupt, every 5 ms: all it does is name
 * the next block. Upstream is emphatic about this ("it is advised that user
 * callback functions are returned from as quickly as possible"), and a block
 * can consume several tens of milliseconds.
 *
 * ANSND_VOICE_PCM_FORMAT_SIGNED_16_PCM + channels=2: interleaved big-endian s16
 * stereo, exactly what backend.h produces in native order on Gekko. No
 * conversion.
 *
 * NOTE: this file only compiles for the GameCube target (devkitPPC defines
 * GEKKO). On the host it reduces to nothing, so it can live in the tree without
 * breaking build-host.ps1.
 * ------------------------------------------------------------------------ */

#include "audio_gc.h"

#ifdef GEKKO

#include <gccore.h>
#include <ansndlib.h>
#include <ogc/aram.h>
#include <ogc/arqueue.h>
#include <ogc/cache.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <malloc.h>
#include <string.h>

/* HOW MANY BLOCKS DOES LIBANSND HOLD? The first version answered TWO. That was
 * wrong, and it was audible: continuous crackle on both backends, including the
 * XM at 12 per mille of load with zero underruns -- the same symptom, exactly,
 * as fault #1 above.
 *
 * There are in fact THREE stages, not two (ansndlib.c):
 *
 *   ansnd_update_stream_buffers()          <- every DSP cycle, 5 ms
 *       if the staging slot is empty  -> fill : calls OUR stream_callback
 *       if the DSP slot is empty      -> write: staging -> DSP, THEN CLEARS
 *                                               THE STAGING SLOT
 *
 * Both run in the same call, in that order. So on every other cycle the staging
 * slot is filled without being emptied: at any instant libansnd holds
 *
 *   1. the block the accelerator IS PLAYING
 *   2. parameter_block->streaming.next_buffer   (the stage the DSP sees)
 *   3. voice->streaming.next_buffer             (the staging slot, driver side)
 *
 * With six blocks and four of lead, the live indices spanned s_taken-3 to
 * s_taken+3, i.e. SEVEN values for six slots. In steady state the ring sits at
 * maximum lead permanently, so the collision landed on the staged block on
 * EVERY block produced, ~23 times per second.
 *
 * We are not sizing this to the last slot again. HELD=4 leaves one stage of
 * margin over what reading the code says, and NCHUNK=10 leaves two more:
 * HELD + LEAD = 8 slots occupied out of 10. It costs 160 kB of ARAM out of
 * 16 MB -- there is no reason to count closely here, and an excellent reason
 * not to. */
#define NCHUNK 10                 /* ARAM blocks                             */
#define HELD   4                  /* held by libansnd (3 observed, +1)       */
#define LEAD   4                  /* blocks we fill ahead                    */

/* The invariant that broke the first time, now checked at compile time rather
 * than in a comment. Live indices run from s_taken-HELD to s_taken+LEAD-1, plus
 * the one being written: there must be strictly fewer than NCHUNK for none to
 * overlap. */
#if (HELD + LEAD) >= NCHUNK
#error "ARAM ring too small: a block held by libansnd would be overwritten"
#endif

/* Ceiling on frames_per_buf. ARAM is allocated ONCE for the whole life of the
 * program: AR_Alloc/AR_Free are a LIFO stack and AR_Init is destructive, so
 * releasing and retaking them per track would gain nothing. */
#define MAX_FRAMES 4096

static u8   *s_stage;             /* render buffer, main memory              */
static u32   s_aram[NCHUNK];      /* ARAM addresses of the blocks            */
static u32   s_aram_idx[NCHUNK];  /* tracking table required by AR_Init      */
static int   s_aram_up;

static s32   s_voice = -1;
static int   s_started;           /* the voice is running                    */
static unsigned s_frames;         /* stereo frames per block                 */
static unsigned s_bytes;
static unsigned s_rate;           /* RENDER rate, not the AI's               */
static gcaudio_fill s_fill;
static void *s_user;
static int   s_running;

/* Monotonic counters, not indices: the unsigned difference gives the fill level
 * with no special case at wrap-around.
 *
 * THIS COMMENT ASSERTED FOR WEEKS that one producer and one consumer shared
 * these two variables -- "s_filled by the main loop, s_taken by the DSP
 * interrupt, no lock needed". THAT WAS FALSE: arm_voice() runs in the main loop
 * and writes s_taken too. See the note in arm_voice(), where the increment is
 * now done with interrupts disabled.
 *
 * s_filled really is only written by the main loop. */
static volatile u32 s_filled;
static volatile u32 s_taken;

static volatile unsigned long s_underruns;
/* DSP cycles where the ring was empty. See stream_cb(). */
static volatile unsigned long s_starved;
static volatile int  s_finished;  /* the voice ran dry (ISR)                 */

/* Volume applied to the voice, 0.0 to 1.0. Kept here because
 * ansnd_configure_pcm_voice() resets the voice: arm_voice() has to reapply it
 * for every track, or the setting would jump between two tracks. */
static float s_volume = 1.0f;
static int   s_paused;

static unsigned s_load;
static unsigned long s_load_sum;
static unsigned long s_load_n;
static unsigned s_load_max;

/* ansnd_initialize() starts the DSP task and pins the AI: once for the whole
 * program. gcaudio_init() is called for every track. */
static int s_ansnd_up;

/* ------------------------------------------------------------------------
 * Callbacks -- DSP interrupt context, every 5 ms.
 * ------------------------------------------------------------------------ */

static void stream_cb(void *user, ansnd_pcm_data_buffer_t *out)
{
    (void)user;

    /* NOTHING READY: THE DSP ASKED FOR A BLOCK AND THERE WAS NONE.
     *
     * This comment used to say "that is a delay, not a fault". It is false, and
     * the endurance run showed it: while a track too heavy for the machine
     * stretched from 20 to 36 %, gcaudio_underruns() -- which only counts
     * VOICE_STATE_FINISHED -- stayed at ZERO. The voice re-arms too fast to
     * fall into the FINISHED state, but the sound really was missing.
     *
     * This is the THIRD counter in this project to report "all is well" on a
     * sick output. This one measures the condition itself: how many times the
     * DSP left empty-handed. */
    if (s_filled == s_taken) { s_starved++; return; }

    out->frame_data_ptr = s_aram[s_taken % NCHUNK];
    out->frame_count    = s_frames;
    s_taken++;
}

static void voice_cb(void *user, s32 state)
{
    (void)user;

    /* THE REAL UNDERRUN COUNTER. The very first one, under the AUDIO_ API, only
     * saw "the render did not keep up" and stayed at ZERO while the console
     * crackled: the buffer was ready, it was the DMA that ran empty. Here the
     * measured condition is the one you can hear -- the voice exhausted its
     * buffers and stopped. */
    if (state == ANSND_VOICE_STATE_FINISHED) {
        s_finished = 1;
        s_underruns++;
    }
}

/* ------------------------------------------------------------------------ */

/* Renders one block and stages it into ARAM. Returns 1 if a block was
 * produced. */
static int produce(void)
{
    ARQRequest req;
    u64 t0, t1;
    unsigned us_spent, us_budget;
    u32 dst_aram;

    if ((u32)(s_filled - s_taken) >= LEAD) return 0;

    t0 = gettime();

    s_fill(s_user, (short *)s_stage, s_frames);

    dst_aram = s_aram[s_filled % NCHUNK];

    DCFlushRange(s_stage, s_bytes);

    /* Synchronous transfer: on return the MRAM buffer is free again, so one is
     * enough for all the ARAM blocks. ARQ_PostRequest sleeps on an LWP queue --
     * legal here (thread context), forbidden inside a callback. */
    ARQ_PostRequest(&req, 0, ARQ_MRAMTOARAM, ARQ_PRIO_HI,
                    dst_aram,
                    (u32)MEM_VIRTUAL_TO_PHYSICAL(s_stage), s_bytes);

    t1 = gettime();

    /* LOAD = EVERYTHING THE BLOCK COST, divided by the audio duration it
     * produces. Above 1000 per mille the Gekko cannot hold real time.
     *
     * The stopwatch now spans the cache flush AND the ARAM transfer, not just
     * the render. The previous version excluded them, with this argument: "it
     * is DMA, a few tens of microseconds, and it does not vary with the track".
     * That may be true, but it was not measured -- and the claim "libansnd
     * costs no more CPU than ASND" rested on it. ARQ_PostRequest is
     * synchronous: the time it takes is time the main loop does not spend
     * elsewhere, and it belongs to the load.
     *
     * The ratio still does not depend on downstream resampling: the DSP
     * consumes no Gekko. */
    us_spent  = (unsigned)ticks_to_microsecs(t1 - t0);
    us_budget = (unsigned)((u64)s_frames * 1000000u / s_rate);
    s_load    = us_budget ? (unsigned)((u64)us_spent * 1000u / us_budget) : 0;
    s_load_sum += s_load;
    s_load_n++;
    if (s_load > s_load_max) s_load_max = s_load;

    s_filled++;
    return 1;
}

/* Configures and starts the voice on the oldest pending block. */
static int arm_voice(void)
{
    ansnd_pcm_voice_config_t cfg;
    u32 level;

    if (s_filled == s_taken) return 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.samplerate      = s_rate;
    cfg.format          = ANSND_VOICE_PCM_FORMAT_SIGNED_16_PCM;
    cfg.channels        = 2;
    cfg.pitch           = 1.0f;
    cfg.left_volume     = s_volume;
    cfg.right_volume    = s_volume;
    cfg.voice_callback  = voice_cb;
    cfg.stream_callback = stream_cb;
    cfg.frame_count     = s_frames;

    /* RACE ON s_taken, AND IT WAS REAL.
     *
     * The header of this file asserts: "s_filled is only written by the main
     * loop, s_taken by the DSP interrupt -- one producer, one consumer, no lock
     * needed". That was false: arm_voice() runs in the main loop and WRITES
     * s_taken too.
     *
     * The scenario, if the stream_callback lands between reading s_aram[s_taken]
     * and the s_taken++:
     *
     *   arm_voice   reads s_aram[5], configures the voice on it
     *   stream_cb   (interrupt) ALSO hands out block 5, s_taken -> 6
     *   arm_voice   s_taken++ -> 7
     *
     * Block 5 is played TWICE and block 6 is skipped. It is a plain, perfectly
     * audible fault, and no counter sees it: the ring was never empty, so `dry`
     * stays at zero.
     *
     * So we read the index and advance it with interrupts disabled. The window
     * was narrow, but it opens on every re-arm -- and if the voice falls into
     * FINISHED often, it opens continuously. */
    _CPU_ISR_Disable(level);
    cfg.frame_data_ptr = s_aram[s_taken % NCHUNK];
    s_taken++;
    _CPU_ISR_Restore(level);

    if (ansnd_configure_pcm_voice((u32)s_voice, &cfg) != ANSND_ERROR_OK)
        return 0;

    if (ansnd_start_voice((u32)s_voice) != ANSND_ERROR_OK) return 0;

    s_started = 1;
    return 1;
}

/* ------------------------------------------------------------------------ */

int gcaudio_init(unsigned rate, unsigned frames_per_buf,
                 gcaudio_fill fill, void *user)
{
    int i;

    if (!fill) return -1;
    /* The AI is pinned at 48 kHz by libansnd and the DSP does the conversion,
     * so any render rate would play. We keep these two values: they are the
     * ones the whole CPU budget was measured on, and raising the render rate
     * would only cost Gekko time. */
    if (rate != 32000 && rate != 48000) return -2;

    /* Multiple of 8 frames: 32 bytes, one Gekko cache line. That is what
     * DCFlushRange requires under pain of flushing the neighbours, and it is
     * also what keeps the ARAM blocks aligned -- AR_Alloc aligns nothing, it
     * stacks lengths as given. */
    frames_per_buf = (frames_per_buf + 7u) & ~7u;
    if (frames_per_buf < 64)         frames_per_buf = 64;
    if (frames_per_buf > MAX_FRAMES) return -5;

    s_rate   = rate;
    s_frames = frames_per_buf;
    s_bytes  = frames_per_buf * 4u;
    s_fill   = fill;
    s_user   = user;
    s_underruns = 0;
    s_starved   = 0;
    s_load    = 0;
    /* Pause does NOT survive a track change -- otherwise the next track would
     * start muted and silently stuck. Volume does: it is a setting, not a
     * playback state. */
    s_paused  = 0;
    s_running = 0;
    s_started = 0;
    s_finished = 0;
    s_filled  = 0;
    s_taken   = 0;

    if (!s_aram_up) {
        /* AR_Init is DESTRUCTIVE to ARAM and must precede any allocation. Once
         * only, and before ansnd_initialize(): libansnd queries
         * AR_GetBaseAddress()/AR_GetSize() to validate the pointers we hand it,
         * and both are zero until AR_Init has run. */
        AR_Init(s_aram_idx, NCHUNK);
        ARQ_Init();
        for (i = 0; i < NCHUNK; i++) {
            s_aram[i] = AR_Alloc(MAX_FRAMES * 4u);
            if (!s_aram[i]) return -3;
        }
        s_stage = (u8 *)memalign(32, MAX_FRAMES * 4u);
        if (!s_stage) return -3;

        /* WE ZERO ALL THE RESERVED ARAM, ONCE.
         *
         * Each block reserves MAX_FRAMES*4 = 16384 bytes, but we only ever
         * write s_bytes = 8192 of them at 2048 frames. THE UPPER HALF OF EVERY
         * BLOCK IS THEREFORE NEVER WRITTEN: at start-up it holds whatever ARAM
         * held before us, i.e. noise.
         *
         * And the DSP accelerator does not stop dead at `next_buffer_end`: it
         * reads continuously and libansnd substitutes the next block on the
         * overrun interrupt, on the following DSP cycle -- up to 5 ms later,
         * i.e. ~240 samples. Those 240 samples come from AFTER the end of our
         * block, so from the never-written area.
         *
         * At full scale, uninitialised ARAM noise sounds exactly like what was
         * reported: a sharp, regular crackle, once per block -- 23 times per
         * second at 2048 frames -- completely independent of the track, the CPU
         * load and the state of the ring. That is why `dry` stays at zero:
         * nothing is missing, it is what is READ IN EXCESS that makes the noise.
         *
         * libansnd's streaming example does not have this problem: it allocates
         * exactly what it writes. We oversize so frames_per_buf can change
         * without reallocating ARAM -- defensible, but then the reserve has to
         * be zeroed. Silence overflows without being heard.
         *
         * Cost: ten 16 kB DMAs, once, on the first track. */
        memset(s_stage, 0, MAX_FRAMES * 4u);
        DCFlushRange(s_stage, MAX_FRAMES * 4u);
        for (i = 0; i < NCHUNK; i++) {
            ARQRequest z;
            ARQ_PostRequest(&z, 0, ARQ_MRAMTOARAM, ARQ_PRIO_HI,
                            s_aram[i],
                            (u32)MEM_VIRTUAL_TO_PHYSICAL(s_stage),
                            MAX_FRAMES * 4u);
        }

        s_aram_up = 1;
    }

    memset(s_stage, 0, s_bytes);

    if (!s_ansnd_up) {
        ansnd_initialize();     /* 48 kHz, DSP_Init + AUDIO_Init included */
        s_ansnd_up = 1;
    }

    /* ONE VOICE, TAKEN ONCE, NEVER RETURNED. This is not laziness: returning a
     * voice that was never CONFIGURED freezes the console.
     *
     *   ansnd_allocate_voice()    memset(voice, 0, ...)  -> parameter_block NULL
     *   ansnd_configure_pcm_voice()  is the ONLY place that sets it
     *   ansnd_deallocate_voice()  sets VOICE_FLAG_ERASED
     *   ... then, from the DSP interrupt:
     *   ansnd_sync_voice() -> ansnd_erase_voice() -> memset(NULL, 0, 128)
     *
     * -> Exception (DSI), DAR 0, DSISR 0x06000000, on `stw r10, 0(r9)` with r9
     * null. Seen on console, LR pointing into ansnd_dsp_request_callback
     * (ansndlib.c:496-501).
     *
     * The path is mundane: gcaudio_init() succeeds, gcc_open() refuses the
     * track, gcaudio_shutdown() follows. The voice was allocated and never
     * configured. The bench hit it as soon as a phase was refused, which it
     * MUST be -- and the player would hit it on the first refused file on the
     * card.
     *
     * Keeping the voice removes the forbidden state instead of dodging it, and
     * leaks nothing: there is only one, out of the 48 the library holds, and
     * arm_voice() reconfigures it entirely for every track. */
    if (s_voice < 0) {
        s_voice = ansnd_allocate_voice();
        if (s_voice < 0) return -4;
    }

    return 0;
}

void gcaudio_start(void)
{
    int i;

    if (s_running) return;
    s_running = 1;

    /* Priming: fill the ring BEFORE handing back, or the first few hundred
     * milliseconds come out silent. This is the only place we render several
     * blocks in a row; afterwards the caller drives it one block at a time. */
    for (i = 0; i < LEAD; i++) produce();

    arm_voice();
}

void gcaudio_stop(void)
{
    if (!s_running) return;
    if (s_started && s_voice >= 0) ansnd_stop_voice((u32)s_voice);
    s_started = 0;
    s_running = 0;
}

void gcaudio_shutdown(void)
{
    /* Stopping the voice is enough: the DSP then stops reading the ARAM blocks
     * and the stream_callback no longer fires.
     *
     * WE DO NOT RETURN THE VOICE -- ansnd_deallocate_voice() on a voice that
     * was never configured freezes the console. See the note in gcaudio_init().
     *
     * The ARAM, the render buffer and the DSP task stay in place too: see
     * s_aram_up and s_ansnd_up. */
    gcaudio_stop();
}

int gcaudio_service_step(void)
{
    if (!s_running || s_voice < 0) return 0;

    /* The voice exhausted its buffers. libansnd requires a full RECONFIGURATION
     * to restart a stream: "the initial buffer will be cleared and the stream
     * can not be started again without reconfiguring the voice with new data"
     * (ansndlib.h). Without this re-arm, the slightest hiccup would kill the
     * sound for good -- the same trap as ASND_SetVoice versus ASND_AddVoice on
     * the previous foundation. */
    if (s_finished) {
        s_finished = 0;
        s_started  = 0;
    }

    return produce();
}

void gcaudio_service_arm(void)
{
    if (!s_running || s_voice < 0) return;
    if (!s_started) arm_voice();
}

void gcaudio_service(void)
{
    while (gcaudio_service_step())
        ;
    gcaudio_service_arm();
}

void gcaudio_set_volume(unsigned percent)
{
    if (percent > 100) percent = 100;
    s_volume = (float)percent / 100.0f;
    if (s_started && s_voice >= 0)
        ansnd_set_voice_volume((u32)s_voice, s_volume, s_volume);
}

unsigned gcaudio_volume(void) { return (unsigned)(s_volume * 100.0f + 0.5f); }

void gcaudio_set_paused(int paused)
{
    if (s_voice < 0 || !s_started) { s_paused = paused ? 1 : 0; return; }

    if (paused && !s_paused)       ansnd_pause_voice((u32)s_voice);
    else if (!paused && s_paused)  ansnd_unpause_voice((u32)s_voice);
    s_paused = paused ? 1 : 0;

    /* NOTE: rendering continues during a pause. That is not an oversight -- the
     * ring fills up, produce() stops by itself at LEAD blocks, and resuming is
     * instant. Suspending the render would gain a few tens of milliseconds of
     * Gekko time and make coming out of pause audible. */
}

int gcaudio_paused(void) { return s_paused; }

void gcaudio_stats_reset(void)
{
    s_load_sum  = 0;
    s_load_n    = 0;
    s_load_max  = 0;
    s_underruns = 0;
    s_starved   = 0;
}

unsigned gcaudio_load_max(void) { return s_load_max; }

unsigned gcaudio_load_avg(void)
{
    return s_load_n ? (unsigned)(s_load_sum / s_load_n) : 0;
}

unsigned      gcaudio_frames_per_buf(void) { return s_frames; }
unsigned long gcaudio_underruns(void)      { return s_underruns; }
unsigned long gcaudio_starved(void)        { return s_starved; }
unsigned      gcaudio_load_permille(void)  { return s_load; }

/* The difference can be read without a lock: it can only understate the lead,
 * which is the cautious direction for a caller deciding whether it is allowed
 * to spend CPU time. */
unsigned gcaudio_lead(void)
{
    if (!s_running) return GCAUDIO_LEAD_MAX;   /* nothing to protect */
    return (unsigned)(s_filled - s_taken);
}

unsigned gcaudio_dsp_permille(void)
{
    /* Unlike ASND_GetDSP_PercentUse(), which mixed ticks and microseconds
     * inside libogc2 itself and returned impossible values (2110 to 3410 per
     * mille once converted), libansnd returns a genuine fraction from 0 to 1.
     * Nothing to fix up. */
    f32 usage = 0.0f;
    if (ansnd_get_dsp_usage_percent(&usage) != ANSND_ERROR_OK) return 0;
    return (unsigned)(usage * 1000.0f);
}

int gcaudio_dsp_stalled(void)
{
    /* A NEW FAILURE MODE, SPECIFIC TO LIBANSND, AND IT HAS TO BE VISIBLE.
     *
     * When the DSP falls behind, ansnd_get_dsp_usage_percent() does not return
     * a load: it returns ANSND_ERROR_DSP_STALLED. Without this test, the
     * function above would answer "0 per mille" -- a perfectly plausible value,
     * the best possible one in fact, on a dead output. That is exactly the
     * shape of trap that cost this project three weeks (docs/STATUS.md 10.4): a
     * credible number on a wrong render.
     *
     * With a single voice we should never get there -- upstream advertises 18
     * simultaneous resampled voices on GameCube. All the more reason to report
     * it if it happens. */
    return ansnd_get_dsp_usage_percent(NULL) == ANSND_ERROR_DSP_STALLED;
}

void gcaudio_flush_denormals(void)
{
    /* Gekko non-IEEE mode: FPSCR[NI] = bit 29 (PowerPC numbering, 0 = most
     * significant). Denormals are then treated as zero.
     *
     * Resonant filters produce them in quantity on release tails, and handling
     * them is a classic performance killer on the 750. It is the cheapest lever
     * there is against the CPU budget. */
    asm volatile ("mtfsb1 29");
}

#else  /* !GEKKO: host target, nothing to compile */

typedef int gcaudio_translation_unit_not_empty;

#endif
