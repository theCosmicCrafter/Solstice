/*
 *  SPDX-FileCopyrightText: 1999 Matthias Elter <me@kde.org>
 *  SPDX-FileCopyrightText: 2003 Patrick Julien <freak@codepimps.org>
 *  SPDX-FileCopyrightText: 2004-2008 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2004 Adrian Page <adrian@pagenet.plus.com>
 *  SPDX-FileCopyrightText: 2005 Bart Coppens <kde@bartcoppens.be>
 *  SPDX-FileCopyrightText: 2007 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_brush.h"

#include <QDomElement>
#include <QFile>
#include <QPainterPath>
#include <QPoint>
#include <QBuffer>

#include <kis_debug.h>
#include <klocalizedstring.h>

#include <KoColor.h>
#include <KoColorSpaceMaths.h>
#include <KoColorSpaceRegistry.h>

#include "kis_datamanager.h"
#include "kis_paint_device.h"
#include "kis_global.h"
#include "kis_boundary.h"
#include "kis_image.h"
#include "kis_iterator_ng.h"
#include "kis_brush_registry.h"
#include <brushengine/kis_paint_information.h>
#include <kis_fixed_paint_device.h>
#include <kis_qimage_pyramid.h>
#include <brushengine/kis_paintop_lod_limitations.h>
#include <resources/KoAbstractGradient.h>
#include <resources/KoCachedGradient.h>
#include <KoResource.h>
#include <KoResourceServerProvider.h>
#include <KisLazySharedCacheStorage.h>
#include <KisOptimizedBrushOutline.h>
#include <KisStaticInitializer.h>

// GPU engine (Solstice): proceduralImageDab()
#include "kis_auto_brush.h"
#include "kis_imagepipe_brush.h"
#include "kis_text_brush.h"
#include <KisProceduralCircleDab.h>
#include <KoColorConversionTransformation.h>
#include <KoColorModelStandardIds.h>
#include <KoColorProfile.h>
#include <QMutex>
#include <QRandomGenerator>

KIS_DECLARE_STATIC_INITIALIZER {
    qRegisterMetaType<KisBrushSP>("KisBrushSP");
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QMetaType::registerEqualsComparator<KisBrushSP>();
#endif
}

const QString KisBrush::brushTypeMetaDataKey = "image-based-brush";

KisBrush::ColoringInformation::~ColoringInformation()
{
}

KisBrush::PlainColoringInformation::PlainColoringInformation(const quint8* color) : m_color(color)
{
}

KisBrush::PlainColoringInformation::~PlainColoringInformation()
{
}

const quint8* KisBrush::PlainColoringInformation::color() const
{
    return m_color;
}

void KisBrush::PlainColoringInformation::nextColumn()
{
}

void KisBrush::PlainColoringInformation::nextRow()
{
}

KisBrush::PaintDeviceColoringInformation::PaintDeviceColoringInformation(const KisPaintDeviceSP source, int width)
    : m_source(source)
    , m_iterator(m_source->createHLineConstIteratorNG(0, 0, width))
{
}

KisBrush::PaintDeviceColoringInformation::~PaintDeviceColoringInformation()
{
}

const quint8* KisBrush::PaintDeviceColoringInformation::color() const
{
    return m_iterator->oldRawData();
}

void KisBrush::PaintDeviceColoringInformation::nextColumn()
{
    m_iterator->nextPixel();
}
void KisBrush::PaintDeviceColoringInformation::nextRow()
{
    m_iterator->nextRow();
}

namespace detail {
KisOptimizedBrushOutline* outlineFactory(const KisBrush *brush) {
    KisFixedPaintDeviceSP dev = brush->outlineSourceImage();

    KisBoundary boundary(dev);
    boundary.generateBoundary();
    return new KisOptimizedBrushOutline(boundary.path(), dev->bounds());
}
}

struct KisBrush::Private {
    Private()
        : brushType(INVALID)
        , brushApplication(ALPHAMASK)
        , width(0)
        , height(0)
        , spacing (1.0)
        , hasColor(false)
        , angle(0)
        , scale(1.0)
        , gradient(0)
        , autoSpacingActive(false)
        , autoSpacingCoeff(1.0)
        , threadingAllowed(true)
        , brushPyramid([] (const KisBrush* brush)
                       {
                           return new KisQImagePyramid(brush->brushTipImage());
                       })
        , brushOutline(&detail::outlineFactory)

    {
    }

    Private(const Private &rhs)
        : brushType(rhs.brushType),
          brushApplication(rhs.brushApplication),
          width(rhs.width),
          height(rhs.height),
          spacing(rhs.spacing),
          hotSpot(rhs.hotSpot),
          hasColor(rhs.hasColor),
          angle(rhs.angle),
          scale(rhs.scale),
          autoSpacingActive(rhs.autoSpacingActive),
          autoSpacingCoeff(rhs.autoSpacingCoeff),
          threadingAllowed(rhs.threadingAllowed),
          brushTipImage(rhs.brushTipImage),
          /**
           * Be careful! The pyramid is shared between two brush objects,
           * therefore you cannot change it, only recreate! That is the
           * reason why it is defined as const!
           *
           * Take it also into account that the object is defined as
           * KisLazySharedCacheStorage**Linked**, that is, when a cloned
           * object updates the cache, the cache of the source object is
           * also updated. The two caches are detached only when any of
           * the objects calls cache.reset().
           */
          brushPyramid(rhs.brushPyramid),
          brushOutline(rhs.brushOutline)
    {
        gradient = rhs.gradient;
        if (rhs.cachedGradient) {
            cachedGradient = rhs.cachedGradient->clone().staticCast<KoCachedGradient>();
        }
    }

    ~Private() {
    }

    enumBrushType brushType;
    enumBrushApplication brushApplication;

    qint32 width;
    qint32 height;
    double spacing;
    QPointF hotSpot;
    bool hasColor;
    qreal angle;
    qreal scale;

    KoAbstractGradientSP gradient;
    QSharedPointer<KoCachedGradient> cachedGradient;

    bool autoSpacingActive;
    qreal autoSpacingCoeff;
    bool threadingAllowed;

    QImage brushTipImage;
    mutable KisLazySharedCacheStorageLinked<KisQImagePyramid, const KisBrush*> brushPyramid;
    mutable KisLazySharedCacheStorageLinked<KisOptimizedBrushOutline, const KisBrush*> brushOutline;
};

