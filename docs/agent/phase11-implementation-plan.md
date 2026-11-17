# Phase 11 implementation plan — art-side native features

Research-backed build plan for the "paint feels real" phase. Written
2026-10-27 after direct source research (papers, repos, licenses); this is
the plan to execute against — not a wish list.

Substrate: Solstice already owns `libs/gpu/` (`kritagpu`, Vulkan 1.3
compute, GLSL→glslc at build time), `libs/image/gpu/`
(`KisGpuProjectionCompositor`, `KisGpuMergeBatch`, GPU tile backend), and
`libs/ui/KisBrushStrokeLayer.*` (recorded strokes, `renderStrokes`,
`KisRecordedBrushStroke` job lists). Phase 11 builds on all three.

---

## 11.1 Spectral pigment mixing (Kubelka–Munk)

**Status (2026-11-02): first working slice implemented — see
`docs/agent/spectral-mixing.md`.** The slice took a cheaper path than
the LUT plan below: spectral.js v3 decomposes RGB→reflectance in
*closed form* (7 virtual base pigments), so no offline LUT or runtime
solve is needed. `KoSpectralMixing` does the math on CPU;
`KoCompositeOpPigment` (`COMPOSITE_PIGMENT = "pigment"`, Mix category)
is registered for every RGB/BGR colorspace, which makes it available
as a layer blend mode, a brush "Blending Mode", and the Color Smudge
"Composite Op" (color-rate) option — blue over yellow composites to
green. Unit test `TestKoSpectralMixing` passes 7/7. Remaining from the
plan below: the GPU kernel (`composite_pigment.comp`) + merge dispatch,
optional CPU LUT for speed, a dedicated smudge "pigment mix" checkbox,
and golden-image/manual paint verification.

**Goal**: opt-in "Pigment" composite mode — blue + yellow → green, not
gray — for layer blending first, brush color-mixing second.

### License finding (decides the implementation)

