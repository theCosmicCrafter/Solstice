/*
 *  SPDX-FileCopyrightText: 2009 Cyrille Berger <cberger@cberger.net>
 *  SPDX-FileCopyrightText: 2014 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */


#include "overviewwidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QCursor>

#include <KoCanvasController.h>

#include <kis_canvas2.h>
#include <KisViewManager.h>
#include <kis_image.h>
#include <kis_signal_compressor.h>
#include <kis_config.h>
#include <QApplication>
#include "KisImageThumbnailStrokeStrategy.h"
#include <kis_display_color_converter.h>
#include <KisMainWindow.h>
#include "KisIdleTasksManager.h"
#include <KisDisplayConfig.h>
#include <kis_config_notifier.h>

#include <QtConcurrent>

OverviewWidget::OverviewWidget(QWidget * parent)
    : KisWidgetWithIdleTask<QWidget>(parent)
    , m_dragging(false)
{
    setMouseTracking(true);
    // No context menu here. Setting this avoids long-presses from delaying
    // inputs or dismissing the palette, see KisLongPressEventFilter.cpp.
    setContextMenuPolicy(Qt::PreventContextMenu);
    KisConfig cfg(true);
    slotThemeChanged();

    // Solstice live updates (see overviewwidget.h).
    m_liveCompressor = new KisSignalCompressor(100, KisSignalCompressor::FIRST_ACTIVE, this);
    connect(m_liveCompressor, SIGNAL(timeout()), SLOT(startLiveUpdate()));
    connect(&m_liveWatcher, SIGNAL(finished()), SLOT(finishLiveUpdate()));
    connect(KisConfigNotifier::instance(), SIGNAL(configChanged()), SLOT(slotConfigChanged()));
    slotConfigChanged();
    recalculatePreviewDimensions();
}

OverviewWidget::~OverviewWidget()
{
}

void OverviewWidget::setCanvas(KisCanvas2 *canvas)
{
    if (m_canvas) {
        m_canvas->image()->disconnect(this);
        m_canvas->displayColorConverter()->disconnect(this);
    }

    KisWidgetWithIdleTask<QWidget>::setCanvas(canvas);

    ++m_liveGeneration;
    {
        QMutexLocker locker(&m_dirtyLock);
        m_dirtyRect = QRect();
    }

    if (m_canvas) {
        // Solstice live updates: the same notification the canvas uses,
        // emitted from the image's worker threads.
        connect(m_canvas->image(),
                SIGNAL(sigImageUpdated(QRect)),
                this,
                SLOT(slotImageUpdated(QRect)),
                Qt::DirectConnection);
        connect(m_canvas->displayColorConverter(), SIGNAL(displayConfigurationChanged()), SLOT(startUpdateCanvasProjection()));
        connect(m_canvas->canvasController()->proxyObject, SIGNAL(canvasStateChanged()), this, SLOT(update()), Qt::UniqueConnection);
        connect(m_canvas->viewManager()->mainWindow(), SIGNAL(themeChanged()), this, SLOT(slotThemeChanged()), Qt::UniqueConnection);
    }
}

void OverviewWidget::recalculatePreviewDimensions()
{
    if (!m_canvas || !m_canvas->image()) {
        return;
    }

    QSize imageSize(m_canvas->image()->bounds().size());

    const qreal hScale = 1.0 * this->width() / imageSize.width();
    const qreal vScale = 1.0 * this->height() / imageSize.height();

    m_previewScale = qMin(hScale, vScale);
    m_previewSize = imageSize * m_previewScale;
    m_previewOrigin = calculatePreviewOrigin(m_previewSize);

}

KisIdleTasksManager::TaskGuard OverviewWidget::registerIdleTask(KisCanvas2 *canvas)
{
    return
        canvas->viewManager()->idleTasksManager()->
        addIdleTaskWithGuard([this](KisImageSP image) {
            const KisDisplayConfig config = m_canvas->displayColorConverter()->displayConfig();

            // If the widget is presented on a device with a pixel ratio > 1.0, we must compensate for it
            // by increasing the thumbnail's resolution. Otherwise it will appear blurry.
            QSize thumbnailSize = m_previewSize * devicePixelRatioF();

            if ((thumbnailSize.width() > image->width()) || (thumbnailSize.height() > image->height())) {
                thumbnailSize.scale(image->size(), Qt::KeepAspectRatio);
            }

            KisImageThumbnailStrokeStrategy *strategy =
                new KisImageThumbnailStrokeStrategy(image->projection(), image->bounds(), thumbnailSize, isPixelArt(), config.profile, config.intent, config.conversionFlags);

            connect(strategy, SIGNAL(thumbnailUpdated(QImage)), this, SLOT(updateThumbnail(QImage)));

            return strategy;
        });
}

