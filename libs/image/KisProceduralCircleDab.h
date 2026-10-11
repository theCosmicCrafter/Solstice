/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISPROCEDURALCIRCLEDAB_H
#define KISPROCEDURALCIRCLEDAB_H

#include <QSharedPointer>
#include <QVector>
#include <QtGlobal>

#include <cmath>
#include <cstring>

#include <half.h>

#include "kis_types.h"

#include <KisImageTipSampler.h>
#include <KoColorSpaceMaths.h>

/**
 * GPU engine (Solstice): the pattern texture of a described dab
 * (KisTextureOption, the alpha-only texturing modes), applied on the GPU
 * after the circle (docs/agent/gpu-engine.md, "Textured dabs"). Dab pixel
 * (x, y) of the textured layout reads pattern texel
 * ((originX + x) mod width, (originY + y) mod height) and composites it
 * into the dab alpha like KisMaskingBrushCompositeOp<float, mode, true,
 * true, soft>. The textured layout is the generation layout reflected by
 * brushFlips (the brush tip's mirroring happens before texturing).
 */
struct KisProceduralDabTexture {
    /// Applies the same texture on the CPU, for materialized dabs.
    struct CpuApplier {
        virtual ~CpuApplier() = default;
        virtual void apply(KisFixedPaintDeviceSP dab) const = 0;
    };

    QSharedPointer<const QVector<quint8>> pattern; // alpha8 texels, row-major
    int width = 0;
    int height = 0;
    int originX = 0;
    int originY = 0;
    quint32 mode = 0; // KisMaskingBrushCompositeFuncTypes
    bool soft = false;
    // The composite function's constants, as its constructor computes
    // them in float (StrengthCompositeFunctionBase and the weights).
    float strength = 0.0f;
    float invertedStrength = 0.0f;
    float weight = 0.0f;
    quint32 brushFlips = 0;
    QSharedPointer<const CpuApplier> cpu;
};

/**
 * GPU engine (Solstice): the parameters of an RGBA F32 dab of a circle auto
 * brush (KisCircleMaskGenerator or KisGaussCircleMaskGenerator, vectorized
 * path), so that the GPU brush can evaluate the dab instead of uploading its
 * pixels (docs/agent/gpu-engine.md, phases 4.83 and 4.84).
 *
 * Pixel (x, y) of the dab, in its unmirrored generation layout, is
 * (color.rgb, 1 - fadeAt(x, y)). fadeAt() repeats, operation by operation,
 * the AVX2+FMA build of FastRowProcessor<...>
 * (kis_brush_mask_processor_factories, compiled with -ffp-contract=fast),
 * including the multiply-adds the compiler fused there, xsimd's exp() and
 * VcExtraMath::erf(). paint_dabs.comp mirrors it. Other CPU kernels round
 * differently; describeCircleDab() then rejects the description.
 */
struct KisProceduralCircleDab {
    enum Kind : quint32 {
        DefaultCircle = 0,
        GaussCircle = 1,
        SoftCircle = 2, // KisCurveCircleMaskGenerator (phase 4.87)
        /// A predefined (image) brush tip in alpha-mask mode ("Image tip
        /// dabs"): the alpha of pixel (x, y) is
        /// Uint8ToFloat(UINT8_MULT(255 - red, alpha)) of the ARGB32 pixel
        /// KisImageTipSampler::pixel() gives (KoColorSpaceTrait::
        /// fillGrayBrushWithColor()); RGBA F32 only.
        ImageTip = 3,
    };
    Kind kind = DefaultCircle;

    float centerX = 0.0f;
    float centerY = 0.0f;
    float cosa = 1.0f;
    float sina = 0.0f;
    // Default circle.
    float xcoef = 0.0f;
    float ycoef = 0.0f; // also the Gaussian y coefficient
    float fadeX = 0.0f;
    float fadeY = 0.0f;
    // Gaussian circle.
    float distfactor = 0.0f;
    float center = 0.0f;
    float alphafactor = 0.0f;
    float radius = 0.0f;
    float fadeStart = 0.0f;
    float fadeStartValue = 0.0f; // an 8-bit value, as the CPU stores it
    float fadeCoeff = 0.0f; // fade maker state, shared by the Gaussian and Soft kinds
    // Soft circle: xcoef/ycoef above, and the curve table as the vector
    // kernel gathers it (double values converted to float).
    float curveResolution = 0.0f;
    QSharedPointer<const QVector<float>> curveTable;
    /// RGBA F32 paint color in the dab color space; alpha is replaced.
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool antialias = false;
    /// RGBA F16 dab (phase 4.86): the color holds exact half values, and the
    /// alpha is stored as half(alphaAt()) (Imath, round to nearest even).
    bool halfPixels = false;
    /// Optional pattern texture (RGBA F32 only). render() and
    /// matchesPixel() describe the circle without it; KisRenderedDab::
    /// materialize() applies it.
    QSharedPointer<const KisProceduralDabTexture> texture;
    /// ImageTip: the transformed tip (KisQImagePyramid::describeImage()).
    QSharedPointer<const KisImageTipGeometry> imageTip;

