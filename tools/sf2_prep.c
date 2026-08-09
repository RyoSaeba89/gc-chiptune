/* ------------------------------------------------------------------------
 * GC-Chiptune: offline preparation of a soundfont for the GameCube.
 *
 *   sf2_prep <input.sf2> <output.sf2> [-le]
 *   sf2_prep -check <file.sf2>
 *
 * By default the output is BIG-ENDIAN, the Gekko's order. -le produces
 * little-endian, which is only useful to replay the round trip on a PC.
 *
 * Why offline: see src/player/sf2_endian.h. In short, tsf.h reads the RIFF
 * container by raw copy; the console therefore receives a file already in its
 * byte order, and merely loads it.
 *
 * The round trip (little -> big -> little) is verified here on every
 * conversion: if the reconstructed file is not bit-identical to the original,
 * the field map is wrong somewhere, and we refuse to write.
 * ------------------------------------------------------------------------ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/player/sf2_endian.h"

static unsigned char *slurp(const char *path, size_t *out_len)
{
    FILE          *f = fopen(path, "rb");
    unsigned char *buf;
    long           n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);

    buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);

    *out_len = (size_t)n;
    return buf;
}

static void print_info(const sf2_info *in)
{
    printf("   %lu presets, %lu instruments, %lu samples\n",
           in->presets, in->instruments, in->samples);
    printf("   smpl block %lu bytes (%lu s16 samples, %.1f MB as float)\n",
           in->sample_bytes, in->sample_bytes / 2,
           (double)in->sample_bytes * 2.0 / (1024.0 * 1024.0));
    printf("   %lu chunks traversed, %lu fields converted\n",
           in->chunks, in->swapped);
}

static int cmd_check(const char *path)
{
    size_t         len;
    unsigned char *data = slurp(path, &len);
    sf2_state      st;
    sf2_info       info;
    int            rc;

    if (!data) { fprintf(stderr, "%s: unreadable\n", path); return 1; }

    st = sf2_check(data, len);
    printf("%s\n", path);

    /* sf2_check answers relative to the CURRENT machine -- which is what the
     * player needs on the Gekko, but said as-is on an x86 PC it would be
     * misleading ("reversed order" for the very file that is prepared). So we
     * name the file's real order, and what it is worth for the console. */
    if (st == SF2_STATE_UNKNOWN) {
        printf("   %lu bytes, state: not recognised\n", (unsigned long)len);
    } else {
        int host = sf2_host_is_big_endian();
        int big  = (st == SF2_STATE_PREPARED) ? host : !host;
        printf("   %lu bytes, %s -- %s\n", (unsigned long)len,
               big ? "big-endian" : "little-endian",
               big ? "prepared for the GameCube"
                   : "standard sf2, MUST BE CONVERTED for the GameCube");
    }

    /* Structural validation: we read in the order sf2_check announced and
     * write nothing (in_big == out_big). */
    if (st != SF2_STATE_UNKNOWN) {
        int big = (st == SF2_STATE_PREPARED) ? sf2_host_is_big_endian()
                                             : !sf2_host_is_big_endian();
        rc = sf2_convert(data, len, big, big, &info);
        if (rc != SF2_OK) {
            printf("   INVALID STRUCTURE: %s", sf2_strerror(rc));
            if (info.error_chunk) printf(" (%s at offset %lu)",
                                         info.error_chunk, info.error_offset);
            printf("\n");
            free(data);
            return 1;
        }
        print_info(&info);
        printf("   describes %lu bytes out of %lu\n", info.consumed, (unsigned long)len);
    }

    free(data);
    return 0;
}

static int cmd_convert(const char *in_path, const char *out_path, int to_le)
{
    size_t         len;
    unsigned char *data = slurp(in_path, &len);
    unsigned char *copy = NULL;
    sf2_info       info;
    FILE          *f;
    int            rc, out_big = !to_le;

    if (!data) { fprintf(stderr, "%s: unreadable\n", in_path); return 1; }

    if (sf2_check(data, len) == SF2_STATE_UNKNOWN) {
        fprintf(stderr, "%s: not a usable SF2 soundfont\n", in_path);
        free(data);
        return 1;
    }

    /* Reference copy for the round trip. */
    copy = (unsigned char *)malloc(len);
    if (!copy) { fprintf(stderr, "out of memory\n"); free(data); return 1; }
    memcpy(copy, data, len);

    printf("%s\n", in_path);

    /* A standard .sf2 is little-endian: in_big = 0. */
    rc = sf2_convert(data, len, 0, out_big, &info);
    if (rc != SF2_OK) {
        fprintf(stderr, "   FAILED: %s", sf2_strerror(rc));
        if (info.error_chunk) fprintf(stderr, " (%s at offset %lu)",
                                      info.error_chunk, info.error_offset);
        fprintf(stderr, "\n");
        free(copy); free(data);
        return 1;
    }
    print_info(&info);

    /* Round trip: converting back to little-endian must yield the original. */
    {
        unsigned char *back = (unsigned char *)malloc(len);
        sf2_info       bi;
        int            same;

        if (!back) { fprintf(stderr, "out of memory\n"); free(copy); free(data); return 1; }
        memcpy(back, data, len);

        rc = sf2_convert(back, len, out_big, 0, &bi);
        same = (rc == SF2_OK) && (memcmp(back, copy, len) == 0);
        free(back);

        if (!same) {
            fprintf(stderr, "   FAILED: the round trip does not yield the original.\n");
            fprintf(stderr, "   The field map is incomplete: do not use.\n");
            free(copy); free(data);
            return 1;
        }
        printf("   round trip: bit-identical\n");
    }
    free(copy);

    f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "   %s: cannot write\n", out_path);
        free(data);
        return 1;
    }
    if (fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "   %s: incomplete write\n", out_path);
        fclose(f); free(data);
        return 1;
    }
    fclose(f);
    free(data);

    printf("   -> %s (%s)\n", out_path, out_big ? "big-endian" : "little-endian");
    return 0;
}

int main(int argc, char **argv)
{
    int i, to_le = 0;
    const char *in_path = NULL, *out_path = NULL;

    if (argc >= 3 && strcmp(argv[1], "-check") == 0)
        return cmd_check(argv[2]);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-le") == 0)      to_le = 1;
        else if (!in_path)                    in_path  = argv[i];
        else if (!out_path)                   out_path = argv[i];
    }

    if (!in_path || !out_path) {
        fprintf(stderr, "usage: sf2_prep <input.sf2> <output.sf2> [-le]\n");
        fprintf(stderr, "       sf2_prep -check <file.sf2>\n");
        return 2;
    }

    return cmd_convert(in_path, out_path, to_le);
}
