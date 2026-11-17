<#
.SYNOPSIS
    Local release packaging for Solstice on the RTX 5090 machine.

.DESCRIPTION
    Mirrors the CI packaging path (build-windows.ps1 'package' stage ->
    packaging/windows/package-complete.py) using the local build tree:

      1. Refuses to run while solstice.exe/krita.exe is running
         (DLL locks — agents never kill the app).
      2. Optionally runs cmake --install on the local _build tree
         (skip with -SkipInstall).
      3. Invokes package-complete.py --no-interactive with the CI
         package-name convention (Solstice-<version>-windows-x64-<sha12>)
         and the shellex pre-zip hook.
      4. Emits SHA256SUMS.txt and BUILD-INFO.txt next to the ZIP(s),
         matching the CI artifact layout.

    The packager stages and writes zips into the CURRENT directory, so
    this script runs it from $OutDir (default
    <SrcRoot>\_packaging\release — gitignored via _packaging).
    Nothing is signed, uploaded, or tagged — the GitHub Release and
    website steps stay manual per docs/agent/devops-release.md.

.EXAMPLE
    .\package-local.ps1 -KritaDevRoot D:\krita-dev -DepsInstallDir D:\krita-dev\deps -MingwBinDir C:\llvm-mingw\bin
    .\package-local.ps1 -KritaDevRoot D:\krita-dev -DepsInstallDir D:\krita-dev\deps -MingwBinDir C:\llvm-mingw\bin -SkipInstall
#>
[CmdletBinding()]
param(
    # Root holding env.bat, _build, _install (the <krita-dev-root> placeholder)
    [Parameter(Mandatory = $true)]
    [string]$KritaDevRoot,

    # Dependency install prefix (--deps-install-dir; the deps _install dir)
    [Parameter(Mandatory = $true)]
    [string]$DepsInstallDir,

    # LLVM-MinGW bin dir providing runtime DLLs ($env:MINGW_BIN_DIR)
    [Parameter(Mandatory = $true)]
    [string]$MingwBinDir,

    # Repo root (defaults to this script's ../../..)
    [string]$SrcRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path,

    # Output directory for the ZIPs + manifests (default: _packaging\release)
    [string]$OutDir = (Join-Path $SrcRoot "_packaging\release"),

    # 7-Zip executable (default: 7z on PATH)
    [string]$SevenZip = "7z",

    # Skip cmake --install (reuse existing _install)
    [switch]$SkipInstall,

    # Extra args forwarded verbatim to package-complete.py
    [string[]]$PackagerArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$envBat = Join-Path $KritaDevRoot "env.bat"
$buildDir = Join-Path $KritaDevRoot "_build"
$installDir = Join-Path $KritaDevRoot "_install"

foreach ($p in @($envBat, $buildDir, $DepsInstallDir, $MingwBinDir)) {
    if (-not (Test-Path $p)) { throw "missing required path: $p" }
}
foreach ($exe in @('bin\solstice.exe', 'bin\solstice.com', 'bin\krita.dll', 'bin\libkritagpu.dll')) {
    if (-not (Test-Path (Join-Path $installDir $exe))) {
        throw "missing install output: $exe (build + install first, or drop -SkipInstall)"
    }
}

# A running Solstice locks DLLs on Windows; refuse early.
$running = Get-Process -Name "solstice", "krita" -ErrorAction SilentlyContinue
if ($running) {
    throw "solstice.exe/krita.exe is running (PID $($running.Id -join ',')). Close it first."
}

# Solstice version from CMakeLists.txt (same logic as CI Get-SolsticeVersion)
$lists = Get-Content -Raw (Join-Path $SrcRoot 'CMakeLists.txt')
$parts = @{}
foreach ($name in 'MAJOR', 'MINOR', 'PATCH', 'LABEL') {
    if ($lists -notmatch "(?m)^set\(SOLSTICE_VERSION_$name `"?([^`")]*)`"?\)") {
        throw "SOLSTICE_VERSION_$name not found in CMakeLists.txt"
    }
    $parts[$name] = $Matches[1]
}
$version = "$($parts.MAJOR).$($parts.MINOR).$($parts.PATCH)"
if ($parts.LABEL) { $version += "-$($parts.LABEL)" }
$sha = (git -C $SrcRoot rev-parse --short=12 HEAD).Trim()
$packageName = "Solstice-$version-windows-x64-$sha"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

if (-not $SkipInstall) {
    Write-Host "== cmake --install =="
    cmd.exe /d /s /c "call `"$envBat`" && cmake --install `"$buildDir`""
    if ($LASTEXITCODE -ne 0) { throw "cmake --install failed ($LASTEXITCODE)" }
}

$packager = Join-Path $SrcRoot "packaging\windows\package-complete.py"
$shellexHook = Join-Path $SrcRoot "build-tools\windows-shellex\add-shellex.py"
foreach ($p in @($packager, $shellexHook)) {
    if (-not (Test-Path $p)) { throw "missing required script: $p" }
}

$env:MINGW_BIN_DIR = $MingwBinDir
$env:SEVENZIP_EXE = (Get-Command $SevenZip).Source

Write-Host "== package-complete.py ($packageName) =="
Push-Location $OutDir
try {
    cmd.exe /d /s /c "call `"$envBat`" && python `"$packager`" --no-interactive --package-name `"$packageName`" --src-dir `"$SrcRoot`" --deps-install-dir `"$DepsInstallDir`" --krita-install-dir `"$installDir`" --pre-zip-hook `"$shellexHook`" $PackagerArgs"
    if ($LASTEXITCODE -ne 0) { throw "packaging failed ($LASTEXITCODE)" }
} finally { Pop-Location }

$zips = Get-ChildItem $OutDir -Filter "*.zip"
if (-not $zips) { throw "no zip produced in $OutDir" }

$zips | ForEach-Object {
    $h = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$h  $($_.Name)"
} | Set-Content -Path (Join-Path $OutDir "SHA256SUMS.txt") -Encoding ascii

@"
Solstice Windows x64 local, unsigned build
Version: $version
Start: Solstice.exe in the package folder (it starts bin\solstice.exe)
Commit: $sha
Built: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss K') on local machine (RTX 5090 / local toolchain)
GPU engine: compiled; GPU runtime and interactive tests were not run.
Explorer thumbnails: shellex\register-thumbnails.cmd (Krita Shell Extension 1.2.4d, MIT)
"@ | Set-Content -Path (Join-Path $OutDir 'BUILD-INFO.txt') -Encoding utf8

Write-Host ""
Write-Host "done: $OutDir"
Write-Host "next: verify artifact contents per docs/agent/devops-release.md, then the manual GitHub Release + website steps."
