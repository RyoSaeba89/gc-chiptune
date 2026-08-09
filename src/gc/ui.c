/* ------------------------------------------------------------------------
 * GC-Chiptune: the two panes, in GX. See ui.h.
 *
 * All text goes through font_gx.h: a glyph atlas built once at start-up. No
 * call to GRRLIB_PrintfTTF or GRRLIB_WidthTTF may reappear here -- they
 * rasterise on every call, and that is what was stealing time from the decoder.
 *
 * LAYOUT COROLLARY: every string in the right pane goes through fx_draw_fit
 * with the pane's usable width. Overflow then becomes impossible by
 * construction, rather than "unlikely if you count correctly" -- the first
 * version let the load line run off the screen.
 * ------------------------------------------------------------------------ */

#include "ui.h"
#include "font_gx.h"
#include "audio_gc.h"    /* volume and load, shown by the player pane */

#include <gccore.h>
#include <grrlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ------------------------------------------------------------------ layout */

/* 640x480, and the safe area counts on a CRT: televisions of that era hide up
 * to eight per cent of the picture behind the bezel. Nothing readable goes
 * outside these margins. */
#define SCR_W      640
#define SCR_H      480
#define SAFE_X      32
#define SAFE_Y      28
#define SAFE_W     (SCR_W - 2 * SAFE_X)
#define SAFE_H     (SCR_H - 2 * SAFE_Y)

#define HEAD_H      38
#define FOOT_H      26
#define PANEL_GAP   14
#define PANEL_Y    (SAFE_Y + HEAD_H)
#define PANEL_H    (SAFE_H - HEAD_H - FOOT_H - 6)

#define LIST_W     300
#define PLAY_X     (SAFE_X + LIST_W + PANEL_GAP)
#define PLAY_W     (SAFE_W - LIST_W - PANEL_GAP)

#define ROW_H       24
#define PAD         10

/* Colours, RGBA8. */
#define C_BG        0x0E1220FF
#define C_PANEL     0x161C2EFF
#define C_PANEL_HI  0x1E2740FF
#define C_LINE      0x2A3350FF
#define C_TEXT      0xE6EAF5FF
#define C_DIM       0x8792AEFF
#define C_ACCENT    0x62D0A0FF
#define C_ACCENT_D  0x2E6E56FF
#define C_WARN      0xE0B341FF

static int gx_up = 0;

/* Scratch buffer for names carved out of the playlist pool. Everything is drawn
 * from a single thread and nothing outlives the call. */
static char g_buf[LIB_MAX_PREFIX];

/* --------------------------------------------------------------- start-up */

#define BOOT_LINES 14
static char boot_line[BOOT_LINES][96];
static int  boot_n = 0;

static void draw_boot(void)
{
    int i;

    GRRLIB_FillScreen(C_BG);
    GRRLIB_Rectangle(SAFE_X, SAFE_Y, SAFE_W, SAFE_H, C_PANEL, true);
    GRRLIB_Rectangle(SAFE_X, SAFE_Y, SAFE_W, SAFE_H, C_LINE, false);

    fx_draw(SAFE_X + PAD, SAFE_Y + 10, "GC-Chiptune", FX_TITLE, C_TEXT);
    for (i = 0; i < boot_n; i++)
        fx_draw_fit(SAFE_X + PAD, SAFE_Y + 52 + i * 22, boot_line[i],
                    FX_BODY, i == boot_n - 1 ? C_TEXT : C_DIM,
                    SAFE_W - 2 * PAD);
    GRRLIB_Render();
}

static void boot_add(const char *fmt, va_list ap)
{
    if (boot_n >= BOOT_LINES) {
        memmove(boot_line[0], boot_line[1], sizeof boot_line[0] * (BOOT_LINES - 1));
        boot_n = BOOT_LINES - 1;
    }
    vsnprintf(boot_line[boot_n], sizeof boot_line[0], fmt, ap);
    boot_n++;
}

void ui_boot(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    boot_add(fmt, ap);
    va_end(ap);

    if (gx_up) draw_boot();
    else       printf("%s\n", boot_line[boot_n - 1]);
}

