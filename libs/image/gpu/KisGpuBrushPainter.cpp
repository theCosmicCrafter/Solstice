/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisGpuBrushPainter.h"
#include "KisGpuEngineSettings.h"
#include "KisGpuMergeBatch.h"
#include "KisPaintTrace.h"
#include "KisRenderedDab.h"
#include "kis_paint_device.h"
#include "kis_painter.h"
#include "kis_pixel_selection.h"
#include "kis_selection.h"
#include <KoColorModelStandardIds.h>
#include <KoCompositeOpRegistry.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <kis_debug.h>
#include <vector>
#ifdef HAVE_KRITA_GPU_ENGINE
#include "KisGpuProjectionCompositor.h"
#include "KisGpuTileAccess.h"
#include "KisGpuTileBackend.h"
#include <KisGpuCommandList.h>
#include <KisGpuContext.h>
#include <KisGpuDabCompositor.h>
#include <QMutexLocker>
#include <QRegion>
#include <compositeops/KoAlphaDarkenParamsWrapper.h>
namespace
{
std::atomic<quint64> s_batches{0};
std::atomic<quint64> s_generatedDabs{0};
std::atomic<int> s_refusePendingBatches{0};
std::atomic<quint64> s_washPreviews{0};
std::atomic<quint64> s_washBaseCopies{0}; // phase 4.90: base copy in the Wash preview submission
std::atomic<quint64> s_washMerges{0};
QMutex s_mutex;
struct Work {
    KisGpuCommandList commands;
    std::unique_ptr<KisGpuDabCompositor> compositor;
    quint64 lastUse = 0;
    explicit Work(KisGpuContext &context)
        : commands(context)
        , compositor(KisGpuDabCompositor::create(context))
    {
    }
    bool wait()
    {
        if (!commands.wait())
            return false;
        lastUse = 0;
        return true;
    }
};
struct WorkPool {
    std::array<std::unique_ptr<Work>, 3> slots;
    size_t next = 0;
    quint64 bytes() const
    {
        quint64 result = 0;
        for (const auto &slot : slots)
            if (slot && slot->compositor)
                result += slot->compositor->uploadBytes();
        return result;
    }
    Work *acquire(KisGpuContext &context, quint64 required)
    {
        if (!required || required > KisGpuDabCompositor::MaxUploadBytes)
            return nullptr;
        const quint64 currentCapacity =
            slots[next] && slots[next]->compositor ? slots[next]->compositor->uploadBytes() : 0;
        if (currentCapacity < required && bytes() - currentCapacity + required > KisGpuDabCompositor::MaxUploadBytes) {
            // Large textured batches may fit only two buffers in the budget.
            // Reuse the oldest sufficiently large buffer instead of continually
            // evicting it to allocate the third round-robin slot from scratch.
            Work *oldest = nullptr;
            for (size_t i = 0; i < slots.size(); ++i) {
                auto *candidate = slots[i].get();
                if (candidate && candidate->compositor && candidate->compositor->uploadBytes() >= required
                    && (!oldest || candidate->lastUse < oldest->lastUse)) {
                    oldest = candidate;
                    next = i;
                }
            }
        }
        auto &slot = slots[next];
        if (!slot)
            slot = std::make_unique<Work>(context);
        if (!slot->compositor || !slot->commands.isValid() || !slot->wait())
            return nullptr;
        // The oldest slot is safe to reuse now. Retain its buffer if it fits;
        // otherwise free it before allocating, including transient peak usage.
        if (slot->compositor->uploadBytes() < required)
            slot->compositor->releaseUpload();
        const quint64 growth = slot->compositor->uploadBytes() ? 0 : required;
        const auto fits = [&]() {
            return bytes() + growth <= KisGpuDabCompositor::MaxUploadBytes;
        };
        const quint64 completed = context.completedValue();
        // Prefer completed buffers; wait for older work only under pressure.
        for (int pass = 0; pass < 2 && !fits(); ++pass) {
            for (size_t offset = 1; offset < slots.size() && !fits(); ++offset) {
                auto &other = slots[(next + offset) % slots.size()];
                if (!other || !other->compositor || !other->compositor->uploadBytes()
                    || (!pass && other->lastUse > completed))
                    continue;
                if (!other->wait())
                    return nullptr;
                other->compositor->releaseUpload();
            }
        }
        auto *result = slot.get();
        next = (next + 1) % slots.size();
        return result;
    }
};
// Process lifetime avoids loader teardown ordering, as the tile backend does.
WorkPool &workPool()
{
    static WorkPool *pool = new WorkPool;
    return *pool;
}
} // namespace
#endif
bool KisGpuBrushPainter::isEnabled()
{
    // The setting (Solstice/GpuBrush, on by default) is read once per
    // process; KRITA_GPU_BRUSH (1 or 0) overrides it and is read every time,
    // so that tests can switch it.
    static const bool inConfig = KisGpuEngineSettings::brushEnabledInConfig();
    const bool requested =
        qEnvironmentVariableIsSet("KRITA_GPU_BRUSH") ? qEnvironmentVariableIntValue("KRITA_GPU_BRUSH") == 1 : inConfig;
    return requested && KisGpuMergeBatch::isEnabled();
}
bool KisGpuBrushPainter::supports(KisPainter *painter)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    if (!isEnabled() || !painter || !painter->device())
        return false;
    if (qEnvironmentVariableIntValue("KRITA_GPU_BRUSH_DEBUG") == 1) {
        static std::atomic<int> messages{0};
        if (messages.fetch_add(1) < 20) {
            qInfo() << "GPU brush candidate: mode" << painter->compositeOpId() << "space"
                    << painter->device()->colorSpace()->id() << "selection" << bool(painter->selection()) << "mirroring"
                    << painter->hasMirroring() << "lod" << painter->device()->defaultBounds()->currentLevelOfDetail();
        }
    }
    KisGpuBlendOp blendOp;
    if (painter->compositeOpId() != COMPOSITE_ALPHA_DARKEN && painter->compositeOpId() != COMPOSITE_ERASE
        && !KisGpuProjectionCompositor::blendOpForCompositeOp(painter->compositeOpId(), &blendOp))
        return false;
    const auto device = painter->device();
    const auto *space = device->colorSpace();
    const bool half = space->colorDepthId() == Float16BitsColorDepthID;
    if (half && painter->compositeOpId() != COMPOSITE_ERASE && painter->compositeOpId() != COMPOSITE_ALPHA_DARKEN
        && !kisGpuSupportsHalfBrushBlend(blendOp))
        return false;
    if (space->colorModelId() != RGBAColorModelID || (!half && space->colorDepthId() != Float32BitsColorDepthID)
        || device->defaultBounds()->wrapAroundMode() || device->defaultBounds()->currentLevelOfDetail())
        return false;
    const QBitArray flags = painter->channelFlags();
    if (!flags.isEmpty()) {
        if (flags.size() != 4)
            return false;
        if (painter->compositeOpId() == COMPOSITE_ALPHA_DARKEN && flags.count(true) != 4)
            return false;
    }
    return KisGpuTileAccess::isSupported(device);
