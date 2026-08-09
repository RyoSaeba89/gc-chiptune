/* ------------------------------------------------------------------------
 * GC-Chiptune: GameCube audio output (libogc2 + libansnd).
 *
 * The need boils down to a stereo callback. It is served by libansnd, the audio
 * library recommended by the libogc2 maintainer: samples are staged in ARAM and
 * fetched by the DSP accelerator, with no copy left on the Gekko's shoulders.
 * The details of the path, and why it changed twice, are at the top of
 * audio_gc.c.
 *
 * Hardware constraints:
 *   - format: signed 16-bit, interleaved stereo, BIG-ENDIAN
 *   - buffers must be 32-byte aligned and a multiple of 32 bytes long
 *     (Gekko cache line)
 *   - they must be flushed from cache (DCFlushRange) before transfer, or the
 *     DMA reads stale memory
 *
 * Since the output is natively big-endian and backend.h produces s16 in native
 * order, there is NO conversion at all between the render and the hardware.
 *
 * No latency constraint: this is a player, not a game. We can therefore take
 * large buffers, which absorbs render load peaks.
 * ------------------------------------------------------------------------ */

#ifndef GC_AUDIO_GC_H_
#define GC_AUDIO_GC_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Fills `frames` interleaved native s16 stereo samples into `dst`. Called from
 * the main loop (see gcaudio_service_step), never from interrupt context.
 * Returning 0 signals the end of the track. */
typedef int (*gcaudio_fill)(void *user, short *dst, unsigned frames);

/* Initialises the output.
 *
 * rate           : RENDER frequency, 32000 or 48000. This is no longer the
 *                  hardware rate: libansnd pins the AI at 48 kHz once and for
 *                  all, and the DSP resamples. The rate is never switched
 *                  mid-flight -- a heavy and fragile operation on real
 *                  hardware.
 * frames_per_buf : buffer size in stereo frames. Rounded up to stay aligned on
 *                  a cache line.
 *
 * Returns 0 on success. */
int  gcaudio_init(unsigned rate, unsigned frames_per_buf,
                  gcaudio_fill fill, void *user);

void gcaudio_start(void);
void gcaudio_stop(void);
void gcaudio_shutdown(void);

/* RENDERS AT MOST ONE BLOCK. Returns 1 if a block was produced, 0 if the ring
 * is already full (or the output is not running).
 *
 * This granularity is the API's whole reason for existing. Rendering happens
 * here, in the main loop, NOT in the DSP interrupt: a block can cost tens of
 * milliseconds, which has no business in an interrupt handler. But it also has
 * no business monopolising the loop, because the same loop reads the pad. The
 * caller therefore drives the fill one block at a time and can service input
 * between two of them.
 *
 * Do not confuse "produce until full" with "produce as fast as possible". The
 * ring stops the first from running away; nothing stops the second from
 * starving the user interface. */
int  gcaudio_service_step(void);

/* Re-arms the voice if it has run dry. Call once after a burst of
 * gcaudio_service_step(). Cheap, and a no-op when the voice is healthy. */
void gcaudio_service_arm(void);

/* Convenience: step until the ring is full, then arm. Only appropriate where
 * nothing else competes for the loop -- the measurement bench does, the player
 * does not. */
void gcaudio_service(void);

/* Enables the Gekko's non-IEEE mode (FPSCR[NI]): denormals are treated as zero.
 * Resonant filters generate them in quantity on release tails, and handling
 * them is a classic performance killer on the PPC 750. Call once at start-up,
 * and on every thread that renders audio (FPSCR is per context). */
void gcaudio_flush_denormals(void);

/* Output volume, 0 to 100. Applied by the DSP, so it costs no Gekko time and
 * does not degrade the render -- unlike scaling the samples, which would lose
 * low-order bits.
 *
 * The setting survives a track change: gcaudio_init() does not reset it to
 * 100. */
void     gcaudio_set_volume(unsigned percent);
unsigned gcaudio_volume(void);

/* Pause. The voice freezes, the ring stays put, resuming is immediate. */
void gcaudio_set_paused(int paused);
int  gcaudio_paused(void);

/* Real buffer size after alignment, in stereo frames. */
unsigned gcaudio_frames_per_buf(void);

/* How many times the voice has run dry since start-up.
 *
 * CAREFUL, this counter has changed definition. The very first one only saw
 * "the render did not keep up" and stayed at ZERO while the console crackled at
 * 18 per mille of load: the buffer was ready, it was the DMA that ran empty. It
 * now measures the condition you can hear -- the voice exhausted its buffers,
 * which libansnd reports plainly as ANSND_VOICE_STATE_FINISHED. */
unsigned long gcaudio_underruns(void);

/* DSP cycles where the ring was EMPTY: the DSP asked for a block and there was
 * none.
 *
 * This is the counter to look at first. gcaudio_underruns() only sees the voice
 * stopped (VOICE_STATE_FINISHED), a state we come out of too quickly for it to
 * be reliable: during the endurance run, a track that stretched from 20 to 36 %
 * left it at ZERO from start to finish. This one counted.
 *
 * The third counter in this project to have lied by omission. The rule that
 * follows: measure the condition itself, not a state derived from it. */
unsigned long gcaudio_starved(void);

/* DSP load in per mille, mixing and resampling included.
 *
 * That work costs no Gekko time -- which is the whole point -- but the DSP has
 * its own ceiling, and we now lean on it. */
unsigned gcaudio_dsp_permille(void);

/* True if the DSP has stalled. READ THIS BEFORE gcaudio_dsp_permille(): that
 * one then answers 0, i.e. the best possible value, on a dead output. A
 * credible number on a broken render is the worst result an instrument can
 * produce -- this project has already paid for that. */
int gcaudio_dsp_stalled(void);

/* CPU cost of the last fill, in per mille of the real time available.
 * 1000 = the render takes exactly as long as the audio it produces (the
 * limit). */
unsigned gcaudio_load_permille(void);

/* Load accumulated since the last gcaudio_stats_reset().
 *
 * The instantaneous value is not enough to conclude anything: it swings widely
 * with the density of the track (measured: 801 per mille at one instant, 1014
 * at another). The maximum is what drives underruns, and the average is what
 * says whether any margin is left. */
void     gcaudio_stats_reset(void);
unsigned gcaudio_load_max(void);
unsigned gcaudio_load_avg(void);

/* Blocks rendered ahead and not yet claimed by the DSP, from 0 to
 * GCAUDIO_LEAD_MAX. This is the lead the decoder has to work with.
 *
 * It doubles as a SWITCH FOR THE INTERFACE: nothing optional should run while
 * this value is falling. The caller decides what "optional" means -- in the
 * player it is the frame, and even then only up to a bounded delay, because a
 * screen that stops refreshing reads as a crash. */
unsigned gcaudio_lead(void);
#define GCAUDIO_LEAD_MAX 4

#ifdef __cplusplus
}
#endif

#endif /* GC_AUDIO_GC_H_ */
