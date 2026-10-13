/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisBrushStrokeLayer.h"

#include <QApplication>
#include <QMutexLocker>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QtMath>
#include <algorithm>

#include <KisAsynchronousStrokeUpdateHelper.h>
#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisUsageLogger.h>
#include <KisView.h>
#include <KoCanvasResourceProvider.h>
#include <KoColorSpace.h>
#include <KoCompositeOpRegistry.h>
#include <kis_command_utils.h>
#include <kis_debug.h>
#include <kis_distance_information.h>
#include <kis_floating_message.h>
#include <kis_global.h>
#include <kis_icon_utils.h>
#include <kis_image.h>
#include <kis_layer_utils.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <kis_paintop_preset.h>
#include <kis_paintop_settings.h>
#include <kis_pixel_selection.h>
#include <kis_resources_snapshot.h>
#include <kis_selection.h>
#include <kis_selection_filters.h>
#include <kis_transaction.h>
#include <klocalizedstring.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/freehand_stroke.h>

KisBrushStrokeLayer::KisBrushStrokeLayer(KisImageWSP image,
                                         const QString &name,
                                         quint8 opacity,
                                         const KoColorSpace *colorSpace)
    : KisPaintLayer(image, name, opacity, colorSpace)
{
}

KisBrushStrokeLayer::KisBrushStrokeLayer(const KisBrushStrokeLayer &rhs)
    : KisPaintLayer(rhs)
    , m_strokes(rhs.strokes())
{
}

KisBrushStrokeLayer::~KisBrushStrokeLayer()
{
}

KisNodeSP KisBrushStrokeLayer::clone() const
{
    return KisNodeSP(new KisBrushStrokeLayer(*this));
}

QIcon KisBrushStrokeLayer::icon() const
{
    // the Layers docker draws this small in the thumbnail's corner, so a
    // badge on the brush would not show: the warning replaces the brush
    // (scaling will scale the pixels)
    return KisIconUtils::loadIcon(recordState() == RecordState::NotMatching ? "warning" : "krita_tool_freehand");
}

QVector<KisRecordedBrushStrokeSP> KisBrushStrokeLayer::strokes() const
{
    QMutexLocker locker(&m_mutex);
    return m_strokes;
}

void KisBrushStrokeLayer::addStroke(KisRecordedBrushStrokeSP stroke)
{
    QMutexLocker locker(&m_mutex);
    m_strokes.append(stroke);
}

void KisBrushStrokeLayer::removeStroke(KisRecordedBrushStrokeSP stroke)
{
    QMutexLocker locker(&m_mutex);
    m_strokes.removeOne(stroke);
}

void KisBrushStrokeLayer::setStrokes(const QVector<KisRecordedBrushStrokeSP> &strokes)
{
    {
        QMutexLocker locker(&m_mutex);
        m_strokes = strokes;
    }
    m_generation.ref();
    // a loaded or replaced record is checked against the pixels
    requestRecordCheck();
}

KisBrushStrokeLayer::RecordState KisBrushStrokeLayer::recordState() const
{
    return RecordState(m_recordState.loadAcquire());
}

void KisBrushStrokeLayer::setDirty(const QVector<QRect> &rects)
{
    m_generation.ref();
    requestRecordCheck();
    KisPaintLayer::setDirty(rects);
}

void KisBrushStrokeLayer::markChangeRecorded()
{
    m_generation.ref();
    m_changeRecorded.storeRelease(1);
}

/**
 * Checks in the background whether the pixels of brush stroke layers are
 * still their recorded strokes, once their image is idle after a change, and
 * tells the user when a layer stops matching (it will be scaled as pixels) or
 * matches again. Lives in the GUI thread; the checks draw the record again
 * in a thread of its own, one layer at a time.
 */
class KisBrushStrokeLayerMonitor : public QObject
{
public:
    static KisBrushStrokeLayerMonitor *instance()
    {
        // never destroyed: a check may still run when the application quits
        static KisBrushStrokeLayerMonitor *monitor = new KisBrushStrokeLayerMonitor();
        return monitor;
    }

