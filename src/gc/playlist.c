/* ------------------------------------------------------------------------
 * GC-Chiptune: recursive walk and playlist. See playlist.h.
 * ------------------------------------------------------------------------ */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "playlist.h"

/* Accepted extensions.
 *
 * The filter is deliberately coarse: gcc_detect() then decides on CONTENT
 * (backend.cpp). Its only job is to avoid opening the thousands of foreign
 * files lying next to the music -- the reference pack contains 120 .sc68,
 * 63 .ahx, 48 .sid, some .mp3, some .jpg... and every open costs an SD read.
 *
 * So we list what our two backends can play: MIDI, and the module formats
 * actually encountered or common in libxmp. */
static const char * const g_midi_exts[] = {
    "mid", "midi", "rmi",
    NULL
};

static const char * const g_module_exts[] = {
    "mod", "xm", "it", "s3m", "mtm", "stm", "669", "far", "okt", "ptm",
    "mdl", "dbm", "amf", "gdm", "imf", "liq", "ult", "umx", "med", "dtm",
    "psm", "rtm", "stx", "wow", "digi", "dmf", "abk", "emod", "fnk", "mgt",
    NULL
};

static int ext_in(const char *name, const char * const *list)
{
    const char *dot;
    int i;

    if (!name) return 0;
    dot = strrchr(name, '.');
    if (!dot || !dot[1]) return 0;
    dot++;

    for (i = 0; list[i]; i++) {
        const char *a = dot, *b = list[i];
        while (*a && *b && tolower((unsigned char)*a) == *b) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}

int pl_is_playable(const char *name)
{
    return ext_in(name, g_midi_exts) || ext_in(name, g_module_exts);
}

int pl_is_midi(const char *name)
{
    return ext_in(name, g_midi_exts);
}

void pl_init(playlist *pl)
{
    memset(pl, 0, sizeof *pl);
}

void pl_free(playlist *pl)
{
    free(pl->pool);
    free(pl->offsets);
    memset(pl, 0, sizeof *pl);
}

static int pl_add(playlist *pl, const char *path)
{
    size_t need = strlen(path) + 1;

    if (pl->count >= PL_MAX_TRACKS) { pl->truncated = 1; return 0; }

    if (pl->pool_len + need > pl->pool_cap) {
        size_t cap = pl->pool_cap ? pl->pool_cap * 2 : 64 * 1024;
        char  *p;
        while (cap < pl->pool_len + need) cap *= 2;
        p = (char *)realloc(pl->pool, cap);
        if (!p) { pl->truncated = 1; return 0; }
        pl->pool = p;
        pl->pool_cap = cap;
    }

    if (pl->count >= pl->cap) {
        int       cap = pl->cap ? pl->cap * 2 : 256;
        unsigned *o;
        if (cap > PL_MAX_TRACKS) cap = PL_MAX_TRACKS;
        o = (unsigned *)realloc(pl->offsets, (size_t)cap * sizeof *o);
        if (!o) { pl->truncated = 1; return 0; }
        pl->offsets = o;
        pl->cap = cap;
    }

    pl->offsets[pl->count++] = (unsigned)pl->pool_len;
    memcpy(pl->pool + pl->pool_len, path, need);
    pl->pool_len += need;
    return 1;
}

/* Type and size of an entry.
 *
 * On GameCube the devoptab's readdir already fills d_type and d_stat, so the
 * size is FREE, whereas a separate stat() would cost one more SD read per file
 * -- 5533 files in the reference pack.
 *
 * None of that is portable: MinGW's dirent has neither d_type nor d_stat. The
 * host therefore goes back through stat(), which is what makes it possible to
 * replay exactly this walk over the full corpus (tools/pl_scan_test.c). */
#ifdef GEKKO
static void entry_info(const struct dirent *e, const char *path,
                       int *is_dir, unsigned long *size)
{
    (void)path;
    *is_dir = (e->d_type == DT_DIR);
    *size   = (unsigned long)e->d_stat.st_size;
}
#else
#include <sys/stat.h>
static void entry_info(const struct dirent *e, const char *path,
                       int *is_dir, unsigned long *size)
{
    struct stat st;
    (void)e;
    if (stat(path, &st) != 0) { *is_dir = 0; *size = 0; return; }
    *is_dir = S_ISDIR(st.st_mode);
    *size   = (unsigned long)st.st_size;
}
#endif

/* Opens `path` (length `len`, writable buffer).
 *
 * Special case of a volume root: the prefix then reduces to "sd:", which the
 * devoptab cannot open -- it needs "sd:/". We add the slash for the duration of
 * the call without keeping it: concatenating entry names has to start from a
 * prefix with no trailing slash. */
static DIR *open_dir(char *path, size_t len)
{
    DIR *d;

    if (len > 0 && path[len - 1] == ':') {
        path[len]     = '/';
        path[len + 1] = '\0';
        d = opendir(path);
        path[len] = '\0';
        return d;
    }
    return opendir(path);
}

/* One path buffer for the whole walk: PATH_MAX bytes per recursion level would
 * overflow the stack. `len` is the current length. */
static void scan_dir(playlist *pl, char *path, size_t len, size_t cap, int depth)
{
    DIR *d;
    struct dirent *e;

    if (depth > PL_MAX_DEPTH) { pl->truncated = 1; return; }

    d = open_dir(path, len);
    if (!d) return;
    pl->dirs++;

    while ((e = readdir(d)) != NULL) {
        size_t        nlen;
        int           is_dir;
        unsigned long size;

        if (e->d_name[0] == '.') continue;   /* . .. and hidden files */

        nlen = strlen(e->d_name);
        if (len + 1 + nlen + 1 > cap) { pl->truncated = 1; continue; }

        path[len] = '/';
        memcpy(path + len + 1, e->d_name, nlen + 1);

        entry_info(e, path, &is_dir, &size);

        if (is_dir) {
            scan_dir(pl, path, len + 1 + nlen, cap, depth + 1);
        } else if (!pl_is_playable(e->d_name)) {
            pl->skipped++;
        } else if (size == 0 || size > PL_MAX_TRACK_BYTES) {
            pl->skipped++;
        } else if (!pl_add(pl, path)) {
            break;   /* bound reached, no point continuing */
        }

        path[len] = '\0';

        if (pl->truncated && pl->count >= PL_MAX_TRACKS) break;
    }

    closedir(d);
}

/* qsort comparator: the offsets no longer move once the walk is done, so we can
 * dereference the pool safely. */
static const playlist *g_sort_pl;

static int cmp_offset(const void *a, const void *b)
{
    const char *pa = g_sort_pl->pool + *(const unsigned *)a;
    const char *pb = g_sort_pl->pool + *(const unsigned *)b;
    return strcmp(pa, pb);
}

int pl_scan(playlist *pl, const char *root)
{
    char   path[512];
    size_t len;
    DIR   *probe;

    if (!pl || !root) return -1;

    len = strlen(root);
    if (len == 0 || len + 2 >= sizeof path) return -1;

    /* We keep the prefix WITHOUT a trailing slash: scan_dir concatenates
     * "/name", and a kept "sd:/" would put double slashes in every path. */
    while (len > 1 && root[len - 1] == '/') len--;
    memcpy(path, root, len);
    path[len] = '\0';

    probe = open_dir(path, len);
    if (!probe) return -1;
    closedir(probe);

    scan_dir(pl, path, len, sizeof path, 0);

    if (pl->count > 1) {
        g_sort_pl = pl;
        qsort(pl->offsets, (size_t)pl->count, sizeof *pl->offsets, cmp_offset);
        g_sort_pl = NULL;
    }

    return pl->count;
}

int pl_count(const playlist *pl) { return pl ? pl->count : 0; }

const char *pl_path(const playlist *pl, int i)
{
    if (!pl || !pl->pool || !pl->offsets || i < 0 || i >= pl->count) return NULL;
    return pl->pool + pl->offsets[i];
}

const char *pl_basename(const playlist *pl, int i)
{
    const char *p = pl_path(pl, i), *slash;
    if (!p) return NULL;
    slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}
