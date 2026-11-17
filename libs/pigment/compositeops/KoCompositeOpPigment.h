/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _KOCOMPOSITEOP_PIGMENT_H_
#define _KOCOMPOSITEOP_PIGMENT_H_

#include <cstring>

#include <QBitArray>
#include <QVector>

#include <KoColorProfile.h>
#include <KoColorSpace.h>
#include <KoColorSpaceMaths.h>
#include <KoCompositeOp.h>
#include <KoCompositeOpRegistry.h>
#include <KoSpectralMixing.h>

/**
 * Subtractive "pigment" composite op: mixes source and destination
 * like paint instead of filtering light. Blue over yellow gives
 * green, not gray.
 *
 * Implementation: single-constant Kubelka-Munk spectral mixing
 * (KoSpectralMixing, port of spectral.js v3, MIT). Where source and
 * destination both cover the pixel, they mix by relative coverage
 * share, f_src = sa/(sa+da); the result is then composited with the
 * usual Porter-Duff blend formula, so alpha semantics match the other
 * blend modes.
 *
 * Works on any KoRgbTraits/KoBgrTraits colorspace (RGBA U8/U16/F16/F32).
 * For non-linear profiles the channel TRC is sampled into a lookup
 * table at construction (exact for sRGB and other RGB profiles; sRGB
 * curve fallback when the profile has no TRC). HDR channel values are
 * clamped to the SDR gamut for the spectral solve.
 */
template<class Traits>
class KoCompositeOpPigment : public KoCompositeOp
{
    typedef typename Traits::channels_type channels_type;

    static const qint32 channels_nb = Traits::channels_nb;
    static const qint32 alpha_pos = Traits::alpha_pos;
    static const qint32 red_pos = Traits::red_pos;
    static const qint32 green_pos = Traits::green_pos;
    static const qint32 blue_pos = Traits::blue_pos;
    static const qint32 pixel_size = Traits::pixelSize;

    static const int trcTableSize = 256;
    static const int numRgbChannels = 3;

public:
    explicit KoCompositeOpPigment(const KoColorSpace *cs)
        : KoCompositeOp(cs, COMPOSITE_PIGMENT, KoCompositeOp::categoryMix())
    {
        const KoColorProfile *profile = cs->profile();

        m_linearSpace = !profile || profile->isLinear();
        m_useProfileTrc = profile && profile->hasTRC() && !profile->isLinear();

        if (m_useProfileTrc) {
            QVector<qreal> v(numRgbChannels);
            for (int ch = 0; ch < numRgbChannels; ++ch) {
                for (int i = 0; i < trcTableSize; ++i) {
                    v[0] = v[1] = v[2] = 0.0;
                    v[ch] = qreal(i) / qreal(trcTableSize - 1);
                    profile->linearizeFloatValue(v);
                    m_linearize[ch][i] = float(v[ch]);

                    v[0] = v[1] = v[2] = 0.0;
                    v[ch] = qreal(i) / qreal(trcTableSize - 1);
                    profile->delinearizeFloatValue(v);
                    m_delinearize[ch][i] = float(v[ch]);
                }
            }
        }
    }

    using KoCompositeOp::composite;

