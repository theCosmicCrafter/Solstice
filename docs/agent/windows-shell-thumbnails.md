# Explorer thumbnails (Krita Shell Extension) — agent reference

User document: [docs/windows-shell-thumbnails.md](../windows-shell-thumbnails.md).

Status: implemented and manually checked 2026-10-10 (user request, task moved
from `todo.md`).

## Why

Explorer thumbnails of `.kra`/`.krz` come from the Krita Shell Extension
(`kritashellex64.dll`, an `IThumbnailProvider` that reads `preview.png`
from the archive). Krita's NSIS installer registers it machine-wide (its
`installer_krita.nsi`, removed from Solstice on 2026-10-13, with macros from
the downloaded `krita_shell_integration.nsh`). Solstice ships a ZIP, so after
Krita is uninstalled nothing registers the extension and new files have no
thumbnail. Solstice's own installer (`packaging/windows/installer/
installer_solstice.nsi`, 2026-10-13) offers the same per-user registration
as an optional component: it runs `shellex\register-thumbnails.ps1` for the
installing user and `-Unregister` on uninstall. The saved previews were verified correct on
2026-10-08.

## Source and license

- Package: `krita-nsis-v1.2.4d.zip` from files.kde.org, the same file and
  SHA-256 as upstream's `MakeInstallerNsis.cmake.in`; built from
  <https://github.com/alvinhochun/KritaShellExtension> v1.2.4d (DLL version
  1.2.4.2 in both bitnesses).
- License: MIT (Alvin Wong); bundled libzip (BSD-3-Clause), tinyxml2 and zlib
  (zlib license). `COPYING.txt` carries all of them and ships next to the DLLs.
- Pinned in `build-tools/windows-shellex/shellex-package.json`: archive
  SHA-256 and the SHA-256 of each extracted member. The DLLs are not
  committed to the repository.

## Files

| File | Role |
| --- | --- |
| `build-tools/windows-shellex/shellex-package.json` | URL, version, archive and member checksums. |
| `build-tools/windows-shellex/add-shellex.py` | Downloads (optional `--cache-dir`), verifies, reads only the pinned members (no PDBs, no NSIS scripts) and writes `<root>/shellex/` with the DLLs, `COPYING.txt`, `VERSION.txt` and the scripts below. Requires `<root>/bin`. Never touches the registry. Works as `package-complete.py --pre-zip-hook` (called with the package root). |
| `build-tools/windows-shellex/register-thumbnails.ps1` | Per-user registration; `-Unregister`, `-DryRun`, `-TestRoot`. ASCII only (Windows PowerShell 5.1 reads BOM-less files as ANSI). |
| `build-tools/windows-shellex/register-thumbnails.cmd`, `unregister-thumbnails.cmd` | Double-click wrappers (`-ExecutionPolicy Bypass`, `pause`); CRLF through `.gitattributes`. |
| `build-tools/windows-shellex/README.txt` | Shipped instructions. |
| `build-tools/github-actions/build-windows.ps1` | Passes `--pre-zip-hook add-shellex.py`; BUILD-INFO line. |

## Registration (HKCU only)

All under `HKEY_CURRENT_USER`, no administrator rights:

- `Software\Classes\CLSID\{C6806289-D605-4AFE-A778-BC584303DB9A}` (Krita's
  thumbnail provider CLSID) with `InprocServer32` = the folder's
  `kritashellex64.dll`, `ThreadingModel=Apartment`; the same in the 32-bit
  registry view (`RegistryView.Registry32`, i.e. `Classes\WOW6432Node`) with
  `kritashellex32.dll` for 32-bit programs' file dialogs.
- `Software\Classes\.kra\shellex\{E357FCCD-A995-4576-B01F-234630154E96}` and
  the same for `.krz` = that CLSID.
- Marker `Software\Solstice\ShellExtension` (`InstallLocation`, `Version`).
- `SHChangeNotify(SHCNE_ASSOCCHANGED)` afterwards.

Not registered, deliberately:

- The property handler (`HKLM\...\PropertySystem\PropertyHandlers`, needs
  administrator rights).
- File associations, ProgIDs, `PerceivedType`, `.ora`: Solstice does not
  take over opening `.kra` files or other formats.

## Coexistence with Krita

Krita registers the same CLSIDs under HKLM. HKCU entries win in the merged
`HKCR` view, so this user's Explorer loads Solstice's copy (same DLL
version). Unregistering removes only HKCU keys whose `InprocServer32` lies
in the script's own folder, and extension keys only when they still name
the provider CLSID; Krita's HKLM registration then applies again. A second
Solstice folder registering later replaces the first (message printed); the
first folder's unregister then keeps the newer registration.

## Tests

- `register-thumbnails.ps1 -DryRun` and `-Unregister -DryRun` print the
  planned writes.
- `-TestRoot SolsticeShellexTest` runs the real write/delete code under
  `HKCU\SolsticeShellexTest` (not seen by Explorer; delete it afterwards).
  It covers registration, the "another folder" keep path, removal and empty
  key cleanup. Outside `Software\Classes` the 32-bit view is not redirected,
  so view separation is only exercised by a real registration.
- `add-shellex.py` against a folder with an empty `bin` checks download,
  checksums and output.
- The development install has `_install\shellex` (added with
  `add-shellex.py C:\...\_install --cache-dir <dir>`); the user registers
  from there.

## Manual checks

1. Run `register-thumbnails.cmd`; a `.kra` and a `.krz` saved by Solstice
   show their thumbnail in Explorer (large icons) and in a 32-bit program's
   file dialog if available.
2. Run `unregister-thumbnails.cmd`; thumbnails of newly saved files are no
   longer generated, and the HKCU keys above are gone.
3. With Krita installed (if available): register, unregister, Krita's
   thumbnails still work.