#else
    Q_UNUSED(painter);
    return false;
#endif
}
bool KisGpuBrushPainter::paint(KisPainter *painter, const QList<KisRenderedDab> &dabs, const QVector<QRect> *paintRects)
{
    return paintImpl(painter, dabs, paintRects, false);
}
bool KisGpuBrushPainter::paintMirrored(KisPainter *painter,
                                       const QList<KisRenderedDab> &dabs,
                                       const QVector<QRect> &paintRects)
{
    if (!painter || !painter->hasMirroring())
        return false;
    return paintImpl(painter, dabs, &paintRects, true);
}
namespace
{
/// KoColorSpaceMaths<quint8, half>::scaleToA() as floats: the alpha of
/// RGBA16F alpha-mask image tips ("Image tip dabs")
const float *uint8ToHalfTable()
{
    static const std::array<float, 256> table = []() {
        std::array<float, 256> values{};
        for (int i = 0; i < 256; ++i)
            values[size_t(i)] = float(KoColorSpaceMaths<quint8, half>::scaleToA(quint8(i)));
        return values;
    }();
    return table.data();
}

/// KoColorSpaceMaths<quint8, float>::scaleToA() (KoLuts::Uint8ToFloat), the
/// texture byte values KisMaskingBrushCompositeOp composites ("Textured dabs").
const float *uint8ToFloatTable()
{
    static const std::array<float, 256> table = []() {
        std::array<float, 256> values{};
        for (int i = 0; i < 256; ++i)
            values[size_t(i)] = KoColorSpaceMaths<quint8, float>::scaleToA(quint8(i));
        return values;
    }();
    return table.data();
}
} // namespace

