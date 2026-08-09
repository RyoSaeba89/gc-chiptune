# GC-Chiptune: installs devkitPPC + libogc2 on this PC.
#
# RUN AS ADMINISTRATOR. The devkitPro installer declares
# requestedExecutionLevel=requireAdministrator in its manifest, and pacman
# writes into C:\devkitPro: both steps require elevation.
#
#   1. Right-click PowerShell -> "Run as administrator"
#   2. cd <this repository>
#   3. .\tools\install-devkitpro.ps1
#
# The script is idempotent: it can be re-run safely.
#
# WATCH STEP 1: in the installer window, tick "GameCube Development". That group
# is what brings devkitPPC and gamecube-tools (including elf2dol).
#
# libogc2 is NOT in the official devkitPro repository: it is a maintained fork,
# distributed from its own pacman repository. The script adds it, placing it
# BEFORE [dkp-libs] so it takes priority -- which is what its documentation
# recommends as soon as you use more than the base packages.

$ErrorActionPreference = 'Continue'

$InstallerUrl = 'https://github.com/devkitPro/installer/releases/download/v3.0.3/devkitProUpdater-3.0.3.exe'
$KeyId        = 'C8A2759C315CFBC3429CC2E422B803BA8AA3D7CE'
$RepoStanza   = @'

[libogc2-devkitpro]
Server = https://packages.libogc2.org/devkitpro/windows/$arch
Server = https://packages.extremscorner.org/devkitpro/windows/$arch
'@

function Fail($msg) { Write-Host "FAILED: $msg" -ForegroundColor Red; exit 1 }
function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }

# --- privilege check --------------------------------------------------------
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail "this script must be run from an administrator PowerShell."
}

# --- 1. devkitPro ------------------------------------------------------------
Step "1/4  devkitPro (devkitPPC + gamecube-tools)"

$dkp = 'C:\devkitPro'
if (Test-Path "$dkp\devkitPPC\bin\powerpc-eabi-gcc.exe") {
    Write-Host "    already installed: $dkp\devkitPPC"
} else {
    $exe = Join-Path $env:TEMP 'devkitProUpdater-3.0.3.exe'
    if (-not (Test-Path $exe)) {
        Write-Host "    downloading the installer..."
        try { Invoke-WebRequest -Uri $InstallerUrl -OutFile $exe -UseBasicParsing -ErrorAction Stop }
        catch { Fail "download failed: $($_.Exception.Message)" }
    }
    Write-Host "    launching the installer."
    Write-Host "    >>> TICK 'GameCube Development' in the component list. <<<" -ForegroundColor Yellow
    Start-Process -FilePath $exe -Wait
    if (-not (Test-Path "$dkp\devkitPPC\bin\powerpc-eabi-gcc.exe")) {
        Fail "devkitPPC not found after installation. Was 'GameCube Development' actually ticked?"
    }
}

# --- 2. locate pacman --------------------------------------------------------
Step "2/4  locating pacman"

$pacman = $null
$conf   = $null
foreach ($base in @("$dkp\msys2", "$dkp\pacman")) {
    if (Test-Path "$base\usr\bin\pacman.exe") { $pacman = "$base\usr\bin\pacman.exe"; $conf = "$base\etc\pacman.conf" }
    elseif (Test-Path "$base\bin\pacman.exe") { $pacman = "$base\bin\pacman.exe";     $conf = "$base\etc\pacman.conf" }
    if ($pacman) { break }
}
if (-not $pacman) { Fail "pacman not found under $dkp. Incomplete installation?" }
if (-not (Test-Path $conf)) { Fail "pacman.conf not found ($conf)." }
Write-Host "    pacman      : $pacman"
Write-Host "    pacman.conf : $conf"

# --- 3. libogc2 repository ---------------------------------------------------
Step "3/4  adding the libogc2 repository"

