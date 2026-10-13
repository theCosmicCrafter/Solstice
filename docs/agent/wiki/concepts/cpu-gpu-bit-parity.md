---
type: concept
updated: 2026-10-13
sources:
  - libs/image/KisImageTipSampler.* (phase 4.103, image tips without QPainter)
  - libs/gpu/shaders/transform_pass.comp (phase 4.94, commit 3828911e98)
  - libs/gpu/shaders/grid_warp.comp (phase 4.97, commit 29912c4daf)
  - libs/image/gpu/KisGpuTransformWorker.*, libs/image/gpu/KisGpuGridWarpWorker.*
  - libs/image/tests/KisGpuPaintDeviceTest.cpp (testGpuTransformMatchesCpu, testGpuLiquifyMatchesCpu, testGpuGaussianMatchesCpu)
  - libs/gpu/shaders/separable_convolution.comp (phase 4.98, tolerance-based)
  - libs/gpu/shaders/composite_blend.glsl (phase 4.107: roundedProduct, exactQuotient, mul3, HSX modes)
  - libs/pigment/KoColorSpaceMaths.h (KoColorSpaceMathsTraits<T>::compositetype)
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
9. **Know the composite type.** `Arithmetic::mul()`, `div()`, `blend()`
   and the `cf*` functions compute in
   `KoColorSpaceMathsTraits<T>::compositetype`: **float for float** (not
   double) and double for half. RGBA32F blend modes therefore follow plain
   float arithmetic with IEEE divisions (`exactQuotient()`), while RGBA16F
   ones round a double product or quotient once, then to half (`mul3()`,
   `oneMinusQuotient()`). Emulating double for float made phase 4.107's
   Hard Mix Softer worse.
10. **`precise` is not enough on the NVIDIA driver.** Phase 4.107 found
   `light + t * scale` evaluated as one fma although the SPIR-V carries
   `NoContraction` on every operation (checked with `spirv-dis`, optimized
   and not); scalar code did not help. The result differed only where the
   exact sum lay on a rounding tie. The phase 4.109 audit showed that it
   depends on the compiled context (driver 596.86, Blackwell): small shaders
   keep every precise form, float and double, in
   `KisGpuEngineTest::testPreciseContraction`, and even an inline copy of
   the function in `testBlendProbe` was not fused, but calling the real
   `hsxToneMapping()` there was. So it cannot be detected statically.
   `roundedProduct(a, b)` (`fma(a, b, -error)`, an fma result the driver
   does not fuse; `roundedProductD()` for doubles) gives the CPU's rounded
   product: use it for every product that feeds an addition where the
   result must be bit-identical, at least where a rounding (to half, to an
   integer) follows.
11. **Debugging a 1-ULP drift.** Paint dab by dab on the CPU and the GPU and
   stop at the first differing dab; reproduce the CPU in numpy `float32` /
   `float16` from the printed inputs; then write GPU intermediates as
   `(value - cpuValue) / ulp` into the output pixels, which shows the step
   that differs. Amplifying modes (Vivid Light, Hard Mix Softer, HSL
   saturation) turn one ULP into visible differences over repeated dabs, so
   tolerance tests catch them only with repeated or mirrored dabs.
12. **Read the specializations.** `KoColorSpaceMaths<float>::clamp()` and
   `<half>::clamp()` return their argument; `kisBoundFast()` maps NaN to the
   lower bound; `KoColorSpaceMaths<half>` fuzzy tests use 0.002. Phase 4.108
   reproduced the generic templates first and was wrong for all three.
13. **Transcendentals.** GLSL `log2()` has an absolute error (2^-22), so
   `pow(x, y)` near x = 1 with a large y was off by percent; `accurateLog()`
   (atanh series, ln2 split so `e * ln2` is exact) and `accurateExp()`
   (range reduction) bring it to float precision. Results that the CPU
   computes in double stay tolerance-based.
14. **Failure messages and NaN.** Print the inputs and both results of the
   worst pixel (`testExtendedBlendBoundaries`); most phase 4.108 failures
   were explained by one such line. Modes that make NaN from HDR channels
   on the CPU need NaN == NaN in the comparisons (`sameNonFinite()`,
   bitwise `PixelValues`).

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
