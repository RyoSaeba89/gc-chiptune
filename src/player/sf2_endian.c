/* ------------------------------------------------------------------------
 * GC-Chiptune: SF2 soundfont endianness conversion. See sf2_endian.h.
 * ------------------------------------------------------------------------ */

#include <string.h>

#include "sf2_endian.h"

/* ---- integer access ----------------------------------------------------- */

static unsigned long rd32(const unsigned char *p, int big)
{
    return big ? ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
                 ((unsigned long)p[2] <<  8) |  (unsigned long)p[3]
               : ((unsigned long)p[3] << 24) | ((unsigned long)p[2] << 16) |
                 ((unsigned long)p[1] <<  8) |  (unsigned long)p[0];
}

static void wr32(unsigned char *p, unsigned long v, int big)
{
    if (big) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
               p[2] = (unsigned char)(v >>  8); p[3] = (unsigned char)v; }
    else     { p[3] = (unsigned char)(v >> 24); p[2] = (unsigned char)(v >> 16);
               p[1] = (unsigned char)(v >>  8); p[0] = (unsigned char)v; }
}

static unsigned rd16(const unsigned char *p, int big)
{
    return big ? (unsigned)(p[0] << 8) | p[1]
               : (unsigned)(p[1] << 8) | p[0];
}

static void wr16(unsigned char *p, unsigned v, int big)
{
    if (big) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
    else     { p[1] = (unsigned char)(v >> 8); p[0] = (unsigned char)v; }
}

static int fourcc_is(const unsigned char *p, const char *s)
{
    return p[0] == (unsigned char)s[0] && p[1] == (unsigned char)s[1] &&
           p[2] == (unsigned char)s[2] && p[3] == (unsigned char)s[3];
}

int sf2_host_is_big_endian(void)
{
    const unsigned long one = 1;
    return ((const unsigned char *)&one)[0] == 0;
}

/* ---- map of the hydra's fields ------------------------------------------ */

/* A field to convert inside a record: offset and width.
 *
 * The record sizes are the FILE's, not the C structs': tsf reads field by field
 * (the TSFR macro) and therefore does not suffer the padding the compiler would
 * introduce. These are the same constants as the ...SizeInFile enum of
 * tsf.h:1396. */
typedef struct { unsigned char off, width; } field;

/* phdr: achPresetName[20], wPreset, wBank, wPresetBagNdx, dwLibrary,
 *        dwGenre, dwMorphology */
static const field F_PHDR[] = {{20,2},{22,2},{24,2},{26,4},{30,4},{34,4},{0,0}};
/* pbag / ibag: two 16-bit indices */
static const field F_BAG [] = {{0,2},{2,2},{0,0}};
/* pmod / imod: five 16-bit fields (modAmount is signed, same treatment) */
static const field F_MOD [] = {{0,2},{2,2},{4,2},{6,2},{8,2},{0,0}};
/* inst: achInstName[20], wInstBagNdx */
static const field F_INST[] = {{20,2},{0,0}};
/* shdr: achSampleName[20], dwStart, dwEnd, dwStartloop, dwEndloop,
 *       dwSampleRate, byOriginalPitch, chPitchCorrection (8 bits: nothing to
 *       do), wSampleLink, sfSampleType */
static const field F_SHDR[] = {{20,4},{24,4},{28,4},{32,4},{36,4},{42,2},{44,2},{0,0}};

typedef struct {
    const char  *id;
    unsigned     recsize;
    const field *fields;
} hydra_spec;

/* pgen / igen have no fixed map: see swap_gen(). */

/* ---- record conversion -------------------------------------------------- */

static void swap_records(unsigned char *p, unsigned long size, unsigned recsize,
                         const field *fields, int in_big, int out_big,
                         sf2_info *info)
{
    unsigned long n = size / recsize, i;

    for (i = 0; i < n; i++, p += recsize) {
        const field *f;
        for (f = fields; f->width; f++) {
            if (f->width == 4) wr32(p + f->off, rd32(p + f->off, in_big), out_big);
            else               wr16(p + f->off, rd16(p + f->off, in_big), out_big);
            if (info) info->swapped++;
        }
    }
}

