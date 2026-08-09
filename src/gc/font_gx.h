/* ------------------------------------------------------------------------
 * GC-Chiptune: constant-cost GX text.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * GRRLIB_PrintfTTF has NO cache at all: GRRLIB_ttfFont holds nothing but a
 * FreeType face, and every character displayed is rasterised from its outlines,
 * on every call. GRRLIB_WidthTTF does the same to measure. One frame of the
 * player is on the order of 700 glyphs -- rasterised on the same Gekko as the
 * synth, on every redraw.
 *
 * Measured with the pad, not with a stopwatch: decoding slowed down audibly
 * while scrolling the list, including on an XM at 12 per mille of load. No
 * frame-rate cap fixes that -- capping is choosing how often you steal time
 * from the sound. Text had to stop costing anything.
 *
 * WHAT THIS FILE DOES
 * -------------------
 * Every glyph is rasterised ONCE at start-up, into a tile atlas (one texture
 * per size). Displaying a character then amounts to placing a textured quad:
 * the GPU does it, the Gekko only stacks vertices. Measuring a string amounts
 * to adding integers -- not one rasterisation left.
 *
 * FreeType is used only during construction, then released.
 * ------------------------------------------------------------------------ */

#ifndef GC_FONT_GX_H_
#define GC_FONT_GX_H_

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Three sizes, and three only: each one costs a complete atlas. */
typedef enum { FX_SMALL = 0, FX_BODY, FX_TITLE, FX_NSIZES } fx_size;

/* Builds the atlases. Call after GRRLIB_Init(). 0 if all is well. */
int  fx_init(void);
void fx_free(void);

/* Suggested line spacing for this size. */
int  fx_line_height(fx_size sz);

/* Width in pixels. A sum of integer advances: no FreeType access, no
 * allocation. That is what lets fx_draw_fit be exact instead of approximating
 * by bisection. */
int  fx_width(const char *s, fx_size sz);

void fx_draw(int x, int y, const char *s, fx_size sz, u32 colour);

/* Cuts at `w` pixels keeping the START, a dot marking the cut. */
void fx_draw_fit(int x, int y, const char *s, fx_size sz, u32 colour, int w);

/* Cuts at `w` pixels keeping the END: in this corpus it is the end of the name
 * that tells tracks apart ("... 2.2 kg.xm" versus "... 2.3 crk.xm"). */
void fx_draw_fit_tail(int x, int y, const char *s, fx_size sz, u32 colour, int w);

#ifdef __cplusplus
}
#endif

#endif /* GC_FONT_GX_H_ */
