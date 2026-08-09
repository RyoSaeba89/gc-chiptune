# GC-Chiptune: prepares the tree to copy onto the SD card.
#
#   .\tools\make-sdcard.ps1                            the whole reference pack
#   .\tools\make-sdcard.ps1 -Source 'Chiptunes\Sub'    a subset
#
# Produces build-host\sdcard\chiptunes\... , to be copied as-is to the root of
# the card (the player looks for sd:/chiptunes first, see src/gc/main.c).
#
# Two reasons to have a tool rather than a plain copy-paste:
#
#  1. The reference pack contains 455 files no backend reads (sc68, sid, ahx,
#     mp3...). Taking them along would only slow down the start-up walk.
#  2. The soundfont has to land under a precise name for the MIDI backend to
#     find it: chiptunes\soundfont.sf2.

param(
    [string]$Source    = 'KEYGENMUSiC MusicPack',
    [string]$Out       = 'build-host\sdcard',
    [string]$Soundfont = 'extern\soundfont\TimGM6mb.sf2',
    [switch]$NoSoundfont
)

$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

if (-not (Test-Path $Source)) {
    Write-Host "FAILED: source not found: $Source"
    exit 1
}

# Must stay aligned with g_exts in src/gc/playlist.c.
$exts = @('mid','midi','rmi',
          'mod','xm','it','s3m','mtm','stm','669','far','okt','ptm',
          'mdl','dbm','amf','gdm','imf','liq','ult','umx','med','dtm',
          'psm','rtm','stx','wow','digi','dmf','abk','emod','fnk','mgt')

$dest = Join-Path $root (Join-Path $Out 'chiptunes')
if (Test-Path $dest) {
    Write-Host "Cleaning $dest ..."
    Remove-Item $dest -Recurse -Force
}
New-Item -ItemType Directory $dest -Force | Out-Null

$srcFull = (Resolve-Path $Source).Path
Write-Host "Source: $srcFull"
Write-Host "Target: $dest"
Write-Host ''

$files = Get-ChildItem $srcFull -Recurse -File |
         Where-Object { $exts -contains $_.Extension.TrimStart('.').ToLower() }

Write-Host "$($files.Count) files selected. Copying..."

$copied  = 0

foreach ($f in $files) {
    $rel = $f.FullName.Substring($srcFull.Length).TrimStart('\')
    $tgt = Join-Path $dest $rel
    $dir = Split-Path $tgt -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory $dir -Force | Out-Null }

    Copy-Item $f.FullName $tgt -Force
    $copied++
}

Write-Host "  $copied files copied as-is"

if (-not $NoSoundfont) {
    if (Test-Path $Soundfont) {
        # The soundfont MUST be converted, not copied: tsf.h reads the RIFF
        # container by raw copy, i.e. little-endian (docs/STATUS.md 7.5). The
        # player detects a raw soundfont and says so, but it will not play it.
        $sf2prep = Join-Path $root 'build-host\sf2_prep.exe'
        if (-not (Test-Path $sf2prep)) {
            Write-Host "  FAILED: $sf2prep missing. Run .\build-host.ps1"
            exit 1
        }
        Write-Host "  soundfont: converting to big-endian..."
        $log = & $sf2prep $Soundfont (Join-Path $dest 'soundfont.sf2')
        if ($LASTEXITCODE -ne 0) {
            $log | ForEach-Object { Write-Host "    $_" }
            Write-Host "  conversion FAILED: .mid files will be skipped"
        } else {
            $log | Select-Object -Last 3 | ForEach-Object { Write-Host "    $_" }
        }
        # Licence reminder: TimGM6mb is GPL v2 (docs/STATUS.md 3.3). On a
        # personal card that has no consequence; for a distribution of the .dol,
        # replace it with a permissive one.
    } else {
        Write-Host "  soundfont missing ($Soundfont): .mid files will be skipped"
    }
}

$sum = (Get-ChildItem $dest -Recurse -File | Measure-Object Length -Sum)
Write-Host ''
Write-Host ("Done: {0} files, {1:N1} MB in {2}" -f $sum.Count, ($sum.Sum / 1MB), $dest)
Write-Host "Copy the 'chiptunes' folder to the root of the SD card (FAT32)."