$text = Get-Content $conf -Raw
if ($text -match '\[libogc2-devkitpro\]') {
    Write-Host "    repository already present."
} else {
    Copy-Item $conf "$conf.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
    # The repository must come BEFORE [dkp-libs] to take priority.
    if ($text -match '(?m)^\[dkp-libs\]') {
        $text = $text -replace '(?m)^\[dkp-libs\]', ($RepoStanza.Trim() + "`r`n`r`n[dkp-libs]")
    } else {
        $text = $text + "`r`n" + $RepoStanza
        Write-Host "    [dkp-libs] not found: repository appended at end of file." -ForegroundColor Yellow
    }
    Set-Content $conf $text -Encoding ascii
    Write-Host "    repository added (original file backed up)."
}

Write-Host "    importing the signing key..."
$pk = Join-Path (Split-Path $pacman) 'pacman-key'
if (Test-Path "$pk.exe") { $pk = "$pk.exe" }
if (-not (Test-Path $pk)) { Fail "pacman-key not found next to pacman ($pk)." }

# The keyring has to exist before any import. Idempotent.
& $pk --init 2>&1 | ForEach-Object { "      $_" }

# --recv-keys goes through gpg's keyserver protocol (dirmngr), which is
# frequently blocked -- that is what failed on the first attempt: the key
# arrived but stayed at "unknown trust", and pacman then refused the
# repository's database. So we import from a local copy of the public key,
# fetched over plain HTTPS from keyserver.ubuntu.com. --recv-keys is now only a
# fallback.
$keyFile = Join-Path $PSScriptRoot 'extremscorner-devkitpro.asc'
$added = $false
if (Test-Path $keyFile) {
    Write-Host "      importing from $keyFile"
    & $pk --add $keyFile 2>&1 | ForEach-Object { "      $_" }
    if ($LASTEXITCODE -eq 0) { $added = $true }
}
if (-not $added) {
    Write-Host "      falling back to the key server"
    & $pk --recv-keys $KeyId --keyserver keyserver.ubuntu.com 2>&1 | ForEach-Object { "      $_" }
}

# Local signature: without it the key stays "unknown trust" even once
# imported, and that is exactly the error message encountered.
& $pk --lsign-key $KeyId 2>&1 | ForEach-Object { "      $_" }
if ($LASTEXITCODE -ne 0) {
    Fail "key $KeyId could not be signed locally. Without that, pacman will refuse the libogc2 repository."
}

# --- 4. installing libogc2 ---------------------------------------------------
Step "4/4  installing libogc2"

& $pacman -Sy --noconfirm 2>&1 | ForEach-Object { "      $_" }
if ($LASTEXITCODE -ne 0) {
    Fail "pacman -Sy failed. If the message mentions a PGP signature, the key is not signed locally (step 3)."
}
& $pacman -S --noconfirm --needed libogc2 libogc2-examples 2>&1 | ForEach-Object { "      $_" }
if ($LASTEXITCODE -ne 0) { Fail "installing libogc2 failed." }

# --- verification ------------------------------------------------------------
Step "verification"

$ok = $true
foreach ($p in @("$dkp\devkitPPC\bin\powerpc-eabi-gcc.exe",
                 "$dkp\libogc2\include\ogc\audio.h",
                 "$dkp\tools\bin\elf2dol.exe")) {
    if (Test-Path $p) { Write-Host "    OK      $p" }
    else { Write-Host "    MISSING $p" -ForegroundColor Yellow; $ok = $false }
}

Write-Host ""
if ($ok) {
    Write-Host "Installation complete." -ForegroundColor Green
} else {
    Write-Host "Partial installation: see the missing items above." -ForegroundColor Yellow
    Write-Host "libogc2 may install elsewhere (e.g. $dkp\libogc). Check with:"
    Write-Host "  & '$pacman' -Ql libogc2 | Select-Object -First 20"
}
Write-Host ""
Write-Host "Variables to set in order to build:"
Write-Host "  `$env:DEVKITPRO = '$dkp'"
Write-Host "  `$env:DEVKITPPC = '$dkp\devkitPPC'"
