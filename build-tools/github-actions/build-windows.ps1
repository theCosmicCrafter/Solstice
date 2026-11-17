# SPDX-FileCopyrightText: 2026 Solstice contributors
# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory)][ValidateSet('configure', 'build', 'package')][string]$Stage,
    [Parameter(Mandatory)][string]$Root,
    [int]$Jobs = 2
)
$ErrorActionPreference = 'Stop'
$Source = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$Root = (Resolve-Path $Root).Path
$Deps = Join-Path $Root 'deps'
$CompilerBin = Join-Path $Root 'llvm-mingw-20251118-ucrt-x86_64/bin'
$Build = Join-Path $Root 'b'
$Install = Join-Path $Root 'i'
$Logs = Join-Path $Root 'logs'
New-Item -ItemType Directory -Force -Path $Logs | Out-Null
# Retain setup-python's executable before placing dependency tools on PATH.
$Python = (Get-Command python).Source
$env:VULKAN_SDK = Join-Path $Root 'vulkan'
$env:PATH = "$CompilerBin;$Deps/bin;$env:VULKAN_SDK/Bin;$env:PATH"
$env:PKG_CONFIG_PATH = "$Deps/lib/pkgconfig;$Deps/share/pkgconfig"
$env:QT_PLUGIN_PATH = "$Deps/plugins"
$env:PYTHONUTF8 = '1'
# SIP, PyQt6 and pyqt-builder from the dependency packages (built for the
# same Python 3.13); without them the Python plugins are silently left out
$env:PYTHONPATH = "$Deps/lib/site-packages"

