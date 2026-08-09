# Testing the .dol in Dolphin

A reproducible procedure, with the traps that were hit.

## Building

From the root of the repository, `$DK` being the devkitPro folder:

```sh
# Once: extern/ + all the libraries (see INSTALLATION.md)
"$DK/msys2/usr/bin/bash.exe" -l tools/bootstrap.sh

# Then, on every change:
"$DK/msys2/usr/bin/bash.exe" -lc "cd \"$PWD\" && make"

# The CPU measurement bench, to redo the figures of §7:
"$DK/msys2/usr/bin/bash.exe" -lc "cd \"$PWD\" && make APP=bench"
```

The shell must be launched with `-l` so `/etc/profile.d/devkit-env.sh` sets
`DEVKITPRO` and `DEVKITPPC`. In that MSYS2, the devkitPro folder is mounted on
`/opt/devkitpro` and the Windows user folder on `/home` — hence the Unix-shaped
paths in the build traces.

## Configuring Dolphin (once)

`Dolphin-x64/portable.txt` (an empty file) forces Dolphin to keep its
configuration in `Dolphin-x64/User/`, without touching the user profile.

`Dolphin-x64/User/Config/Dolphin.ini`:

```ini
[Display]
RenderToMain = True
Fullscreen = False
[DSP]
DumpAudio = True
DumpAudioSilent = True
[Interface]
ConfirmStop = False
UsePanicHandlers = False
```

`DumpAudio` is the important one: it gives an **objective check** that sound is
really being produced, instead of trusting the screen.

## Running

```powershell
Dolphin-x64\Dolphin.exe -b -e build-gc\gc-chiptune.dol
```

`-b` = batch mode (quits when emulation ends), `-e` = execute.

## Collecting results

**Screenshot.** Dolphin has no command-line screenshot option. Go through
`GetWindowRect` + `Graphics.CopyFromScreen` on `$p.MainWindowHandle`.

**Audio dump.** In `Dolphin-x64/User/Dump/Audio/`:

| file | expected content |
|---|---|
| `*_dspdump1.wav` | **the player's stream** — 48042 Hz, not silent |
| `*_dspdump.wav` | silent (we bypass the DSP mixer) |
| `*_dtkdump.wav` | silent (DTK unused) |

⚠️ The WAV header Dolphin writes announces a wrong duration (a placeholder).
Trust the real file size, not the frame counter.

**Automated capture.** `tools/dolphin-shot.ps1` does the whole round: launches
the `.dol`, waits, grabs, stops Dolphin.

```powershell
.\tools\dolphin-shot.ps1 -Dol build-gc\gc-chiptune.dol -Wait 20 -Out build-host\shot.png
```

## Traps

**Dolphin does NOT emulate the SD Gecko or the SD2SP2.** Checked on Dolphin
2606: the GameCube slots only offer Memory Card, GCI Folder, Advance Game Port,
Microphone, Broadband Adapter, Skylanders, Infinity — no SD card; only the Wii
has one. The player therefore shows "no SD card" under Dolphin, **and that is
the expected behaviour**. The mount is only validated on real hardware. What
remains testable here: the folder walk, through the host bench
`tools/pl_scan_test.c` (see `STATUS.md` §8.4).

**Keyboard input does not reach emulation reliably in batch mode.** `SendKeys`
to the Dolphin window does not get through. That is why the bench
(`src/gc/bench_main.c`) is **automatic**: it runs the matrix (track × frequency)
on its own with no interaction. If manual navigation becomes necessary, Dolphin
writes its own `GCPadNew.ini` on first launch, with A = `X`, B = `Z`,
START = `RETURN`.

**libogc's console does not clear to end of line** on redraw. Always format
labels at fixed width (`%-20s`), or the tail of the previous text remains — an
"ok" written over "<== running" gave "ok= running".

**An instantaneous load proves nothing.** It fluctuates widely with the density
of the track: 801 then 1014 per mille recorded on the same file. Always reason
about the cumulative max and mean (`gcaudio_load_max` / `gcaudio_load_avg`).

**Dolphin is not cycle-accurate and does not emulate the caches.** The figures
obtained here are optimistic, see `STATUS.md` §7.3.
