# Installation, and picking the project up on another machine

Nothing in this repository depends on an absolute path: the scripts locate
themselves (`dirname "$0"`, `$PSScriptRoot`) and the tools come from
`$DEVKITPRO`. **The repository can therefore be cloned anywhere**, and devkitPro
installed anywhere.

---

## 1. Quick start

```sh
git clone https://github.com/RyoSaeba89/gc-chiptune.git
cd gc-chiptune

# rebuilds extern/ (4 repositories, pinned commits) and compiles the libraries
<devkitPro>/msys2/usr/bin/bash.exe -l tools/bootstrap.sh

# the player
<devkitPro>/msys2/usr/bin/bash.exe -lc "cd \"$PWD\" && make"
```

Then, on the PowerShell side, for the PC tools:

```powershell
.\build-host.ps1
```

Replace `<devkitPro>` with the real installation path (`C:\devkitPro` by default
on Windows).

---

## 2. Prerequisites

### 2.1 devkitPPC

devkitPro's **GameCube Development** group. Provides `powerpc-eabi-gcc`,
`elf2dol`, `bin2s` and `gcdsptool`.

`gcdsptool` is not optional: it assembles the DSP mixers of `libasnd` and
`libaesnd`, and without it the libogc2 build stops on `asnd_dsp_mixer.h`.

⚠️ **The one in `gamecube-tools 1.0.7` is not enough for libansnd.** Its
`src/dspmixer.s` uses extended DSP opcodes (`movr'ldaxn`) that the 1.0.6
assembler shipped with that package does not know: it stops on *"Unknown opcode
Line: 442"*. Take the one from the libogc2 maintainer's repository:

```sh
pacman -S gamecube-tools-git
```

It replaces `gamecube-tools` (the two conflict, which is expected) and assembles
libogc2's mixers just as well.

### 2.2 `ppc-libmad`

**A real dependency of libogc2**, not an extra: `libasnd/mp3player.c` and
`libaesnd/aesndmp3player.c` include `<mad.h>`. Without it, the build stops right
at the end.

```sh
pacman -S ppc-libmad
```

It comes from the **official** devkitPro repository, so no signature trouble.

### 2.3 A host compiler

GCC (MinGW-w64 here) for `build-host.ps1`, which produces the PC tools. No
external dependency: no SDL, no CMake.

---

## 3. The pacman trap, and why we build from source

Installing libogc2 **through pacman failed on the development machine**: the
upstream repository requires a locally signed PGP key, and `pacman-key
--recv-keys` goes through gpg's keyserver protocol (dirmngr), which was blocked
there. The key arrived but stayed at `unknown trust`, so pacman refused the
repository's database.

`tools/bootstrap.sh` works around the problem by building from source, and needs
**no administrator privilege** (the devkitPro tree is writable).

If the upstream repository works for you, `pacman -S libogc2 libfat-ogc2` does
the same thing — but **then check that `libfat` is the matching fork**, see §5.

> The broken libogc2 repository had to be removed from `pacman.conf` first, or
> `pacman -Sy` failed globally. The public key is kept in
> `tools/extremscorner-devkitpro.asc` if you want to retry the import.

---

## 4. libogc2, not libogc

**The project targets `libogc2`, the maintained fork.** libogc and libogc2
diverge on APIs, and part of this repository's code only compiles against
libogc2 (`-mogc`, `TB_BUS_CLOCK`, the shape of `DISC_INTERFACE`).

`tools/build-libogc2.sh` builds only the `cube` target. The upstream Makefile's
`install` target depends on `wii cube` and would build the whole Wii stack
(wiiuse, bluetooth, USB keyboard) with no use here; the script therefore redoes
by hand only the GameCube install lines.

Installed into `$DEVKITPRO/libogc2/gamecube`.

---

## 5. libfat: the matching fork, necessarily

devkitPro's **`libfat-ogc` package is unusable with libogc2**, for two reasons:

- it is built on FatFs + the `dvm` volume manager, and wants `f_mount`,
  `dvmInit`, `g_vfatFsDriver`, which no `libogc.a` defines;
- above all, **libogc2's `DISC_INTERFACE` is not libogc's**: libogc2 passes the
  `DISC_INTERFACE*` as the first argument of every callback, uses a 64-bit
  `sec_t` and adds `eraseSectors` / `flush` / `numberOfSectors` /
  `sectorsPerBlock` / `bytesPerSector`.

Linking the two would produce calls with the wrong signature, **silently**.
Hence the matching fork, built by `tools/build-libfat-gc.sh`.

---

## 5 bis. libansnd: the audio output

`tools/build-libansnd-gc.sh`, called by `bootstrap.sh`. One `.c` and one `.s`,
installed into `$DEVKITPRO/libogc2/gamecube`.

**We do not use devkitPro's CMake toolchain** (`powerpc-eabi-cmake`), which the
upstream README recommends: `cmake/ogc-common.cmake` hard-codes
`OGC_ROOT=$DEVKITPRO/libogc`, i.e. the **original** libogc. Compiling against its
headers would pass without a word and break at run time, for the reason given in
§5.

The `libogc2-libansnd-git` package from the upstream repository does the same
thing if that repository works for you.

---

## 6. Build flags not to lose

| flag | why |
|---|---|
| `-mogc` | defines `HW_DOL`. Without it `TB_BUS_CLOCK` does not exist and `ogc/timesupp.h` will not compile (`ticks_to_microsecs`). Not cosmetic. |
| `-std=c++11` | devkitPPC ships GCC 16, whose default C++ exposes `std::lerp`; `math.h` pulls it into the global namespace where it can collide with local helpers. |
| `-lfat` and `-lansnd` **before** `-logc` | libfat calls `__io_gcsda` & co. and libansnd the DSP, all defined in libogc. The reverse order leaves unresolved symbols. |
| `-DNDEBUG` | on a console, an assert calls `abort()`, i.e. freezes the machine. |

The machine flags come from `$DEVKITPRO/libogc2/gamecube_rules`, taken verbatim:
`-DGEKKO -mogc -mcpu=750 -meabi -mhard-float`.

---

## 7. Test corpus

`Chiptunes/` (~750 MB) is **not in the repository**: it is a personal
collection. Dropping it at the root of the repository is enough, the tools find
it.

Without it, everything builds and the `test/` set works; only the full-corpus
validation runs (`build-host.ps1 -Test`, `gcc_render -batch`) need the files.

> A second corpus, `KEYGENMUSiC MusicPack`, has been **culled**: the 5533 files
> were run through the backends (`gcc_render -triage`), 466 moved aside, 5067
> kept. See `pack-kept.txt` and `pack-dropped.txt` — **no need to redo that
> check**.

## 8. Dolphin

`Dolphin-x64/` is not in the repository either (~420 MB). Version used for all
the measurements: **Dolphin 2606**. The configuration to set and the traps are
in `TEST-DOLPHIN.md`.

⚠️ **Dolphin emulates neither the SD Gecko nor the SD2SP2.** The player
therefore shows "no SD card" there, and that is the expected behaviour. Only the
bench (`make APP=bench`), which embeds its tracks, can be measured under the
emulator.
