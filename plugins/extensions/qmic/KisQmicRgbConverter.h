/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_QMIC_RGB_CONVERTER_H
#define KIS_QMIC_RGB_CONVERTER_H

#include "kis_qmic_interface.h"
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceTraits.h>
#include <kis_paint_device.h>
#include <kis_random_accessor_ng.h>

namespace KisQmicRgbConverter
{
template<typename Operation>
bool dispatch(const KoColorSpace *cs, const Operation &operation)
{
    if (cs->colorModelId() != RGBAColorModelID)
        return false;
    if (cs->colorDepthId() == Integer8BitsColorDepthID) {
        operation(static_cast<KoBgrU8Traits *>(nullptr));
    } else if (cs->colorDepthId() == Integer16BitsColorDepthID) {
        operation(static_cast<KoBgrU16Traits *>(nullptr));
#ifdef HAVE_OPENEXR
    } else if (cs->colorDepthId() == Float16BitsColorDepthID) {
        operation(static_cast<KoRgbF16Traits *>(nullptr));
#endif
    } else if (cs->colorDepthId() == Float32BitsColorDepthID) {
        operation(static_cast<KoRgbF32Traits *>(nullptr));
    } else {
        return false;
    }
    return true;
}

template<typename Traits>
void toPlanar(KisPaintDeviceSP source, KisQMicImage &image, const QRect &rect)
{
    using T = typename Traits::channels_type;
    using Pixel = typename Traits::Pixel;
    const size_t planeSize = size_t(image.m_width) * image.m_height;
    auto accessor = source->createRandomConstAccessorNG();
    // Scale and deinterleave in one pass, with the original arithmetic order.
    for (int y = 0; y < rect.height();) {
        const int rows = qMin(accessor->numContiguousRows(rect.y() + y), rect.height() - y);
        for (int x = 0; x < rect.width();) {
            const int px = rect.x() + x, py = rect.y() + y;
            const int columns = qMin(accessor->numContiguousColumns(px), rect.width() - x);
            accessor->moveTo(px, py);
            const quint8 *tile = accessor->rawDataConst();
            const int stride = accessor->rowStride(px, py);
            for (int row = 0; row < rows; ++row) {
                const auto *src = reinterpret_cast<const Pixel *>(tile + row * stride);
                const size_t offset = size_t(y + row) * image.m_width + x;
                float *red = image.m_data + offset;
                float *green = red + planeSize;
                float *blue = green + planeSize;
                float *alpha = blue + planeSize;
                for (int col = 0; col < columns; ++col) {
                    red[col] = KoColorSpaceMaths<T, float>::scaleToA(src[col].red) * 255.0f;
                    green[col] = KoColorSpaceMaths<T, float>::scaleToA(src[col].green) * 255.0f;
                    blue[col] = KoColorSpaceMaths<T, float>::scaleToA(src[col].blue) * 255.0f;
                    alpha[col] = KoColorSpaceMaths<T, float>::scaleToA(src[col].alpha) * 255.0f;
                }
            }
            x += columns;
        }
        y += rows;
    }
}

template<typename Traits, bool HasAlpha>
void fromPlanarImpl(const KisQMicImage &image, KisPaintDeviceSP destination, float unit)
{
    using T = typename Traits::channels_type;
    using Pixel = typename Traits::Pixel;
    const size_t planeSize = size_t(image.m_width) * image.m_height;
    const float factor = KoColorSpaceMathsTraits<float>::unitValue / unit;
    const float *red = image.m_data;
    const float *green = image.m_spectrum < 3 ? red : red + planeSize;
    const float *blue = image.m_spectrum < 3 ? red : red + 2 * planeSize;
    const float *alpha = image.m_spectrum == 2 ? red + planeSize
        : image.m_spectrum == 4                ? red + 3 * planeSize
                                               : nullptr;
    auto accessor = destination->createRandomAccessorNG();
    for (int y = 0; y < image.m_height;) {
        const int rows = qMin(accessor->numContiguousRows(y), image.m_height - y);
        for (int x = 0; x < image.m_width;) {
            const int columns = qMin(accessor->numContiguousColumns(x), image.m_width - x);
            accessor->moveTo(x, y);
            quint8 *tile = accessor->rawData();
            const int stride = accessor->rowStride(x, y);
            for (int row = 0; row < rows; ++row) {
                Pixel *__restrict dst = reinterpret_cast<Pixel *>(tile + row * stride);
                const size_t offset = size_t(y + row) * image.m_width + x;
                for (int col = 0; col < columns; ++col) {
                    const size_t i = offset + col;
                    dst[col].red = KoColorSpaceMaths<float, T>::scaleToA(red[i] * factor);
                    dst[col].green = KoColorSpaceMaths<float, T>::scaleToA(green[i] * factor);
                    dst[col].blue = KoColorSpaceMaths<float, T>::scaleToA(blue[i] * factor);
                    dst[col].alpha = KoColorSpaceMaths<float, T>::scaleToA((HasAlpha ? alpha[i] : unit) * factor);
                }
            }
            x += columns;
        }
        y += rows;
    }
    accessor.clear();
    destination->crop(0, 0, image.m_width, image.m_height);
}

template<typename Traits>
void fromPlanar(const KisQMicImage &image, KisPaintDeviceSP destination, float unit)
{
    if (image.m_spectrum == 2 || image.m_spectrum == 4) {
        fromPlanarImpl<Traits, true>(image, destination, unit);
    } else {
        fromPlanarImpl<Traits, false>(image, destination, unit);
    }
}
} // namespace KisQmicRgbConverter
#endif
