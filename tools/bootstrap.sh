#!/bin/sh
# ---------------------------------------------------------------------------
# GC-Chiptune: rebuilds extern/ and compiles the libraries.
#
# Run from devkitPro's MSYS2 bash, with -l so /etc/profile.d/devkit-env.sh is
# loaded (it sets DEVKITPRO and DEVKITPPC):
#
#   /path/to/devkitPro/msys2/usr/bin/bash.exe -l tools/bootstrap.sh
#
# No hard-coded paths: everything is relative to the script, and the tools come
# from $DEVKITPRO. The repository can therefore live anywhere.
#
# extern/ is NOT versioned: it is third-party code, and the project rule is that
# it stays strictly pristine (no patches to reapply on an update). The commits
# below are the ones everything was validated against.
#
# Options:
#   --no-build   clone only, compile nothing
#   --update     move to the latest upstream instead of the pinned commits (at
#                your own risk: the measurements were made on the commits
#                below)
# ---------------------------------------------------------------------------

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$ROOT/extern"

DO_BUILD=1
PINNED=1
for a in "$@"; do
    case "$a" in
        --no-build) DO_BUILD=0 ;;
        --update)   PINNED=0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

# --- repositories and validated commits ------------------------------------
# libogc2     the maintained fork -- NOT the original libogc
# libfat      the matching fork, the only one compatible with libogc2's
#             DISC_INTERFACE
# libansnd    the audio output; recommended by the libogc2 maintainer in place
#             of libasnd (docs/STATUS.md 11)
# libxmp      module player

OGC_URL=https://github.com/extremscorner/libogc2.git
OGC_SHA=cf2277922d0736b3780f5655b8a4638954cf8605

FAT_URL=https://github.com/extremscorner/libfat.git
FAT_SHA=c756e1e3024e2ef2e037e99d3fd057b37ac02c5b

ANSND_URL=https://github.com/extremscorner/libansnd.git
ANSND_SHA=cbb297e4dca4b28288a7426c9941f4133c55686c

XMP_URL=https://github.com/libxmp/libxmp.git
XMP_SHA=a13276d27feabcf9ee4f982913f718ee05a65cb7

echo "=== environment ==="
echo "  repo      : $ROOT"
if [ "$DO_BUILD" = 1 ]; then
    : "${DEVKITPRO:?DEVKITPRO not set -- run bash with -l}"
    : "${DEVKITPPC:?DEVKITPPC not set -- run bash with -l}"
    echo "  DEVKITPRO : $DEVKITPRO"
fi
echo "  commits   : $([ "$PINNED" = 1 ] && echo 'pinned' || echo 'latest upstream')"
echo

mkdir -p "$EXT"

# checkout <directory> <url> <sha>
checkout() {
    dir="$EXT/$1"; url="$2"; sha="$3"
    if [ -d "$dir/.git" ]; then
        echo "  $1: already present, leaving it alone"
        return
    fi
    echo "  $1: cloning..."
    git clone --quiet "$url" "$dir"
    if [ "$PINNED" = 1 ]; then
        git -C "$dir" checkout --quiet "$sha"
    fi
}

echo "=== repositories ==="

checkout libogc2 "$OGC_URL" "$OGC_SHA"
checkout libfat  "$FAT_URL" "$FAT_SHA"
checkout libansnd "$ANSND_URL" "$ANSND_SHA"
checkout libxmp  "$XMP_URL" "$XMP_SHA"
echo

# tsf (TinySoundFont) is not cloned: it is two single headers, MIT licensed,
# versioned directly in the repository (extern/tsf/).
if [ ! -f "$EXT/tsf/tsf.h" ]; then
    echo "WARNING: extern/tsf/tsf.h missing -- it should be in the repository." >&2
fi

# The soundfont is versioned as well (extern/soundfont/), see README.
if [ ! -f "$EXT/soundfont/TimGM6mb.sf2" ]; then
    echo "WARNING: extern/soundfont/TimGM6mb.sf2 missing -- MIDI will not play." >&2
fi

if [ "$DO_BUILD" = 0 ]; then
    echo "=== --no-build: stopping here ==="
    exit 0
fi

echo "=== libraries ==="
echo
echo "--- libogc2 ---"
sh "$ROOT/tools/build-libogc2.sh"
echo
echo "--- libfat ---"
sh "$ROOT/tools/build-libfat-gc.sh"
echo
echo "--- libansnd ---"
sh "$ROOT/tools/build-libansnd-gc.sh"
echo
echo "--- libxmp (GameCube) ---"
sh "$ROOT/tools/build-libxmp-gc.sh"
echo

echo "=== done ==="
echo
echo "Next:"
echo "  make                 the player     -> build-gc/gc-chiptune.dol"
echo "  make APP=bench       the CPU bench  -> build-gc/gc-chiptune-bench.dol"
echo "  ./build-host.ps1     the PC tools   (PowerShell, not this shell)"
