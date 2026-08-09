#!/bin/sh
# ---------------------------------------------------------------------------
# GC-Chiptune: cross-compiles libxmp for the Gekko.
#
#   <devkitPro>/msys2/usr/bin/bash.exe -l .../tools/build-libxmp-gc.sh
#
# Produces build-gc/libxmp.a.
#
# The source list is EXTRACTED from libxmp's Makefiles, not written by hand.
# libxmp ships several .c files that are not part of the build (ptk.c, pm.c,
# pm01.c, pm20.c, pm40.c, pp30.c ...): they no longer compile, and a
# hand-maintained list misses them every time. Same approach as the host build,
# see tools/libxmp-sources.ps1.
#
# Depackers EXCLUDED (LIBXMP_NO_DEPACKERS): temporary files and popen(),
# meaningless on GameCube, and they drag in lhasa. ProWizard kept: no external
# dependency, and it catches the exotic MOD variants.
# ---------------------------------------------------------------------------

set -e

: "${DEVKITPRO:?run bash with -l}"
: "${DEVKITPPC:?run bash with -l}"
export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LX="$ROOT/extern/libxmp"
OUT="$ROOT/build-gc"
OBJ="$OUT/libxmp-obj"

# Reads a Makefile variable, folding line continuations.
makevar() {
    awk -v var="$2" '
        BEGIN { collecting = 0 }
        {
            line = $0
            if (!collecting) {
                if (match(line, "^" var "[ \t]*[:+]?=")) {
                    line = substr(line, RSTART + RLENGTH)
                    collecting = 1
                } else next
            }
            cont = (line ~ /\\[ \t]*$/)
            sub(/\\[ \t]*$/, "", line)
            print line
            if (!cont) exit
        }' "$1"
}

# Expands an object list into source names, following $(OTHER_VARIABLE)
# references.
expand_objs() {
    for tok in $(makevar "$1" "$2"); do
        case "$tok" in
            '$('*')')
                inner=$(printf '%s' "$tok" | sed 's/^\$(//; s/)$//')
                expand_objs "$1" "$inner"
                ;;
            *.o)
                printf '%s\n' "${tok%.o}.c"
                ;;
        esac
    done
}

mkdir -p "$OBJ"
rm -f "$OBJ"/*.o "$OUT/libxmp.a"

MACHDEP="-DGEKKO -mogc -mcpu=750 -meabi -mhard-float"
# -DWORDS_BIGENDIAN IS NOT COSMETIC: IT IS SAMPLE DECODING.
#
# libxmp does not detect byte order by itself. common.h only defines
# WORDS_BIGENDIAN under AC_APPLE_UNIVERSAL_BUILD; everywhere else it comes from
# autotools or CMake, i.e. from the command line. Without it, on Gekko:
#
#   loaders/sample.c:362
#     if (xxs->flg & XMP_SAMPLE_16BIT) {
#     #ifdef WORDS_BIGENDIAN
#         if (~flags & SAMPLE_FLAG_BIGEND) convert_endian(...);   <- never built
#     #else
#         if (flags & SAMPLE_FLAG_BIGEND)  convert_endian(...);
#     #endif
#     }
#
# The 16-bit samples of an XM or an IT are little-endian in the file. Without
# this macro they are NEVER converted back: they play byte-swapped, i.e.
# full-scale noise. 8-bit samples have no byte order and came out perfectly --
# hence a fault that hits many files without hitting all of them.
#
# AND IT IS INVISIBLE ON A PC: x86 compiles the #else branch, which is the right
# one there. No amount of host rendering could have shown it. Exactly the same
# trap as the .sf2 in 7.5, in a different library.
#
# Two other places depend on it and are fixed by the same macro:
# loaders/xm_load.c:418 (OXM samples) and loaders/it_load.c:975 (output of the
# IT decompressor).
CFLAGS="-O2 -DLIBXMP_STATIC -DLIBXMP_NO_DEPACKERS -DWORDS_BIGENDIAN=1 -I$LX/include -I$LX/src $MACHDEP"

SRCS=""
for c in $(expand_objs "$LX/src/Makefile" SRC_OBJS); do
    SRCS="$SRCS $LX/src/$c"
done
for c in $(expand_objs "$LX/src/loaders/Makefile" LOADERS_OBJS); do
    SRCS="$SRCS $LX/src/loaders/$c"
done
for c in $(expand_objs "$LX/src/loaders/prowizard/Makefile" PROWIZ_OBJS); do
    SRCS="$SRCS $LX/src/loaders/prowizard/$c"
done

n=0
missing=0
for f in $SRCS; do
    if [ ! -f "$f" ]; then
        echo "  source announced but missing: $f" >&2
        missing=$((missing + 1))
        continue
    fi
    # Prefixed with the parent directory: several libxmp sub-folders contain a
    # common.c, and a flat ar would overwrite them.
    o="$OBJ/$(basename "$(dirname "$f")")_$(basename "$f" .c).o"
    powerpc-eabi-gcc -c $CFLAGS -o "$o" "$f"
    n=$((n + 1))
done

powerpc-eabi-ar rcs "$OUT/libxmp.a" "$OBJ"/*.o

echo "libxmp.a: $n sources compiled, $missing missing, $(stat -c %s "$OUT/libxmp.a") bytes"
