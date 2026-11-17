/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisBrushStrokeLayer.h"
#include "KisBrushStrokeLayerPrivate.h"

#include <QApplication>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisUsageLogger.h>
#include <KisView.h>
#include <kis_default_bounds.h>
#include <kis_floating_message.h>
#include <kis_icon_utils.h>
#include <kis_image.h>
#include <kis_layer_utils.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

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

    /// set while a schedule() is on its way or the timer is waiting for
    /// process(): the layers' requests need not post another
    static QAtomicInt &scheduled()
    {
        static QAtomicInt value;
        return value;
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
        // requests from now on schedule another pass; a layer that is not in
        // an open document now (deleted, then restored by undo) keeps its
        // request for a later pass
        scheduled().storeRelease(0);
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
                if (layer->strokes().isEmpty()) {
                    // no record (a fresh layer, or the record was
                    // forgotten): nothing to check and nothing to warn about
                    layer->setRecordState(KisBrushStrokeLayer::RecordState::Unknown, QString());
                    return;
                }
                const bool unrecorded = layer->m_unrecordedChange.fetchAndStoreOrdered(0);
                const bool recorded = layer->m_changeRecorded.fetchAndStoreOrdered(0);
                // a brush stroke or a redraw keeps the record and the pixels
                // together; it cannot make a mismatching layer match either
                if (recorded && !unrecorded && layer->recordState() != KisBrushStrokeLayer::RecordState::Unknown) {
                    return;
                }
                // the record is never drawn again on an animated layer
                if (layer->paintDevice() && !layer->paintDevice()->keyframeChannel()) {
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
        // before the copy: a change after this point discards the result
        const int generation = layer->m_generation.loadAcquire();
        const QVector<KisRecordedBrushStrokeSP> strokes = layer->strokes();
        // the device copies the tiles on write; the layer may change
        // meanwhile. The copy holds the full resolution pixels, and bounds
        // of its own: the image's would switch it to the level of detail
        // the image works on at the time (Instant Preview) and would end
        // with the image
        KisPaintDeviceSP pixels = new KisPaintDevice(*layer->paintDevice());
        pixels->setDefaultBounds(new KisDefaultBounds());
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
        if (layer->m_generation.loadAcquire() == generation) {
            layer->setRecordState(matching ? KisBrushStrokeLayer::RecordState::Matching
                                           : KisBrushStrokeLayer::RecordState::NotMatching,
                                  difference);
        } else {
            // changed meanwhile: check again, even if the later change was
            // a recorded one, since this check's change is still unchecked
            layer->m_unrecordedChange.storeRelease(1);
            layer->m_checkRequested.storeRelease(1);
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

} // namespace

/// showImageMessage() from any thread; plain values only (see
/// KisBrushStrokeLayerMonitor::process())
void KisBrushStrokeLayerPrivate::postImageMessage(const void *image, const QString &message)
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

void KisBrushStrokeLayer::requestRecordCheck()
{
    m_checkRequested.storeRelease(1);
    if (!qApp || !KisBrushStrokeLayerMonitor::scheduled().testAndSetOrdered(0, 1)) {
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
