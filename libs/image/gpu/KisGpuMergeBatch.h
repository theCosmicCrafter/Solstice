/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUMERGEBATCH_H
#define KISGPUMERGEBATCH_H

#include <QRect>
#include <QVector>

#include "kis_types.h"
#include "kritaimage_export.h"

/**
 * GPU engine phase 2: collects consecutive layer composites of one
 * KisAsyncMerger pass and runs them as a single GPU dispatch
 * (docs/agent/gpu-engine.md).
 *
 * KisAsyncMerger offers every composite to tryAdd(). Supported layers are
 * deferred; anything else makes the merger flush() the batch and composite on
 * the CPU as before. The merger also flushes before anything reads the
 * projection (adjustment layers, writing the projection, the end of a pass),
 * so the result is identical to compositing each layer immediately.
 *
 * Without the GPU engine, or while it is disabled, tryAdd() always returns
 * false and the merger behaves exactly like upstream Krita.
 */
class KRITAIMAGE_EXPORT KisGpuMergeBatch
{
public:
    /**
     * GPU projection is enabled by the Solstice/GpuEngine setting (read once
     * per process, KisGpuEngineSettings), overridden by the
     * KRITA_GPU_PROJECTION environment variable (1 or 0) or setEnabled()
     * in tests, and requires a usable GPU.
     */
    static bool isEnabled();
    static void setEnabled(bool enabled);
    /// Number of batches composited on the GPU so far (diagnostics, tests).
    static quint64 gpuCompositeCount();
    /**
     * True if merges of @p node's image may composite on the GPU: the GPU
     * projection is enabled and the image is RGBA F32/F16. Only then do
     * merge jobs that share a tile have to be kept apart, so tryAdd()
     * refuses leaves of other images (even into an RGBA float group).
     */
    static bool mayCompositeOnGpu(KisNodeSP node);
    /// @p rect grown to the 64x64 tile grid of the image.
    static QRect tileAligned(const QRect &rect);

    KisGpuMergeBatch();
    ~KisGpuMergeBatch();

    /**
     * Defers compositing @p leaf onto @p projection inside @p rect.
     * Returns false if the leaf cannot be composited on the GPU; the caller
     * must then flush() and composite it on the CPU itself.
     */
    bool tryAdd(KisProjectionLeafSP leaf, KisPaintDeviceSP projection, const QRect &rect);

    /**
     * Phase 4.111: defers KisAsyncMerger::setupProjection()'s clear of
     * @p rect in @p projection (a destination of @p node's image). The next
     * batch on the same projection and rect first replaces the rect with the
     * projection's default pixel on the GPU, so the partly covered border
     * tiles of an unaligned rect are not downloaded for a CPU clear.
     *
     * Returns false if the caller must clear on the CPU itself: the GPU
     * projection is off, the image is not RGBA F32/F16, the rect is tile
     * aligned (a CPU clear of whole tiles only drops them), or the projection
     * wraps around.
     */
    bool deferClear(KisNodeSP node, KisPaintDeviceSP projection, const QRect &rect);

    /**
     * Composites every deferred leaf: on the GPU, or on the CPU if the GPU fails.
     * @p cpuReadsNext: the caller accesses the projection on the CPU right
     * after. A deferred clear with no leaf to composite is then done on the
     * CPU: the CPU access downloads its border tiles anyway.
     */
    void flush(bool cpuReadsNext = false);

private:
    struct Entry {
        KisProjectionLeafSP leaf;
        KisPaintDeviceSP device;
        quint32 op = 0;
        float opacity = 1.0f;
        bool alphaLocked = false;
        quint32 channelMask = 0xf;
        /// The Copy mode at full opacity (phase 4.115); such entries lead the batch.
        bool replace = false;
    };

    void compositeOnCpu();
    /// An empty device with the projection's color space and default pixel.
    KisPaintDeviceSP clearSource();

    KisPaintDeviceSP m_projection;
    QRect m_rect;
    QVector<Entry> m_entries;
    /// m_rect of m_projection is cleared before the entries are composited.
    bool m_clearPending = false;
    KisPaintDeviceSP m_clearSource;
};

#endif // KISGPUMERGEBATCH_H