    /// GUI thread; a layer of an open document has asked for a check
    void schedule()
    {
        // after the change settles
        m_timer.start(1500);
    }

private:
    KisBrushStrokeLayerMonitor()
    {
        moveToThread(qApp->thread());
        m_timer.moveToThread(qApp->thread());
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this]() {
            process();
        });
        m_pool.setMaxThreadCount(1);
        // a check must not outlive the application's resources
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
            m_quitting = true;
            m_pool.waitForDone();
            m_running = nullptr;
        });
    }

    /**
     * Finds the layers that asked for a check in the open documents. The
     * requests carry no reference to the layer: a queued event that owns a
     * node would destroy it while Qt holds its event lock at exit.
     */
    void process()
    {
        if (m_quitting) {
            return;
        }
        bool waiting = false;
        for (const QPointer<KisDocument> &document : KisPart::instance()->documents()) {
            KisImageSP image = document ? document->image() : nullptr;
            if (!image) {
                continue;
            }
            const bool busy = m_running || !image->isIdle();
            KisLayerUtils::recursiveApplyNodes(image->root(), [&](KisNodeSP node) {
                KisBrushStrokeLayer *layer = dynamic_cast<KisBrushStrokeLayer *>(node.data());
                if (!layer || !layer->m_checkRequested.loadAcquire()) {
                    return;
                }
                if (busy || m_running) {
                    waiting = true;
                    return;
                }
                layer->m_checkRequested.storeRelease(0);
                // a brush stroke or a redraw keeps the record and the pixels
                // together; it cannot make a mismatching layer match either
                if (layer->m_changeRecorded.fetchAndStoreOrdered(0)
                    && layer->recordState() != KisBrushStrokeLayer::RecordState::Unknown) {
                    return;
                }
                if (layer->paintDevice()) {
                    startCheck(node);
                }
            });
        }
        if (waiting) {
            m_timer.start(1000);
        }
    }

    void startCheck(KisNodeSP node)
    {
        KisBrushStrokeLayer *layer = static_cast<KisBrushStrokeLayer *>(node.data());
        // the device copies the tiles on write; the layer may change meanwhile
        KisPaintDeviceSP pixels = new KisPaintDevice(*layer->paintDevice());
        const QVector<KisRecordedBrushStrokeSP> strokes = layer->strokes();
        const int generation = layer->m_generation.loadAcquire();
        m_running = node;
        KisBrushStrokeLayerMonitor *self = this;
        m_pool.start([self, layer, pixels, strokes, generation]() {
            QString difference;
            const bool matching = KisBrushStrokeLayer::strokesMatchPixels(strokes, pixels, &difference);
            // plain values only (see process())
            QMetaObject::invokeMethod(
                self,
                [self, layer, generation, matching, difference]() {
                    self->finished(layer, generation, matching, difference);
                },
                Qt::QueuedConnection);
        });
    }

    void finished(KisBrushStrokeLayer *layer, int generation, bool matching, const QString &difference)
    {
        if (m_quitting || m_running.data() != layer) {
            return;
        }
        // m_running keeps the layer alive until here
        KisNodeSP node = m_running;
        m_running = nullptr;
        // a later change has asked for its own check
        if (layer->m_generation.loadAcquire() == generation) {
            layer->setRecordState(matching ? KisBrushStrokeLayer::RecordState::Matching
                                           : KisBrushStrokeLayer::RecordState::NotMatching,
                                  difference);
        }
        m_timer.start(0);
    }

    KisNodeSP m_running;
    QTimer m_timer;
    QThreadPool m_pool;
    bool m_quitting = false;
};

namespace
{
/// A message on the canvas of a view showing @p image (GUI thread); the
/// image is only compared, never used
void showImageMessage(const void *image, const QString &message, const QIcon &icon)
{
    KisView *target = nullptr;
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    for (const QPointer<KisView> &view : KisPart::instance()->views()) {
        if (view && view->image().data() == image) {
            if (!target || (window && view.data() == window->activeView())) {
                target = view.data();
            }
        }
    }
    if (target) {
        target->showFloatingMessage(message, icon, 7000, KisFloatingMessage::High);
    }
}

/// showImageMessage() from any thread; plain values only (see
/// KisBrushStrokeLayerMonitor::process())
void postImageMessage(const void *image, const QString &message)
{
    if (!qApp) {
        return;
    }
    QMetaObject::invokeMethod(
        qApp,
        [image, message]() {
            showImageMessage(image, message, KisIconUtils::loadIcon("krita_tool_freehand"));
        },
        Qt::QueuedConnection);
}
} // namespace

