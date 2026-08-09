/* ------------------------------------------------------------------------
 * GC-Chiptune: common interface to the playback backends.
 *
 * Two implementations behind one interface:
 *   - modules : libxmp (MOD/XM/IT/S3M and ~90 other formats)
 *   - MIDI    : TinyMidiLoader + TinySoundFont, with a GM soundfont
 *
 * THERE WERE THREE. The V2M backend (farbrausch's V2 synth) was removed: it did
 * not hold real time on the Gekko. See docs/STATUS.md 13.
 *
 * OUTPUT FORMAT: signed 16-bit, interleaved stereo, **native order**.
 *
 * That choice is not neutral. libxmp (xmp_play_buffer) and TinySoundFont
 * (tsf_render_short) already produce native s16, and that is exactly what the
 * GameCube audio DMA consumes: big-endian s16 stereo. Going through floats
 * would impose two pointless conversions.
 *
 * LOADING FROM MEMORY: the platform layer reads the file; the backends do no
 * I/O. That avoids depending on fatfs here, and it is the common denominator of
 * both libraries.
 * ------------------------------------------------------------------------ */

#ifndef GC_BACKEND_H_
#define GC_BACKEND_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GCC_FMT_UNKNOWN = 0,
    GCC_FMT_MODULE,
    GCC_FMT_MIDI
} gcc_format;

enum {
    GCC_OK            =  0,
    GCC_ERR_FORMAT    = -1,  /* format not recognised                       */
    GCC_ERR_LOAD      = -2,  /* the library refused the file                */
    GCC_ERR_MEMORY    = -3,
    GCC_ERR_NO_SF2    = -4,  /* MIDI file but no soundfont loaded           */
    GCC_ERR_ARGS      = -5
};

typedef struct gcc_backend gcc_backend;

/* Recognises the format. `path` is an extension hint and may be NULL; detection
 * relies on content first. */
gcc_format gcc_detect(const char *path, const unsigned char *data, size_t len);

/* Opens a track already in memory.
 *
 * IMPORTANT: `data` must stay valid until gcc_close(). Both current backends
 * copy what they need at open time, but the contract stays conservative -- the
 * caller reuses one buffer anyway, so honouring it costs nothing. */
int gcc_open(const void *data, size_t len, unsigned rate,
             gcc_format fmt, gcc_backend **out);

/* Renders `frames` interleaved native s16 stereo samples into `dst`, which must
 * hold 2*frames shorts. Fills with silence once the track is over. Returns the
 * number of frames the track actually produced (0 once finished); the buffer is
 * always filled completely. */
int gcc_render(gcc_backend *b, short *dst, unsigned frames);

void gcc_close(gcc_backend *b);

/* Readable name of the active backend ("libxmp", "midi"). */
const char *gcc_backend_name(const gcc_backend *b);

/* Track title if the format carries one, "" otherwise. */
const char *gcc_title(const gcc_backend *b);

/* Track duration in milliseconds, known FROM THE MOMENT IT OPENS and without
 * rendering a single sample. 0 = unknown.
 *
 * This is not an estimate. Both formats carry their length or allow it to be
 * computed exactly:
 *
 *   module  libxmp unrolls the module symbolically in xmp_start_player() and
 *           detects its loop -- the same technique as XMPlay or foobar
 *   midi    timestamp of the SMF's last event (tml_get_info)
 *
 * NOT TO BE CONFUSED with the end of playback, which is detected separately and
 * remains the only authority: gcc_render() returns 0 on the last useful sample.
 * The two can differ -- a MIDI plays past its last event while the releases die
 * out. The duration is for DISPLAY and for decisions; gcc_render()'s zero is
 * for SEQUENCING. */
unsigned long gcc_duration_ms(const gcc_backend *b);

/* Installs the soundfont used by the MIDI backend. The block must stay valid
 * for as long as a MIDI backend is open. One soundfont at a time. Passing NULL
 * uninstalls it.
 *
 * CAREFUL: the soundfont must have been prepared by tools/sf2_prep for the
 * machine's byte order. tsf.h reads the RIFF container by raw copy
 * (docs/STATUS.md 7.5); a standard soundfont fails here on Gekko. */
int gcc_midi_set_soundfont(const void *sf2, size_t len);

