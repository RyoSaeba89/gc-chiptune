/* ------------------------------------------------------------------------
 * GC-Chiptune: implementation of font_gx.h.
 * ------------------------------------------------------------------------ */

#include "font_gx.h"
#include "ui.h"          /* gcchip_font / gcchip_font_size */

#include <grrlib.h>
#include <string.h>
#include <stdlib.h>

#include <ft2build.h>
#include FT_FREETYPE_H

/* Range covered. The pack's names are ASCII with rare exceptions; we take all
 * of Latin-1 anyway, the atlas stays small and a missing character would leave
 * an unexplained hole. */
#define FX_FIRST 32
#define FX_LAST  255
#define FX_COUNT (FX_LAST - FX_FIRST + 1)
#define FX_COLS  16
#define FX_ROWS  ((FX_COUNT + FX_COLS - 1) / FX_COLS)

static const int fx_px[FX_NSIZES] = { 14, 18, 24 };

typedef struct {
    s16 advance;      /* how far to advance the cursor    */
} fx_glyph;

typedef struct {
    GRRLIB_texImg *tex;
    fx_glyph       g[FX_COUNT];
    int            cellw, cellh;
    int            line;          /* line spacing */
    int            ok;
} fx_atlas;

static fx_atlas s_atlas[FX_NSIZES];

/* Round up to a multiple of 4: GX tiles its textures in 4x4 blocks, and
 * GRRLIB_CreateEmptyTexture assumes dimensions that divide evenly. */
static u32 round4(u32 v) { return (v + 3u) & ~3u; }

/* Rasterises a whole size into a tile texture.
 *
 * Every glyph is placed in its cell at a CONSTANT ORIGIN: the bearing on x, the
 * ascender minus the vertical bearing on y. Placing the tile at (x, y) is then
 * enough to position the glyph correctly, without carrying the metrics all the
 * way to the drawing code -- only the advance is still needed. */
static int build_one(FT_Face face, int idx)
{
    fx_atlas *a = &s_atlas[idx];
    int       px = fx_px[idx];
    int       ascender, descender, maxadv = 0;
    int       c;
    u32       tw, th;

    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px) != 0) return -1;

    ascender  = (int)(face->size->metrics.ascender  >> 6);
    descender = (int)(-(face->size->metrics.descender >> 6));
    a->line   = (int)(face->size->metrics.height >> 6);

    /* A first pass to size the cell: the font is proportional, so the cell has
     * to hold the widest glyph. */
    for (c = FX_FIRST; c <= FX_LAST; c++) {
        if (FT_Load_Char(face, (FT_ULong)c, FT_LOAD_RENDER) != 0) continue;
        {
            int adv   = (int)(face->glyph->advance.x >> 6);
            int right = face->glyph->bitmap_left + (int)face->glyph->bitmap.width;
            if (adv   > maxadv) maxadv = adv;
            if (right > maxadv) maxadv = right;
        }
    }

    a->cellw = maxadv + 1;
    a->cellh = ascender + descender + 1;
    if (a->cellw < 1) a->cellw = 1;
    if (a->cellh < 1) a->cellh = 1;

    tw = round4((u32)(a->cellw * FX_COLS));
    th = round4((u32)(a->cellh * FX_ROWS));

    a->tex = GRRLIB_CreateEmptyTexture(tw, th);
    if (!a->tex) return -1;
    GRRLIB_ClearTex(a->tex);

    for (c = FX_FIRST; c <= FX_LAST; c++) {
        int  n  = c - FX_FIRST;
        int  cx = (n % FX_COLS) * a->cellw;
        int  cy = (n / FX_COLS) * a->cellh;
        int  bx, by, row, col;

        if (FT_Load_Char(face, (FT_ULong)c, FT_LOAD_RENDER) != 0) {
            a->g[n].advance = 0;
            continue;
        }
        a->g[n].advance = (s16)(face->glyph->advance.x >> 6);

        bx = face->glyph->bitmap_left;
        by = ascender - face->glyph->bitmap_top;
        if (bx < 0) bx = 0;
        if (by < 0) by = 0;

        /* pitch can be negative (bottom-up bitmap). It does not happen with
         * FT_RENDER_MODE_NORMAL, but the indexing below would then read before
         * the buffer: no reason to depend on that detail. */
        if (face->glyph->bitmap.pitch <= 0) continue;

        for (row = 0; row < (int)face->glyph->bitmap.rows; row++) {
            for (col = 0; col < (int)face->glyph->bitmap.width; col++) {
                u8  cov = face->glyph->bitmap.buffer[row * face->glyph->bitmap.pitch + col];
                int px_x = cx + bx + col;
                int px_y = cy + by + row;

                if (!cov) continue;
                if (px_x >= cx + a->cellw || px_y >= cy + a->cellh) continue;
                /* White, alpha = coverage: the tint comes from
                 * GRRLIB_DrawTile's `color` parameter, which modulates. One
                 * atlas therefore serves every colour. */
                GRRLIB_SetPixelTotexImg(px_x, px_y, a->tex,
                                        0xFFFFFF00u | (u32)cov);
            }
        }
    }

    GRRLIB_FlushTex(a->tex);
    GRRLIB_InitTileSet(a->tex, (u32)a->cellw, (u32)a->cellh, 0);
    a->ok = 1;
    return 0;
}

