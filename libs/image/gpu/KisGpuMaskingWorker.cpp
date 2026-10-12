/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuMaskingWorker.h"

#include <atomic>

#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceMaths.h>
#include <KoCompositeOpRegistry.h>

#include "KisGpuBrushPainter.h"
#include "kis_default_bounds_base.h"
#include "kis_paint_device.h"

#ifdef HAVE_KRITA_GPU_ENGINE
#include <KisGpuCommandList.h>
#include <KisGpuContext.h>
#include <KisGpuMaskingCompositePass.h>

#include "KisGpuTileAccess.h"
#include "KisGpuTileBackend.h"
#include "KisPaintTrace.h"
#endif

namespace
{
std::atomic<quint64> s_runs{0};

/// KisMaskingBrushCompositeFuncTypes of the modes without strength (the
/// order of KisMaskingBrushCompositeOpFactory::create()), or -1
int modeFor(const QString &id)
{
    if (id == COMPOSITE_MULT)
        return 0;
    if (id == COMPOSITE_DARKEN)
        return 1;
    if (id == COMPOSITE_OVERLAY)
        return 2;
    if (id == COMPOSITE_DODGE)
        return 3;
    if (id == COMPOSITE_BURN)
        return 4;
    if (id == COMPOSITE_LINEAR_BURN)
        return 5;
    if (id == COMPOSITE_LINEAR_DODGE)
        return 6;
    if (id == COMPOSITE_HARD_MIX_PHOTOSHOP)
        return 7;
    if (id == COMPOSITE_HARD_MIX_SOFTER_PHOTOSHOP)
        return 8;
    if (id == COMPOSITE_SUBTRACT)
        return 9;
    return -1;
}

bool isRgbaF32(const KoColorSpace *colorSpace)
{
    return colorSpace && colorSpace->colorModelId() == RGBAColorModelID
        && colorSpace->colorDepthId() == Float32BitsColorDepthID && colorSpace->pixelSize() == 16;
}

// phase 4.106
bool isRgbaF16(const KoColorSpace *colorSpace)
{
    return colorSpace && colorSpace->colorModelId() == RGBAColorModelID
        && colorSpace->colorDepthId() == Float16BitsColorDepthID && colorSpace->pixelSize() == 8;
}
} // namespace

bool KisGpuMaskingWorker::supports(const KoColorSpace *colorSpace, const QString &compositeOpId)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    if (qgetenv("KRITA_GPU_MASKING") == "0" || !KisGpuBrushPainter::isEnabled()
        || !(isRgbaF32(colorSpace) || isRgbaF16(colorSpace)) || modeFor(compositeOpId) < 0)
        return false;
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    return backend && !backend->hasFailed() && backend->context().deviceInfo().supportsFloat64;
#else
    Q_UNUSED(colorSpace);
    Q_UNUSED(compositeOpId);
    return false;
#endif
}

quint64 KisGpuMaskingWorker::runCount()
{
    return s_runs.load();
}

bool KisGpuMaskingWorker::apply(KisPaintDeviceSP strokeDevice,
                                KisPaintDeviceSP maskDevice,
                                KisPaintDeviceSP dstDevice,
                                const QRect &rect,
                                const QString &compositeOpId)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    if (rect.isEmpty() || !strokeDevice || !maskDevice || !dstDevice)
        return false;
    const int mode = modeFor(compositeOpId);
    const bool halfTiles = isRgbaF16(strokeDevice->colorSpace());
    if (mode < 0 || !(halfTiles || isRgbaF32(strokeDevice->colorSpace()))
        || *strokeDevice->colorSpace() != *dstDevice->colorSpace() || maskDevice->pixelSize() != 2
        || !KisGpuTileAccess::isSupported(strokeDevice) || !KisGpuTileAccess::isSupported(dstDevice))
        return false;
    if (dstDevice->defaultBounds()->wrapAroundMode() || dstDevice->defaultBounds()->currentLevelOfDetail() != 0)
        return false;
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    if (!backend || backend->hasFailed())
        return false;
    KisGpuContext &context = backend->context();
    QString error;
    std::unique_ptr<KisGpuMaskingCompositePass> recorder = KisGpuMaskingCompositePass::create(context, &error);
    if (!recorder)
        return false;
    KisPaintTrace::Scope trace("masking.gpu_composite", dstDevice.data());

    KisGpuMaskingCompositePass::Pass pass;
    pass.rect = rect;
    pass.mode = mode;
    pass.half = halfTiles;
    // the mask byte as the CPU composite scales it (KisMaskingBrushCompositeOp)
    for (int i = 0; i < 256; ++i)
        pass.uint8ToFloat[i] = halfTiles ? float(KoColorSpaceMaths<quint8, half>::scaleToA(quint8(i)))
                                         : KoColorSpaceMaths<quint8, float>::scaleToA(quint8(i));
    // the mask is painted on the CPU: gray * alpha, as the CPU composite
    // takes it (KisMaskingBrushCompositeOp::preprocessMask())
    {
        const int pixels = rect.width() * rect.height();
        QByteArray grayAlpha(pixels * 2, Qt::Uninitialized);
        maskDevice->readBytes(reinterpret_cast<quint8 *>(grayAlpha.data()), rect);
        pass.mask.resize(pixels);
        const quint8 *src = reinterpret_cast<const quint8 *>(grayAlpha.constData());
        quint8 *dst = reinterpret_cast<quint8 *>(pass.mask.data());
        for (int i = 0; i < pixels; ++i)
            dst[i] = KoColorSpaceMaths<quint8>::multiply(src[2 * i], src[2 * i + 1]);
    }

    KisGpuCommandList commands(context);
    if (!commands.isValid())
        return false;
    KisGpuTileAccess strokeAccess(strokeDevice, rect, KisGpuTileAccess::ReadOnly);
    KisGpuTileAccess dstAccess(dstDevice, rect, KisGpuTileAccess::ReadWrite);
    const QVector<KisGpuTileAccess *> accesses{&strokeAccess, &dstAccess};
    commands.begin();
    if (!strokeAccess.prepare(commands, &error) || !dstAccess.prepare(commands, &error)) {
        KisGpuTileAccess::finishUnsubmitted(commands, accesses);
        return false;
    }
    // both devices share the tile grid of the image
    const QRect strokeGrid = strokeAccess.tileGrid();
    const QRect dstGrid = dstAccess.tileGrid();
    if (strokeGrid != dstGrid
        || strokeAccess.tileOrigin(strokeGrid.left(), strokeGrid.top())
            != dstAccess.tileOrigin(dstGrid.left(), dstGrid.top())) {
        KisGpuTileAccess::finishUnsubmitted(commands, accesses);
        return false;
    }
    pass.gridOrigin = dstAccess.tileOrigin(dstGrid.left(), dstGrid.top());
    pass.gridWidth = dstGrid.width();
    pass.gridHeight = dstGrid.height();
    pass.strokeTiles = strokeAccess.addresses();
    pass.dstTiles = dstAccess.addresses();
    if (!recorder->record(commands, pass, &error)) {
        KisGpuTileAccess::finishUnsubmitted(commands, accesses);
        return false;
    }
    const quint64 value = KisGpuTileAccess::submitAndFinish(commands, accesses);
    // the recorder's tables must outlive the work
    if (!value || !commands.wait())
        return false;
    ++s_runs;
    return true;
#else
    Q_UNUSED(strokeDevice);
    Q_UNUSED(maskDevice);
    Q_UNUSED(dstDevice);
    Q_UNUSED(rect);
    Q_UNUSED(compositeOpId);
    return false;
#endif
}