void KisBrushStrokeLayer::requestRecordCheck()
{
    if (!qApp || !m_checkRequested.testAndSetOrdered(0, 1)) {
        return;
    }
    KisBrushStrokeLayerMonitor *monitor = KisBrushStrokeLayerMonitor::instance();
    QMetaObject::invokeMethod(
        monitor,
        [monitor]() {
            monitor->schedule();
        },
        Qt::QueuedConnection);
}

void KisBrushStrokeLayer::setRecordState(RecordState state, const QString &difference)
{
    const RecordState previous = RecordState(m_recordState.fetchAndStoreOrdered(int(state)));
    if (previous == state) {
        return;
    }
    baseNodeChangedCallback();

    QString message;
    if (state == RecordState::NotMatching) {
        KisUsageLogger::log(QStringLiteral("Brush stroke layer \"%1\": its pixels are no longer its %2 recorded "
                                           "strokes (%3); scaling will transform the pixels")
                                .arg(name())
                                .arg(strokes().size())
                                .arg(difference));
        message = i18nc("floating message",
                        "\"%1\" no longer matches its recorded brush strokes: an edit other than the brush "
                        "changed it. Scaling it will now scale its pixels. Undo that edit to keep it sharp.",
                        name());
    } else if (state == RecordState::Matching && previous == RecordState::NotMatching) {
        message = i18nc("floating message", "\"%1\" matches its recorded brush strokes again.", name());
    }
    const QIcon messageIcon =
        KisIconUtils::loadIcon(state == RecordState::NotMatching ? "warning" : "krita_tool_freehand");
    if (message.isEmpty()) {
        return;
    }

    showImageMessage(image().toStrongRef().data(), message, messageIcon);
}