void ui_boot_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    boot_add(fmt, ap);
    va_end(ap);

    if (gx_up) {
        draw_boot();
        fx_draw(SAFE_X + PAD, SAFE_Y + SAFE_H - 34, "Switch the console off.",
                FX_BODY, C_WARN);
        GRRLIB_Render();
    } else {
        printf("%s\nSwitch the console off.\n", boot_line[boot_n - 1]);
    }
    for (;;) VIDEO_WaitVSync();
}

/* Fallback libogc console: if GX would not start, the caller's printf output
 * has to land somewhere visible. */
static void fallback_console(void)
{
    static void *xfb = NULL;
    GXRModeObj  *rmode;

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
}

int ui_init(void)
{
    int ret = GRRLIB_Init();

    /* -2 (fat devoptab) and -4 (system font) come with a perfectly working
     * video and GX subsystem, and do not concern a program that mounts its own
     * volumes and carries its own font. Only -1 and -3 leave nothing to draw
     * with. */
    if (ret == -1 || ret == -3) { GRRLIB_Exit(); fallback_console(); return ret; }

    GRRLIB_SetAntiAliasing(true);
    GRRLIB_SetBlend(GRRLIB_BLEND_ALPHA);

    if (fx_init() != 0) { GRRLIB_Exit(); fallback_console(); return -3; }

    gx_up = 1;
    return 0;
}

void ui_shutdown(void)
{
    if (!gx_up) return;
    fx_free();
    GRRLIB_Exit();
    gx_up = 0;
}

void ui_present(void) { if (gx_up) GRRLIB_Render(); }

/* ------------------------------------------------------------------- tools */

static void panel(int x, int y, int w, int h)
{
    GRRLIB_Rectangle((f32)x, (f32)y, (f32)w, (f32)h, C_PANEL, true);
    GRRLIB_Rectangle((f32)x, (f32)y, (f32)w, (f32)h, C_LINE, false);
}

static void put_time(char *dst, size_t cap, unsigned long ms)
{
    unsigned long s = ms / 1000;
    snprintf(dst, cap, "%lu:%02lu", s / 60, s % 60);
}

/* Appends a trailing '/' to `dir` if it has none. Returns the resulting length.
 * The slash is not cosmetic: without it, "tPORt" would also match "tPORtable",
 * and the whole prefix logic depends on that. */
static size_t with_slash(char *dst, size_t cap, const char *dir)
{
    size_t n;

    snprintf(dst, cap, "%s", dir ? dir : "");
    n = strlen(dst);
    if (n && dst[n - 1] != '/' && n + 1 < cap) {
        dst[n++] = '/';
        dst[n]   = 0;
    }
    return n;
}

/* Displayable name of an item: the segment following the folder prefix, cut at
 * the '/' if it is a sub-folder. */
static const char *item_name(const ui_browser *br, const playlist *pl, int i)
{
    const char *p = pl_path(pl, br->item[i].track);
    const char *slash;
    size_t      n;

    /* pl_path() returns NULL for an out-of-range index. That should not happen,
     * but "should not" is how the browser used to dereference a pool that had
     * just been freed by a re-index. One test costs nothing here. */
    if (!p) return "";
    p += br->dirlen;

    slash = strchr(p, '/');
    if (!slash) return p;                       /* a file */

    n = (size_t)(slash - p);
    if (n >= sizeof g_buf) n = sizeof g_buf - 1;
    memcpy(g_buf, p, n);
    g_buf[n] = 0;
    return g_buf;
}

/* --------------------------------------------------------------- browser */