    /// UINT8_MULT(255 - red, alpha) of an ARGB32 tip pixel, as a float alpha
    static inline float imageTipAlpha(quint32 pixel)
    {
        const quint32 t = (255 - ((pixel >> 16) & 0xff)) * (pixel >> 24) + 0x80;
        return KoColorSpaceMaths<quint8, float>::scaleToA(quint8(((t >> 8) + t) >> 8));
    }

    inline float fadeAt(int x, int y) const
    {
        return kind == GaussCircle ? gaussFadeAt(x, y) : kind == SoftCircle ? softFadeAt(x, y) : circleFadeAt(x, y);
    }

    inline float alphaAt(int x, int y) const
    {
        if (kind == ImageTip) {
            return imageTipAlpha(KisImageTipSampler::pixel(*imageTip, x, y));
        }
        return 1.0f * (1.0f - fadeAt(x, y));
    }

    /**
     * Writes the RGBA F32 dab pixels, mirrored by @p flips (horizontal = 1,
     * vertical = 2) relative to the generation layout, exactly as the CPU
     * generator (and KisFixedPaintDevice::mirror()) would have produced them.
     */
    inline void render(quint8 *pixels, int width, int height, quint32 flips) const
    {
        if (kind == ImageTip) {
            const QImage tip = KisImageTipSampler::render(*imageTip);
            float *out = reinterpret_cast<float *>(pixels);
            for (int y = 0; y < height; y++) {
                const int gy = (flips & 2) ? height - 1 - y : y;
                const quint32 *line =
                    gy < tip.height() ? reinterpret_cast<const quint32 *>(tip.constScanLine(gy)) : nullptr;
                for (int x = 0; x < width; x++) {
                    const int gx = (flips & 1) ? width - 1 - x : x;
                    out[0] = color[0];
                    out[1] = color[1];
                    out[2] = color[2];
                    out[3] = line && gx < tip.width() ? imageTipAlpha(line[gx]) : 0.0f;
                    out += 4;
                }
            }
            return;
        }
        if (halfPixels) {
            half *out = reinterpret_cast<half *>(pixels);
            const half red(color[0]), green(color[1]), blue(color[2]);
            for (int y = 0; y < height; y++) {
                const int gy = (flips & 2) ? height - 1 - y : y;
                for (int x = 0; x < width; x++) {
                    const int gx = (flips & 1) ? width - 1 - x : x;
                    out[0] = red;
                    out[1] = green;
                    out[2] = blue;
                    out[3] = half(alphaAt(gx, gy));
                    out += 4;
                }
            }
            return;
        }
        float *out = reinterpret_cast<float *>(pixels);
        for (int y = 0; y < height; y++) {
            const int gy = (flips & 2) ? height - 1 - y : y;
            for (int x = 0; x < width; x++) {
                const int gx = (flips & 1) ? width - 1 - x : x;
                out[0] = color[0];
                out[1] = color[1];
                out[2] = color[2];
                out[3] = alphaAt(gx, gy);
                out += 4;
            }
        }
    }

    /// True if pixel (x, y) of the generation layout of @p pixels (RGBA F32,
    /// or RGBA F16 for halfPixels) is exactly the described one.
    inline bool matchesPixel(const quint8 *pixels, int width, int x, int y, int gx, int gy) const
    {
        const qint64 index = 4 * (qint64(y) * width + x);
        if (halfPixels) {
            const half *pixel = reinterpret_cast<const half *>(pixels) + index;
            return pixel[0].bits() == half(color[0]).bits() && pixel[1].bits() == half(color[1]).bits()
                && pixel[2].bits() == half(color[2]).bits() && pixel[3].bits() == half(alphaAt(gx, gy)).bits();
        }
        const float *pixel = reinterpret_cast<const float *>(pixels) + index;
        return pixel[0] == color[0] && pixel[1] == color[1] && pixel[2] == color[2] && pixel[3] == alphaAt(gx, gy);
    }

    inline float circleFadeAt(int x, int y) const
    {
        const float y_ = float(y) - centerY;
        const float sinay_ = sina * y_;
        const float cosay_ = cosa * y_;
        const float x_ = float(x) - centerX;
        float xr = std::fma(x_, cosa, -sinay_);
        float yr = std::fma(x_, sina, cosay_);
        const float a = xr * xcoef;
        const float b = yr * ycoef;
        const float n = std::fma(a, a, b * b);
        if (n > 1.0f) {
            return 1.0f;
        }
        if (antialias) {
            xr = std::abs(xr) + 1.0f;
            yr = std::abs(yr) + 1.0f;
        }
        const float fa = xr * fadeX;
        const float fb = yr * fadeY;
        const float normFade = std::fma(fb, fb, fa * fa);
        if (normFade < 1.0f) {
            return 0.0f;
        }
        return n * (normFade - 1.0f) / (normFade - n);
    }

