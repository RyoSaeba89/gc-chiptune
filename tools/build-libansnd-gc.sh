#!/bin/sh
# ---------------------------------------------------------------------------
# GC-Chiptune: builds libansnd for the GameCube.
#
# libansnd (extremscorner/libansnd, "Another Sound Library") replaces libasnd.
# It is the libogc2 maintainer's advice: "Use libansnd for best performance."
#
# What it changes over ASND, see docs/STATUS.md 11:
#   - samples are read by the DSP from ARAM through the hardware accelerator,
#     not copied by the Gekko for every block;
#   - windowed-sinc resampling, where ASND does linear;
#   - the library reports DSP load and total load separately.
#
# We do NOT use devkitPro's CMake toolchain (powerpc-eabi-cmake):
# ogc-common.cmake hard-codes OGC_ROOT=$DEVKITPRO/libogc, i.e. the original
# libogc. The whole project targets libogc2, whose DISC_INTERFACE and headers
# differ (docs/STATUS.md 8.1). Compiling against the wrong headers would pass
# without a word and break at run time.
#
# The library is one .c and one .s anyway: we redo the project Makefile's flags
# by hand.
#
# Run from the MSYS2 bash:
#   <msys2>/usr/bin/bash.exe -l tools/build-libansnd-gc.sh
# ---------------------------------------------------------------------------

set -e

: "${DEVKITPRO:?DEVKITPRO not set - run bash with -l to load devkit-env.sh}"
: "${DEVKITPPC:?DEVKITPPC not set - run bash with -l to load devkit-env.sh}"

export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"

SRC="$(cd "$(dirname "$0")/../extern/libansnd" && pwd)"
OUT="$(cd "$(dirname "$0")/.." && pwd)/build-gc"
DEST="$DEVKITPRO/libogc2/gamecube"

# Flags from $DEVKITPRO/libogc2/gamecube_rules. -mogc defines HW_DOL, which
# ansndlib.h needs on its very first line (#error otherwise).
MACHDEP="-DGEKKO -mogc -mcpu=750 -meabi -mhard-float"
# -fno-math-errno: required by the upstream CMakeLists (lrintf with no libc
# call).
CFLAGS="-O2 -DNDEBUG $MACHDEP -fno-math-errno -Wall"

echo "=== environment ==="
echo "  sources   : $SRC"
echo "  cible     : $DEST"
echo "  gcc       : $(powerpc-eabi-gcc -dumpversion)"
echo "  gcdsptool : $(command -v gcdsptool || echo ABSENT)"
echo

command -v gcdsptool >/dev/null || {
    echo "FAILED: gcdsptool not found (gamecube-tools package)." >&2
    exit 1
}

[ -d "$DEST/include" ] || {
    echo "FAILED: $DEST/include missing - run tools/build-libogc2.sh first." >&2
    exit 1
}

mkdir -p "$OUT/ansnd"
cd "$OUT/ansnd"

# The mixer runs ON THE DSP: gcdsptool assembles dspmixer.s into a C array that
# ansndlib.c includes and uploads into the DSP's IRAM at start-up.
echo "=== assembling the DSP mixer ==="
gcdsptool -c "$SRC/src/dspmixer.s" -o dspmixer.h
echo "  dspmixer.h : $(wc -c < dspmixer.h) bytes"
echo

echo "=== build ==="
powerpc-eabi-gcc -c $CFLAGS \
    -I"$SRC/include" -I. -I"$DEST/include" \
    -o ansndlib.o "$SRC/src/ansndlib.c"

powerpc-eabi-ar rcs libansnd.a ansndlib.o
echo "  libansnd.a : $(wc -c < libansnd.a) bytes"
echo

echo "=== installing into $DEST ==="
cp -f libansnd.a  "$DEST/lib/"
cp -f "$SRC/include/ansndlib.h" "$DEST/include/"
echo

echo "=== verification ==="
rc=0
for f in "$DEST/lib/libansnd.a" "$DEST/include/ansndlib.h"; do
    if [ -e "$f" ]; then echo "  OK      $f"; else echo "  MISSING $f"; rc=1; fi
done

echo
[ $rc -eq 0 ] && echo "libansnd installed." || echo "Installation incomplete." >&2
exit $rc