/* pgen / igen: sfGenOper (16 bits) then genAmount, a two-byte UNION.
 *
 * This is the format's only real trap. For most generators genAmount is a
 * 16-bit integer, so it must be swapped. But for keyRange (43) and velRange
 * (44) they are TWO INDEPENDENT BYTES, lo and hi: swapping them would invert
 * the bounds of every key and velocity zone. tsf does read the two forms
 * differently (genAmount.range vs .shortAmount).
 *
 * So we read genOper in the INPUT order before deciding. */
#define SF2_GEN_KEYRANGE 43
#define SF2_GEN_VELRANGE 44

static void swap_gen(unsigned char *p, unsigned long size,
                     int in_big, int out_big, sf2_info *info)
{
    unsigned long n = size / 4, i;

    for (i = 0; i < n; i++, p += 4) {
        unsigned oper = rd16(p, in_big);
        wr16(p, oper, out_big);
        if (info) info->swapped++;

        if (oper != SF2_GEN_KEYRANGE && oper != SF2_GEN_VELRANGE) {
            wr16(p + 2, rd16(p + 2, in_big), out_big);
            if (info) info->swapped++;
        }
    }
}

/* ---- walking the RIFF tree ---------------------------------------------- */

typedef struct {
    int        in_big, out_big;
    sf2_info  *info;
    int        seen_pdta, seen_smpl;
    int        err;
    const char *err_chunk;
    unsigned long err_off;
} ctx;

static void fail(ctx *c, int err, const unsigned char *at,
                 const unsigned char *base, const char *id)
{
    if (c->err) return;                   /* keep the first cause */
    c->err       = err;
    c->err_chunk = id;
    c->err_off   = (unsigned long)(at - base);
}

/* Walks the body of a LIST/RIFF. `p` points just past the type fourcc.
 * `in_pdta` / `in_sdta` say which block we are inside. */
static void walk(ctx *c, unsigned char *base, unsigned char *p,
                 unsigned long size, int in_pdta, int in_sdta)
{
    unsigned char *end = p + size;

    while (p + 8 <= end) {
        unsigned char *id   = p;
        unsigned long  csz  = rd32(p + 4, c->in_big);
        unsigned char *body = p + 8;

        /* The size is converted even if the chunk does not interest us: it is
         * what tsf navigates by. */
        wr32(p + 4, csz, c->out_big);
        if (c->info) { c->info->chunks++; c->info->swapped++; }

        if (csz > (unsigned long)(end - body)) {
            fail(c, SF2_ERR_TRUNCATED, p, base, "?");
            return;
        }

        if (fourcc_is(id, "LIST")) {
            if (csz < 4) { fail(c, SF2_ERR_TRUNCATED, p, base, "LIST"); return; }
            walk(c, base, body + 4, csz - 4,
                 in_pdta || fourcc_is(body, "pdta"),
                 in_sdta || fourcc_is(body, "sdta"));
            if (c->err) return;
        } else if (in_sdta && fourcc_is(id, "smpl")) {
            /* Samples are little-endian s16, read as-is and then converted to
             * float in place (tsf.h:999). */
            unsigned long i, n = csz / 2;
            unsigned char *s = body;
            for (i = 0; i < n; i++, s += 2) wr16(s, rd16(s, c->in_big), c->out_big);
            if (c->info) {
                c->info->sample_bytes = csz;
                c->info->swapped += n;
            }
            c->seen_smpl = 1;
        } else if (in_pdta) {
            static const hydra_spec specs[] = {
                { "phdr", 38, F_PHDR },
                { "pbag",  4, F_BAG  },
                { "pmod", 10, F_MOD  },
                { "inst", 22, F_INST },
                { "ibag",  4, F_BAG  },
                { "imod", 10, F_MOD  },
                { "shdr", 46, F_SHDR },
            };
            unsigned k;

            if (fourcc_is(id, "pgen") || fourcc_is(id, "igen")) {
                if (csz % 4) { fail(c, SF2_ERR_RECSIZE, p, base, "pgen"); return; }
                swap_gen(body, csz, c->in_big, c->out_big, c->info);
            } else {
                for (k = 0; k < sizeof specs / sizeof specs[0]; k++) {
                    if (!fourcc_is(id, specs[k].id)) continue;
                    /* tsf simply ignores a chunk whose size is not a multiple
                     * of the record size (the !(chunk.size % ...SizeInFile)
                     * test of tsf.h:1386). We refuse rather than let a
                     * truncated soundfont through. */
                    if (csz % specs[k].recsize) {
                        fail(c, SF2_ERR_RECSIZE, p, base, specs[k].id);
                        return;
                    }
                    swap_records(body, csz, specs[k].recsize, specs[k].fields,
                                 c->in_big, c->out_big, c->info);
                    if (c->info) {
                        unsigned long n = csz / specs[k].recsize;
                        if      (fourcc_is(id, "phdr")) c->info->presets     = n;
                        else if (fourcc_is(id, "inst")) c->info->instruments = n;
                        else if (fourcc_is(id, "shdr")) c->info->samples     = n;
                    }
                    break;
                }
            }
            c->seen_pdta = 1;
        }

        /* No even-byte padding: we walk like tsf_riffchunk_read, which does
         * not account for it either (tsf.h:490). */
        p = body + csz;
    }

    if (c->info) {
        unsigned long done = (unsigned long)(p - base);
        if (done > c->info->consumed) c->info->consumed = done;
    }
}