int fx_init(void)
{
    FT_Library lib;
    FT_Face    face;
    int        i, bad = 0;

    if (FT_Init_FreeType(&lib) != 0) return -1;
    if (FT_New_Memory_Face(lib, gcchip_font, (FT_Long)gcchip_font_size, 0, &face) != 0) {
        FT_Done_FreeType(lib);
        return -1;
    }

    for (i = 0; i < FX_NSIZES; i++)
        if (build_one(face, i) != 0) bad = 1;

    /* FreeType has nothing left to do here: the atlases are built, so the face
     * and its memory can go. That is also what guarantees no rasterisation can
     * happen again by accident. */
    FT_Done_Face(face);
    FT_Done_FreeType(lib);

    return bad ? -1 : 0;
}

void fx_free(void)
{
    int i;
    for (i = 0; i < FX_NSIZES; i++) {
        if (s_atlas[i].tex) { GRRLIB_FreeTexture(s_atlas[i].tex); s_atlas[i].tex = NULL; }
        s_atlas[i].ok = 0;
    }
}

int fx_line_height(fx_size sz)
{
    if (sz < 0 || sz >= FX_NSIZES) return 0;
    return s_atlas[sz].line;
}

int fx_width(const char *s, fx_size sz)
{
    const fx_atlas *a = &s_atlas[sz];
    int w = 0;

    if (!s || sz < 0 || sz >= FX_NSIZES || !a->ok) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < FX_FIRST) continue;
        w += a->g[c - FX_FIRST].advance;
    }
    return w;
}

void fx_draw(int x, int y, const char *s, fx_size sz, u32 colour)
{
    const fx_atlas *a = &s_atlas[sz];

    if (!s || sz < 0 || sz >= FX_NSIZES || !a->ok) return;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < FX_FIRST) continue;
        if (c != ' ')       /* a space has nothing to draw */
            GRRLIB_DrawTile((f32)x, (f32)y, a->tex, 0.0f, 1.0f, 1.0f,
                            colour, c - FX_FIRST);
        x += a->g[c - FX_FIRST].advance;
    }
}

/* How many leading characters fit in `w`. A single exact pass: the advances
 * are integers known in advance. */
static int fit_head(const fx_atlas *a, const char *s, int w)
{
    int acc = 0, n = 0;

    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        int adv = (c < FX_FIRST) ? 0 : a->g[c - FX_FIRST].advance;
        if (acc + adv > w) break;
        acc += adv;
    }
    return n;
}

void fx_draw_fit(int x, int y, const char *s, fx_size sz, u32 colour, int w)
{
    const fx_atlas *a = &s_atlas[sz];
    char buf[256];
    int  keep;

    if (!s || !*s || sz < 0 || sz >= FX_NSIZES || !a->ok) return;
    if (fx_width(s, sz) <= w) { fx_draw(x, y, s, sz, colour); return; }

    keep = fit_head(a, s, w);
    if (keep > (int)sizeof buf - 1) keep = (int)sizeof buf - 1;
    if (keep < 1) return;

    memcpy(buf, s, (size_t)keep);
    buf[keep] = 0;
    buf[keep - 1] = '.';        /* one dot is enough at these sizes */
    fx_draw(x, y, buf, sz, colour);
}

void fx_draw_fit_tail(int x, int y, const char *s, fx_size sz, u32 colour, int w)
{
    const fx_atlas *a = &s_atlas[sz];
    int n, cut, acc = 0;

    if (!s || !*s || sz < 0 || sz >= FX_NSIZES || !a->ok) return;
    if (fx_width(s, sz) <= w) { fx_draw(x, y, s, sz, colour); return; }

    /* Walk back from the end for as long as it fits. */
    n = (int)strlen(s);
    for (cut = n; cut > 0; cut--) {
        unsigned char c = (unsigned char)s[cut - 1];
        int adv = (c < FX_FIRST) ? 0 : a->g[c - FX_FIRST].advance;
        if (acc + adv > w) break;
        acc += adv;
    }
    fx_draw(x, y, s + cut, sz, colour);
}