bool KisGpuBrushPainter::paintImpl(KisPainter *painter,
                                   const QList<KisRenderedDab> &dabs,
                                   const QVector<QRect> *paintRects,
                                   bool combineMirrors)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    if (!supports(painter) || dabs.isEmpty())
        return false;
    if (s_refusePendingBatches.load() > 0 && KisRenderedDab::hasPendingPixels(dabs)
        && s_refusePendingBatches.fetch_sub(1) > 0)
        return false;
    QRect bounds;
    using Mode = KisGpuDabCompositor::CompositeMode;
    const bool alphaDarken = painter->compositeOpId() == COMPOSITE_ALPHA_DARKEN;
    quint32 channelMask = 0xf;
    const auto channelFlags = painter->channelFlags();
    const int pixelSize = painter->device()->pixelSize();
    if (!channelFlags.isEmpty()) {
        channelMask = 0;
        for (int i = 0; i < 4; ++i)
            if (channelFlags.testBit(i))
                channelMask |= 1u << i;
        if (pixelSize == 8)
            channelMask |= 16; // F16 scalar Over distinguishes explicit flags
    }
    Mode mode = painter->compositeOpId() == COMPOSITE_ERASE ? Mode::Erase : Mode::Normal;
    KisGpuBlendOp blendOp;
    if (KisGpuProjectionCompositor::blendOpForCompositeOp(painter->compositeOpId(), &blendOp)
        && blendOp != KisGpuBlendOp::Over) {
        // Both enums retain the shared shader's order for generic blend modes.
        static_assert(quint32(Mode::Multiply) == quint32(KisGpuBlendOp::Multiply) + 3);
        static_assert(quint32(Mode::Exclusion) == quint32(KisGpuBlendOp::Exclusion) + 3);
        // The layer enum reserves 11 for Erase; the dab enum places it at 3.
        static_assert(quint32(Mode::LinearBurn) == quint32(KisGpuBlendOp::LinearBurn) + 2);
        static_assert(quint32(Mode::LighterColor) == quint32(KisGpuBlendOp::LighterColor) + 2);
        static_assert(quint32(Mode::Count) == quint32(KisGpuBlendOp::Count) + 2);
        mode = Mode(quint32(blendOp) + (blendOp < KisGpuBlendOp::Erase ? 3 : 2));
    }
    if (alphaDarken) {
        // The CPU op fixes this process-wide setting when the color space is created.
        static const Mode alphaMode = useCreamyAlphaDarken() ? Mode::AlphaDarkenCreamy : Mode::AlphaDarkenHard;
        mode = alphaMode;
    }
    QVector<KisGpuDabCompositor::Dab> inputs;
    quint64 generatedDabs = 0;
    const auto selection = painter->selection();
    const auto appendPass = [&](const QList<KisRenderedDab> &passDabs, const QVector<QRect> &rects, quint32 flips) {
        QRegion covered;
        QRect passBounds;
        if (paintRects) {
            for (const QRect &rect : rects) {
                // The brush splitter supplies disjoint rectangles. Overlapping
                // rectangles would paint twice on CPU, so keep that fallback.
                if (covered.intersects(rect))
                    return false;
                covered += rect;
            }
        }
        for (const auto &dab : passDabs) {
            if (!dab.device || dab.device->bounds().isEmpty() || !std::isfinite(dab.opacity) || dab.opacity < 0
                || dab.opacity > 1 || *dab.device->colorSpace() != *painter->device()->colorSpace())
                return false;
            if (alphaDarken
                && (!std::isfinite(dab.flow) || dab.flow < 0 || dab.flow > 1 || !std::isfinite(dab.averageOpacity)
                    || dab.averageOpacity < 0 || dab.averageOpacity > 1))
                return false;
            const QRect effective = selection ? dab.realBounds() & selection->selectedRect() : dab.realBounds();
            const QRegion clipped = paintRects ? QRegion(effective) & covered : QRegion(effective);
            // Preserve the CPU's fuzzy-equal opacity/average normalization.
            KoCompositeOp::ParameterInfo parameters;
            parameters.setOpacityAndAverage(float(dab.opacity), float(dab.averageOpacity));
            // Disjoint fragments of a dab never touch the same destination
            // pixel. Preserve dab/pass order, and upload their shared source once.
            // Phases 4.83/4.84: an RGBA32F default or Gaussian circle dab is evaluated on the GPU
            // from its description instead of uploading its pixels. The pixel
            // reflections since generation combine with this pass's reflection.
            // RGBA32F and (phase 4.86) RGBA16F descriptions; the dab color
            // space already equals the device's.
            // Textured descriptions ("Textured dabs") are RGBA32F only.
            // Image tip descriptions ("Image tip dabs") are RGBA32F only.
            const bool generated = dab.procedural && pixelSize == (dab.procedural->halfPixels ? 8 : 16)
                && (!dab.procedural->texture || dab.procedural->texture->pattern)
                && (dab.procedural->kind != KisProceduralCircleDab::ImageTip
                    || (dab.procedural->imageTip
                        && dab.procedural->imageTip->source.format() == QImage::Format_ARGB32));
            KisGpuDabCompositor::Circle circle;
            if (generated) {
                const KisProceduralCircleDab &source = *dab.procedural;
                circle.centerX = source.centerX;
                circle.centerY = source.centerY;
                circle.cosa = source.cosa;
                circle.sina = source.sina;
                circle.xcoef = source.xcoef;
                circle.ycoef = source.ycoef;
                circle.fadeX = source.fadeX;
                circle.fadeY = source.fadeY;
                circle.distfactor = source.distfactor;
                circle.center = source.center;
                circle.alphafactor = source.alphafactor;
                circle.radius = source.radius;
                circle.fadeStart = source.fadeStart;
                circle.fadeStartValue = source.fadeStartValue;
                circle.fadeCoeff = source.fadeCoeff;
                circle.kind = quint32(source.kind);
                circle.halfPixels = source.halfPixels ? 1 : 0;
                circle.curveResolution = source.curveResolution;
                std::copy(source.color, source.color + 4, circle.color);
                circle.antialias = source.antialias ? 1 : 0;
                if (const KisProceduralDabTexture *texture = source.texture.data()) {
                    circle.textureOriginX = texture->originX;
                    circle.textureOriginY = texture->originY;
                    circle.textureWidth = quint32(texture->width);
                    circle.textureHeight = quint32(texture->height);
                    circle.textureMode = texture->mode;
                    circle.textureSoft = texture->soft ? 1 : 0;
                    circle.textureStrength = texture->strength;
                    circle.textureInvertedStrength = texture->invertedStrength;
                    circle.textureWeight = texture->weight;
                    circle.textureBrushFlips = texture->brushFlips & 3;
                }
                if (const KisImageTipGeometry *tip = source.imageTip.data()) {
                    circle.imageWidth = tip->source.width();
                    circle.imageHeight = tip->source.height();
                    circle.imageFdx = tip->fdx;
                    circle.imageFdy = tip->fdy;
                    circle.imageIdentity = tip->identity ? 1 : 0;
                    circle.imageApplication = source.imageApplication;
                }
            }
            for (const QRect &clip : clipped) {
                if (inputs.size() >= 65536)
                    return false; // bound host metadata before preparing any tiles
                passBounds |= clip;
                KisGpuDabCompositor::Dab input;
                input.pixels = dab.device->constData();
                input.origin = dab.offset;
                input.size = dab.device->bounds().size();
                input.opacity = float(dab.opacity);
                input.flow = float(dab.flow);
                input.averageOpacity = *parameters.lastOpacity;
                input.mirrorFlags = generated ? flips ^ (dab.proceduralFlips & 3) : flips;
                input.clip = clip;
                input.generated = generated;
                input.circle = circle;
                if (generated && dab.procedural->kind == KisProceduralCircleDab::SoftCircle) {
                    if (!dab.procedural->curveTable)
                        return false;
                    input.curveTable = dab.procedural->curveTable->constData();
                    input.curveTableSize = dab.procedural->curveTable->size();
                }
                if (generated && dab.procedural->texture) {
                    input.texture = dab.procedural->texture->pattern->constData();
                    // RGBA16F: KoColorSpaceMaths<quint8, half>
                    input.textureLut = dab.procedural->halfPixels ? uint8ToHalfTable() : uint8ToFloatTable();
                }
                if (generated && dab.procedural->kind == KisProceduralCircleDab::ImageTip) {
                    const KisImageTipGeometry &tip = *dab.procedural->imageTip;
                    if (!tip.identity && tip.rows.size() != input.size.height())
                        return false;
                    input.imageTexels = reinterpret_cast<const quint32 *>(tip.source.constBits());
                    input.imageRows = tip.identity ? nullptr : reinterpret_cast<const qint32 *>(tip.rows.constData());
                    input.textureLut = dab.procedural->halfPixels ? uint8ToHalfTable() : uint8ToFloatTable();
                    if (dab.procedural->imageTable) {
                        input.imageTable = dab.procedural->imageTable->constData();
                        input.imageTableSize = dab.procedural->imageTable->size();
                    }
                }
                inputs << input;
            }
            generatedDabs += generated ? 1 : 0;
        }
        bounds |= passBounds;
        return true;
    };
    QVector<QRect> passRects = paintRects ? *paintRects : QVector<QRect>{};
    if (!appendPass(dabs, passRects, 0))
        return false;
    int passCount = 1;
    if (combineMirrors) {
        QList<KisRenderedDab> reflected = dabs;
        QVector<Qt::Orientation> directions;
        if (painter->hasHorizontalMirroring())
            directions << Qt::Horizontal;
        if (painter->hasVerticalMirroring())
            directions << Qt::Vertical;
        if (painter->hasHorizontalMirroring() && painter->hasVerticalMirroring())
            directions << Qt::Horizontal;
        quint32 flips = 0;
        for (const auto direction : directions) {
            flips ^= direction == Qt::Horizontal ? 1u : 2u;
            for (auto &dab : reflected)
                painter->mirrorDab(direction, &dab, true); // positions only; source pixels remain immutable
            for (auto &rect : passRects)
                painter->mirrorRect(direction, &rect);
            if (!appendPass(reflected, passRects, flips))
                return false;
            ++passCount;
        }
    }
    if (inputs.isEmpty())
        return true; // No intersection with the CPU rectangles or selection.
    QRegion tileRects;
    qint64 tileCount = 0;
    {
        // Union on the device's tile grid: overlapping passes must share one
        // access/workgroup, while gaps between dabs/reflections allocate none.
        // Wash final merge enumerates this region. Allocating empty gap tiles
        // would clear extra hidden RGB with restricted generic blend modes.
        const QPoint offset(painter->device()->x(), painter->device()->y());
        for (const auto &input : inputs)
            tileRects += KisGpuMergeBatch::tileAligned(input.clip.translated(-offset)).translated(offset);
        for (const auto &rect : tileRects)
            tileCount += qint64(rect.width() / 64) * (rect.height() / 64);
        if (tileCount > (combineMirrors ? 4096 : 8192))
            return false;
    }
    if (!combineMirrors) {
        if (qint64(bounds.width()) * bounds.height() > 4096 * 4096
            || (qint64(bounds.width()) + 127) / 64 * ((qint64(bounds.height()) + 127) / 64) > 8192)
            return false;
    }
    QByteArray maskPixels;
    KisGpuDabCompositor::Mask mask{nullptr, bounds};
    if (selection) {
        // The mask remains a dense CPU snapshot; cap it even for sparse tiles.
        if (qint64(bounds.width()) * bounds.height() > 4096 * 4096)
            return false;
        maskPixels.resize(qsizetype(bounds.width()) * bounds.height());
        // Read the same projected 8-bit mask as KisPainter::bltFixed. Its
        // lifetime follows the same stroke selection ownership as the CPU path.
        selection->projection()->readBytes(reinterpret_cast<quint8 *>(maskPixels.data()), bounds);
        mask.pixels = reinterpret_cast<const quint8 *>(maskPixels.constData());
    }
    QMutexLocker locker(&s_mutex);
    auto *backend = KisGpuTileBackend::instance();
    QString error;
    const quint64 required = KisGpuDabCompositor::requiredUploadBytes(int(tileCount),
                                                                      inputs,
                                                                      selection ? &mask : nullptr,
                                                                      &error,
                                                                      pixelSize);
    Work *work = workPool().acquire(backend->context(), required);
    if (!work) {
        if (!error.isEmpty() && qEnvironmentVariableIntValue("KRITA_GPU_BRUSH_DEBUG") == 1)
            qInfo() << "GPU brush: recording refused" << error;
        return false;
    }
    auto &commands = work->commands;
    commands.begin();
    std::vector<std::unique_ptr<KisGpuTileAccess>> ownedAccesses;
    QVector<KisGpuTileAccess *> accesses;
    QVector<VkDeviceAddress> addresses;
    QVector<QPoint> origins;
    for (const auto &rect : tileRects) {
        ownedAccesses.push_back(
            std::make_unique<KisGpuTileAccess>(painter->device(), rect, KisGpuTileAccess::ReadWrite));
        auto *access = ownedAccesses.back().get();
        accesses << access;
        if (!access->prepare(commands)) {
            KisGpuTileAccess::finishUnsubmitted(commands, accesses);
            return false;
        }
        addresses += access->addresses();
        const QRect grid = access->tileGrid();
        for (int row = grid.top(); row <= grid.bottom(); ++row)
            for (int col = grid.left(); col <= grid.right(); ++col)
                origins << access->tileOrigin(col, row);
    }
    if (!work->compositor->record(commands,
                                  addresses,
                                  1,
                                  QPoint(),
                                  inputs,
                                  mode,
                                  selection ? &mask : nullptr,
                                  channelMask,
                                  &error,
                                  origins,
                                  pixelSize)) {
        if (qEnvironmentVariableIntValue("KRITA_GPU_BRUSH_DEBUG") == 1)
            qInfo() << "GPU brush: recording refused" << error << "dabs" << dabs.size() << "passes" << passCount;
        KisGpuTileAccess::finishUnsubmitted(commands, accesses);
        return false;
    }
    const quint64 value = KisGpuTileAccess::submitAndFinish(commands, accesses);
    if (!value)
        return false;
    KisPaintTrace::link("path.brush.submitted", painter, KisPaintTrace::currentJob());
    if (generatedDabs) {
        KisPaintTrace::link("path.brush.generated_dabs", painter, KisPaintTrace::currentJob());
        s_generatedDabs += generatedDabs;
    }
    work->lastUse = value;
    // A submitted write must never be replayed by CPU fallback, even on device loss.
    // Existing tile readback/content-loss reporting handles subsequent CPU reads.
    // Tile state is published at submission. CPU readers wait on tile lastUse;
    // later GPU jobs observe queue order. Keep source/mask/table bytes alive
    // in this ring slot until reuse instead of waiting after every batch.
    const auto count = ++s_batches;
    if (qEnvironmentVariableIntValue("KRITA_GPU_BRUSH_DEBUG") == 1) {
        // Log each path even if the user first paints many batches in another mode.
        static std::atomic<quint32> messages[quint32(Mode::Count) * 2 * 16 * 4 * 2]{};
        const quint32 mirrorFlags =
            (painter->hasHorizontalMirroring() ? 1u : 0u) | (painter->hasVerticalMirroring() ? 2u : 0u);
        const quint32 path = ((quint32(mode) * 32 + (selection ? 16 : 0) + (channelMask & 15)) * 4 + mirrorFlags) * 2
            + (combineMirrors ? 1 : 0);
        if (messages[path].fetch_add(1) < 3) {
            qInfo() << "GPU brush: batch" << count << "mode" << painter->compositeOpId() << "variant" << quint32(mode)
                    << "selection" << bool(selection) << "channels" << channelMask << "mirrors" << mirrorFlags << "dabs"
                    << dabs.size() << "passes" << passCount << "fragments" << inputs.size() << "tiles"
                    << addresses.size() << "bounds" << bounds;
        }
    }
    return true;
