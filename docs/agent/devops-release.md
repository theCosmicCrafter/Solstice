# DevOps: release & packaging — agent reference

Master doc: `devops.md`. Version mechanics + history:
`versioning.md`. CI build internals: `github-actions.md`. Website
publish: `versioning.md` "Releasing" (same-day `tidalboxstudio-web`
update, Cloudflare Workers).

## Release channels

| Channel | Built by | Signed? | Used for |
| --- | --- | --- | --- |
| CI trial build | `windows-build.yml` (`workflow_dispatch`) | No — unsigned ZIP, 7-day artifact retention | Verification + source of GitHub Release assets |
| Local GPU build | `build-tools/release/package-local.ps1` on the RTX 5090 box | No | Phases requiring CUDA/TRT (megaplan Phase 3+), GPU-validated packages |
| GitHub Release | manual (`gh release create`) | Assets carry `SHA256SUMS.txt` | `v<version>` public releases (prerelease while version has a label) |

## Runbook (current, manual)

1. **Version bump** — edit `SOLSTICE_VERSION_*` in the top-level
   `CMakeLists.txt`, record under History in `versioning.md`, commit,
   push `main`. **First verify the current value** — `CMakeLists.txt`
   reads `0.2.0-beta.4` but the F:\ packaged build identifies as
   `0.5.0`; reconcile which number the next release continues from
   before bumping (open item below).
2. **Build** — `gh workflow run windows-build.yml --ref main` and wait
   (45-100 min). For GPU-dependent features, run
   `package-local.ps1` instead/alongside.
3. **Verify the artifact** — download, check `SHA256SUMS.txt` digests,
   inner-ZIP CRC pass, required-file checklist (executables, GPU +
   VisionML libs, `platforms/qwindows.dll`, `org.krita.components`
   `qmldir`, `lib/kritaplugins/kritapykrita.dll` + PyQt6 + bundled
   plugins). The 2026-10-04 verified run's checklist in
   `github-actions.md` is the template.
4. **Release** — `gh release create v<version>` at the built commit
   (prerelease while labeled); attach app ZIP, `-dbg.zip`,
   `SHA256SUMS.txt`, `BUILD-INFO.txt`, `windows-dependencies.lock.json`;
   notes follow the previous release's shape (what's new, download,
   highlights, supported environment, build commit, run, SHA-256).
5. **Website same day** — `udtren/tidalboxstudio-web`: bump
   `solstice/src/site.ts` on `develop`, `npm run check && npm run build`,
   verify `dist/`, push `develop`, merge to `main` **after** the
   GitHub Release exists; confirm the live Download page.
6. **Post-release** — `wiki/log.md` entry; `integration-ledger.md`
   status updates for anything the release ships.

## `package-local.ps1` (local GPU/release packaging)

`build-tools/release/package-local.ps1` wraps the CI packaging path for
the dev machine: resolves `env.bat`/`_build`/`_install` (or accepts
explicit `-SrcDir`/`-DepsInstallDir`/`-InstallDir`), runs local-only
`cmake --install`, invokes `packaging/windows/package-complete.py
--no-interactive`, and emits `SHA256SUMS.txt` + `BUILD-INFO.txt` in the
CI layout. Refuses to run while `solstice.exe` is running (DLL locks).
`-SkipBuild` reuses an existing `_install`. It never touches `kritarc`
and never signs anything. First real run is a manual step — validate
against the same artifact checklist as CI before publishing.

### Verified Qt6 package recipe (2026-11-17)

A full self-contained package was built and verified from the Qt6 tree
at `C:\solstice-ci` (deps + llvm-mingw + Vulkan SDK already present):

1. `ninja install` into `C:\solstice-ci\i` (whole tree, incl. PyKrita).
2. G'MIC runtime: `build-tools/gmic/build-runtime.py --work C:\solstice-ci\gmic
   --install C:\solstice-ci\i --deps C:\solstice-ci\deps
   --compiler-bin <llvm-mingw>\bin` — required;
   `package-complete.py` hard-fails without `lib/kritaplugins/krita_gmic_qt.dll`.
3. `packaging/windows/package-complete.py --no-interactive`
   with `--src-dir`, `--deps-install-dir C:\solstice-ci\deps`,
   `--krita-install-dir C:\solstice-ci\i`, `--pre-zip-hook
   build-tools/windows-shellex/add-shellex.py`; env:
   `MINGW_BIN_DIR`, `SEVENZIP_EXE`. Output dir must not exist.
4. Result: 690 MiB zip (8 803 files) + 12 MiB dbg zip + `SHA256SUMS.txt`.

Verified on the extracted copy (no deps on PATH): `Solstice.exe` boots,
Vulkan/GPU healthy on RTX 5090, kritamcp on :5678 + native TestControl
on :5679 merge to 116 tools on `/mcp`, `document_new` + 3 input
scenarios pass, fresh-profile managed-backend bootstrap installs
ComfyUI automatically (`server_mode` defaults `managed`).

AI payloads are deliberately *not* in the zip: managed ComfyUI +
models install on first run into `settings.server_path` (pinned,
hash-checked by the server machinery); the TensorRT bundle resolves at
runtime (`share/krita/livediffusion/bin` if bundled, else
`%LOCALAPPDATA%\Solstice\livediffusion`). AGPL obligations attach to the
engine bundle when it ships, not to the app zip.

## Open decisions (record resolution in `wiki/decisions/` when made)

- **Version reconciliation**: repo `SOLSTICE_VERSION_*` says
  `0.2.0-beta.4`; the packaged build on `F:\solstice-test\...` is
  `0.5.0`. Decide the true current version before the next bump —
  likely the working tree carries an uncommitted bump or the package
  was built with a higher override.
- **Code signing**: packages are unsigned today. KDE-inherited signing
  tooling exists at `build-tools/ci-scripts/sign-windows-package-at-notary-service.py`
  — adopt, buy a cert, or stay unsigned: user's call.
- **Model bundling size**: base GGUFs (~124 MiB) bundled vs heavier
  optional models as a separate downloadable pack — decide when Phase 2
  packages the first `1.1.1`-based build.
- **Installer vs ZIP**: `installer/` payload dir is minimal today
  (`krita.ico`); NSIS/MSIX inputs exist unused upstream-side. ZIP is
  status quo; an installer is its own project if wanted.