/* ---- API ---------------------------------------------------------------- */

int sf2_convert(unsigned char *data, size_t len, int in_big, int out_big,
                sf2_info *info)
{
    ctx           c;
    unsigned long riff;

    if (info) memset(info, 0, sizeof *info);
    if (!data || len < 12) return SF2_ERR_ARGS;

    if (!fourcc_is(data, "RIFF") || !fourcc_is(data + 8, "sfbk"))
        return SF2_ERR_NOT_RIFF;

    riff = rd32(data + 4, in_big);
    if (info) info->riff_size = riff;

    /* The RIFF size covers everything after the field itself, including the
     * "sfbk" fourcc. We clamp to the real file: several commercial soundfonts
     * announce a few bytes more than they contain. */
    if (riff + 8 > len) riff = (unsigned long)len - 8;
    wr32(data + 4, riff, out_big);

    memset(&c, 0, sizeof c);
    c.in_big  = in_big;
    c.out_big = out_big;
    c.info    = info;
    if (info) { info->chunks = 1; info->swapped = 1; info->consumed = 12; }

    walk(&c, data, data + 12, riff - 4, 0, 0);

    if (c.err) {
        if (info) { info->error_offset = c.err_off; info->error_chunk = c.err_chunk; }
        return c.err;
    }
    if (!c.seen_pdta) return SF2_ERR_NO_PDTA;
    if (!c.seen_smpl) return SF2_ERR_NO_SAMPLES;
    return SF2_OK;
}

sf2_state sf2_check(const unsigned char *data, size_t len)
{
    int           host = sf2_host_is_big_endian();
    unsigned long native, swapped, want;

    if (!data || len < 12) return SF2_STATE_UNKNOWN;
    if (!fourcc_is(data, "RIFF") || !fourcc_is(data + 8, "sfbk"))
        return SF2_STATE_UNKNOWN;

    want    = (unsigned long)len - 8;
    native  = rd32(data + 4, host);
    swapped = rd32(data + 4, !host);

    /* Tolerance downwards only: a RIFF size smaller than the file is common
     * (data appended afterwards), larger is not. The criterion discriminates
     * comfortably -- the gap between the two readings is several orders of
     * magnitude. */
    if (native  <= want && native  >= want / 2) return SF2_STATE_PREPARED;
    if (swapped <= want && swapped >= want / 2) return SF2_STATE_RAW;
    return SF2_STATE_UNKNOWN;
}

const char *sf2_strerror(int code)
{
    switch (code) {
    case SF2_OK:             return "ok";
    case SF2_ERR_ARGS:       return "invalid arguments";
    case SF2_ERR_NOT_RIFF:   return "not a RIFF/sfbk soundfont";
    case SF2_ERR_TRUNCATED:  return "truncated chunk";
    case SF2_ERR_RECSIZE:    return "inconsistent chunk size";
    case SF2_ERR_NO_PDTA:    return "no pdta block";
    case SF2_ERR_NO_SAMPLES: return "no samples";
    default:                 return "unknown error";
    }
}
