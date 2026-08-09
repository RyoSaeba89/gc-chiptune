# Extracts libxmp's source list from its own Makefiles.
#
# libxmp ships .c files that are NOT part of the build: in
# src/loaders/prowizard/Makefile, PROWIZ_OBJS2 (pm, pm01, pm20, pm40, pp30) is
# defined but never added to PROWIZARD_OBJS -- those are leftovers that no
# longer compile. Other .c files are #included rather than compiled
# (ice_unpack_fn.c). Blindly globbing *.c therefore fails.
#
# We read the Makefiles' OBJS variables so this stays correct as libxmp
# evolves.

function Get-MakeObjs {
    param([string]$Path, [string]$Var, [hashtable]$Vars = @{})

    if (-not (Test-Path $Path)) { return @() }
    $text = (Get-Content $Path -Raw) -replace '\\\r?\n', ' '   # fold continuations
    foreach ($line in $text -split "`n") {
        if ($line -match "^\s*$Var\s*[:+]?=\s*(.*)$") {
            $items = $matches[1].Trim() -split '\s+'
            $out = @()
            foreach ($it in $items) {
                if (-not $it) { continue }
                if ($it -match '^\$\((\w+)\)$') {
                    # reference to another variable of the same Makefile
                    $out += Get-MakeObjs -Path $Path -Var $matches[1] -Vars $Vars
                } elseif ($it -match '\.o$') {
                    $out += $it -replace '\.o$', '.c'
                }
            }
            return $out
        }
    }
    return @()
}

function Get-LibxmpSources {
    param([string]$LibxmpRoot, [switch]$NoDepackers, [switch]$NoProwizard)

    $srcs = @()
    Get-MakeObjs "$LibxmpRoot\src\Makefile" 'SRC_OBJS' |
        ForEach-Object { $srcs += "$LibxmpRoot\src\$_" }
    Get-MakeObjs "$LibxmpRoot\src\loaders\Makefile" 'LOADERS_OBJS' |
        ForEach-Object { $srcs += "$LibxmpRoot\src\loaders\$_" }
    if (-not $NoProwizard) {
        Get-MakeObjs "$LibxmpRoot\src\loaders\prowizard\Makefile" 'PROWIZ_OBJS' |
            ForEach-Object { $srcs += "$LibxmpRoot\src\loaders\prowizard\$_" }
    }
    if (-not $NoDepackers) {
        Get-MakeObjs "$LibxmpRoot\src\depackers\Makefile" 'DEPACKERS_OBJS' |
            ForEach-Object { $srcs += "$LibxmpRoot\src\depackers\$_" }
    }

    $missing = $srcs | Where-Object { -not (Test-Path $_) }
    if ($missing) {
        Write-Warning "libxmp sources not found:`n  $($missing -join "`n  ")"
    }
    return $srcs | Where-Object { Test-Path $_ }
}
