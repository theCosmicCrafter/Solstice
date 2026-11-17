/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuPerspectiveWorker.h"

#include <atomic>

#ifdef HAVE_KRITA_GPU_ENGINE
#include <KisGpuCommandList.h>
#include <KisGpuContext.h>
#include <KisGpuPerspectivePass.h>

#include <KoColor.h>
#include <KoColorSpace.h>

#include <QElapsedTimer>

#include <cstring>
#include <half.h>

#include "KisGpuMergeBatch.h"
#include "KisGpuTileAccess.h"
#include "KisGpuTileBackend.h"
#include "KisPaintTrace.h"
#include "kis_default_bounds_base.h"
#include "kis_paint_device.h"
#endif

namespace
{
std::atomic<quint64> s_runs{0};

#ifdef HAVE_KRITA_GPU_ENGINE
void defaultPixelAsFloat(KisPaintDeviceSP device, float *pixel)
{
    const KoColor color = device->defaultPixel();
    if (device->pixelSize() == 8) {
        const half *channels = reinterpret_cast<const half *>(color.data());
        for (int i = 0; i < 4; ++i)
            pixel[i] = float(channels[i]);
    } else {
        std::memcpy(pixel, color.data(), 4 * sizeof(float));
    }
}

void setGrid(const KisGpuTileAccess &access, QPoint *origin, int *width, int *height, QVector<VkDeviceAddress> *tiles)
{
    const QRect grid = access.tileGrid();
    *origin = access.tileOrigin(grid.left(), grid.top());
    *width = grid.width();
    *height = grid.height();
    *tiles = access.addresses();
}
#endif
} // namespace

bool KisGpuPerspectiveWorker::isEnabled()
{
#ifdef HAVE_KRITA_GPU_ENGINE
    return KisGpuMergeBatch::isEnabled() && qgetenv("KRITA_GPU_TRANSFORM") != "0"
        && qgetenv("KRITA_GPU_PERSPECTIVE") != "0";
#else
    return false;
#endif
}

quint64 KisGpuPerspectiveWorker::runCount()
{
    return s_runs.load();
}

bool KisGpuPerspectiveWorker::canRun(KisPaintDeviceSP src, KisPaintDeviceSP dst)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    if (!isEnabled() || !src || !dst || src == dst)
        return false;
    if (!KisGpuTileAccess::isSupported(src) || !KisGpuTileAccess::isSupported(dst))
        return false;
    if (*src->colorSpace() != *dst->colorSpace() || src->pixelSize() != dst->pixelSize())
        return false;
    for (const KisPaintDeviceSP &device : {src, dst}) {
        if (device->defaultBounds()->wrapAroundMode() || device->defaultBounds()->currentLevelOfDetail() != 0)
            return false;
    }
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    return backend && !backend->hasFailed() && backend->context().deviceInfo().supportsFloat64;
#else
    Q_UNUSED(src);
    Q_UNUSED(dst);
    return false;
#endif
}

