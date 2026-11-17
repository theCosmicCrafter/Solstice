/*
 *  SPDX-FileCopyrightText: 2007 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2008 Cyrille Berger <cberger@cberger.net>
 *  SPDX-FileCopyrightText: 2009 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "kis_prescaled_projection.h"

#include <math.h>

#include <QImage>
#include <QColor>
#include <QRect>
#include <QPoint>
#include <QSize>
#include <QPainter>

#include <KoColorProfile.h>
#include <KoViewConverter.h>

#include "kis_config.h"
#include "kis_image_config.h"
#include "kis_config_notifier.h"
#include "kis_image.h"
#include "krita_utils.h"

#include "kis_coordinates_converter.h"
#include "kis_display_filter.h"
#include "kis_image_pyramid.h"
#include "kis_projection_backend.h"
#include "opengl/kis_opengl.h"
#include <KisDisplayConfig.h>

#include <KisCanvasState.h>

namespace {
/**
 * RelevantCanvasState represents a part of the canvas state
 * which is relevant for the prescaled projection. Basically,
 * if this structure changes, then the prescaled projection
 * needs an update.
 */
struct RelevantCanvasState {
    qreal zoom = 1.0;
    qreal rotation = 0.0;
    QPointF viewportOffsetF;
    qreal devicePixelRatio = 1.0;

    std::tuple<qreal, qreal, qreal> transformations() const
    {
        return {zoom, rotation, devicePixelRatio};
    }

    bool operator==(const RelevantCanvasState &other) const
    {
        return qFuzzyCompare(zoom, other.zoom) && qFuzzyCompare(rotation, other.rotation)
            && qFuzzyCompare(devicePixelRatio, other.devicePixelRatio) && viewportOffsetF == other.viewportOffsetF;
    }

    bool operator!=(const RelevantCanvasState &other) const
    {
        return !(*this == other);
    }

    static RelevantCanvasState fromCanvasState(const KisCanvasState &state)
    {
        return {state.effectiveZoom, state.rotation, state.viewportOffsetF, state.devicePixelRatio};
    }
};
} // namespace

#define ceiledSize(sz) QSize(ceil((sz).width()), ceil((sz).height()))

void copyQImage(qint32 deltaX, qint32 deltaY, QImage* dstImage, const QImage& srcImage)
{
    const qint32 height = dstImage->height();
    const qint32 width = dstImage->width();
    /**
     * The offset reaches here in *device* pixels while the caller's
     * overlap check uses viewport units, so |delta| may exceed the image
     * bounds on HiDPI canvases. The old code fed 4*(width - deltaX)
     * straight to memcpy, which goes negative and scribbles the heap
     * (release builds drop the size assert). Clip the copy instead;
     * in-range behavior is identical.
     */
    if (dstImage->size() != srcImage.size() || dstImage->depth() != srcImage.depth())
        return;
    const int pixelSize = dstImage->depth() / 8;
    const int copyW = width - qAbs(deltaX);
    const int copyH = height - qAbs(deltaY);
    if (copyW <= 0 || copyH <= 0)
        return;
    const int srcX = qMax(0, -deltaX);
    const int dstX = qMax(0, deltaX);
    const int srcY = qMax(0, -deltaY);
    const int dstY = qMax(0, deltaY);
    for (int y = 0; y < copyH; ++y) {
        memcpy(dstImage->scanLine(dstY + y) + pixelSize * dstX,
               srcImage.scanLine(srcY + y) + pixelSize * srcX,
               size_t(pixelSize) * size_t(copyW));
    }
}

struct KisPrescaledProjection::Private {
    Private()
        : viewportSize(0, 0)
        , projectionBackend(0)
        , smoothHighZoom(KisConfig(true).smoothHighZoom())
        , filterMode((KisOpenGL::FilterMode)KisConfig(true).openGLFilteringMode())
    {
    }

    QImage prescaledQImage;

    std::optional<RelevantCanvasState> currentRelevantCanvasState;
    QSize updatePatchSize;
    QSize canvasSize;
    QSize viewportSize;
    KisImageWSP image;
    KisCoordinatesConverter *coordinatesConverter{0};
    KisProjectionBackend *projectionBackend{0};
    bool smoothHighZoom{true};
    KisOpenGL::FilterMode filterMode{KisOpenGL::BilinearFilterMode};
};

