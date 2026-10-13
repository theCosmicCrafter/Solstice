# GitHub Actions Windows trial build

## Scope

`.github/workflows/windows-build.yml` is manual-only (`workflow_dispatch`),
Windows x64, Release, unsigned ZIP plus logs. It uses GitHub-hosted
`windows-2022`, four build workers, a 330-minute timeout, `contents: read`, and
SHA-pinned official actions. There are no push/PR triggers, publishing jobs,
signing keys, self-hosted runners, or inherited KDE CI credentials.

The user authorized creation, push and a first trial run on 2026-10-03.
User instructions: `docs/test-builds.md`.

## Dependency and build files

- `build-tools/github-actions/windows-dependencies.lock.json`: 86 public
  KDE Windows package versions, download URLs, sizes, SHA-256 checksums,
  source recipe revisions and dependency relationships. The package set was
  seeded from the local Qt6 dependency inventory. Some older local versions
  were already absent from the registry, so available versions on the same
  Qt6 dependency branch were pinned for CI. Do not claim binary identity with
  the local development installation. The resolved Qt version is in logs.
- `bootstrap-windows.py`: checksum verification, constrained archive
  extraction, omission of dependency debug symbols, LLVM-MinGW 20251118
  and Vulkan SDK 1.4.357.0 installation. SDK installation uses `copy_only=1`.
- `build-windows.ps1`: configure, build/install, and package stages; all paths
  are under the runner's temporary `solstice` directory. It never uses the
  developer's `env.bat`, `_build`, `_install`, or active `kritarc`.

Dependencies are downloaded sequentially and each verified tar is deleted
after extraction, reducing peak disk usage. An exact cache key includes the
lock and bootstrap script hashes. The cache is saved only after a complete
bootstrap. No partial-key cache fallback is allowed. An incomplete cache
fails rather than mixing dependency sets. If the public registry prunes a
locked package, deliberately refresh the lock and verify the new version;
never bypass a checksum or silently fall back to a floating version.

CMake uses Qt6, Next branding, Python 3.13, a separate dependency prefix and
install tree. `BUILD_TESTING=OFF` keeps GUI tests out of cloud jobs. Configure
must record `HAVE_KRITA_GPU_ENGINE=ON`; otherwise the workflow fails instead
of passing with an accidentally disabled GPU engine. Vision ML is built by
the regular application build, with Vulkan SDK available.

`KDE_INSTALL_USE_QT_SYS_PATHS=ON` and `KDE_INSTALL_QMLDIR=qml` are required and
checked during configuration.
With separate dependency and application prefixes, ECM otherwise defaults it
to OFF and installs QML modules under `lib/qml`. The Windows packager requires
the Qt layout (`qml`) and correctly rejects that incompatible installation.
The explicit relative QML directory also prevents ECM from selecting an absolute
directory in the dependency prefix instead of the application's install tree.
The packager passes the application install tree's `qml` directory through
`windeployqt --qmlimport` so application modules are discovered even when Qt
is in the separate dependency prefix. Only an existing import directory is
passed: Qt 6.11's scanner returns no JSON for a nonexistent directory.
Debug splitting, like copying, skips optional CLI tools when they are absent
(notably `kritarunner` when Python support is disabled).

Packaging reuses `packaging/windows/package-complete.py`, with noninteractive
arguments and LLVM-MinGW runtime DLLs. Its `--pre-zip-hook` runs
`build-tools/windows-shellex/add-shellex.py`, which downloads the pinned Krita
Shell Extension, verifies its checksums and adds the `shellex` folder
(`windows-shell-thumbnails.md`); a checksum mismatch fails the package stage. It uploads the ZIP and dependency/build
metadata as an artifact, not a GitHub Release. Build logs and CMake diagnostics
are uploaded even if a later stage fails. Retention is seven days.

## Verification and operation