KisBrush::KisBrush()
    : KoResource(QString())
    , d(new Private)
{
}

KisBrush::KisBrush(const QString& filename)
    : KoResource(filename)
    , d(new Private)
{
}

KisBrush::KisBrush(const KisBrush& rhs)
    : KoResource(rhs)
    , d(new Private(*rhs.d))
{
}

KisBrush::~KisBrush()
{
    delete d;
}

QImage KisBrush::brushTipImage() const
{
    KIS_SAFE_ASSERT_RECOVER_NOOP(!d->brushTipImage.isNull());
    return d->brushTipImage;
}

qint32 KisBrush::width() const
{
    return d->width;
}

void KisBrush::setWidth(qint32 width)
{
    d->width = width;
}

qint32 KisBrush::height() const
{
    return d->height;
}

void KisBrush::setHeight(qint32 height)
{
    d->height = height;
}

void KisBrush::setHotSpot(QPointF pt)
{
    double x = pt.x();
    double y = pt.y();

    if (x < 0)
        x = 0;
    else if (x >= width())
        x = width() - 1;

    if (y < 0)
        y = 0;
    else if (y >= height())
        y = height() - 1;

    d->hotSpot = QPointF(x, y);
}

QPointF KisBrush::hotSpot(KisDabShape const& shape, const KisPaintInformation& info) const
{
    Q_UNUSED(info);

    QSizeF metric = characteristicSize(shape);

    qreal w = metric.width();
    qreal h = metric.height();

    // The smallest brush we can produce is a single pixel.
    if (w < 1) {
        w = 1;
    }

    if (h < 1) {
        h = 1;
    }

    // XXX: This should take d->hotSpot into account, though it
    // isn't specified by gimp brushes so it would default to the center
    // anyway.
    QPointF p(w / 2, h / 2);
    return p;
}