void ui_browser_open(ui_browser *br, const playlist *pl, const char *dir)
{
    int    i, n = pl_count(pl);
    size_t dl;

    br->gen++;
    dl = with_slash(br->dir, sizeof br->dir, dir);

    br->dirlen    = dl;
    br->n         = 0;
    br->truncated = 0;
    br->sel       = 0;
    br->top       = 0;
    br->slice_end = 0;

    /* A single sweep of the slice. Paths being sorted, all the tracks of a
     * given sub-folder follow each other: comparing with the previous segment
     * is enough to keep only one entry per sub-folder. */
    for (i = 0; i < n; i++) {
        const char *p = pl_path(pl, i);
        const char *rest, *slash;

        if (!p) break;
        if (strncmp(p, br->dir, dl) != 0) {
            if (br->n) break;      /* we have left the slice */
            continue;
        }
        br->slice_end = i + 1;     /* last index seen in the slice, +1 */
        rest  = p + dl;
        slash = strchr(rest, '/');

        if (slash) {
            /* Sub-folder: count it only once. */
            size_t seglen = (size_t)(slash - rest) + 1;
            if (br->n && br->item[br->n - 1].is_dir) {
                const char *prev = pl_path(pl, br->item[br->n - 1].track);
                if (prev && strncmp(prev + dl, rest, seglen) == 0) continue;
            }
        }

        if (br->n >= UI_MAX_ITEMS) { br->truncated = 1; break; }
        br->item[br->n].track  = i;
        br->item[br->n].is_dir = slash != NULL;
        br->n++;
    }
}

int ui_browser_up(ui_browser *br, const playlist *pl, const char *root)
{
    char   parent[LIB_MAX_PREFIX];
    char   from[LIB_MAX_PREFIX];
    char   rootdir[LIB_MAX_PREFIX];
    size_t rl;
    char  *slash;
    int    i;

    /* THE ROOT IS COMPARED WITH ITS TRAILING SLASH.
     *
     * br->dir always carries one; `root` as passed in usually does not
     * ("sd:/chiptunes"). Comparing the two raw lengths made 14 > 13 true at the
     * scan root, so B climbed out of it and listed sd:/ -- a directory the
     * playlist knows nothing about. */
    rl = with_slash(rootdir, sizeof rootdir, root);
    if (br->dirlen <= rl) return 0;        /* already at the scan root */

    snprintf(from, sizeof from, "%s", br->dir);
    snprintf(parent, sizeof parent, "%s", br->dir);
    parent[strlen(parent) - 1] = 0;        /* drop the trailing '/' */
    slash = strrchr(parent, '/');
    if (!slash) return 0;
    slash[1] = 0;                          /* keep the parent's '/' */

    ui_browser_open(br, pl, parent);

    /* Put the cursor back on the folder we came out of: stepping out of an
     * artist only to land at the top of the list would mean scrolling all the
     * way down again. */
    for (i = 0; i < br->n; i++) {
        const char *p = pl_path(pl, br->item[i].track);
        if (p && br->item[i].is_dir && strncmp(p, from, strlen(from)) == 0) {
            br->sel = i;
            br->top = (i >= UI_ROWS) ? i - UI_ROWS / 2 : 0;
            break;
        }
    }
    return 1;
}

void ui_browser_move(ui_browser *br, int delta)
{
    if (br->n == 0) return;

    br->sel += delta;

    /* WRAPPING on the single step: going up from the first row lands on the
     * last. In a folder of three tracks as much as at the root, that avoids
     * bumping into an invisible edge.
     *
     * UI_JUMP jumps STOP instead. A jump of ten that wraps drops you anywhere:
     * you do not jump in order to get lost. */
    if (delta == 1 || delta == -1) {
        if (br->sel < 0)      br->sel = br->n - 1;
        if (br->sel >= br->n) br->sel = 0;
    } else {
        if (br->sel < 0)      br->sel = 0;
        if (br->sel >= br->n) br->sel = br->n - 1;
    }

    if (br->sel < br->top)             br->top = br->sel;
    if (br->sel >= br->top + UI_ROWS)  br->top = br->sel - UI_ROWS + 1;
    if (br->top > br->n - UI_ROWS)     br->top = br->n - UI_ROWS;
    if (br->top < 0)                   br->top = 0;
}

const char *ui_browser_sel_dir(ui_browser *br, const playlist *pl)
{
    static char full[LIB_MAX_PREFIX];
    const char *p;
    const char *slash;
    size_t      n;

    if (br->n == 0 || !br->item[br->sel].is_dir) return NULL;

    p = pl_path(pl, br->item[br->sel].track);
    if (!p) return NULL;

    slash = strchr(p + br->dirlen, '/');
    if (!slash) return NULL;

    n = (size_t)(slash - p) + 1;           /* '/' included */
    if (n >= sizeof full) return NULL;
    memcpy(full, p, n);
    full[n] = 0;
    return full;
}

/* -------------------------------------------------------------------- panes */