namespace
{
/// The scale a transform applies to lengths (the square root of its area
/// scale)
qreal lengthScale(const QTransform &transform)
{
    return qSqrt(qAbs(transform.determinant()));
}

/**
 * Keeps a stroke's brush texture under the stroke when the stroke is drawn
 * elsewhere: points move by @p shift, then by @p transform (scaling and
 * moving). The texture is anchored to the canvas, not to the stroke: the
 * pattern pixel under image point X is X - offset, with the pattern scaled
 * by its scale (KisTextureOption). Without this a moved layer or selection
 * draws its strokes again over another part of the pattern. A random
 * offset (chosen per stroke) cannot follow.
 */
void mapTexture(KisPaintOpSettingsSP settings, const QTransform &transform, const QPointF &shift)
{
    if (!settings->hasProperty("Texture/Pattern/Scale")) {
        return;
    }
    const qreal scale = lengthScale(transform);
    if (!qFuzzyCompare(scale, 1.0)) {
        settings->setProperty("Texture/Pattern/Scale", settings->getDouble("Texture/Pattern/Scale") * scale);
    }
    // where the canvas origin of the stroke goes
    const QPointF origin = transform.map(shift);
    settings->setProperty("Texture/Pattern/OffsetX",
                          qRound(transform.m11() * settings->getInt("Texture/Pattern/OffsetX") + origin.x()));
    settings->setProperty("Texture/Pattern/OffsetY",
                          qRound(transform.m22() * settings->getInt("Texture/Pattern/OffsetY") + origin.y()));
}

/// A direction (radians) after @p transform
qreal mappedAngle(const QTransform &transform, qreal angle)
{
    const QPointF direction = transform.map(QPointF(qCos(angle), qSin(angle))) - transform.map(QPointF());
    return qAtan2(direction.y(), direction.x());
}

void drawStroke(KisImageSP image,
                KisPaintLayerSP layer,
                const KisRecordedBrushStroke &stroke,
                const QPointF &offset,
                const QTransform &transform)
{
    const qreal scale = lengthScale(transform);
    auto map = [&](const QPointF &point) {
        return transform.map(point + offset);
    };

    KisPaintOpPresetSP preset = stroke.preset->clone().dynamicCast<KisPaintOpPreset>();
    KisPaintOpSettingsSP settings = preset->settings();
    if (!qFuzzyCompare(scale, 1.0)) {
        settings->setPaintOpSize(settings->paintOpSize() * scale);
    }
    mapTexture(settings, transform, offset);

    KoCanvasResourceProvider provider;
    provider.setResource(KoCanvasResource::ForegroundColor, QVariant::fromValue(stroke.fgColor));
    provider.setResource(KoCanvasResource::BackgroundColor, QVariant::fromValue(stroke.bgColor));
    provider.setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));
    provider.setResource(KoCanvasResource::Opacity, stroke.opacity);
    provider.setResource(KoCanvasResource::CurrentCompositeOp, stroke.compositeOpId);
    provider.setResource(KoCanvasResource::CurrentEffectiveCompositeOp, stroke.compositeOpId);
    provider.setResource(KoCanvasResource::EffectiveZoom, stroke.effectiveZoom);
    provider.setResource(KoCanvasResource::HdrExposure, 0.0);
    provider.setResource(KoCanvasResource::EraserMode, false);
    provider.setResource(KoCanvasResource::GlobalAlphaLock, false);
    provider.setResource(KoCanvasResource::MirrorHorizontal, false);
    provider.setResource(KoCanvasResource::MirrorVertical, false);
    provider.setResource(KoCanvasResource::EffectiveLodAvailability, false);
    provider.setResource(KoCanvasResource::Size, settings->paintOpSize());
    if (stroke.pattern) {
        provider.setResource(KoCanvasResource::CurrentPattern, QVariant::fromValue(stroke.pattern));
    }
    if (stroke.gradient) {
        provider.setResource(KoCanvasResource::CurrentGradient, QVariant::fromValue(stroke.gradient));
    }

    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, layer, &provider, nullptr, {}, preset);
    resources->setOpacity(stroke.opacity);
    resources->setMirroring(false, false);
    resources->setFGColorOverride(stroke.fgColor);
    resources->setBGColorOverride(stroke.bgColor);

    // each copy's dab spacing starts as it did when the stroke was drawn
    QVector<KisFreehandStrokeInfo *> strokeInfos;
    for (int i = 0; i < qMax(1, stroke.strokeInfoCount); i++) {
        const KisRecordedBrushStroke::Start start =
            i < stroke.starts.size() ? stroke.starts[i] : KisRecordedBrushStroke::Start();
        if (start.hasLastDab) {
            strokeInfos << new KisFreehandStrokeInfo(KisDistanceInformation(map(start.lastPosition),
                                                                            mappedAngle(transform, start.lastAngle),
                                                                            start.spacingUpdateInterval,
                                                                            start.timingUpdateInterval,
                                                                            start.dabSeqNo));
        } else {
            strokeInfos << new KisFreehandStrokeInfo(
                KisDistanceInformation(start.spacingUpdateInterval, start.timingUpdateInterval, start.dabSeqNo));
        }
    }
    FreehandStrokeStrategy *strategy =
        new FreehandStrokeStrategy(resources, strokeInfos, kundo2_noi18n("redraw brush stroke"));
    strategy->setPreviewRandomSeed(stroke.seed);
    // as the stroke was drawn: its dabs one after another
    strategy->setSequentialDabRendering(true);
    KisStrokeId id = image->startStroke(strategy);

    for (const KisRecordedBrushStroke::Job &job : stroke.jobs) {
        KisPaintInformation pi1 = job.pi1;
        KisPaintInformation pi2 = job.pi2;
        pi1.setPos(map(pi1.pos()));
        pi2.setPos(map(pi2.pos()));
        switch (job.type) {
        case FreehandStrokeStrategy::Data::POINT:
            image->addJob(id, new FreehandStrokeStrategy::Data(job.strokeInfoId, pi1));
            break;
        case FreehandStrokeStrategy::Data::LINE:
            image->addJob(id, new FreehandStrokeStrategy::Data(job.strokeInfoId, pi1, pi2));
            break;
        case FreehandStrokeStrategy::Data::CURVE:
            image->addJob(
                id,
                new FreehandStrokeStrategy::Data(job.strokeInfoId, pi1, map(job.control1), map(job.control2), pi2));
            break;
        default:
            break;
        }
    }
    // the dabs are rendered asynchronously; an update job flushes them, as
    // the brush tool's update timer does
    image->addJob(id, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
    image->endStroke(id);
    image->waitForDone();
}
} // namespace