void KisBrush::setBrushApplication(enumBrushApplication brushApplication)
{
    if (d->brushApplication != brushApplication) {
        d->brushApplication = brushApplication;
        clearBrushPyramid();
    }
}

enumBrushApplication KisBrush::brushApplication() const
{
    return d->brushApplication;
}

bool KisBrush::preserveLightness() const
{
    return d->brushApplication == LIGHTNESSMAP;
}

bool KisBrush::applyingGradient() const
{
    return d->brushApplication == GRADIENTMAP;
}

void KisBrush::setGradient(KoAbstractGradientSP gradient) {
    if (gradient && gradient->valid()) {
        d->gradient = gradient;

        if (!d->cachedGradient) {
            d->cachedGradient = toQShared(new KoCachedGradient(d->gradient, 256, d->gradient->colorSpace()));
        } else {
            d->cachedGradient->setGradient(d->gradient, 256, d->gradient->colorSpace());
        }
    }
}

bool KisBrush::isPiercedApprox() const
{
    QImage image = brushTipImage();

    qreal w = image.width();
    qreal h = image.height();

    if (w < 3 && h < 3) { return false; }

    qreal xPortion = qMin(0.1, 5.0 / w);
    qreal yPortion = qMin(0.1, 5.0 / h);

    int x0 = std::floor((0.5 - xPortion) * w);
    int x1 = std::ceil((0.5 + xPortion) * w);

    int y0 = std::floor((0.5 - yPortion) * h);
    int y1 = std::ceil((0.5 + yPortion) * h);

    const int maxNumSamples = (x1 - x0 + 1) * (y1 - y0 + 1);
    const int failedPixelsThreshold = 0.1 * maxNumSamples;
    const int thresholdValue = 0.95 * 255;
    int failedPixels = 0;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            QRgb pixel = image.pixel(x,y);

            if (qRed(pixel) > thresholdValue) {
                failedPixels++;
            }
        }
    }

    return failedPixels > failedPixelsThreshold;
}

namespace {
void fetchPremultipliedRed(const QRgb* src, quint8 *dst, int maskWidth)
{
    for (int x = 0; x < maskWidth; x++) {
        *dst = KoColorSpaceMaths<quint8>::multiply(255 - *src, qAlpha(*src));
        src++;
        dst++;
    }
}
}

KisFixedPaintDeviceSP KisBrush::outlineSourceImage() const
{
    /**
     * We need to generate the mask manually, skipping the
     * construction of the image pyramid
     */

    const KoColorSpace* cs = KoColorSpaceRegistry::instance()->alpha8();
    KisFixedPaintDeviceSP dev = new KisFixedPaintDevice(cs);
    const QImage image = brushTipImage().convertToFormat(QImage::Format_ARGB32);

    dev->setRect(image.rect());
    dev->lazyGrowBufferWithoutInitialization();

    const int maskWidth = image.width();
    const int maskHeight = image.height();

    quint8 *dstPtr = dev->data();

    for (int y = 0; y < maskHeight; y++) {
        const QRgb* maskPointer = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        fetchPremultipliedRed(maskPointer, dstPtr, maskWidth);
        dstPtr += maskWidth;
    }

    return dev;
}

bool KisBrush::canPaintFor(const KisPaintInformation& /*info*/)
{
    return true;
}

