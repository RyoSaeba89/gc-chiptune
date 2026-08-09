/* ------------------------------------------------------------------------
 * GC-Chiptune: which modules contain 16-bit samples?
 *
 * WHY THIS TOOL EXISTS. libxmp only converts the 16-bit samples of an XM/IT --
 * stored little-endian in the file -- if WORDS_BIGENDIAN is defined at compile
 * time (loaders/sample.c:362). Our GameCube build did not define it: on the
 * Gekko, every 16-bit sample played byte-swapped, i.e. full-scale noise.
 *
 * 8-bit samples have no byte order: those modules sounded perfectly fine. Hence
 * a fault that hits "lots of" files but not all -- and that is INVISIBLE on a
 * PC, where the #else compiles the right branch.
 *
 * This tool counts both populations to say exactly how many files of the pack
 * were affected, and to verify after the fix.
 *
 * List of paths on stdin, one per line.
 * ------------------------------------------------------------------------ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xmp.h"

int main(int argc, char **argv)
{
    char line[1024];
    unsigned long n = 0, with16 = 0, only8 = 0, nosmp = 0, bad = 0;
    int verbose = (argc > 1 && !strcmp(argv[1], "-v"));

    while (fgets(line, sizeof line, stdin)) {
        xmp_context ctx;
        struct xmp_module_info mi;
        size_t len = strlen(line);
        int i, n16 = 0, ntot = 0;

        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (!len) continue;

        n++;
        ctx = xmp_create_context();
        if (!ctx) { bad++; continue; }
        if (xmp_load_module(ctx, line) != 0) {
            bad++;
            xmp_free_context(ctx);
            continue;
        }
        xmp_get_module_info(ctx, &mi);

        if (mi.mod) {
            ntot = mi.mod->smp;
            for (i = 0; i < mi.mod->smp; i++)
                if (mi.mod->xxs[i].len && (mi.mod->xxs[i].flg & XMP_SAMPLE_16BIT))
                    n16++;
        }

        if (ntot == 0)      nosmp++;
        else if (n16 > 0)   with16++;
        else                only8++;

        if (verbose)
            printf("%s\t%d/%d\t%s\n", n16 ? "16BITS" : "8bits", n16, ntot, line);

        xmp_release_module(ctx);
        xmp_free_context(ctx);
    }

    fprintf(stderr,
            "\nfiles                    : %lu\n"
            "  with 16-bit samples       : %lu  <- played as NOISE on Gekko\n"
            "  8-bit only                : %lu  <- intact\n"
            "  no samples                : %lu\n"
            "  refused                   : %lu\n",
            n, with16, only8, nosmp, bad);
    return 0;
}
