# DevOps: dependency & supply-chain management — agent reference

Master doc: `devops.md`. Toolchain pins: `devops-environments.md`.
Intake provenance: `integration-ledger.md`, `plugin-intake.md`,
`upstream-intake.md`.

## Layers

| Layer | What | Pinned how | Checked by |
| --- | --- | --- | --- |
| KDE dependency packages | 87 Windows packages (Qt 6.11 fork, exiv2, FFmpeg, Eigen, …) | `build-tools/github-actions/windows-dependencies.lock.json`: URL, size, SHA-256, source recipe revision, `mirrorUrl` | `bootstrap-windows.py` verifies on download; weekly `dependency-check.yml` HEAD-checks the mirror |
| Toolchain (CI) | LLVM-MinGW 20251118, Vulkan SDK 1.4.357.0 | entries in the same lock | bootstrap installs (`copy_only=1` for the SDK) |
| Python build modules | `packaging`, `setuptools` for SIP | `python-build-requirements.txt` (hashes, wheels only) | pip `--require-hashes` |
| Toolchain (local) | CUDA, TensorRT, Rust+cbindgen, glslc/dxc, plus the CI tools | `build-tools/toolchains/windows-dev.lock.json` | `check-local-toolchains.py` (report-only) |
| Vendored code | Python plugins, Rust crates, shader ports | pinned commit/version + license in `plugin-intake.md` / `integration-ledger.md` | re-check cadence in ledgers |
| Model/asset payloads | GGUFs, KTX2 packs, `.pur` boards, wildcards, LUTs, poses | bundled vs auto-discovered policy below | manual/in-app indexing |

## Dependency lock & mirror (recap)

The KDE registry keeps only recent builds — on 2026-10-13 all 86 locked
packages 404'd mid-run. The permanent fix: mirror repo
`udtren/solstice-deps` (one GitHub Release per locked set, tag
`deps-<date>-<rev>`, byte-identical `archive.tar` assets, 2 GiB asset
limit) + `mirrorUrl` per package + the weekly check workflow.

Refresh procedure (from `github-actions.md`):

1. `python build-tools/github-actions/refresh-dependency-lock.py` —
   regenerates the lock from the registry API (newest
   `transition.now-qt6-*` build per package), writes `mirror` +
   `mirrorUrl` fields.
2. Review the recipe diff (version upgrades ship inside rebuilds — e.g.
   Boost 1.81->1.90, FFmpeg 7.1.1->8.0.1 in the 2026-10-13 refresh).
3. Commit + push, then run the mirror workflow with that ref **at once**
   (KDE keeps builds only a few weeks), then a `windows-build.yml` run.

Never bypass a checksum, never silently fall back to a floating version.
Cache key covers lock + bootstrap script; incomplete cache fails rather
than mixing dependency sets.

## Vendored / adopted third-party code

- **Python plugins**: `plugins/python/` lane — pinned commit, license,
  PyQt6 status and local patches in `plugin-intake.md`.
- **Rust crates / shader ports** (megaplan Phases 3-5): `MIT OR
  Apache-2.0` storytold crates via static C-FFI (`cbindgen`) or
  clean-room WGSL->GLSL ports. Every adopted crate gets an
  `integration-ledger.md` provenance entry: source path/repo, pin,
  license, integration path. No `egui`, no `-cli` sidecars, no
  `docs/brand/` assets.
- **Upstream Solstice `1.1.1`**: selective rebase of `ac468d9`; the
  fork-only file list in `rendering-art-engine-megaplan.md` is the
  merge checklist.
- **Upstream Krita**: no merges — weekly Hermes reports, user-approved
  cherry-picks, ledger in `upstream-intake.md`.

## License policy

- Allowed into the tree: GPL-compatible licenses — `MIT`, `Apache-2.0`,
  `MIT OR Apache-2.0`, GPL itself (Krita heritage). **AGPLv3 accepted for
  `librediffusion`** (user decision 2026-11-15 — the combined binary is
  AGPLv3; ship its `LICENSE`, mark modifications, §13 source-notice on
  the `/mcp` surface). CC BY 4.0 media only
  as test assets with attribution (`glitchVids`).
- Barred: `CC BY-NC` (mixbox precedent — concept reimplementation only),
  any proprietary code/assets/branding (zero-copy list in
  `integration-ledger.md`), `docs/brand/` trademark material.
- When vendoring, record the SPDX identifier seen at pin time in the
  ledger. Unlicensed sources (e.g. `ComfyUI_MPEG_Diffusion_Suite` ships
  no LICENSE) must be verified **before** any code port — port the
  published algorithm clean-room if the license can't be confirmed.

## Model & asset payloads

- **Bundled** (inside the package): the 3 base VisionML GGUFs
  (~124 MiB total) copied from `Solstice-1.1.1-windows-x64`, plus any
  KTX2 asset packs the LookFX/Flarecore port produces (built at package
  time, not fetched at runtime).
- **Auto-discovered, never required**: `D:\models`, `D:\wildcards`,
  `D:\models\Poses`, `D:\models\LUTs`, `D:\krita\ComfyUI`,
  `E:\AI_Art\PureRefBoards` — indexed when present (megaplan decision #4);
  the app must fully function without them.
- **No runtime downloads**: zero-sidecar rule — anything the app needs
  ships in the package or is user-provided local content.