void KisBrush::setBrushTipImage(const QImage& image)
{
    d->brushTipImage = image;

    if (!image.isNull()) {
        if (image.width() > 128 || image.height() > 128) {
            KoResource::setImage(image.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        else {
            KoResource::setImage(image);
        }
        setWidth(image.width());
        setHeight(image.height());
    }
    clearBrushPyramid();
    resetOutlineCache();
}

void KisBrush::setBrushType(enumBrushType type)
{
    d->brushType = type;
    addMetaData(brushTypeMetaDataKey,
                QVariant::fromValue(type == IMAGE || type == PIPE_IMAGE));
}

enumBrushType KisBrush::brushType() const
{
    return d->brushType;
}

void KisBrush::predefinedBrushToXML(const QString &type, QDomElement& e) const
{
    e.setAttribute("type", type);
    e.setAttribute("filename", filename());
    e.setAttribute("md5sum", md5Sum());
    e.setAttribute("spacing", QString::number(spacing()));
    e.setAttribute("useAutoSpacing", QString::number(autoSpacingActive()));
    e.setAttribute("autoSpacingCoeff", QString::number(autoSpacingCoeff()));
    e.setAttribute("angle", QString::number(angle()));
    e.setAttribute("scale", QString::number(scale()));
    e.setAttribute("brushApplication", QString::number((int)brushApplication()));
}

void KisBrush::toXML(QDomDocument& /*document*/ , QDomElement& element) const
{
    element.setAttribute("BrushVersion", "2");
}

KisBrushSP KisBrush::fromXML(const QDomElement& element, KisResourcesInterfaceSP resourcesInterface)
{
    KoResourceLoadResult result = fromXMLLoadResult(element, resourcesInterface);

    KisBrushSP brush = result.resource<KisBrush>();
    if (!brush) {
        QDomElement el;
        brush = KisBrushRegistry::instance()->get("auto_brush")->createBrush(el, resourcesInterface).resource<KisBrush>();
    }
    return brush;
}

KoResourceLoadResult KisBrush::fromXMLLoadResult(const QDomElement& element, KisResourcesInterfaceSP resourcesInterface)
{
    KoResourceLoadResult result = KisBrushRegistry::instance()->createBrush(element, resourcesInterface);

    KisBrushSP brush = result.resource<KisBrush>();
    if (brush && element.attribute("BrushVersion", "1") == "1") {
        brush->setScale(brush->scale() * 2.0);
    }

    return result;
}

QSizeF KisBrush::characteristicSize(KisDabShape const& shape) const
{
    KisDabShape normalizedShape(
                shape.scale() * d->scale,
                shape.ratio(),
                normalizeAngle(shape.rotation() + d->angle));
    return KisQImagePyramid::characteristicSize(
                QSize(width(), height()), normalizedShape);
}

qint32 KisBrush::maskWidth(KisDabShape const& shape, qreal subPixelX, qreal subPixelY, const KisPaintInformation& info) const
{
    Q_UNUSED(info);

    qreal angle = normalizeAngle(shape.rotation() + d->angle);
    qreal scale = shape.scale() * d->scale;

    return KisQImagePyramid::imageSize(QSize(width(), height()),
                                       KisDabShape(scale, shape.ratio(), angle),
                                       subPixelX, subPixelY).width();
}

qint32 KisBrush::maskHeight(KisDabShape const& shape, qreal subPixelX, qreal subPixelY, const KisPaintInformation& info) const
{
    Q_UNUSED(info);

    qreal angle = normalizeAngle(shape.rotation() + d->angle);
    qreal scale = shape.scale() * d->scale;

    return KisQImagePyramid::imageSize(QSize(width(), height()),
                                       KisDabShape(scale, shape.ratio(), angle),
                                       subPixelX, subPixelY).height();
}

double KisBrush::maskAngle(double angle) const
{
    return normalizeAngle(angle + d->angle);
}

quint32 KisBrush::brushIndex() const
{
    return 0;
}

void KisBrush::setSpacing(double s)
{
    if (s < 0.02) s = 0.02;
    d->spacing = s;
}

double KisBrush::spacing() const
{
    return d->spacing;
}

void KisBrush::setAutoSpacing(bool active, qreal coeff)
{
    d->autoSpacingCoeff = coeff;
    d->autoSpacingActive = active;
}

bool KisBrush::autoSpacingActive() const
{
    return d->autoSpacingActive;
}

qreal KisBrush::autoSpacingCoeff() const
{
    return d->autoSpacingCoeff;
}

void KisBrush::notifyStrokeStarted()
{
}

void KisBrush::notifyBrushIsGoingToBeClonedForStroke()
{
    /// Default implementation for all image-based brushes:
    /// just recreate the shared pyramid
    d->brushPyramid.initialize(this);
}

void KisBrush::prepareForSeqNo(const KisPaintInformation &info, int seqNo)
{
    Q_UNUSED(info);
    Q_UNUSED(seqNo);
}

void KisBrush::clearBrushPyramid()
{
    d->brushPyramid.reset();
}

void KisBrush::mask(KisFixedPaintDeviceSP dst, const KoColor& color, KisDabShape const& shape, const KisPaintInformation& info, double subPixelX, double subPixelY, qreal softnessFactor, qreal lightnessStrength) const
{
    PlainColoringInformation pci(color.data());
    generateMaskAndApplyMaskOrCreateDab(dst, &pci, shape, info, subPixelX, subPixelY, softnessFactor, lightnessStrength);
}

void KisBrush::mask(KisFixedPaintDeviceSP dst, const KisPaintDeviceSP src, KisDabShape const& shape, const KisPaintInformation& info, double subPixelX, double subPixelY, qreal softnessFactor, qreal lightnessStrength) const
{
    PaintDeviceColoringInformation pdci(src, maskWidth(shape, subPixelX, subPixelY, info));
    generateMaskAndApplyMaskOrCreateDab(dst, &pdci, shape, info, subPixelX, subPixelY, softnessFactor, lightnessStrength);
}

void KisBrush::generateMaskAndApplyMaskOrCreateDab(KisFixedPaintDeviceSP dst,
                                                   ColoringInformation* coloringInformation,
                                                   KisDabShape const& shape,
                                                   const KisPaintInformation& info_,
                                                   double subPixelX, double subPixelY, qreal softnessFactor) const
{
    generateMaskAndApplyMaskOrCreateDab(dst, coloringInformation, shape, info_, subPixelX, subPixelY, softnessFactor, DEFAULT_LIGHTNESS_STRENGTH);
}

namespace
{
/// Channel @p c of an RGBA F32 or (8-byte pixels) RGBA F16 pixel as a float
float channelValue(const quint8 *pixel, int c, int pixelSize)
{
    return pixelSize == 8 ? float(reinterpret_cast<const half *>(pixel)[c]) : reinterpret_cast<const float *>(pixel)[c];
}

/**
 * GPU engine (Solstice): the image stamp table of @p colorSpace (see
 * KisProceduralCircleDab::ImageStamp): paintDevice() converts the 8-bit
 * tip with KisFixedPaintDevice::convertFromQImage(); the table holds that
 * conversion per channel, or is null when the conversion is not per
 * channel (checked on a grid of colors and alphas and random pixels).
 * Cached per color space.
 */
QSharedPointer<const QVector<float>> imageStampTable(const KoColorSpace *colorSpace)
{
    static QMutex mutex;
    static QHash<QString, QSharedPointer<const QVector<float>>> cache;
    const QString key =
        colorSpace->id() + QLatin1Char('|') + (colorSpace->profile() ? colorSpace->profile()->name() : QString());
    QMutexLocker locker(&mutex);
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) {
        return it.value();
    }

    const KoColorSpace *rgb8 =
        KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Integer8BitsColorDepthID.id(), QString());
    const int pixelSize = colorSpace->pixelSize();
    auto convert = [&](const QVector<quint32> &argb) {
        QVector<quint8> pixels(argb.size() * pixelSize);
        rgb8->convertPixelsTo(reinterpret_cast<const quint8 *>(argb.constData()),
                              pixels.data(),
                              colorSpace,
                              argb.size(),
                              KoColorConversionTransformation::internalRenderingIntent(),
                              KoColorConversionTransformation::internalConversionFlags());
        QVector<float> result(argb.size() * 4);
        for (int i = 0; i < argb.size(); ++i) {
            for (int c = 0; c < 4; ++c) {
                result[4 * i + c] = channelValue(pixels.constData() + i * pixelSize, c, pixelSize);
            }
        }
        return result;
    };

    QVector<quint32> ramp(256);
    for (int v = 0; v < 256; ++v) {
        ramp[v] = qRgba(v, v, v, v);
    }
    const QVector<float> converted = convert(ramp);
    QSharedPointer<QVector<float>> table(new QVector<float>(1024));
    for (int v = 0; v < 256; ++v) {
        for (int c = 0; c < 4; ++c) {
            (*table)[c * 256 + v] = converted[4 * v + c];
        }
    }

    QVector<quint32> samples;
    for (int r = 0; r < 256; r += 17) {
        for (int g = 0; g < 256; g += 17) {
            for (int b = 0; b < 256; b += 17) {
                for (int a : {0, 1, 128, 255}) {
                    samples << qRgba(r, g, b, a);
                }
            }
        }
    }
    QRandomGenerator random(1);
    for (int i = 0; i < 65536; ++i) {
        samples << random.generate();
    }
    const QVector<float> check = convert(samples);
    bool perChannel = true;
    for (int i = 0; perChannel && i < samples.size(); ++i) {
        const quint32 p = samples[i];
        const float expected[4] = {(*table)[qRed(p)],
                                   (*table)[256 + qGreen(p)],
                                   (*table)[512 + qBlue(p)],
                                   (*table)[768 + qAlpha(p)]};
        perChannel = std::memcmp(expected, check.constData() + 4 * i, sizeof(expected)) == 0;
    }
    QSharedPointer<const QVector<float>> result = perChannel ? table : QSharedPointer<const QVector<float>>();
    cache.insert(key, result);
    return result;
}
} // namespace

