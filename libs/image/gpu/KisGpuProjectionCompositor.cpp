/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuProjectionCompositor.h"
#include "KisPaintTrace.h"

#include "KisGpuTileAccess.h"
#include "KisGpuTileBackend.h"

#include <KisGpuCommandList.h>
#include <KisGpuContext.h>
#include <KisGpuTilePool.h>

#include <KoColor.h>
#include <KoColorSpace.h>
#include <KoCompositeOpRegistry.h>

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cstring>
#include <half.h>
#include <memory>
#include <vector>

#include <kis_debug.h>

#include "kis_paint_device.h"

namespace
{
constexpr int TileSize = 64;

/// Exclusively leased by one composite() call. Commands and shader tables
/// remain untouched until lastUse completes, including coverage snapshots.
struct WorkContext {
    explicit WorkContext(KisGpuContext &context)
        : commands(context)
    {
    }

    KisGpuCommandList commands;
    std::unique_ptr<KisGpuLayerCompositor> compositor32;
    std::unique_ptr<KisGpuLayerCompositor> compositor16;
    quint64 lastUse = 0;
};

QMutex s_contextsMutex;
/**
 * Intentionally never destroyed, like KisGpuTileBackend: destroying Vulkan
 * objects during static destruction runs after the Vulkan loader and layers
 * may already have been torn down (crashed with the validation layer).
 */
std::vector<std::unique_ptr<WorkContext>> *s_freeContexts = new std::vector<std::unique_ptr<WorkContext>>();
size_t s_contextCount = 0;
constexpr size_t MinimumContexts = 3;

std::unique_ptr<WorkContext> acquireContext(KisGpuContext &context)
{
    const quint64 completed = context.completedValue();
    QMutexLocker locker(&s_contextsMutex);
    const auto oldest =
        std::min_element(s_freeContexts->begin(), s_freeContexts->end(), [](const auto &a, const auto &b) {
            return a->lastUse < b->lastUse;
        });
    if (oldest != s_freeContexts->end() && ((*oldest)->lastUse <= completed || s_contextCount >= MinimumContexts)) {
        std::unique_ptr<WorkContext> work = std::move(*oldest);
        s_freeContexts->erase(oldest);
        return work;
    }
    // Keep up to three serial submissions in flight. Beyond that, grow only
    // when all contexts are leased by concurrent callers, as before.
    ++s_contextCount;
    locker.unlock();
    return std::unique_ptr<WorkContext>(new WorkContext(context));
}

/// The device's default pixel in tile memory order (RGBA F32 or F16), as float.
void defaultPixelAsFloat(KisPaintDeviceSP device, float *fill)
{
    const KoColor pixel = device->defaultPixel();
    if (device->pixelSize() == 8) {
        const half *channels = reinterpret_cast<const half *>(pixel.data());
        for (int i = 0; i < 4; ++i)
            fill[i] = float(channels[i]);
    } else {
        std::memcpy(fill, pixel.data(), 4 * sizeof(float));
    }
}

void releaseContext(std::unique_ptr<WorkContext> work)
{
    QMutexLocker locker(&s_contextsMutex);
    s_freeContexts->push_back(std::move(work));
}
} // namespace

bool KisGpuProjectionCompositor::resetWorkContextsForTesting()
{
    std::vector<std::unique_ptr<WorkContext>> retired;
    {
        QMutexLocker locker(&s_contextsMutex);
        if (s_contextCount != s_freeContexts->size())
            return false;
        retired.swap(*s_freeContexts);
        s_contextCount = 0;
    }
    // Destructors wait for submitted commands; never hold the pool lock here.
    return true;
}

int KisGpuProjectionCompositor::workContextCountForTesting()
{
    QMutexLocker locker(&s_contextsMutex);
    return int(s_contextCount);
}

int KisGpuProjectionCompositor::leasedWorkContextCountForTesting()
{
    QMutexLocker locker(&s_contextsMutex);
    return int(s_contextCount - s_freeContexts->size());
}

