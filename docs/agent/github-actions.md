# GitHub Actions Windows trial build

## Scope

`.github/workflows/windows-build.yml` is manual-only (`workflow_dispatch`),
Windows x64, Release, unsigned ZIP plus logs. It uses GitHub-hosted
`windows-2022`, four build workers, a 330-minute timeout, `contents: read`, and
SHA-pinned official actions. There are no push/PR triggers, publishing jobs,
signing keys, self-hosted runners, or inherited KDE CI credentials.

The user authorized creation, push and a first trial run on 2026-10-03.
User instructions: `docs/test-builds.md`.

`.github/workflows/dependency-check.yml` (2026-10-13) runs weekly (Monday
03:17 UTC) and on demand, with `contents: read` and `issues: write`; see
"Dependency mirror" below.

## Dependency and build files

- `build-tools/github-actions/windows-dependencies.lock.json`: 87 public
  KDE Windows package versions (86 until the 2026-10-13 refresh below), download URLs, sizes, SHA-256 checksums,
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

## Lock refresh (2026-10-13)

The first build on `main` (run
<https://github.com/udtren/Solstice/actions/runs/38406141485>) failed in
"Prepare dependencies": all 86 locked packages (`transition.now-qt6-179033…`,
source revision `31c578a`) returned HTTP 404; the registry keeps only recent
builds. `krita-sol-gpu` still built because its branch-scoped Actions cache
held the old set (caches are per branch; one saved on the default branch is
shared, others are not, and an unused cache expires after seven days).

`build-tools/github-actions/refresh-dependency-lock.py` regenerates the lock
from the registry API: for every locked package, the newest
`transition.now-qt6-<timestamp>` build whose `metadata.json` names the branch
`transition.now/qt6`, with the registry's size and SHA-256 of `archive.tar`,
the metadata's source revision and dependencies. It reports dependencies
outside the set; toolchain and Vulkan entries are kept. All URLs were checked
(HEAD, sizes match); the checksums are verified by the bootstrap on download.

Result: 87 packages at source revision `4aff6eb` of
`packaging/krita-deps-management` (builds of 2026-10-09). The branch merged
master's dependency updates, so this is a version upgrade, not only a
rebuild: Boost 1.81 → 1.90, Eigen 3.4.0 → 5.0.1, exiv2 0.27.5 → 0.28.8,
FFmpeg 7.1.1 → 8.0.1, expat 2.4.8 → 2.7.5, brotli 1.1.0 → 1.2.0, dlfcn-win32
1.3.1 → 1.4.2, DrMingw 0.9.7 → 0.9.13 and others; Imath is a separate package
(`ext_imath`, needed by `ext_openexr`). Qt stays on KDE's 6.11.0 fork
(`QT_GIT_TAG` `3a13b2d` → `97e5e18`: Wayland and Android fixes; the move to
6.11.3 was reverted upstream). Both revisions were built on the
`krita-windows-clang21` image, matching LLVM-MinGW 20251118. Solstice's
source already handles exiv2 0.28 (`EXIV2_TEST_VERSION(0,28,0)`) and prefers
Eigen 5 (`find_package(Eigen3 5)`, 3.3 fallback). The local
development dependencies are not affected.

## Dependency mirror (2026-10-13)

The user chose a permanent fix after the registry removal above: a mirror
(A) plus a weekly check (B). The Actions cache saved on the default branch
`main` is now shared with other branches as a side effect.

- **Mirror:** public repository `udtren/solstice-deps` (created 2026-10-13).
  One GitHub Release per locked set, tag `deps-<newest build date>-<source
  revision>` (now `deps-20261009-4aff6eb`), assets `<package>-<version>.tar`
  byte-identical to the registry's `archive.tar` (largest: `ext_qt`, 813 MiB;
  the asset limit is 2 GiB). Its README covers provenance and licenses. Its
  workflow "Mirror Solstice dependencies" (`mirror.yml`, `contents: write`,
  its own `GITHUB_TOKEN`, no secrets) takes a Solstice ref, reads that lock,
  downloads each missing package from KDE, checks size and SHA-256 against
  the lock and uploads it, then verifies that every asset is present.
- **Lock:** `mirror` (`repository`, `tag`) and a `mirrorUrl` per package,
  written by `refresh-dependency-lock.py` (a full refresh, or
  `--mirror-only` for the current set). The cache key includes the lock, so
  changing these fields starts a new cache.
- **Bootstrap:** `download()` tries `mirrorUrl`, then the KDE `url`; both
  must match the locked SHA-256. Toolchain and Vulkan SDK still come from
  their original URLs.
- **Weekly check:** `check-dependency-lock.py` HEAD-checks every mirror URL
  (status and size; required) and KDE URL (informational), writes the job
  summary, and the workflow opens or comments on the issue "Windows build
  dependencies: mirror files missing" when the mirror is incomplete.

Refresh procedure: run `refresh-dependency-lock.py`, review the recipe diff
(as above), commit and push, then run the mirror workflow with that ref at
once (KDE keeps builds only for a few weeks), then a build on `main`.

## Python plugins (2026-10-14)

The user found no Python Plugin Manager in the 0.2.0-beta ZIP. Every CI build
until then had configured without Python plugins (pykrita): CMake ran with
setup-python's interpreter, which has neither the dependency packages' SIP
and PyQt6 nor the build-only modules SIP imports. Local builds get them from
`env.bat`'s `PYTHONPATH` (`<install>\lib\site-packages`) and the system
Python's `packaging` and `setuptools`.

- `build-windows.ps1` sets `PYTHONPATH` to `$Deps/lib/site-packages` (the
  dependency packages install SIP, PyQt6, pyqt6-sip and pyqt-builder there,
  built for Python 3.13 like setup-python's).
- The workflow's "Prepare Python build modules" step installs `packaging`
  25.0 and `setuptools` 80.9.0 (the local versions; SIP requires setuptools
  69.5 or newer) from `python-build-requirements.txt`, hash-pinned,
  wheels only. The KDE packages stopped shipping such build-only modules
  (upstream moved them to build dependencies). The `run:` line is quoted:
  `--only-binary :all:` contains `: `, which YAML reads as a mapping.
- Configure fails when `plugins/extensions/pykrita/plugin` is not in the
  build tree (pykrita adds `sip`, `plugin` and `kritarunner` only with
  Python, SIP and PyQt found), so the plugins cannot be lost silently again.
- The packager already copies the dependencies' `lib\site-packages` and
  Krita's `lib\krita-python-libs`.

First check: run <https://github.com/udtren/Solstice/actions/runs/38488208727>
(branch `ci/python-plugins`) configured with the Python plugins.

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
- (Fixed 2026-10-14, see "Python plugins" below.) SIP/PyQt6 were not found with setup-python's interpreter, so Python plugin
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