KisPaintDeviceSP KisBrushStrokeLayer::renderStrokes(const QVector<KisRecordedBrushStrokeSP> &strokes,
                                                    const KoColorSpace *colorSpace,
                                                    const QRect &imageBounds,
                                                    const QPoint &deviceOffset,
                                                    const QTransform &transform)
{
    const QRect bounds = transform.mapRect(QRectF(imageBounds)).toAlignedRect();
    KisImageSP image = new KisImage(nullptr,
                                    qMax(1, bounds.right() + 1),
                                    qMax(1, bounds.bottom() + 1),
                                    colorSpace,
                                    "brush stroke layer redraw");
    KisPaintLayerSP layer = new KisPaintLayer(image, "redraw", OPACITY_OPAQUE_U8, colorSpace);

    for (const KisRecordedBrushStrokeSP &stroke : strokes) {
        if (stroke && stroke->preset) {
            drawStroke(image, layer, *stroke, QPointF(deviceOffset - stroke->deviceOffset), transform);
        }
    }
    return layer->paintDevice();
}

namespace
{
/// Whether two devices hold the same pixels in @p rect, allowing a
/// difference of 3 in each 8-bit channel; otherwise @p difference describes
/// how they differ. The colors are compared premultiplied by alpha: the
/// color of a transparent pixel does not show, and smudging brushes leave
/// it different from what drawing again gives.
bool sameContent(KisPaintDeviceSP a,
                 KisPaintDeviceSP b,
                 const QRect &rect,
                 QString *difference,
                 KisPixelSelectionSP mismatch = nullptr,
                 int *differingCount = nullptr,
                 int *coveredCount = nullptr)
{
    if (rect.isEmpty()) {
        *difference = QStringLiteral("the layer is empty, the redrawn strokes are not");
        return b->exactBounds().isEmpty();
    }
    const QImage first = a->convertToQImage(nullptr, rect).convertToFormat(QImage::Format_ARGB32);
    const QImage second = b->convertToQImage(nullptr, rect).convertToFormat(QImage::Format_ARGB32);
    int differing = 0;
    int covered = 0;
    int largest = 0;
    QRect area;
    QVector<quint8> selected(mismatch ? rect.width() * rect.height() : 0, MIN_SELECTED);
    for (int y = 0; y < first.height(); y++) {
        const QRgb *lineA = reinterpret_cast<const QRgb *>(first.constScanLine(y));
        const QRgb *lineB = reinterpret_cast<const QRgb *>(second.constScanLine(y));
        for (int x = 0; x < first.width(); x++) {
            const QRgb p = lineA[x];
            const QRgb q = lineB[x];
            const int alphaP = qAlpha(p);
            const int alphaQ = qAlpha(q);
            auto premultiplied = [](int channel, int alpha) {
                return channel * alpha / 255;
            };
            const int channel =
                qMax(qMax(qAbs(alphaP - alphaQ), qAbs(premultiplied(qRed(p), alphaP) - premultiplied(qRed(q), alphaQ))),
                     qMax(qAbs(premultiplied(qGreen(p), alphaP) - premultiplied(qGreen(q), alphaQ)),
                          qAbs(premultiplied(qBlue(p), alphaP) - premultiplied(qBlue(q), alphaQ))));
            covered += qMax(alphaP, alphaQ) > 3;
            if (channel > 3) {
                differing++;
                largest = qMax(largest, channel);
                area |= QRect(rect.x() + x, rect.y() + y, 1, 1);
                if (mismatch) {
                    selected[y * rect.width() + x] = MAX_SELECTED;
                }
            }
        }
    }
    if (mismatch && differing) {
        mismatch->writeBytes(selected.constData(), rect);
    }
    if (differingCount) {
        *differingCount = differing;
    }
    if (coveredCount) {
        *coveredCount = covered;
    }
    *difference = QStringLiteral("%1 of %2 pixels differ, by up to %3, in %4,%5 %6x%7")
                      .arg(differing)
                      .arg(rect.width() * rect.height())
                      .arg(largest)
                      .arg(area.x())
                      .arg(area.y())
                      .arg(area.width())
                      .arg(area.height());
    return differing == 0;
}

/// Replaces the layer's strokes; undo restores the previous ones. A redraw
/// writes pixels that are the record (@p recorded); otherwise the layer
/// checks the new record against its pixels.
class SetStrokesCommand : public KUndo2Command
{
public:
    SetStrokesCommand(KisBrushStrokeLayer *layer,
                      const QVector<KisRecordedBrushStrokeSP> &before,
                      const QVector<KisRecordedBrushStrokeSP> &after,
                      bool recorded)
        : m_layer(layer)
        , m_before(before)
        , m_after(after)
        , m_recorded(recorded)
    {
    }