KisPrescaledProjection::KisPrescaledProjection()
        : QObject(0)
        , m_d(new Private())
{
    updateSettings();

    // we disable building the pyramid with setting its height to 1
    // XXX: setting it higher than 1 is broken because it's not updated until you show/hide the layer
    m_d->projectionBackend = new KisImagePyramid(1);

    connect(KisConfigNotifier::instance(), SIGNAL(configChanged()), SLOT(updateSettings()));
}

KisPrescaledProjection::~KisPrescaledProjection()
{
    delete m_d->projectionBackend;
    delete m_d;
}


void KisPrescaledProjection::setImage(KisImageWSP image)
{
    Q_ASSERT(image);
    m_d->image = image;
    m_d->projectionBackend->setImage(image);
}

QImage KisPrescaledProjection::prescaledQImage() const
{
    return m_d->prescaledQImage;
}

bool KisPrescaledProjection::smoothHighZoom() const
{
    return m_d->smoothHighZoom;
}

void KisPrescaledProjection::setSmoothHighZoom(bool value)
{
    if (m_d->smoothHighZoom != value) {
        m_d->smoothHighZoom = value;
        preScale();
        Q_EMIT sigSettingsChanged();
    }
}

void KisPrescaledProjection::setCoordinatesConverter(KisCoordinatesConverter *coordinatesConverter)
{
    m_d->coordinatesConverter = coordinatesConverter;
    m_d->currentRelevantCanvasState = RelevantCanvasState::fromCanvasState(KisCanvasState::fromConverter(*coordinatesConverter));
}

void KisPrescaledProjection::updateSettings()
{
    KisImageConfig imageConfig(false);
    m_d->updatePatchSize.setWidth(imageConfig.updatePatchWidth());
    m_d->updatePatchSize.setHeight(imageConfig.updatePatchHeight());

    KisConfig cfg(true);
    setSmoothHighZoom(cfg.smoothHighZoom());

    const KisOpenGL::FilterMode filterMode = (KisOpenGL::FilterMode)cfg.openGLFilteringMode();
    if (m_d->filterMode != filterMode) {
        m_d->filterMode = filterMode;
        preScale();
        Q_EMIT sigSettingsChanged();
    }
}

void KisPrescaledProjection::notifyCanvasStateChanged(const KisCanvasState &state)
{
    auto relevantState = RelevantCanvasState::fromCanvasState(state);

    if (m_d->currentRelevantCanvasState == relevantState) return;

    if (!m_d->currentRelevantCanvasState ||
        m_d->currentRelevantCanvasState->transformations() != relevantState.transformations()) {

        updateViewportSize();
        preScale();
    } else {
        const QPointF moveOffset = m_d->currentRelevantCanvasState->viewportOffsetF - relevantState.viewportOffsetF;
        viewportMoved(-moveOffset);
    }

    m_d->currentRelevantCanvasState = relevantState;
}

