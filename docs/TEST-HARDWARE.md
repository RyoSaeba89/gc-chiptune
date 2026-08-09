# Measurement campaigns on real hardware

- [Campaign 1 — ASND output](#campaign-1) *(below)*
- [Campaign 2 — libansnd output](#campaign-2--libansnd)
- [**Campaign 3 — the interface**](#campaign-3--the-interface)
- [Campaign 4 — V2M endurance](#campaign-4--v2m-endurance-closed--the-format-is-dropped) *(closed)*
- [Campaign 5 — the bench, whole tracks](#campaign-5--the-bench-whole-tracks)
- [Campaign 6 — interface responsiveness](#campaign-6--interface-responsiveness)

> ⚠️ **`.v2m` has been dropped** (see `STATUS.md` §13). Campaigns 1
> and 2 contain lines for it: those are historical records, kept as they were
> written. The current player only reads modules and MIDI.

---

## Campaign 1

Every figure in `STATUS.md` §7 comes from Dolphin. Dolphin is not
cycle-accurate and, above all, **does not emulate the caches** (§7.3): the V2
synth state is 2.5 MB against 256 kB of L2 on the Gekko. The console should
therefore be **slower** than those measurements, by an unknown factor.

This campaign goes looking for that factor.

---

## What we measure, and why nothing else

| | |
|---|---|
| tracks | the **three worst cases** of the corpus, one per backend, and those only |
| frequencies | **48 kHz** and **32 kHz** — the AI's only two rates |
| polyphony | **unlimited**, MIDI included |

Six phases, 12 s each, ~1 min 30 in total. The bench runs them on its own.

**Why the worst cases only.** Light and "typical" tracks settle nothing: the
typical module runs at 29 per mille under Dolphin, it would still be free ten
times slower. On a console driven by hand, every phase costs 12 s of
measurement; better to keep only the ones that can change a decision.

**Why with no voice limit.** The 64-voice ceiling (§7.7) is the production
setting, not a measurement. It takes the MIDI worst case from 1525 to 506 per
mille — that is, it **masks exactly the gap** between Dolphin and the console
that we are trying to quantify. So we measure the raw number. The ceiling is
reapplied afterwards, once we know how much everything moved.

The three tracks, chosen by ranking the whole corpus individually on the host
(`test/README.txt`):

| backend | file | host |
|---|---|---|
| v2m | `HoG - Spec Ops The Line` | ×8.5 |
| xm | `DARKSiDERS - Royal Heroes intro` | ×73.0 |
| midi | `BACKLASH - Fighting Force` | ×15.3 |

⚠️ The embedded XM **changed**. The §7.1 measurements were on `data/test.xm`, a
*typical* module, not the corpus's worst case. The bench now embeds
`data/worst.xm`, and the Dolphin reference line below was redone with it.

---

## Building

```sh
"$DK/msys2/usr/bin/bash.exe" -lc "cd \"$PWD\" && make APP=bench"
```

→ `build-gc/gc-chiptune-bench.dol`, **7.9 MB**.

The tracks **and the soundfont** are embedded in the `.dol`. On real hardware
the SD card would work, but we keep it embedded for two reasons: the figures
stay directly comparable with Dolphin's (same `.dol`, same load path), and the
measurement does not depend on an SD driver not yet validated on a console
(§8.4).

---

## Running it on the console

The `.dol` loads by any of the usual means — Swiss from an SD card (SD Gecko
slot A/B or SD2SP2), SD Media Launcher, GC Loader, a modchip.

Nothing else to copy: the bench is self-contained. **The SD card is not
required** to measure; it is only used to retrieve the results (see below).

Allow 1 min 30. The bench does not wait for a gamepad.

---

## Collecting the results

**On screen.** The table fills in line by line, the running phase marked
`<== running`. At the end the screen freezes: `avg`, `max`, `dry` per
configuration.

**On the SD card.** Right at the end, the bench mounts the card and writes
`gc-chiptune-bench.txt` at its root. It is text, directly usable — on a console
there is no debug output and no Dolphin audio dump, and the only alternative
would be photographing the screen.

The write happens **after** all the measurements: a mount failure cannot skew
them. The last line displayed says what happened:

| display | meaning |
|---|---|
| `written to SD (SD Gecko slot A)` | file written, and **the SD driver works** — §8.4 closed |
| `SD card: none` | no carrier detected; the figures are on screen |
| `SD ...: write refused` | card mounted but not writable (FAT32? lock?) |

That is incidentally the first real exercise of the SD mount, which could not be
validated under Dolphin.

---

## Reading the figures

Load in **per mille of real time**. 1000 = the render takes exactly the duration
it produces, i.e. the hard limit.

- **`max`** decides: it is what causes underruns.
- **`avg`** says whether any margin is left.
- **`dry`** is the final arbiter: one is enough to hear.
- **`voices`** (MIDI) = requested limit / observed peak. Here `inf/…`: the peak
  is the real polyphony the track asks for. Dolphin gives **200 to 205** —
  release tails piling up, not notes.

---

## Results

Dolphin reference, **taken on this same bench**, identical matrix (Dolphin
2606):

| track | kHz | avg | max | dry | voice peak |
|---|---|---|---|---|---|
| V2M worst case | 48 | 897 | 1071 | 127 | — |
| V2M worst case | 32 | 675 | 797 | 3 | — |
| XM worst case | 48 | 16 | 28 | 0 | — |
| XM worst case | 32 | 11 | 17 | 0 | — |
| MIDI worst case, no limit | 48 | 697 | 1525 | 95 | **200** |
| MIDI worst case, no limit | 32 | 448 | 914 | 17 | **205** |

Three checks this run gives along the way:

- the two **V2M lines reproduce to the figure** those of §7.1 (897/1071 and
  675/797): changing the matrix did not make the bench drift;
- the **XM worst case costs the same as the typical module** (28 against 29 per
  mille). The ×73 / ×1111 gap measured on the host is not found here: both are
  far below the measurement floor;
- the **SD dump** answers "SD card: none" and returns — the expected behaviour
  under Dolphin (§8.4), and proof that its failure freezes nothing.

Console measurements, SD Gecko slot A:

| track | kHz | avg | max | dry | voice peak | max/Dolphin |
|---|---|---|---|---|---|---|
| V2M worst case | 48 | 1016 | 1229 | 134 | — | ×1.15 |
| ~~V2M worst case~~ | ~~32~~ | ~~764~~ | ~~897~~ | ~~54~~ | — | — |
| XM worst case | 48 | 10 | 18 | 0 | — | **×0.64** |
| XM worst case | 32 | 7 | 11 | 0 | — | ×0.65 |
| MIDI worst case | 48 | 815 | 1720 | 118 | 174 | ×1.13 |
| MIDI worst case | 32 | 565 | 1156 | 54 | 205 | ×1.26 |

⚠️ **The V2M line at 32 kHz is struck through: it measured silence.** The V2
synth diverges into NaN below 44.1 kHz, and the player converted those NaNs to
zeros without saying anything (`STATUS.md` §10.4). A load of 764/896
with zero underruns, on a completely wrong render — that is what kept "32 kHz"
written down as a decision for three weeks.

**The caveat of §7.3 was founded, but modest: ~1.15×.** Not the factor that
would have overturned any format decision.

And a result nobody expected: **the XM is faster on real silicon than under
Dolphin** (18 against 28 per mille). The emulator was the pessimist on that
backend.

The SD mount worked first time — `gc-chiptune-bench.txt` written to slot A. The
point left open in §8.4 is closed.

### What these figures did not say

This campaign delivered something other than figures: **the sound crackled on
all three formats**, including the XM at 18 per mille with zero underruns
counted.

Those two facts are incompatible with a CPU problem, and that is what led to the
real defect — the audio output layer, not the synths. See
`STATUS.md` §10. The table's figures remain valid: the load is
measured on the render time, whether the output is correct or not.

---

## What the figures decided

1. **Console/Dolphin ratio: ×1.15.** No format decision to revisit.
2. **MIDI voice ceiling: 64 voices holds.** The lines above are *without* a
   limit, which is not the production setting — that was the point, getting the
   raw number. The cost being nearly linear in the number of voices, the ceiling
   brings the worst case to 506 × 1.13 ≈ **570 per mille at 48 kHz**.
   Comfortable.
3. **Modules: free, and more so than estimated.** 18 per mille at worst.
4. **V2M: 48 kHz mandatory**, 32 kHz never having worked (§10.4). The worst case
   is over real time there (1229) — but it is **the only file of the corpus in
   that position** out of 130, the mean being 397 (§10.5).

---
---

# Campaign 2 — libansnd

The audio output moved from ASND to **libansnd** (`STATUS.md` §11).
**All of campaign 1's figures are therefore void**: the sample path changed from
end to end.

This campaign redoes them identically. Same matrix, same tracks, same duration —
that is the condition for the comparison to mean anything.

## The two binaries, since the question comes up

| | `make` | `make APP=bench` |
|---|---|---|
| file | `build-gc/gc-chiptune.dol` | `build-gc/gc-chiptune-bench.dol` |
| size | **1.0 MB** | **7.9 MB** |
| role | the player | the measurement bench |
| music | read from the SD card | **embedded** (`bin2s`) |
| soundfont | read from the SD card | **embedded** |

A 1.0 MB player is not a crippled player: it is the other target. Nothing was
removed — the player has never embedded anything since it started reading the SD
card (§8). It is the **bench** that carries the worst cases and the soundfont,
and it is the bench we measure. `test/gc-chiptune.dol` is a copy of the player.

## What we measure — unchanged

| | |
|---|---|
| tracks | the **three worst cases** of the corpus, one per backend, and those only |
| frequencies | **48 kHz** and **32 kHz** |
| polyphony | **unlimited** first, then both production settings |

Eight phases, 12 s each, ~1 min 40. The bench runs them on its own, no gamepad.

The first six lines are **without a voice limit**: that is the raw number, the
one that lets the effect of the output change show. The 64-voice ceiling is a
production setting, it would mask precisely what we are looking for. The last
two lines measure it anyway, so it does not have to be extrapolated —
extrapolation is what has been expensive twice (§7.1, §10.6).

The **V2M at 32 kHz line stays on the programme and must be REFUSED**: "the V2
synth diverges below 44.1 kHz". It is the guard rail's witness. If it shows a
load, the guard rail has fallen.

## Building

```sh
"$DK/msys2/usr/bin/bash.exe" -lc "cd \"$PWD\" && make APP=bench"
```

→ `build-gc/gc-chiptune-bench.dol`, **7.9 MB**.

⚠️ New prerequisite: `pacman -S gamecube-tools-git`. The `gcdsptool` 1.0.6 of
the `gamecube-tools` package cannot assemble libansnd's DSP mixer (§11.4).

## Running

Like campaign 1: Swiss, SD Media Launcher, GC Loader, it does not matter. The
bench is self-contained, the SD card is only used to retrieve
`gc-chiptune-bench.txt` at the end. Allow 1 min 40.

⚠️ **Dolphin is out of the game this time.** libansnd uploads its **own DSP
microcode**, which the emulator's HLE audio cannot execute. There will therefore
be no Dolphin reference column — the console is the only judge, and that is a
first.

## What to look at, in this order

**1. Does it produce sound at all?** Before any figure. The sample path is new
from end to end: render → MRAM → ARQ DMA → **ARAM** → DSP accelerator. None of
that existed before. An ARAM address error would give noise, silence, or a track
stuttering on the same block.

**2. Does it crackle?** That is the symptom that led to the real defect last
time, at 18 per mille of load with zero underruns counted. The queue went from
~128 ms (ASND) to **~256 ms** (6 ARAM blocks, 2 of them at the DSP), so more
margin in principle — but "in principle" cannot be heard.

**3. The `dry` column.** It now counts `ANSND_VOICE_STATE_FINISHED`: the voice
exhausted its buffers. Same meaning as before, reported plainly by the library
instead of being guessed.

**4. The `dsp` column.** On ASND: 52 to 84 per mille. On libansnd, **unknown** —
it does more work (it also reads the samples) but with sinc instead of linear.
Upstream advertises 5.56 % for one resampled voice on GameCube; we have **one**.
A figure around 56 per mille would be consistent.

**5. The `DSP STALLED` verdict.** New. When the DSP falls behind, libansnd
returns an error rather than a load, and without this test the `dsp` column
would show **0 per mille** — the best value in the table, on a dead output. That
is the exact shape of the trap that cost three weeks on the V2M at 32 kHz. It
should never fire with a single voice.

## Grid to fill in

Campaign 1 reference (console, ASND) alongside:

| track | kHz | voices | ASND avg | ASND max | ASND dry | ASND dsp | libansnd avg | max | dry | dsp | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|
| V2M worst case | 48 | — | 1027 | 1231 | 37 | 84 | | | | | |
| V2M worst case | 32 | — | *refused* | | | | | | | | *must stay refused* |
| XM worst case | 48 | — | 12 | 20 | 0 | 61 | | | | | |
| XM worst case | 32 | — | 8 | 12 | 0 | 52 | | | | | |
| MIDI, no limit | 48 | inf/200 | 980 | 2028 | 103 | 84 | | | | | |
| MIDI, no limit | 32 | inf/205 | 544 | 1157 | 0 | 52 | | | | | |
| MIDI, 64 voices | 48 | 64/64 | 474 | 663 | 0 | 61 | | | | | |
| MIDI, 64 voices | 32 | 64/64 | 282 | 409 | 0 | 52 | | | | | |

Plus, outside the table: **does it crackle, yes or no, on each phase.**

## What would change a decision

| observation | consequence |
|---|---|
| Gekko load drops noticeably | ASND's MRAM copy cost more than expected; Spec Ops could go back under 1000 and the fallback of §10.5 becomes unnecessary |
| Gekko load does not move | expected — the Gekko renders the same thing, we have only taken one copy away from it |
| Gekko load **rises** | the synchronous ARQ transfer costs more than the copy it replaces; switch to `ARQ_PostRequestAsync` |
| DSP load rises a lot | sinc is expensive; the voice can be pinned to `ANSND_DSP_FREQ_48KHZ` to remove resampling |
| it still crackles | it is not the library, it is us — look at the 6-block ring before anything else |
| it produces no sound at all | ARAM addressing; check `AR_Init` before `ansnd_initialize`, and that `frame_count` is in **frames**, not bytes |

## Trial 1 — **Exception (DSI)**

The bench froze on an exception screen. Photographed, and the screen was enough:

```
Exception (DSI) occurred!
GPR09 00000000
SRR0 800BBAAC   DAR 00000000   DSISR 06000000
CODE DUMP:
800bbaac:  91490000  91490004  91490008  9148FFFC
```

`DSISR 0x06000000` = a **write** access, `DAR = 0` = at address **zero**. The
instruction is `91490000` → `stw r10, 0(r9)`, and `GPR09 = 0`. A write through a
null pointer, no ambiguity possible.

```sh
powerpc-eabi-addr2line -f -e build-gc/gc-chiptune-bench.elf 0x800bbaac 0x8008b5a8
    memset
    ansnd_dsp_request_callback
```

Cause: **an upstream libansnd bug** — returning a voice that was never
configured does `memset(NULL, 0, 128)` from the DSP interrupt. The full
mechanism is in `STATUS.md` §11.5.

It is no accident that it happened so fast: the trigger is `gcc_open()` refusing
a track, and **phase 2 of the bench is precisely the one that must be refused**
(V2M at 32 kHz). The guard rail's witness became the crash's trigger.

**Fixed** — one voice, taken once, never returned. `.dol` rebuilt. The sequence
"open refused then shutdown" is now exercised twice by the bench itself, so the
fix verifies itself: if phase 2 shows `refused: ...` and phase 3 starts, it is
sorted.

## Trial 2 — eight phases, but **it crackles**

The bench went all the way and wrote to the card (slot A). Full figures and
comparison with ASND: `STATUS.md` §11.7.

In three lines:

- **Gekko load identical** to ASND, to the per mille, over all eight phases;
- **DSP load halved at 48 kHz** (31 against 61-84), rising to 58 at 32 kHz —
  which is consistent, that is the ×1.5 resampling;
- **underruns sharply down** (37→7, 103→30) on the phases that overflow.

**But the sound crackles again, on all three backends.** Including the XM, at 12
per mille of load and zero underruns — the exact signature of §10.1: a defect
that depends neither on the backend nor on the load is **downstream of the
render**.

Cause found in our layer, not in the library: **the ARAM ring was one slot too
short**. libansnd holds **three** blocks, not two (it keeps a staging slot on the
driver side in addition to the one the DSP sees), and steady state made the
collision systematic — the overwritten block was re-read ~23 times per second.
Full mechanism in `STATUS.md` §11.6.

Fixed: 10 blocks, 4 reserved, invariant checked at compile time. `.dol` rebuilt.

## Trial 3 — **clean**

**No crackle left.** The ring really was the cause.

| track | kHz | voices | avg | max | dry | dsp | peak | NaN | verdict |
|---|---|---|---|---|---|---|---|---|---|
| V2M worst case | 48 | — | 1029 | 1232 | 7 | 32 | 15296 | 0 | over real time |
| V2M worst case | 32 | — | — | — | — | — | — | — | **refused: diverges below 44.1 kHz** |
| XM worst case | 48 | — | 12 | 20 | 0 | 31 | 25692 | 0 | ok |
| XM worst case | 32 | — | 8 | 12 | 0 | 58 | 25692 | 0 | ok |
| MIDI, no limit | 48 | inf/200 | 985 | 2031 | 30 | 31 | 32737 | 0 | over real time |
| MIDI, no limit | 32 | inf/205 | 542 | 1159 | 0 | 58 | 32698 | 0 | over real time |
| **MIDI, 64 voices** | **48** | **64/64** | **477** | **665** | **0** | 31 | 32737 | 0 | **ok — chosen** |
| MIDI, 64 voices | 32 | 64/64 | 284 | 410 | 0 | 58 | 32698 | 0 | ok |

**The figures are identical to trial 2's, to the per mille** (1029 against 1029,
985 against 984, `dry` 7 against 7). That is the most instructive result of the
run: fixing the ring changed **nothing** in the computation times. The crackle
was therefore not a delay or a load problem, it was **data corruption** — the
DSP was re-reading a buffer we had just rewritten. A defect that does not cost a
microsecond and is audible across the room.

Two guard rails verified on target: the V2M at 32 kHz refused with its reason,
and zero `NaN` over the eight phases.

The three `over real time` lines are **expected**: the V2M worst case has since
been dropped from the pack, and MIDI with no limit is not the production setting
— it is the raw measurement that justifies the ceiling.

## What campaign 2 decided — see also campaign 3, below

1. **libansnd is adopted.** No CPU gain — there was none to take — but the DSP
   load is halved at 48 kHz, the queue goes from ~128 to ~340 ms, and the
   underrun signal becomes explicit.
2. **One single file of the corpus is out of budget**, and it is set aside. The
   next is at 801 mean: no grey zone.
3. **The 64-voice MIDI ceiling is confirmed** by measurement, not extrapolated:
   477/665, zero dry passes.
4. **The CPU budget is closed.** What remains open is the interface.

---
---

# Campaign 3 — the interface

This campaign measures nothing. **It is listened to and it is handled.** The
logic is proved on a PC (`build-host\lib_test.exe`, 44 tests, including the
permutation over the real corpus); what remains can only be judged on a TV, with
a pad in hand.

Binary: `build-gc/gc-chiptune.dol` (also copied into `test/`). The **bench**
`.dol` has nothing to do here, it is the player we are trying.

## The only figure to record

**The first start-up time.** It displays itself:

```
  4937 tracks walked and indexed in XX.XX s
  index written: later start-ups will be immediate
```

That is the full walk of the card. On the second power-on the line must say
`read from the index` and be near-instant. **Both figures matter.**

## What to check, from the most serious to the most cosmetic

**1. It starts and it plays.** Everything else depends on that.

**2. Playback stops in the right place.** The 3-minute cap is gone; a 5-minute
track must now go all the way. Take a known long one and check it is not cut —
and that it moves on instead of getting stuck.

**3. Shuffle does not repeat.** A real check would need 4937 tracks; in practice,
enter a folder of ten or so tracks, `A` on a file (scope = that folder alone),
`Z` for shuffle, and check that one complete pass never gives the same one
twice. `B` must return to the track **actually played before**.

**4. The two scopes.** `A` on a file = that folder alone. `START` = the folder
**and everything under it**. From the root, `START` must give the 4937 tracks —
the counter at the top right says so.

**5. Resume.** Switch off mid-playback, switch on again: it must start again on
the same track, the same scope, the same modes, the same volume.

**6. The duration cache.** On the first pass, the browser's right-hand column is
empty. After playing a few tracks, come back to the browser: their durations
must be there. That is the fill-as-you-go.

**7. Pause, volume, jumps.** `X` pause, C-stick volume, `L`/`R` jump ∓10,
up/down cursor.

**8. Layout.** Text spilling from one pane into the other, names cut too short,
anything leaving the CRT's safe area (margins of 32 and 28 pixels).

## What can go wrong, and where to look

| symptom | lead |
|---|---|
| black screen at start-up | GX did not start; `ui_init()` puts a libogc console back as a fallback, the message must show there |
| very long start-up every time | the index is not being written — card full or read-only? |
| tracks scrolling past without playing | the cache points at files that no longer exist → `L+R` to rebuild it |
| shuffle repeats | the seed would not be sown; it is sown on the first button press |
| resume does not resume | `gc-chiptune.state` not written, or the track is no longer on the card |
| the picture stutters on a heavy track | expected: redrawing gives way to audio rendering (§12.10) |
| the sound hiccups while scrolling | the opposite of the previous one; see `AUDIO_BLOCKS_PER_PASS` in `main.c` |

## Trial 1 — start-up OK, then three defects

**The player starts, indexes the tracks in 4.5 s and plays.** *(5066 at the
time, 4937 since the `.v2m` cull.)* Point 1 is secured.

The three other observations and their full cause: `STATUS.md` §12.8.
In brief:

| observation | cause | fixed |
|---|---|---|
| `.mid` files do not start | **the card had no soundfont** | data, not code |
| one screen only, the list unreachable | the wait after a refusal did not poll the pad | yes |
| *(not yet observable)* end of track | `xmp_play_buffer(..., 0)` means **loop forever**, not "do not loop" | yes |

The third is the most serious and was not visible yet: no module in the corpus
stopped by itself, all of them were cut off by the watchdog.

---

## Preparing the card for trial 2

Two things to copy, plus the `.dol`. Everything is already built in
`build-host\release\`.

**1. The soundfont**, `build-host\release\soundfont.sf2`, **next to the `.dol`.**
Without it `.mid` files are skipped; modules play regardless.

It was embedded in the `.dol` for a while, then taken back out: read from the
card, its source block is freed after loading, which `.rodata` does not allow —
5.7 MB of RAM at stake (§12.17).

**2. The long-track test folder** — what point 2 asks for.

```
build-host\sdcard-test\TEST-LONG\
```

→ next to the label folders. Two tracks, one per backend, both over 5 minutes,
chosen by measuring **the whole corpus**:

| backend | track | duration | host cost |
|---|---|---|---|
| libxmp | `kZ - Moo0 ImageViewer SP 1.69 crk.xm` (2.7 MB) | **6:58** | ×862 |
| midi | `ORiGiN - Descent 3 installer.mid` (158 kB) | **6:53** | ×57 |

Both are comfortably within budget.

**3. Delete two files at the root of the music folder:**

```
gc-chiptune.idx     the index is taken on trust; without this TEST-LONG will not appear
gc-chiptune.state   otherwise the player resumes where it left off
```

*(`L+R` rebuilds the index without deleting — but while we are at it, deleting
both gives a clean first power-on, and a second scan figure.)*

---

## Trial 2 — the list at the top of this campaign, revisited

What changes from trial 1:

- **the interface is in GX, with the two panes side by side**: the list on the
  left, the player on the right, both permanently visible;
- **playback no longer stops when you navigate.** That is the change you hear:
  you can browse the whole card without cutting the sound;
- **modules stop when they finish**, which had never happened;
- a refused track shows its reason for **3 s** in the right pane before moving
  on.

Point 2 (end of track) is judged in `TEST-LONG`: both must go all the way, the
bar must reach the end, and they must **move on** to the next without getting
stuck.

Two more things to watch, specific to the move to GX:

- **start-up narrates itself on screen** (card mount, root found, number of
  tracks). If the screen stays black, GX did not start and a fallback console
  must take over with the message;
- **fluidity, and above all the sound while you handle it.** On a heavy track
  the interface is allowed to slow down — that is deliberate, sound comes first.
  But the sound must **never** hiccup while you scroll the list.

## Trial 2 — the GX interface, and five defects

Detail and mechanisms: `STATUS.md` §12.10 to §12.10 quater.

| observation | cause | fixed |
|---|---|---|
| START / up / down blur together | an invisible focus gave two meanings to every button | yes — focus removed, one button one role |
| decoding slows down while scrolling | TTF text was rasterised every frame, with no cache | yes — glyph atlas |
| "0 durations found" then a frozen screen | redraw-on-state-change stopped firing | yes — redraw every frame |
| the load line overflows bottom right | one line too long | yes — everything goes through `fx_draw_fit` |
| the right pane is empty below the bar | — | yes — stereo level meter |
| the `.v2m` files: refused, or a crash | **never explained** | moot: format dropped (§13) |

### The point that stayed open: the `.v2m` files

They were refused, or they crashed the application, and the cause was never
found. The question is closed another way: **the format has been dropped**
(campaign 4, `STATUS.md` §13). A `.v2m` is no longer listed at all.

---
---

# Campaign 5 — the bench, whole tracks

First measurement with the fixes: phases of 200 s instead of 12, the worst
one-second window recorded, the `dry` counter, the ARQ transfer counted in the
load, and the corpus's real MIDI worst case.

| track | kHz | voices | avg | worst | dry | dsp | verdict |
|---|---|---|---|---|---|---|---|
| XM worst case | 48 | — | 15 | 23 | 0 | 31 | ok |
| XM worst case | 32 | — | 10 | 14 | 0 | 58 | ok |
| MIDI worst case | 48 | inf/254 | 1346 | 2574 | **20820** | 32 | over real time |
| MIDI worst case | 32 | inf/248 | 813 | 1566 | **2807** | 59 | over real time |
| **MIDI worst case** | **48** | **64/64** | **593** | **675** | **0** | 31 | **ok — chosen** |
| MIDI worst case | 32 | 64/64 | 362 | 423 | 0 | 59 | ok |

**Three things this run establishes.**

**1. The production setting holds, measured over the whole track.** 64 voices at
48 kHz: 593 mean, **675 on the worst second**, zero dropouts — on the heaviest
track of the corpus, and no longer on the one we thought was. A factor of 1.5 of
margin. That is the chosen setting: **48 kHz everywhere, 64 voices.**

**2. The new counter works, and the old one really was lying.** On the phases
with no voice limit, `dry` records **20820** empty DSP cycles where the old one
(`VOICE_STATE_FINISHED`) saw only 631. And on the phases that pass, it shows
**0** — it does not cry wolf. That was exactly the defect that had let the V2M
through.

**3. The ARQ transfer costs almost nothing, and now we know it.** The XM goes
from 12/20 to 15/23 once the DMA is counted: **~3 per mille**. The claim
"libansnd costs no more CPU than ASND" finally has a measurement behind it,
instead of a convenient exclusion.

⚠️ Loads from before this campaign are no longer directly comparable: they
excluded the transfer.

---
---

# Campaign 4 — V2M endurance *(closed: the format is dropped)*

This campaign played six `.v2m` files **in full** on a console, instead of the
twelve seconds of opening that campaigns 1 and 2 measured. It is what showed the
V2M budget had never been tenable, and it concluded with dropping the format.

| track | mean | peak | time above real time |
|---|---|---|---|
| HoG Spec Ops *(the anchor)* | 1100 | 1291 | 85 % |
| Power Video Converter | 1050 | 1604 | 78 % |
| Audio Convertor Plus | **1236** | 1454 | 81 % |
| Hidetools Spy Monitor | **1208** | 1624 | 77 % |
| Stereoscopic Player | 1005 | 1494 | 55 % |
| Kyodai Mahjongg | 897 | 1651 | 48 % |

The anchor fell back to **1053 per mille over its first twelve seconds** against
1029 recorded in campaign 2: the measurement chain was fine, it was the
**window** that was too short.

Decision, options examined and full figures: `STATUS.md` §13. That
bench's binary was removed along with the rest of the V2M.

**What to take away for the rest**: twelve seconds of an opening do not describe
a track. Campaigns 1 and 2 still measure that way for the modules and MIDI — it
did not bite, which does not make it validated.

---

# Campaign 4 (evening) — two faults reported by ear

Two symptoms, reported after the release:

1. **no `.mid` plays** — "soundfont refused (memory)";
2. **many XMs and ITs crackle** and "do not play cleanly".

The first is diagnosed and fixed without a console (`STATUS.md` §14).
The second **is not settled**, and this campaign exists to settle it.

## What has already been ruled out, without a console

Full corpus re-run through `gcc_render -triage` at the production setting:
**3386 XMs and 254 ITs, zero refusals, 12 files that clip, 1163 samples in
total.** The peaks of the other 3628 sit between 0.15 and 0.94.

**Clipping is therefore ruled out** as the cause of a widespread crackle — it is
residual and absent from 99.7 % of the pack. The decoder renders cleanly on a
PC. See `STATUS.md` §15.1.

## The only figure to record: `dry`

The player pane now shows, under the volume:

```
load 12 / 20   dry 0
```

`dry` is `gcaudio_starved()`: **how many times the DSP asked for a block and the
ring was empty**, i.e. how many holes were punched in the sound. It existed
since §12.18 and was displayed nowhere — that is why the first two campaigns
could conclude "healthy load" on an output that crackled. The line turns to the
warning colour as soon as `dry` goes above zero.

**Record `dry` during a track that crackles, not after.** The counter is reset on
every track change (`gcaudio_stats_reset()` in `track_open`).

| `dry` observed | conclusion | next |
|---|---|---|
| **> 0** | the **output** is falling behind — `audio_gc.c`, the ARAM ring, or servicing too rarely in the GX loop | fix there. **libopenmpt would change nothing** |
| **0**, and it crackles | the **decoding** is at fault | libopenmpt (`STATUS.md` §15.4) |

That is the same fork as in §10.1 and §11.6, where the answer was "the output"
both times. Do not presume the result for all that.

## The counter-test that does not need the console

Note **3 or 4 names of files that crackle**, then, on the PC:

```
.\build-host\gcc_render.exe "<path to the file>" out.wav
```

and listen to `out.wav` in XMPlay or foobar. It is **exactly** the player's
code, same libxmp, same −6 dB headroom, only the output differs.

| the WAV | conclusion |
|---|---|
| **clean** | the defect is on the console side, downstream of decoding |
| **crackles or plays wrong too** | it is libxmp → libopenmpt |

Both measurements are independent and must agree. If `dry = 0` **and** the WAV
is clean, it is a third case — the conversion to ARAM or the DSP resampling —
and it will have to be looked for there.

## Also worth checking while the console is on

- **do the `.mid` files finally play?** The soundfont is now streamed from the
  card, never loaded whole (§14.2). On failure the message reports the **free
  memory**: record it.
- **the start-up line** "soundfont expected at: …" says where the player will
  look. It comes before the mount, so it is visible even if everything else
  fails.
- **`soundfont loaded: X -> Y kB free`** after the first `.mid`: the gap between
  the two numbers is the bank's real cost on that machine. It would be the first
  time it was measured rather than computed.

## Grid to fill in

| | recorded |
|---|---|
| free memory at start-up | |
| does a `.mid` play? | |
| `soundfont loaded: … -> … kB free` | |
| track that crackles (name) | |
| its `load avg / max` | |
| its **`dry`** | |
| does the WAV rendered on the PC crackle? | |

---
---

# Campaign 6 — interface responsiveness

Reported by ear: **"the left menu freezes and is extremely slow when a chiptune
is playing."**

Four causes were found and fixed without a console (`STATUS.md`
§19). This campaign checks them on hardware. It measures nothing new — it is
handled, like campaign 3.

## What to check, in this order

**1. The d-pad no longer does anything strange.** Left and right used to be
bound to two leftover diagnostics: left swapped the music for a 440 Hz sine,
right stopped drawing for eight seconds. They now leave and enter folders. If
either of those old behaviours reappears, the wrong `.dol` was loaded.

**2. Holding up or down scrolls.** In a folder of a hundred entries, holding a
direction must scroll continuously after a short delay, not move by one.

**3. The list still answers on a heavy track.** Play the MIDI worst case (64
voices, ~593 per mille) and scroll during it. It is allowed to be slower than on
a light module; it must not stop answering. Press and release a direction
quickly — the press must not be lost.

**4. The volume no longer stalls anything.** Hold the C-stick left or right for
several seconds. The volume must move smoothly and **the sound must not
hiccup**: nothing writes to the SD card from that path any more.

**5. The screen never blanks.** Even on the heaviest track, a frame must be
drawn at least four times a second. A screen that stops refreshing is the
symptom to report immediately.

**6. The end of a track is not clipped.** Take a track whose ending you know
well and check the last notes are there before the next one starts.

**7. `L+R` on a card you then remove.** Brutal, but it is the path that used to
dereference a freed pool. It must fail cleanly, showing an empty list, without
freezing.

## Grid to fill in

| | recorded |
|---|---|
| scrolling on a light module | |
| scrolling on the MIDI worst case | |
| `dry` while scrolling | |
| a quickly pressed button is registered? | |
| C-stick volume: any hiccup? | |
| end of track clipped? | |
| screen ever blank? | |

---
---

# Campaign 7 — the soundfont gives its memory back

Reported by ear: **"the soundfont and the `.mid` files never free from memory,
it saturates afterwards."**

Measured on the host and fixed without a console (`STATUS.md` §20):
nothing leaks, but the soundfont's 11.5 MB were taken on the first `.mid` and
never released. They are now released as soon as a non-MIDI track comes along
and fetched again on the next `.mid`.

**This campaign measures something the host cannot answer.** The host proved the
release returns everything and that the reload survives repetition. What it
cannot prove is that libogc's heap stays whole enough to hand back ~11 MB **in
one piece** after dozens of release/reload cycles — `tsf` keeps its samples as a
single float array. That is what `blk` is on screen for.

## What to check, in this order

**1. `blk` recovers after a `.mid`.** Note `free` and `blk` while a module
plays. Play one `.mid`, note both again — they should fall by about 11 500 kB.
Go back to a module and note them a third time. **Both must return to roughly
their first values.** If `free` comes back and `blk` does not, the hole has been
settled into by something else: report both numbers.

**2. The load says what it is.** Starting an isolated `.mid` shows "loading
soundfont, 5.7 MB from the card…" for a few seconds before the sound starts.
That wait is expected. A wait with **no message** is a different fault — report
it.

**3. Consecutive MIDIs load it once.** Play a folder of `.mid` straight through.
The message must appear on the **first** track only. If it appears on every
track, the release is firing when it should not.

**4. The long session, which is the whole point.** Shuffle the whole pack
(`START` at the root, `Z` for random) and leave it for an hour or more, so
several `.mid` come round. Then check that a large module still starts. **This
is the fault as it was reported: after a while, nothing starts any more.**

**5. MIDI still works at the end of that hour.** Once §20.6's risk is what it
is, the way it would show is "soundfont refused (load, N kB free)" on a `.mid`
that played earlier in the same session. Record `N` and `blk` if it happens.

**6. `L+R` after a `.mid`.** Rebuilding the index also releases the soundfont.
The walk must complete as fast as it does at start-up.

## Grid to fill in

| | recorded |
|---|---|
| `free` / `blk` on a module, before any `.mid` | |
| `free` / `blk` while the `.mid` plays | |
| `free` / `blk` back on a module | |
| load message shown, and for how long | |
| a folder of `.mid`: how many loads? | |
| after an hour of shuffle: does a large module start? | |
| after an hour of shuffle: `free` / `blk` | |
| any "soundfont refused"? with what numbers | |