    void redo() override
    {
        m_layer->setStrokes(m_after);
        if (m_recorded) {
            m_layer->markChangeRecorded();
        }
    }

    void undo() override
    {
        m_layer->setStrokes(m_before);
        if (m_recorded) {
            m_layer->markChangeRecorded();
        }
    }

private:
    KisBrushStrokeLayerSP m_layer;
    QVector<KisRecordedBrushStrokeSP> m_before;
    QVector<KisRecordedBrushStrokeSP> m_after;
    bool m_recorded;
};

enum class SelectionCoverage {
    Outside,
    Inside,
    Crossing,
};

/// Where @p stroke lies relative to @p mask (selection values 0-255): the
/// Move Tool cuts the selected part of each pixel, so a stroke moves whole
/// only if every pixel it covers is fully selected
SelectionCoverage selectionCoverage(const KisRecordedBrushStrokeSP &stroke,
                                    const QPoint &deviceOffset,
                                    KisPaintDeviceSP mask,
                                    const QRect &maskBounds,
                                    const KoColorSpace *colorSpace)
{
    // the dab centres, widened by the brush size (generously)
    const QPointF shift(deviceOffset - stroke->deviceOffset);
    QRectF centres;
    for (const KisRecordedBrushStroke::Job &job : stroke->jobs) {
        centres |= QRectF(job.pi1.pos() + shift, QSizeF(0.01, 0.01));
        centres |= QRectF(job.pi2.pos() + shift, QSizeF(0.01, 0.01));
        if (job.type == FreehandStrokeStrategy::Data::CURVE) {
            centres |= QRectF(job.control1 + shift, QSizeF(0.01, 0.01));
            centres |= QRectF(job.control2 + shift, QSizeF(0.01, 0.01));
        }
    }
    const qreal margin = stroke->preset->settings()->paintOpSize() + 4.0;
    const QRect rough = centres.adjusted(-margin, -margin, margin, margin).toAlignedRect();
    if (!rough.intersects(maskBounds)) {
        return SelectionCoverage::Outside;
    }
    QVector<quint8> selected(rough.width() * rough.height());
    mask->readBytes(selected.data(), rough);
    if (std::all_of(selected.cbegin(), selected.cend(), [](quint8 value) {
            return value == MAX_SELECTED;
        })) {
        return SelectionCoverage::Inside;
    }

    // near the edge: the pixels the stroke covers when drawn alone
    KisPaintDeviceSP drawn =
        KisBrushStrokeLayer::renderStrokes({stroke}, colorSpace, rough | QRect(0, 0, 1, 1), deviceOffset);
    const QRect area = drawn->exactBounds();
    if (area.isEmpty()) {
        return SelectionCoverage::Outside;
    }
    const QImage pixels = drawn->convertToQImage(nullptr, area).convertToFormat(QImage::Format_ARGB32);
    QVector<quint8> values(area.width() * area.height());
    mask->readBytes(values.data(), area);
    bool inside = true;
    bool outside = true;
    for (int y = 0; y < area.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
        for (int x = 0; x < area.width(); x++) {
            const int alpha = qAlpha(line[x]);
            const int value = values[y * area.width() + x];
            // what the cut leaves behind or takes, within the redraw's
            // tolerance of 3
            inside = inside && alpha * (MAX_SELECTED - value) <= 3 * 255;
            outside = outside && alpha * value <= 3 * 255;
        }
    }
    return outside ? SelectionCoverage::Outside : (inside ? SelectionCoverage::Inside : SelectionCoverage::Crossing);
}
} // namespace

bool KisBrushStrokeLayer::strokesMatchPixels(const QVector<KisRecordedBrushStrokeSP> &strokes,
                                             KisPaintDeviceSP original,
                                             QString *difference)
{
    const QPoint offset(original->x(), original->y());
    const QRect bounds = original->exactBounds();
    KisPaintDeviceSP current = renderStrokes(strokes, original->colorSpace(), bounds | QRect(0, 0, 1, 1), offset);
    return sameContent(original, current, bounds, difference);
}

