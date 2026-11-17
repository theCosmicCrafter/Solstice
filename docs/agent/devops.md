# DevOps — agent reference (master)

Solstice DevOps: how the project builds, tests, packages, releases, and
keeps its supply chain honest. Written 2026-11-15 alongside the
`rendering-art-engine-megaplan.md` — that megaplan's toolchain and
release requirements are covered here.

## Topology

| Piece | Location | Role |
| --- | --- | --- |
| This repo | `github.com/theCosmicCrafter/Solstice` (origin), branch `main` | Source, workflows, docs, `tests/e2e` |
| Dependency mirror | `github.com/udtren/solstice-deps` | Release-asset mirror of KDE build packages (lock-driven) |
| Website | `github.com/udtren/tidalboxstudio-web` -> Cloudflare Workers -> `solstice.tidalboxstudio.com` | Download page; updated same day as each release |
| Upstream intake | `\\192.168.1.37\share\upstream-krita-check` (DGX Spark share) | Weekly Hermes reports -> approved cherry-picks (`upstream-intake.md`) |
| Local GPU / release machine | RTX 5090 dev box (`sm_120`), `D:\`/`E:\` asset roots | CUDA/TRT builds, GPU e2e, local packaging |

## Policy summary

- **Hosted CI = gates, not truth**: compile/sanity checks + reproducible
  unsigned builds. GPU builds, GPU e2e and release packaging are local.
- **Manual-only build workflow**: `windows-build.yml` runs on dispatch;
  `ci-checks.yml` gives push/PR a ~5-min sanity gate.
- **Reproducibility over speed**: SHA-256-pinned deps, mirror fallback,
  exact cache keys, hash-pinned Python modules, SHA-pinned actions.
- **Zero-sidecar**: nothing the app needs is fetched at runtime; third-
  party capability arrives as vendored source, C-FFI, or shader ports
  (`integration-ledger.md` tracks all of it).
- **Unsigned artifacts today**: ZIP + `SHA256SUMS.txt` is the contract;
  signing is an open decision (`devops-release.md`).

## Document index

| Doc | Covers |
| --- | --- |
| `devops-ci.md` | Runner policy, workflow inventory, `ci-checks.yml` spec, CI edit rules |
| `devops-environments.md` | Environment matrix, toolchain pins, local asset roots, env vars |
| `devops-dependencies.md` | Dep lock/mirror, vendored-code provenance, license policy, model/asset payloads |
| `devops-testing.md` | Test pyramid, GPU/e2e rules, license-ledger check |
| `devops-release.md` | Release channels, runbook, `package-local.ps1`, open decisions |
| `devops-security.md` | Secrets, supply-chain controls, `/mcp` exposure, compliance, audit habits |

## Adjacent docs (pre-existing, authoritative in their area)

- `github-actions.md` — CI workflow internals, dep-lock history, mirror
  mechanics, verified runs
- `development-workflow.md` — build/test/install/hand-off commands
- `versioning.md` — Solstice vs Krita-compat version, bump + release
  steps, history
- `e2e-testing.md` — `tests/e2e/` suite mechanics and coverage
- `upstream-intake.md` — upstream Krita cherry-pick procedure + ledger
- `plugin-intake.md` — vendored plugin ledger
- `integration-ledger.md` — megaplan item status/provenance ledger
- `rendering-art-engine-megaplan.md` — the roadmap these docs support
- `test-builds.md` — user-facing trial-build guide (`docs/test-builds.md`)

## First-run checklist for a new machine

1. `python build-tools/toolchains/check-local-toolchains.py` — report
   what satisfies `windows-dev.lock.json`.
2. `env.bat` present with `_build`/`_install` (see
   `development-workflow.md` placeholders).
3. For CI parity: nothing local needed — hosted runners self-provision.
4. For releases: `build-tools/release/package-local.ps1` dry-run after
   step 1 shows the base toolchain complete.
