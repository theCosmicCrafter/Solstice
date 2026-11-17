# Feature research synthesis — 2026-10-26

User-requested competitive scan across AI-art tools, digital-paint apps, and
animation software. Three research threads; candidates ranked by ROI against
Solstice's existing assets (Vulkan brush engine, brush-stroke layers,
ComfyUI/ai_diffusion, Vision ML, kritamcp, Qwen-Image-Layered).

**Status:** all items assigned to megaplan Phases 9-13 (approved
2026-10-26, `plugin-integration-megaplan.md`); explicit deprioritizations
listed at the end.

## Strategic notes

- **Clip Studio has zero generative AI** (Celsys cancelled its SD palette 2022
  after backlash). Solstice positioning: local-first, opt-in, open-weights,
  no telemetry/no training on user work — marketable differentiator.
- **Affinity v3 (Canva, Oct 2025)** gates AI features behind ~$120/yr — the
  local equivalents are a direct rebuttal.
- **`KisBrushStrokeLayer` is a unique substrate**: recorded stroke data
  (points + pressure + preset + seed) enables parametric stroke editing AND
  true stroke-level tweening that no raster competitor can do.

## Top shortlist (recommended order)

1. **Live painting / realtime co-generation** (Acly "Live" workspace,
   Leonardo Realtime Canvas) — flagship feature users cite; LCM/Hyper-SD
   LoRAs already on disk. Solstice-side: throttle on stroke events →
   regional live preview.
2. **Parametric stroke editing** — stroke-list inspector + swap preset /
   edit stroke data on `KisBrushStrokeLayer`. Unique in market.
3. **Wet-media engine** (Rebelle/Painter) — GPU wetness+diffusion sim;
   only affordable on the Vulkan engine. Phase 2: impasto height.
4. **Spectral pigment mixing** (Mixbox/Kubelka-Munk LUT, opt-in blend
   mode) — small shader, large perceived-quality win.
5. **Instruction-based edit models** — Qwen-Image-Edit 2509/2511 (on disk),
   Flux Kontext (on disk) → "make it winter / fix the text / rotate 45°".
   Unlocks several Photoshop-parity items at once.
6. **Harmonize** (Photoshop) → IC-Light ComfyUI nodes — auto relight/tone
   blend of generated/pasted layers.
7. **Generative upscale** — Ultimate SD Upscale/SUPIR/AuraSR nodes; near
   free, high perceived value.
8. **Ideation surface** ("Generate Similar" grid/infinite canvas of
   variations + prompt mutation) — UI + batch queue only.

## Animation lane (Krita's biggest gaps + AI leverage)

- **High/cheap:** auto-keyframe on draw; 2D camera folder with keyframed
  pan/zoom/rotate; Rhubarb lip-sync (CLI → mouth-shape cels); RIFE
  frame interpolation.
- **High/AI:** Wan 2.2 FLF2V first-last-frame inbetweening (native ComfyUI
  template, models on disk); VACE/EbSynth "paint one frame, propagate";
  SparseCtrl keyframe conditioning.
- **Medium:** ToonCrafter cartoon inbetweening (512×320, ~15 GB VRAM);
  Wan S2V / MiniMax H3 audio-driven performance; Grease-Pencil-style
  mesh puppet (extends Puppet Warp); x-sheet view; TVPaint CTG
  color-guide layers.
- **Note:** MiniMax H3 outputs require MiniMax commercial license.

## Art-side workflow wins (cheap, daily-use)

- Sub View docker (reference panel with eyedropper) — CSP staple.
- Lazy-string stabilization + post-stroke smoothing on recorded strokes.
- QuickShape (stroke-end hold → fitted editable shape).
- Layer Comps (visibility-state snapshots) — CSP 3.0/Photoshop.
- Color-mixing scratch palette + palette extraction from image.
- Decoration/ribbon brushes (image-tip sequences, seeded randomness —
  deterministic under the stroke-layer system).
- Vector line suite: erase-to-intersection, simplify, adjust weight on
  `KoPathShape` (Trace-to-Vector already produces these).
- Comic tooling: word balloons + panel-frame clip folders; extraction/LT
  conversion (photo → lineart + tones layers); Auto Action macro recorder
  (kritamcp already has replayable plumbing).

## AI-side candidates

- On-device LoRA training from document/layer crops (kohya/ai-toolkit
  behind LoRA Manager) — "train from open document" is hard to copy.
- Per-region reference images (IP-Adapter per region, InvokeAI-style).
- Point-drag generative editing (FramePainter lineage) — complements
  Puppet Warp.
- "Suggest next stroke" guidance overlay (SemanticDraw/CoProSketch
  research) — category-first if shipped.
- Distraction auto-detect → inpaint chain (wires/people via Smart Select).

## Explicitly deprioritized

- Full 3D pose/material viewport (CSP) — heavyweight, out of scope unless
  strategic.
- MyPaint infinite canvas — conflicts with fixed-image architecture.
- Runway-class video editing — out of painting-app scope.

## What competitors DON'T have (Solstice could own)

Parametric strokes, stroke tweening, layered generation (already shipped
first via Qwen-Image-Layered), local-first no-telemetry AI, agent
automation (kritamcp — no competitor exposes one).

## Phase assignment map

| Research item | Phase |
|---|---|
| Live co-generation | 10.1–10.2 |
| Parametric stroke editing | 11.3 |
| Wet-media engine | 11.2 |
| Spectral pigment mixing | 11.1 |
| Instruction edits (Qwen-Edit/Kontext) | 9.1 |
| Harmonize (IC-Light) | 9.2 |
| Generative upscale (SeedVR2) | 9.3 |
| Ideation surface | 9.4 |
| Distraction-detect → fill | 9.5 |
| Stroke QoL (lazy-string, QuickShape, smoothing) | 11.4 |
| Sub View / Layer Comps / palettes / balloons / deco brushes / vector lines / LT convert / Auto Action | 11.6 |
| LookFX halftone+paper+separations, flarecore flares | 11.5 |
| Camera folder / auto-keyframe / x-sheet / light table | 12.1 |
| Rhubarb lip-sync | 12.2 |
| RIFE | 12.3 |
| FLF2V inbetweening (Wan i2v, H3 fl2va) | 12.4 |
| VACE / EbSynth propagation | 12.5 |
| Stroke tweening | 12.6 |
| Wan S2V / H3 audio-performance | 12.7 (optional) |
| Train-from-document LoRA | 13.1 |
| Per-region reference images | 13.2 |
| Point-drag edit (FramePainter) | 13.3 (optional) |
| ToonCrafter | 13.4 (optional) |
| Suggest-next-stroke overlay | 10.2 (exploratory) |

## Deprioritized — no phase

- MRT (Canva CVPR'26) — SOTA on paper, no public weights; revisit on release
- PixelART / RevealLayer / RLD — research code, no ComfyUI path
- 3D pose/material viewport (CSP) — full 3D project, out of scope
- MyPaint infinite canvas — conflicts with fixed-image architecture
- Runway-class video editing — out of painting-app scope
- Multi-layer Liquify — minor Puppet Warp extension, fold into 11.4 later if wanted
