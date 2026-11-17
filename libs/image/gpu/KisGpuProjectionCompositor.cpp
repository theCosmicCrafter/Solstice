/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuProjectionCompositor.h"
#include "KisPaintTrace.h"

#include "KisGpuTileAccess.h"
#include "KisGpuTileBackend.h"

#include <KisGpuColorTransformPass.h>
#include <KisGpuCommandList.h>
#include <KisGpuContext.h>
#include <KisGpuCoveragePass.h>
#include <KisGpuShiftedCopyPass.h>
#include <KisGpuStrokeStylePass.h>
#include <KisGpuTilePool.h>

#include <KoColor.h>
#include <KoColorSpace.h>
#include <KoCompositeOpRegistry.h>

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <half.h>
#include <memory>
#include <vector>

#include <kis_debug.h>

#include "kis_gaussian_kernel.h"
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
    std::unique_ptr<KisGpuShiftedCopyPass> shiftedCopy32;
    std::unique_ptr<KisGpuShiftedCopyPass> shiftedCopy16;
    std::unique_ptr<KisGpuCoveragePass> coverage32;
    std::unique_ptr<KisGpuCoveragePass> coverage16;
    std::unique_ptr<KisGpuStrokeStylePass> strokeStyle32;
    std::unique_ptr<KisGpuStrokeStylePass> strokeStyle16;
    std::unique_ptr<KisGpuColorTransformPass> colorTransform32;
    std::unique_ptr<KisGpuColorTransformPass> colorTransform16;
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
std::atomic<quint64> s_shiftedCopyCount{0};
std::atomic<quint64> s_coverageCount{0};
std::atomic<quint64> s_strokeStyleCount{0};
std::atomic<quint64> s_colorTransformCount{0};
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
                params.alphaLocked = layer.alphaLocked;
                params.channelMask = layer.channelMask;
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
        // layer's extent stay 0 (skipped), like KisLayerProjectionPlane. A
        // layer moved by a distance that is not a multiple of 64 (phase 4.112)
        // has its own grid, one column and row larger: its tile at
        // 64 * (c - 1) + shift from the grid origin is tile c of that grid.
        const QPoint delta = layer.device->offset() - projection->offset();
        const QPoint shift(((delta.x() % TileSize) + TileSize) % TileSize,
                           ((delta.y() % TileSize) + TileSize) % TileSize);
        const bool shifted = !shift.isNull();
        const int tableWidth = grid.width() + (shifted ? 1 : 0);
        const int tableHeight = grid.height() + (shifted ? 1 : 0);
        const QVector<VkDeviceAddress> addresses = access->addresses();
        const QRect layerGrid = access->tileGrid();
        QVector<VkDeviceAddress> placed(tableWidth * tableHeight, 0);
        for (int row = 0; row < layerGrid.height(); row++) {
            for (int col = 0; col < layerGrid.width(); col++) {
                const QPoint origin = access->tileOrigin(layerGrid.left() + col, layerGrid.top() + row) - gridOrigin;
                const int tableCol = shifted ? (origin.x() - shift.x()) / TileSize + 1 : origin.x() / TileSize;
                const int tableRow = shifted ? (origin.y() - shift.y()) / TileSize + 1 : origin.y() / TileSize;
                KIS_SAFE_ASSERT_RECOVER(tableCol >= 0 && tableRow >= 0 && tableCol < tableWidth
                                        && tableRow < tableHeight)
                {
                    continue;
                }
                placed[tableRow * tableWidth + tableCol] = addresses[row * layerGrid.width() + col];
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
        params.shift = shift;
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

quint64 KisGpuProjectionCompositor::shiftedCopyCount()
{
    return s_shiftedCopyCount.load();
}

bool KisGpuProjectionCompositor::copyShifted(KisPaintDeviceSP dst,
                                             const QRect &dstRect,
                                             KisPaintDeviceSP src,
                                             const QPoint &offset,
                                             QString *errorMessage)
{
    auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    if (dstRect.isEmpty()) {
        return true;
    }
    if (!src || !dst || src == dst) {
        return fail(QStringLiteral("the source must be another device"));
    }
    if (!KisGpuTileAccess::isSupported(dst, errorMessage) || !KisGpuTileAccess::isSupported(src, errorMessage)) {
        return false;
    }
    if (!(*src->colorSpace() == *dst->colorSpace())) {
        return fail(QStringLiteral("source color space differs from the destination"));
    }
    const qint32 pixelSize = dst->pixelSize();
    if (std::memcmp(src->defaultPixel().data(), dst->defaultPixel().data(), size_t(pixelSize))) {
        return fail(QStringLiteral("shifted copy requires equal default pixels"));
    }
    // The same exclusivity rule as composite(): merge jobs never share a
    // tile of the image grid.
    if (dst->x() % TileSize || dst->y() % TileSize) {
        return fail(QStringLiteral("destination offset is not a multiple of 64"));
    }

    const QRect srcSample = src->extent() & dstRect.translated(-offset);
    if (srcSample.isEmpty() && (dst->extent() & dstRect).isEmpty()) {
        // copyAreaOptimized() does nothing here either: both are default.
        return true;
    }

    KisGpuContext &context = KisGpuTileBackend::instance()->context();
    std::unique_ptr<WorkContext> work = acquireContext(context);
    if (!work->commands.isValid() || !work->commands.wait()) {
        releaseContext(std::move(work));
        return fail(QStringLiteral("GPU work context is unavailable"));
    }
    work->lastUse = 0;
    std::unique_ptr<KisGpuShiftedCopyPass> &pass = pixelSize == 8 ? work->shiftedCopy16 : work->shiftedCopy32;
    if (!pass) {
        pass = KisGpuShiftedCopyPass::create(context,
                                             pixelSize == 8 ? KisGpuTileFormat::RGBA16F : KisGpuTileFormat::RGBA32F,
                                             errorMessage);
    }
    if (!pass) {
        releaseContext(std::move(work));
        return false;
    }

    KisGpuCommandList &commands = work->commands;
    commands.begin();

    KisGpuTileAccess target(dst, dstRect, KisGpuTileAccess::ReadWrite);
    KisGpuTileAccess::UploadArena uploads(
        qMin<VkDeviceSize>(16 * 1024 * 1024, VkDeviceSize(target.tileCount()) * pixelSize * TileSize * TileSize * 3));
    std::unique_ptr<KisGpuTileAccess> source;
    KisGpuShiftedCopyPass::Copy copy;

    bool ok = target.prepare(commands, uploads, errorMessage);
    if (ok) {
        const QRect grid = target.tileGrid();
        const QPoint dstOrigin = target.tileOrigin(grid.left(), grid.top());
        copy.dstTiles = target.addresses();
        copy.dstGridWidth = grid.width();
        copy.clip = dstRect.translated(-dstOrigin);
        defaultPixelAsFloat(src, copy.fill);

        // Without source tiles the grid is empty and every pixel is filled.
        QPoint srcOrigin = dstOrigin - offset;
        if (!srcSample.isEmpty()) {
            source.reset(new KisGpuTileAccess(src, srcSample, KisGpuTileAccess::ReadOnly));
            if (source->tileCount()) {
                ok = source->prepare(commands, uploads, errorMessage);
                const QRect srcGrid = source->tileGrid();
                srcOrigin = source->tileOrigin(srcGrid.left(), srcGrid.top());
                copy.srcTiles = source->addresses();
                copy.srcGridWidth = srcGrid.width();
                copy.srcGridHeight = srcGrid.height();
            }
        }
        // Source grid pixel of destination grid pixel q:
        // (dstOrigin + q - offset) - srcOrigin.
        copy.shift = dstOrigin - offset - srcOrigin;
    }
    if (ok) {
        commands.computeBarrier();
        ok = pass->record(commands, copy, errorMessage);
    }

    QVector<KisGpuTileAccess *> accessList;
    accessList << &target;
    if (source) {
        accessList << source.get();
    }
    if (ok) {
        work->lastUse = KisGpuTileAccess::submitAndFinish(commands, accessList);
        ok = work->lastUse != 0;
        if (!ok && errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("GPU submission refused or failed");
        }
    } else {
        KisGpuTileAccess::finishUnsubmitted(commands, accessList);
    }
    releaseContext(std::move(work));
    if (ok) {
        ++s_shiftedCopyCount;
    }
    return ok;
}

bool KisGpuProjectionCompositor::applyCoverage(KisPaintDeviceSP device,
                                               const QRect &rect,
                                               const quint8 *coverage,
                                               QString *errorMessage)
{
    auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    if (rect.isEmpty()) {
        return true;
    }
    if (!device || !coverage) {
        return fail(QStringLiteral("no device or coverage"));
    }
    if (!KisGpuTileAccess::isSupported(device, errorMessage)) {
        return false;
    }
    if (qint64(rect.width()) * rect.height() > KisGpuCoveragePass::MaxMaskBytes) {
        return fail(QStringLiteral("coverage rect is too large"));
    }
    if (device->defaultPixel().opacityU8() != OPACITY_TRANSPARENT_U8) {
        return fail(QStringLiteral("coverage requires a transparent default pixel"));
    }
    // The same exclusivity rule as composite(): merge jobs never share a
    // tile of the image grid.
    if (device->x() % TileSize || device->y() % TileSize) {
        return fail(QStringLiteral("device offset is not a multiple of 64"));
    }

    // Pixels outside the extent are transparent and stay so.
    const QRect touched = rect & device->extent();
    if (touched.isEmpty()) {
        return true;
    }

    KisGpuContext &context = KisGpuTileBackend::instance()->context();
    std::unique_ptr<WorkContext> work = acquireContext(context);
    if (!work->commands.isValid() || !work->commands.wait()) {
        releaseContext(std::move(work));
        return fail(QStringLiteral("GPU work context is unavailable"));
    }
    work->lastUse = 0;
    const qint32 pixelSize = device->pixelSize();
    std::unique_ptr<KisGpuCoveragePass> &pass = pixelSize == 8 ? work->coverage16 : work->coverage32;
    if (!pass) {
        pass = KisGpuCoveragePass::create(context,
                                          pixelSize == 8 ? KisGpuTileFormat::RGBA16F : KisGpuTileFormat::RGBA32F,
                                          errorMessage);
    }
    if (!pass) {
        releaseContext(std::move(work));
        return false;
    }

    KisGpuCommandList &commands = work->commands;
    commands.begin();

    KisGpuTileAccess target(device, touched, KisGpuTileAccess::ReadWrite);
    KisGpuTileAccess::UploadArena uploads(
        qMin<VkDeviceSize>(16 * 1024 * 1024, VkDeviceSize(target.tileCount()) * pixelSize * TileSize * TileSize * 2));
    bool ok = target.prepare(commands, uploads, errorMessage);
    if (ok) {
        const QRect grid = target.tileGrid();
        const QPoint origin = target.tileOrigin(grid.left(), grid.top());
        KisGpuCoveragePass::Pass coveragePass;
        coveragePass.tiles = target.addresses();
        coveragePass.gridWidth = grid.width();
        coveragePass.clip = touched.translated(-origin);
        coveragePass.mask = coverage;
        coveragePass.maskBounds = rect.translated(-origin);
        commands.computeBarrier();
        ok = pass->record(commands, coveragePass, errorMessage);
    }

    QVector<KisGpuTileAccess *> accessList;
    accessList << &target;
    if (ok) {
        work->lastUse = KisGpuTileAccess::submitAndFinish(commands, accessList);
        ok = work->lastUse != 0;
        if (!ok && errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("GPU submission refused or failed");
        }
    } else {
        KisGpuTileAccess::finishUnsubmitted(commands, accessList);
    }
    releaseContext(std::move(work));
    if (ok) {
        ++s_coverageCount;
    }
    return ok;
}

quint64 KisGpuProjectionCompositor::coverageCount()
{
    return s_coverageCount.load();
}

bool KisGpuProjectionCompositor::supportsStrokeStyle()
{
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    return backend && !backend->hasFailed() && backend->context().deviceInfo().supportsFloat64;
}

bool KisGpuProjectionCompositor::strokeStyleSupportsBlendOp(KisGpuBlendOp op)
{
    return KisGpuStrokeStylePass::supportsBlendOp(op);
}

int KisGpuProjectionCompositor::maxStrokeStyleSize()
{
    return KisGpuStrokeStylePass::MaxRadius;
}

bool KisGpuProjectionCompositor::compositeStrokeStyle(KisPaintDeviceSP projection,
                                                      const QRect &rect,
                                                      const StrokeStyle &style,
                                                      QString *errorMessage)
{
    auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    if (rect.isEmpty()) {
        return true;
    }
    if (!projection || !style.layer || style.layer == projection) {
        return fail(QStringLiteral("the layer must be another device"));
    }
    if (!supportsStrokeStyle()) {
        return fail(QStringLiteral("the stroke style needs 64-bit float shaders"));
    }
    if (!KisGpuTileAccess::isSupported(projection, errorMessage)
        || !KisGpuTileAccess::isSupported(style.layer, errorMessage)) {
        return false;
    }
    if (!(*style.layer->colorSpace() == *projection->colorSpace())
        || !(*style.strokeColor.colorSpace() == *projection->colorSpace())) {
        return fail(QStringLiteral("layer or stroke color space differs from the projection"));
    }
    if (style.layer->defaultPixel().opacityU8() != OPACITY_TRANSPARENT_U8) {
        return fail(QStringLiteral("the stroke style requires a transparent layer default pixel"));
    }
    const int radius = int(std::ceil(style.strokeSize));
    if (!(style.strokeSize > 0.0) || radius < 1 || radius > KisGpuStrokeStylePass::MaxRadius) {
        return fail(QStringLiteral("unsupported stroke size"));
    }
    if (!KisGpuStrokeStylePass::supportsBlendOp(style.layerOp)
        || !KisGpuStrokeStylePass::supportsBlendOp(style.strokeOp) || style.layerChannelMask > 0xf
        || style.strokeChannelMask > 0xf) {
        return fail(QStringLiteral("unsupported blend mode or channel mask"));
    }
    // The same exclusivity rule as composite(): merge jobs never share a
    // tile of the image grid.
    if (projection->x() % TileSize || projection->y() % TileSize) {
        return fail(QStringLiteral("projection offset is not a multiple of 64"));
    }

    // Without layer pixels within the stroke's reach there is neither a
    // layer nor a stroke to composite.
    const QRect layerRect = rect.adjusted(-radius, -radius, radius, radius) & style.layer->extent();
    if (layerRect.isEmpty()) {
        return true;
    }

    KisGpuContext &context = KisGpuTileBackend::instance()->context();
    std::unique_ptr<WorkContext> work = acquireContext(context);
    if (!work->commands.isValid() || !work->commands.wait()) {
        releaseContext(std::move(work));
        return fail(QStringLiteral("GPU work context is unavailable"));
    }
    work->lastUse = 0;
    const qint32 pixelSize = projection->pixelSize();
    std::unique_ptr<KisGpuStrokeStylePass> &pass = pixelSize == 8 ? work->strokeStyle16 : work->strokeStyle32;
    if (!pass) {
        pass = KisGpuStrokeStylePass::create(context,
                                             pixelSize == 8 ? KisGpuTileFormat::RGBA16F : KisGpuTileFormat::RGBA32F,
                                             errorMessage);
    }
    if (!pass) {
        releaseContext(std::move(work));
        return false;
    }

    KisGpuStrokeStylePass::Pass strokePass;
    strokePass.layerOp = style.layerOp;
    strokePass.layerOpacity = style.layerOpacity;
    strokePass.layerAlphaLocked = style.layerAlphaLocked;
    strokePass.layerChannelMask = style.layerChannelMask;
    strokePass.strokeOp = style.strokeOp;
    strokePass.strokeOpacity = style.strokeOpacity;
    strokePass.strokeAlphaLocked = style.strokeAlphaLocked;
    strokePass.strokeChannelMask = style.strokeChannelMask;
    if (pixelSize == 8) {
        const half *channels = reinterpret_cast<const half *>(style.strokeColor.data());
        for (int i = 0; i < 4; ++i)
            strokePass.strokeColor[i] = float(channels[i]);
    } else {
        std::memcpy(strokePass.strokeColor, style.strokeColor.data(), 4 * sizeof(float));
    }
    // The CPU's kernel (KisGaussianKernel::applyDilate()), row-major.
    strokePass.radius = radius;
    const Eigen::Matrix<qreal, Eigen::Dynamic, Eigen::Dynamic> matrix =
        KisGaussianKernel::createDilateMatrix(style.strokeSize);
    const int kernelSize = 2 * radius + 1;
    KIS_SAFE_ASSERT_RECOVER(matrix.rows() == kernelSize && matrix.cols() == kernelSize)
    {
        releaseContext(std::move(work));
        return fail(QStringLiteral("unexpected dilate kernel size"));
    }
    strokePass.kernel.resize(kernelSize * kernelSize);
    for (int y = 0; y < kernelSize; ++y) {
        for (int x = 0; x < kernelSize; ++x) {
            strokePass.kernel[y * kernelSize + x] = matrix(x, y);
        }
    }

    KisGpuCommandList &commands = work->commands;
    commands.begin();

    KisGpuTileAccess target(projection, rect, KisGpuTileAccess::ReadWrite);
    KisGpuTileAccess source(style.layer, layerRect, KisGpuTileAccess::ReadOnly);
    KisGpuTileAccess::UploadArena uploads(
        qMin<VkDeviceSize>(16 * 1024 * 1024,
                           VkDeviceSize(target.tileCount() + source.tileCount()) * pixelSize * TileSize * TileSize));
    bool ok = target.prepare(commands, uploads, errorMessage);
    if (ok) {
        ok = source.prepare(commands, uploads, errorMessage);
    }
    if (ok) {
        const QRect grid = target.tileGrid();
        const QPoint gridOrigin = target.tileOrigin(grid.left(), grid.top());
        const QRect layerGrid = source.tileGrid();
        const QPoint layerOrigin = source.tileOrigin(layerGrid.left(), layerGrid.top());
        strokePass.dstTiles = target.addresses();
        strokePass.gridWidth = grid.width();
        strokePass.clip = rect.translated(-gridOrigin);
        strokePass.layerTiles = source.addresses();
        strokePass.layerTableSize = layerGrid.size();
        strokePass.layerBias = gridOrigin - layerOrigin;
        commands.computeBarrier();
        ok = pass->record(commands, strokePass, errorMessage);
    }

    QVector<KisGpuTileAccess *> accessList;
    accessList << &target << &source;
    if (ok) {
        work->lastUse = KisGpuTileAccess::submitAndFinish(commands, accessList);
        ok = work->lastUse != 0;
        if (!ok && errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("GPU submission refused or failed");
        }
    } else {
        KisGpuTileAccess::finishUnsubmitted(commands, accessList);
    }
    releaseContext(std::move(work));
    if (ok) {
        ++s_strokeStyleCount;
    }
    return ok;
}

quint64 KisGpuProjectionCompositor::strokeStyleCount()
{
    return s_strokeStyleCount.load();
}

bool KisGpuProjectionCompositor::supportsColorTransform()
{
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    return backend && !backend->hasFailed() && backend->context().deviceInfo().supportsFloat64;
}

qint64 KisGpuProjectionCompositor::maxColorTransformCoveragePixels()
{
    return KisGpuColorTransformPass::MaxMaskBytes;
}

quint64 KisGpuProjectionCompositor::colorTransformCount()
{
    return s_colorTransformCount.load();
}

bool KisGpuProjectionCompositor::transformColors(KisPaintDeviceSP dst,
                                                 const QRect &rect,
                                                 KisPaintDeviceSP src,
                                                 const KisGpuColorTransform &transform,
                                                 const quint8 *coverage,
                                                 QString *errorMessage)
{
    auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    if (rect.isEmpty()) {
        return true;
    }
    if (!src || !dst) {
        return fail(QStringLiteral("no source or destination"));
    }
    // In place (a filter mask on its layer's projection): every pixel is
    // read and written by the same shader invocation.
    const bool inPlace = src == dst;
    if (!KisGpuTileAccess::isSupported(dst, errorMessage) || !KisGpuTileAccess::isSupported(src, errorMessage)) {
        return false;
    }
    if (!supportsColorTransform()) {
        return fail(QStringLiteral("the GPU has no 64-bit float shaders"));
    }
    if (!(*src->colorSpace() == *dst->colorSpace())) {
        return fail(QStringLiteral("source color space differs from the destination"));
    }
    const qint32 pixelSize = dst->pixelSize();
    if (transform.kind() != KisGpuColorTransform::Desaturate && pixelSize != 8) {
        return fail(QStringLiteral("color transform tables need RGBA F16"));
    }
    if (coverage && qint64(rect.width()) * rect.height() > KisGpuColorTransformPass::MaxMaskBytes) {
        return fail(QStringLiteral("coverage rect is too large"));
    }
    // The same exclusivity rule as composite(): merge jobs never share a
    // tile of the image grid.
    if (dst->x() % TileSize || dst->y() % TileSize) {
        return fail(QStringLiteral("destination offset is not a multiple of 64"));
    }

    KisGpuContext &context = KisGpuTileBackend::instance()->context();
    std::unique_ptr<WorkContext> work = acquireContext(context);
    if (!work->commands.isValid() || !work->commands.wait()) {
        releaseContext(std::move(work));
        return fail(QStringLiteral("GPU work context is unavailable"));
    }
    work->lastUse = 0;
    std::unique_ptr<KisGpuColorTransformPass> &pass = pixelSize == 8 ? work->colorTransform16 : work->colorTransform32;
    if (!pass) {
        pass = KisGpuColorTransformPass::create(context,
                                                pixelSize == 8 ? KisGpuTileFormat::RGBA16F : KisGpuTileFormat::RGBA32F,
                                                errorMessage);
    }
    if (!pass) {
        releaseContext(std::move(work));
        return false;
    }

    KisGpuCommandList &commands = work->commands;
    commands.begin();

    const QRect srcSample = src->extent() & rect;
    KisGpuTileAccess target(dst, rect, KisGpuTileAccess::ReadWrite);
    KisGpuTileAccess::UploadArena uploads(
        qMin<VkDeviceSize>(16 * 1024 * 1024, VkDeviceSize(target.tileCount()) * pixelSize * TileSize * TileSize * 3));
    std::unique_ptr<KisGpuTileAccess> source;
    KisGpuColorTransformPass::Pass transformPass;

    bool ok = target.prepare(commands, uploads, errorMessage);
    if (ok) {
        const QRect grid = target.tileGrid();
        const QPoint dstOrigin = target.tileOrigin(grid.left(), grid.top());
        transformPass.dstTiles = target.addresses();
        transformPass.dstGridWidth = grid.width();
        transformPass.clip = rect.translated(-dstOrigin);
        defaultPixelAsFloat(src, transformPass.fill);

        // Without source tiles the grid is empty and every pixel reads the fill.
        QPoint srcOrigin = dstOrigin;
        if (inPlace) {
            const QRect grid = target.tileGrid();
            transformPass.srcTiles = transformPass.dstTiles;
            transformPass.srcGridWidth = grid.width();
            transformPass.srcGridHeight = grid.height();
        } else if (!srcSample.isEmpty()) {
            source.reset(new KisGpuTileAccess(src, srcSample, KisGpuTileAccess::ReadOnly));
            if (source->tileCount()) {
                ok = source->prepare(commands, uploads, errorMessage);
                const QRect srcGrid = source->tileGrid();
                srcOrigin = source->tileOrigin(srcGrid.left(), srcGrid.top());
                transformPass.srcTiles = source->addresses();
                transformPass.srcGridWidth = srcGrid.width();
                transformPass.srcGridHeight = srcGrid.height();
            }
        }
        transformPass.shift = dstOrigin - srcOrigin;

        if (transform.kind() != KisGpuColorTransform::Desaturate) {
            transformPass.kind = transform.kind() == KisGpuColorTransform::ChannelTables
                ? KisGpuColorTransformPass::ChannelTables
                : KisGpuColorTransformPass::ColorReference;
            transformPass.tables = transform.tableData();
            transformPass.tableBytes = transform.tableBytes();
            transformPass.tablesId = transform.tablesId();
        } else {
            transformPass.kind = KisGpuColorTransformPass::Desaturate + quint32(transform.desaturateType());
        }
        if (coverage) {
            transformPass.mask = coverage;
            transformPass.maskBounds = rect.translated(-dstOrigin);
        }
    }
    if (ok) {
        commands.computeBarrier();
        ok = pass->record(commands, transformPass, errorMessage);
    }

    QVector<KisGpuTileAccess *> accessList;
    accessList << &target;
    if (source) {
        accessList << source.get();
    }
    if (ok) {
        work->lastUse = KisGpuTileAccess::submitAndFinish(commands, accessList);
        ok = work->lastUse != 0;
        if (!ok && errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("GPU submission refused or failed");
        }
    } else {
        KisGpuTileAccess::finishUnsubmitted(commands, accessList);
    }
    releaseContext(std::move(work));
    if (ok) {
        ++s_colorTransformCount;
    }
    return ok;
}