    void composite(const KoCompositeOp::ParameterInfo &params) const override
    {
        const QBitArray &flags = params.channelFlags.isEmpty() ? QBitArray(channels_nb, true) : params.channelFlags;
        const bool allChannelFlags =
            params.channelFlags.isEmpty() || params.channelFlags == QBitArray(channels_nb, true);
        const bool alphaLocked = (alpha_pos != -1) && !flags.testBit(alpha_pos);
        const bool useMask = params.maskRowStart != 0;

        using namespace Arithmetic;

        const qint32 srcInc = (params.srcRowStride == 0) ? 0 : channels_nb;
        const channels_type opacity = scale<channels_type>(params.opacity);

        const quint8 *srcRowStart = params.srcRowStart;
        quint8 *dstRowStart = params.dstRowStart;
        const quint8 *maskRowStart = params.maskRowStart;

        SourceSpectrum srcSpec;
        srcSpec.valid = false;

        for (qint32 r = 0; r < params.rows; ++r) {
            const channels_type *src = reinterpret_cast<const channels_type *>(srcRowStart);
            channels_type *dst = reinterpret_cast<channels_type *>(dstRowStart);
            const quint8 *mask = maskRowStart;

            for (qint32 c = 0; c < params.cols; ++c) {
                const channels_type srcAlpha = (alpha_pos == -1) ? unitValue<channels_type>() : src[alpha_pos];
                const channels_type dstAlpha = (alpha_pos == -1) ? unitValue<channels_type>() : dst[alpha_pos];
                const channels_type maskAlpha = useMask ? scale<channels_type>(*mask) : unitValue<channels_type>();

                if (!allChannelFlags && dstAlpha == zeroValue<channels_type>()) {
                    memset(reinterpret_cast<quint8 *>(dst), 0, pixel_size);
                }

                const channels_type effectiveSrcAlpha = mul(srcAlpha, maskAlpha, opacity);

                if (!isZeroValueFuzzy(effectiveSrcAlpha)) {
                    composePixel(src, effectiveSrcAlpha, dst, dstAlpha, flags, alphaLocked, srcSpec);
                }

                src += srcInc;
                dst += channels_nb;
                if (useMask) {
                    ++mask;
                }
            }

            srcRowStart += params.srcRowStride;
            dstRowStart += params.dstRowStride;
            maskRowStart += params.maskRowStride;
        }
    }

private:
    /**
     * Cached K/S spectrum + luminance of the most recently decoded
     * source color. When the source is a constant color
     * (srcRowStride == 0, e.g. the color-rate blend inside the color
     * smudge engine) this avoids re-running the spectral uplift per
     * pixel.
     */
    struct SourceSpectrum {
        float ks[KoSpectralMixing::SampleCount];
        float luminance{0.0f};
        float lrgb[3]{0.0f, 0.0f, -1.0f};
        bool valid{false};
    };

    inline float tableLookup(const float *table, float x) const
    {
        x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        const float pos = x * float(trcTableSize - 1);
        const int idx = int(pos);
        const float frac = pos - float(idx);
        const int next = idx + 1 < trcTableSize ? idx + 1 : trcTableSize - 1;
        return table[idx] + (table[next] - table[idx]) * frac;
    }

    /// encoded [0,1] -> linear [0,1] for the channel at memory position `pos`
    inline float toLinear(qint32 pos, float encoded) const
    {
        if (m_linearSpace) {
            return encoded;
        }
        if (m_useProfileTrc) {
            return tableLookup(m_linearize[pos], encoded);
        }
        return KoSpectralMixing::uncompandSrgb(encoded);
    }

    /// linear [0,1] -> encoded [0,1] for the channel at memory position `pos`
    inline float fromLinear(qint32 pos, float linear) const
    {
        if (m_linearSpace) {
            return linear;
        }
        if (m_useProfileTrc) {
            return tableLookup(m_delinearize[pos], linear);
        }
        return KoSpectralMixing::compandSrgb(linear);
    }