void OverviewWidget::clearCachedState()
{
    m_pixmap = QPixmap();
    m_oldPixmap = QPixmap();
}

bool OverviewWidget::isPixelArt()
{
    return m_previewScale > 1;
}

QPointF OverviewWidget::calculatePreviewOrigin(QSize previewSize)
{
    return QPointF((width() - previewSize.width()) / 2.0f, (height() - previewSize.height()) / 2.0f);
}

QPolygonF OverviewWidget::previewPolygon()
{
    if (m_canvas) {
        const QRectF &canvasRect = QRectF(m_canvas->canvasWidget()->rect());
        return canvasToPreviewTransform().map(canvasRect);
    }
    return QPolygonF();
}

QTransform OverviewWidget::previewToCanvasTransform()
{
    QTransform previewToImage =
            QTransform::fromTranslate(-this->width() / 2.0, -this->height() / 2.0) *
            QTransform::fromScale(1.0 / m_previewScale, 1.0 / m_previewScale) *
            QTransform::fromTranslate(m_canvas->image()->width() / 2.0, m_canvas->image()->height() / 2.0);

    return previewToImage * m_canvas->coordinatesConverter()->imageToWidgetTransform();
}

QTransform OverviewWidget::canvasToPreviewTransform()
{
    return previewToCanvasTransform().inverted();
}

void OverviewWidget::startUpdateCanvasProjection()
{
    triggerCacheUpdate();
}

void OverviewWidget::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event);
    if (m_canvas) {
        if (!m_oldPixmap.isNull()) {
            recalculatePreviewDimensions();
            m_pixmap = m_oldPixmap.scaled(m_previewSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        triggerCacheUpdate();
    }
}

void OverviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (m_canvas) {
        QPointF previewPos = event->pos();

        if (!previewPolygon().containsPoint(previewPos, Qt::WindingFill)) {
            const QRect& canvasRect = m_canvas->canvasWidget()->rect();
            const QPointF newCanvasPos = previewToCanvasTransform().map(previewPos) -
                    QPointF(canvasRect.width() / 2.0f, canvasRect.height() / 2.0f);
            m_canvas->canvasController()->pan(newCanvasPos.toPoint());
        }
        m_lastPos = previewPos;
        m_dragging = true;
        Q_EMIT signalDraggingStarted();
    }
    event->accept();
    update();
}

void OverviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging) {
        QPointF previewPos = event->pos();
        const QPointF lastCanvasPos = previewToCanvasTransform().map(m_lastPos);
        const QPointF newCanvasPos = previewToCanvasTransform().map(event->pos());

        QPointF diff = newCanvasPos - lastCanvasPos;
        m_canvas->canvasController()->pan(diff.toPoint());
        m_lastPos = previewPos;
    }
    event->accept();
}

void OverviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragging) {
        m_dragging = false;
        Q_EMIT signalDraggingFinished();
    }
    event->accept();
    update();
}

void OverviewWidget::wheelEvent(QWheelEvent* event)
{
    if (m_canvas) {
        if (event->angleDelta().y() > 0) {
            m_canvas->canvasController()->zoomIn();
        } else {
            m_canvas->canvasController()->zoomOut();
        }
    }
}

void OverviewWidget::updateThumbnail(QImage pixmap)
{
    if (pixmap.size() != m_pixmap.size()) {
        ++m_liveGeneration; // pending live results were scaled for the old size
    }
    m_pixmap = QPixmap::fromImage(pixmap);
    m_oldPixmap = m_pixmap.copy();
    update();
}

void OverviewWidget::slotConfigChanged()
{
    m_liveUpdates = KisConfig(true).readEntry<bool>("Solstice/OverviewLiveUpdate", true);
}

void OverviewWidget::slotImageUpdated(const QRect &rect)
{
    // Worker thread: only collect the area and wake the GUI thread.
    if (!m_liveUpdates) {
        return;
    }
    {
        QMutexLocker locker(&m_dirtyLock);
        m_dirtyRect |= rect;
    }
    QMetaObject::invokeMethod(m_liveCompressor, "start", Qt::QueuedConnection);
}

