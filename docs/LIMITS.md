# GC-Chiptune — known limits

What the player does **not** do, and why. Every number here was measured on a
real GameCube, libansnd output, unless stated otherwise.

For the decisions and the method: `STATUS.md`. To redo the
measurements: [TEST-HARDWARE.md](TEST-HARDWARE.md).

---

## 1. Formats read

**Two backends: modules (libxmp) and MIDI.** The `.v2m` existed and was dropped
— the V2 synth does not hold real time on the Gekko (STATUS §13). A `.v2m`
placed on the card is no longer listed.

**The strength criterion is the mean, not the maximum.** A maximum above 1000
per mille on a low mean is a spike, and the ~340 ms of queue absorb it. A mean
above 1000 is a permanent deficit: no buffer catches that up. That is what
condemned the V2M, and it is the criterion to apply if a third backend is ever
added.

## 2. MIDI

**Unplayable with no polyphony ceiling.** The worst case asks for up to 205
simultaneous voices and climbs to **2031 per mille** — twice real time. Those
are stacked release tails, not notes. With the ceiling: **477 mean, 665
maximum**, zero dry passes.

**The 64-voice ceiling bites.** On the worst case the measurement shows `64/64`:
the limit is saturated permanently, so voices are being stolen continuously. tsf
sacrifices the ones furthest into their release first, which costs reverberation
rather than notes — but the degradation is real on dense passages.

⚠️ **The bench does not measure the corpus's worst MIDI.** It embeds
`BACKLASH - Fighting Force` (host ×39.6), but the heaviest of the pack is
`PHROZEN CREW - 32 Card Bridge for Windows 1.6 nfo.mid` (host ×33.7), i.e. 1.17×
more expensive. Projected: ~560 mean, comfortably within budget — but the
reference track has been badly chosen from the start. To fix before the next
campaign.

**The soundfont costs 11.5 MB of RAM out of 24.** TinySoundFont converts the s16
samples to float at load time, i.e. double the file's 5.7 MB. Measured on the
host: `tsf_load_filename(TimGM6mb)` commits 11832 kB.

It is **the program's largest single item** and there is no upstream option:
`fontSamples` is a `float*` in this version of `tsf.h`. The only two known
levers are a smaller bank, or a local copy of `tsf.h` keeping the samples as s16
(−5.5 MB). See STATUS §14.4.

**It is loaded on the first `.mid` and released on the next non-MIDI track.**
For months it was loaded and never released, on a pack that is 97.8 % modules —
the "memory saturates" fault. `tsf_close()` returns all of it (measured: five
cycles come back to within 48–124 kB of baseline). The cost of the release is a
reload for every *isolated* `.mid`: several seconds of card read, announced on
screen. Consecutive MIDIs pay nothing. See STATUS §20.

**The soundfont is streamed, never loaded whole.** It used to be, and the
resulting 16.7 MB peak — 5.7 MB of file plus 11.0 MB of samples, simultaneously
— made the load fail on an arena of ~21.5 MB already eaten into, with the
misleading message "soundfont refused (memory)". Since `tsf_load()` only reads
forwards, the file has no reason to be in memory. See STATUS §14.

⚠️ **Fixed but never played on a console.** Dolphin emulates neither the SD card
nor libansnd's DSP microcode: no `.mid` from this player has yet been heard on a
GameCube.

**TimGM6mb is GPL v2** — usable for personal testing, to be replaced with a
permissive bank before any distribution of the `.dol`. The replacement will have
to go through `sf2_prep` as well.

**Upstream behaviour difference**: as soon as `maxVoiceNum` is set, tsf runs
`tsf_voice_envelope_nextsegment` twice in `voice_end`, which restarts the
release. No measurable consequence here, but it is not a no-op.

## 3. Modules

**By far the most comfortable backend**: **12 mean, 20 maximum**, faster on real
silicon than under Dolphin. The heaviest of the pack's 4827 modules projects to
**4 per mille**. No practical limit — it would take a module 250 times more
expensive than the worst known to cause a problem.

**10 files of the pack are refused** and 1 is silent — genuinely damaged
modules, not a missing component. Set aside, detail in `docs/pack-dropped.txt`.

**Packed modules are not read.** libxmp's depackers are excluded
(`LIBXMP_NO_DEPACKERS`): they rely on temporary files and `popen()`, meaningless
on a GameCube. No refusal in the corpus is due to that exclusion.

