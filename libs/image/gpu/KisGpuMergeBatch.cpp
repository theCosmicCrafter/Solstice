/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuMergeBatch.h"
#include "KisPaintTrace.h"

#include <QBitArray>

#include <atomic>
#include <cstring>
#include <mutex>

#include <kis_debug.h>

#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoCompositeOpRegistry.h>

#include "KisGpuEngineSettings.h"
#include "kis_abstract_projection_plane.h"
#include "kis_default_bounds_base.h"
#include "kis_image.h"
#include "kis_layer.h"
#include "kis_layer_projection_plane.h"
#include "kis_paint_device.h"
#include "kis_painter.h"
#include "kis_projection_leaf.h"

#ifdef HAVE_KRITA_GPU_ENGINE
#include "KisGpuProjectionCompositor.h"
#include "KisGpuTileAccess.h"
#endif

namespace
{
/// -1: not decided yet (read the environment), 0: off, 1: on
std::atomic<int> s_enabled{-1};
std::atomic<quint64> s_gpuCompositeCount{0};

constexpr int TileSize = 64;
} // namespace

bool KisGpuMergeBatch::isEnabled()
{
#ifdef HAVE_KRITA_GPU_ENGINE
    // Decided once per process, by exactly one thread: switching while merges
    // run would mix GPU merges with CPU-style job exclusivity
    // (KisUpdaterContext). setEnabled() (tests) before the first call wins.
    static std::once_flag decided;
    std::call_once(decided, []() {
        int enabled = 0;
        if (qEnvironmentVariableIsSet("KRITA_GPU_PROJECTION")) {
            enabled = qEnvironmentVariableIntValue("KRITA_GPU_PROJECTION") == 1 ? 1 : 0;
        } else {
            enabled = KisGpuEngineSettings::enabledInConfig() ? 1 : 0;
        }
        int undecided = -1;
        s_enabled.compare_exchange_strong(undecided, enabled);
    });
    return s_enabled.load() == 1;
#else
    return false;
#endif
}

void KisGpuMergeBatch::setEnabled(bool enabled)
{
    s_enabled.store(enabled ? 1 : 0);
}

quint64 KisGpuMergeBatch::gpuCompositeCount()
{
    return s_gpuCompositeCount.load();
}

bool KisGpuMergeBatch::mayCompositeOnGpu(KisNodeSP node)
{
    if (!isEnabled() || !node) {
        return false;
    }
    KisImageSP image = node->image().toStrongRef();
    return KisGpuEngineSettings::isGpuColorSpace(image ? image->colorSpace() : node->colorSpace());
}

QRect KisGpuMergeBatch::tileAligned(const QRect &rect)
{
    if (rect.isEmpty()) {
        return rect;
    }
    auto floorTile = [](int v) {
        return (v >= 0 ? v / TileSize : -((-v + TileSize - 1) / TileSize)) * TileSize;
    };
    const int left = floorTile(rect.left());
    const int top = floorTile(rect.top());
    const int right = floorTile(rect.right()) + TileSize - 1;
    const int bottom = floorTile(rect.bottom()) + TileSize - 1;
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

KisGpuMergeBatch::KisGpuMergeBatch() = default;

KisGpuMergeBatch::~KisGpuMergeBatch()
{
    KIS_SAFE_ASSERT_RECOVER_NOOP(m_entries.isEmpty() && !m_clearPending && "GPU merge batch destroyed without flush()");
}

bool KisGpuMergeBatch::tryAdd(KisProjectionLeafSP leaf, KisPaintDeviceSP projection, const QRect &rect)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    if (!isEnabled() || !projection || rect.isEmpty()) {
        return false;
    }

    // Only plain layer planes: layer styles and other planes have their own apply().
    KisAbstractProjectionPlaneSP plane = leaf->projectionPlane();
    if (!dynamic_cast<KisLayerProjectionPlane *>(plane.data())) {
        return false;
    }
    KisLayer *layer = qobject_cast<KisLayer *>(leaf->node().data());
    if (!layer) {
        return false;
    }
    // KisUpdaterContext keeps jobs that share a tile apart only for these
    // images: a float group inside an RGBA8 image stays on the CPU.
    if (!mayCompositeOnGpu(KisNodeSP(layer))) {
        return false;
    }
    KisPaintDeviceSP device = layer->projection();
    if (!device) {
        return false;
    }

    KisGpuBlendOp op = KisGpuBlendOp::Over;
    // the leaf's blend mode differs from the layer's inside a clip stack
    const QString compositeOpId = leaf->compositeOpId();
    // Phase 4.115: Copy at full opacity (the mode of filter layers) replaces
    // the rect with the layer, its default pixel where it has no data:
    // KisLayerProjectionPlane does not clip this mode to the layer's extent.
    const bool replace = compositeOpId == COMPOSITE_COPY;
    if (replace) {
        if (leaf->opacity() != OPACITY_OPAQUE_U8 || device->defaultBounds()->wrapAroundMode()
            || std::memcmp(device->defaultPixel().data(), projection->defaultPixel().data(), device->pixelSize())) {
            return false;
        }
    } else if (!KisGpuProjectionCompositor::blendOpForCompositeOp(compositeOpId, &op)) {
        return false;
    }

    // Empty flags mean all channels; RGBA float channels use storage order.
    const QBitArray flags = leaf->channelFlags();
    bool alphaLocked = false;
    quint32 channelMask = 0xf;
    if (!flags.isEmpty()) {
        if (flags.size() != 4) {
            return false;
        }
        channelMask = 0;
        for (int i = 0; i < 4; ++i) {
            if (flags.testBit(i))
                channelMask |= 1u << i;
        }
        alphaLocked = !flags.testBit(3);
    }
    // KisPainter::bitBlt() copies whole pixels in the Copy mode when both
    // devices share their offset (fastBitBltPossible(); the color spaces are
    // compared below): the channel flags do nothing then.
    if (replace && projection->x() == device->x() && projection->y() == device->y()) {
        alphaLocked = false;
        channelMask = 0xf;
    }

    // The layer may sit at any offset (phase 4.112); the projection is
    // written in place, so only its tiles must be on the 64 px image grid.
    if (!(*device->colorSpace() == *projection->colorSpace()) || projection->x() % TileSize
        || projection->y() % TileSize) {
        return false;
    }

    static bool reportedUnavailable = false;
    if (!KisGpuTileAccess::isSupported(projection)) {
        if (!reportedUnavailable) {
            reportedUnavailable = true;
            QString reason;
            KisGpuTileAccess::isSupported(projection, &reason);
            dbgImage << "GPU projection not used:" << reason;
        }
        return false;
    }

    if ((!m_entries.isEmpty() || m_clearPending) && (m_projection != projection || m_rect != rect)) {
        flush();
    }
    // Replace entries lead a batch (KisGpuProjectionCompositor::composite()).
    if (replace && !m_entries.isEmpty() && !m_entries.last().replace) {
        flush();
    }

    m_projection = projection;
    m_rect = rect;
    Entry entry;
    entry.leaf = leaf;
    entry.device = device;
    entry.op = quint32(op);
    entry.opacity = leaf->opacity() / 255.0f;
    entry.alphaLocked = alphaLocked;
    entry.channelMask = channelMask;
    entry.replace = replace;
    m_entries << entry;
    return true;
#else
    Q_UNUSED(leaf);
    Q_UNUSED(projection);
    Q_UNUSED(rect);
    return false;
#endif
}

