# GC-Chiptune: launches a .dol in Dolphin, grabs the screen, then stops.
#
# Dolphin has no command-line screenshot option: we go through GetWindowRect on
# the main window + CopyFromScreen (docs/TEST-DOLPHIN.md).
#
#   .\tools\dolphin-shot.ps1 -Dol build-gc\gc-chiptune.dol -Wait 20 -Out build-host\shot.png

param(
    [string]$Dol  = 'build-gc\gc-chiptune.dol',
    [int]   $Wait = 20,
    [string]$Out  = 'build-host\shot.png'
)

$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@

$p = Start-Process -FilePath 'Dolphin-x64\Dolphin.exe' `
                   -ArgumentList @('-b', '-e', $Dol) -PassThru

Start-Sleep -Seconds $Wait

$p.Refresh()
if ($p.HasExited) {
    Write-Host "Dolphin exited on its own (code $($p.ExitCode)) -- no screenshot."
    exit 1
}

[void][Win]::SetForegroundWindow($p.MainWindowHandle)
Start-Sleep -Milliseconds 500

$r = New-Object Win+RECT
if (-not [Win]::GetWindowRect($p.MainWindowHandle, [ref]$r)) {
    Write-Host 'GetWindowRect failed.'
    $p.Kill(); exit 1
}

$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g   = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save((Join-Path $root $Out), [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

$p.Kill()
$p.WaitForExit(5000) | Out-Null
Write-Host "Screenshot: $Out  ($w x $h)"
