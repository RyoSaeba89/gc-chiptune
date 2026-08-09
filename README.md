# GC-Chiptune

Chiptune music player for the **GameCube**, on libogc2. Two formats, two
engines, one internal interface:

| format | engine | state on a real GameCube |
|---|---|---|
| **`.mid`** | TinyMidiLoader + TinySoundFont + GM soundfont | 48 kHz with 64 voices |
| modules | libxmp (MOD/XM/IT/S3M and ~90 others) | free — 18 per mille at worst |

There was a third format, farbrausch's `.v2m`. It has been **dropped**: the V2
synth does not hold real time on the Gekko, one file in six stutters, and no
workaround covers the files a user might bring. The numbers and the options that
were examined are in `docs/STATUS.md` §13.

Tracks are read from an **SD card** (SD Gecko slot A/B, or SD2SP2), walked
recursively.

> **State: complete.** Two-pane GX interface, playback from the SD card, index
> and resume, measurements taken on a real GameCube. The release lands in
> `build-host/release/` — see [Install](#install).

---

## Where to start

| you want to… | read |
|---|---|
| set the environment up and build | **[docs/INSTALLATION.md](docs/INSTALLATION.md)** |
| know what the player does not do | **[docs/LIMITS.md](docs/LIMITS.md)** |
| **measure on a real GameCube** | **[docs/TEST-HARDWARE.md](docs/TEST-HARDWARE.md)** — the protocol |
| re-run a measurement in Dolphin | [docs/TEST-DOLPHIN.md](docs/TEST-DOLPHIN.md) |
| know why 12 files of the corpus do not play | [docs/corrupt-modules.txt](docs/corrupt-modules.txt) |

> **`docs/STATUS.md` is not published.** It is the study journal — the
> chronological account of every fault found, every measurement taken and every
> decision reversed, written for the person maintaining this. The source
> comments and the documents above cite its sections by number (`§13`, `§12.20`,
> `§20`…); those citations point into a document that lives outside this
> repository. **Nothing needed to build, run or measure the player depends on
> it**: the conclusions it reaches are restated in `LIMITS.md` and in the
> comments at the point they apply.

---

## Install

Download the release archive. **Nothing to convert, nothing to prepare** — the
soundfont in it is ready to use.

| what | where |
|---|---|
| `gc-chiptune.dol` | anywhere; loaded by Swiss, SD Media Launcher, GC Loader, a modchip |
| `soundfont.sf2` | **next to the `.dol`**, as it is. Without it `.mid` files are skipped; modules play regardless |
| your music | `sd:/chiptunes`, `sd:/GC-Chiptune`, `sd:/music`, or the root — sub-folders allowed |

Modules and MIDI files are copied **as they are**.

> The archive also holds `gpl-source/TimGM6mb.sf2`. **That one is not for the
> card** — it is the unconverted original, present only because GPL-2.0 requires
> the source to travel with the binary. The player refuses it and says so.

The player writes two files next to the music: `gc-chiptune.idx` (the index and
the durations — without it the first start-up walks the card again, 4.5 s for
5000 tracks) and `gc-chiptune.state` (where you were). Deleting them breaks
nothing.

To rebuild the release after a change:

```powershell
bash -lc "make"                 # the .dol
.\tools\make-release.ps1        # + converted soundfont + README
```

---

## The buttons

**One button = one role, always the same. There is no mode.** The first version
moved a focus with `START`; in use, nobody knew which state they were in.

```
THE LIST                              THE PLAYER
  ↑ ↓    cursor (hold to scroll)        X      pause
  ← →    leave / enter a folder         Y      repeat mode
  L R    jump ten rows                  Z      shuffle
  A      enter / play                   C ← →  volume
  B      up one level                   C ↑ ↓  previous / next
  START  play the WHOLE tree

  L+R    rebuild the index
```

`A` on a file plays **the current folder alone**, `START` plays the targeted
folder **and everything under it**. The current track stays marked in the list
even while you browse elsewhere — and playback does not stop while you navigate.

```sh
git clone https://github.com/RyoSaeba89/gc-chiptune.git && cd gc-chiptune
<devkitPro>/msys2/usr/bin/bash.exe -l tools/bootstrap.sh    # extern/ + libraries
<devkitPro>/msys2/usr/bin/bash.exe -lc "cd \"$PWD\" && make"
```

No absolute path anywhere: the repository and devkitPro can live wherever you
like.

---

## Two rules you cannot work around

**1. A soundfont has to be prepared offline — and the one you are given already
is.** The Gekko is big-endian, the `.sf2` format is little-endian, and
TinySoundFont reads the RIFF container by raw pointer copy: a stock soundfont
cannot work here. So the release ships it **already converted**. Copy it as it
is; you have nothing to run.

This only becomes your problem if you insist on a *different* bank — and there
is no reason to. If you do, `tools/sf2_prep` converts one:

```powershell
.\build-host\sf2_prep.exe your-bank.sf2 soundfont.sf2   # one other bank
.\tools\make-sdcard.ps1 -Source Chiptunes               # or a whole card at once
```

The player detects an unprepared soundfont and says so ("soundfont not
prepared") instead of failing with no explanation. Modules and MIDI files, on
the other hand, are always copied as they are.

**2. `extern/` stays strictly pristine.** No patch is applied there, so an
upstream update requires nothing to be reapplied.

---

## CPU load, measured on a real GameCube

In per mille of real time — **1000 = hard limit**. Corpus worst cases, **tracks
played in full**, and it is the **worst second** that is reported, not the mean.

| track | avg | worst second | dry | verdict |
|---|---|---|---|---|
| **MIDI worst case, 64 voices** *(the setting)* | 593 | **675** | 0 | factor 1.5 of margin |
| MIDI worst case, no voice limit | 1346 | 2574 | 20820 | 254 stacked voices — not the setting |
| module worst case | 15 | 23 | 0 | free |

Three results that were not guessable:

- **the console/Dolphin gap is only ×1.15**, and modules are even **faster on
  real silicon** than under the emulator;
- **twelve seconds of an opening do not describe a track.** That is what made
  the V2M budget look tenable for two days: its load rose well after the window
  the bench was looking at (§13). The bench now plays tracks all the way
  through;
- **the DMA transfer to ARAM costs ~3 per mille**, now counted in the load. It
  was excluded, and "libansnd costs no more CPU" rested on that exclusion.

Full detail, method and caveats: `docs/STATUS.md` §10 and §12.

---

## Tree

```
src/gc/          the GameCube binary: audio, SD card, playlist, UI
src/player/      common interface to the 2 backends + soundfont conversion
tools/           bootstrap, library builds, PC tools, card preparation
docs/            INSTALLATION.md, LIMITS.md, TEST-HARDWARE.md, TEST-DOLPHIN.md
data/            tracks embedded in the measurement bench
test/            a test set ready to copy onto an SD card, with the .dol
extern/          third-party code — PRISTINE, rebuilt by tools/bootstrap.sh
```

Two binaries from the same tree:

```sh
make               # the player, tracks read from the SD card
make APP=bench     # the CPU measurement bench, tracks and soundfont embedded
```

The bench exists because **Dolphin emulates neither the SD Gecko nor the SD2SP2**
(checked on Dolphin 2606): embedding the tracks is the only way to measure
anything without a console.

---

## Test set

`test/` holds the `.dol` and four tracks — one light and one heavy per engine,
chosen **by measurement** after ranking the whole corpus — plus the soundfont.
Copy `test/chiptunes` to the root of a FAT32 SD card. See `test/README.txt`.

---

## Licences

**The source and the `.dol` are [MIT](LICENSE). The soundfont shipped alongside
is GPL-2.0.**

Two separate works on the same medium: the player reads a data file at start-up,
it does not derive from it. That was not true for a while — the soundfont was
*embedded in the binary*, which imposed GPL-2.0 on the whole thing. It was taken
back out for memory reasons (5.7 MB permanently out of 24, §12.20), and the
licence followed.

Redistributing the soundfont requires providing its source: hence
`extern/soundfont/TimGM6mb.sf2`, the only file of `extern/` that is versioned
here, and `gpl-source/TimGM6mb.sf2` in the release archive — kept in a
sub-folder so it cannot be mistaken for the one to copy.

| component | licence |
|---|---|
| the code in this repository | MIT |
| libogc2, libfat | see their repositories |
| libxmp | LGPL-2.1+ |
| TinySoundFont, TinyMidiLoader | MIT |
| GRRLIB | MIT |
| FreeType | FTL or GPL-2+ |
| libpng, zlib, brotli, bzip2 | permissive |
| DejaVu Sans Condensed | Bitstream Vera + Arev |
| **TimGM6mb.sf2** | **GPL-2.0** — separate file |

Everything else being permissive, swapping TimGM6mb for MuseScore's `MS Basic`
(MIT) would make the release entirely permissive, binary **and** data. That is a
change to make **here**, in a future release, so that users keep getting a
soundfont that just works — not something to hand to them as homework. See
§12.20 of `docs/STATUS.md`.
