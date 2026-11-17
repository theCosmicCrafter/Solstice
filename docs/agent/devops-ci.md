# DevOps: continuous integration — agent reference

Master doc: `devops.md`. Detailed workflow internals: `github-actions.md`.

## Runner policy (decision, 2026-11-15)

GitHub-hosted runners are **CPU/compile-gate only**. They have no GPU and
no CUDA/TensorRT toolchain. CUDA/TRT builds (`librediffusion`), GPU e2e
runs and release packaging execute **locally on the RTX 5090 machine**
(`sm_120`); see `devops-environments.md` and `devops-release.md`. No
self-hosted runner is registered; revisit if a second contributor needs
reproducible GPU builds (`wiki/decisions/local-gpu-builds.md`).

## Workflow inventory

| Workflow | Trigger | Job | Permissions | Purpose |
| --- | --- | --- | --- | --- |
| `.github/workflows/windows-build.yml` | `workflow_dispatch` (manual only; optional `run_e2e` input) | Full deps bootstrap -> Release configure -> compile -> install -> `package-complete.py` -> ZIP + `SHA256SUMS.txt` + `BUILD-INFO.txt` artifact (7-day retention) | `contents: read` | Reproducible unsigned Windows x64 package; feeds GitHub Releases |
| `.github/workflows/dependency-check.yml` | Weekly Mon 03:17 UTC + `workflow_dispatch` | HEAD-checks every `mirrorUrl` (required) and KDE `url` (informational) in the dep lock; opens/comments issue "Windows build dependencies: mirror files missing" | `contents: read`, `issues: write` | Dependency-mirror health |
| `.github/workflows/ci-checks.yml` | `push` to `main`, `pull_request`, `workflow_dispatch` | Fast sanity gate — see below | `contents: read` | Catch syntax/docs breakage before a 60-90 min build is wasted |

Conventions all workflows follow: SHA-pinned official actions (pinned
commit + version comment), `persist-credentials: false` on checkout,
`contents: read` unless a job writes issues, no secrets, no cache writes
outside `windows-build.yml`'s dependency cache, `windows-2022`,
PowerShell `pwsh` default shell.

## `ci-checks.yml` sanity gate

Budget ~5 minutes. Jobs:

1. **Python syntax** — `python -m compileall` on `plugins/python/` and
   `tests/e2e/` (the vendored pykrita plugins and the e2e suite are the
   only shipped Python).
2. **Workflow YAML** — parse every `.github/workflows/*.yml` with PyYAML.
3. **Whitespace** — `git diff --check` against the base ref (PR) or
   `HEAD~1` (push).
4. **Docs index** — `python build-tools/github-actions/check-docs.py`:
   every `docs/agent/*.md` path referenced by `AGENTS.md` exists, and
   relative Markdown links inside `docs/` resolve. Exits 1 on any broken
   reference. The same script is runnable locally before committing docs.

This workflow intentionally does **not** compile C++, run tests, cache
anything, or touch credentials. The first run after landing should be
verified in the Actions tab; a manual `workflow_dispatch` run is the
fastest check.

## `windows-build.yml` notes

- Manual-only by design: builds take 45-100 min and produce the artifact
  used for releases; there are no push/PR triggers and none should be
  added (a broken tree is caught by `ci-checks.yml` + local builds).
- `run_e2e: true` is **experimental**: it executes `tests/e2e/` against
  the packaged build on a hosted runner with no GPU and no ComfyUI —
  GPU-dependent tests skip. Use it for bridge/core-behavior coverage,
  never as AI/GPU verification.
- Configure must record `HAVE_KRITA_GPU_ENGINE=ON`,
  `KDE_INSTALL_USE_QT_SYS_PATHS=ON`, `KDE_INSTALL_QMLDIR=qml`, and the
  `plugins/extensions/pykrita/plugin` build-tree entry, or the job fails
  fast (see `github-actions.md`).
- Cache policy: exact cache key over the dep lock + bootstrap script;
  no partial-key fallback; cache saved only after a complete bootstrap.

## Adding or changing CI

- New workflows stay read-only (`contents: read`) and secret-free unless
  the user approves otherwise in writing (record in `wiki/log.md`).
- Pin every action by full SHA with a `# vX.Y` comment.
- Anything that needs a GPU, CUDA, TensorRT, ComfyUI, or `D:\` local
  assets belongs in the local pipeline (`package-local.ps1`,
  `tests/e2e` on the dev machine), not in hosted YAML.
- After editing any workflow file: parse it locally
  (`check-docs` step's YAML job does this in CI too), then dispatch a
  manual run and follow the result — `github-actions.md`'s "Verification
  and operation" rules apply (a skipped GPU test is not a pass).
