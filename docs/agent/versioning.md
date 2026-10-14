# Versioning: technical notes

User guide: [`../versioning.md`](../versioning.md). Decision record:
[`wiki/decisions/versioning.md`](wiki/decisions/versioning.md).

Added 2026-10-07 at the user's request: Solstice shows its own version,
starting at `0.1.0-alpha`.

## Two versions

| Version | Value | Defined in | Used for |
| --- | --- | --- | --- |
| Solstice version | `0.2.0-beta.3` | top-level `CMakeLists.txt`: `SOLSTICE_VERSION_MAJOR/MINOR/PATCH/LABEL/BUILD` -> `SOLSTICE_VERSION_STRING` | everything shown to users |
| Krita compatibility version | `6.0.5-prealpha` | top-level `CMakeLists.txt`: `KRITA_VERSION_STRING` and related | file formats, resources, scripting, library versions |

`libs/version/kritaversion.h.cmake` defines both. `KritaVersionWrapper`
provides:

- `versionString(checkGit)`: the Krita compatibility version (unchanged API);
- `solsticeVersionString(checkGit)`: the Solstice version, with
  `" (git <sha>)"` when `checkGit` is true and the build has a git hash.

## Where each is used

**Solstice version:**

- `KAboutData` in `krita/main.cc`, and so `qApp->applicationVersion()`: the
  splash screen and About dialog, the KBugReport dialog, the network
  User-Agent, Spriter export's `generator_version`;
- bug information (`plugins/extensions/buginfo/dlg_buginfo.cpp`) and the usage
  log (`libs/global/KisUsageLogger.cpp`), each followed by
  "Based on Krita: <compatibility version>";
- XMP `CreatorTool` of exported images ("Solstice <version>",
  `libs/metadata/kis_meta_data_filter_p.cc`);
- Windows `versioninfo.rc`:
  - `FILEVERSION`/`PRODUCTVERSION` are `MAJOR,MINOR,PATCH,BUILD`;
  - the version strings carry the label and git hash;
- the application manifests (`krita/krita*.exe.manifest.in`,
  `kritarunner*.exe.manifest.in`).

**Krita compatibility version (keep it):**

- `.kra` `kritaVersion` attribute. `plugins/impex/libkra/kra_converter.cpp`
  writes it, and `KisKraLoader` chooses compatibility behavior from it. A
  lower number would make files look like they came from an old Krita, in
  Solstice and in Krita.
- `KRITA_RESOURCE_VERSION` in the resource folder
  (`libs/resources/KisResourceLocator.cpp`): bundled resources are
  reinstalled only when the application version is greater than the stored
  one.
- The resource database `version_information` table
  (`KisResourceCacheDb.cpp`) and bundle `generator` metadata.
- Python `Krita.version()` (`libs/libkis/Krita.cpp`): scripts compare it.
- Library `VERSION`/`SOVERSION` (`GENERIC_KRITA_LIB_VERSION*`), the
  `kritaversion` command-line tool, `KisManualUpdater`. The updater compares
  with krita.org releases, is not used for Solstice, and is left unchanged.
- The unused NSIS/MSIX/AppImage packaging inputs.

## Bumping the version

Edit only the `SOLSTICE_VERSION_*` values in the top-level `CMakeLists.txt`:

- `MINOR` for a milestone, `PATCH` for fixes only (pre-1.0 semantic
  versioning);
- `LABEL`: `beta.3` now (`alpha` until 2026-10-13, `beta` for the first
  beta), then `beta.N` or `rc.N`, empty for a release;
- `BUILD`: the fourth Windows file-version digit, for rebuilds of the same
  version; otherwise 0.

Do not change `KRITA_VERSION_STRING` unless the compatibility consequences
above are intended. The Windows file version is numeric, so the label only
appears in the version strings.

## Releasing

1. Bump the version (above), record it under History, commit and push
   `main`.
2. Run the "Windows x64 trial build" workflow on `main`
   (`gh workflow run windows-build.yml --ref main`) and wait for it to
   succeed (`docs/agent/github-actions.md`).
3. Create the GitHub Release `v<version>` at the built commit, as a
   prerelease while the version has a label. Attach the run's application
   ZIP, `-dbg.zip`, `SHA256SUMS.txt`, `BUILD-INFO.txt` and
   `windows-dependencies.lock.json`, and follow the previous release's
   notes (what's new, download, highlights, supported environment, build
   commit, run and SHA-256).
4. Update the website the same day (user decision, 2026-10-14), in
   `https://github.com/udtren/tidalboxstudio-web`:
   - on `develop`, set `links.release`, `version` and `versionName` in
     `solstice/src/site.ts` (the only place the version appears);
   - in `solstice/`, run `npm run check` and `npm run build`, and confirm
     the new version in `dist/index.html` and `dist/download/index.html`;
   - commit and push `develop`, then, after the GitHub Release exists,
     merge `develop` into `main` and push. Cloudflare Workers Builds
     publishes `main` to `https://solstice.tidalboxstudio.com`; check the
     live Download page shows the new version and its link opens the
     release.

## History

| Version | Date | Reason |
| --- | --- | --- |
| `0.1.0-alpha` | 2026-10-07 | First Solstice version. |
| `0.2.0-alpha` | 2026-10-10 | Milestone (user request): GPU brush by default, masking brush on the GPU, ABR import improvements, Solstice icons and splash, Explorer thumbnails. |
| `0.2.0-beta` | 2026-10-13 | Beta for publication (user request), after the pre-release review; only the label changes. Released 2026-10-14 as GitHub Release `v0.2.0-beta` (prerelease, commit `a442a5ce25`); its CI build lacked the Python plugins. |
| `0.2.0-beta.2` | 2026-10-14 | Second beta (user request): the CI build includes the Python plugins (Python Plugin Manager, pykrita) again; new Puppet Warp and Brush Stroke Layer images. GitHub Release `v0.2.0-beta.2`. |
| `0.2.0-beta.3` | 2026-10-14 | Third beta (user request): first upstream Krita intake (2026-W42: thumbnail cache crash, animation curves scrolling, second-wheel horizontal scrolling, perspective transform conditions, PSD group collapse and layer locks). Released 2026-10-14 as GitHub Release `v0.2.0-beta.3` (prerelease, commit `c89bc02475`, run 38503291774); website updated the same day. |

## Checks

- After reconfiguring, check the CMake output for
  `-- Solstice version: <version>`, and check that
  `_build/libs/version/kritaversion.h` defines both versions.
- `(Get-Item <install>\bin\solstice.exe).VersionInfo`: `ProductName` is
  `Solstice`, and the versions are the Solstice version.
- Manual checks: the splash screen and Help > About show the Solstice version;
  Help > Show system information shows both versions; a saved `.kra` still has
  `kritaVersion="6.0.5-prealpha"` in `maindoc.xml`.

## Related build fix

The full build with the version change also compiled the Python bindings,
which had failed since 2026-10-04. `KisPresetChooser::eventFilter()` was
`private`, and sip subclasses the widget. It is now `protected`
(`libs/ui/widgets/kis_preset_chooser.h`).
