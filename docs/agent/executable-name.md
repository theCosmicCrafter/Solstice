# Executable name (solstice.exe) — agent reference

Status: done and manually checked 2026-10-11 at the user's request (Windows). User-facing note:
[docs/test-builds.md](../test-builds.md).

## What changed

On Windows the application is started by two small stubs that call
`krita_main()` in `krita.dll` (`krita/windows_stub_main.cpp`). Their output
names changed from `krita` to `solstice`:

| File | Before | Now |
| --- | --- | --- |
| GUI launcher (`krita_windows_stub_exe`) | `bin/krita.exe` | `bin/solstice.exe` |
| Console launcher (`krita_windows_stub_com`) | `bin/krita.com` | `bin/solstice.com` |
| Version resource (`krita/versioninfo.rc.in`) | `InternalName` `krita`, `OriginalFilename` `krita.exe` | `solstice`, `solstice.exe` |
| Package root launcher (`solstice_root_launcher`, 2026-10-13) | `krita.lnk` and two other prebuilt shortcuts | `Solstice.exe` |

Kept on purpose:

- `krita.dll`, `libkrita*.dll`, plugin names, `kritarunner`, the CMake
  target names, the embedded manifest resource file name and the manifest
  assembly identity. They are invisible to users and renaming them touches
  every library and plugin.
- The application name `krita`. `krita_main()` now calls
  `QCoreApplication::setApplicationName("krita")` first: before `KAboutData`
  (component name `krita`) Qt would otherwise derive the name from the
  executable, `solstice`. The profile paths and config names are already
  explicit (`KisSolsticePaths`, `KConfig::setMainConfigName()`), so the
  settings stay in `%APPDATA%\Solstice\config\kritarc`.
- The single-instance key (`"Krita5"` + home) and Python's `Krita.instance()`.
- Linux and macOS: the executable target `krita` is unchanged (not validated
  for Solstice).

## Package root launcher (2026-10-13)

The ZIP's root holds `Solstice.exe`, so users need not open `bin`. Krita
ships prebuilt shortcuts there (`packaging/windows/krita*.lnk`: `%COMSPEC%
/C start "" "%CD%\bin\krita.exe"` with an empty working folder) because a
`.lnk` stores an absolute target; they start `krita.exe`, flash a console
and carry an icon path from Krita's build machine, so Solstice does not copy
them (user decision, option 2 of three: shortcut, launcher, `.cmd`).

- `krita/windows_root_launcher.cpp`: a GUI (`WIN32`) program without Qt.
  It starts `<own folder>\bin\solstice.exe`, or `<own folder>\solstice.exe`
  when there is no `bin` (as installed), with the command line after its own
  program name unchanged (quoting kept), the inherited working folder and
  environment, then `AllowSetForegroundWindow()` and exits. On failure it
  shows a message box with the path.
- `krita/CMakeLists.txt`: target `solstice_root_launcher`, output
  `solstice-launcher.exe` (a second `solstice.exe` would clash in `bin`),
  the application icon (`krita_windows_stub_ICONS_SRCS`) and the version
  resource. Linked with `-s` (MinGW) or `/DEBUG:NONE` (MSVC): the packager's
  debug splitting puts `.debug` files in `<folder>.debug`, which for the
  root would be outside the package. MinGW also links `-static`: llvm-mingw
  otherwise imports `libc++.dll` and `libunwind.dll`, which the package has
  only in `bin`, and the launcher fails with `0xC0000135` (found by the
  stand-in test below). It then imports only `KERNEL32`, `USER32` and the
  Windows `api-ms-win-crt-*` libraries (about 400 KB).
- `packaging/windows/package-complete.py` copies
  `bin\solstice-launcher.exe` to `<package>\Solstice.exe` (the installed copy
  stays out of the package's `bin`); `package-complete-msvc.py` does the same
  when the file exists.
- `build-tools/github-actions/build-windows.ps1` requires the installed
  `bin/solstice-launcher.exe`.
- The NSIS installer's shortcuts start `bin\solstice.exe` directly and do
  not install the launcher.

Taskbar and Task Manager show the running `bin\solstice.exe`; pinning the
running window pins that file.

Test without starting Solstice: put the launcher as `Solstice.exe` beside a
stand-in `bin\solstice.exe` that records its command line and working
folder, then start it with arguments containing spaces and quotes from
another folder.

## Touch points

- `krita/CMakeLists.txt`: `OUTPUT_NAME "solstice"` for both stubs.
- `krita/versioninfo.rc.in`: internal and original file names.
- `krita/main.cc`: the application name at the start of `krita_main()`.
- `packaging/windows/package-complete.py`: copies, `objdump` checks,
  `windeployqt` inputs and debug splitting use `solstice.exe`/`solstice.com`.
  The unused `package-complete-msvc.py`, the NSIS installer and the MSIX
  scripts still name `krita.exe`; update them if they are ever used.
- `build-tools/github-actions/build-windows.ps1`: required outputs.
- `run-krita.bat`, `build-tools/paint-trace/run.cmd` (launcher and the
  running-process check `solstice`, `krita`).

## Development install

A rebuilt `_install\bin` contains `solstice.exe` and `solstice.com`. The old
`krita.exe`/`krita.com` from earlier installs are not removed by CMake and
still start the same `krita.dll`; delete them only with the user's consent.

Before installing, check that neither process runs:
`Get-Process | Where-Object { $_.ProcessName -match '^(krita|solstice)' }`.

## Checks

- `(Get-Item <install>\bin\solstice.exe).VersionInfo`: `ProductName`
  Solstice, `InternalName` solstice, `OriginalFilename` solstice.exe.
- Manual: start `solstice.exe`; the existing profile, settings, workspaces
  and resources are used (no first-start question); the taskbar and Task
  Manager show Solstice; `solstice.com --version` prints the version.
- Trial build: the ZIP has `Solstice.exe` in its root, and `bin/solstice.exe`
  and `bin/solstice.com`.