/* Installs the soundfont BY READING IT STRAIGHT OFF THE MEDIUM, without ever
 * holding it whole in memory. PREFER THIS ON CONSOLE.
 *
 * WHY THIS VARIANT EXISTS. gcc_midi_set_soundfont() requires the caller to have
 * read the file already: on TimGM6mb that is 5.7 MB of source block coexisting
 * with the 11.0 MB of float samples tsf allocates during the load. The peak is
 * therefore 16.7 MB out of a GameCube's ~21.5 MB arena, 11.0 MB of it
 * CONTIGUOUS -- and nothing is left for GX, the glyph atlases and the index.
 * That is what produced "soundfont refused (memory)" from a card.
 *
 * tsf_load() only reads its input FORWARDS (read + skip, tsf.h): the file has
 * no reason to be in RAM. This variant therefore goes through
 * tsf_load_filename(), and the peak drops back to 11.0 MB. 5.7 MB recovered for
 * no downside -- same result, same duration, one more sequential read from the
 * card.
 *
 * SAME BYTE-ORDER CONSTRAINT as the memory variant: the file must have been
 * prepared by tools/sf2_prep. The caller is still responsible for checking that
 * (sf2_check on the first twelve bytes is enough). */
int gcc_midi_set_soundfont_file(const char *path);

/* Releases the soundfont and gives its memory back. No-op if none is loaded.
 *
 * WHY THIS IS NAMED, AND NOT LEFT AS set_soundfont(NULL, 0). Because for months
 * nothing called it, and the 11.5 MB stayed resident from the first `.mid` to
 * power-off on a machine with a ~21.5 MB arena -- reported as "the soundfont
 * never frees, memory saturates". The uninstall path was there and worked; what
 * was missing was a caller and a name that says out loud that this is the thing
 * to call.
 *
 * MEASURED, host, TimGM6mb: tsf_load_filename() commits 11832 kB and five
 * load/close cycles come back to within 48-124 kB of the baseline. tsf_close()
 * really does return everything -- the presets, the regions and the float
 * samples.
 *
 * FORBIDDEN WHILE A MIDI BACKEND IS OPEN: the render reads the soundfont
 * directly (one shared tsf, see gcc_midi_set_soundfont). Close the backend
 * first. */
void gcc_midi_free_soundfont(void);

/* Polyphony limit of the MIDI backend. 0 = no limit (upstream default).
 *
 * This is THE CPU lever for MIDI: render cost is proportional to the number of
 * active voices, and the corpus's worst case exceeds real time on Gekko with no
 * limit (docs/STATUS.md 7.7). At the limit, tsf kills the voice furthest into
 * its release, and only drops the note if there is none -- so the degradation
 * is gradual, not abrupt.
 *
 * Two constraints coming from tsf_set_max_voices():
 *   - the value can only go UP on an already loaded soundfont; lowering it
 *     requires reloading the soundfont;
 *   - it preallocates the voices, hence a call after loading.
 * So we remember the setting and reapply it on every soundfont install. */
int gcc_midi_set_max_voices(int max_voices);

/* The limit we settled on, measured on Gekko (docs/STATUS.md 7.7).
 *
 * The corpus's worst case asks for up to 205 simultaneous voices with no limit,
 * which makes no musical sense: those are release tails piling up. 64 voices is
 * the bound of the GM/GS expanders of the era, and tsf sacrifices the voices
 * furthest into their release first -- so reverberation, not notes.
 *
 * Cost measured on the worst case, in per mille of real time (maximum):
 *
 *   voices    32 kHz    48 kHz
 *   none       914       1525     <- outside real time
 *   64         321        506
 *   48         247        386
 *   32         170        265
 *
 * 64 voices at 48 kHz leaves a factor of 2 of margin. That setting is what
 * gives MIDI 48 kHz, whereas with no limit we had to drop to 32. */
#define GCC_MIDI_DEFAULT_VOICES 64

/* Number of currently active voices, 0 if no soundfont. Used to measure a
 * track's real polyphony peak. */
int gcc_midi_voices(void);

/* --- render validity check ------------------------------------------------
 *
 * This counter exists because a bench that only measures render TIME does not
 * measure its VALIDITY. The V2M at 32 kHz was reported "ok, 756/896 per mille,
 * 0 underruns" while it was producing nothing but NaNs converted to silence. A
 * plausible CPU load on a silent render is the worst possible result: it reads
 * as a success.
 *
 * The V2M has since been removed (docs/STATUS.md 13), but the safety net stays:
 * the MIDI backend also goes through a float buffer, and nothing guarantees a
 * malformed soundfont will never produce NaNs there. The test costs one
 * comparison per sample on a path that already does several. */
unsigned long gcc_render_nans(void);
void          gcc_render_stats_reset(void);

const char *gcc_strerror(int code);

/* Detail on the last gcc_open() failure, when the underlying library gives one
 * (e.g. "corrupt module"). Static string, never NULL, empty if no detail. */
const char *gcc_last_detail(void);

#ifdef __cplusplus
}
#endif

#endif /* GC_BACKEND_H_ */
