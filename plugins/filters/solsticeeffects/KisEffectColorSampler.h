/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_EFFECT_COLOR_SAMPLER_H
#define KIS_EFFECT_COLOR_SAMPLER_H

#include <KoChannelInfo.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceRegistry.h>
#include <cmath>
#include <kis_paint_device.h>
#include <kis_random_accessor_ng.h>
#include <memory>

namespace SolsticeEffects
{
// Each patch owns its converters: KoColorSpace::convertPixelsTo() is not
// thread-safe. Native RGB needs no conversion, including float/HDR profiles.
class ColorSampler
{
public:
    explicit ColorSampler(KisPaintDeviceSP device)
        : source(device->createRandomConstAccessorNG())
        , native(device->colorSpace())
        , working(native->colorModelId() == RGBAColorModelID
                      ? native
                      : KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(),
                                                                     Float32BitsColorDepthID.id(),
                                                                     QString()))
        , buffer(working->pixelSize(), '\0')
        , values(working->channelCount())
    {
        for (int i = 0; i < working->channels().size(); ++i) {
            const auto *channel = working->channels()[i];
            if (channel->channelType() == KoChannelInfo::COLOR) {
                if (channel->displayPosition() == 0)
                    red = i;
                if (channel->displayPosition() == 1)
                    green = i;
                if (channel->displayPosition() == 2)
                    blue = i;
            }
        }
        if (working != native) {
            toRgb.reset(native->createColorConverter(working,
                                                     KoColorConversionTransformation::internalRenderingIntent(),
                                                     KoColorConversionTransformation::internalConversionFlags()));
            fromRgb.reset(working->createColorConverter(native,
                                                        KoColorConversionTransformation::internalRenderingIntent(),
                                                        KoColorConversionTransformation::internalConversionFlags()));
        }
    }

    void decode(const quint8 *pixel, QVector<float> &result)
    {
        if (toRgb) {
            toRgb->transform(pixel, reinterpret_cast<quint8 *>(buffer.data()), 1);
            pixel = reinterpret_cast<const quint8 *>(buffer.constData());
        }
        working->normalisedChannelsValue(pixel, result);
    }

    void encode(const QVector<float> &result, quint8 *pixel)
    {
        if (fromRgb) {
            working->fromNormalisedChannelsValue(reinterpret_cast<quint8 *>(buffer.data()), result);
            fromRgb->transform(reinterpret_cast<const quint8 *>(buffer.constData()), pixel, 1);
        } else {
            working->fromNormalisedChannelsValue(pixel, result);
        }
    }

    float sample(double x, double y, int channel, float fallback)
    {
        const int ix = int(std::floor(x));
        const int iy = int(std::floor(y));
        const double fx = x - ix;
        const double fy = y - iy;
        double sum = 0.0;
        double coverage = 0.0;
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                const double weight = (dx ? fx : 1.0 - fx) * (dy ? fy : 1.0 - fy);
                if (weight == 0.0)
                    continue;
                source->moveTo(ix + dx, iy + dy);
                const quint8 *pixel = source->oldRawData();
                const double alpha = native->opacityF(pixel);
                if (!(alpha > 0.0) || !std::isfinite(alpha))
                    continue;
                decode(pixel, values);
                if (!std::isfinite(values[channel]))
                    continue;
                sum += weight * alpha * values[channel];
                coverage += weight * alpha;
            }
        }
        // Unpremultiply the sampled color, but retain destination alpha.
        // An empty footprint retains the original channel instead of black.
        return coverage > 0.0 ? float(sum / coverage) : fallback;
    }

    KisRandomConstAccessorSP source;
    const KoColorSpace *native;
    const KoColorSpace *working;
    int red = -1;
    int green = -1;
    int blue = -1;
    QByteArray buffer;
    QVector<float> values;
    std::unique_ptr<KoColorConversionTransformation> toRgb;
    std::unique_ptr<KoColorConversionTransformation> fromRgb;
};
} // namespace SolsticeEffects

#endif
