# Spectral pigment mixing (Phase 11.1)

Subtractive ("paint-like") color mixing for Solstice: blue + yellow →
green, not gray. This document is the concrete design for the first
working slice; the research context is in
`phase11-implementation-plan.md` §11.1.

## License decision (unchanged from the plan)

- Mixbox is CC BY-NC — **not used** (no code, no LUT image).
- spectral.js (rvanwijnen, MIT) is the reference implementation. The
  spectral data tables (base pigment spectra, D65-weighted CIE CMF) and
  the algorithm are ported under its MIT license; attribution is kept in
  the source files.
- Scott Burns' "RGB → plausible reflectance" concept is the published
  basis; spectral.js v3 implements it as a closed-form decomposition.

## Algorithm (as implemented)

`KoSpectralMixing` (`libs/pigment/KoSpectralMixing.{h,cpp}`) implements
spectral.js v3 verbatim:

1. **Spectral model**: 38 samples over the visible range; 7 virtual base
   pigments W, C, M, Y, R, G, B, each with a precomputed reflectance
   curve (these curves are the product of Burns' optimization applied
   once offline by the spectral.js authors — we ship the constants).
2. **RGB → reflectance (closed form)**: linear RGB is decomposed into
   weights `w=min(r,g,b)`, `c`, `m`, `y` (pairwise mins of the
   whiteness-removed channels), `r,g,b` (positive residuals);
   `R[λ] = Σ weight_i · base_i[λ]`, clamped to `ε`. No iterative solve —
   the offline-LUT step from the original plan is unnecessary for this
   model.
3. **Kubelka–Munk mixing (single-constant)**: `K/S(λ) = (1−R)²/(2R)`.
   Two colors mix as
   `ks_mix(λ) = (c_dst·K/S_dst(λ) + c_src·K/S_src(λ)) / (c_dst+c_src)`
   with **concentrations `c_i = f_i² · Y_i`** (`f_i` = mixing share,
   `Y_i` = spectral luminance of the color). Luminance weighting is
   spectral.js's anti-mud correction: high-scattering (light) pigments
   dominate the mix the way white does in real paint.
   `R_mix(λ) = 1 + ks − √(ks² + 2ks)`.
4. **Reflectance → RGB**: XYZ via the stored 3×38 D65·CMF matrix, then
   the standard XYZ→linear-sRGB matrix, then TRC encoding.

Verified against the Python port (`tools/spectral_proto.py`):
blue+white→tinted blue, blue+yellow(50/50)→`#398F54` green,
ultramarine+cadmium yellow→olive, red+white→pink, red+green→brown.

## Integration point: `COMPOSITE_PIGMENT`

The slice implements one new composite op instead of scattering hooks:

- `libs/pigment/compositeops/KoCompositeOpPigment.h` — templated
  `KoCompositeOp` subclass (like `KoCompositeOpDissolve`) for any
  `KoRgbTraits`/`KoBgrTraits` colorspace (RGBA U8/U16/F16/F32 — every
  paintable RGB space including the F32 GPU working space).
- Registered in `_Private::AddRGBOps<Traits,true>::add()`
  (`libs/pigment/compositeops/KoCompositeOps.h`) so it appears
  everywhere composite ops are listed:
  - **Layer blend mode** — "Pigment" in the Mix category.
  - **Brush "Blending Mode" option** (pixel brush and every paintop
    with a composite-op selector) — dabs mix spectrally with the
    canvas.
  - **Color Smudge engine "Composite Op" option** — the selected op is
    used as `m_colorRateOp` (`KisColorSmudgeStrategyBase::
    initializePaintingImpl`), i.e. the Color Rate blending becomes a
    Kubelka–Munk pigment mix. This is the 11.1b brush-mixing surface.
- `COMPOSITE_PIGMENT = "pigment"` in `KoCompositeOpRegistry.h` + display
  name in the registry's "mix" category.

### Mix weight semantics

