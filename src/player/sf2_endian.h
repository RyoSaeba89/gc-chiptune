/* ------------------------------------------------------------------------
 * GC-Chiptune: endianness conversion for SF2 soundfonts.
 *
 * An .sf2 is a RIFF container, hence little-endian, and TinySoundFont reads its
 * fields by raw copy: `stream->read(&chunk->size, sizeof(tsf_u32))` (tsf.h:488)
 * for chunk sizes, and the TSFR macro (tsf.h:409-417) for the hydra's nine
 * chunks. On the Gekko, everything arrives back to front.
 *
 * The on-target measurement that closed the diagnosis (docs/STATUS.md 7.5):
 *   RIFF size as read by the machine  1,947,687,680 = 0x74175B00
 *   expected size                         5,969,780 = 0x005B1774
 *
 * The conversion is done OFF-LINE, on a PC (tools/sf2_prep). The console
 * receives a ready file. Loading = read and play.
 *
 * SCOPE: we convert exactly what tsf.h READS, and nothing else.
 *
 *   - the sizes of all RIFF/LIST chunks (tsf uses them to navigate)
 *   - the 16/32-bit fields of the hydra's nine chunks, inside pdta
 *   - the s16 samples of the smpl chunk, inside sdta
 *
 * The contents of the INFO block (ifil, isng, INAM...) are left as they are:
 * tsf skips it entirely. Accepted consequence: the resulting file is no longer
 * a valid .sf2 for anybody else. It is a file for OUR loader.
 *
 * We walk the RIFF tree exactly like tsf_riffchunk_read, including its lack of
 * even-byte padding handling: converting at offsets the loader never visits
 * would achieve nothing, and would diverge on malformed files. The SF2
 * specification mandates even sizes anyway.
 *
 * This pass also acts as a bounded VALIDATOR: soundfonts come from untrusted
 * sources and a malformed file must not crash the console.
 * ------------------------------------------------------------------------ */

#ifndef GC_SF2_ENDIAN_H_
#define GC_SF2_ENDIAN_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SF2_OK             =  0,
    SF2_ERR_ARGS       = -1,
    SF2_ERR_NOT_RIFF   = -2,  /* no RIFF....sfbk signature                  */
    SF2_ERR_TRUNCATED  = -3,  /* a chunk overflows its parent or the file   */
    SF2_ERR_RECSIZE    = -4,  /* hydra chunk size not a multiple of the step */
    SF2_ERR_NO_PDTA    = -5,  /* no pdta block: nothing to play             */
    SF2_ERR_NO_SAMPLES = -6   /* no smpl block: no sound                    */
};

/* Diagnostics filled in by sf2_convert(). Optional (NULL accepted). */
typedef struct {
    unsigned long riff_size;     /* size announced by the RIFF header        */
    unsigned long consumed;      /* bytes actually described                 */
    unsigned long error_offset;  /* offending offset on error                */
    const char   *error_chunk;   /* id of the offending chunk, NULL if none  */

    unsigned long presets;       /* phdr records                             */
    unsigned long instruments;   /* inst records                             */
    unsigned long samples;       /* shdr records                             */
    unsigned long sample_bytes;  /* size of the smpl block                   */

    unsigned long chunks;        /* chunks traversed                         */
    unsigned long swapped;       /* fields actually converted                */
} sf2_info;

/* State of a soundfont with respect to the current machine. */
typedef enum {
    SF2_STATE_PREPARED = 0,  /* native byte order: usable as-is           */
    SF2_STATE_RAW      = 1,  /* reversed order: unprepared standard .sf2  */
    SF2_STATE_UNKNOWN  = 2   /* neither: this is not an .sf2              */
} sf2_state;

/* Converts a soundfont's structure in place.
 *
 *   in_big  : 1 if the blob's integers are big-endian (a standard .sf2 is
 *             little-endian: 0)
 *   out_big : 1 to write big-endian
 *
 * If in_big == out_big, not one byte is modified: the call behaves as a pure
 * structural validator.
 *
 * On error the blob may have been partially converted: do not use it. */
int sf2_convert(unsigned char *data, size_t len,
                int in_big, int out_big, sf2_info *info);

/* Recognises a soundfont's state without modifying anything, by comparing the
 * RIFF size read in both orders against the real file size.
 *
 * Used by the player to tell "unprepared soundfont" from "unreadable file". */
sf2_state sf2_check(const unsigned char *data, size_t len);

/* True if the host machine is big-endian (evaluated at run time). */
int sf2_host_is_big_endian(void);

const char *sf2_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif /* GC_SF2_ENDIAN_H_ */
