# SPDX-License-Identifier: BSD-3-Clause
<#
.SYNOPSIS
    Stage a relocatable PolyMesh tree for Windows, the PE counterpart of
    scripts/bundle_linux.sh.

.DESCRIPTION
    Copies bin/ and share/ out of a CMake install prefix, then walks the import
    table of every staged executable and copies each non-system DLL it needs
    next to it, transitively. Windows has no RUNPATH: the loader searches the
    directory of the running executable first, so an app-local DLL set is the
    relocatable form here.

    The import table is read directly from the PE headers instead of shelling
    out to dumpbin, which only exists inside a Developer Command Prompt.

    Libraries that ship with Windows -- and the Visual C++ runtime, which is a
    separately installed redistributable, not ours to relicense -- are left to
    the host and named in the bundle README.

.PARAMETER Prefix
    CMake install prefix (contains bin\ and share\).

.PARAMETER Stage
    Output directory. Removed and recreated.

.PARAMETER SearchPath
    Extra directories to resolve DLLs from, e.g. the vcpkg installed bin
    directory and the CMake build output directory.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [Parameter(Mandatory = $true)][string]$Stage,
    [string[]]$SearchPath = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-AsciiString {
    param([byte[]]$Bytes, [int]$Offset)
    $end = $Offset
    while ($end -lt $Bytes.Length -and $Bytes[$end] -ne 0) { $end++ }
    return [System.Text.Encoding]::ASCII.GetString($Bytes, $Offset, $end - $Offset)
}

function Convert-RvaToOffset {
    param([object[]]$Sections, [uint32]$Rva)
    foreach ($section in $Sections) {
        $size = [Math]::Max($section.VirtualSize, $section.RawSize)
        if ($Rva -ge $section.VirtualAddress -and $Rva -lt ($section.VirtualAddress + $size)) {
            return [int]($Rva - $section.VirtualAddress + $section.RawOffset)
        }
    }
    return -1
}

function Get-PeImport {
    <# Imported DLL names of a PE image, including delay-loaded ones. #>
    param([string]$Path)

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "bundle_windows: $Path is not a PE image"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ([BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
        throw "bundle_windows: $Path has no PE signature"
    }

    $coff = $peOffset + 4
    $sectionCount = [BitConverter]::ToUInt16($bytes, $coff + 2)
    $optionalSize = [BitConverter]::ToUInt16($bytes, $coff + 16)
    $optional = $coff + 20
    $magic = [BitConverter]::ToUInt16($bytes, $optional)
    # PE32+ has eight extra bytes of 64-bit fields before the data directories.
    $directories = if ($magic -eq 0x20B) { $optional + 112 } else { $optional + 96 }

    $sections = @()
    for ($i = 0; $i -lt $sectionCount; $i++) {
        $header = $optional + $optionalSize + ($i * 40)
        $sections += [pscustomobject]@{
            VirtualSize    = [BitConverter]::ToUInt32($bytes, $header + 8)
            VirtualAddress = [BitConverter]::ToUInt32($bytes, $header + 12)
            RawSize        = [BitConverter]::ToUInt32($bytes, $header + 16)
            RawOffset      = [BitConverter]::ToUInt32($bytes, $header + 20)
        }
    }

    $names = New-Object System.Collections.Generic.List[string]

    # Data directory 1: import table, 20-byte descriptors, DLL name RVA at +12.
    $importRva = [BitConverter]::ToUInt32($bytes, $directories + 8)
    if ($importRva -ne 0) {
        $cursor = Convert-RvaToOffset -Sections $sections -Rva $importRva
        while ($cursor -ge 0) {
            $nameRva = [BitConverter]::ToUInt32($bytes, $cursor + 12)
            $thunkRva = [BitConverter]::ToUInt32($bytes, $cursor)
            $iatRva = [BitConverter]::ToUInt32($bytes, $cursor + 16)
            if ($nameRva -eq 0 -and $thunkRva -eq 0 -and $iatRva -eq 0) { break }
            $nameOffset = Convert-RvaToOffset -Sections $sections -Rva $nameRva
            if ($nameOffset -lt 0) { break }
            $names.Add((Read-AsciiString -Bytes $bytes -Offset $nameOffset))
            $cursor += 20
        }
    }

    # Data directory 13: delay-load imports, 32-byte descriptors, name RVA at +4.
    $delayRva = [BitConverter]::ToUInt32($bytes, $directories + (13 * 8))
    if ($delayRva -ne 0) {
        $cursor = Convert-RvaToOffset -Sections $sections -Rva $delayRva
        while ($cursor -ge 0) {
            $nameRva = [BitConverter]::ToUInt32($bytes, $cursor + 4)
            if ($nameRva -eq 0) { break }
            $nameOffset = Convert-RvaToOffset -Sections $sections -Rva $nameRva
            if ($nameOffset -lt 0) { break }
            $names.Add((Read-AsciiString -Bytes $bytes -Offset $nameOffset))
            $cursor += 32
        }
    }

    return @($names | Sort-Object -Unique)
}

$script:SystemDirectories = @(
    (Join-Path $env:SystemRoot 'System32'),
    (Join-Path $env:SystemRoot 'SysWOW64')
)

function Test-HostDll {
    <# True when the DLL belongs to Windows or to the Visual C++ redistributable. #>
    param([string]$Name)
    if ($Name -match '^(api-ms-|ext-ms-)') { return $true }
    foreach ($directory in $script:SystemDirectories) {
        if (Test-Path -LiteralPath (Join-Path $directory $Name)) { return $true }
    }
    return $false
}

# --- stage the install tree ------------------------------------------------
if (-not (Test-Path -LiteralPath (Join-Path $Prefix 'bin'))) {
    throw "bundle_windows: '$Prefix' does not look like an install prefix (no bin\)"
}
if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $Prefix 'bin') -Destination $Stage -Recurse
foreach ($extra in @('share', 'lib')) {
    $source = Join-Path $Prefix $extra
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination $Stage -Recurse
    }
}

