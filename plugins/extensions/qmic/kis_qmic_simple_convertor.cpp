/*
 * SPDX-FileCopyrightText: 2013 Lukáš Tvrdý <lukast.dev@gmail.com>
 * SPDX-FileCopyrightText: 2020-2022 L. E. Segovia <amy@amyspark.me>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_qmic_simple_convertor.h"
#include "KisQmicRgbConverter.h"

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <type_traits>
#include <vector>

#include <kis_debug.h>
#include <kis_random_accessor_ng.h>

#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoColorSpaceTraits.h>
#include <KoCompositeOpRegistry.h>

#define SCALE_TO_FLOAT(v) KoColorSpaceMaths<_channel_type_, float>::scaleToA(v)

using KoColorTransformationSP = std::shared_ptr<KoColorTransformation>;

template<typename _channel_type_, typename traits>
class KisColorToFloatConvertor : public KoColorTransformation
{
    using RGBTrait = traits;
    using RGBPixel = typename RGBTrait::Pixel;

public:
    KisColorToFloatConvertor(float gmicUnitValue = 255.0f)
        : m_gmicUnitValue(gmicUnitValue)
    {
    }

    float m_gmicUnitValue;

    void
    transform(const quint8 *src, quint8 *dst, qint32 nPixels) const override
    {
        const float gmicUnitValue2KritaUnitValue =
            m_gmicUnitValue / KoColorSpaceMathsTraits<float>::unitValue;

        const auto *srcPixel = reinterpret_cast<const RGBPixel *>(src);
        auto *dstPixel = reinterpret_cast<KoRgbF32Traits::Pixel *>(dst);

        while (nPixels > 0) {
            dstPixel->red =
                SCALE_TO_FLOAT(srcPixel->red) * gmicUnitValue2KritaUnitValue;
            dstPixel->green =
                SCALE_TO_FLOAT(srcPixel->green) * gmicUnitValue2KritaUnitValue;
            dstPixel->blue =
                SCALE_TO_FLOAT(srcPixel->blue) * gmicUnitValue2KritaUnitValue;
            dstPixel->alpha =
                SCALE_TO_FLOAT(srcPixel->alpha) * gmicUnitValue2KritaUnitValue;

            --nPixels;
            ++srcPixel;
            ++dstPixel;
        }
    }
};


const std::map<QString, QString> blendingModeMap = {
    {"add", COMPOSITE_ADD},
    //   {"alpha", ""}, // XXX
    {"and", COMPOSITE_AND},
    //   {"average", ""}, // XXX
    {"blue", COMPOSITE_COPY_BLUE},
    {"burn", COMPOSITE_BURN},
    {"darken", COMPOSITE_DARKEN},
    {"difference", COMPOSITE_DIFF},
    {"divide", COMPOSITE_DIVIDE},
    {"dodge", COMPOSITE_DODGE},
    //   {"edges", ""}, // XXX
    {"exclusion", COMPOSITE_EXCLUSION},
    {"freeze", COMPOSITE_FREEZE},
    {"grainextract", COMPOSITE_GRAIN_EXTRACT},
    {"grainmerge", COMPOSITE_GRAIN_MERGE},
    {"green", COMPOSITE_COPY_GREEN},
    {"hardlight", COMPOSITE_HARD_LIGHT},
    {"hardmix", COMPOSITE_HARD_MIX},
    {"hue", COMPOSITE_HUE},
    {"interpolation", COMPOSITE_INTERPOLATION},
    {"lighten", COMPOSITE_LIGHTEN},
    {"lightness", COMPOSITE_LIGHTNESS},
    {"linearburn", COMPOSITE_LINEAR_BURN},
    {"linearlight", COMPOSITE_LINEAR_LIGHT},
    {"luminance", COMPOSITE_LUMINIZE},
    {"multiply", COMPOSITE_MULT},
    {"negation", COMPOSITE_NEGATION},
    {"or", COMPOSITE_OR},
    {"overlay", COMPOSITE_OVERLAY},
    {"pinlight", COMPOSITE_PIN_LIGHT},
    {"red", COMPOSITE_COPY_RED},
    {"reflect", COMPOSITE_REFLECT},
    {"saturation", COMPOSITE_SATURATION},
    //   {"seamless", ""},       // XXX
    //   {"seamless_mixed", ""}, // XXX
    {"screen", COMPOSITE_SCREEN},
    //   {"shapeareamax", ""},                    // XXX
    //   {"shapeareamax0", ""},                   // XXX
    //   {"shapeareamin", ""},                    // XXX
    //   {"shapeareamin0", ""},                   // XXX
    //   {"shapeaverage", ""},                    // XXX
    //   {"shapeaverage0", ""},                   // XXX
    //   {"shapemedian", ""},                     // XXX
    //   {"shapemedian0", ""},                    // XXX
    //   {"shapemin", ""},                        // XXX
    //   {"shapemin0", ""},                       // XXX
    //   {"shapemax", ""},                        // XXX
    //   {"shapemax0", ""},                       // XXX
    //   {"softburn", ""},                        // XXX
    //   {"softdodge", ""},                       // XXX
    {"softlight", COMPOSITE_SOFT_LIGHT_SVG}, // XXX Is this correct?
    //   {"stamp", ""}, // XXX
    {"subtract", COMPOSITE_SUBTRACT},
    {"value", COMPOSITE_VALUE},
    {"vividlight", COMPOSITE_VIVID_LIGHT},
    {"xor", COMPOSITE_XOR}};


static KoColorTransformation *
createTransformation(const KoColorSpace *colorSpace)
{
    KoColorTransformation *colorTransformation = nullptr;
    if (colorSpace->colorModelId() != RGBAColorModelID) {
        dbgKrita << "Unsupported color space for fast pixel transformation to "
                    "gmic pixel format"
                 << colorSpace->id();
        return nullptr;
    }

    if (colorSpace->colorDepthId() == Float32BitsColorDepthID) {
        colorTransformation =
            new KisColorToFloatConvertor<float, KoRgbTraits<float>>();
    }
#ifdef HAVE_OPENEXR
    else if (colorSpace->colorDepthId() == Float16BitsColorDepthID) {
        colorTransformation =
            new KisColorToFloatConvertor<half, KoRgbTraits<half>>();
    }
#endif
    else if (colorSpace->colorDepthId() == Integer16BitsColorDepthID) {
        colorTransformation =
            new KisColorToFloatConvertor<quint16, KoBgrTraits<quint16>>();
    } else if (colorSpace->colorDepthId() == Integer8BitsColorDepthID) {
        colorTransformation =
            new KisColorToFloatConvertor<quint8, KoBgrTraits<quint8>>();
    } else {
        dbgKrita << "Unsupported color space " << colorSpace->id()
                 << " for fast pixel transformation to gmic pixel format";
        return nullptr;
    }
    return colorTransformation;
}

void KisQmicSimpleConvertor::convertFromGmicFast(const KisQMicImage &image, KisPaintDeviceSP destination, float unit)
{
    destination->clear();
    destination->moveTo(0, 0);
    const bool converted = KisQmicRgbConverter::dispatch(destination->colorSpace(), [&](auto *tag) {
        using Traits = std::remove_pointer_t<decltype(tag)>;
        KisQmicRgbConverter::fromPlanar<Traits>(image, destination, unit);
    });
    if (!converted)
        convertFromGmicImage(image, destination, unit);
}

void KisQmicSimpleConvertor::convertToGmicImageFast(KisPaintDeviceSP source, KisQMicImage &image, QRect rect)
{
    if (rect.isEmpty())
        rect = QRect(0, 0, image.m_width, image.m_height);
    const bool converted = KisQmicRgbConverter::dispatch(source->colorSpace(), [&](auto *tag) {
        using Traits = std::remove_pointer_t<decltype(tag)>;
        KisQmicRgbConverter::toPlanar<Traits>(source, image, rect);
    });
    if (!converted)
        convertToGmicImage(source, image, rect);
}

// gmic assumes float rgba in 0.0 - 255.0
void KisQmicSimpleConvertor::convertToGmicImage(KisPaintDeviceSP dev,
                                                KisQMicImage &gmicImage,
                                                QRect rc)
{
    Q_ASSERT(!dev.isNull());
    Q_ASSERT(gmicImage.m_spectrum == 4); // rgba

    if (rc.isEmpty()) {
        rc = QRect(0,
                   0,
                   static_cast<int>(gmicImage.m_width),
                   static_cast<int>(gmicImage.m_height));
    }

    const KoColorSpace *rgbaFloat32bitcolorSpace =
        KoColorSpaceRegistry::instance()->colorSpace(
            RGBAColorModelID.id(),
            Float32BitsColorDepthID.id(),
            KoColorSpaceRegistry::instance()->rgb8()->profile());
    Q_CHECK_PTR(rgbaFloat32bitcolorSpace);

    const KoColorTransformationSP pixelToGmicPixelFormat(
        createTransformation(rgbaFloat32bitcolorSpace));

    const size_t greenOffset =
        static_cast<size_t>(gmicImage.m_width) * gmicImage.m_height;
    const size_t blueOffset = greenOffset * 2;
    const size_t alphaOffset = greenOffset * 3;

    const auto renderingIntent =
        KoColorConversionTransformation::internalRenderingIntent();
    const auto conversionFlags =
        KoColorConversionTransformation::internalConversionFlags();

    const KoColorSpace *colorSpace = dev->colorSpace();
    KisRandomConstAccessorSP it = dev->createRandomConstAccessorNG();

    const size_t optimalBufferSize =
        64; // most common numContiguousColumns, tile size?
    std::vector<quint8> floatRGBApixelStorage(
        rgbaFloat32bitcolorSpace->pixelSize() * optimalBufferSize);
    quint8 *floatRGBApixel = floatRGBApixelStorage.data();

    const auto pixelSize = rgbaFloat32bitcolorSpace->pixelSize();
    for (int y = 0; y < rc.height(); y++) {
        int x = 0;
        while (x < rc.width()) {
            it->moveTo(rc.x() + x, rc.y() + y);
            auto numContiguousColumns =
                qMin(static_cast<size_t>(it->numContiguousColumns(rc.x() + x)),
                     optimalBufferSize);
            numContiguousColumns =
                qMin(numContiguousColumns, static_cast<size_t>(rc.width() - x));

            colorSpace->convertPixelsTo(
                it->rawDataConst(),
                floatRGBApixel,
                rgbaFloat32bitcolorSpace,
                static_cast<quint32>(numContiguousColumns),
                renderingIntent,
                conversionFlags);
            pixelToGmicPixelFormat->transform(
                floatRGBApixel,
                floatRGBApixel,
                static_cast<qint32>(numContiguousColumns));

            auto pos = static_cast<size_t>(y) * gmicImage.m_width
                + static_cast<size_t>(x);
            for (size_t bx = 0; bx < numContiguousColumns; bx++) {
                memcpy(gmicImage.m_data + pos,
                       floatRGBApixel + bx * pixelSize,
                       4);
                memcpy(gmicImage.m_data + pos + greenOffset,
                       floatRGBApixel + bx * pixelSize + 4,
                       4);
                memcpy(gmicImage.m_data + pos + blueOffset,
                       floatRGBApixel + bx * pixelSize + 8,
                       4);
                memcpy(gmicImage.m_data + pos + alphaOffset,
                       floatRGBApixel + bx * pixelSize + 12,
                       4);
                pos++;
            }

            x += static_cast<int>(numContiguousColumns);
        }
    }
}

void KisQmicSimpleConvertor::convertFromGmicImage(const KisQMicImage &gmicImage,
                                                  KisPaintDeviceSP dst,
                                                  float gmicMaxChannelValue)
{
    dbgPlugins << "convertFromGmicSlow";
    Q_ASSERT(!dst.isNull());
    const KoColorSpace *rgbaFloat32bitcolorSpace =
        KoColorSpaceRegistry::instance()->colorSpace(
            RGBAColorModelID.id(),
            Float32BitsColorDepthID.id(),
            KoColorSpaceRegistry::instance()->rgb8()->profile());
    const KoColorSpace *dstColorSpace = dst->colorSpace();

    KisPaintDeviceSP dev = dst;
    const size_t greenOffset =
        static_cast<size_t>(gmicImage.m_width) * gmicImage.m_height;
    const size_t blueOffset = greenOffset * 2;
    const size_t alphaOffset = greenOffset * 3;
    QRect rc(0,
             0,
             static_cast<int>(gmicImage.m_width),
             static_cast<int>(gmicImage.m_height));

    KisRandomAccessorSP it = dev->createRandomAccessorNG();
    float r = 0;
    float g = 0;
    float b = 0;
    float a = 1.0;

    const size_t optimalBufferSize =
        64; // most common numContiguousColumns, tile size?
    std::vector<quint8> floatRGBApixelStorage(
        rgbaFloat32bitcolorSpace->pixelSize() * optimalBufferSize);
    quint8 *floatRGBApixel = floatRGBApixelStorage.data();
    const auto pixelSize = rgbaFloat32bitcolorSpace->pixelSize();

    const auto renderingIntent =
        KoColorConversionTransformation::internalRenderingIntent();
    const auto conversionFlags =
        KoColorConversionTransformation::internalConversionFlags();

    // Krita needs rgba in 0.0...1.0
    const float multiplied =
        KoColorSpaceMathsTraits<float>::unitValue / gmicMaxChannelValue;

    switch (gmicImage.m_spectrum) {
    case 1: {
        // convert grayscale to rgba
        for (int y = 0; y < rc.height(); y++) {
            int x = 0;
            while (x < rc.width()) {
                it->moveTo(x, y);
                auto numContiguousColumns =
                    qMin(static_cast<size_t>(it->numContiguousColumns(x)),
                         optimalBufferSize);
                numContiguousColumns =
                    qMin(numContiguousColumns,
                         static_cast<size_t>(rc.width() - x));

                size_t pos = static_cast<size_t>(y) * gmicImage.m_width
                    + static_cast<size_t>(x);
                for (size_t bx = 0; bx < numContiguousColumns; bx++) {
                    r = g = b = gmicImage.m_data[pos] * multiplied;
                    a = KoColorSpaceMathsTraits<float>::unitValue;

                    memcpy(floatRGBApixel + bx * pixelSize, &r, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 4, &g, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 8, &b, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 12, &a, 4);
                    pos++;
                }
                rgbaFloat32bitcolorSpace->convertPixelsTo(
                    floatRGBApixel,
                    it->rawData(),
                    dstColorSpace,
                    static_cast<quint32>(numContiguousColumns),
                    renderingIntent,
                    conversionFlags);
                x += static_cast<int>(numContiguousColumns);
            }
        }
        break;
    }
    case 2: {
        // convert grayscale alpha to rgba
        for (int y = 0; y < rc.height(); y++) {
            int x = 0;
            while (x < rc.width()) {
                it->moveTo(x, y);
                auto numContiguousColumns =
                    qMin(static_cast<size_t>(it->numContiguousColumns(x)),
                         optimalBufferSize);
                numContiguousColumns =
                    qMin(numContiguousColumns,
                         static_cast<size_t>(rc.width() - x));

                size_t pos = static_cast<size_t>(y) * gmicImage.m_width
                    + static_cast<size_t>(x);
                for (size_t bx = 0; bx < numContiguousColumns; bx++) {
                    r = g = b = gmicImage.m_data[pos] * multiplied;
                    a = gmicImage.m_data[pos + greenOffset] * multiplied;

                    memcpy(floatRGBApixel + bx * pixelSize, &r, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 4, &g, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 8, &b, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 12, &a, 4);
                    pos++;
                }
                rgbaFloat32bitcolorSpace->convertPixelsTo(
                    floatRGBApixel,
                    it->rawData(),
                    dstColorSpace,
                    static_cast<quint32>(numContiguousColumns),
                    renderingIntent,
                    conversionFlags);
                x += static_cast<int>(numContiguousColumns);
            }
        }
        break;
    }
    case 3: {
        // convert rgb -> rgba
        for (int y = 0; y < rc.height(); y++) {
            int x = 0;
            while (x < rc.width()) {
                it->moveTo(x, y);
                auto numContiguousColumns =
                    qMin(static_cast<size_t>(it->numContiguousColumns(x)),
                         optimalBufferSize);
                numContiguousColumns =
                    qMin(numContiguousColumns,
                         static_cast<size_t>(rc.width() - x));

                size_t pos = static_cast<size_t>(y) * gmicImage.m_width
                    + static_cast<size_t>(x);
                for (size_t bx = 0; bx < numContiguousColumns; bx++) {
                    r = gmicImage.m_data[pos] * multiplied;
                    g = gmicImage.m_data[pos + greenOffset] * multiplied;
                    b = gmicImage.m_data[pos + blueOffset] * multiplied;
                    a = gmicMaxChannelValue * multiplied;

                    memcpy(floatRGBApixel + bx * pixelSize, &r, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 4, &g, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 8, &b, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 12, &a, 4);
                    pos++;
                }
                rgbaFloat32bitcolorSpace->convertPixelsTo(
                    floatRGBApixel,
                    it->rawData(),
                    dstColorSpace,
                    static_cast<quint32>(numContiguousColumns),
                    renderingIntent,
                    conversionFlags);
                x += static_cast<int>(numContiguousColumns);
            }
        }
        break;
    }
    case 4: {
        for (int y = 0; y < rc.height(); y++) {
            int x = 0;
            while (x < rc.width()) {
                it->moveTo(x, y);
                auto numContiguousColumns =
                    qMin(static_cast<size_t>(it->numContiguousColumns(x)),
                         optimalBufferSize);
                numContiguousColumns =
                    qMin(numContiguousColumns,
                         static_cast<size_t>(rc.width() - x));

                size_t pos = static_cast<size_t>(y) * gmicImage.m_width
                    + static_cast<size_t>(x);
                for (size_t bx = 0; bx < numContiguousColumns; bx++) {
                    r = gmicImage.m_data[pos] * multiplied;
                    g = gmicImage.m_data[pos + greenOffset] * multiplied;
                    b = gmicImage.m_data[pos + blueOffset] * multiplied;
                    a = gmicImage.m_data[pos + alphaOffset] * multiplied;

                    memcpy(floatRGBApixel + bx * pixelSize, &r, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 4, &g, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 8, &b, 4);
                    memcpy(floatRGBApixel + bx * pixelSize + 12, &a, 4);
                    pos++;
                }
                rgbaFloat32bitcolorSpace->convertPixelsTo(
                    floatRGBApixel,
                    it->rawData(),
                    dstColorSpace,
                    static_cast<quint32>(numContiguousColumns),
                    renderingIntent,
                    conversionFlags);
                x += static_cast<int>(numContiguousColumns);
            }
        }
        break;
    }

    default: {
        dbgPlugins << "Unsupported gmic output format : " << gmicImage.m_width
                   << gmicImage.m_height << gmicImage.m_spectrum;
    }
    }
}

QImage KisQmicSimpleConvertor::convertToQImage(const KisQMicImage &gmicImage,
                                               float gmicActualMaxChannelValue)
{
    QImage image = QImage(static_cast<int>(gmicImage.m_width),
                          static_cast<int>(gmicImage.m_height),
                          QImage::Format_ARGB32);

    dbgPlugins << image.format() << "first pixel:" << gmicImage.m_data[0]
               << gmicImage.m_width << gmicImage.m_height
               << gmicImage.m_spectrum;

    const size_t greenOffset =
        static_cast<size_t>(gmicImage.m_width) * gmicImage.m_height;
    const size_t blueOffset = greenOffset * 2;

    // always put 255 to qimage
    const float multiplied = 255.0f / gmicActualMaxChannelValue;

    for (int y = 0; y < gmicImage.m_height; y++) {
        QRgb *pixel =
            reinterpret_cast<QRgb *>(image.scanLine(static_cast<int>(y)));
        for (int x = 0; x < gmicImage.m_width; x++) {
            const auto pos = y * gmicImage.m_width + x;
            const float r = gmicImage.m_data[pos] * multiplied;
            const float g = gmicImage.m_data[pos + greenOffset] * multiplied;
            const float b = gmicImage.m_data[pos + blueOffset] * multiplied;
            pixel[x] = qRgb(int(r), int(g), int(b));
        }
    }
    return image;
}

void KisQmicSimpleConvertor::convertFromQImage(const QImage &image,
                                               KisQMicImage &gmicImage,
                                               float gmicUnitValue)
{
    const auto greenOffset =
        static_cast<size_t>(gmicImage.m_width) * gmicImage.m_height;
    const size_t blueOffset = greenOffset * 2;
    const size_t alphaOffset = greenOffset * 3;

    // QImage has 0..255
    const float qimageUnitValue = 255.0f;
    const float multiplied = gmicUnitValue / qimageUnitValue;

    Q_ASSERT(image.width() == int(gmicImage.m_width));
    Q_ASSERT(image.height() == int(gmicImage.m_height));
    Q_ASSERT(image.format() == QImage::Format_ARGB32);

    switch (gmicImage.m_spectrum) {
    case 1: {
        for (int y = 0; y < image.height(); y++) {
            const QRgb *pixel =
                reinterpret_cast<const QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); x++) {
                const auto pos = static_cast<size_t>(y) * gmicImage.m_width
                    + static_cast<size_t>(x);
                gmicImage.m_data[pos] =
                    static_cast<float>(qGray(pixel[x])) * multiplied;
            }
        }
        break;
    }
    case 2: {
        for (int y = 0; y < image.height(); y++) {
            const QRgb *pixel =
                reinterpret_cast<const QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); x++) {
                const auto pos = static_cast<size_t>(y) * gmicImage.m_width
                    + static_cast<size_t>(x);
                gmicImage.m_data[pos] =
                    static_cast<float>(qGray(pixel[x])) * multiplied;
                gmicImage.m_data[pos + greenOffset] =
                    static_cast<float>(qAlpha(pixel[x])) * multiplied;
            }
        }
        break;
    }
    case 3: {
        for (int y = 0; y < image.height(); y++) {
            const QRgb *pixel =
                reinterpret_cast<const QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); x++) {
                const auto pos = static_cast<size_t>(y) * gmicImage.m_width
                    + static_cast<size_t>(x);
                gmicImage.m_data[pos] =
                    static_cast<float>(qRed(pixel[x])) * multiplied;
                gmicImage.m_data[pos + greenOffset] =
                    static_cast<float>(qGreen(pixel[x])) * multiplied;
                gmicImage.m_data[pos + blueOffset] =
                    static_cast<float>(qBlue(pixel[x])) * multiplied;
            }
        }
        break;
    }
    case 4: {
        for (int y = 0; y < image.height(); y++) {
            const QRgb *pixel =
                reinterpret_cast<const QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); x++) {
                const auto pos = static_cast<size_t>(y) * gmicImage.m_width
                    + static_cast<size_t>(x);
                gmicImage.m_data[pos] =
                    static_cast<float>(qRed(pixel[x])) * multiplied;
                gmicImage.m_data[pos + greenOffset] =
                    static_cast<float>(qGreen(pixel[x])) * multiplied;
                gmicImage.m_data[pos + blueOffset] =
                    static_cast<float>(qBlue(pixel[x])) * multiplied;
                gmicImage.m_data[pos + alphaOffset] =
                    static_cast<float>(qAlpha(pixel[x])) * multiplied;
            }
        }
        break;
    }
    default: {
        Q_ASSERT(false);
        dbgKrita << "Unexpected gmic image format";
        break;
    }
    }
}

std::map<QString, QString> reverseMap()
{
    std::map<QString, QString> result{};
    for (const auto &pair : blendingModeMap) {
        result.emplace(pair.second, pair.first);
    }
    return result;
}

QString KisQmicSimpleConvertor::blendingModeToString(QString blendMode)
{
    static auto reverseModeMap = reverseMap();
    if (reverseModeMap.find(blendMode) != reverseModeMap.end())
        return reverseModeMap.at(blendMode);
    return "alpha";
}

QString KisQmicSimpleConvertor::stringToBlendingMode(QString blendMode)
{
    if (blendingModeMap.find(blendMode) != blendingModeMap.end())
        return blendingModeMap.at(blendMode);
    return COMPOSITE_OVER;
}

QString KisQmicSimpleConvertor::nameToGMicName(QString name)
{
    name.replace(QChar('('), QChar(21)).replace(QChar(')'), QChar(22));
    return name;
}

QString KisQmicSimpleConvertor::gMicNameToName(QString name)
{
    name.replace(QChar(21), QChar('(')).replace(QChar(22), QChar(')'));
    return name;
}