bool KisGpuProjectionCompositor::blendOpForCompositeOp(const QString &id, KisGpuBlendOp *op)
{
    static const QHash<QString, KisGpuBlendOp> ops = {
        {COMPOSITE_OVER, KisGpuBlendOp::Over},
        {COMPOSITE_MULT, KisGpuBlendOp::Multiply},
        {COMPOSITE_SCREEN, KisGpuBlendOp::Screen},
        {COMPOSITE_ADD, KisGpuBlendOp::Add},
        {COMPOSITE_LINEAR_DODGE, KisGpuBlendOp::Add},
        {COMPOSITE_SUBTRACT, KisGpuBlendOp::Subtract},
        {COMPOSITE_DARKEN, KisGpuBlendOp::Darken},
        {COMPOSITE_LIGHTEN, KisGpuBlendOp::Lighten},
        {COMPOSITE_DIFF, KisGpuBlendOp::Difference},
        {COMPOSITE_OVERLAY, KisGpuBlendOp::Overlay},
        {COMPOSITE_HARD_LIGHT, KisGpuBlendOp::HardLight},
        {COMPOSITE_EXCLUSION, KisGpuBlendOp::Exclusion},
        {COMPOSITE_LINEAR_BURN, KisGpuBlendOp::LinearBurn},
        {COMPOSITE_LINEAR_LIGHT, KisGpuBlendOp::LinearLight},
        {COMPOSITE_PIN_LIGHT, KisGpuBlendOp::PinLight},
        {COMPOSITE_SOFT_LIGHT_SVG, KisGpuBlendOp::SoftLightSvg},
        {COMPOSITE_SOFT_LIGHT_PHOTOSHOP, KisGpuBlendOp::SoftLightPhotoshop},
        {COMPOSITE_DODGE, KisGpuBlendOp::ColorDodge},
        {COMPOSITE_BURN, KisGpuBlendOp::ColorBurn},
        {COMPOSITE_DIVIDE, KisGpuBlendOp::Divide},
        {COMPOSITE_VIVID_LIGHT, KisGpuBlendOp::VividLight},
        {COMPOSITE_HARD_MIX, KisGpuBlendOp::HardMix},
        {COMPOSITE_HARD_MIX_PHOTOSHOP, KisGpuBlendOp::HardMixPhotoshop},
        {COMPOSITE_HARD_MIX_SOFTER_PHOTOSHOP, KisGpuBlendOp::HardMixSofterPhotoshop},
        {COMPOSITE_GRAIN_MERGE, KisGpuBlendOp::GrainMerge},
        {COMPOSITE_GRAIN_EXTRACT, KisGpuBlendOp::GrainExtract},
        {COMPOSITE_NEGATION, KisGpuBlendOp::Negation},
        {COMPOSITE_ALLANON, KisGpuBlendOp::Allanon},
        {COMPOSITE_HUE, KisGpuBlendOp::Hue},
        {COMPOSITE_SATURATION, KisGpuBlendOp::Saturation},
        {COMPOSITE_COLOR, KisGpuBlendOp::Color},
        {COMPOSITE_LUMINIZE, KisGpuBlendOp::Luminosity},
        {COMPOSITE_DARKER_COLOR, KisGpuBlendOp::DarkerColor},
        {COMPOSITE_LIGHTER_COLOR, KisGpuBlendOp::LighterColor},
        {COMPOSITE_TINT, KisGpuBlendOp::Tint},
        {COMPOSITE_INC_SATURATION, KisGpuBlendOp::IncreaseSaturation},
        {COMPOSITE_DEC_SATURATION, KisGpuBlendOp::DecreaseSaturation},
        {COMPOSITE_INC_LUMINOSITY, KisGpuBlendOp::IncreaseLuminosity},
        {COMPOSITE_DEC_LUMINOSITY, KisGpuBlendOp::DecreaseLuminosity},
        {COMPOSITE_COLOR_HSI, KisGpuBlendOp::ColorHsi},
        {COMPOSITE_HUE_HSI, KisGpuBlendOp::HueHsi},
        {COMPOSITE_SATURATION_HSI, KisGpuBlendOp::SaturationHsi},
        {COMPOSITE_INC_SATURATION_HSI, KisGpuBlendOp::IncreaseSaturationHsi},
        {COMPOSITE_DEC_SATURATION_HSI, KisGpuBlendOp::DecreaseSaturationHsi},
        {COMPOSITE_INTENSITY, KisGpuBlendOp::Intensity},
        {COMPOSITE_INC_INTENSITY, KisGpuBlendOp::IncreaseIntensity},
        {COMPOSITE_DEC_INTENSITY, KisGpuBlendOp::DecreaseIntensity},
        {COMPOSITE_COLOR_HSL, KisGpuBlendOp::ColorHsl},
        {COMPOSITE_HUE_HSL, KisGpuBlendOp::HueHsl},
        {COMPOSITE_SATURATION_HSL, KisGpuBlendOp::SaturationHsl},
        {COMPOSITE_INC_SATURATION_HSL, KisGpuBlendOp::IncreaseSaturationHsl},
        {COMPOSITE_DEC_SATURATION_HSL, KisGpuBlendOp::DecreaseSaturationHsl},
        {COMPOSITE_LIGHTNESS, KisGpuBlendOp::Lightness},
        {COMPOSITE_INC_LIGHTNESS, KisGpuBlendOp::IncreaseLightness},
        {COMPOSITE_DEC_LIGHTNESS, KisGpuBlendOp::DecreaseLightness},
        {COMPOSITE_COLOR_HSV, KisGpuBlendOp::ColorHsv},
        {COMPOSITE_HUE_HSV, KisGpuBlendOp::HueHsv},
        {COMPOSITE_SATURATION_HSV, KisGpuBlendOp::SaturationHsv},
        {COMPOSITE_INC_SATURATION_HSV, KisGpuBlendOp::IncreaseSaturationHsv},
        {COMPOSITE_DEC_SATURATION_HSV, KisGpuBlendOp::DecreaseSaturationHsv},
        {COMPOSITE_VALUE, KisGpuBlendOp::Value},
        {COMPOSITE_INC_VALUE, KisGpuBlendOp::IncreaseValue},
        {COMPOSITE_DEC_VALUE, KisGpuBlendOp::DecreaseValue},
        {COMPOSITE_HARD_MIX_HDR, KisGpuBlendOp::HardMixHdr},
        {COMPOSITE_DODGE_HDR, KisGpuBlendOp::ColorDodgeHdr},
        {COMPOSITE_VIVID_LIGHT_HDR, KisGpuBlendOp::VividLightHdr},
        {COMPOSITE_HARD_OVERLAY, KisGpuBlendOp::HardOverlay},
        {COMPOSITE_HARD_OVERLAY_HDR, KisGpuBlendOp::HardOverlayHdr},
        {COMPOSITE_GEOMETRIC_MEAN, KisGpuBlendOp::GeometricMean},
        {COMPOSITE_PARALLEL, KisGpuBlendOp::Parallel},
        {COMPOSITE_INTERPOLATION, KisGpuBlendOp::Interpolation},
        {COMPOSITE_INTERPOLATIONB, KisGpuBlendOp::Interpolation2x},
        {COMPOSITE_PENUMBRAA, KisGpuBlendOp::PenumbraA},
        {COMPOSITE_PENUMBRAB, KisGpuBlendOp::PenumbraB},
        {COMPOSITE_PENUMBRAC, KisGpuBlendOp::PenumbraC},
        {COMPOSITE_PENUMBRAD, KisGpuBlendOp::PenumbraD},
        {COMPOSITE_SOFT_LIGHT_IFS_ILLUSIONS, KisGpuBlendOp::SoftLightIfsIllusions},
        {COMPOSITE_SOFT_LIGHT_PEGTOP_DELPHI, KisGpuBlendOp::SoftLightPegtopDelphi},
        {COMPOSITE_GAMMA_LIGHT, KisGpuBlendOp::GammaLight},
        {COMPOSITE_GAMMA_ILLUMINATION, KisGpuBlendOp::GammaIllumination},
        {COMPOSITE_FLAT_LIGHT, KisGpuBlendOp::FlatLight},
        {COMPOSITE_PNORM_A, KisGpuBlendOp::PNormA},
        {COMPOSITE_PNORM_B, KisGpuBlendOp::PNormB},
        {COMPOSITE_SUPER_LIGHT, KisGpuBlendOp::SuperLight},
        {COMPOSITE_TINT_IFS_ILLUSIONS, KisGpuBlendOp::TintIfsIllusions},
        {COMPOSITE_FOG_LIGHTEN_IFS_ILLUSIONS, KisGpuBlendOp::FogLightenIfsIllusions},
        {COMPOSITE_EASY_DODGE, KisGpuBlendOp::EasyDodge},
        {COMPOSITE_GAMMA_DARK, KisGpuBlendOp::GammaDark},
        {COMPOSITE_SHADE_IFS_ILLUSIONS, KisGpuBlendOp::ShadeIfsIllusions},
        {COMPOSITE_FOG_DARKEN_IFS_ILLUSIONS, KisGpuBlendOp::FogDarkenIfsIllusions},
        {COMPOSITE_EASY_BURN, KisGpuBlendOp::EasyBurn},
        {COMPOSITE_INVERSE_SUBTRACT, KisGpuBlendOp::InverseSubtract},
        {COMPOSITE_MOD, KisGpuBlendOp::Modulo},
        {COMPOSITE_MODULO_SHIFT, KisGpuBlendOp::ModuloShift},
        {COMPOSITE_MODULO_SHIFT_CON, KisGpuBlendOp::ModuloShiftContinuous},
        {COMPOSITE_ARC_TANGENT, KisGpuBlendOp::ArcTangent},
        {COMPOSITE_EQUIVALENCE, KisGpuBlendOp::Equivalence},
        {COMPOSITE_ADDITIVE_SUBTRACTIVE, KisGpuBlendOp::AdditiveSubtractive},
        {COMPOSITE_REFLECT, KisGpuBlendOp::Reflect},
        {COMPOSITE_GLOW, KisGpuBlendOp::Glow},
        {COMPOSITE_FREEZE, KisGpuBlendOp::Freeze},
        {COMPOSITE_HEAT, KisGpuBlendOp::Heat},
        {COMPOSITE_GLEAT, KisGpuBlendOp::Gleat},
        {COMPOSITE_HELOW, KisGpuBlendOp::Helow},
        {COMPOSITE_REEZE, KisGpuBlendOp::Reeze},
        {COMPOSITE_FRECT, KisGpuBlendOp::Frect},
        {COMPOSITE_FHYRD, KisGpuBlendOp::Fhyrd},
        {COMPOSITE_LAMBERT_LIGHTING, KisGpuBlendOp::LambertLighting},
        {COMPOSITE_LAMBERT_LIGHTING_GAMMA_2_2, KisGpuBlendOp::LambertLightingGamma22},
        {COMPOSITE_TANGENT_NORMALMAP, KisGpuBlendOp::TangentNormalmap},
        {COMPOSITE_COMBINE_NORMAL, KisGpuBlendOp::CombineNormal},
        {COMPOSITE_LUMINOSITY_SAI, KisGpuBlendOp::LuminositySai},
        {COMPOSITE_BEHIND, KisGpuBlendOp::Behind},
        {COMPOSITE_GREATER, KisGpuBlendOp::Greater},
        {COMPOSITE_COPY_RED, KisGpuBlendOp::CopyRed},
        {COMPOSITE_COPY_GREEN, KisGpuBlendOp::CopyGreen},
        {COMPOSITE_COPY_BLUE, KisGpuBlendOp::CopyBlue},
    };
    auto it = ops.constFind(id);
    if (it == ops.constEnd()) {
        return false;
    }
    *op = it.value();
    return true;
}

