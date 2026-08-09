#!/bin/sh
# ---------------------------------------------------------------------------
# GC-Chiptune: builds libfat for libogc2 (GameCube).
#
#   <devkitPro>/msys2/usr/bin/bash.exe -l tools/build-libfat-gc.sh
#
# Normally called by tools/bootstrap.sh.
#
# Why not devkitPro's libfat-ogc package?
#
#   It may already be installed, but under the original libogc tree, and it is
#   INCOMPATIBLE with libogc2:
#
#   - libfat-ogc 2.x is built on FatFs + the "dvm" volume manager; it wants
#     f_mount, dvmInit, g_vfatFsDriver... which neither libogc.a defines.
#   - above all, libogc2's DISC_INTERFACE is NOT libogc's: libogc2 passes the
#     DISC_INTERFACE* as the first argument of every callback, uses a 64-bit
#     sec_t and adds eraseSectors / flush / numberOfSectors / sectorsPerBlock /
#     bytesPerSector. Linking the two would produce calls with the wrong
#     signature -- silently.
#
# So we build the matching libfat fork (extern/libfat), which follows the new
# interface, against libogc2. Same approach as build-libogc2.sh: no
# administrator privilege needed.
#
# We build the GameCube ONLY. The upstream Makefile's "install" target copies
# gamecube AND wii, and "cube-release" additionally tries libogc-rice (another
# libogc variant, absent here); so we call the libogc2/ sub-Makefile directly
# and copy by hand.
# ---------------------------------------------------------------------------

set -e

: "${DEVKITPRO:?DEVKITPRO not set - run bash with -l to load devkit-env.sh}"
: "${DEVKITPPC:?DEVKITPPC not set - run bash with -l to load devkit-env.sh}"

export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"

SRC="$(cd "$(dirname "$0")/../extern/libfat" && pwd)"
DEST="$DEVKITPRO/libogc2/gamecube"

echo "=== environnement ==="
echo "  DEVKITPRO : $DEVKITPRO"
echo "  sources   : $SRC"
echo "  cible     : $DEST"
echo "  gcc       : $(powerpc-eabi-gcc -dumpversion)"
echo

[ -f "$DEVKITPRO/libogc2/gamecube_rules" ] || {
    echo "FAILED: $DEVKITPRO/libogc2/gamecube_rules missing." >&2
    echo "        Run tools/build-libogc2.sh first." >&2
    exit 1
}

cd "$SRC"

# Generates include/libfatversion.h, required by libfat.c. A root Makefile
# target; we cannot go through "cube-release", which would drag in libogc-rice
# and the wii target.
echo "=== version ==="
make include/libfatversion.h
echo

echo "=== build (gamecube) ==="
make -C libogc2 PLATFORM=gamecube BUILD=gamecube_release
echo

echo "=== installing into $DEST ==="
mkdir -p "$DEST/lib" "$DEST/include"
cp -f libogc2/gamecube/lib/libfat.a "$DEST/lib/"
cp -f include/fat.h include/libfatversion.h "$DEST/include/"
cp -f libfat_license.txt "$DEVKITPRO/libogc2/"
echo

echo "=== verification ==="
ls -l "$DEST/lib/libfat.a" "$DEST/include/fat.h"
# fatInitDefault is the entry point src/gc uses: if it is missing, the rest is
# pointless.
powerpc-eabi-nm --defined-only "$DEST/lib/libfat.a" | grep -q "T fatInitDefault" \
    && echo "  fatInitDefault present -- OK" \
    || { echo "FAILED: fatInitDefault missing from libfat.a" >&2; exit 1; }
