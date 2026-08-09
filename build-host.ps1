# GC-Chiptune: builds the host (PC) tools.
#
# Does NOT build the .dol: these tools exist to validate the audio chain and to
# prepare files before copying them onto the SD card.
#
#   .\build-host.ps1            build everything
#   .\build-host.ps1 -Test      build, then replay the corpus validation

param([switch]$Test)

# Deliberately NOT 'Stop': under PowerShell 5.1, anything a native exe writes to
# stderr (here GCC's warnings) becomes a terminating error. We rely on the exit
# code instead.
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$out  = Join-Path $root 'build-host'
$lx   = Join-Path $root 'extern/libxmp'
$cflags = @('-O2','-Wall','-Wextra','-std=c99')
# LIBXMP_NO_DEPACKERS: libxmp's depackers (gzip/zip/lha/xz...) are excluded.
# They are useless here -- no packed module in the corpus -- they drag in lhasa,
# and above all they rely on temporary files and popen(), which make no sense on
# a GameCube. ProWizard is kept: no external dependency, and it catches the
# exotic MOD variants.
$cxxflags = @('-O2','-std=c++11','-DLIBXMP_STATIC','-DLIBXMP_NO_DEPACKERS',
              '-I',"$root/extern/tsf",'-I',"$lx/include")

if (-not (Test-Path $out)) { New-Item -ItemType Directory $out | Out-Null }
Set-Location $root

$failed = 0
function Build($desc, $script) {
    Write-Host "  $desc" -NoNewline
    $log = & $script 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAILED"
        $log | Select-String 'error|undefined reference' | Select-Object -First 10 |
            ForEach-Object { Write-Host "      $_" }
        $script:failed++
    } else {
        Write-Host "  ok"
    }
}

Write-Host "objects:"

# libxmp: source list taken from its own Makefiles, see the comment at the top
# of tools/libxmp-sources.ps1 (some shipped .c files are not part of the build).
if (-not (Test-Path "$out/libxmp.a")) {
    Write-Host "  libxmp.a      " -NoNewline
    . (Join-Path $root 'tools/libxmp-sources.ps1')
    $xsrc = Get-LibxmpSources -LibxmpRoot $lx -NoDepackers
    $xobj = Join-Path $out 'libxmp'
    if (-not (Test-Path $xobj)) { New-Item -ItemType Directory $xobj | Out-Null }
    Remove-Item "$xobj/*.o" -Force -ErrorAction SilentlyContinue
    Push-Location $xobj
    & gcc -c -O2 -DLIBXMP_STATIC -DLIBXMP_NO_DEPACKERS -I "$lx/include" -I "$lx/src" @xsrc 2>&1 | Out-Null
    $ok = $LASTEXITCODE -eq 0
    if ($ok) { & ar rcs "$out/libxmp.a" (Get-ChildItem -Filter '*.o' | ForEach-Object { $_.Name }) }
    Pop-Location
    if ($ok -and (Test-Path "$out/libxmp.a")) { Write-Host "  ok ($($xsrc.Count) sources)" }
    else { Write-Host "  FAILED"; $failed++ }
} else {
    Write-Host "  libxmp.a        already built"
}

Write-Host "binaries:"
Build 'sf2_prep.exe  ' { gcc @cflags -o "$out/sf2_prep.exe" tools/sf2_prep.c src/player/sf2_endian.c }
Build 'pl_scan_test.ex' { gcc @cflags -o "$out/pl_scan_test.exe" tools/pl_scan_test.c src/gc/playlist.c }
# scope_test: replays the A button on EVERY folder of the corpus, before and
# after a round trip through the index cache. It is what found why some folders
# answered "nothing playing" (docs/STATUS.md 12.12).
Build 'scope_test.exe' { gcc @cflags -o "$out/scope_test.exe" tools/scope_test.c src/gc/library.c src/gc/playlist.c src/gc/index.c }
# library.c depends on neither the screen, nor the audio, nor the card: the
# shuffle bag and the repeat modes are proved here, not with a gamepad.
Build 'lib_test.exe  ' { gcc @cflags -o "$out/lib_test.exe" tools/lib_test.c src/gc/library.c src/gc/playlist.c src/gc/index.c src/gc/state.c }
Build 'gcc_render.exe' { g++ @cxxflags -o "$out/gcc_render.exe" tools/gcc_render.cpp src/player/backend.cpp "$out/libxmp.a" }
# leak_test: replays the player's sequence in a loop and watches whether memory
# comes back. It is what cleared backend.cpp, libxmp and tsf on the "no song
# starts any more" fault (docs/STATUS.md 16.1) -- and therefore what made us
# look for fragmentation rather than a leak. -lpsapi for PrivateUsage.
Build 'leak_test.exe  ' { g++ @cxxflags -o "$out/leak_test.exe" tools/leak_test.cpp src/player/backend.cpp "$out/libxmp.a" -lpsapi }
# sf_cycle: the one thing leak_test structurally cannot see. It loads the
# soundfont once, BEFORE its reference measurement, so the 11.5 MB were part of
# its baseline -- and "the soundfont never frees" (docs/STATUS.md 20) went
# unmeasured for weeks. This one releases and reloads it every pass.
Build 'sf_cycle.exe  ' { g++ @cxxflags -o "$out/sf_cycle.exe" tools/sf_cycle.cpp src/player/backend.cpp "$out/libxmp.a" -lpsapi }
# smp_depth: which modules have 16-bit samples. It is what quantified the extent
# of the WORDS_BIGENDIAN fault (docs/STATUS.md 18.2) -- 1005 files out of 4826 --
# and showed that the bench's worst case, data/worst.xm, is 8-bit and could
# therefore detect nothing.
Build 'smp_depth.exe  ' { gcc @cflags -DLIBXMP_STATIC -I "$lx/include" -o "$out/smp_depth.exe" tools/smp_depth.c "$out/libxmp.a" }

Write-Host ""
if ($failed) { Write-Host "$failed target(s) failed." ; exit 1 }
Write-Host "Tools in build-host\ :"
Write-Host "  sf2_prep    soundfont -> big-endian for the console (-check to inspect)"
Write-Host "  pl_scan_test  the player's folder walk, replayed on a PC"
Write-Host "  scope_test  scope of every folder, index cache included"
Write-Host "  lib_test    scope, shuffle bag, index cache and resume (arg = a corpus)"
Write-Host "  gcc_render  both backends (modules / midi)"
Write-Host "  leak_test   does memory come back? (list on stdin, -n passes)"
Write-Host "  sf_cycle    does the SOUNDFONT come back? (-sf f.sf2 -mid a.mid -mod b.xm)"
Write-Host "  smp_depth   modules with 16-bit samples (list on stdin, -v)"
Write-Host ""
Write-Host "Example: run the whole corpus through"
Write-Host '  Get-ChildItem Chiptunes -Recurse -Include *.xm,*.mod,*.it,*.s3m,*.mid |'
Write-Host '    %{ $_.FullName } | .\build-host\gcc_render.exe -batch -sf extern\soundfont\TimGM6mb.sf2'