bool KisBrush::proceduralImageDab(KisDabShape const &shape,
                                  double subPixelX,
                                  double subPixelY,
                                  const quint8 *color,
                                  const KoColorSpace *colorSpace,
                                  qreal lightnessStrength,
                                  KisProceduralCircleDab *dab) const
{
    // Brushes that generate their dabs differently; RGBA F32 or F16 dabs
    const int pixelSize = colorSpace ? colorSpace->pixelSize() : 0;
    if (!valid() || !color || !dab || !colorSpace || dynamic_cast<const KisAutoBrush *>(this)
        || dynamic_cast<const KisImagePipeBrush *>(this) || dynamic_cast<const KisTextBrush *>(this)
        || (pixelSize != 16 && pixelSize != 8)) {
        return false;
    }
    // The per-pixel work of the application as tables (see
    // KisProceduralCircleDab::ImageApplication), made with the CPU code
    quint32 application = KisProceduralCircleDab::AlphaMask;
    QSharedPointer<const QVector<float>> table;
    if (brushApplication() == IMAGESTAMP) {
        application = KisProceduralCircleDab::ImageStamp;
        table = imageStampTable(colorSpace);
    } else if (preserveLightness()) {
        application = KisProceduralCircleDab::LightnessMap;
        // the color depends on the red value only, the alpha on the alpha
        QVector<QRgb> brush(256);
        for (int v = 0; v < 256; ++v) {
            brush[v] = qRgba(v, 0, 0, v);
        }
        QVector<quint8> pixels(256 * pixelSize);
        QVector<quint8> paintColor(pixelSize);
        std::memcpy(paintColor.data(), color, size_t(paintColor.size()));
        colorSpace->fillGrayBrushWithColorAndLightnessWithStrength(pixels.data(),
                                                                   brush.constData(),
                                                                   paintColor.data(),
                                                                   lightnessStrength,
                                                                   256);
        QSharedPointer<QVector<float>> values(new QVector<float>(1024));
        for (int v = 0; v < 256; ++v) {
            for (int c = 0; c < 4; ++c) {
                (*values)[c * 256 + v] = channelValue(pixels.constData() + v * pixelSize, c, pixelSize);
            }
        }
        table = values;
    } else if (applyingGradient()) {
        if (!d->cachedGradient) {
            return false;
        }
        application = KisProceduralCircleDab::GradientMap;
        d->cachedGradient->setColorSpace(colorSpace);
        // the cached colors of the 256 qGray() values and the mask
        // opacities alpha / 255.0 (doubles after the 1024 floats)
        QSharedPointer<QVector<float>> values(new QVector<float>(1024 + 512));
        for (int v = 0; v < 256; ++v) {
            const quint8 *cached = d->cachedGradient->cachedAt(qreal(v) / 255.0);
            for (int c = 0; c < 4; ++c) {
                (*values)[c * 256 + v] = channelValue(cached, c, pixelSize);
            }
            const double maskOpacity = qreal(v) / 255.0;
            std::memcpy(values->data() + 1024 + 2 * v, &maskOpacity, sizeof(double));
        }
        table = values;
    } else if (brushApplication() != ALPHAMASK) {
        return false;
    }
    if (application != KisProceduralCircleDab::AlphaMask && !table) {
        return false;
    }
    // The same pyramid and shape as generateMaskAndApplyMaskOrCreateDab()
    QSharedPointer<KisImageTipGeometry> geometry(new KisImageTipGeometry());
    if (!d->brushPyramid.value(this)->describeImage(
            KisDabShape(shape.scale() * d->scale, shape.ratio(), -normalizeAngle(shape.rotation() + d->angle)),
            subPixelX,
            subPixelY,
            geometry.data())
        || geometry->size.isEmpty()) {
        return false;
    }
    *dab = KisProceduralCircleDab();
    dab->kind = KisProceduralCircleDab::ImageTip;
    dab->halfPixels = pixelSize == 8;
    for (int c = 0; c < 4; ++c) {
        dab->color[c] = channelValue(color, c, pixelSize);
    }
    dab->imageTip = geometry;
    dab->imageApplication = application;
    dab->imageTable = table;
    return true;
}

