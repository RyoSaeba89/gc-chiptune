/* ------------------------------------------------------------------------
 * GC-Chiptune: index cache on the SD card. See index.h.
 * ------------------------------------------------------------------------ */

#include "index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDX_MAGIC   0x47434349u   /* "GCCI" */
/* Version 2: version 1 wrote the RAW POOL, i.e. the paths in walk order rather
 * than sorted -- see idx_save(). Version 1 caches are refused, which triggers a
 * rewalk and replaces them. */
#define IDX_VERSION 2u
#define IDX_HDR     64
#define IDX_ROOT    32            /* bytes reserved for the root in the header */

/* Explicit BIG-ENDIAN read/write, byte by byte.
 *
 * The Gekko is big-endian and the console is both the only producer and the
 * only consumer of this file: a raw fwrite() of u32 would work. We still write
 * it by hand, for the same reason that cost three weeks on the .sf2
 * (docs/STATUS.md 7.5) -- a binary format that depends on the byte order of the
 * machine that wrote it is a trap that only goes off later. As it stands the
 * file is readable by any host tool, with no conversion. */
static void put_u32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >>  8); p[3] = (unsigned char)(v);
}

static unsigned long get_u32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] <<  8) |  (unsigned long)p[3];
}

void idx_init(track_index *ix)
{
    memset(ix, 0, sizeof *ix);
}

void idx_free(track_index *ix)
{
    free(ix->dur);
    ix->dur   = NULL;
    ix->count = 0;
    ix->open  = 0;
}

/* ---------------------------------------------------------------- reading */

int idx_load(track_index *ix, playlist *pl, const char *file, const char *root)
{
    unsigned char hdr[IDX_HDR];
    FILE         *f;
    unsigned long count, pool_len;
    char         *pool = NULL;
    unsigned     *offs = NULL;
    unsigned char *durbuf = NULL;
    unsigned long  i;
    size_t         off;

    idx_free(ix);

    f = fopen(file, "rb");
    if (!f) return -1;

    if (fread(hdr, 1, IDX_HDR, f) != IDX_HDR)          goto bad;
    if (get_u32(hdr)     != IDX_MAGIC)                 goto bad;
    if (get_u32(hdr + 4) != IDX_VERSION)               goto bad;

    count    = get_u32(hdr + 8);
    pool_len = get_u32(hdr + 12);

    /* A cache written for ANOTHER root would play paths that no longer exist.
     * Refusing it beats scrolling errors. */
    if (strncmp((const char *)hdr + 16, root, IDX_ROOT - 1) != 0) goto bad;

    if (count == 0 || count > (unsigned long)PL_MAX_TRACKS) goto bad;
    if (pool_len == 0 || pool_len > 64u * 1024u * 1024u)    goto bad;

    pool   = (char *)malloc(pool_len);
    offs   = (unsigned *)malloc(sizeof(unsigned) * count);
    durbuf = (unsigned char *)malloc(4u * count);
    ix->dur = (unsigned long *)malloc(sizeof(unsigned long) * count);
    if (!pool || !offs || !durbuf || !ix->dur) goto bad;

    if (fread(pool, 1, pool_len, f) != pool_len) goto bad;
    /* The pool MUST end on a zero, or pl_path() would read past it. */
    if (pool[pool_len - 1] != 0) goto bad;

    if (fread(durbuf, 1, 4u * count, f) != 4u * count) goto bad;

    /* Rebuild the offsets: the strings follow each other, zero-terminated. */
    off = 0;
    for (i = 0; i < count; i++) {
        if (off >= pool_len) goto bad;
        offs[i] = (unsigned)off;

        /* THE CACHE MUST COME BACK SORTED, and we verify that rather than
         * believe it. lib_set_scope() searches by bisection: on a list that is
         * not sorted it returns a wrong interval, and whole folders become
         * unplayable with nothing to signal it.
         *
         * Refusing the cache costs a rewalk -- 4.5 s over 4937 tracks --
         * whereas believing it cost a silent fault. Easy choice. */
        if (i > 0 && strcmp(pool + offs[i - 1], pool + offs[i]) > 0) goto bad;

        off += strlen(pool + off) + 1;
        ix->dur[i] = get_u32(durbuf + 4 * i);
    }
    if (off != pool_len) goto bad;      /* the arithmetic has to come out even */

    free(durbuf);
    fclose(f);

    pl_free(pl);
    pl->pool     = pool;
    pl->pool_len = pl->pool_cap = pool_len;
    pl->offsets  = offs;
    pl->count    = pl->cap = (int)count;
    pl->dirs = pl->skipped = pl->truncated = 0;

    ix->count      = (int)count;
    ix->dur_offset = (long)(IDX_HDR + pool_len);
    ix->open       = 1;
    snprintf(ix->file, sizeof ix->file, "%s", file);

    return (int)count;

bad:
    free(pool); free(offs); free(durbuf);
    free(ix->dur); ix->dur = NULL;
    ix->count = 0; ix->open = 0;
    fclose(f);
    return -1;
}

