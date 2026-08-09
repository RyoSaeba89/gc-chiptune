# GC-Chiptune: assembles the final release.
#
#   .\tools\make-release.ps1
#
# Produces build-host\release\: the .dol, the prepared soundfont, and enough
# information to know where to put them. Compiles nothing -- run first:
#
#   bash -lc "make"          (the .dol)
#   .\build-host.ps1         (sf2_prep)
#
# Why a script rather than a copy-paste: the soundfont has to be CONVERTED to
# big-endian (tsf.h reads the RIFF container by raw copy), and that is the kind
# of step one forgets every other time.

param(
    [string]$Soundfont = 'extern\soundfont\TimGM6mb.sf2',
    [string]$Out       = 'build-host\release'
)

$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$dol = Join-Path $root 'build-gc\gc-chiptune.dol'
if (-not (Test-Path $dol)) {
    Write-Host "FAILED: $dol missing. Run first: bash -lc `"make`""
    exit 1
}

if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory $Out -Force | Out-Null

Copy-Item $dol (Join-Path $Out 'gc-chiptune.dol')
Write-Host ("  gc-chiptune.dol  {0:N1} MB" -f ((Get-Item $dol).Length / 1MB))

# --- the soundfont, converted ------------------------------------------------
$sf2prep = Join-Path $root 'build-host\sf2_prep.exe'
if (-not (Test-Path $sf2prep)) {
    Write-Host "FAILED: $sf2prep missing. Run .\build-host.ps1"
    exit 1
}
if (-not (Test-Path $Soundfont)) {
    Write-Host "FAILED: soundfont not found: $Soundfont"
    exit 1
}
$log = & $sf2prep $Soundfont (Join-Path $Out 'soundfont.sf2')
if ($LASTEXITCODE -ne 0) {
    $log | ForEach-Object { Write-Host "    $_" }
    Write-Host "Converting the soundfont FAILED."
    exit 1
}
Write-Host ("  soundfont.sf2    {0:N1} MB (big-endian)" -f `
            ((Get-Item (Join-Path $Out 'soundfont.sf2')).Length / 1MB))

# --- the soundfont's licence travels with it ---------------------------------
# TimGM6mb is GPL-2.0: redistributing it requires providing its licence and its
# source. It sits NEXT TO the .dol, not inside it -- the binary stays MIT.
Copy-Item $Soundfont (Join-Path $Out 'soundfont-source-TimGM6mb.sf2')

@'
GC-Chiptune
===========

Chiptune music player for the GameCube. Modules (MOD/XM/IT/S3M and ~90 other
formats) and MIDI, read from an SD card.


WHAT TO COPY
------------

  gc-chiptune.dol    anywhere you like; load it with Swiss, SD Media Launcher,
                     GC Loader, a modchip -- it does not matter.

  soundfont.sf2      NEXT TO THE .dol. Without it, .mid files are skipped;
                     modules play regardless.

  your music         in one of these folders, at the root of the card:
                       sd:/chiptunes        (looked for first)
                       sd:/GC-Chiptune
                       sd:/music
                       sd:/                 (the whole card)
                     Sub-folders allowed, as many as you like.

Modules and MIDI files are copied AS THEY ARE. Nothing to convert.


THE BUTTONS
-----------

  up/down      list cursor (hold to scroll)   X      pause
  left/right   leave / enter a folder         Y      repeat mode
  L / R        jump ten rows                  Z      shuffle
  A            enter / play                   START  play the WHOLE tree
  B            up one level                   L+R    rebuild the index
  C-stick left/right: volume
  C-stick up/down   : previous / next track

A on a file plays the current folder alone. START plays the targeted folder AND
everything under it -- from the root, that is the whole card.

One button = one role, always the same. There is no mode.


TWO FILES WRITTEN TO THE CARD
-----------------------------

  gc-chiptune.idx     the index of tracks and their durations. Without it the
                      first start-up walks the card again (4.5 s for 5000
                      tracks); with it, start-up is immediate. After adding or
                      removing tracks: L+R to rebuild it.

  gc-chiptune.state   where you were. Powering back on resumes the track, the
                      scope, the modes and the volume.

Both sit next to your music. Deleting them breaks nothing.


LICENCES
--------

  gc-chiptune.dol    MIT
  soundfont.sf2      GPL-2.0 -- TimGM6mb, by Tim Brechbill.

The soundfont is a separate file, not a part of the binary: each keeps its own
licence. GPL-2.0 requires its source to accompany redistribution, hence
soundfont-source-TimGM6mb.sf2 -- the unconverted original, of which
soundfont.sf2 is only the big-endian version.

For a fully permissive binary AND data, replace TimGM6mb with MuseScore's
MS Basic (MIT) and run it through tools/sf2_prep.
'@ | Set-Content (Join-Path $Out 'README.txt') -Encoding UTF8

Write-Host "  README.txt"
Write-Host "  soundfont-source-TimGM6mb.sf2  (GPL-2.0 requirement)"
Write-Host ''
Write-Host "Release in $Out"