- **Mixbox is CC BY-NC 4.0** — non-commercial only, no sublicensing.
  Incompatible with GPL/Krita (confirmed: scrtwpns/mixbox LICENSE +
  issue #15 unresolved since 2023). **Do not include mixbox code or
  `mixbox_lut.png`**. The *concept* (RGB↔latent pigment space) is
  published science (Sochorová & Jamriška, mixbox.pdf) — implement our
  own.
- **spectral.js (rvanwijnen, MIT)** — Kubelka–Munk mixing library
  implementing Scott Allen Burns' method (arXiv 1710.06364): RGB →
  representative spectral reflectance curve, mix curves, → RGB via CIE
  integrals. Free to port.
- Scott Burns' published method is itself the clean algorithm: find a
  plausible spectral curve per RGB, mix by weighted geometric mean.

### Architecture

Two-stage, all-GPU-friendly:

1. **Offline LUT build (Python, one-time, committed)**: sample the
   sRGB cube on a 25³–33³ grid; for each RGB solve the spectral.js /
   Burns optimization → 36-sample (or 8–10 basis) spectral reflectance;
   pack into a 3D texture (`spectral_lut.ktx`/bin + generator script in
   `libs/gpu/luts/`). No runtime optimization — LUT lookup is one
   trilinear sample.
2. **Composite kernel (GLSL compute)**: for src/dst pixels → LUT → two
   spectral vectors → mix (weighted geometric mean by src coverage) →
   evaluate mixed spectrum → XYZ via stored 36×3 CMF matrix → linear
   sRGB → encode. Constant-time per pixel; the whole op is ~30 ALU + 2
   texture reads.

### Touch points

| Layer | Files | Work |
|---|---|---|
| Blend-mode registry | `libs/pigment/compositeops/KoCompositeOpRegistry.*`, colorspace's composite-op id list | new `COMPOSITE_PIGMENT` id |
| CPU fallback path | `plugins/paintops/defaultpaintops` / pigment compositeop dirs | scalar/xsimd variant of same math (LUT is shared data) |
| GPU merge | `libs/gpu/shaders/` (new `composite_pigment.comp`), `KisGpuMergeBatch`/`KisGpuProjectionCompositor` dispatch table | dispatch by composite id |
| Smudge/brush mixing (phase 11.1b) | `plugins/paintops/defaultpaintops` color-smudge option | optional "pigment mix" checkbox → same LUT path inside the dab composite |
| Resources | `libs/gpu/luts/spectral_*.bin` + `tools/build_spectral_lut.py` | committed LUT, regenerable |

### Testing

- Unit: C++ test feeding known pairs (blue+yellow→green hue check via
  OKLab hue angle; white+X→X-ish; black+X→dark X), LUT round-trip error
  < 2 ΔE average.
- Golden-image test: gradient strip mixed under Normal vs Pigment —
  assert hue shift + saturation preserved (catches "still doing sRGB
  lerp" regressions).
- Manual: paint blue wash, yellow over → green; report parity vs
  spectral.js web demo on same swatch pairs.

### Risk

- LUT fit quality: Burns' method picks *a plausible* spectrum, not the
  true pigment's — out-of-gamut RGBs get representative curves. 33³ +
  finer near neutrals if banding shows.
- 36 channels → XYZ in shader is fine; alpha compositing stays
  standard Porter-Duff (mix only affects RGB channels).

---

## 11.2 Wet-media engine (Vulkan compute)

**Goal**: real watercolor — wetness field, pigment advection, edge
darkening, backruns, granulation — not a brush texture trick.

### Research basis

- **Curtis et al. 1997, "Computer-Generated Watercolor"** — the
  canonical, implementable model: three coupled layers —
  (a) shallow-water surface (water + suspended pigment flow),
  (b) pigment deposition (pigment settles onto paper; adsorb/desorb),
  (c) capillary layer (moisture in paper pores → backruns). Edge
  darkening = perimeter water loss carrying pigment outward. Open
  reference implementations exist (`inchkev/watercolor`, Curtis-model
  ports; Ollin documents the same architecture commercially).
- **MoXi (Chu & Tai 2005) + 2008 watercolor extension** — lattice
  Boltzmann alternative; adds backrun/granulation/edge-darkening
  coverage. LBM is compute-shader native (proven: `jinleili/fluid-webgpu`
  runs LBM pigments-diffuse at 60fps on iPhone).
- Field layout reference: `pigment-drift-generator` keeps *pigment
  amounts* (not shares) + wetness per pixel — carry-along math stays
  linear.
- Glazing/layering renders via Kubelka–Munk → reuses 11.1's kernel.

### Architecture

Per wet-capable layer, persistent GPU side-channel buffers (owned by a
`KisWetMediaState` hung off the layer, parallel to its paint device):

- `water` (R16F), `pigment[0..N]` (RGBA16F ×2 = 8 spectral basis
  slots), `deposited` (same shape), `paper` (shared global texture —
  heightfield tooth from a paper preset)
- Dab writes: brush deposits `water += w`, `pigment += c` at dab
  footprint (compute pass, same dispatch stream as the existing GPU
  dab path).
- Sim step: ping-pong compute passes — (1) water/pigment advection +
  diffusion (5-point shallow-water or D2Q9 LBM), (2) deposit/adsorb,
  (3) evaporation + edge shedding, (4) capillary backrun. N steps per
  tick while `water.max > ε`; scheduled as an idle-time updater job so
  paint keeps flowing after the stroke (the signature watercolor
  behavior).
- Render: deposited + suspended pigment → K-M composite → layer's
  paint device (or a custom `KisWetLayer` type if side channels prove
  cleaner than a parallel device).

### Phasing (each phase is a manual-verification gate)

1. **Prototype gate** (decides GO/NO-GO before integration):
   standalone `wetmedia_demo` target — one 1024² field, scripted dab
   stamps, all four passes, dump PNGs. Verify: wet edge darkening,
   backruns on rewetting, granulation with per-pigment rates. If it
   doesn't look like watercolor here, it won't inside Krita.
2. **Layer integration**: `KisWetMediaState` on `KisPaintLayer` (opt-in
   "wet" flag), dab hook in the GPU stroke path, sim job in
   `KisUpdaterContext`, undo = paint-device-level (state buffers are
   derived/transient — document: undo restores pixels, not sim state).
3. **Brush controls**: water load, paint load, dry time, granulation
   per preset → paintop options.
4. **Impasto phase 2**: height field + normal-mapped lighting in the
   render pass (research: Rebelle NanoPixel-style; deferred).

### Touch points

- `libs/gpu/shaders/wet_*.comp`, `libs/gpu/` buffer helpers
- `libs/image/KisWetMediaState.{h,cpp}` (new)
- `libs/ui/tool/strokes/kis_painter_based_stroke_strategy.cpp` (dab
  deposit hook beside existing GPU prefetch)
- `libs/image/kis_updater_context.cpp` (sim job scheduling)
- `plugins/paintops/defaultpaintops` (wet-brush options page)

### Risk

- Biggest item in the phase. The prototype gate exists precisely so we
  fail cheap.
- Memory: 1024² wet layer ≈ 5 RGBA16F planes ≈ 40 MB — fine;
  scale with tile-size subregions if needed.
- Undo semantics are the subtlety — sim state is continuous, so undo
  restores the rendered pixels and *truncates* the sim (accepted
  design: wet layers are "commit when dry").

---

## 11.3 Parametric stroke editing (unique in market)

**Goal**: stroke inspector docker + edit ops over
`KisBrushStrokeLayer`'s recorded strokes — a feature no raster
competitor has.

### Design (no external research needed — substrate exists)

`KisRecordedBrushStroke` already stores: job list (POINT/LINE/CURVE +
`KisPaintInformation` + beziers), preset ref, fg/bg, compositeOp,
pattern/gradient, per-mirror `Start` dab anchors, seed; and
`renderStrokes()` replays any stroke set through the brush path.

Build:

1. `KisStrokeEditCommand` (KUndo2Command): replace/remove/transform
   one stroke in `m_strokes`, then `renderStrokes()` → layer device.
2. Stroke inspector docker (`plugins/dockers/strokeinspector/`): list
   strokes (time, preset, length, point count); per-stroke edits —
   swap preset, scale size, tint color, translate; live re-render.
3. libkis exposure `Node.strokeCount()/editStroke(i, ops)` → bridge
   `ai_strokes` (agent can list/edit strokes — unique automation
   surface).

Risk: low-medium (re-render determinism depends on recorded seeds —
already proven by the transform-redraw path).

---

## 11.4 Stroke QoL pack

- Lazy-string pulled stabilization (existing stabilizer infra +
  distance-pulled anchor), post-stroke re-fit on recorded point stream
  (operates on `KisBrushStrokeLayer` data — free with 11.3),
  QuickShape: stroke-end hold → fitted editable line/ellipse/rect
  (`KoPathShape` insertion or stroke replacement).

---

## 11.5 LookFX + flarecore → native print-FX & flares

**Source code on disk** (user-owned, port approved):
`F:\lookfx\src\cmykmagic` (~2.4k LOC: halftone screens, 21 paper
substrates, capillary bleed, misregistration, CMYK separations) and
`F:\AIPrograms\_Dev_Projects\flarecore` (flare engine: 41 presets /
182 elements, depth occlusion, linear-space render).

### Recommended path — 100% Built-In Native (Two-Tier Sidecar Superseded)

> **2026-11-15 Zero-Sidecar Override:** Per user directive (*100% built-in, no sidecars, no headless MCPs, no extra downloads*), skip the external Python worker tier below completely and go straight to **native C++ `KisFilter` / `KisGeneratorLayer` + Vulkan/GLSL compute shaders + bundled KTX2 paper/flare assets** inside Solstice (Phase 11.5b/11.5c directly).

1. **Superseded Tier (Do Not Build): external-worker filter.** A `KisFilter` that
   serializes the region to a temp buffer → runs `lookfx`/`flarecore`
   CLI in the existing `D:\krita\venv` (torch already installed) →
   reads back. kritamcp's subprocess+tempfile plumbing is the template.
   Days, not weeks; correctness = the source implementation.
2. **Port hot paths to compute shaders** as usage proves value:
   halftone screens and K-M separations are small deterministic kernels
   (AM/FM screening + channel ops) → real-time preview; flare element
   compositing is additively blendable sprites in linear space → also
   shader-able. Keep torch for the heavy caps (capillary bleed, depth
   occlusion solve).
3. **Generator layer** wrapper (`KisGeneratorLayer` subclass) so
   print-FX can live as a non-destructive layer, not just a filter.

### Testing

- Golden: run lookfx CLI on a fixed input, run the filter, pixel-compare
  (the two-tier design makes the worker the reference oracle).
- flarecore: deterministic-seed preset render → fixture image.

---

## 11.6 Docker/UI QoL pack

Sub-view reference docker (eyedropper from reference image), Layer
Comps, mixing scratch palette + image palette extraction, decoration
brushes (seeded image-tip sequences), vector line suite on
`KoPathShape`, word balloons + panel clip folders, LT conversion,
Auto-Action macro recorder over kritamcp plumbing.

All self-contained Qt/docker work; schedule interleaved with the GPU
items as palette cleansers.

---

## Build/install sequence (per AGENTS.md)

1. C++ edits on `main` → incremental build → **install rebuilt DLLs**
   into the test install → full Solstice restart (native DLLs are
   locked while running — ask user to close if held).
2. Each subsection ships independently: LUT+composite (11.1) →
   inspector (11.3) → QoL (11.4) → worker filter (11.5a) → wet-media
   prototype gate (11.2a) → integration (11.2b).
3. Every item ends in the standard e2e: real canvas operation → pixel
   verification → docs+wiki update.

## Recommended order

11.1 (small, highest visible win) → 11.3 (unique, low-risk, builds on
existing layer) → 11.5a (worker-filter, fast ship) → 11.2a prototype
gate → decide → 11.4/11.6 as filler.
