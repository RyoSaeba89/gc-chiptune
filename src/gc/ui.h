/* ------------------------------------------------------------------------
 * GC-Chiptune: the interface, in GX.
 *
 * ONE SCREEN, TWO PANES SIDE BY SIDE
 * ----------------------------------
 *   left    the list: where you are in the tree, what you can play
 *   right   the player: what is playing, how far in
 *
 * Both are drawn every frame. That is the structural change from the
 * console-output version, which switched between TWO screens: you never saw the
 * list and playback at the same time, and nothing on screen said the other one
 * existed.
 *
 * Direct consequence: PLAYBACK NO LONGER STOPS WHEN YOU BROWSE. The old browser
 * was a blocking loop that closed the audio output on entry; here there is one
 * loop, which services the audio, reads the pad and draws.
 *
 * NO FOCUS, NO MODE: ONE BUTTON = ONE ROLE
 * ----------------------------------------
 * The first version moved a focus with START, and every button meant two things
 * depending on the active pane. In use it was unusable: you never know which
 * state you are in, and START / up / down all blur together.
 *
 * Every button therefore has exactly ONE meaning, always the same:
 *
 *   up/down       list cursor              X      pause
 *   left/right    leave / enter folder     Y      repeat mode
 *   L / R         jump ten rows            Z      shuffle
 *   A             enter / play             START  play the WHOLE tree
 *   B             up one level             L+R    rebuild the index
 *   C-stick left/right: volume
 *   C-stick up/down   : previous / next track
 *
 * Holding a direction repeats. That is not a nicety on a library of several
 * thousand entries -- see nav_repeat() in main.c.
 *
 * WHY THERE IS NO RENDER THREAD
 * -----------------------------
 * There is one loop, and it is bounded on purpose: it renders at most
 * AUDIO_BLOCKS_PER_PASS audio blocks per pass and reads the pad between two of
 * them (main.c, service_audio). A thread would buy the same thing at the price
 * of sharing the decoder state between two contexts, on a project where every
 * synchronisation fault so far has only been visible on real hardware.
 *
 * THE TREE IS NOT STORED. playlist.c sorts by full path, so the direct children
 * of a folder are found by sweeping its slice: the first segment after the
 * prefix gives either a file name or a sub-folder name. We build neither a tree
 * nor a table -- just a list of items recomputed when the folder changes, with
 * indices into the playlist rather than copies of strings.
 * ------------------------------------------------------------------------ */

#ifndef GC_UI_H_
#define GC_UI_H_

#include "library.h"
#include "index.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Displayable direct children of a folder. A folder in the pack holds a few
 * hundred at most; beyond that we truncate and say so, rather than growing
 * without limit on a console that only has 24 MB. */
#define UI_MAX_ITEMS 1024
#define UI_ROWS        12      /* visible list rows */

/* Trigger jump. Ten and not UI_ROWS: it is a round number, you know what you
 * are doing when you press it, and it does not change if the list height
 * changes. */
#define UI_JUMP        10

typedef struct {
    int track;    /* playlist index: the file, or the FIRST of the folder */
    int is_dir;
} ui_item;

typedef struct {
    char    dir[LIB_MAX_PREFIX];   /* current prefix, '/'-terminated     */
    size_t  dirlen;
    ui_item item[UI_MAX_ITEMS];
    int     n;
    int     truncated;
    int     sel;                   /* cursor                             */
    int     top;                   /* first displayed row                */
    int     slice_end;             /* first playlist index OUTSIDE the current
                                    * folder. Used to tell whether the current
                                    * track is somewhere under a displayed
                                    * sub-folder, without comparing strings
                                    * every frame. */
    unsigned gen;                  /* bumped on every folder change */
} ui_browser;

/* What the player pane needs to know about the moment. Filled in by main.c: the
 * interface fetches nothing itself. */