static void draw_list(const ui_browser *br, const playlist *pl,
                      const track_index *ix, const char *root, int cur)
{
    const int x = SAFE_X, y = PANEL_Y;
    const int inner = LIST_W - 2 * PAD - 8;   /* -8: the scrollbar gutter */
    int i, list_y;
    char head[32];

    panel(x, y, LIST_W, PANEL_H);

    /* The current folder, tail kept: it is the end of the path that locates
     * you. */
    fx_draw_fit_tail(x + PAD, y + 8, br->dir[0] ? br->dir : root,
                     FX_SMALL, C_DIM, inner - 60);

    /* Position in the folder, on the right: without it the scrollbar says
     * "downwards" but not "by how much". */
    snprintf(head, sizeof head, "%d/%d", br->n ? br->sel + 1 : 0, br->n);
    fx_draw(x + LIST_W - PAD - fx_width(head, FX_SMALL), y + 8, head,
            FX_SMALL, C_DIM);

    GRRLIB_Line((f32)(x + PAD), (f32)(y + 30),
                (f32)(x + LIST_W - PAD), (f32)(y + 30), C_LINE);

    list_y = y + 38;

    if (br->n == 0)
        fx_draw(x + PAD, list_y + 2, "(empty)", FX_BODY, C_DIM);

    for (i = 0; i < UI_ROWS; i++) {
        int k  = br->top + i;
        int ry = list_y + i * ROW_H;
        char dur[12];
        int  dw = 0, playing = 0;
        u32  col;

        if (k >= br->n) break;

        /* IS THIS WHAT IS PLAYING?
         *
         * For a file, comparing indices is enough. For a folder, the current
         * track is inside it if its index falls between the first track of that
         * folder and the first of the next -- paths being sorted, a folder
         * occupies a contiguous slice. No strings to compare. */
        if (cur >= 0) {
            if (!br->item[k].is_dir) {
                playing = (br->item[k].track == cur);
            } else {
                int next = (k + 1 < br->n) ? br->item[k + 1].track : br->slice_end;
                playing = (cur >= br->item[k].track && cur < next);
            }
        }

        if (k == br->sel)
            GRRLIB_Rectangle((f32)(x + 4), (f32)(ry - 2), (f32)(LIST_W - 8),
                             (f32)(ROW_H - 2), C_PANEL_HI, true);

        /* WHAT IS PLAYING AND WHAT YOU ARE LOOKING AT ARE TWO DIFFERENT
         * QUESTIONS, and both have to be answered at once: the cursor is a
         * light background, playback is a colour and a stripe. You can browse
         * the card without losing sight of what is coming out of the
         * speakers. */
        if (playing)
            GRRLIB_Rectangle((f32)(x + 4), (f32)(ry - 2), 3.0f,
                             (f32)(ROW_H - 2), C_ACCENT, true);

        dur[0] = 0;
        if (!br->item[k].is_dir) {
            unsigned long ms = idx_duration(ix, br->item[k].track);
            if (ms) {
                put_time(dur, sizeof dur, ms);
                dw = fx_width(dur, FX_SMALL) + 10;
                fx_draw(x + PAD + inner - dw + 10, ry + 3, dur,
                        FX_SMALL, playing ? C_ACCENT_D : C_DIM);
            }
        }

        if (playing)            col = C_ACCENT;
        else if (k == br->sel)  col = C_TEXT;
        else if (br->item[k].is_dir) col = C_TEXT;
        else                    col = C_DIM;

        if (br->item[k].is_dir)
            fx_draw(x + PAD + 4, ry, "/", FX_BODY, playing ? C_ACCENT : C_ACCENT_D);

        fx_draw_fit(x + PAD + 14, ry, item_name(br, pl, k), FX_BODY,
                    col, inner - 14 - dw);
    }

    /* THE SCROLLBAR. A twelve-line window on a folder of two hundred says
     * nothing about where you are; the bar says it at a glance. Drawn only when
     * there is something to scroll. */
    if (br->n > UI_ROWS) {
        int track_h = UI_ROWS * ROW_H;
        int knob_h  = track_h * UI_ROWS / br->n;
        int knob_y;

        if (knob_h < 14) knob_h = 14;
        knob_y = list_y + (track_h - knob_h) * br->top / (br->n - UI_ROWS);

        GRRLIB_Rectangle((f32)(x + LIST_W - 8), (f32)list_y, 3.0f,
                         (f32)track_h, C_LINE, true);
        GRRLIB_Rectangle((f32)(x + LIST_W - 8), (f32)knob_y, 3.0f,
                         (f32)knob_h, C_ACCENT_D, true);
    }

    if (br->truncated)
        fx_draw(x + PAD, y + PANEL_H - 22, "folder truncated", FX_SMALL, C_WARN);
}

