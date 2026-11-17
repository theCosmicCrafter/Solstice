/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Spectral (Kubelka-Munk) pigment mixing.
 *
 * The algorithm and the spectral data tables are a C++ port of
 * spectral.js v3 by Ronald van Wijnen
 * (https://github.com/rvanwijnen/spectral.js), released under the MIT
 * license. The model implements Scott Allen Burns' "RGB to plausible
 * spectral reflectance" method as a closed-form decomposition into
 * seven virtual base pigments, and single-constant Kubelka-Munk
 * mixing of the resulting reflectance curves.
 */

#ifndef KO_SPECTRAL_MIXING_H_
#define KO_SPECTRAL_MIXING_H_

#include <cmath>

#include "kritapigment_export.h"

/**
 * @brief Kubelka-Munk spectral pigment mixing utilities.
 *
 * All functions operate on linear sRGB channel triplets in [0,1] and
 * on 38-sample spectral reflectance curves. They are stateless,
 * allocation-free and safe to call from any thread.
 */
class KRITAPIGMENT_EXPORT KoSpectralMixing
{
public:
    /// Number of wavelength samples in every reflectance curve.
    static constexpr int SampleCount = 38;

    /// sRGB display transfer function (gamma -> linear).
    static float uncompandSrgb(float x);

    /// Inverse sRGB display transfer function (linear -> gamma).
    static float compandSrgb(float x);

    /**
     * Decompose a clamped linear RGB color into a spectral reflectance
     * curve via the seven base pigments (W, C, M, Y, R, G, B).
     * Inputs outside [0,1] are clamped.
     */
    static void lrgbToReflectance(float r, float g, float b, float reflectance[SampleCount]);

    /**
     * Evaluate a reflectance curve to XYZ (D65-weighted CIE CMF), then
     * to linear sRGB. The result is clamped to [0,1] (SDR gamut).
     */
    static void reflectanceToLrgb(const float reflectance[SampleCount], float rgb[3]);

    /**
     * Spectral luminance (Y tristimulus) of a reflectance curve.
     * Always >= epsilon.
     */
    static float luminance(const float reflectance[SampleCount]);

    /// Kubelka-Munk absorption/scattering ratio K/S = (1-R)^2 / (2R).
    static inline float reflectanceToKs(float r)
    {
        const float q = 1.0f - r;
        return q * q / (2.0f * r);
    }

    /// Inverse: R = 1 + K/S - sqrt((K/S)^2 + 2 K/S).
    static inline float ksToReflectance(float ks)
    {
        return 1.0f + ks - sqrtf(ks * ks + 2.0f * ks);
    }

    /**
     * Kubelka-Munk mix of two reflectance curves.
     *
     * @param dstR, srcR   input reflectance curves (SampleCount samples)
     * @param dstFactor, srcFactor  mixing shares (>= 0, need not sum to 1;
     *        they are normalized internally). Both zero is invalid.
     * @param outR          mixed reflectance curve
     *
     * Each color's concentration is factor^2 * luminance, matching
     * spectral.js: lighter (highly scattering) pigments dominate the
     * mix, which keeps subtractive mixes from going muddy.
     */
    static void mix(const float dstR[SampleCount],
                    float dstFactor,
                    const float srcR[SampleCount],
                    float srcFactor,
                    float outR[SampleCount]);

    /**
     * Convenience helper: full linear-RGB -> linear-RGB mix.
     * srcFactor/dstFactor as in mix().
     */
    static void
    mixLrgb(const float dstLrgb[3], const float srcLrgb[3], float dstFactor, float srcFactor, float outLrgb[3]);
};

#endif // KO_SPECTRAL_MIXING_H_