void KisBrush::generateMaskAndApplyMaskOrCreateDab(KisFixedPaintDeviceSP dst,
        ColoringInformation* coloringInformation,
        KisDabShape const& shape,
        const KisPaintInformation& info_,
        double subPixelX, double subPixelY, qreal softnessFactor, qreal lightnessStrength) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(valid());
    Q_UNUSED(info_);
    Q_UNUSED(softnessFactor);

    QImage outputImage = d->brushPyramid.value(this)->createImage(KisDabShape(
                                                                         shape.scale() * d->scale, shape.ratio(),
                                                                         -normalizeAngle(shape.rotation() + d->angle)),
                                                                     subPixelX, subPixelY);

    qint32 maskWidth = outputImage.width();
    qint32 maskHeight = outputImage.height();

    dst->setRect(QRect(0, 0, maskWidth, maskHeight));
    dst->lazyGrowBufferWithoutInitialization();

    KIS_SAFE_ASSERT_RECOVER_RETURN(coloringInformation);

    quint8* color = 0;
    if (dynamic_cast<PlainColoringInformation*>(coloringInformation)) {
        color = const_cast<quint8*>(coloringInformation->color());
    }

    const KoColorSpace *cs = dst->colorSpace();
    const quint32 pixelSize = cs->pixelSize();
    const quint32 maskPixelSize = sizeof(QRgb);
    quint8 *rowPointer = dst->data();

    const bool preserveLightness = this->preserveLightness();
    bool applyGradient = this->applyingGradient();
    QScopedPointer<KoColor> fallbackColor;

    if (applyGradient) {
        if (d->cachedGradient) {
            KIS_SAFE_ASSERT_RECOVER_RETURN(d->cachedGradient);
            d->cachedGradient->setColorSpace(cs); //convert gradient to colorspace so we don't have to convert each pixel
        } else {
            fallbackColor.reset(new KoColor(Qt::red, cs));
            color = fallbackColor->data();
            applyGradient = false;
        }
    }

    KoColor gradientcolor(Qt::blue, cs);
    for (int y = 0; y < maskHeight; y++) {
        const quint8* maskPointer = outputImage.constScanLine(y);
        if (color) {
            if (preserveLightness) {
                cs->fillGrayBrushWithColorAndLightnessWithStrength(rowPointer, reinterpret_cast<const QRgb*>(maskPointer), color, lightnessStrength, maskWidth);
            }
            else if (applyGradient) {
                quint8* pixel = rowPointer;
                for (int x = 0; x < maskWidth; x++) {
                    const QRgb* maskQRgb = reinterpret_cast<const QRgb*>(maskPointer);
                    qreal maskOpacity = qreal(qAlpha(*maskQRgb)) / 255.0;
                    // Solstice (GPU engine, "Image tip dabs"): transparent
                    // pixels take their own gradient color too, instead of the
                    // previous pixel's, so that each pixel depends on its
                    // tip pixel only (their alpha stays 0)
                    {
                        qreal gradientvalue = qreal(qGray(*maskQRgb)) / 255.0;
                        gradientcolor.setColor(d->cachedGradient->cachedAt(gradientvalue), cs);
                    }
                    qreal gradientOpacity = gradientcolor.opacityF();
                    qreal opacity = gradientOpacity * maskOpacity;
                    gradientcolor.setOpacity(opacity);
                    memcpy(pixel, gradientcolor.data(), pixelSize);

                    maskPointer += maskPixelSize; 
                    pixel += pixelSize;
                }
            }
            else {
                cs->fillGrayBrushWithColor(rowPointer, reinterpret_cast<const QRgb*>(maskPointer), color, maskWidth);
            }
        }
        else {
            {
                quint8 *dst = rowPointer;
                for (int x = 0; x < maskWidth; x++) {
                    memcpy(dst, coloringInformation->color(), pixelSize);
                    coloringInformation->nextColumn();
                    dst += pixelSize;
                }
            }

            QScopedArrayPointer<quint8> alphaArray(new quint8[maskWidth]);
            fetchPremultipliedRed(reinterpret_cast<const QRgb*>(maskPointer), alphaArray.data(), maskWidth);
            cs->applyAlphaU8Mask(rowPointer, alphaArray.data(), maskWidth);
        }

        rowPointer += maskWidth * pixelSize;

        if (!color) {
            coloringInformation->nextRow();
        }
    }


}