/* Level meter: two bars, left and right, fed by the peak of the last rendered
 * block.
 *
 * This is not decoration. It is the only thing on screen that moves
 * continuously, hence the only thing that says at a glance that sound is COMING
 * OUT -- a seconds counter advances just as happily on a silent render, and
 * this project has already spent three weeks measuring silence (STATUS 10.4). */
static void draw_vu(int x, int y, int w, const ui_now *now)
{
    const int h = 6;
    int l = (int)now->vu_l * w / 255;
    int r = (int)now->vu_r * w / 255;

    if (l > w) l = w;
    if (r > w) r = w;

    GRRLIB_Rectangle((f32)x, (f32)y, (f32)w, (f32)h, C_PANEL_HI, true);
    if (l > 0)
        GRRLIB_Rectangle((f32)x, (f32)y, (f32)l, (f32)h,
                         l >= w - 1 ? C_WARN : C_ACCENT, true);

    GRRLIB_Rectangle((f32)x, (f32)(y + h + 3), (f32)w, (f32)h, C_PANEL_HI, true);
    if (r > 0)
        GRRLIB_Rectangle((f32)x, (f32)(y + h + 3), (f32)r, (f32)h,
                         r >= w - 1 ? C_WARN : C_ACCENT, true);
}

static void draw_player(const library *lb, const ui_now *now)
{
    const int x = PLAY_X, y = PANEL_Y;
    const int inner = PLAY_W - 2 * PAD;
    char el[12], du[12], line[96];
    int  cy, barw, fill;

    panel(x, y, PLAY_W, PANEL_H);

    cy = y + 8;

    if (!now->playing && !now->note) {
        fx_draw_fit(x + PAD, cy, "nothing playing", FX_BODY, C_DIM, inner);
        cy += 26;
        fx_draw_fit(x + PAD, cy, "A on a track", FX_SMALL, C_DIM, inner);
        cy += 20;
        fx_draw_fit(x + PAD, cy, "START for the whole tree", FX_SMALL, C_DIM, inner);
    } else {
        /* The format's internal title when there is one, the file name
         * otherwise: many modules in the pack have no title. */
        const char *t = (now->title && now->title[0]) ? now->title : NULL;
        if (!t && now->path) {
            const char *slash = strrchr(now->path, '/');
            t = slash ? slash + 1 : now->path;
        }
        fx_draw_fit(x + PAD, cy, t ? t : "", FX_TITLE, C_TEXT, inner);
        cy += 32;

        fx_draw_fit_tail(x + PAD, cy, now->path ? now->path : "",
                         FX_SMALL, C_DIM, inner);
        cy += 24;

        if (now->note) {
            fx_draw_fit(x + PAD, cy, now->note, FX_BODY, C_WARN, inner);
        } else {
            snprintf(line, sizeof line, "%s  %u kHz",
                     now->backend ? now->backend : "-", now->rate / 1000);
            fx_draw_fit(x + PAD, cy, line, FX_SMALL, C_DIM, inner);
            cy += 24;

            put_time(el, sizeof el, now->elapsed_ms);
            if (now->duration_ms) put_time(du, sizeof du, now->duration_ms);
            else                  snprintf(du, sizeof du, "?:??");
            snprintf(line, sizeof line, "%s / %s%s", el, du,
                     now->paused ? "   PAUSED" : "");
            fx_draw_fit(x + PAD, cy, line, FX_BODY,
                        now->paused ? C_WARN : C_TEXT, inner);
            cy += 26;

            /* Progress bar. It only exists because the duration is known from
             * the moment the file opens (gcc_duration_ms). */
            barw = inner;
            fill = 0;
            if (now->duration_ms)
                fill = (int)((unsigned long long)now->elapsed_ms * barw
                             / now->duration_ms);
            if (fill > barw) fill = barw;
            GRRLIB_Rectangle((f32)(x + PAD), (f32)cy, (f32)barw, 10.0f,
                             C_PANEL_HI, true);
            if (fill > 0)
                GRRLIB_Rectangle((f32)(x + PAD), (f32)cy, (f32)fill, 10.0f,
                                 C_ACCENT, true);
            cy += 18;

            draw_vu(x + PAD, cy, inner, now);
        }
    }

    /* The bottom of the pane, at a FIXED position and always drawn -- including
     * when nothing is playing. Z and the C-stick respond in that state too, and
     * their effect has to be visible somewhere.
     *
     * Four short lines rather than one long one: the previous version fitted on
     * a single line and ran off the right of the screen. */
    {
        static const char * const rep_name[] =
            { "repeat list", "repeat track", "play once" };
        int my = y + PANEL_H - 136;

        GRRLIB_Line((f32)(x + PAD), (f32)my, (f32)(x + PLAY_W - PAD), (f32)my,
                    C_LINE);
        my += 10;

        fx_draw_fit(x + PAD, my,
                    lb->order_mode == LIB_ORDER_RANDOM ? "shuffle" : "sequential",
                    FX_SMALL,
                    lb->order_mode == LIB_ORDER_RANDOM ? C_ACCENT : C_DIM, inner);
        my += 20;
        fx_draw_fit(x + PAD, my, rep_name[lb->repeat], FX_SMALL, C_DIM, inner);
        my += 20;

        snprintf(line, sizeof line, "volume %u%%", gcaudio_volume());
        fx_draw_fit(x + PAD, my, line, FX_SMALL, C_DIM, inner);
        my += 20;

        /* LOAD DOES NOT TELL YOU WHETHER THE SOUND IS COMING OUT CLEANLY, AND
         * THAT IS THE TRAP THIS PROJECT HAS ALREADY PAID FOR THREE TIMES.
         *
         * Twice the same listening report: "it crackles" -- on an XM measuring
         * 12 per mille of load and zero anomalies (docs/STATUS.md 10.1 then
         * 11.6). Both times the fault was DOWNSTREAM of the render, and both
         * times the screen had nothing to say about it: it showed a healthy
         * load, which is what it was.
         *
         * gcaudio_starved() measures the condition you can hear -- the DSP
         * asked for a block and the ring was empty, i.e. a hole in the sound.
         * It existed since 12.18 and was displayed NOWHERE. A counter nobody
         * looks at is no better than a counter that lies. */
        {
            unsigned long dry = gcaudio_starved();

            /* `end` = gcaudio_underruns(): how many times the voice fell back
             * to ANSND_VOICE_STATE_FINISHED and had to be fully reconfigured.
             * EVERY OCCURRENCE IS A CLICK.
             *
             * This counter existed and was displayed nowhere either -- the
             * second in that situation after `dry`. It answers a question `dry`
             * does not ask: `dry` says "the ring was empty", `end` says "the
             * voice stopped". You can have the second without the first. */
            unsigned long end = gcaudio_underruns();

            snprintf(line, sizeof line, "load %u/%u  dry %lu  end %lu",
                     gcaudio_load_avg(), gcaudio_load_max(), dry, end);
            fx_draw_fit(x + PAD, my, line, FX_SMALL,
                        (gcaudio_load_max() > 1000 || dry || end) ? C_WARN
                                                                 : C_DIM,
                        inner);
            my += 20;
        }

        /* MEMORY, PERMANENTLY AND IN BOTH ITS FORMS.
         *
         * Reported while listening: "after a while no song would start any
         * more, memory full". The player only showed memory at START-UP -- the
         * one moment when it is fine. So there was no way to see whether it was
         * leaking, or how fast.
         *
         * `blk` is the largest contiguous piece, and that is what decides: a
         * file is read whole. A holed heap shows a comfortable total and still
         * refuses the next module.
         *
         *   both fall together and never come back -> LEAK
         *   the total holds, the block collapses    -> FRAGMENTATION
         *
         * Warns as soon as the largest block drops below 3 MB: the largest
         * module in the pack asks for 2.7. */
        {
            snprintf(line, sizeof line, "ram %lu   blk %lu kB",
                     now->ram_free, now->ram_block);
            fx_draw_fit(x + PAD, my, line, FX_SMALL,
                        (now->ram_block && now->ram_block < 3072) ? C_WARN
                                                                 : C_DIM,
                        inner);
            my += 20;
        }

        /* DSP LOAD, THE THIRD COUNTER WE HAD WITHOUT LOOKING AT IT.
         *
         * When the DSP has not finished mixing in time, libansnd does not
         * improvise: it sends its SILENCE buffer to the AI
         * (ansnd_audio_dma_callback). That is a clean hole in the sound, and
         * NONE of our other counters sees it -- not `dry`, not `end`, both of
         * which measure what happens BEFORE the DSP.
         *
         * Measured on the bench: 31 per mille. If the number here is much
         * higher, or if DSP STALLED appears, we have the culprit. */
        {
            unsigned dsp = gcaudio_dsp_permille();

            snprintf(line, sizeof line, "dsp %u%s", dsp,
                     gcaudio_dsp_stalled() ? "  STALLED" : "");
            fx_draw_fit(x + PAD, my, line, FX_SMALL,
                        (gcaudio_dsp_stalled() || dsp > 500) ? C_WARN : C_DIM,
                        inner);
        }
    }
}