**Residual clipping is measured and negligible.** The full corpus re-run at the
production setting (`XMP_PLAYER_VOLUME` 50, i.e. −6 dB):

| format | files | that clip | samples |
|---|---|---|---|
| xm | 3386 | 7 (0.2 %) | 207 |
| it | 254 | 5 (2.0 %) | 956 |

ITs clip ten times more often than XMs, but 1163 samples over 3640 files remain
inaudible. **A crackle heard on many files is therefore not clipping** — see
STATUS §15.

**libxmp is not libopenmpt.** libxmp is correct on very nearly the whole pack,
but libopenmpt is OpenMPT's engine and stays more faithful on the edge effects
of XMs and especially ITs (NNA/DNA, resonant filters, volume ramps). No case of
wrong rendering has been **isolated** here. If a track plays wrong — notes or
effects, not hiss — that is the remedy, and the CPU does not object (12 per
mille today). The cost is ~2 MB of resident `.dol` and a build from source:
**there is no `ppc-libopenmpt` package at devkitPro.** See STATUS §15.4.

## 4. File preparation

**The soundfont cannot be copied as it is.** The Gekko is big-endian; the `.sf2`
is little-endian and TinySoundFont reads it by raw pointer copy.
`tools/make-sdcard.ps1` does the conversion. Modules and MIDI files, on the
other hand, are copied as they are.

**A prepared soundfont is no longer a standard file.** An `.sf2` run through
`sf2_prep` is no longer readable by anything else. That is accepted, but it must
not be confused with the original.

The player detects the case and says so ("soundfont not prepared") instead of
failing with no explanation.

## 5. Platform

**SD carrier: only slot A is proven.** SD Gecko slot B and SD2SP2 are coded and
tried in order, never validated on hardware.

**The AI only offers 32 and 48 kHz.** No 44.1. Everything is rendered at 48.

**The DSP is on the critical path.** Mixing, resampling and now the *reading* of
the samples from ARAM are handed to it (STATUS §11). Measured on libansnd:
**31 per mille at 48 kHz, 58 at 32 kHz**. The asymmetry is normal — the output
is pinned at 48 kHz, so a voice at 48000 Hz is 1:1 while a voice at 32000 Hz has
to be raised by a factor of 1.5. Wide margin in both cases (upstream advertises
18 simultaneous resampled voices; we use **one**).

**Dolphin can no longer serve at all for audio.** libansnd uploads its own DSP
microcode, which the emulator's HLE audio cannot execute. Every measurement or
listen goes through the console.

**The GX interface has never been validated on a console.** Both panes, the
glyph atlas and the level meter are written; the logic is proved on a PC
(`tools/lib_test`, `tools/scope_test`) — but legibility, layout and
responsiveness can only be judged on a TV. See STATUS §12.

**No format filter and no search.** You navigate by folder; there is no way to
say "only the .mid" nor to search for a title among 4937.

**The index cache is taken on trust.** It is not revalidated at start-up — that
would be redoing the walk it avoids. After adding or removing tracks on the
card, it has to be rebuilt (`L+R` in the browser).

## 6. What the measurements do not cover

This section exists because two faults lived for weeks behind healthy-looking
numbers.

**A correct counter you do not display is no better than a counter that lies.**
`gcaudio_starved()` has measured exactly the condition you can hear — the DSP
leaving empty-handed — since STATUS §12.18. It was displayed **nowhere**: the
player pane showed the load, healthy, while the console crackled. Fourth
occurrence of the same trap in this project, and the only one where the number
already existed. It is now on screen, next to the load, and warns as soon as it
goes above zero.

**The bench only measures worst cases**, one per backend. It does not sweep the
corpus on a console: that would be hours of handling.

**The corpus ranking is a projection.** Host timings related to **one single**
console measurement per backend (midi: Fighting Force 477; module: worst.xm 12).
Good for sorting, not for deciding within 50 per mille. The MIDI projection is
the most fragile: the 64-voice ceiling is not a constant factor.

**The CPU load counter now spans the DMA transfer.** It used to time only
`gcc_render()`, with the ARAM transfer explicitly excluded — so "libansnd costs
no more CPU than ASND" was only demonstrated for the render part. The transfer
is now inside the stopwatch and measures ~3 per mille.