Before pushing changes, parse the Python/PowerShell/YAML and inspect archive
handling and checksum rejection. Dispatch the workflow on `main` (`krita-sol-gpu` until 2026-10-13),
record the exact commit and run URL, and follow its result. Inspect configure
output, GPU enablement, compiler failures, package output and artifacts. A
run is only successful when build, installation, packaging and artifact
upload complete. Do not report a skipped GPU test as passed.

No broad UI tests or `kis_kra_saver_test` run here. Interactive painting,
Undo/Redo, save/reload and Vulkan/OpenGL sharing remain manual real-hardware
checks. No local application restart is required for workflow-only changes.

## First trial run (2026-10-03)

- Commit: `70bf96017d55f9e4c7b24ce3297416a199a2264f`.
- Run: <https://github.com/udtren/Solstice/actions/runs/37120885525>.
- Dependency bootstrap, cache save and Release configuration completed.
  The GPU-engine enablement check passed.
- Qt resolved to 6.11.0. Compilation and installation succeeded in 96 minutes
  with two workers, including the Vulkan engine and Vision ML.
- Packaging failed before ZIP creation because `KDE_INSTALL_USE_QT_SYS_PATHS`
  defaulted to OFF. The CI configure arguments now explicitly enable it and
  validate the cache value before starting the long compilation step.
- No application ZIP was produced by this run; subsequent runs below verified
  the corrected configuration and packaging.
- SIP/PyQt6 were not found with setup-python's interpreter, so Python plugin
  support is disabled. Resolve the dependency package's Python module layout
  and interpreter compatibility before claiming parity with local builds.
  Eigen3 and xsimd were found through their supported-version fallbacks; the
  earlier failed version probes are not missing-dependency failures.

The corrected run is
<https://github.com/udtren/Solstice/actions/runs/37126839545> at
`8b18d8a13ba5fd60dee423f573f95c46ed35d54c`, using four build workers.
Compilation and installation passed in about 66 minutes. Packaging reached
`windeployqt` but failed because the scanner was passed the nonexistent `i/qml`
directory; ECM had installed the modules in `deps/qml`. This was reproduced
locally with the exact locked Qt 6.11, ICU and zlib archives: direct scanning
without that missing path returned JSON, while the extra path returned exit 1
and no stdout. The correction sets a relative QML install directory, only
passes existing import paths, and handles absent optional CLI tools.
With an existing import directory, both the direct scanner and the Qt 6.11
`windeployqt --dry-run` returned exit 0 in the isolated local reproduction.
Locally, a `windeployqt --dry-run` with Qt Quick enabled found
`org.krita.components` through the explicit application QML import path.

## Verified successful run (2026-10-04 JST)

- Run: <https://github.com/udtren/Solstice/actions/runs/37131426139>.
- Commit: `e7927d99f943accc2fa23e536269420cdf13c15b`.
- All required steps succeeded: dependency cache restore, configuration,
  compilation, installation, ZIP creation and artifact upload. Total job time
  was 47 minutes 39 seconds; completion was 00:44 JST on October 4.
- Artifact: `Solstice-windows-x64-3`, ID `11277529982`, 385,298,266 bytes.
  Its downloaded SHA-256 matched GitHub's recorded digest:
  `d39a119dc86548dc4a93c12b4084da73d56e2bfc60f9905b92375a88391bc705`.
- Both inner ZIPs matched `SHA256SUMS.txt`. Application ZIP SHA-256:
  `d912ab6ef82483079563f91c5145e5464b66cce7bcb1c122b439cfd990690900`.
- Application ZIP CRC validation passed for all 5,039 entries. Twelve required
  files were present, including the executables, GPU and Vision ML libraries,
  Qt Core/Gui/Widgets, `platforms/qwindows.dll`, and the
  `org.krita.components` module's `qmldir` and DLL.
- The install log confirms application QML files are now under `i/qml`.
- The artifact expires at 00:43 JST on October 11, 2026. A new manual run is
  required to obtain another artifact after expiration.

This verifies compilation and packaging, not application execution. GPU runtime,
interactive behavior and Python plugin support remain outside this trial's
verified scope. The local application, development installation and `kritarc`
were not changed during verification.