KisFixedPaintDeviceSP KisBrush::paintDevice(const KoColorSpace * colorSpace,
                                            KisDabShape const& shape,
                                            const KisPaintInformation& info,
                                            double subPixelX, double subPixelY) const
{
    Q_ASSERT(valid());
    Q_UNUSED(info);
    double angle = normalizeAngle(shape.rotation() + d->angle);
    double scale = shape.scale() * d->scale;

    QImage outputImage = d->brushPyramid.value(this)->createImage(
                KisDabShape(scale, shape.ratio(), -angle), subPixelX, subPixelY);

    KisFixedPaintDeviceSP dab = new KisFixedPaintDevice(colorSpace);
    Q_CHECK_PTR(dab);
    dab->convertFromQImage(outputImage, "");

    return dab;
}

void KisBrush::resetOutlineCache()
{
    d->brushOutline.reset();
}

void KisBrush::generateOutlineCache()
{
    d->brushOutline.initialize(this);
}

bool KisBrush::outlineCacheIsValid() const
{
    return !d->brushOutline.isNull();
}

void KisBrush::setScale(qreal _scale)
{
    d->scale = _scale;
}

qreal KisBrush::scale() const
{
    return d->scale;
}

void KisBrush::setAngle(qreal _rotation)
{
    d->angle = _rotation;
}

qreal KisBrush::angle() const
{
    return d->angle;
}

KisOptimizedBrushOutline KisBrush::outline(bool forcePreciseOutline) const
{
    Q_UNUSED(forcePreciseOutline);

    return *d->brushOutline.value(this);
}

void KisBrush::lodLimitations(KisPaintopLodLimitations *l) const
{
    if (spacing() > 0.5) {
        l->limitations << KoID("huge-spacing", i18nc("PaintOp instant preview limitation", "Spacing > 0.5, consider disabling Instant Preview"));
    }
}

bool KisBrush::supportsCaching() const
{
    return true;
}

void KisBrush::coldInitBrush()
{
    d->brushPyramid.initialize(this);
    generateOutlineCache();
}