**The polyphony peak is sampled from the main loop**: very brief spikes between
two passes are missed.

**Dolphin cannot reproduce** cache effects, audio DMA timing, the SD mount — nor
the class of bug that made the output crackle. Whatever passes under emulation
proves nothing on those points.

**A load verdict is only worth anything if `peak > 0` and `NaN = 0`.** The bench
and the results file now say so on every run.

## 7. Memory

**There is no leak.** Measured, not assumed: `tools/leak_test` replays the
player's sequence in a loop and records the committed bytes. 300 cycles over the
pack's largest modules — non-monotonic variation between −292 and +1136 kB; 240
MIDI cycles with the soundfont loaded — a plateau at +48 kB from the third pass.
See STATUS §16.1.

⚠️ **That measurement is blind to anything held across its own baseline.**
`leak_test` loads the soundfont once, before its reference pass, so 11.5 MB
that were never released looked exactly like 11.5 MB that were never taken.
`tools/sf_cycle` releases and reloads it every pass; that is where the fault of
STATUS §20 finally showed.

**The constraint is fragmentation, not the total.** A file is read in one piece.
libogc's allocator returns nothing to the system, so a holed heap can announce
ten megabytes free and refuse a 2.7 MB module. The player now allocates **one
single track buffer, which never shrinks** (STATUS §16.2), and **releases the
soundfont as soon as a non-MIDI track comes along** (STATUS §20). Both large
blocks are therefore accounted for; what is left unproven is whether libogc's
heap stays whole enough to hand ~11 MB back **in one piece** after dozens of
release/reload cycles. That is what `blk` is on screen for.

**Two numbers on screen, and it is the second that decides**: `ram` (free total)
and `blk` (largest contiguous block). The line warns below 3 MB. A comfortable
total proves nothing.

**The pack's largest module is 2.7 MB.** That is the bound `blk` has to stay
above for the whole library to remain playable.

⚠️ **None of this has been observed over a long console session.** The single
buffer removes *one* source of fragmentation; if another remains, `blk` is what
will say so.

## 8. Cross-compilation: the byte-order macros

**The Gekko is big-endian, and two libraries out of three do not notice by
themselves.**

| library | what has to be done | what happens otherwise |
|---|---|---|
| **libxmp** | `-DWORDS_BIGENDIAN=1` in CFLAGS | the **16-bit** samples of XM/IT play byte-swapped — full-scale noise on 21 % of the pack (STATUS §18) |
| **TinySoundFont** | soundfont converted offline by `tools/sf2_prep` | the `.sf2` is unreadable, MIDI does not load (STATUS §7.5) |
| libansnd, libogc2, GRRLIB | nothing | — |

Both faults have the same shape and together cost close to a month: a
third-party library reads little-endian data and only corrects it if you say so
at compile time.

⚠️ **Neither of those two faults can appear on a PC.** On x86 the `#else`
branches are the right ones. `tools/gcc_render` validated 3640 modules without
seeing a thing. **A host validation says nothing about byte order** — it is the
one class of fault for which the PC bench has no value at all.

The counter-test that does work is run on a PC: forcing the macro **the wrong
way round** on the host reproduces the console fault exactly. That is how the
extent described in §18.3 was quantified without switching the GameCube on.

⚠️ **The bench's worst case is an 8-bit module** (`data/worst.xm`, checked with
`tools/smp_depth`). The bench is therefore structurally blind to that class of
fault, and "the bench is clean" proved nothing. To be replaced with a 16-bit
module.

## 9. Interface responsiveness

**The main loop is bounded, not preemptive.** There is one thread: it services
the audio, reads the pad and draws. Audio rendering is capped at
`AUDIO_BLOCKS_PER_PASS` blocks per pass and the pad is read between two of them
(`src/gc/main.c`), so a heavy track slows the interface down but can no longer
freeze it. A decoder that is permanently slower than real time will still crackle
— nothing in the loop can fix that.

**The frame can be skipped when the audio ring runs short**, but never for
longer than 250 ms: a screen that stops refreshing reads as a crash, and a
previous version had exactly that fault.

**Nothing writes to the SD card from an input handler.** The resume file is
marked dirty and flushed at most once every few seconds, and only while the ring
is full. An SD write blocks the loop for tens of milliseconds; doing one on
every volume step starved the audio and stalled the list.
