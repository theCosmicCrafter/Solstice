# AI Generative Fill — agent development notes

Solstice feature, added 2026-10-29. One-click inpaint of the active
selection via the vendored `ai_diffusion` plugin; result lands as a new
paint layer. Menu: **Select > AI Generative Fill** (action id
`ai_diffusion_generative_fill`).

## Design

The operation is entirely Python-side; no C++ implements the fill.
Reuses the plugin's own generation machinery end-to-end:

1. **Action** — `AIToolsExtension.createActions()`
   (`plugins/python/ai_diffusion/extension.py`) registers
   `ai_diffusion_generative_fill` via `window.createAction(id, "", "Select")`
   and sets the `inpaint-fill` themed icon. Two placement paths cover the
   lifecycle:
   - `krita/krita5.xmlgui` `<Action name="ai_diffusion_generative_fill"/>`
     plugs it into the Select menu at a fixed position (own group after
     the select/deselect block). Plugin `createActions` runs before
     `guiFactory()->addClient` (`KisMainWindow.cpp`, the
     `notifyMainWindowIsBeingCreated` hook exists exactly for this), so
     the action is in the action collection when menus are built.
   - the `menulocation="Select"` property +
     `KisActionManager::synchronizeDynamicActions` covers the case where
     the plugin is enabled mid-session (xmlgui already built) —
     `addExtension` re-syncs existing windows. No duplicate: the sync
     skips actions already present in the menu.
   - Plugin disabled → no action → xmlgui plug is silently skipped → no
     menu entry (graceful absence).
2. **Handler** — `ui/actions.py::generative_fill()` is the thin
   per-document wrapper; `DocumentModel.generative_fill()`
   (`model/model.py`) does the work:
   - `_has_active_selection()` guards on the *live* Krita selection
     (`document.selection()`), not only the 20 ms-polled
     `selection_bounds`. No selection → `report_error` (standard docker
     ErrorBox) + a transient `View.showFloatingMessage`.
   - Not connected → `_connect_then_fill()` runs `root.autostart` (the
     plugin's normal managed-server start/connect, same path as
     kritamcp's `ai_connect`), then fills on success or reports the
     standard connection error.
   - `_enqueue_fill()` saves + sets `inpaint.mode = InpaintMode.automatic`
     (resolves to fill or expand from selection geometry) and
     `strength = 1.0`, calls `_generate(queue_mode, auto_apply=True)`,
     then restores both. The docker's prompt is used as-is.
3. **Auto-apply** — `enqueue_jobs`/`_enqueue_jobs_sweep`/
   `_enqueue_jobs_per_line` take `auto_apply` (default off); the *first*
   enqueued job gets `params.metadata["auto_apply"] = True` (a re-bound
   dict — the shared `params.metadata` is never mutated). `_finish_job`
   checks the flag before the finished-action setting and calls
   `apply_generated_result(job.id, 0, ApplyBehavior.layer)` — forced
   "new layer" apply regardless of the user's apply/replace preference.
   `apply_generated_result` gained an optional `behavior` parameter
   (`None` → `settings.apply_behavior`).
4. **Layer** — `apply_result` → `layers.create(name, image, bounds)` →
   `Document.createNode` + `addChildNode`: a normal undoable paint layer,
   named `[Generated] <prompt> (<seed>)`, including provenance and the
   output post-processing (`matte_output`/`vectorize_output`) like any
   applied result.

## Prompt handling decision

Zero-friction: the docker's current prompt is used. Empty prompt →
`workflow.detect_inpaint` sets `use_reference` → context-conditioned
fill (content-aware). Typed prompt → guided fill. `InpaintMode.automatic`
mirrors the docker's default: edge-touching selections outpaint, inner
selections fill.

## Touch points

- `plugins/python/ai_diffusion/model/model.py` — `generative_fill()`,
  `_has_active_selection()`, `_notify_error()`, `_connect_then_fill()`,
  `_enqueue_fill()`; `auto_apply` threaded through `_generate`,
  `enqueue_jobs`, `_enqueue_jobs_sweep`, `_enqueue_jobs_per_line`;
  `_finish_job` auto-apply branch; `apply_generated_result(behavior=)`;
  `QIcon` import.
- `plugins/python/ai_diffusion/ui/actions.py` — `generative_fill()`.
- `plugins/python/ai_diffusion/extension.py` — `_create_action` gained a
  `menu_location` param; registers the action into `Select` + sets icon.
- `plugins/python/ai_diffusion/ai_diffusion.action` — action entry.
  `activationFlags=10000000000` is binary for
  `ANY_SELECTION_WITH_PIXELS` (0x400; `activationFlags` is parsed base-2
  in `kis_action_manager.cpp`) — the menu item greys without a selection.
- `krita/krita5.xmlgui` — `<Action>` ref in the `Select` menu;
  **version 549→550 + hash bumped in `krita/CMakeLists.txt`** (required
  for every xmlgui edit; hash = MD5 of the whitespace-stripped file).
- No C++ changes; Python only (no rebuild needed for plugin files).

## Decision notes

- `docs/agent/wiki/decisions/xmlgui-unchanged.md` prefers not touching
  `krita5.xmlgui` (a version bump can discard a user's local
  customizations). This task explicitly mandated the xmlgui route —
  same trade-off `layer_to_vector` made — and the `menulocation`
  fallback additionally covers plugin-disabled/mid-session cases the
  xmlgui alone does not.
- kritamcp's `cmd_ai_generate` (`plugins/python/kritamcp/__init__.py`)
  reaches the same machinery for agents; its job-watcher auto-apply
  (`_ai_auto_apply`) predates the `auto_apply` metadata flag — the flag
  is the in-plugin equivalent and could eventually replace the watcher.

## Manual check

1. New document → paint → make a selection → **Select > AI Generative
   Fill**.
2. Managed ComfyUI starts/connects if needed; job runs; a
   `[Generated] …` layer appears when it finishes; Ctrl+Z removes it.
3. Repeat with a prompt typed in the docker → guided fill; empty prompt
   → context fill.
4. No selection → menu item greyed out.
5. With the finished-action setting on Preview, the fill still applies —
   the `auto_apply` flag overrides it.