/* ---------------------------------------------------------------- writing */

int idx_save(track_index *ix, const playlist *pl, const char *file,
             const char *root)
{
    unsigned char hdr[IDX_HDR];
    unsigned char four[4];
    FILE         *f;
    int           n = pl_count(pl), i;
    unsigned long *keep = ix->dur;
    int            keep_n = ix->count;

    unsigned long written = 0;

    if (n <= 0) return -1;

    /* PATHS ARE WRITTEN IN SORTED ORDER, NOT AS THE RAW POOL.
     *
     * The pool is filled in card-walk order; it is the `offsets` array that
     * carries the sort, and that was not written. So the cache came back in
     * readdir order.
     *
     * Two consequences, both invisible on the first power-on since that one
     * scans, and present on every one after:
     *
     *  - lib_set_scope() searches by BISECTION and requires a sorted list. On a
     *    list that is no longer sorted it returns a wrong interval -- often an
     *    empty one. The browser, which sweeps linearly, kept showing the files:
     *    so you saw the track, and playing it answered "nothing playing". Only
     *    in some folders, the ones where the bisection missed.
     *  - the durations, on the other hand, were written in sorted order (loop
     *    over the playlist index). Paths and durations were not even in the
     *    same order as each other.
     *
     * Writing the paths through pl_path() puts everything back in agreement:
     * index i means the same thing on both sides of the save. */
    for (i = 0; i < n; i++) written += (unsigned long)strlen(pl_path(pl, i)) + 1;

    f = fopen(file, "wb");
    if (!f) return -1;

    memset(hdr, 0, sizeof hdr);
    put_u32(hdr,      IDX_MAGIC);
    put_u32(hdr + 4,  IDX_VERSION);
    put_u32(hdr + 8,  (unsigned long)n);
    put_u32(hdr + 12, written);
    snprintf((char *)hdr + 16, IDX_ROOT, "%s", root);

    if (fwrite(hdr, 1, IDX_HDR, f) != IDX_HDR) { fclose(f); return -1; }

    for (i = 0; i < n; i++) {
        const char *p = pl_path(pl, i);
        size_t      l = strlen(p) + 1;
        if (fwrite(p, 1, l, f) != l) { fclose(f); return -1; }
    }

    /* Carry over already known durations if we are rewriting the cache over
     * itself (same track count = same card, very probably). Otherwise start
     * from zero: an unknown duration beats a duration attributed to the wrong
     * file. */
    for (i = 0; i < n; i++) {
        unsigned long ms = (keep && keep_n == n) ? keep[i] : 0;
        put_u32(four, ms);
        if (fwrite(four, 1, 4, f) != 4) { fclose(f); return -1; }
    }

    fclose(f);

    /* Reload the in-memory state from what we have just written. */
    {
        unsigned long *nd = (unsigned long *)calloc((size_t)n, sizeof(unsigned long));
        if (!nd) return -1;
        if (keep && keep_n == n)
            memcpy(nd, keep, (size_t)n * sizeof(unsigned long));
        free(ix->dur);
        ix->dur = nd;
    }

    ix->count      = n;
    ix->dur_offset = (long)(IDX_HDR + written);
    ix->open       = 1;
    snprintf(ix->file, sizeof ix->file, "%s", file);

    return 0;
}

/* ------------------------------------------------------------------ durations */

unsigned long idx_duration(const track_index *ix, int i)
{
    if (!ix || !ix->dur || i < 0 || i >= ix->count) return 0;
    return ix->dur[i];
}

void idx_set_duration(track_index *ix, int i, unsigned long ms)
{
    unsigned char four[4];
    FILE *f;

    if (!ix || !ix->open || !ix->dur) return;
    if (i < 0 || i >= ix->count) return;
    if (ix->dur[i] == ms) return;        /* nothing new: do not write */

    ix->dur[i] = ms;

    /* FOUR BYTES, at a known offset. No rewrite of the file, no recopying of
     * the 400 kB of paths. That is what makes filling it in as we go free --
     * one write per track, i.e. one every two minutes.
     *
     * "r+b" and not "wb": "wb" would truncate the file to zero. */
    f = fopen(ix->file, "r+b");
    if (!f) return;
    if (fseek(f, ix->dur_offset + 4L * i, SEEK_SET) == 0) {
        put_u32(four, ms);
        fwrite(four, 1, 4, f);
    }
    fclose(f);
}

int idx_known(const track_index *ix)
{
    int i, n = 0;
    if (!ix || !ix->dur) return 0;
    for (i = 0; i < ix->count; i++) if (ix->dur[i]) n++;
    return n;
}