/* -------------------------------------------------------------------- screens */

void ui_draw(const ui_browser *br, const playlist *pl, const track_index *ix,
             const library *lb, const ui_now *now,
             const char *root, const char *src)
{
    char line[96];
    int  w;

    GRRLIB_FillScreen(C_BG);

    /* Header: who we are, where the music comes from, where we are in it. */
    fx_draw(SAFE_X, SAFE_Y, "GC-Chiptune", FX_TITLE, C_TEXT);

    snprintf(line, sizeof line, "%d / %d", lib_position(lb), lib_count(lb));
    w = fx_width(line, FX_BODY);
    fx_draw(SAFE_X + SAFE_W - w, SAFE_Y + 2, line, FX_BODY, C_ACCENT);

    if (src) {
        w = fx_width(src, FX_SMALL);
        fx_draw(SAFE_X + SAFE_W - w, SAFE_Y + 22, src, FX_SMALL, C_DIM);
    }

    draw_list(br, pl, ix, root, lib_current(lb));
    draw_player(lb, now);

    /* Footer: every button having a single role, it all fits on two lines and
     * nothing depends on invisible state. */
    fx_draw_fit(SAFE_X, SAFE_Y + SAFE_H - FOOT_H,
                "up/down move   left/right out/in   L/R jump 10   "
                "A open or play   B up   START whole tree",
                FX_SMALL, C_DIM, SAFE_W);
    fx_draw_fit(SAFE_X, SAFE_Y + SAFE_H - FOOT_H + 16,
                "X pause   Y repeat   Z shuffle   "
                "C-stick: volume and track   L+R rebuild index",
                FX_SMALL, C_DIM, SAFE_W);
}

void ui_draw_scan(const char *root, int found, const char *note)
{
    char line[128];

    if (!gx_up) return;

    GRRLIB_FillScreen(C_BG);
    GRRLIB_Rectangle(SAFE_X, SAFE_Y, SAFE_W, SAFE_H, C_PANEL, true);
    GRRLIB_Rectangle(SAFE_X, SAFE_Y, SAFE_W, SAFE_H, C_LINE, false);

    fx_draw(SAFE_X + PAD, SAFE_Y + 10, "GC-Chiptune", FX_TITLE, C_TEXT);
    fx_draw_fit_tail(SAFE_X + PAD, SAFE_Y + 56, root, FX_BODY, C_DIM,
                     SAFE_W - 2 * PAD);

    snprintf(line, sizeof line, "%d tracks", found);
    fx_draw(SAFE_X + PAD, SAFE_Y + 86, line, FX_TITLE, C_ACCENT);

    if (note)
        fx_draw_fit(SAFE_X + PAD, SAFE_Y + 126, note, FX_BODY, C_TEXT,
                    SAFE_W - 2 * PAD);

    GRRLIB_Render();
}