#else
    Q_UNUSED(painter);
    Q_UNUSED(dabs);
    Q_UNUSED(paintRects);
    Q_UNUSED(combineMirrors);
    return false;
#endif
}
void KisGpuBrushPainter::refusePendingBatchesForTesting(int count)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    s_refusePendingBatches = count;
#else
    Q_UNUSED(count);
#endif
}

quint64 KisGpuBrushPainter::generatedDabCount()
{
#ifdef HAVE_KRITA_GPU_ENGINE
    return s_generatedDabs.load();
#else
    return 0;
#endif
}

quint64 KisGpuBrushPainter::batchCount()
{
#ifdef HAVE_KRITA_GPU_ENGINE
    return s_batches.load();
#else
    return 0;
#endif
}
KisGpuBrushPainter::StagingStatistics KisGpuBrushPainter::stagingStatistics()
{
    StagingStatistics result;
#ifdef HAVE_KRITA_GPU_ENGINE
    QMutexLocker locker(&s_mutex);
    result.bytes = workPool().bytes();
    for (const auto &slot : workPool().slots)
        if (slot)
            ++result.contexts;
#endif
    return result;
}
bool KisGpuBrushPainter::resetStagingForTesting()
{
#ifdef HAVE_KRITA_GPU_ENGINE
    QMutexLocker locker(&s_mutex);
    auto &pool = workPool();
    for (const auto &slot : pool.slots)
        if (slot && !slot->wait())
            return false;
    for (auto &slot : pool.slots)
        slot.reset();
    pool.next = 0;
#endif
    return true;
}
bool KisGpuBrushPainter::compositeWash(KisPainter *painter,
                                       KisPaintDeviceSP source,
                                       const QRect &rect,
                                       KisPaintDeviceSP base)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    if (!supports(painter) || painter->compositeOpId() == COMPOSITE_ALPHA_DARKEN || !source
        || source == painter->device() || source->defaultPixel().opacityF() != 0.0
        || (base && (base == painter->device() || base == source)))
        return false;
    const bool half = painter->device()->pixelSize() == 8;
    const auto flags = painter->channelFlags();
    quint32 channels = 0xf;
    if (!flags.isEmpty()) {
        channels = 0;
        for (int i = 0; i < 4; ++i)
            if (flags.testBit(i))
                channels |= 1u << i;
    }
    const float opacity = float(painter->opacityF());
    if (!std::isfinite(opacity) || opacity < 0 || opacity > 1)
        return false;
    KisGpuBlendOp op = KisGpuBlendOp::Erase;
    if (painter->compositeOpId() != COMPOSITE_ERASE
        && !KisGpuProjectionCompositor::blendOpForCompositeOp(painter->compositeOpId(), &op))
        return false;
    QRect paintRect = rect;
    QByteArray maskPixels;
    KisGpuLayerCompositor::Mask mask;
    const auto selection = painter->selection();
    // The base copy covers the whole rect; the Wash layer only the selection.
    QVector<KisGpuProjectionCompositor::Layer> layers;
    if (base) {
        KisGpuProjectionCompositor::Layer copy;
        copy.device = base;
        copy.replace = true;
        layers << copy;
    }
    if (selection) {
        paintRect &= selection->selectedRect();
        if (paintRect.isEmpty())
            return !base || KisGpuProjectionCompositor::composite(painter->device(), rect, layers);
        const qint64 bytes = qint64(paintRect.width()) * paintRect.height();
        if (bytes > 4096 * 4096)
            return false;
        maskPixels.resize(qsizetype(bytes));
        selection->projection()->readBytes(reinterpret_cast<quint8 *>(maskPixels.data()), paintRect);
        mask = {reinterpret_cast<const quint8 *>(maskPixels.constData()), paintRect};
    }
    layers << KisGpuProjectionCompositor::Layer{source,
                                                op,
                                                opacity,
                                                false,
                                                channels,
                                                half && (op == KisGpuBlendOp::Over || op == KisGpuBlendOp::Erase),
                                                !flags.isEmpty()};
    return KisGpuProjectionCompositor::composite(painter->device(),
                                                 base ? rect : paintRect,
                                                 layers,
                                                 nullptr,
                                                 selection ? &mask : nullptr);