# The Solstice version from the top-level CMakeLists.txt (docs/agent/versioning.md)
function Get-SolsticeVersion {
    $Lists = Get-Content -Raw (Join-Path $Source 'CMakeLists.txt')
    $Parts = @{}
    foreach ($Name in 'MAJOR', 'MINOR', 'PATCH', 'LABEL') {
        if ($Lists -notmatch "(?m)^set\(SOLSTICE_VERSION_$Name `"?([^`")]*)`"?\)") {
            throw "SOLSTICE_VERSION_$Name not found in CMakeLists.txt"
        }
        $Parts[$Name] = $Matches[1]
    }
    $Version = "$($Parts.MAJOR).$($Parts.MINOR).$($Parts.PATCH)"
    if ($Parts.LABEL) { $Version += "-$($Parts.LABEL)" }
    return $Version
}

function Invoke-Logged([string]$Program, [string[]]$Arguments, [string]$LogName) {
    & $Program @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $Logs $LogName)
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

if ($Stage -eq 'configure') {
    Invoke-Logged cmake @(
        '-S', $Source, '-B', $Build, '-G', 'Ninja',
        "-DCMAKE_C_COMPILER=$CompilerBin/clang.exe",
        "-DCMAKE_CXX_COMPILER=$CompilerBin/clang++.exe",
        "-DCMAKE_PREFIX_PATH=$Deps", "-DCMAKE_INSTALL_PREFIX=$Install",
        "-DPython_EXECUTABLE=$Python", "-DPKG_CONFIG_EXECUTABLE=$Deps/bin/pkgconf.exe",
        '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_WITH_QT6=ON', '-DALLOW_UNSTABLE=QT6',
        '-DBUILD_TESTING=OFF', '-DKRITA_ENABLE_PCH=OFF', '-DFOUNDATION_BUILD=ON',
        '-DBRANDING=Next', '-DENABLE_UPDATERS=OFF', '-DKDE_INSTALL_USE_QT_SYS_PATHS=ON',
        '-DKDE_INSTALL_QMLDIR:PATH=qml'
    ) 'configure.log'
    $Cache = Get-Content -Raw (Join-Path $Build 'CMakeCache.txt')
    if ($Cache -notmatch '(?m)^HAVE_KRITA_GPU_ENGINE:INTERNAL=ON\r?$') {
        throw 'Configuration silently disabled the Vulkan GPU engine'
    }
    # pykrita configures its plugin only with Python, SIP and PyQt6 found
    if (-not (Test-Path (Join-Path $Build 'plugins/extensions/pykrita/plugin'))) {
        throw 'Configuration left out the Python plugins (SIP or PyQt6 not found)'
    }
    if ($Cache -notmatch '(?m)^KDE_INSTALL_USE_QT_SYS_PATHS:BOOL=ON\r?$') {
        throw 'Qt install paths are required for Windows QML deployment'
    }
    if ($Cache -notmatch '(?m)^KDE_INSTALL_QMLDIR:PATH=qml\r?$') {
        throw 'Application QML modules must be installed in the application prefix'
    }
    $QtVersionFile = Join-Path $Deps 'lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake'
    Select-String -Path $QtVersionFile -Pattern '^set\(PACKAGE_VERSION ' |
        ForEach-Object { $_.Line } | Tee-Object -FilePath (Join-Path $Logs 'qt-version.txt')
} elseif ($Stage -eq 'build') {
    Invoke-Logged cmake @('--build', $Build, '--parallel', "$Jobs") 'build.log'
    Invoke-Logged cmake @('--install', $Build) 'install.log'
    # Build the CPU runtime against this job's installed Qt/QMic ABI.
    Invoke-Logged $Python @(
        (Join-Path $Source 'build-tools/gmic/build-runtime.py'),
        '--work', (Join-Path $Root 'gmic'), '--install', $Install,
        '--deps', $Deps, '--compiler-bin', $CompilerBin, '--jobs', "$Jobs"
    ) 'gmic-build.log'
    foreach ($Required in @('bin/solstice.exe', 'bin/solstice.com', 'bin/solstice-launcher.exe', 'bin/krita.dll', 'bin/libkritagpu.dll')) {
        if (-not (Test-Path (Join-Path $Install $Required))) { throw "Missing output: $Required" }
    }
} else {
    $PackageDir = Join-Path $Root 'packages'
    New-Item -ItemType Directory -Force -Path $PackageDir | Out-Null
    $env:MINGW_BIN_DIR = $CompilerBin
    $env:SEVENZIP_EXE = (Get-Command 7z).Source
    $Version = Get-SolsticeVersion
    $PackageName = "Solstice-$Version-windows-x64-$($env:GITHUB_SHA.Substring(0, 12))"
    Push-Location $PackageDir
    try {
        Invoke-Logged $Python @(
            (Join-Path $Source 'packaging/windows/package-complete.py'),
            '--no-interactive', '--package-name', $PackageName,
            '--src-dir', $Source, '--deps-install-dir', $Deps,
            '--krita-install-dir', $Install,
            # Explorer thumbnails of .kra/.krz (docs/agent/windows-shell-thumbnails.md)
            '--pre-zip-hook', (Join-Path $Source 'build-tools/windows-shellex/add-shellex.py')
        ) 'package.log'
    } finally { Pop-Location }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows-dependencies.lock.json') -Destination $PackageDir
    @"
Solstice Windows x64 experimental, unsigned build
Version: $Version
Start: Solstice.exe in the package folder (it starts bin\solstice.exe)
Commit: $env:GITHUB_SHA
Run: https://github.com/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID
GPU engine: compiled; GPU runtime and interactive tests were not run.
Dependency versions and source revisions: windows-dependencies.lock.json
G'MIC CPU runtime: 3.7.4.1; pinned source and Solstice patches in share/gmic/source
Explorer thumbnails: shellex\register-thumbnails.cmd (Krita Shell Extension 1.2.4d, MIT)
"@ | Set-Content -Path (Join-Path $PackageDir 'BUILD-INFO.txt') -Encoding utf8
    Get-ChildItem -LiteralPath $PackageDir -Filter '*.zip' | ForEach-Object {
        $Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$Hash  $($_.Name)"
    } | Set-Content -Path (Join-Path $PackageDir 'SHA256SUMS.txt') -Encoding ascii
}
