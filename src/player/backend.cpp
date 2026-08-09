/* ------------------------------------------------------------------------
 * GC-Chiptune: implementation of the two backends. See backend.h.
 * ------------------------------------------------------------------------ */

#include "backend.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "xmp.h"
}

#define TSF_IMPLEMENTATION
#include "tsf.h"
#define TML_IMPLEMENTATION
#include "tml.h"

/* MIDI rendering advances in short blocks so events land on the right sample.
 * 64 is TinySoundFont's internal effect block. */
#define MIDI_BLOCK 64

struct gcc_backend {
    gcc_format   fmt;
    unsigned     rate;
    char         title[64];
    unsigned long duration_ms;   /* 0 = unknown; see gcc_duration_ms() */

    float       *fbuf;        /* intermediate float buffer (MIDI) */
    unsigned     fbuf_frames;

    /* modules */
    xmp_context  xmp;
    int          xmp_started;

    /* MIDI */
    tml_message *midi;        /* list head, to be freed */
    tml_message *cur;         /* next event             */
    double       ms;          /* playback clock         */
    int          finished;
};

/* Shared soundfont: one at a time, rarely reloaded. */
static tsf *g_sf = NULL;

/* Detail on the last failure, for display. Always points at a string literal:
 * no allocation, no copy. */
static const char *g_detail = "";

/* Soft clipping, threshold around -3 dB.
 *
 * A complete GM soundfont regularly exceeds full scale as soon as a track
 * stacks voices at full velocity, and TinySoundFont does not limit: it
 * truncates. Lowering the global gain enough for every case would make the
 * whole thing too quiet, so we keep -6 dB and round off the peaks.
 *
 * Below the threshold the signal passes intact (no transcendental on that
 * path); above it, a hyperbolic tangent compresses towards 1.0 without ever
 * reaching it abruptly. */
static float soft_clip(float x)
{
    const float t = 0.70f;
    float a;
    if (x >= -t && x <= t) return x;
    a = (x < 0.0f) ? -x : x;
    a = t + (1.0f - t) * tanhf((a - t) / (1.0f - t));
    return (x < 0.0f) ? -a : a;
}

/* Ensures a float buffer of at least `frames` stereo samples. */
static float *ensure_fbuf(gcc_backend *b, unsigned frames);

/* ---------------------------------------------------------------- detection */

/* Reads a 32-bit little-endian integer (RIFF chunk sizes always are, whatever
 * the machine). */
static unsigned long rd_le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* Unwraps a RIFF-MIDI (RMID): an SMF file encapsulated in a RIFF container,
 * "RIFF"<size>"RMID" followed by chunks whose "data" holds the SMF. Returns 1
 * if the MIDI content was located. The Windows .mid files produced by some
 * tools are in this format; TinyMidiLoader only accepts a bare "MThd". */
static int midi_payload(const unsigned char *data, size_t len,
                        const unsigned char **out, size_t *outlen)
{
    size_t p;

    *out = data; *outlen = len;
    if (len >= 4 && !memcmp(data, "MThd", 4)) return 1;

    if (len < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "RMID", 4))
        return 0;

    for (p = 12; p + 8 <= len; ) {
        unsigned long sz = rd_le32(data + p + 4);
        const unsigned char *body = data + p + 8;
        size_t avail = len - (p + 8);
        if (sz > avail) sz = (unsigned long)avail;      /* truncated chunk */
        if (!memcmp(data + p, "data", 4) && sz >= 4 && !memcmp(body, "MThd", 4)) {
            *out = body; *outlen = (size_t)sz;
            return 1;
        }
        p += 8 + sz + (sz & 1);                          /* even-sized chunks */
    }
    return 0;
}

gcc_format gcc_detect(const char *path, const unsigned char *data, size_t len)
{
    const unsigned char *mid; size_t midlen;

    /* `path` is no longer used: both remaining formats are recognised by
     * content. It was a last-resort hint for the V2M, which has no signature --
     * the only one of the three in that position. */
    (void)path;

    if (!data || len < 16) return GCC_FMT_UNKNOWN;

    /* MIDI: bare "MThd", or wrapped in a RIFF/RMID container. */
    if (midi_payload(data, len, &mid, &midlen)) return GCC_FMT_MIDI;

    /* Modules: libxmp can pass judgement on ~90 formats. */
    if (xmp_test_module_from_memory((void *)data, (long)len, NULL) == 0)
        return GCC_FMT_MODULE;

    return GCC_FMT_UNKNOWN;
}