    inline float softFadeAt(int x, int y) const
    {
        const float y_ = float(y) - centerY;
        const float sinay_ = sina * y_;
        const float cosay_ = cosa * y_;
        const float x_ = float(x) - centerX;
        const float xr = std::fma(x_, cosa, -sinay_);
        const float yr = std::fma(x_, sina, cosay_);
        const float a = xr * xcoef;
        const float b = yr * ycoef;
        float dist = std::fma(a, a, b * b);

        // KisAntialiasingFadeMaker1D::needFade() (vector form, square norm).
        const bool outside = dist > radius;
        if (outside) {
            dist = 1.0f;
        }
        bool fadeStartMask = false;
        if (antialias) {
            fadeStartMask = dist > fadeStart;
            if (fadeStartMask && !outside) {
                dist = std::fma(dist - fadeStart, fadeCoeff, fadeStartValue) / 255.0f;
            }
        }
        if (outside || fadeStartMask) {
            return dist;
        }

        const float valDist = dist * curveResolution;
        int index = int(valDist); // truncation, like cvttps2dq
        const float fraction = valDist - float(index);
        if (index < 0) {
            index = 0;
        }
        const QVector<float> &table = *curveTable;
        const float full = std::fma(table[index], 1.0f - fraction, table[index + 1] * fraction);
        const float clamped = full > 0.0f ? full : 0.0f;
        return clamped < 1.0f ? 1.0f - clamped : 0.0f;
    }

    /// xsimd exp<float> on FMA architectures (Cephes reduction and polynomial).
    static inline float fusedExp(float a)
    {
        auto bits = [](quint32 value) {
            float result;
            std::memcpy(&result, &value, sizeof(result));
            return result;
        };
        const float k = std::nearbyint(1.442695040888963407359924681001892137426645954152986f * a);
        float x = std::fma(-k, bits(0x3f318000), a);
        x = std::fma(-k, bits(0xb95e8083), x);
        const float y =
            std::fma(x,
                     std::fma(x,
                              std::fma(x, std::fma(x, bits(0x3ab778cf), bits(0x3c098d8b)), bits(0x3d2aa957)),
                              bits(0x3e2aa9a5)),
                     bits(0x3f000000));
        float result = std::fma(y, x * x, x) + 1.0f;
        const quint32 exponent = quint32(int(k) + 127) << 23;
        result = result * bits(exponent);
        if (a <= -88.3762626647949f) {
            return 0.0f;
        }
        if (a >= 88.3762626647949f) {
            return INFINITY;
        }
        return result;
    }

    /// VcExtraMath::erf (Abramowitz and Stegun 7.1.26).
    static inline float vectorErf(float x)
    {
        float xa = std::abs(x);
        const bool limit = xa >= 9.3f;
        if (limit) {
            xa = 0.0f;
        }
        const float sign = x < 0.0f ? -1.0f : 1.0f;
        const float t = 1.0f / std::fma(xa, 0.3275911f, 1.0f);
        const float e = fusedExp(-xa * xa);
        const float poly =
            std::fma(std::fma(std::fma(std::fma(1.061405429f, t, -1.453152027f), t, 1.421413741f), t, -0.284496736f),
                     t,
                     0.254829592f);
        float result = std::fma(-(poly * t), e, 1.0f);
        if (limit) {
            result = 1.0f;
        }
        return sign * result;
    }

    inline float gaussFadeAt(int x, int y) const
    {
        const float y_ = float(y) - centerY;
        const float sinay_ = sina * y_;
        const float cosay_ = cosa * y_;
        const float x_ = float(x) - centerX;
        const float xr = std::fma(x_, cosa, -sinay_);
        const float yr = std::fma(x_, sina, cosay_);
        const float b = yr * ycoef;
        float dist = std::sqrt(std::fma(xr, xr, b * b));

        // KisAntialiasingFadeMaker1D::needFade() (vector form).
        const bool outside = dist > radius;
        if (outside) {
            dist = 1.0f;
        }
        bool fadeStartMask = false;
        if (antialias) {
            fadeStartMask = dist > fadeStart;
            if (fadeStartMask && !outside) {
                dist = std::fma(dist - fadeStart, fadeCoeff, fadeStartValue) / 255.0f;
            }
        }
        if (outside || fadeStartMask) {
            return dist;
        }

        const float valDist = dist * distfactor;
        float fullFade = (vectorErf(valDist + center) - vectorErf(valDist - center)) * alphafactor;
        if (fullFade < 0.0f) {
            fullFade = 0.0f;
        }
        if (fullFade > 254.974f) {
            return 0.0f;
        }
        return (255.0f - fullFade) / 255.0f;
    }
};

#endif // KISPROCEDURALCIRCLEDAB_H