KUndo2Command *KisBrushStrokeLayer::createSelectionMoveCommand(KisPaintDeviceSP mask, const QPoint &offset)
{
    const QVector<KisRecordedBrushStrokeSP> recorded = strokes();
    KisPaintDeviceSP device = paintDevice();
    if (recorded.isEmpty() || !device || !mask || offset.isNull() || device->keyframeChannel()) {
        return nullptr;
    }
    const QPoint deviceOffset(device->x(), device->y());
    const QRect maskBounds = mask->exactBounds();

    QVector<KisRecordedBrushStrokeSP> staying;
    QVector<KisRecordedBrushStrokeSP> moving;
    for (const KisRecordedBrushStrokeSP &stroke : recorded) {
        switch (selectionCoverage(stroke, deviceOffset, mask, maskBounds, device->colorSpace())) {
        case SelectionCoverage::Outside:
            staying << stroke;
            break;
        case SelectionCoverage::Inside:
            moving << stroke;
            break;
        case SelectionCoverage::Crossing: {
            const QString message =
                QStringLiteral(
                    "Brush stroke layer \"%1\": a recorded stroke crosses the edge of the moved selection; "
                    "its record no longer matches the pixels")
                    .arg(name());
            warnKrita << message;
            KisUsageLogger::log(message);
            return nullptr;
        }
        }
    }
    if (moving.isEmpty()) {
        return nullptr;
    }
    // the moved pixels are pasted over the rest
    const QVector<KisRecordedBrushStrokeSP> after =
        staying + transformedStrokes(moving, deviceOffset, QTransform::fromTranslate(offset.x(), offset.y()));
    return new SetStrokesCommand(this, recorded, after, false);
}

QVector<KisRecordedBrushStrokeSP>
KisBrushStrokeLayer::transformedStrokes(const QVector<KisRecordedBrushStrokeSP> &strokes,
                                        const QPoint &deviceOffset,
                                        const QTransform &transform)
{
    const qreal scale = lengthScale(transform);
    QVector<KisRecordedBrushStrokeSP> result;
    for (const KisRecordedBrushStrokeSP &stroke : strokes) {
        QSharedPointer<KisRecordedBrushStroke> mapped(new KisRecordedBrushStroke(*stroke));
        const QPointF shift(deviceOffset - stroke->deviceOffset);
        auto map = [&](const QPointF &point) {
            return transform.map(point + shift);
        };
        for (KisRecordedBrushStroke::Job &job : mapped->jobs) {
            job.pi1.setPos(map(job.pi1.pos()));
            job.pi2.setPos(map(job.pi2.pos()));
            job.control1 = map(job.control1);
            job.control2 = map(job.control2);
        }
        for (KisRecordedBrushStroke::Start &start : mapped->starts) {
            if (start.hasLastDab) {
                start.lastPosition = map(start.lastPosition);
                start.lastAngle = mappedAngle(transform, start.lastAngle);
            }
        }
        mapped->deviceOffset = deviceOffset;

        mapped->preset = stroke->preset->clone().dynamicCast<KisPaintOpPreset>();
        KisPaintOpSettingsSP settings = mapped->preset->settings();
        settings->setPaintOpSize(settings->paintOpSize() * scale);
        mapTexture(settings, transform, shift);
        result << mapped;
    }
    return result;
}