bool KisGpuProjectionCompositor::composite(KisPaintDeviceSP projection,
                                           const QRect &rect,
                                           const QVector<Layer> &layers,
                                           QString *errorMessage,
                                           const KisGpuLayerCompositor::Mask *mask)
{
    auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    if (rect.isEmpty() || layers.isEmpty()) {
        return true;
    }
    if (!KisGpuTileAccess::isSupported(projection, errorMessage)) {
        return false;
    }

    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    const qint32 pixelSize = projection->pixelSize();
    // Replace layers lead the stack; coverage applies to one blended layer.
    int replaceLayers = 0;
    while (replaceLayers < layers.size() && layers[replaceLayers].replace)
        ++replaceLayers;
    for (int i = replaceLayers; i < layers.size(); i++) {
        if (layers[i].replace)
            return fail(QStringLiteral("replace layers must lead the stack"));
    }
    // copyAreaOptimized() yields the source's default pixel where the source
    // has no data only when both devices share it (otherwise it may clear to
    // the destination's default).
    const KoColor projectionDefault = projection->defaultPixel();
    for (int i = 0; i < replaceLayers; i++) {
        const KoColor layerDefault = layers[i].device->defaultPixel();
        if (layers[i].device->pixelSize() != pixelSize
            || std::memcmp(layerDefault.data(), projectionDefault.data(), size_t(pixelSize)))
            return fail(QStringLiteral("replace requires the projection's default pixel"));
    }
    const bool singleBlended = layers.size() - replaceLayers == 1;
    const Layer &blended = layers.last();
    const bool halfCoverage = pixelSize == 8 && singleBlended
        && (blended.halfBrush
            || (blended.op > KisGpuBlendOp::Over && blended.op != KisGpuBlendOp::Erase
                && kisGpuSupportsHalfBrushBlend(blended.op)));
    if (mask
        && ((pixelSize != 16 && !halfCoverage) || !mask->data || mask->bounds.isEmpty()
            || qint64(mask->bounds.width()) * mask->bounds.height() > 4096 * 4096 || !singleBlended)) {
        return fail(QStringLiteral("coverage requires one supported RGBA float layer"));
    }
    KisGpuContext &context = backend->context();

    // KisUpdaterContext keeps merge jobs that share a tile of the image grid
    // apart; that only protects projections whose tiles are on that grid.
    if (projection->x() % TileSize || projection->y() % TileSize) {
        return fail(QStringLiteral("projection offset is not a multiple of 64"));
    }

    for (const Layer &layer : layers) {
        if (layer.halfBrush
            && (pixelSize != 8 || (layer.op != KisGpuBlendOp::Over && layer.op != KisGpuBlendOp::Erase)))
            return fail(QStringLiteral("half brush arithmetic requires F16 Normal or Erase"));
        if (layer.channelMask > 0xf || quint32(layer.op) >= quint32(KisGpuBlendOp::Count)) {
            return fail(QStringLiteral("unsupported blend operation or channel mask"));
        }
        if (!(*layer.device->colorSpace() == *projection->colorSpace())) {
            return fail(QStringLiteral("layer color space differs from the projection"));
        }
        if ((layer.device->x() - projection->x()) % TileSize || (layer.device->y() - projection->y()) % TileSize) {
            return fail(QStringLiteral("layer tiles are not aligned with the projection tiles"));
        }
    }

    std::unique_ptr<WorkContext> work = [&] {
        KisPaintTrace::Scope trace("compositor.acquire_context",
                                   projection.data(),
                                   nullptr,
                                   KisPaintTrace::currentFlow());
        return acquireContext(context);
    }();
    // Waiting outside the pool mutex lets unrelated completed contexts remain
    // available to other workers. On failure, do not reset or overwrite tables.
    const bool contextReady = [&] {
        KisPaintTrace::Scope trace("compositor.wait_context", projection.data(), nullptr, KisPaintTrace::currentFlow());
        return work->commands.isValid() && work->commands.wait();
    }();
    if (!contextReady) {
        releaseContext(std::move(work));
        return fail(QStringLiteral("GPU work context is unavailable"));
    }
    work->lastUse = 0;
    std::unique_ptr<KisGpuLayerCompositor> &compositor = pixelSize == 8 ? work->compositor16 : work->compositor32;
    if (!compositor) {
        compositor =
            KisGpuLayerCompositor::create(context,
                                          pixelSize == 8 ? KisGpuTileFormat::RGBA16F : KisGpuTileFormat::RGBA32F,
                                          errorMessage);
    }
    if (!compositor || !work->commands.isValid()) {
        releaseContext(std::move(work));
        return false;
    }

    // The projection's tiles are composited in place on the GPU. Merge jobs
    // that share a projection tile never run concurrently while the GPU
    // projection is enabled (KisUpdaterContext::walkerIntersectsJob), and
    // the clip rect leaves the rest of each tile untouched.
    KisGpuCommandList &commands = work->commands;
    commands.begin();

    KisGpuTileAccess target(projection, rect, KisGpuTileAccess::ReadWrite);
    // Share upload allocations across this submission's layers. Allocate
    // lazily, with at most 16 MiB per chunk (larger individual uploads keep
    // their exact size). Resident layers require no upload storage.
    KisGpuTileAccess::UploadArena uploads(
        qMin<VkDeviceSize>(16 * 1024 * 1024,
                           VkDeviceSize(target.tileCount()) * pixelSize * TileSize * TileSize * (layers.size() + 2)));
    std::vector<std::unique_ptr<KisGpuTileAccess>> accesses;
    QVector<VkDeviceAddress> layerTiles;
    QVector<KisGpuLayerCompositor::Layer> layerParams;

    bool ok = [&] {
        KisPaintTrace::Scope trace("compositor.prepare_target",
                                   projection.data(),
                                   nullptr,
                                   KisPaintTrace::currentFlow());
        return target.prepare(commands, uploads, errorMessage);
    }();
    const QRect grid = target.tileGrid();
    const int tileCount = target.tileCount();
    const QPoint gridOrigin = target.tileOrigin(grid.left(), grid.top());
    layerTiles.reserve(tileCount * layers.size());

    for (int i = 0; ok && i < layers.size(); i++) {
        const Layer &layer = layers[i];
        const QRect extent = layer.device->extent();
        std::unique_ptr<KisGpuTileAccess> access(
            new KisGpuTileAccess(layer.device, rect & extent, KisGpuTileAccess::ReadOnly));
        if (!access->tileCount()) {
            // Nothing of this layer inside the rect. A replace layer still
            // clears the rect: all its tile addresses are 0.
            if (layer.replace) {
                layerTiles += QVector<VkDeviceAddress>(tileCount, 0);
                KisGpuLayerCompositor::Layer params;
                params.replace = true;
                defaultPixelAsFloat(layer.device, params.fill);
                layerParams << params;
            }
            continue;
        }
        const bool prepared = [&] {
            KisPaintTrace::Scope trace("compositor.prepare_layer",
                                       projection.data(),
                                       nullptr,
                                       KisPaintTrace::currentFlow());
            return access->prepare(commands, uploads, errorMessage);
        }();
        if (!prepared) {
            ok = false;
            accesses.push_back(std::move(access));
            break;
        }

        // Place the layer's tiles in the projection grid; tiles outside the
        // layer's extent stay 0 (skipped), like KisLayerProjectionPlane.
        const QVector<VkDeviceAddress> addresses = access->addresses();
        const QRect layerGrid = access->tileGrid();
        QVector<VkDeviceAddress> placed(tileCount, 0);
        for (int row = 0; row < layerGrid.height(); row++) {
            for (int col = 0; col < layerGrid.width(); col++) {
                const QPoint origin = access->tileOrigin(layerGrid.left() + col, layerGrid.top() + row) - gridOrigin;
                const int gridCol = origin.x() / TileSize;
                const int gridRow = origin.y() / TileSize;
                KIS_SAFE_ASSERT_RECOVER(gridCol >= 0 && gridRow >= 0 && gridCol < grid.width()
                                        && gridRow < grid.height())
                {
                    continue;
                }
                placed[gridRow * grid.width() + gridCol] = addresses[row * layerGrid.width() + col];
            }
        }
        layerTiles += placed;
        KisGpuLayerCompositor::Layer params{layer.op,
                                            layer.opacity,
                                            layer.alphaLocked,
                                            layer.channelMask,
                                            layer.halfBrush,
                                            layer.explicitChannelFlags,
                                            layer.replace};
        if (layer.replace)
            defaultPixelAsFloat(layer.device, params.fill);
        layerParams << params;
        accesses.push_back(std::move(access));
    }

    if (ok) {
        KisGpuLayerCompositor::Mask gridMask;
        // Without tiles of the blended layer, only replace layers remain.
        const bool useMask = mask && !layerParams.isEmpty() && !layerParams.last().replace;
        if (useMask)
            gridMask = {mask->data, mask->bounds.translated(-gridOrigin)};
        commands.computeBarrier();
        ok = compositor->record(commands,
                                layerTiles,
                                target.addresses(),
                                layerParams,
                                grid.width(),
                                rect.translated(-gridOrigin),
                                errorMessage,
                                useMask ? &gridMask : nullptr);
    }

    QVector<KisGpuTileAccess *> accessList;
    accessList << &target;
    for (auto &access : accesses) {
        accessList << access.get();
    }

    if (ok) {
        KisPaintTrace::Scope trace("compositor.submit_finish",
                                   projection.data(),
                                   nullptr,
                                   KisPaintTrace::currentFlow());
        work->lastUse = KisGpuTileAccess::submitAndFinish(commands, accessList);
        ok = work->lastUse != 0;
        if (ok) {
            KisPaintTrace::link("path.compositor.submitted",
                                nullptr,
                                KisPaintTrace::currentFlow(),
                                KisPaintTrace::currentJob());
        }
        if (!ok && errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("GPU submission refused or failed");
        }
    } else {
        // Nothing must run: the projection keeps its content and the merge
        // falls back to the CPU.
        KisGpuTileAccess::finishUnsubmitted(commands, accessList);
    }

    // No wait: the result stays on the GPU. CPU readers of the projection
    // download it (ordered after this submission). Prefer a completed context
    // on the next call; only wait when every available context is still busy.
    releaseContext(std::move(work));
    return ok;
}