/* --------------------------------------------------------------- ouverture */

static int open_module(gcc_backend *b, const void *data, size_t len)
{
    struct xmp_module_info mi;
    int rc;

    b->xmp = xmp_create_context();
    if (!b->xmp) return GCC_ERR_MEMORY;
    rc = xmp_load_module_from_memory(b->xmp, (void *)data, (long)len);
    if (rc != 0) {
        /* Telling the causes apart helps to know whether a refusal comes from
         * the file or from a missing component (the depackers are deliberately
         * excluded). */
        switch (-rc) {
            case XMP_ERROR_FORMAT: g_detail = "format not supported by libxmp"; break;
            case XMP_ERROR_LOAD:   g_detail = "corrupt module";                  break;
            case XMP_ERROR_DEPACK: g_detail = "packed module (depackers excluded)"; break;
            case XMP_ERROR_SYSTEM: g_detail = "system error";                    break;
            default:               g_detail = "libxmp refusal";                  break;
        }
        xmp_free_context(b->xmp); b->xmp = NULL;
        return GCC_ERR_LOAD;
    }
    if (xmp_start_player(b->xmp, (int)b->rate, 0) != 0) {
        xmp_release_module(b->xmp); xmp_free_context(b->xmp); b->xmp = NULL;
        return GCC_ERR_LOAD;
    }
    b->xmp_started = 1;

    /* HEADROOM. Without it, libxmp saturates inside its own mixer.
     *
     * Measured over 400 modules from the pack: 49 of them -- 12 % -- produce
     * samples pinned to the 16-bit rail, up to 2.2 % of the track in the worst
     * case. It is not heard as outright distortion but as a continuous
     * background fizz, and that is exactly what was reported while listening.
     *
     * This is not a libxmp defect: the format does not bound the sum of the
     * channels, and a module stacking eight voices at full volume exceeds full
     * scale. Every player takes headroom -- it is XMPlay's amplification
     * setting. XMP_PLAYER_VOLUME acts BEFORE the conversion to 16 bits, so
     * before saturation: lowering here avoids clipping instead of rounding it
     * off afterwards.
     *
     * 50 = -6 dB, the same headroom as the MIDI backend, so levels do not jump
     * between formats as tracks follow each other. Measured at this setting:
     * 1 file in 400 still clips, 8 samples in 30 s -- inaudible. At 70 there
     * were 8 files and 2786 samples left. */
    xmp_set_player(b->xmp, XMP_PLAYER_VOLUME, 50);
    xmp_get_module_info(b->xmp, &mi);

    /* Exact duration, and free: xmp_start_player already unrolls the module
     * symbolically to detect its loop -- the same technique as XMPlay or
     * foobar. seq_data[0] is the main sequence, the one starting from order 0;
     * the others are orphan sections reachable only by a jump, which the player
     * never plays. */
    if (mi.num_sequences > 0 && mi.seq_data)
        b->duration_ms = (unsigned long)mi.seq_data[0].duration;

    if (mi.mod && mi.mod->name[0]) {
        strncpy(b->title, mi.mod->name, sizeof(b->title) - 1);
        b->title[sizeof(b->title) - 1] = 0;
    }
    return GCC_OK;
}

static int open_midi(gcc_backend *b, const void *data, size_t len)
{
    const unsigned char *mid; size_t midlen;

    if (!g_sf) return GCC_ERR_NO_SF2;

    if (!midi_payload((const unsigned char *)data, len, &mid, &midlen))
        return GCC_ERR_LOAD;

    b->midi = tml_load_memory(mid, (int)midlen);
    if (!b->midi) return GCC_ERR_LOAD;
    b->cur = b->midi;
    b->ms  = 0.0;

    /* Exact duration: an SMF carries its timings, and tml_get_info returns the
     * last event's. The release tail of the final notes comes on top -- the
     * render plays it (see tsf_active_voice_count in render_midi), it is not
     * counted here. Typical difference: one to two seconds. */
    {
        unsigned int len_ms = 0;
        tml_get_info(b->midi, NULL, NULL, NULL, NULL, &len_ms);
        b->duration_ms = len_ms;
    }

    tsf_reset(g_sf);
    /* Global gain at -6 dB. At 0 dB a complete GM soundfont clips as soon as a
     * track stacks a few voices at full velocity: measured peak = 1.000 on the
     * first .mid of the corpus. TinySoundFont does not limit, it truncates. */
    tsf_set_output(g_sf, TSF_STEREO_INTERLEAVED, (int)b->rate, -6.0f);
    /* Channel 10 (index 9) = percussion in General MIDI: bank 128. */
    tsf_channel_set_bank_preset(g_sf, 9, 128, 0);
    return GCC_OK;
}