KUndo2Command *KisBrushStrokeLayer::createTransformRedrawCommand(const QTransform &transform,
                                                                 KisPaintDeviceSP original,
                                                                 const PixelTransform &transformPixels)
{
    const QVector<KisRecordedBrushStrokeSP> recorded = strokes();
    KisPaintDeviceSP device = paintDevice();
    if (recorded.isEmpty() || !device || !original || device->keyframeChannel()) {
        return nullptr;
    }
    // scaling and moving only: a rotated or mirrored brush tip would not
    // match the transformed pixels
    if (transform.type() > QTransform::TxScale || transform.m11() <= 0.0 || transform.m22() <= 0.0) {
        return nullptr;
    }

    const QPoint offset(original->x(), original->y());
    const QRect bounds = original->exactBounds();
    const KoColorSpace *colorSpace = device->colorSpace();

    // where the layer holds just its recorded strokes
    KisPaintDeviceSP current = renderStrokes(recorded, colorSpace, bounds | QRect(0, 0, 1, 1), offset);
    KisPixelSelectionSP mismatch = new KisPixelSelection();
    QString difference;
    int differing = 0;
    int covered = 0;
    const bool same = sameContent(original, current, bounds, &difference, mismatch, &differing, &covered);
    // mostly not the record (or nothing to draw): the pixels are
    // transformed as on a paint layer
    if (!same && (bounds.isEmpty() || !transformPixels || differing * 2 > covered)) {
        const QString message = QStringLiteral(
                                    "Brush stroke layer \"%1\": its pixels are not its %2 recorded strokes (%3); "
                                    "transforming the pixels instead of drawing again")
                                    .arg(name())
                                    .arg(recorded.size())
                                    .arg(difference);
        warnKrita << message;
        KisUsageLogger::log(message);
        return nullptr;
    }

    const QVector<KisRecordedBrushStrokeSP> mapped = transformedStrokes(recorded, offset, transform);
    const QRect mappedBounds = transform.mapRect(QRectF(bounds)).toAlignedRect();
    KisPaintDeviceSP redrawn = renderStrokes(mapped, colorSpace, mappedBounds | QRect(0, 0, 1, 1), offset);

    if (!same) {
        // the part that is not the record keeps its pixels, transformed as
        // the caller transforms pixels and blended in at the edge; the
        // region grows to take in the antialiased edges around it
        const QRect mismatchRect = mismatch->selectedExactRect();
        KisGrowSelectionFilter(2, 2).process(mismatch, kisGrowRect(mismatchRect, 2));
        KisPaintDeviceSP pixels = transformPixels(original);
        KisPaintDeviceSP region = transformPixels(mismatch);

        KisSelectionSP selection = new KisSelection();
        const QRect regionRect = region->exactBounds();
        KisPainter::copyAreaOptimized(regionRect.topLeft(), region, selection->pixelSelection(), regionRect);
        const int feather = qMax(1, qRound(lengthScale(transform)));
        KisFeatherSelectionFilter(feather).process(selection->pixelSelection(), kisGrowRect(regionRect, feather));

        const QRect blendRect = selection->pixelSelection()->selectedExactRect();
        KisPainter gc(redrawn);
        gc.setCompositeOpId(COMPOSITE_COPY);
        gc.setSelection(selection);
        gc.bitBlt(blendRect.topLeft(), pixels, blendRect);
        gc.end();

        const int percent = qMax(1, qRound(100.0 * differing / qMax(1, covered)));
        KisUsageLogger::log(QStringLiteral("Brush stroke layer \"%1\": drawn again except where its pixels are not "
                                           "its %2 recorded strokes (%3); that part is transformed as pixels")
                                .arg(name())
                                .arg(recorded.size())
                                .arg(difference));
        postImageMessage(image().toStrongRef().data(),
                         i18nc("floating message",
                               "\"%1\" was drawn again, except about %2% of it that no longer matches its recorded "
                               "brush strokes: that part was scaled as pixels.",
                               name(),
                               percent));
    }

    KisCommandUtils::CompositeCommand *command = new KisCommandUtils::CompositeCommand();
    command->setText(kundo2_i18n("Redraw Brush Strokes"));
    command->addCommand(new SetStrokesCommand(this, recorded, mapped, true));

    KisTransaction transaction(device);
    device->clear();
    const QRect redrawnRect = redrawn->extent();
    if (!redrawnRect.isEmpty()) {
        KisPainter::copyAreaOptimized(redrawnRect.topLeft(), redrawn, device, redrawnRect);
    }
    command->addCommand(transaction.endAndTake());
    setDirty();

    return command;
}

KisRecordBrushStrokeCommand::KisRecordBrushStrokeCommand(KisBrushStrokeLayer *layer, KisRecordedBrushStrokeSP stroke)
    : m_layer(layer)
    , m_stroke(stroke)
{
}

void KisRecordBrushStrokeCommand::redo()
{
    m_layer->addStroke(m_stroke);
    m_layer->markChangeRecorded();
}

void KisRecordBrushStrokeCommand::undo()
{
    m_layer->removeStroke(m_stroke);
    m_layer->markChangeRecorded();
}