void KisPrescaledProjection::viewportMoved(const QPointF &offset)
{
    // FIXME: \|/
    if (m_d->prescaledQImage.isNull()) return;
    if (offset.isNull()) return;

    QPoint alignedOffset = offset.toPoint();

    if(offset != alignedOffset) {
        /**
         * We can't optimize anything when offset is float :(
         * Just prescale entire image.
         */
        dbgRender << "prescaling the entire image because the offset is float";
        preScale();
        return;
    }

    const qreal devicePixelRatio = m_d->coordinatesConverter->devicePixelRatio();

    /**
     * copyQImage() works in the raw pixels of the device-pixel-sized
     * buffers, so the offset has to be mapped to the device pixel grid as
     * well. If it doesn't land on an exact device pixel, just prescale.
     */
    const QPointF devicePixelOffset = QPointF(alignedOffset) * devicePixelRatio;
    const QPoint alignedDevicePixelOffset = devicePixelOffset.toPoint();

    if (devicePixelOffset != alignedDevicePixelOffset) {
        dbgRender << "prescaling the entire image because the offset is not aligned to device pixels";
        preScale();
        return;
    }

    QImage newImage = QImage(m_d->prescaledQImage.size(), QImage::Format_ARGB32);
    newImage.setDevicePixelRatio(devicePixelRatio);
    newImage.fill(0);

    /**
     * TODO: viewport rects should be cropped by the borders of
     * the image, because it may be requested to read/write
     * outside QImage and copyQImage will not catch it
     */
    QRect newViewportRect = QRect(QPoint(0,0), m_d->viewportSize);
    QRect oldViewportRect = newViewportRect.translated(alignedOffset);

    QRegion updateRegion = newViewportRect;
    QRect savedArea = newViewportRect & oldViewportRect;
    if(!savedArea.isEmpty()) {
        copyQImage(alignedDevicePixelOffset.x(), alignedDevicePixelOffset.y(), &newImage, m_d->prescaledQImage);
        updateRegion -= savedArea;
    }

    QPainter gc(&newImage);
    auto rc = updateRegion.begin();
    while (rc != updateRegion.end()) {
        QRect rect = *rc;
        QRect imageRect =
            m_d->coordinatesConverter->viewportToImage(rect).toAlignedRect();
        QVector<QRect> patches =
            KritaUtils::splitRectIntoPatches(imageRect, m_d->updatePatchSize);

        Q_FOREACH (const QRect& rc, patches) {
            QRect viewportPatch =
                m_d->coordinatesConverter->imageToViewport(rc).toAlignedRect();

            KisPPUpdateInfoSP info = getInitialUpdateInformation(QRect());
            fillInUpdateInformation(viewportPatch, info);
            drawUsingBackend(gc, info);
        }
        rc++;
    }

    m_d->prescaledQImage = newImage;
}

void KisPrescaledProjection::slotImageSizeChanged(qint32 w, qint32 h)
{
    m_d->projectionBackend->setImageSize(w, h);
    // viewport size is cropped by the size of the image
    // so we need to update it as well
    updateViewportSize();
}

KisUpdateInfoSP KisPrescaledProjection::updateCache(const QRect &dirtyImageRect)
{
    if (!m_d->image) {
        dbgRender.noquote() << "Calling updateCache without an image:" << kisBacktrace() << Qt::endl;
        // return invalid info
        return new KisPPUpdateInfo();
    }

    /**
     * We needn't this stuff outside KisImage's area. We're not displaying
     * anything painted outside the image anyway.
     */
    QRect croppedImageRect = dirtyImageRect & m_d->image->bounds();
    if (croppedImageRect.isEmpty()) return new KisPPUpdateInfo();

    KisPPUpdateInfoSP info = getInitialUpdateInformation(croppedImageRect);
    m_d->projectionBackend->updateCache(croppedImageRect);

    return info;
}

void KisPrescaledProjection::recalculateCache(KisUpdateInfoSP info)
{
    KisPPUpdateInfoSP ppInfo = dynamic_cast<KisPPUpdateInfo*>(info.data());
    if(!ppInfo) return;

    QRect rawViewRect =
        m_d->coordinatesConverter->
        imageToViewport(ppInfo->dirtyImageRectVar).toAlignedRect();

    fillInUpdateInformation(rawViewRect, ppInfo);

    m_d->projectionBackend->recalculateCache(ppInfo);

    if(!info->dirtyViewportRect().isEmpty())
        updateScaledImage(ppInfo);
}

void KisPrescaledProjection::preScale()
{
    if (!m_d->image) return;

    m_d->prescaledQImage.fill(0);

    QRect viewportRect(QPoint(0, 0), m_d->viewportSize);
    QRect imageRect =
        m_d->coordinatesConverter->viewportToImage(viewportRect).toAlignedRect();

    QVector<QRect> patches =
        KritaUtils::splitRectIntoPatches(imageRect, m_d->updatePatchSize);

    Q_FOREACH (const QRect& rc, patches) {
        QRect viewportPatch = m_d->coordinatesConverter->imageToViewport(rc).toAlignedRect();
        KisPPUpdateInfoSP info = getInitialUpdateInformation(QRect());
        fillInUpdateInformation(viewportPatch, info);
        QPainter gc(&m_d->prescaledQImage);
        gc.setCompositionMode(QPainter::CompositionMode_Source);
        drawUsingBackend(gc, info);
    }

}

