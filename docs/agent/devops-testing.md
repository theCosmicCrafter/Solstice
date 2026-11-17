# DevOps: testing strategy — agent reference

Master doc: `devops.md`. Running builds/tests: `development-workflow.md`.
E2E mechanics: `e2e-testing.md`.

## Pyramid

| Layer | Runner | Where it runs | Covers |
| --- | --- | --- | --- |
| Sanity gate | `ci-checks.yml` | Hosted CI (push/PR) | Python compileall, workflow YAML, whitespace, docs links |
| Unit tests | `ctest` / `kis_add_tests` | Local only (`BUILD_TESTING` off in CI) | Parsing, persistence, serialization, pure layout/logic; `TestKoSpectralMixing`, `kis_view_res_vector_compositor_test` precedent |
| E2E | `tests/e2e/` (32 tests, pytest) | Local vs real `solstice.exe` via kritamcp; optional experimental `run_e2e` on hosted builds | Real app behavior: bridge auth, documents/.kra, pixel readback, layers/undo, actions, screenshots, selections/filters, `ai_*` contracts |
| GPU e2e | e2e + live ComfyUI/`librediffusion` | **Local RTX 5090 only** | `ai_generate` full loop, live canvas latency/coherence, Vulkan engine paths |
| Benchmarks | manual runs | Local RTX 5090 | Numbers -> `docs/agent/wiki/benchmarks/` (date, hardware, workload, source) |
| Manual regression | user | Local | Focus, shortcuts, tablet input, canvas rendering, interactive painting — per feature doc's regression list |

## Rules

- **A skipped GPU test is not a pass** (`github-actions.md` rule). When
  reporting test results, state what ran and what skipped and why
  (no ComfyUI, no GPU, attach mode).
- Hosted CI never runs the suite — `windows-build.yml` sets
  `BUILD_TESTING=OFF`; the `run_e2e` dispatch input is experimental and
  covers bridge/core behavior only.
- Add `ctest` unit tests for any new parsing/persistence/serialization
  logic (megaplan Phases 3-5: `.pur` importer, `.af` reader, KTX2 packs,
  C-FFI boundaries, LUT atlas indexer).
- Add e2e coverage for new bridge/`/mcp` actions (`showa_*`,
  `blockout_*`, `capture_at`) — assert observable app state, not mocks.
- Interactive features list their manual checks in the feature doc's
  regression section and in the hand-off note
  (`development-workflow.md` checklist).

## GPU & real-time verification (local)

- Run e2e in **launch mode** (fixture spawns `solstice.exe`, captures
  stderr, refuses if `:5678` is occupied) or **attach mode**
  (`SOLSTICE_E2E_ATTACH=1` — no stderr/crash checks).
- Live-engine work (Phase 3): verify frame coherence (fixed seed,
  mask-delta regeneration), latency targets, and the ComfyUI fallback
  path — record numbers in `wiki/benchmarks/`.
- Shader work (Phase 5): the Sentinel-pattern TDR watchdog sandbox is
  the mitigation for WDDM resets; soak experimental shaders through the
  `capture_at` parameter sweep before exposing them in filters.
- Vulkan/GPU regressions: compare against `wiki/benchmarks/` baselines;
  parity rules for CPU/GPU bit-identity are in
  `wiki/concepts/cpu-gpu-bit-parity.md`.

## License / ledger check

Before closing a megaplan task that vendors code or assets:

- `integration-ledger.md` row exists with source, pin, license.
- License is on the allowed list (`devops-dependencies.md`).
- Proprietary-list items show "concept port — no code copied" in notes.
- `check-docs.py` passes (docs/index integrity).