int gcc_open(const void *data, size_t len, unsigned rate,
             gcc_format fmt, gcc_backend **out)
{
    gcc_backend *b;
    int rc;

    if (!data || !out || !rate) return GCC_ERR_ARGS;
    *out = NULL;

    if (fmt == GCC_FMT_UNKNOWN) fmt = gcc_detect(NULL, (const unsigned char *)data, len);
    if (fmt == GCC_FMT_UNKNOWN) return GCC_ERR_FORMAT;

    b = (gcc_backend *)calloc(1, sizeof *b);
    if (!b) return GCC_ERR_MEMORY;
    b->fmt = fmt;
    b->rate = rate;

    switch (fmt) {
        case GCC_FMT_MODULE: rc = open_module(b, data, len); break;
        case GCC_FMT_MIDI:   rc = open_midi(b, data, len);   break;
        default:             rc = GCC_ERR_FORMAT;            break;
    }
    if (rc != GCC_OK) { gcc_close(b); return rc; }

    *out = b;
    return GCC_OK;
}

/* ------------------------------------------------------------------ rendu */

static float *ensure_fbuf(gcc_backend *b, unsigned frames)
{
    if (b->fbuf_frames < frames) {
        free(b->fbuf);
        b->fbuf = (float *)malloc(sizeof(float) * frames * 2);
        b->fbuf_frames = b->fbuf ? frames : 0;
    }
    return b->fbuf;
}

/* Non-numeric samples seen since the last reset. See backend.h. */
static unsigned long g_render_nans = 0;

unsigned long gcc_render_nans(void)        { return g_render_nans; }
void          gcc_render_stats_reset(void) { g_render_nans = 0; }

static int render_module(gcc_backend *b, short *dst, unsigned frames)
{
    /* xmp_play_buffer takes a size in BYTES. The last argument is a NUMBER OF
     * PASSES, not a boolean: libxmp's stop condition is
     * "loop > 0 && fi.loop_count >= loop" (player.c:2196), so 0 DISABLES
     * detection and plays forever. 1 = stop at the end of the first pass, which
     * is exactly the duration announced by seq_data[0].duration -- the progress
     * bar and the end of the track then say the same thing. Measured before the
     * fix: not one module in the corpus stopped, not even after 50 minutes; it
     * was main.c's watchdog cutting them off, in the middle of a loop. */
    int rc = xmp_play_buffer(b->xmp, dst, (int)(frames * 2 * sizeof(short)), 1);
    if (rc != 0) { memset(dst, 0, frames * 4); return 0; }
    return (int)frames;
}

static int render_midi(gcc_backend *b, short *dst, unsigned frames)
{
    unsigned done = 0, i, n2 = frames * 2;
    int alive = 0;
    float *fb = ensure_fbuf(b, frames);

    if (!fb) { memset(dst, 0, frames * 4); return 0; }

    while (done < frames) {
        unsigned n = frames - done;
        if (n > MIDI_BLOCK) n = MIDI_BLOCK;

        b->ms += n * (1000.0 / (double)b->rate);
        while (b->cur && b->cur->time <= b->ms) {
            switch (b->cur->type) {
                case TML_PROGRAM_CHANGE:
                    tsf_channel_set_presetnumber(g_sf, b->cur->channel,
                                                 b->cur->program,
                                                 (b->cur->channel == 9));
                    break;
                case TML_NOTE_ON:
                    tsf_channel_note_on(g_sf, b->cur->channel, b->cur->key,
                                        b->cur->velocity / 127.0f);
                    break;
                case TML_NOTE_OFF:
                    tsf_channel_note_off(g_sf, b->cur->channel, b->cur->key);
                    break;
                case TML_PITCH_BEND:
                    tsf_channel_set_pitchwheel(g_sf, b->cur->channel, b->cur->pitch_bend);
                    break;
                case TML_CONTROL_CHANGE:
                    tsf_channel_midi_control(g_sf, b->cur->channel, b->cur->control,
                                             b->cur->control_value);
                    break;
                default:
                    break;
            }
            b->cur = b->cur->next;
        }

        /* Rendered in float so peaks can be rounded off before the 16-bit
         * quantisation. */
        tsf_render_float(g_sf, fb + done * 2, (int)n, 0);
        done += n;
    }

    for (i = 0; i < n2; i++) {
        float v = fb[i];
        int   s;
        /* NaN -> silence, BUT counted: replacing without counting turns an
         * outright fault into a silent one. See gcc_render_nans(). */
        if (!(v == v)) { v = 0.0f; g_render_nans++; }
        v = soft_clip(v);
        s = (int)(v * 32767.0f);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        dst[i] = (short)s;
    }

    /* The track lives as long as events or active voices remain. */
    alive = (b->cur != NULL) || (tsf_active_voice_count(g_sf) > 0);
    if (!alive) b->finished = 1;
    return alive ? (int)frames : 0;
}