void OverviewWidget::startLiveUpdate()
{
    if (!m_canvas || !m_canvas->image() || m_pixmap.isNull()) {
        return;
    }
    if (m_liveWatcher.isRunning()) {
        return; // finishLiveUpdate() picks up what accumulated meanwhile
    }
    QRect dirty;
    {
        QMutexLocker locker(&m_dirtyLock);
        dirty = m_dirtyRect;
        m_dirtyRect = QRect();
    }
    KisImageSP image = m_canvas->image();
    const QRect bounds = image->bounds();
    dirty &= bounds;
    if (dirty.isEmpty()) {
        return;
    }
    // Large changes (fills, filters, transforms) are left to the idle task's
    // full regeneration after the change.
    if (qint64(dirty.width()) * dirty.height() * 4 > qint64(bounds.width()) * bounds.height()) {
        return;
    }

    // The thumbnail pixels covering the change. They are sampled from the
    // projection at twice the thumbnail size (once for thumbnails as large as
    // the image) and only then converted to display colors and scaled down,
    // so the cost follows the thumbnail area, not the image area: about 2 ms
    // for a brush-sized change of a 2480x3508 RGBA32F image.
    const QSize thumbnailSize = m_pixmap.size();
    const qreal sx = qreal(thumbnailSize.width()) / bounds.width();
    const qreal sy = qreal(thumbnailSize.height()) / bounds.height();
    const QRect target = QRectF(dirty.x() * sx, dirty.y() * sy, dirty.width() * sx, dirty.height() * sy)
                             .toAlignedRect()
                             .adjusted(-1, -1, 1, 1)
        & QRect(QPoint(), thumbnailSize);
    if (target.isEmpty()) {
        return;
    }
    const int oversample =
        thumbnailSize.width() * 2 <= bounds.width() && thumbnailSize.height() * 2 <= bounds.height() ? 2 : 1;

    const KisDisplayConfig config = m_canvas->displayColorConverter()->displayConfig();
    const bool pixelArt = isPixelArt();
    const int generation = m_liveGeneration;
    KisPaintDeviceSP projection = image->projection();
    m_liveWatcher.setFuture(QtConcurrent::run([=]() {
        const QRect sampled(target.topLeft() * oversample, target.size() * oversample);
        KisPaintDeviceSP thumbnail = projection->createThumbnailDevice(thumbnailSize.width() * oversample,
                                                                       thumbnailSize.height() * oversample,
                                                                       bounds,
                                                                       sampled);
        QImage image = thumbnail->convertToQImage(config.profile,
                                                  sampled.x(),
                                                  sampled.y(),
                                                  sampled.width(),
                                                  sampled.height(),
                                                  config.intent,
                                                  config.conversionFlags);
        LiveResult result;
        result.target = target;
        result.generation = generation;
        result.image = oversample == 1 ? image
                                       : image.scaled(target.size(),
                                                      Qt::IgnoreAspectRatio,
                                                      pixelArt ? Qt::FastTransformation : Qt::SmoothTransformation);
        return result;
    }));
}

void OverviewWidget::finishLiveUpdate()
{
    const LiveResult result = m_liveWatcher.result();
    if (result.generation == m_liveGeneration && !m_pixmap.isNull() && !result.image.isNull()) {
        QPainter painter(&m_pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(result.target, result.image);
        painter.end();
        update();
    }
    bool pending = false;
    {
        QMutexLocker locker(&m_dirtyLock);
        pending = !m_dirtyRect.isEmpty();
    }
    if (pending) {
        m_liveCompressor->start();
    }
}

void OverviewWidget::slotThemeChanged()
{
    m_outlineColor = qApp->palette().color(QPalette::Highlight);
}


void OverviewWidget::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);

    if (m_canvas) {
        recalculatePreviewDimensions();
        QPainter p(this);

        const QRectF previewRect = QRectF(m_previewOrigin, m_previewSize);
        p.drawPixmap(previewRect.toRect(), m_pixmap);

        QRect r = rect();
        QPolygonF outline;
        outline << r.topLeft() << r.topRight() << r.bottomRight() << r.bottomLeft();

        QPen pen;
        pen.setColor(m_outlineColor);
        pen.setStyle(Qt::DashLine);

        p.setPen(pen);
        p.drawPolygon(outline.intersected(previewPolygon()));

        pen.setStyle(Qt::SolidLine);
        p.setPen(pen);
        p.drawPolygon(previewPolygon());

    }
}


