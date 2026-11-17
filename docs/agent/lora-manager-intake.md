# ComfyUI-Lora-Manager intake — survey and integration status

Status: quick wins shipped (2026-10-27); recipes, bulk sync and badges remain a
menu. Survey of the user's local checkout of
[ComfyUI-Lora-Manager](https://github.com/willmiao/ComfyUI-Lora-Manager)
(MIT) against the vendored `plugins/python/ai_diffusion` (Acly's plugin stays
the single integration; no second ComfyUI backend is added — per project
policy).

## Already had (verified in the vendored tree)

- Safetensors header + sidecar reading: `backend/modelspec.py::extract_sidecar_metadata`
  merges modelspec + Kohya `ss_*` metadata and Lora-Manager `.metadata.json`
  sidecars (trigger words, base model, civitai ids).
- Persistent metadata cache: `backend/files.py::ModelMetadataCache`; merged
  metadata drives `File.metadata`/`lora_triggers`.
- Prompt injection: `backend/workflow.py` auto-appends `lora_triggers`;
  `ui/autocomplete.py` offers them on `<lora:…>` completion.
- Model browser: `ui/model_browser.py` card grid with previews and opt-in
  CivitAI identify; `backend/civitai.py` hash/search client.
- Catalog: `presets/models.json` already carries `sha256`, `license`,
  `min_vram_gb` on entries; downloads hash-verify existing files before
  skipping (`backend/server.py::_missing_or_invalid`).

## Shipped 2026-10-27 (quick wins)

- **`.civitai.info` sidecar fallback** — `backend/modelspec.py::extract_sidecar_metadata`
  now also reads third-party `<name>.civitai.info` files
  (`trainedWords`, `baseModel`, `modelId`) alongside `.metadata.json`, so
  libraries managed by other tools still enrich `lora_triggers`.
- **Duplicate-download detection** — `ui/model_browser.py::_download` warns
  (with Show in Folder) when the identified CivitAI `version_id` already
  exists in local loras/checkpoints metadata instead of downloading again.
- **Provenance in sidecars** — `_write_civitai_sidecar` persists
  `nsfwLevel`, `pageUrl`, `downloadUrl`, `previewUrl` for offline provenance.
- **Folder filter** — `ModelBrowserFilter.folder` + dropdown groups/filter by
  `Path(file.id).parent`.
- **`last_used` tracking** — selecting a LoRA in a style (`ui/style.py::LoraItem`)
  writes `last_used` via `FileCollection.set_meta`; the browser's recent sort
  prefers it over mtime.
- **Update-available hint** — the unused `CivitaiClient.check_for_updates` is
  wired into the identify/details panel; opt-in (`settings.civitai_lookup`)
  as before. No card badges.

All remain local-first: no new default-on network behavior. Validation:
55 ai_diffusion unit tests pass.

## Model-catalog audit 2026-10-27 (D:\models, ~3.57 TB)

- All 16 sample-hashed catalog files verified OK except the SeedVR2 pair,
  whose on-disk sha256 (`5aa0d25f…` DiT, `20678548…` VAE) matched the current
  HF LFS oids — the catalog's recorded hashes were stale (convrot re-pack).
  Updated in `presets/models.json`; files are good, no re-download needed.
- Download path bug fixed: an existing-but-invalid file (hash mismatch)
  previously made `_download_cached` skip the download and
  `_verify_downloaded` delete + abort. `Server.download` now removes the
  corrupt target first (same pattern as `fix_models`) so a fresh file is
  fetched and verified.
- Known gaps left as-is (documented): files under `alternatives[]` are
  accepted without a hash; `find_missing` is existence-only by design
  (cheap startup check; installs still verify); bundle archives (Python,
  ComfyUI) are skip-if-exists without verification; ~900 on-disk community
  models (MOSS blends, Wan video, Flux finetunes, Z-Image variants, …) are
  intentionally outside the catalog, which covers curated presets only.

## Not adopted

- Second backend / aiohttp service layer / WebSocket broadcasts — duplicates
  the existing ComfyUI client and Qt UI.
- AutoV2/AutoV3/Blake3 hash suite — full SHA256 already feeds CivitAI
  `by-hash`; `_find_matching` accepts autov2 prefixes from metadata.
- Auto-organize (moving user files) — risky in a plugin; folder layout is
  exploited read-only only.

## Remaining menu — detailed spec (2026-10-27)

All three items share one dependency: `modelId`/`versionId` persisted in
the sidecar metadata written by the shipped intake (quick wins already
store sha256 + CivitAI `versionId` where resolvable).

### L5a — Bulk metadata refresh (M)

Single pass over the model index that re-resolves stale sidecars:
- Enumerate indexed files missing `modelId` or flagged `stale`.
- Batch CivitAI `by-hash` lookups (serialised, rate-limit-aware —
  reuse `backend/civitai.py` client, honour its retry/backoff).
- Rewrite sidecars in place; emit a summary (N updated, M still unknown).
- Surface as a `Lora Manager: Refresh Metadata` action in the managed
  ComfyUI runtime + a progress row in the models docker.
- Cost: M. Mostly orchestration — no new data model.

### L5b — Update badges (M)

Show "update available" on model cards:
- Requires stored `modelId` (L5a backfills it).
- On index load, for files with `modelId`: query CivitAI
  `models/{id}` → compare latest `versionId` vs stored.
- Badge the card + a filter chip "updates available"; clicking offers
  download-to-same-folder with hash verification (reuse the shipped
  verified-download path) and keeps the old file (`.bak` sidecar note).
- Do NOT auto-update in place — user files must never be replaced
  silently (same risk class as auto-organize).
- Cost: M; ~1 extra CivitAI call per known model, cacheable.

### L5c — Recipes: image → prompt + LoRA reconstruction (L)

Highest-value deferred item. Given a dropped/generated image:
1. Read PNG/JPEG metadata (ComfyUI/A1111-style `parameters`,
   `prompt`, `workflow` chunks) — parser for the common formats.
2. Extract LoRA names + strengths + trigger words from the prompt
   block / workflow nodes.
3. Resolve each name against the LOCAL index via `files.find`
   (name/alias/hash match, already shipped).
4. Present: reconstructed prompt text, matched local LoRA files,
   *missing* ones with CivitAI links (via stored `modelId` or
   hash search) — one click to open the model page.
5. "Send to generation" pre-fills the ai_diffusion prompt + LoRA
   stack; works entirely offline except step-4 lookups.

Boundary: image → recipe, not image → image. No model files are
moved or modified. Drag-drop target in the LoRA docker plus a
`Lora Manager: Inspect Image Recipe` action.

Cost: L (parser + matching UI). Most logic is Python-side testable
without a running ComfyUI.

### Ordering

L5a → L5b (badges need modelId) → L5c independent; L5c is the
biggest UX win and can start in parallel since it only needs the
shipped index/find path.

## Sources

- Intake analysis session 2026-10-27 (this checkout, read-only).
- Implementation + audit sessions 2026-10-27 (files cited inline).
