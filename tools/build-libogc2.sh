#!/bin/sh
# ---------------------------------------------------------------------------
# GC-Chiptune: builds libogc2 from source.
#
# Works around pacman and its PGP signature check: the upstream repository
# requires a locally signed key, and key-server import (dirmngr) may be blocked.
#
# Run from the MSYS2 bash shipped with devkitPro:
#   <devkitPro>/msys2/usr/bin/bash.exe -l tools/build-libogc2.sh
#
# Normally called by tools/bootstrap.sh, which clones the sources first.
#
# No administrator privilege needed: the devkitPro tree is writable.
#
# We build ONLY the "cube" target (GameCube). The Makefile's "install" target
# depends on "wii cube" and would therefore also build the whole Wii stack
# (wiiuse, bluetooth, USB keyboard...) which we have no use for. So we redo by
# hand only the install lines that concern the GameCube.
# ---------------------------------------------------------------------------

set -e

: "${DEVKITPRO:?DEVKITPRO not set - run bash with -l to load devkit-env.sh}"
: "${DEVKITPPC:?DEVKITPPC not set - run bash with -l to load devkit-env.sh}"

export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"

SRC="$(cd "$(dirname "$0")/../extern/libogc2" && pwd)"
DEST="$DEVKITPRO/libogc2"

echo "=== environnement ==="
echo "  DEVKITPRO : $DEVKITPRO"
echo "  sources   : $SRC"
echo "  cible     : $DEST"
echo "  gcc       : $(powerpc-eabi-gcc -dumpversion)"
echo "  gcdsptool : $(command -v gcdsptool || echo ABSENT)"
echo

# gcdsptool assembles the DSP mixers of libasnd and libaesnd: without it the
# build stops on asnd_dsp_mixer.h.
command -v gcdsptool >/dev/null || {
    echo "FAILED: gcdsptool not found (gamecube-tools package)." >&2
    exit 1
}

cd "$SRC"

echo "=== build (cube target) ==="
make cube -j"$(nproc 2>/dev/null || echo 2)"
echo

echo "=== installing into $DEST ==="
mkdir -p "$DEST/gamecube/lib"
cp -fr include "$DEST/gamecube/"
cp -f  lib/cube/*.a "$DEST/gamecube/lib/"
cp -f  ./*_license.txt "$DEST/"
# gamecube_rules and wii_rules are included by project Makefiles.
cp -f  ./*_rules "$DEST/"
echo

echo "=== verification ==="
rc=0
for f in "$DEST/gamecube/include/ogc/audio.h" \
         "$DEST/gamecube/include/gccore.h" \
         "$DEST/gamecube/lib/libogc.a" \
         "$DEST/gamecube_rules"; do
    if [ -e "$f" ]; then
        echo "  OK      $f"
    else
        echo "  MISSING $f"
        rc=1
    fi
done

echo
echo "  installed libraries:"
ls -1 "$DEST/gamecube/lib/" | sed 's/^/    /'

echo
if [ $rc -eq 0 ]; then
    echo "libogc2 installed."
else
    echo "Installation incomplete." >&2
fi
exit $rc