void KisPrescaledProjection::setDisplayConfig(const KisDisplayConfig &config)
{
    m_d->projectionBackend->setMonitorProfile(config.profile, config.intent, config.conversionFlags);
}

void KisPrescaledProjection::setChannelFlags(const QBitArray &channelFlags)
{
    m_d->projectionBackend->setChannelFlags(channelFlags);
}

void KisPrescaledProjection::setDisplayFilter(QSharedPointer<KisDisplayFilter> displayFilter)
{
    m_d->projectionBackend->setDisplayFilter(displayFilter);
}


void KisPrescaledProjection::updateViewportSize()
{
    QRect imageRect = m_d->coordinatesConverter->imageRectInWidgetPixels().toAlignedRect();
    QSizeF minimalSize(qMin(imageRect.width(), m_d->canvasSize.width()),
                       qMin(imageRect.height(), m_d->canvasSize.height()));
    QRectF minimalRect(QPointF(0,0), minimalSize);

    const QSize viewportSize = m_d->coordinatesConverter->widgetToViewport(minimalRect).toAlignedRect().size();

    /**
     * The prescaled image is rendered in device pixels so that its content
     * is not upscaled once again when blitting onto a High-DPI canvas
     * widget. All the rects calculated via KisCoordinatesConverter stay in
     * logical (widget-like) units; QImage::devicePixelRatio() on the buffer
     * makes every QPainter opened on it work in logical units as well.
     */
    const qreal devicePixelRatio = m_d->coordinatesConverter->devicePixelRatio();
    const QSize devicePixelSize = ceiledSize(QSizeF(viewportSize) * devicePixelRatio);

    /**
     * During view construction/window-state churn the converter can report
     * a degenerate rect (0-size or effectively unbounded transform) and
     * canvasSize may still be invalid. Allocating a QImage from that threw
     * std::bad_alloc inside the maximize cascade. The bound allows any real
     * configuration (a 32k canvas at 8x HiDPI is 262144 device px) while
     * still rejecting garbage. Defer until a sane notify arrives; the old
     * size and buffer remain valid meanwhile.
     */
    if (devicePixelSize.isEmpty() || devicePixelSize.width() > 262144
        || devicePixelSize.height() > 262144 || !devicePixelSize.isValid()) {
        return;
    }

    m_d->viewportSize = viewportSize;

    if (m_d->prescaledQImage.isNull() || m_d->prescaledQImage.size() != devicePixelSize
        || !qFuzzyCompare(m_d->prescaledQImage.devicePixelRatio(), devicePixelRatio)) {
        m_d->prescaledQImage = QImage(devicePixelSize, QImage::Format_ARGB32);
        m_d->prescaledQImage.setDevicePixelRatio(devicePixelRatio);
        m_d->prescaledQImage.fill(0);
    }
}

void KisPrescaledProjection::notifyCanvasSizeChanged(const QSize &widgetSize)
{
    m_d->canvasSize = widgetSize;
    updateViewportSize();
    preScale();
}

KisPPUpdateInfoSP KisPrescaledProjection::getInitialUpdateInformation(const QRect &dirtyImageRect)
{
    /**
     * This update information has nothing more than an information
     * about dirty image rect. All the other information used for
     * scaling will be fetched in fillUpdateInformation() later,
     * when we are working in the context of the UI thread
     */

    KisPPUpdateInfoSP info = new KisPPUpdateInfo();
    info->dirtyImageRectVar = dirtyImageRect;

    return info;
}

