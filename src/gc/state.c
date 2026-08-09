/* ------------------------------------------------------------------------
 * GC-Chiptune: resume after power-off. See state.h.
 * ------------------------------------------------------------------------ */

#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void st_defaults(player_state *st)
{
    memset(st, 0, sizeof *st);
    st->recursive = 1;          /* the whole pack at once */
    st->order     = LIB_ORDER_SEQ;
    st->repeat    = LIB_REPEAT_LIST;
    st->seed      = 1u;
    st->volume    = 100u;
    st->valid     = 0;
}

/* Strips the trailing newline. Files like this end up being edited on Windows,
 * hence the \r too. */
static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

int st_load(player_state *st, const char *file)
{
    char  line[ST_MAX_PATH + 64];
    FILE *f;

    st_defaults(st);

    f = fopen(file, "r");
    if (!f) return 0;

    while (fgets(line, sizeof line, f)) {
        char *eq;
        chomp(line);
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;

        /* An unknown field is ignored, a missing field keeps its default:
         * adding a setting later will not break older files. */
        if      (!strcmp(line, "track"))  snprintf(st->track, sizeof st->track, "%s", eq + 1);
        else if (!strcmp(line, "scope"))  snprintf(st->scope, sizeof st->scope, "%s", eq + 1);
        else if (!strcmp(line, "recursive")) st->recursive = atoi(eq + 1) ? 1 : 0;
        else if (!strcmp(line, "order"))  st->order  = atoi(eq + 1) ? LIB_ORDER_RANDOM : LIB_ORDER_SEQ;
        else if (!strcmp(line, "repeat")) {
            int r = atoi(eq + 1);
            st->repeat = (r == 1) ? LIB_REPEAT_TRACK
                       : (r == 2) ? LIB_REPEAT_OFF
                                  : LIB_REPEAT_LIST;
        }
        else if (!strcmp(line, "seed"))   st->seed   = (unsigned)strtoul(eq + 1, NULL, 10);
        else if (!strcmp(line, "volume")) {
            long v = strtol(eq + 1, NULL, 10);
            st->volume = (v < 0) ? 0u : (v > 100) ? 100u : (unsigned)v;
        }
    }
    fclose(f);

    if (!st->seed) st->seed = 1u;
    st->valid = st->track[0] != 0;
    return st->valid;
}

int st_save(const player_state *st, const char *file)
{
    char  tmp[ST_MAX_PATH + 8];
    FILE *f;
    int   rep;

    snprintf(tmp, sizeof tmp, "%s.tmp", file);

    f = fopen(tmp, "w");
    if (!f) return -1;

    fprintf(f, "# GC-Chiptune -- resume state. Safe to delete.\n");
    fprintf(f, "track=%s\n",     st->track);
    fprintf(f, "scope=%s\n",     st->scope);
    fprintf(f, "recursive=%d\n", st->recursive ? 1 : 0);
    fprintf(f, "order=%d\n",     st->order == LIB_ORDER_RANDOM ? 1 : 0);
    rep = (st->repeat == LIB_REPEAT_TRACK) ? 1
        : (st->repeat == LIB_REPEAT_OFF)   ? 2 : 0;
    fprintf(f, "repeat=%d\n",    rep);
    fprintf(f, "seed=%u\n",      st->seed);
    fprintf(f, "volume=%u\n",    st->volume);

    if (fclose(f) != 0) { remove(tmp); return -1; }

    /* Rename rather than write in place: a power cut during the write then
     * leaves the OLD state intact. remove() first, because rename() onto an
     * existing target is not guaranteed by the libc and certainly not on FAT
     * through libfat. */
    remove(file);
    if (rename(tmp, file) != 0) { remove(tmp); return -1; }

    return 0;
}
