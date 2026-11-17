/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisLensBlurHighlights.h"
#include <KisSequentialIteratorProgress.h>
#include <KoChannelInfo.h>
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
#include <cmath>
#include <cstring>
#include <filter/kis_filter_configuration.h>
#include <kis_convolution_kernel.h>
#include <kis_convolution_painter.h>
#include <kis_default_bounds.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <kis_random_accessor_ng.h>
#include <memory>

namespace
{
// RGB and gray retain native channel encoding. Other models use private
// per-call converters, avoiding the shared convertPixelsTo() cache in workers.
class WorkingColor
{
public:
    WorkingColor(const KoColorSpace *native, const KoColorSpace *rgb)
        : native(native)
        , rgb(rgb)
        , values(native->channelCount())
        , buffer(native->pixelSize(), '\0')
    {
        gray = native->colorModelId() == GrayAColorModelID;
        direct = gray || native->colorModelId() == RGBAColorModelID;
        for (int i = 0; i < values.size(); ++i) {
            const auto *channel = native->channels()[i];
            if (channel->channelType() == KoChannelInfo::COLOR && channel->displayPosition() < 3)
                indices[channel->displayPosition()] = i;
            if (channel->channelType() == KoChannelInfo::ALPHA)
                alpha = i;
        }
        if (!direct) {
            toRgb.reset(native->createColorConverter(rgb,
                                                     KoColorConversionTransformation::internalRenderingIntent(),
                                                     KoColorConversionTransformation::internalConversionFlags()));
            fromRgb.reset(rgb->createColorConverter(native,
                                                    KoColorConversionTransformation::internalRenderingIntent(),
                                                    KoColorConversionTransformation::internalConversionFlags()));
        }
    }
    void decode(const quint8 *pixel, float *out)
    {
        if (!direct) {
            toRgb->transform(pixel, reinterpret_cast<quint8 *>(out), 1);
            return;
        }
        native->normalisedChannelsValue(pixel, values);
        for (int i = 0; i < 3; ++i)
            out[i] = values[indices[gray ? 0 : i]];
        out[3] = alpha >= 0 ? values[alpha] : 1.0f;
    }
    const quint8 *encode(const float *pixel)
    {
        auto *out = reinterpret_cast<quint8 *>(buffer.data());
        if (!direct) {
            fromRgb->transform(reinterpret_cast<const quint8 *>(pixel), out, 1);
            return out;
        }
        for (int i = 0; i < (gray ? 1 : 3); ++i)
            values[indices[i]] = pixel[i];
        if (alpha >= 0)
            values[alpha] = pixel[3];
        native->fromNormalisedChannelsValue(out, values);
        return out;
    }
    const KoColorSpace *native;
    const KoColorSpace *rgb;
    QVector<float> values;
    QByteArray buffer;
    bool direct = false, gray = false;
    int indices[3] = {0, 0, 0};
    int alpha = -1;
    std::unique_ptr<KoColorConversionTransformation> toRgb, fromRgb;
};
} // namespace

double KisLensBlurHighlights::boundedPercent(const KisFilterConfigurationSP config, const char *key)
{
    const double value = config->getDouble(key, 0.0);
    return std::isfinite(value) ? qBound(0.0, value, 100.0) : 0.0;
}

void KisLensBlurHighlights::apply(KisPaintDeviceSP device,
                                  const QRect &rect,
                                  const QRect &sourceRect,
                                  const KisFilterConfigurationSP config,
                                  KisConvolutionKernelSP kernel,
                                  KoUpdater *progress,
                                  KisConvolutionPainter::EnginePreference engine)
{
    const auto *native = device->colorSpace();
    const auto *rgb =
        KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(), QString());
    WorkingColor color(native, rgb);
    const double factor = 1.0 + 8.0 * boundedPercent(config, "highlightStrength") / 100.0;
    constexpr double threshold = 0.75;
    const double split = threshold / factor;
    auto source = device->createRandomConstAccessorNG();
    KisPaintDeviceSP prepared = new KisPaintDevice(rgb);
    // Match the legacy repeat-border domain. Fill the halo explicitly before
    // using BORDER_IGNORE so scratch bounds can never move a source edge.
    QRect bounds = device->defaultBounds()->bounds();
    if (bounds == KisDefaultBounds().bounds())
        bounds = device->exactBounds();
    bounds |= rect;
    const bool wrap = device->defaultBounds()->wrapAroundMode() && device->supportsWraproundMode();
    if (progress)
        progress->setProgress(0);
    {
        KisSequentialIterator it(prepared, sourceRect);
        while (it.nextPixel()) {
            if (it.x() == sourceRect.x() && progress && progress->interrupted())
                return;
            source->moveTo(wrap ? it.x() : qBound(bounds.left(), it.x(), bounds.right()),
                           wrap ? it.y() : qBound(bounds.top(), it.y(), bounds.bottom()));
            float value[4];
            color.decode(source->oldRawData(), value);
            value[3] = std::isfinite(value[3]) ? qBound(0.0f, value[3], 1.0f) : 0.0f;
            for (int i = 0; i < 3; ++i) {
                const double v = value[3] > 0.0f && std::isfinite(value[i]) ? value[i] : 0.0;
                // Convex, piecewise-linear map. Unlike a power, this does not
                // overflow HDR highlights or amplify negative float values.
                value[i] = float(v <= threshold ? v / factor : v - threshold + split);
            }
            std::memcpy(it.rawData(), value, sizeof(value));
        }
    }
    KisPaintDeviceSP blurred = new KisPaintDevice(rgb);
    {
        KisConvolutionPainter painter(blurred, engine);
        painter.setProgress(progress);
        // Always convolve coverage too, even if the user's alpha is locked.
        // The final native copy applies the requested channel mask.
        painter.applyMatrix(kernel, prepared, rect.topLeft(), rect.topLeft(), rect.size(), BORDER_IGNORE);
    }
    prepared.clear();
    if (progress && progress->interrupted())
        return;
    KisPaintDeviceSP output = new KisPaintDevice(native);
    output->setX(device->x());
    output->setY(device->y());
    output->setDefaultPixel(device->defaultPixel());
    const auto channels = native->channels();
    const auto flags = config->channelFlags();
    auto result = blurred->createRandomConstAccessorNG();
    {
        KisSequentialIterator it(output, rect);
        while (it.nextPixel()) {
            if (it.x() == rect.x() && progress && progress->interrupted())
                return;
            source->moveTo(it.x(), it.y());
            std::memcpy(it.rawData(), source->oldRawData(), native->pixelSize());
            result->moveTo(it.x(), it.y());
            float value[4];
            std::memcpy(value, result->rawDataConst(), sizeof(value));
            for (int i = 0; i < 3; ++i) {
                const double v = value[i];
                value[i] = float(v <= split ? v * factor : v + threshold - split);
            }
            const auto *encoded = color.encode(value);
            for (int i = 0; i < channels.size(); ++i) {
                if (!flags.isEmpty() && (i >= flags.size() || !flags.testBit(i)))
                    continue;
                // Keep hidden color when no coverage survives convolution.
                if (channels[i]->channelType() == KoChannelInfo::COLOR && !(value[3] > 0.0f))
                    continue;
                std::memcpy(it.rawData() + channels[i]->pos(), encoded + channels[i]->pos(), channels[i]->size());
            }
        }
    }
    result.clear();
    source.clear();
    if (progress && progress->interrupted())
        return;
    KisPainter::copyAreaOptimized(rect.topLeft(), output, device, rect);
    if (progress)
        progress->setProgress(100);
}