bool KisGpuPerspectiveWorker::run(KisPaintDeviceSP src,
                                  KisPaintDeviceSP dst,
                                  const QTransform &backward,
                                  const QRectF &srcClipRect,
                                  const Region &region,
                                  Mode mode,
                                  bool nearest)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    // An empty region or a clip rect that contains() reports false
    // everywhere paints nothing; the destination is already in the state
    // the CPU path leaves (cleared for runImpl(), untouched for
    // runPartialDst()).
    const QRectF clip = srcClipRect.normalized();
    if (region.bounds.isEmpty() || clip.isEmpty())
        return true;
    if (!canRun(src, dst) || region.rows.isEmpty() || region.spans.isEmpty())
        return false;
    KisGpuContext &context = KisGpuTileBackend::instance()->context();
    const KisGpuTileFormat format = src->pixelSize() == 8 ? KisGpuTileFormat::RGBA16F : KisGpuTileFormat::RGBA32F;
    QString error;
    std::unique_ptr<KisGpuPerspectivePass> recorder = KisGpuPerspectivePass::create(context, format, &error);
    if (!recorder)
        return false;
    KisPaintTrace::Scope trace("transform.gpu_perspective", dst.data());
    QElapsedTimer timer;
    timer.start();

    // Every pixel the shader may read: the clip rect is the only area
    // contains() passes, and bilinear/nearest sampling reaches at most two
    // pixels further (a point on the right/bottom edge samples floor+1).
    const QRect srcRect = clip.toAlignedRect().adjusted(-2, -2, 2, 2);
    KisGpuCommandList commands(context);
    if (!commands.isValid())
        return false;
    KisGpuTileAccess srcAccess(src, srcRect, KisGpuTileAccess::ReadOnly);
    KisGpuTileAccess dstAccess(dst,
                               region.bounds,
                               mode == OverwriteAll ? KisGpuTileAccess::WriteOnly : KisGpuTileAccess::ReadWrite);
    commands.begin();
    const QVector<KisGpuTileAccess *> accesses{&srcAccess, &dstAccess};
    if (!srcAccess.prepare(commands, &error) || !dstAccess.prepare(commands, &error)) {
        KisGpuTileAccess::finishUnsubmitted(commands, accesses);
        return false;
    }
    const qint64 prepared = timer.nsecsElapsed();

    KisGpuPerspectivePass::Pass pass;
    pass.backward[0] = backward.m11();
    pass.backward[1] = backward.m21();
    pass.backward[2] = backward.m31();
    pass.backward[3] = backward.m12();
    pass.backward[4] = backward.m22();
    pass.backward[5] = backward.m32();
    pass.backward[6] = backward.m13();
    pass.backward[7] = backward.m23();
    pass.backward[8] = backward.m33();
    pass.srcClipRect = clip;
    pass.mode = mode == KeepUnwritten ? KisGpuPerspectivePass::KeepUnwritten : KisGpuPerspectivePass::OverwriteAll;
    pass.nearest = nearest;
    pass.regionTop = region.bounds.top();
    pass.rows = region.rows;
    pass.spans = region.spans;
    setGrid(srcAccess, &pass.srcGridOrigin, &pass.srcGridWidth, &pass.srcGridHeight, &pass.srcTiles);
    setGrid(dstAccess, &pass.dstGridOrigin, &pass.dstGridWidth, &pass.dstGridHeight, &pass.dstTiles);
    defaultPixelAsFloat(src, pass.defaultPixel);
    defaultPixelAsFloat(dst, pass.clearPixel);
    if (!recorder->record(commands, pass, &error)) {
        KisGpuTileAccess::finishUnsubmitted(commands, accesses);
        return false;
    }
    const qint64 recorded = timer.nsecsElapsed();
    const quint64 value = KisGpuTileAccess::submitAndFinish(commands, accesses);
    const qint64 submitted = timer.nsecsElapsed();
    if (!value || !commands.wait())
        return false;
    const qint64 waited = timer.nsecsElapsed();
    if (recorder->failed()) {
        if (qEnvironmentVariableIntValue("KRITA_GPU_TRANSFORM_DEBUG") == 1)
            qInfo() << "GPU perspective transform: a sample fell outside the source rect, using the CPU";
        return false;
    }
    // The caller continues on the CPU (further passes, the projection);
    // download the result in one batch instead of tile by tile.
    KisGpuTileAccess::syncToCpu(dst, region.bounds);
    if (qEnvironmentVariableIntValue("KRITA_GPU_TRANSFORM_DEBUG") == 1)
        qInfo() << "GPU perspective transform" << region.bounds << (nearest ? "nearest" : "bilinear") << "prepare ms"
                << prepared / 1e6 << "record ms" << (recorded - prepared) / 1e6 << "submit ms"
                << (submitted - recorded) / 1e6 << "wait ms" << (waited - submitted) / 1e6 << "download ms"
                << (timer.nsecsElapsed() - waited) / 1e6 << "src tiles" << pass.srcTiles.size() << "dst tiles"
                << pass.dstTiles.size();
    ++s_runs;
    return true;
#else
    Q_UNUSED(src);
    Q_UNUSED(dst);
    Q_UNUSED(backward);
    Q_UNUSED(srcClipRect);
    Q_UNUSED(region);
    Q_UNUSED(mode);
    Q_UNUSED(nearest);
    return false;
#endif
}