int gcc_render(gcc_backend *b, short *dst, unsigned frames)
{
    if (!b || !dst) return 0;
    if (!frames) return 0;

    switch (b->fmt) {
        case GCC_FMT_MODULE: return render_module(b, dst, frames);
        case GCC_FMT_MIDI:   return render_midi(b, dst, frames);
        default: break;
    }
    memset(dst, 0, frames * 4);
    return 0;
}

/* --------------------------------------------------------------- fermeture */

void gcc_close(gcc_backend *b)
{
    if (!b) return;

    free(b->fbuf);

    if (b->xmp) {
        if (b->xmp_started) xmp_end_player(b->xmp);
        xmp_release_module(b->xmp);
        xmp_free_context(b->xmp);
    }

    if (b->midi) tml_free(b->midi);

    free(b);
}

const char *gcc_backend_name(const gcc_backend *b)
{
    if (!b) return "?";
    switch (b->fmt) {
        case GCC_FMT_MODULE: return "libxmp";
        case GCC_FMT_MIDI:   return "midi";
        default:             return "?";
    }
}

const char *gcc_title(const gcc_backend *b) { return b ? b->title : ""; }

unsigned long gcc_duration_ms(const gcc_backend *b)
{
    return b ? b->duration_ms : 0;
}

const char *gcc_last_detail(void) { return g_detail; }

/* Desired polyphony limit, 0 = none. Remembered here because
 * tsf_set_max_voices() preallocates the voices and therefore has to be called
 * again after every soundfont load. */
static int g_max_voices = 0;

/* Settings common to both load variants, once `g_sf` is in place. */
static int soundfont_installed(void)
{
    if (!g_sf) return GCC_ERR_LOAD;

    if (g_max_voices > 0 && !tsf_set_max_voices(g_sf, g_max_voices)) {
        tsf_close(g_sf);
        g_sf = NULL;
        return GCC_ERR_MEMORY;
    }
    return GCC_OK;
}

int gcc_midi_set_soundfont(const void *sf2, size_t len)
{
    if (g_sf) { tsf_close(g_sf); g_sf = NULL; }
    if (!sf2 || !len) return GCC_OK;

    g_sf = tsf_load_memory(sf2, (int)len);
    return soundfont_installed();
}

int gcc_midi_set_soundfont_file(const char *path)
{
    if (g_sf) { tsf_close(g_sf); g_sf = NULL; }
    if (!path) return GCC_OK;

    /* The file is never held in memory: tsf_load_filename() opens a stream and
     * tsf_load() only walks it forwards. See backend.h. */
    g_sf = tsf_load_filename(path);
    return soundfont_installed();
}

void gcc_midi_free_soundfont(void)
{
    if (!g_sf) return;
    tsf_close(g_sf);
    g_sf = NULL;
}

int gcc_midi_set_max_voices(int max_voices)
{
    if (max_voices < 0) return GCC_ERR_ARGS;
    g_max_voices = max_voices;

    /* On an already loaded soundfont, tsf_set_max_voices can only raise the
     * limit (newVoiceNum = max(voiceNum, max_voices), tsf.h:1543). A lower
     * value will only take effect on the next load -- which is why the bench
     * reloads the soundfont for every phase. */
    if (g_sf && max_voices > 0 && !tsf_set_max_voices(g_sf, max_voices))
        return GCC_ERR_MEMORY;
    return GCC_OK;
}

int gcc_midi_voices(void)
{
    return g_sf ? tsf_active_voice_count(g_sf) : 0;
}

const char *gcc_strerror(int code)
{
    switch (code) {
        case GCC_OK:         return "ok";
        case GCC_ERR_FORMAT: return "format not recognised";
        case GCC_ERR_LOAD:   return "load refused";
        case GCC_ERR_MEMORY: return "out of memory";
        case GCC_ERR_NO_SF2: return "no soundfont loaded";
        case GCC_ERR_ARGS:   return "invalid arguments";
        default:             return "unknown error";
    }
}