Inside the op, the spectral blend color is
`B = K/M-mix(dst, src, f_src = sa/(sa+da), f_dst = da/(sa+da))` —
each pixel mixes by the *relative coverage share* of the two pigment
layers, then standard Porter-Duff blend compositing applies
(`Arithmetic::blend(src, sa, dst, da, B)`, `unionShapeOpacity`).

Consequences (all physical):

- Opaque src over opaque dst → 50/50 pigment mix (blue layer/dab over
  yellow → green). The headline behavior.
- Semi-transparent src over opaque dst → lighter pigment share; dab
  edges tint instead of covering — wet-edge feel for free.
- `dstAlpha = 0` → src wins (nothing to mix with) — same as Normal.
- In the Color Smudge color-rate path the `opacity` argument is the
  Color Rate value, so the slider directly controls pigment
  concentration.

### Encoding handling

- `profile()->isLinear()` → channel values used directly (RGBA
  F16/F32 working space fast path).
- Otherwise a 256-entry linearize/delinearize table is built once per
  op instance from `profile()->linearizeFloatValue` /
  `delinearizeFloatValue` (exact TRC for sRGB U8/U16 and any other RGB
  profile; sRGB-curve fallback if the profile reports no TRC).
- Channel values are clamped to [0,1] for the spectral solve: HDR
  (>1.0) pixels mix as their clamped SDR color (documented limitation;
  the K-M model is SDR-defined).
- Alpha and disabled channel handling follow the generic
  `KoCompositeOpGenericHSLFunctor` conventions (`channelFlags`,
  `alphaLocked`, `unionShapeOpacity`).

## Performance note

Per pixel ≈ 1.5–3 kFLOP (two 38×7 uplifts, 38-wide K/S ops, 3×38 CMF).
The op caches the constant-source spectrum inside `composite()`
(`srcRowStride == 0` — the smudge color-rate path and solid fills), so
only the destination spectrum is computed per pixel there. It is still
an order of magnitude slower than a plain blend mode; acceptable for an
opt-in mode, and the documented productization path is:

- GPU `composite_pigment.comp` dispatched from `KisGpuMergeBatch` /
  dab compositing (the plan's ~30-ALU kernel — fold the uplift weights
  into a small LUT or keep the 7-basis closed form, both fine on GPU).
- Optional CPU LUT: 33³ × (7 basis weights | compressed K/S) if the CPU
  path needs more speed before the GPU path lands.

## Testing

`libs/pigment/tests/TestKoSpectralMixing.{h,cpp}` (in `kis_add_tests`):
round-trip rgb→R→rgb error, blue+yellow→green hue check, white+X,
black+X, and the end-to-end `cs->compositeOp("pigment")->composite()`
path on rgb8. Numbers were generated by `tools/spectral_proto.py`
(faithful port of spectral.js kept as the reference oracle).
`tools/run-unittest.ps1 <exe>` runs a test exe with the
compiler/deps/build PATH set.

**Harness gotchas (learned 2026-11-02)**: pigment tests that touch real
colorspaces must use `testpigment.h`/`KISTEST_MAIN` — it sets
`KRITA_PLUGIN_PATH` to `<install>/lib/kritaplugins`. Qt's
`QTEST_GUILESS_MAIN` leaves it empty; `KoJsonTrader` then scans the
working directory and can load *stale installed* `kritalcmsengine.dll`
copies, so new composite ops silently "don't exist". Also,
`addStandardCompositeOps` is a header template instantiated inside the
colorspace plugin (kritalcmsengine), not only libkritapigment — after
changing it, rebuild **and install** both before running tests or the
app.

## Remaining for full productization

- GPU dispatch of the op for RGBA F32/F16 (projection + brush dab
  paths) — currently falls back to CPU like other non-accelerated ops.
- Optional spectral LUT/commit generator (`tools/build_spectral_lut.py`)
  if a denser uplift basis is wanted later.
- A "tinting strength" per-preset factor (spectral.js exposes it; the
  op fixes it at 1.0).
- Eraser/other Porter-Duff variants stay non-spectral (mix only touches
  RGB, alpha unchanged — by design).
