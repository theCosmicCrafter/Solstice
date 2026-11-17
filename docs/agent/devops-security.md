# DevOps: security & compliance — agent reference

Master doc: `devops.md`. Supply-chain specifics:
`devops-dependencies.md`.

## Secrets

- The only credentials this project uses are **local loopback tokens**:
  `KRITAMCP_TOKEN` (kritamcp bridge, `127.0.0.1:5678`) and
  `SOLSTICE_TEST_CONTROL_TOKEN` (upstream testcontrol server). Both are
  user-held, resolved from kritarc/sidecar files — never committed, never
  logged, never in CI.
- No signing keys, no service accounts, no cloud credentials exist in the
  pipeline today. If code signing is adopted (`devops-release.md` open
  item), key handling becomes a new documented decision.
- Never write tokens, cookies, or machine-specific secrets to files,
  commits, PR text, logs, or docs. Docs use placeholders
  (`<repository-root>`, `<krita-dev-root>`); `D:\`/`E:\` asset roots
  appear only as documented local defaults.

## Supply chain

| Control | Where |
| --- | --- |
| Actions pinned by full commit SHA + version comment | all `.github/workflows/*.yml` |
| `contents: read`, `persist-credentials: false`, no secrets in YAML | all workflows; `dependency-check.yml` adds `issues: write` only |
| SHA-256 verified dependency downloads, mirror-first with KDE fallback, no floating versions | `bootstrap-windows.py`, lock `mirrorUrl` |
| Hash-pinned, wheels-only Python build modules | `python-build-requirements.txt` |
| Exact cache keys, no partial fallback, cache only after verified bootstrap | `windows-build.yml` |
| Vendored code pinned by commit + license recorded | `plugin-intake.md`, `integration-ledger.md` |
| No runtime downloads (zero-sidecar mandate) | megaplan decision #4 — removes an entire class of supply-chain risk |

## Bridge / `/mcp` exposure

- kritamcp binds `127.0.0.1` only; `/health` is the sole unauthenticated
  endpoint; `Host`-header enforcement is implemented and e2e-tested
  (`e2e-testing.md`). Keep those invariants when merging
  `TestControlMcpServer` in Phase 2 — the unified `/mcp` stays loopback,
  bearer-authed, env-configured port.
- E2E launch mode refuses to start if `:5678` is already served (no
  driving a foreign instance).
- `/health` and `/source` are the only unauthenticated endpoints —
  `/source` returns the AGPL §13 source-availability notice.
- `_resolve_allowed_path` confines file params to allowed roots
  (canvas output, templates, doc dir, user Docs/Pictures, temp,
  `KRITAMCP_ALLOWED_ROOTS`, kritarc `allowed_roots`). `for_write=False`
  additionally requires the file to exist (kills the existence-oracle).
  Filename components joined *after* validation must be char-filtered —
  the `export_layers` `prefix` was a confirmed escape, fixed 2026-11-16.
- Residual boundary, by design: any same-user process can read the
  kritamcp token file → full action surface (incl. `trigger_action`,
  `ai_install` downloads — catalog-pinned only). Loopback automation
  bridges are inherently same-user trust; document, don't gate.

## Native engine boundary (librediffusion)

- In-process inference means engine bugs land in solstice.exe — the
  `KleinStream` calls run under a `threading.Lock` against `close()`,
  C-side return counts are clamped before slicing (`rife_frames`), and
  every native failure degrades to ComfyUI instead of stalling Live.
- ctypes rule verified by a shipped bug: pass the buffer object to C
  output params, never `buffer.raw` (fresh copy → freed → zeros + UAF).
- `native_engine_dir` selects engine *data files* only — the DLL search
  path is fixed (`SOLSTICE_LIBREDIFFUSION_DIR`, install dir, appdata).
  A hostile local config can point TRT's deserializer at chosen `.plan`
  files — same-user trust boundary, documented.
- TRT `.plan` files are version-locked to the linked runtime
  (11.0.0.114) — see `native-live-engine.md` for the rebuild recipe.

## License & provenance compliance

- Allowed: GPLv3-compatible (`MIT`, `Apache-2.0`, `MIT OR Apache-2.0`,
  GPL); CC BY 4.0 test media with attribution.
- Barred: `CC BY-NC` (mixbox precedent), proprietary code/assets/brands
  — the zero-copy list lives in `integration-ledger.md` ("Proprietary
  zero-copy list"). Concept ports reimplement behavior clean-room.
- Unlicensed sources get verified before any port; if unverifiable, port
  the published algorithm only (`devops-dependencies.md`).
- `.pur` import compat is format-level reverse engineering for
  user-owned files — importer only, never the app or its assets.

## Review & audit habits

- Review `git diff --cached` before committing; stage specific paths —
  the tree carries ~143 user-owned dirty entries.
- New workflows: read-only permissions, SHA pins, YAML parsed locally
  before push; first run verified in the Actions tab.
- Any request to disable a checksum, widen permissions, add secrets, or
  relax the zero-sidecar rule is escalated to the user, not worked
  around.
