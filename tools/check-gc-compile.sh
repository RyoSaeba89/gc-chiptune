#!/bin/sh
# Quick check: does the audio layer compile for the Gekko, against the libogc2
# headers actually installed?
#
#   <devkitPro>/msys2/usr/bin/bash.exe -l .../tools/check-gc-compile.sh

set -e
export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build-host/gc-check"
mkdir -p "$OUT"

# Flags taken verbatim from $DEVKITPRO/libogc2/gamecube_rules.
#
# -mogc is not cosmetic: this devkitPPC spec defines HW_DOL, without which
# TB_BUS_CLOCK does not exist and ogc/timesupp.h will not compile
# (ticks_to_microsecs).
MACHDEP="-DGEKKO -mogc -mcpu=750 -meabi -mhard-float"

INC="-I$DEVKITPRO/libogc2/gamecube/include -I$DEVKITPRO/portlibs/gamecube/include -I$DEVKITPRO/portlibs/ppc/include"

echo "=== GameCube audio layer ==="
powerpc-eabi-gcc -c -O2 -Wall -Wextra $MACHDEP $INC \
    -o "$OUT/audio_gc.o" "$ROOT/src/gc/audio_gc.c"
echo "  audio_gc.o OK"

echo