    inline void composePixel(const channels_type *src,
                             channels_type srcAlpha,
                             channels_type *dst,
                             channels_type dstAlpha,
                             const QBitArray &channelFlags,
                             bool alphaLocked,
                             SourceSpectrum &srcSpec) const
    {
        using namespace Arithmetic;

        if (alphaLocked && isZeroValueFuzzy(dstAlpha)) {
            // alpha-locked destination with no pigment: nothing to mix into
            return;
        }

        if (!alphaLocked && isZeroValueFuzzy(dstAlpha)) {
            // No destination pigment: the source covers like Normal
            for (qint32 i = 0; i < channels_nb; ++i) {
                if (i != alpha_pos && channelFlags.testBit(i)) {
                    dst[i] = src[i];
                }
            }
            if (alpha_pos != -1) {
                dst[alpha_pos] = srcAlpha;
            }
            return;
        }

        const float srcR = toLinear(red_pos, scale<float>(src[red_pos]));
        const float srcG = toLinear(green_pos, scale<float>(src[green_pos]));
        const float srcB = toLinear(blue_pos, scale<float>(src[blue_pos]));

        if (!srcSpec.valid || srcSpec.lrgb[0] != srcR || srcSpec.lrgb[1] != srcG || srcSpec.lrgb[2] != srcB) {
            float srcRefl[KoSpectralMixing::SampleCount];
            KoSpectralMixing::lrgbToReflectance(srcR, srcG, srcB, srcRefl);
            srcSpec.luminance = KoSpectralMixing::luminance(srcRefl);
            for (int i = 0; i < KoSpectralMixing::SampleCount; ++i) {
                srcSpec.ks[i] = KoSpectralMixing::reflectanceToKs(srcRefl[i]);
            }
            srcSpec.lrgb[0] = srcR;
            srcSpec.lrgb[1] = srcG;
            srcSpec.lrgb[2] = srcB;
            srcSpec.valid = true;
        }

        const float dstR = toLinear(red_pos, scale<float>(dst[red_pos]));
        const float dstG = toLinear(green_pos, scale<float>(dst[green_pos]));
        const float dstB = toLinear(blue_pos, scale<float>(dst[blue_pos]));

        float dstRefl[KoSpectralMixing::SampleCount];
        KoSpectralMixing::lrgbToReflectance(dstR, dstG, dstB, dstRefl);
        const float dstLum = KoSpectralMixing::luminance(dstRefl);

        // Pigment shares by relative coverage; both are > 0 here.
        const float saF = qBound(0.0f, scale<float>(srcAlpha), 1.0f);
        const float daF = qBound(0.0f, scale<float>(dstAlpha), 1.0f);
        const float shareSum = saF + daF;
        const float srcFactor = shareSum > 0.0f ? saF / shareSum : 1.0f;
        const float dstFactor = 1.0f - srcFactor;

        const float srcConcentration = srcFactor * srcFactor * srcSpec.luminance;
        const float dstConcentration = dstFactor * dstFactor * dstLum;
        const float totalConcentration = srcConcentration + dstConcentration;

        float blendLrgb[3] = {0.0f, 0.0f, 0.0f};

        if (totalConcentration > 0.0f) {
            float mixRefl[KoSpectralMixing::SampleCount];
            for (int i = 0; i < KoSpectralMixing::SampleCount; ++i) {
                const float ksMix = (KoSpectralMixing::reflectanceToKs(dstRefl[i]) * dstConcentration
                                     + srcSpec.ks[i] * srcConcentration)
                    / totalConcentration;
                mixRefl[i] = KoSpectralMixing::ksToReflectance(ksMix);
            }
            KoSpectralMixing::reflectanceToLrgb(mixRefl, blendLrgb);
        } else {
            blendLrgb[0] = dstR;
            blendLrgb[1] = dstG;
            blendLrgb[2] = dstB;
        }

        // Encode the blend color back into channel space
        const channels_type blendRed = scale<channels_type>(fromLinear(red_pos, blendLrgb[0]));
        const channels_type blendGreen = scale<channels_type>(fromLinear(green_pos, blendLrgb[1]));
        const channels_type blendBlue = scale<channels_type>(fromLinear(blue_pos, blendLrgb[2]));

        if (alphaLocked) {
            if (channelFlags.testBit(red_pos)) {
                dst[red_pos] = lerp(dst[red_pos], blendRed, srcAlpha);
            }
            if (channelFlags.testBit(green_pos)) {
                dst[green_pos] = lerp(dst[green_pos], blendGreen, srcAlpha);
            }
            if (channelFlags.testBit(blue_pos)) {
                dst[blue_pos] = lerp(dst[blue_pos], blendBlue, srcAlpha);
            }
        } else {
            const channels_type newDstAlpha = unionShapeOpacity(srcAlpha, dstAlpha);

            if (!isZeroValueFuzzy(newDstAlpha)) {
                if (channelFlags.testBit(red_pos)) {
                    dst[red_pos] = div(blend(src[red_pos], srcAlpha, dst[red_pos], dstAlpha, blendRed), newDstAlpha);
                }
                if (channelFlags.testBit(green_pos)) {
                    dst[green_pos] =
                        div(blend(src[green_pos], srcAlpha, dst[green_pos], dstAlpha, blendGreen), newDstAlpha);
                }
                if (channelFlags.testBit(blue_pos)) {
                    dst[blue_pos] =
                        div(blend(src[blue_pos], srcAlpha, dst[blue_pos], dstAlpha, blendBlue), newDstAlpha);
                }
            }

            if (alpha_pos != -1) {
                dst[alpha_pos] = newDstAlpha;
            }
        }
    }

private:
    bool m_linearSpace{true};
    bool m_useProfileTrc{false};
    float m_linearize[numRgbChannels][trcTableSize];
    float m_delinearize[numRgbChannels][trcTableSize];
};

#endif // _KOCOMPOSITEOP_PIGMENT_H_