$stageBin = Join-Path $Stage 'bin'
$searchDirectories = @($stageBin) + ($SearchPath | Where-Object { $_ -and (Test-Path -LiteralPath $_) })

$executables = @(Get-ChildItem -LiteralPath $stageBin -Filter '*.exe' -File)
if ($executables.Count -eq 0) {
    throw "bundle_windows: no executables found in $stageBin"
}

# --- resolve the DLL closure ----------------------------------------------
$bundled = @{}      # staged DLL name -> directory it came from
$hostDlls = @{}     # DLL name left to the host
$queue = New-Object System.Collections.Generic.Queue[string]
foreach ($executable in $executables) { $queue.Enqueue($executable.FullName) }
foreach ($dll in Get-ChildItem -LiteralPath $stageBin -Filter '*.dll' -File) {
    $bundled[$dll.Name] = 'install tree'
    $queue.Enqueue($dll.FullName)
}

while ($queue.Count -gt 0) {
    $current = $queue.Dequeue()
    foreach ($import in @(Get-PeImport -Path $current)) {
        if (Test-HostDll -Name $import) {
            $hostDlls[$import] = $true
            continue
        }
        if ($bundled.ContainsKey($import)) { continue }

        $resolved = $null
        foreach ($directory in $searchDirectories) {
            $candidate = Join-Path $directory $import
            if (Test-Path -LiteralPath $candidate) { $resolved = $candidate; break }
        }
        if (-not $resolved) {
            $searched = $searchDirectories -join '; '
            $owner = Split-Path -Leaf $current
            throw "bundle_windows: $owner imports $import, which is neither a Windows DLL nor present in any search directory ($searched)"
        }
        $target = Join-Path $stageBin $import
        if (-not (Test-Path -LiteralPath $target)) {
            Copy-Item -LiteralPath $resolved -Destination $target
        }
        $bundled[$import] = (Split-Path -Parent $resolved)
        $queue.Enqueue($target)
    }
}

# --- provenance + README ---------------------------------------------------
$manifest = Join-Path $stageBin 'BUNDLED-DLLS.txt'
$bundled.Keys | Sort-Object | ForEach-Object {
    "{0}`t{1}" -f $_, $bundled[$_]
} | Set-Content -LiteralPath $manifest -Encoding ascii

$readme = @"
PolyMesh for Windows (x64)
==========================

This archive is a relocatable install tree, not a fully static build. Extract
it anywhere and run the executables from bin\ directly:

  bin\polymesh.exe --version
  bin\polymesh-gui.exe path\to\model.step
  bin\polymesh-webd.exe --help

Every non-system DLL these executables need is already in bin\ beside them;
bin\BUNDLED-DLLS.txt records where each one came from.

What the host must provide
--------------------------

* Microsoft Visual C++ 2015-2022 Redistributable (x64). The executables are
  compiled against the dynamic MSVC runtime (vcruntime140.dll, msvcp140.dll),
  which Microsoft ships as a separate installer:
  https://aka.ms/vs/17/release/vc_redist.x64.exe
  Windows 11 and up-to-date Windows 10 machines usually already have it.
* A working GPU driver with OpenGL 3.3 for bin\polymesh-gui.exe. The Microsoft
  Basic Display Adapter only offers OpenGL 1.1 and cannot run the GUI. The CLI
  and the web server do not need a GPU.
* Windows 10 1809 or newer, 64-bit.

Windows DLLs this build loads from the system:
$(($hostDlls.Keys | Sort-Object | ForEach-Object { "  $_" }) -join "`r`n")
"@
Set-Content -LiteralPath (Join-Path $Stage 'README-Windows.txt') -Value $readme -Encoding ascii

# --- verify ----------------------------------------------------------------
$missing = @()
foreach ($image in @(Get-ChildItem -LiteralPath $stageBin -Include '*.exe', '*.dll' -File -Recurse)) {
    foreach ($import in @(Get-PeImport -Path $image.FullName)) {
        if (Test-HostDll -Name $import) { continue }
        if (-not (Test-Path -LiteralPath (Join-Path $stageBin $import))) {
            $missing += "{0} -> {1}" -f $image.Name, $import
        }
    }
}
if ($missing.Count -gt 0) {
    throw "bundle_windows: unresolved imports in the staged tree:`n$($missing -join "`n")"
}

$payload = (Get-ChildItem -LiteralPath $Stage -Recurse -File | Measure-Object -Property Length -Sum).Sum
"PolyMesh bundle ready: {0} executables, {1} bundled DLLs, {2:N1} MiB total" -f `
    $executables.Count, $bundled.Count, ($payload / 1MB)
