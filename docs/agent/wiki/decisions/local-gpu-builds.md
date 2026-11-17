---
type: decision
updated: 2026-11-15
sources: docs/agent/devops-ci.md, docs/agent/devops-environments.md, docs/agent/rendering-art-engine-megaplan.md
related: decisions/upstream-selective-intake.md
---

# Hosted CI for gates only; GPU builds and releases stay local

**Decision (user, 2026-11-15):** GitHub-hosted runners remain
CPU/compile-gate only. CUDA/TensorRT builds (`librediffusion`,
`sm_120`), GPU-dependent e2e runs and release packaging execute locally
on the RTX 5090 machine. No self-hosted runner is registered.

**Why:** hosted `windows-2022` runners have no GPU and no CUDA/TRT
toolchain, so they cannot compile or verify the megaplan's Phase 3+
work; a self-hosted runner would hand CI machine access to a queue and
require the box to stay online for marginal gain — the project's release
flow is already manual and single-machine. The dependency mirror +
`windows-build.yml` already give reproducible CPU-side packages.

**Consequences:** `ci-checks.yml` is the only push/PR gate (~5 min,
read-only); `windows-build.yml` stays manual-dispatch; Phase-gated
toolchain pins live in `build-tools/toolchains/windows-dev.lock.json`
and are checked with `check-local-toolchains.py`; local packaging uses
`build-tools/release/package-local.ps1`. Revisit if a second
contributor needs reproducible GPU builds or the box becomes a release
bottleneck.