bool KisGpuMergeBatch::deferClear(KisNodeSP node, KisPaintDeviceSP projection, const QRect &rect)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    if (!projection || rect.isEmpty() || tileAligned(rect) == rect || !mayCompositeOnGpu(node)) {
        return false;
    }
    // The wrapped strategy splits clears at the wrap rect; composite() does not.
    if (projection->defaultBounds()->wrapAroundMode() || projection->x() % TileSize || projection->y() % TileSize
        || !KisGpuTileAccess::isSupported(projection)) {
        return false;
    }

    if (!m_entries.isEmpty() || m_clearPending) {
        flush();
    }
    m_projection = projection;
    m_rect = rect;
    m_clearPending = true;
    return true;
#else
    Q_UNUSED(node);
    Q_UNUSED(projection);
    Q_UNUSED(rect);
    return false;
#endif
}

void KisGpuMergeBatch::flush(bool cpuReadsNext)
{
    if (m_entries.isEmpty() && !m_clearPending) {
        return;
    }

    bool done = false;
#ifdef HAVE_KRITA_GPU_ENGINE
    if (!m_entries.isEmpty() || !cpuReadsNext) {
        QVector<KisGpuProjectionCompositor::Layer> layers;
        if (m_clearPending) {
            // A replace layer without tiles sets the rect to its default pixel.
            KisGpuProjectionCompositor::Layer clear;
            clear.device = clearSource();
            clear.replace = true;
            layers << clear;
        }
        for (const Entry &entry : m_entries) {
            KisGpuProjectionCompositor::Layer layer;
            layer.device = entry.device;
            layer.op = KisGpuBlendOp(entry.op);
            layer.opacity = entry.opacity;
            layer.alphaLocked = entry.alphaLocked;
            layer.channelMask = entry.channelMask;
            layer.replace = entry.replace;
            layers << layer;
        }

        QString error;
        done = KisGpuProjectionCompositor::composite(m_projection, m_rect, layers, &error);
        if (done) {
            if (!m_entries.isEmpty())
                s_gpuCompositeCount++;
        } else {
            warnImage << "GPU projection failed, compositing on the CPU:" << error;
        }
    }
#endif

    if (!done && m_clearPending) {
        m_projection->clear(m_rect);
    }
    if (!done && !m_entries.isEmpty()) {
        KisPaintTrace::link("path.projection.cpu_fallback", this, KisPaintTrace::currentFlow());
        compositeOnCpu();
    }

    m_entries.clear();
    m_clearPending = false;
    m_projection = nullptr;
    m_rect = QRect();
}

KisPaintDeviceSP KisGpuMergeBatch::clearSource()
{
    const KoColor defaultPixel = m_projection->defaultPixel();
    if (!m_clearSource || !(*m_clearSource->colorSpace() == *m_projection->colorSpace())
        || !(m_clearSource->defaultPixel() == defaultPixel)) {
        m_clearSource = new KisPaintDevice(m_projection->colorSpace());
        m_clearSource->setDefaultPixel(defaultPixel);
    }
    return m_clearSource;
}

void KisGpuMergeBatch::compositeOnCpu()
{
    // Exactly what KisAsyncMerger::compositeWithProjection() does per leaf.
    for (const Entry &entry : m_entries) {
        KisPainter gc(m_projection);
        entry.leaf->projectionPlane()->apply(&gc, m_rect);
    }
}
