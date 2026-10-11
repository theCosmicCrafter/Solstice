---
type: concept
updated: 2026-10-11
sources:
  - libs/image/KisImageTipSampler.* (phase 4.103, image tips without QPainter)
  - libs/gpu/shaders/transform_pass.comp (phase 4.94, commit 3828911e98)
  - libs/gpu/shaders/grid_warp.comp (phase 4.97, commit 29912c4daf)
  - libs/image/gpu/KisGpuTransformWorker.*, libs/image/gpu/KisGpuGridWarpWorker.*
  - libs/image/tests/KisGpuPaintDeviceTest.cpp (testGpuTransformMatchesCpu, testGpuLiquifyMatchesCpu, testGpuGaussianMatchesCpu)
  - libs/gpu/shaders/separable_convolution.comp (phase 4.98, tolerance-based)
related:
  - krita-copy-semantics.md
  - qt-numeric-and-geometry.md
  - ../history/gpu-phases-4.93-.md
---

# CPU/GPU bit-identical parity

## Which rule applies

- **Brushes and the projection** follow the parity rule from phase 0
  (`docs/agent/gpu-engine.md`, "Parity rule found in phase 0"): within 1e-5
  for F32, and pixels with alpha 0 are compared by alpha only.
- **Transform workers** moved to the GPU are **bit-identical**: the affine
  passes (phase 4.94) and the Liquify grid warp (phase 4.97). Tests compare
  raw bytes, including the color of transparent pixels, plus `exactBounds()`
  and Undo/Redo. Treat any new transform worker the same way unless the user
  decides otherwise.
- **The Gaussian convolution** (phase 4.98) cannot be bit-identical: the CPU
  convolves with FFTW, whose rounding the GPU cannot reproduce. The user
  approved a tolerance: relative 1e-5 for F32 and 1e-3 for F16 (about one
  half ulp), alpha only where an alpha is near the FFT worker's null
  threshold. The GPU sums directly in doubles and follows the CPU's write
  rules (clamping, alpha handling, double -> float -> half rounding), so the
  measured difference is at most one rounding step. Pixels outside the rect
  must still be bit-identical.
- **Puppet Warp** (phase 4.96) paints its polygons bit-identically (the
  Liquify recorder). With several stacking groups, though, the CPU composites
  the group layers with "over", and for RGBA32F that is not reproducible even
  between CPU runs (see
  [Krita copy and sampling semantics](krita-copy-semantics.md)). Those cases
  use the phase 0 rule; one group and F16 stay bit-identical.

## Techniques that made bit parity work

1. **Plan on the CPU with the CPU's own code.** Do not reimplement the
   CPU's decisions.
   - The affine passes call `KisFilterWeightsApplicator::setupLine()`, which
     was extracted from `processLine()`.
   - Liquify passes a recorder (`KisGpuGridWarpWorker::Recorder`) to the same
     `iterateThroughGrid()` that the CPU uses.
   - Per-operation constants come from the CPU objects, e.g.
     `KisFourPointInterpolatorBackward::coefficients()`.
   - The GPU only evaluates what depends on the pixel.
2. **Doubles with `precise`.** The CPU build (llvm-mingw clang, x86-64
   without an FMA target) does not contract `a * b + c` into an fma. Mark
   every GLSL double temporary `precise`, and write expressions in the C++
   evaluation order (left to right, the same parenthesization).
3. **Correctly rounded division and square root.** GPU double `/` and
   `sqrt` are not guaranteed to be correctly rounded. `exactDivide()` and
   `exactSqrt()` take the hardware result and pick the neighbouring double
   (within 2 ulps) with the smallest fma residual. Copy them from
   `transform_pass.comp` / `grid_warp.comp`.
4. **Store like the CPU.**
   - double -> float rounds to nearest even (`toFloat()`).
   - half rounds like Imath (`roundToHalf()`).
   - Values are clamped to +-FLT_MAX / +-HALF_MAX before the conversion,
     as `KoMixColorsOpImpl` does.
5. **Mix colors in the CPU order.** `KoMixColorsOpImpl::mixColors()` with
   `qint16` weights:
   - accumulation: `alphaTimesWeight = alpha * weight`, then
     `total[c] += color[c] * alphaTimesWeight`, in pixel order, in doubles;
   - color: `total[c] / totalAlpha`;
   - alpha: `totalAlpha / weightSum`, where `weightSum` is the explicit sum
     passed by the caller (e.g. `KisRandomSubAccessor`'s rounded weights),
     not always 255;
   - `totalAlpha <= 0` gives all zeros.
6. **Order of overlapping writes.** When the CPU paints items one after
   another and later items overwrite earlier ones (Liquify's cells), a claim
   pass records `atomicMax(owner, index + 1)` per pixel; a resolve pass lets
   the owner compute the pixel.
7. **Fail safely.** If the GPU would need data it does not have, e.g. a
   sample outside the uploaded source grid:
   - the shader sets a flag;
   - the worker returns false;
   - the caller restores the CPU starting state (`dst->clear()`) and runs
     the CPU path.
   Restrict `canRun()` to cases where the CPU behavior is deterministic; see
   [Krita copy semantics](krita-copy-semantics.md) for a case that is not.
8. **Replace CPU code that is not deterministic.** QPainter's
   `drawImage()` (brush image tips) is not: Qt swaps its pixel conversions
   at run time for AVX2/SSE4 versions with different rounding, and the
   SSE4 unpremultiply uses `rcpps`, whose result differs between CPU
   vendors. Phase 4.103 replaced it, with the user's approval, by integer
   code that follows Qt's portable path (`KisImageTipSampler`) and that the
   CPU and the shader both run; the difference from Qt is at most one 8-bit
   step. Ask before changing upstream CPU output like this.

## Test pattern

`KisGpuPaintDeviceTest::testGpuTransformMatchesCpu` and
`testGpuLiquifyMatchesCpu`:

1. Run the CPU path with the GPU switch off (`KRITA_GPU_TRANSFORM=0` /
   `KRITA_GPU_LIQUIFY=0`) and assert the GPU run counter did not move.
2. Run the GPU path inside a `KisTransaction` on a copy; assert the counter
   moved by one (a silent CPU fallback would hide bugs).
3. Compare raw bytes over a rect larger than both results; on mismatch,
   print the first differing pixel and channel (`memcmp`).
4. Compare `exactBounds()`; check Undo restores and Redo reapplies.
5. Cover F32 and F16; offset devices; an opaque default pixel; degenerate
   geometry (folds, tiny scales); and every filter or parameter family.
6. Run with `KRITA_GPU_VALIDATION=1` (Vulkan validation; the fixture asserts
   no validation errors).