void KisPrescaledProjection::fillInUpdateInformation(const QRect &viewportRect,
                                                     KisPPUpdateInfoSP info)
{
    m_d->coordinatesConverter->imageScale(&info->scaleX, &info->scaleY);

    /**
     * The prescaled buffer is rendered in device pixels, so all the
     * scale-dependent decisions below (direct/patch transfer, smooth
     * scaling, border size) are driven by the physical scale, like in
     * KisOpenGLCanvasRenderer.
     */
    qreal physicalScaleX, physicalScaleY;
    m_d->coordinatesConverter->imagePhysicalScale(&physicalScaleX, &physicalScaleY);

    // first, crop the part of the view rect that is outside of the canvas
    QRect croppedViewRect = viewportRect.intersected(QRect(QPoint(0, 0), m_d->viewportSize));

    // second, align this rect to the KisImage's pixels and pixels
    // of projection backend.
    info->imageRect = m_d->coordinatesConverter->viewportToImage(QRectF(croppedViewRect)).toAlignedRect();

    /**
     * To avoid artifacts while scaling we use mechanism like
     * changeRect/needRect for layers. Here we grow the rect to update
     * pixels which depend on the dirty rect (like changeRect), and
     * later we request a bit more pixels for the patch to make the
     * scaling safe (like needRect).
     */
    const int borderSize = BORDER_SIZE(qMax(physicalScaleX, physicalScaleY));
    info->imageRect.adjust(-borderSize, -borderSize, borderSize, borderSize);

    info->imageRect = info->imageRect & m_d->image->bounds();

    m_d->projectionBackend->alignSourceRect(info->imageRect, info->scaleX);

    // finally, compute the dirty rect of the canvas
    info->viewportRect = m_d->coordinatesConverter->imageToViewport(info->imageRect);

    info->borderWidth = 0;
    if (SCALE_MORE_OR_EQUAL_TO(physicalScaleX, physicalScaleY, 1.0)) {
        // The QPainter canvas backend maps KisOpenGL::FilterMode onto its two
        // available sampling modes: NearestFilterMode always forces pixel-exact
        // blitting on magnification, like in KisOpenGLCanvasRenderer.
        if (m_d->filterMode != KisOpenGL::NearestFilterMode
            && (SCALE_LESS_THAN(physicalScaleX, physicalScaleY, 2.0) || m_d->smoothHighZoom)) {
            info->renderHints = QPainter::SmoothPixmapTransform;
            info->borderWidth = borderSize;
        }
        info->transfer = KisPPUpdateInfo::DIRECT;
    } else { // <100%
        info->renderHints = QPainter::SmoothPixmapTransform;
        info->borderWidth = borderSize;
        info->transfer = KisPPUpdateInfo::PATCH;
    }

    dbgRender << "#####################################";
    dbgRender << ppVar(info->scaleX) << ppVar(info->scaleY);
    dbgRender << ppVar(info->borderWidth) << ppVar(info->renderHints);
    dbgRender << ppVar(info->transfer);
    dbgRender << ppVar(info->dirtyImageRectVar);
    dbgRender << "Not aligned rect of the canvas (raw):\t" << croppedViewRect;
    dbgRender << "Update rect in KisImage's pixels:\t" << info->imageRect;
    dbgRender << "Update rect in canvas' pixels:\t" << info->viewportRect;
    dbgRender << "#####################################";
}

void KisPrescaledProjection::updateScaledImage(KisPPUpdateInfoSP info)
{
    QPainter gc(&m_d->prescaledQImage);
    gc.setCompositionMode(QPainter::CompositionMode_Source);
    drawUsingBackend(gc, info);
}

void KisPrescaledProjection::drawUsingBackend(QPainter &gc, KisPPUpdateInfoSP info)
{
    if (info->imageRect.isEmpty()) return;

    if (info->transfer == KisPPUpdateInfo::DIRECT) {
        m_d->projectionBackend->drawFromOriginalImage(gc, info);
    } else /* if info->transfer == KisPPUpdateInformation::PATCH */ {
        KisImagePatch patch = m_d->projectionBackend->getNearestPatch(info);
        // prescale the patch because otherwise we'd scale using QPainter, which gives
        // a crap result compared to QImage's smoothscale. The patch is prescaled
        // to device pixels: the painter on the device-pixel prescaled image
        // converts logical units to device pixels automatically.
        const qreal devicePixelRatio = m_d->coordinatesConverter->devicePixelRatio();
        const QRectF devicePixelRect(info->viewportRect.topLeft() * devicePixelRatio,
                                     info->viewportRect.size() * devicePixelRatio);
        patch.preScale(devicePixelRect);
        patch.drawMe(gc, info->viewportRect, info->renderHints);
    }
}