typedef struct {
    const char   *path;         /* track path, NULL if none            */
    const char   *title;        /* format's internal title, "" if none */
    const char   *backend;      /* "libxmp" or "midi"                  */
    const char   *note;         /* message in place of the help, or NULL */
    unsigned      rate;
    unsigned long elapsed_ms;
    unsigned long duration_ms;  /* 0 = unknown                         */
    int           paused;
    int           playing;      /* 0 = nothing in progress             */
    /* Peak of the last rendered block, 0..255 per channel. Feeds the level
     * meter under the progress bar: it is the only thing on screen that moves
     * continuously, and it says at a glance that sound is really coming out --
     * which a seconds counter does not. */
    unsigned char vu_l, vu_r;

    /* MEMORY, IN KILOBYTES. Two numbers, and you need both.
     *
     * `ram_free` is the free total -- what mallinfo keeps plus what the arena
     * has not handed out yet. `ram_block` is the LARGEST CONTIGUOUS BLOCK the
     * allocator will still return.
     *
     * Both, because they answer two different questions and only the second
     * decides: a holed heap announces 10 000 kB free and still refuses the next
     * module, which wants 2700 in one piece. The message "cannot read (10 000
     * kB free)" was therefore true and useless -- exactly the shape of trap
     * that has already cost this project three weeks (docs/STATUS.md 10.4).
     *
     * Showing them permanently is what lets you WATCH memory drain over a
     * session, and tell a leak (both fall together and never come back) from
     * fragmentation (the total holds, the block collapses). */
    unsigned long ram_free;
    unsigned long ram_block;
} ui_now;

/* Takes the screen: GX, then the embedded font. CALL THIS FIRST, before
 * mounting the card -- GRRLIB_Init() takes over the display and does not give
 * it back, so any later printf would land in a buffer that is no longer on
 * screen. That is why start-up narrates itself through ui_boot().
 *
 * Returns 0, or negative if GX or the font would not start; in that case a
 * libogc console is put back so printf is still worth something. */
int  ui_init(void);
void ui_shutdown(void);

/* Start-up trace. Replaces printf: it is the only thing separating a failed
 * start-up from a silent black screen. */
void ui_boot(const char *fmt, ...);

/* Last line, then we stop: for the paths that showed a message before giving
 * up. Does not return. */
void ui_boot_fatal(const char *fmt, ...);

/* Waiting screen while the card is walked, which can take a while. */
void ui_draw_scan(const char *root, int found, const char *note);

/* Places the browser on a folder and recomputes its children. `dir` gets a
 * trailing '/' if it has none. */
void ui_browser_open(ui_browser *br, const playlist *pl, const char *dir);

/* Goes up one level. Returns 0 if already at the scan root. The cursor lands
 * back on the folder we came out of, not at the top: that is what you expect
 * when stepping out of an artist. */
int  ui_browser_up(ui_browser *br, const playlist *pl, const char *root);

void ui_browser_move(ui_browser *br, int delta);

/* Full prefix of the selected item if it is a folder; NULL if it is a file. The
 * returned buffer belongs to the browser. */
const char *ui_browser_sel_dir(ui_browser *br, const playlist *pl);

/* ONE frame: both panes, the header and the footer. Does not present -- that is
 * the caller's job via ui_present(), so it keeps control of when the loop waits
 * for the retrace.
 *
 * Since text goes through a glyph atlas (font_gx.h), a complete frame costs
 * nothing but quads: it can be drawn every frame without taking anything from
 * the decoder. */
void ui_draw(const ui_browser *br, const playlist *pl, const track_index *ix,
             const library *lb, const ui_now *now,
             const char *root, const char *src);

/* Sends the frame and waits for the retrace. This is what paces the loop. */
void ui_present(void);

/* The embedded font, src/gc/font_data.c. */
extern const unsigned char gcchip_font[];
extern const unsigned int  gcchip_font_size;

#ifdef __cplusplus
}
#endif

#endif /* GC_UI_H_ */