#else
    Q_UNUSED(painter);
    Q_UNUSED(source);
    Q_UNUSED(rect);
    Q_UNUSED(base);
    return false;
#endif
}
bool KisGpuBrushPainter::paintWashPreview(KisPainter *painter,
                                          KisPaintDeviceSP source,
                                          const QRect &rect,
                                          KisPaintDeviceSP base)
{
    if (!compositeWash(painter, source, rect, base))
        return false;
#ifdef HAVE_KRITA_GPU_ENGINE
    if (base)
        ++s_washBaseCopies;
    const auto count = ++s_washPreviews;
    if (count <= 3 && qEnvironmentVariableIntValue("KRITA_GPU_BRUSH_DEBUG") == 1)
        qInfo() << "GPU brush: Wash preview" << count << "mode" << painter->compositeOpId() << "rect" << rect;
#endif
    return true;
}
bool KisGpuBrushPainter::mergeWash(KisPainter *painter, KisPaintDeviceSP source, const QRect &rect)
{
    // Source region rectangles are disjoint, but partial destination tiles
    // could still overlap another concurrent CPU/GPU merge job.
    if (rect.isEmpty() || rect.x() % 64 || rect.y() % 64 || rect.width() % 64 || rect.height() % 64)
        return false;
    if (!compositeWash(painter, source, rect))
        return false;
    painter->addDirtyRect(rect);
#ifdef HAVE_KRITA_GPU_ENGINE
    const auto count = ++s_washMerges;
    if (count <= 3 && qEnvironmentVariableIntValue("KRITA_GPU_BRUSH_DEBUG") == 1)
        qInfo() << "GPU brush: Wash final merge" << count << "mode" << painter->compositeOpId() << "rect" << rect;
#endif
    return true;
}
quint64 KisGpuBrushPainter::washMergeCount()
{
#ifdef HAVE_KRITA_GPU_ENGINE
    return s_washMerges.load();
#else
    return 0;
#endif
}
quint64 KisGpuBrushPainter::washBaseCopyCount()
{
#ifdef HAVE_KRITA_GPU_ENGINE
    return s_washBaseCopies.load();
#else
    return 0;
#endif
}
quint64 KisGpuBrushPainter::washPreviewCount()
{
#ifdef HAVE_KRITA_GPU_ENGINE
    return s_washPreviews.load();
#else
    return 0;
#endif
}
