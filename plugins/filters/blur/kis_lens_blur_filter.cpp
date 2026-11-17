/*
 * This file is part of Krita
 *
 * SPDX-FileCopyrightText: 2010 Edward Apap <schumifer@hotmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_lens_blur_filter.h"
#include "KisLensBlurHighlights.h"
#include "kis_wdg_lens_blur.h"

#include <KoColorModelStandardIds.h>
#include <KoCompositeOp.h>
#include <QLabel>
#include <QLayout>

#include <kis_convolution_kernel.h>
#include <kis_convolution_painter.h>

#include "ui_wdg_lens_blur.h"

#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_selection.h>
#include <kis_paint_device.h>
#include <kis_processing_information.h>
#include "kis_lod_transform.h"


#include <QPainter>

#include <math.h>


KisLensBlurFilter::KisLensBlurFilter() : KisFilter(id(), FiltersCategoryBlurId, i18n("&Lens Blur..."))
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);

}

KisConfigWidget *
KisLensBlurFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool) const
{
    auto *widget = new KisWdgLensBlur(parent);
    if (device && device->colorSpace()->colorModelId() != RGBAColorModelID
        && device->colorSpace()->colorModelId() != GrayAColorModelID) {
        auto *notice = new QLabel(
            i18n("Highlight enhancement converts colors through floating-point RGB and back; appearance may change."),
            widget);
        notice->setWordWrap(true);
        widget->layout()->addWidget(notice);
    }
    return widget;
}

QSize KisLensBlurFilter::getKernelHalfSize(const KisFilterConfigurationSP config, int lod)
{
    QPolygonF iris = getIrisPolygon(config, lod);
    QRect rect = iris.boundingRect().toAlignedRect();

    int w = std::ceil(qreal(rect.width()) / 2.0);
    int h = std::ceil(qreal(rect.height()) / 2.0);

    return QSize(w, h);
}

KisFilterConfigurationSP KisLensBlurFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    config->setProperty("irisShape", "Pentagon (5)");
    config->setProperty("irisRadius", 5);
    config->setProperty("irisRotation", 0);
    config->setProperty("irisRoundness", 0.0);
    config->setProperty("highlightStrength", 0.0);

    QSize halfSize = getKernelHalfSize(config, 0);
    config->setProperty("halfWidth", halfSize.width());
    config->setProperty("halfHeight", halfSize.height());

    return config;
}

QPolygonF KisLensBlurFilter::getIrisPolygon(const KisFilterConfigurationSP config, int lod)
{
    KIS_ASSERT_RECOVER(config) { return QPolygonF(); }

    KisLodTransformScalar t(lod);

    QVariant value;
    config->getProperty("irisShape", value);
    QString irisShape = value.toString();
    config->getProperty("irisRadius", value);
    uint irisRadius = t.scale(value.toUInt());
    config->getProperty("irisRotation", value);
    uint irisRotation = value.toUInt();

    if (irisRadius < 1)
        return QPolygon();

    QPolygonF irisShapePoly;

    int sides = 1;
    qreal angle = 0;

    if (irisShape == "Triangle") sides = 3;
    else if (irisShape == "Quadrilateral (4)") sides = 4;
    else if (irisShape == "Pentagon (5)") sides = 5;
    else if (irisShape == "Hexagon (6)") sides = 6;
    else if (irisShape == "Heptagon (7)") sides = 7;
    else if (irisShape == "Octagon (8)") sides = 8;
    else return QPolygonF();

    const double roundness = KisLensBlurHighlights::boundedPercent(config, "irisRoundness") / 100.0;
    if (roundness > 0.0) {
        // Radially interpolate the polygon boundary toward its circumcircle.
        // At 100%, shape and rotation must not affect the circular aperture.
        const int segments = roundness == 1.0 ? 256 : sides * 64;
        for (int i = 0; i < segments; ++i) {
            const double theta = 2.0 * M_PI * i / segments;
            const double sectorAngle = 2.0 * M_PI * (i % 64) / (sides * 64);
            const double polygonRadius = 0.5 * cos(M_PI / sides) / cos(sectorAngle - M_PI / sides);
            const double radius = roundness == 1.0 ? 0.5 : (1.0 - roundness) * polygonRadius + roundness * 0.5;
            irisShapePoly << QPointF(radius * cos(theta), radius * sin(theta));
        }
        if (roundness == 1.0)
            irisRotation = 0;
    } else
        for (int i = 0; i < sides; ++i) {
            irisShapePoly << QPointF(0.5 * cos(angle), 0.5 * sin(angle));
            angle += 2 * M_PI / sides;
        }

    QTransform transform;
    transform.rotate(irisRotation);
    transform.scale(irisRadius * 2, irisRadius * 2);

    QPolygonF transformedIris = transform.map(irisShapePoly);

    return transformedIris;
}

void KisLensBlurFilter::processImpl(KisPaintDeviceSP device,
                                    const QRect& rect,
                                    const KisFilterConfigurationSP config,
                                    KoUpdater* progressUpdater
                                    ) const
{
    QPoint srcTopLeft = rect.topLeft();

    Q_ASSERT(device != 0);
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);

    QBitArray channelFlags = config->channelFlags();
    if (channelFlags.isEmpty()) {
        channelFlags = QBitArray(device->colorSpace()->channelCount(), true);
    }

    const int lod = device->defaultBounds()->currentLevelOfDetail();
    QPolygonF transformedIris = getIrisPolygon(config, lod);
    if (transformedIris.isEmpty()) return;

    QRectF boundingRect = transformedIris.boundingRect();

    int kernelWidth = boundingRect.toAlignedRect().width();
    int kernelHeight = boundingRect.toAlignedRect().height();

    QImage kernelRepresentation(kernelWidth, kernelHeight, QImage::Format_RGB32);
    kernelRepresentation.fill(0);

    QPainter imagePainter(&kernelRepresentation);
    imagePainter.setRenderHint(QPainter::Antialiasing);
    imagePainter.setBrush(QColor::fromRgb(255, 255, 255));

    QTransform offsetTransform;
    offsetTransform.translate(-boundingRect.x(), -boundingRect.y());
    imagePainter.setTransform(offsetTransform);
    imagePainter.drawPolygon(transformedIris, Qt::WindingFill);

    // construct kernel from image
    const bool extended = KisLensBlurHighlights::boundedPercent(config, "irisRoundness") > 0.0
        || KisLensBlurHighlights::boundedPercent(config, "highlightStrength") > 0.0;
    // FFT padding assumes odd support. With even kernels the first output
    // pixel can wrap to the far end of the FFT cache, producing patch seams.
    // Leading zeros preserve the aperture's offsets relative to its anchor.
    // Keep the historical even kernel untouched for old configurations.
    const int padX = extended && kernelWidth % 2 == 0 ? 1 : 0;
    const int padY = extended && kernelHeight % 2 == 0 ? 1 : 0;
    Eigen::Matrix<qreal, Eigen::Dynamic, Eigen::Dynamic> irisKernel(kernelHeight + padY, kernelWidth + padX);
    irisKernel.setZero();
    for (int j = 0; j < kernelHeight; ++j) {
        for (int i = 0; i < kernelWidth; ++i) {
            irisKernel(j + padY, i + padX) = qRed(kernelRepresentation.pixel(i, j));
        }
    }

    KisConvolutionKernelSP kernel = KisConvolutionKernel::fromMatrix(irisKernel, 0, irisKernel.sum());
    if (KisLensBlurHighlights::boundedPercent(config, "highlightStrength") > 0.0) {
        KisLensBlurHighlights::apply(device, rect, neededRect(rect, config, lod), config, kernel, progressUpdater);
        return;
    }

    // apply convolution (unchanged legacy path when both new controls are zero)
    KisConvolutionPainter painter(device);
    painter.setChannelFlags(channelFlags);
    painter.setProgress(progressUpdater);

    painter.applyMatrix(kernel, device, srcTopLeft, srcTopLeft, rect.size(), BORDER_REPEAT);
}

QRect KisLensBlurFilter::neededRect(const QRect & rect, const KisFilterConfigurationSP _config, int lod) const
{
    if (KisLensBlurHighlights::boundedPercent(_config, "irisRoundness") > 0.0
        || KisLensBlurHighlights::boundedPercent(_config, "highlightStrength") > 0.0) {
        const QSize half = getKernelHalfSize(_config, lod);
        return rect.adjusted(-2 * half.width(), -2 * half.height(), 2 * half.width(), 2 * half.height());
    }
    KisLodTransformScalar t(lod);

    QVariant value;
    const int halfWidth = t.scale(_config->getProperty("halfWidth", value) ? value.toUInt() : 5);
    const int halfHeight = t.scale(_config->getProperty("halfHeight", value) ? value.toUInt() : 5);

    return rect.adjusted(-halfWidth * 2, -halfHeight * 2, halfWidth * 2, halfHeight * 2);
}

QRect KisLensBlurFilter::changedRect(const QRect & rect, const KisFilterConfigurationSP _config, int lod) const
{
    if (KisLensBlurHighlights::boundedPercent(_config, "irisRoundness") > 0.0
        || KisLensBlurHighlights::boundedPercent(_config, "highlightStrength") > 0.0) {
        const QSize half = getKernelHalfSize(_config, lod);
        return rect.adjusted(-half.width(), -half.height(), half.width(), half.height());
    }
    KisLodTransformScalar t(lod);

    QVariant value;
    const int halfWidth = t.scale(_config->getProperty("halfWidth", value) ? value.toUInt() : 5);
    const int halfHeight = t.scale(_config->getProperty("halfHeight", value) ? value.toUInt() : 5);

    return rect.adjusted(-halfWidth, -halfHeight, halfWidth, halfHeight);
}
