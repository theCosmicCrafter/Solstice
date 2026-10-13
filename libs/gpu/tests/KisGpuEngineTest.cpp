/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuEngineTest.h"

#include <simpletest.h>

#include <QElapsedTimer>
#include <QFloat16>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent>

#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOp.h>
#include <KoCompositeOpRegistry.h>

#include <KisGpuBuffer.h>
#include <KisGpuCommandList.h>
#include <KisGpuComputePipeline.h>
#include <KisGpuContext.h>
#include <KisGpuDabCompositor.h>
#include <KisGpuLayerCompositor.h>
#include <KisGpuLayerStackCompositor.h>
#include <KisGpuTilePool.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <random>
#include <set>
#include <thread>

namespace
{
constexpr int TileSize = KisGpuTilePool::TileSize;
constexpr int PixelsPerTile = TileSize * TileSize;
constexpr int FloatsPerTile = PixelsPerTile * 4;

/// A stack of layers stored tile by tile (RGBA float, straight alpha).
struct LayerStack {
    int tileCount = 0;
    int layerCount = 0;
    QVector<float> opacities;
    /// layer-major: pixels of (layer, tile) start at (layer * tileCount + tile) * FloatsPerTile
    std::vector<float> pixels;
    /// layer-major: true if the tile of that layer is transparent (not allocated)
    std::vector<bool> empty;

    const float *tile(int layer, int tile) const
    {
        return pixels.data() + (size_t(layer) * tileCount + tile) * FloatsPerTile;
    }
};

LayerStack generateLayerStack(int tileCount, int layerCount, quint32 seed)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    LayerStack stack;
    stack.tileCount = tileCount;
    stack.layerCount = layerCount;
    stack.pixels.resize(size_t(tileCount) * layerCount * FloatsPerTile);
    stack.empty.resize(size_t(tileCount) * layerCount);

    for (int layer = 0; layer < layerCount; layer++) {
        // Exercise the special cases: fully transparent and fully opaque layers.
        const float opacity = layer == 1 ? 1.0f : (layer == 2 ? 0.0f : 0.25f + 0.75f * unit(random));
        stack.opacities << opacity;
    }

    for (size_t i = 0; i < stack.empty.size(); i++) {
        stack.empty[i] = unit(random) < 0.1f;
    }

    for (size_t pixel = 0; pixel < stack.pixels.size() / 4; pixel++) {
        float *p = stack.pixels.data() + pixel * 4;
        p[0] = unit(random);
        p[1] = unit(random);
        p[2] = unit(random);
        const float a = unit(random);
        p[3] = a < 0.25f ? 0.0f : (a < 0.5f ? 1.0f : unit(random));
    }
    return stack;
}

void roundToHalf(std::vector<float> &values)
{
    QVector<qfloat16> half(int(values.size()));
    qFloatToFloat16(half.data(), values.data(), qsizetype(values.size()));
    qFloatFromFloat16(values.data(), half.constData(), qsizetype(values.size()));
}

const KoCompositeOp *overOp(bool f16)
{
    const KoColorSpace *cs =
        KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(),
                                                     f16 ? Float16BitsColorDepthID.id() : Float32BitsColorDepthID.id(),
                                                     QString());
    return cs ? cs->compositeOp(COMPOSITE_OVER) : nullptr;
}

/// Flattens the stack with Krita's own CPU composite op, one tile at a time.
std::vector<float> compositeOnCpu(const LayerStack &stack, const KoCompositeOp *op, int threads)
{
    std::vector<float> result(size_t(stack.tileCount) * FloatsPerTile, 0.0f);
    const int rowStride = TileSize * 4 * sizeof(float);

    auto compositeTile = [&](int tile) {
        quint8 *dst = reinterpret_cast<quint8 *>(result.data() + size_t(tile) * FloatsPerTile);
        for (int layer = 0; layer < stack.layerCount; layer++) {
            if (stack.empty[size_t(layer) * stack.tileCount + tile]) {
                continue;
            }
            const quint8 *src = reinterpret_cast<const quint8 *>(stack.tile(layer, tile));
            op->composite(dst, rowStride, src, rowStride, nullptr, 0, TileSize, TileSize, stack.opacities[layer]);
        }
    };

    if (threads <= 1) {
        for (int tile = 0; tile < stack.tileCount; tile++) {
            compositeTile(tile);
        }
    } else {
        QVector<int> tiles(stack.tileCount);
        std::iota(tiles.begin(), tiles.end(), 0);
        QThreadPool pool;
        pool.setMaxThreadCount(threads);
        QtConcurrent::blockingMap(&pool, tiles, compositeTile);
    }
    return result;
}

/// Same as compositeOnCpu for 16-bit float pixels (the op works on qfloat16 data).
std::vector<float> compositeOnCpuF16(const LayerStack &stack, const KoCompositeOp *op)
{
    QVector<qfloat16> dst(stack.tileCount * FloatsPerTile, qfloat16(0.0f));
    QVector<qfloat16> src(FloatsPerTile);
    const int rowStride = TileSize * 4 * sizeof(qfloat16);

    for (int tile = 0; tile < stack.tileCount; tile++) {
        quint8 *dstTile = reinterpret_cast<quint8 *>(dst.data() + tile * FloatsPerTile);
        for (int layer = 0; layer < stack.layerCount; layer++) {
            if (stack.empty[size_t(layer) * stack.tileCount + tile]) {
                continue;
            }
            qFloatToFloat16(src.data(), stack.tile(layer, tile), FloatsPerTile);
            op->composite(dstTile,
                          rowStride,
                          reinterpret_cast<const quint8 *>(src.constData()),
                          rowStride,
                          nullptr,
                          0,
                          TileSize,
                          TileSize,
                          stack.opacities[layer]);
        }
    }

    std::vector<float> result(dst.size());
    qFloatFromFloat16(result.data(), dst.constData(), dst.size());
    return result;
}

struct GpuTimings {
    double kernelMs = 0.0;
    double uploadMs = 0.0;
    double readbackMs = 0.0;
};

/// Uploads the stack into a tile pool, flattens it on the GPU and reads the result back.
std::vector<float> compositeOnGpu(KisGpuContext &context,
                                  KisGpuTileFormat format,
                                  const LayerStack &stack,
                                  GpuTimings *timings = nullptr,
                                  int kernelRepetitions = 1)
{
    QString error;
    KisGpuTilePool pool(context, format, 4096);
    std::unique_ptr<KisGpuLayerStackCompositor> compositor =
        KisGpuLayerStackCompositor::create(context, format, &error);
    if (!compositor) {
        qWarning() << error;
        return {};
    }

    const quint32 pixelSize = pool.pixelSize();
    const VkDeviceSize tileBytes = pool.tileBytes();

    // Allocate slots for the non-empty layer tiles and the destination.
    QVector<quint32> layerSlots;
    QVector<VkDeviceAddress> layerTiles(stack.layerCount * stack.tileCount, 0);
    for (int layer = 0; layer < stack.layerCount; layer++) {
        for (int tile = 0; tile < stack.tileCount; tile++) {
            if (stack.empty[size_t(layer) * stack.tileCount + tile]) {
                continue;
            }
            const quint32 slot = pool.allocate(&error);
            if (slot == KisGpuTilePool::InvalidSlot) {
                qWarning() << error;
                return {};
            }
            layerSlots << slot;
            layerTiles[layer * stack.tileCount + tile] = pool.deviceAddress(slot);
        }
    }
    QVector<quint32> dstSlots;
    QVector<VkDeviceAddress> dstTiles;
    for (int tile = 0; tile < stack.tileCount; tile++) {
        dstSlots << pool.allocate(&error);
        dstTiles << pool.deviceAddress(dstSlots.last());
    }

    // Fill a staging buffer with the non-empty tiles in slot order.
    std::unique_ptr<KisGpuBuffer> staging =
        KisGpuBuffer::create(context, tileBytes * layerSlots.size(), KisGpuBuffer::Location::Upload, &error);
    std::unique_ptr<KisGpuBuffer> readback =
        KisGpuBuffer::create(context, tileBytes * dstSlots.size(), KisGpuBuffer::Location::Readback, &error);
    if (!staging || !readback) {
        qWarning() << error;
        return {};
    }

    {
        quint8 *out = static_cast<quint8 *>(staging->mapped());
        QVector<qfloat16> half(pixelSize == 8 ? FloatsPerTile : 0);
        for (int layer = 0; layer < stack.layerCount; layer++) {
            for (int tile = 0; tile < stack.tileCount; tile++) {
                if (stack.empty[size_t(layer) * stack.tileCount + tile]) {
                    continue;
                }
                if (pixelSize == 8) {
                    qFloatToFloat16(half.data(), stack.tile(layer, tile), FloatsPerTile);
                    std::memcpy(out, half.constData(), tileBytes);
                } else {
                    std::memcpy(out, stack.tile(layer, tile), tileBytes);
                }
                out += tileBytes;
            }
        }
    }

    KisGpuCommandList commands(context);
    commands.begin();
    commands.writeTimestamp(0);
    pool.recordUpload(commands, *staging, 0, layerSlots);
    commands.writeTimestamp(1);
    commands.computeBarrier();
    for (int i = 0; i < kernelRepetitions; i++) {
        compositor->record(commands, layerTiles, dstTiles, stack.opacities);
        commands.computeBarrier();
    }
    commands.writeTimestamp(2);
    pool.recordReadback(commands, dstSlots, *readback, 0);
    commands.writeTimestamp(3);
    commands.barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_HOST_BIT,
                     VK_ACCESS_2_HOST_READ_BIT);
    if (!commands.submit() || !commands.wait()) {
        qWarning() << "GPU submission failed";
        return {};
    }

    if (timings) {
        timings->uploadMs = commands.elapsedMs(0, 1);
        timings->kernelMs = commands.elapsedMs(1, 2) / kernelRepetitions;
        timings->readbackMs = commands.elapsedMs(2, 3);
    }

    std::vector<float> result(size_t(stack.tileCount) * FloatsPerTile);
    if (pixelSize == 8) {
        qFloatFromFloat16(result.data(), static_cast<const qfloat16 *>(readback->mapped()), qsizetype(result.size()));
    } else {
        std::memcpy(result.data(), readback->mapped(), result.size() * sizeof(float));
    }

    for (quint32 slot : layerSlots) {
        pool.release(slot);
    }
    for (quint32 slot : dstSlots) {
        pool.release(slot);
    }
    return result;
}

/**
 * Largest per-channel difference between two RGBA float images.
 *
 * The color of a fully transparent pixel is unspecified: Krita's SIMD over op
 * copies the source color into pixels that stay at alpha 0, the scalar paths
 * keep the old color. Such pixels are compared by alpha only.
 */
float maxAbsDifference(const std::vector<float> &a, const std::vector<float> &b, size_t *worstIndex = nullptr)
{
    float worst = 0.0f;
    for (size_t pixel = 0; pixel < a.size() / 4; pixel++) {
        const bool transparent = a[pixel * 4 + 3] == 0.0f && b[pixel * 4 + 3] == 0.0f;
        for (size_t channel = transparent ? 3 : 0; channel < 4; channel++) {
            const size_t i = pixel * 4 + channel;
            const float difference = std::abs(a[i] - b[i]);
            if (difference > worst || std::isnan(difference)) {
                worst = std::isnan(difference) ? INFINITY : difference;
                if (worstIndex) {
                    *worstIndex = i;
                }
            }
        }
    }
    return worst;
}
} // namespace

KisGpuEngineTest::KisGpuEngineTest() = default;
KisGpuEngineTest::~KisGpuEngineTest() = default;

void KisGpuEngineTest::initTestCase()
{
    m_context = KisGpuContext::create(&m_unavailableReason);
    if (m_context) {
        qInfo().noquote() << "GPU device:" << m_context->deviceInfo().summary()
                          << (m_context->validationEnabled() ? "[validation]" : "");
    }
}

void KisGpuEngineTest::cleanupTestCase()
{
    if (m_context) {
        QCOMPARE(m_context->validationErrorCount(), 0);
    }
    m_context.reset();
}

namespace
{
const quint32 PreciseProbeFloat[] = {
#include "precise_probe_float.spv.inc"
};
const quint32 PreciseProbeDouble[] = {
#include "precise_probe_double.spv.inc"
};
const quint32 PreciseProbeHalf[] = {
#include "precise_probe_half.spv.inc"
};
const quint32 BlendProbeRgba32f[] = {
#include "blend_probe_rgba32f.spv.inc"
};
const quint32 BlendProbeRgba16f[] = {
#include "blend_probe_rgba16f.spv.inc"
};
const quint32 PreciseProbeFloatUnoptimized[] = {
#include "precise_probe_float_unoptimized.spv.inc"
};
const quint32 PreciseProbeDoubleUnoptimized[] = {
#include "precise_probe_double_unoptimized.spv.inc"
};

// a + b * c with the product rounded first (the CPU build has no FMA
// target, so it never contracts) and fused
template<typename Real>
Real unfusedMultiplyAdd(Real a, Real b, Real c)
{
    volatile Real product = b * c;
    return a + product;
}

/**
 * Runs precise_probe.comp on triples whose fused and unfused results differ
 * and counts, per form, the results equal to the unfused (CPU) value.
 */
// light + ((v - light) * factor) * scale, every operation rounded
template<typename Real>
Real unfusedStretch(Real v, Real light, Real factor, Real scale)
{
    volatile Real difference = v - light;
    volatile Real scaled = difference * factor;
    volatile Real product = scaled * scale;
    return light + product;
}

// toneMappingAboveOne() of precise_probe.comp on the CPU, green channel
float unfusedToneMapping(float r, float g, float b)
{
    volatile float c[3] = {r + 1.0f, g + 1.0f, b + 1.0f};
    const float high = std::max(std::max(c[0], c[1]), c[2]);
    const float low = std::min(std::min(c[0], c[1]), c[2]);
    volatile float sum = high + low;
    volatile float light = sum * 0.5f;
    volatile float stretch = high - light;
    volatile float inverse = 1.0f - light;
    volatile float scale = 1.0f / stretch;
    volatile float difference = c[1] - light;
    volatile float scaled = difference * inverse;
    volatile float product = scaled * scale;
    return light + product;
}

template<typename Real>
bool runPreciseProbe(KisGpuContext &context,
                     const quint32 *spirv,
                     size_t bytes,
                     int counts[8],
                     int *items,
                     bool halfToneMapping = false)
{
    std::mt19937 random(11);
    std::uniform_real_distribution<Real> unit(-2, 2);
    std::vector<Real> input;
    while (input.size() < 3 * 4096) {
        const Real a = unit(random), b = unit(random), c = unit(random);
        if (unfusedMultiplyAdd(a, b, c) != std::fma(b, c, a)) {
            input.push_back(a);
            input.push_back(b);
            input.push_back(c);
        }
    }
    const quint32 count = quint32(input.size() / 3);
    *items = int(count);
    auto inputBuffer = KisGpuBuffer::create(context, input.size() * sizeof(Real), KisGpuBuffer::Location::Upload);
    auto outputBuffer = KisGpuBuffer::create(context, count * 8 * sizeof(Real), KisGpuBuffer::Location::Readback);
    auto pipeline = KisGpuComputePipeline::create(context, spirv, bytes, 20);
    if (!inputBuffer || !outputBuffer || !pipeline)
        return false;
    std::memcpy(inputBuffer->mapped(), input.data(), input.size() * sizeof(Real));
    struct {
        VkDeviceAddress input;
        VkDeviceAddress output;
        quint32 count;
    } constants{inputBuffer->deviceAddress(), outputBuffer->deviceAddress(), count};
    KisGpuCommandList commands(context);
    commands.begin();
    pipeline->dispatchRaw(commands.commandBuffer(), &constants, 20, (count + 63) / 64, 1, 1);
    commands.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_HOST_BIT,
                     VK_ACCESS_2_HOST_READ_BIT);
    if (!commands.submit() || !commands.wait())
        return false;
    const Real *output = static_cast<const Real *>(outputBuffer->mapped());
    for (int form = 0; form < 8; ++form)
        counts[form] = 0;
    for (quint32 i = 0; i < count; ++i) {
        const Real a = input[3 * i], b = input[3 * i + 1], c = input[3 * i + 2];
        const Real expected = unfusedMultiplyAdd(a, b, c);
        for (int form = 0; form < 5; ++form)
            counts[form] += output[8 * i + form] == expected;
        counts[5] += output[8 * i + 5] == unfusedStretch(b, a, c, c);
        counts[6] += output[8 * i + 6] == Real(unfusedStretch(float(b), float(a), float(c), float(c)));
        const float toneMapped = unfusedToneMapping(float(a), float(b), float(c));
        counts[7] += output[8 * i + 7] == Real(halfToneMapping ? float(qfloat16(toneMapped)) : toneMapped);
    }
    return true;
}
} // namespace

#define REQUIRE_GPU()                                                                                                  \
    if (!m_context) {                                                                                                  \
        QSKIP(qPrintable(QStringLiteral("No usable Vulkan device: %1").arg(m_unavailableReason)));                     \
    }

void KisGpuEngineTest::testPreciseContraction_data()
{
    QTest::addColumn<bool>("doubles");
    QTest::addColumn<bool>("optimized");
    QTest::addColumn<bool>("half");
    for (bool doubles : {false, true})
        for (bool optimized : {true, false})
            QTest::newRow(qPrintable(
                QStringLiteral("%1-%2").arg(doubles ? "double" : "float", optimized ? "optimized" : "unoptimized")))
                << doubles << optimized << false;
    QTest::newRow("float-half-extension") << false << true << true;
}

void KisGpuEngineTest::testPreciseContraction()
{
    // Phase 4.108 found "l + t * s" fused into one fma although the SPIR-V
    // marks it NoContraction. This records, per driver, which forms of a
    // precise multiply-add the driver fuses (qInfo), and checks that the
    // fusion-proof roundedProduct() form always keeps the CPU's rounding.
    REQUIRE_GPU();
    QFETCH(bool, doubles);
    QFETCH(bool, optimized);
    QFETCH(bool, half);
    if (doubles && !m_context->deviceInfo().supportsFloat64)
        QSKIP("shaderFloat64 is not enabled");
    int counts[8];
    int items = 0;
    const bool ran = half
        ? runPreciseProbe<float>(*m_context, PreciseProbeHalf, sizeof(PreciseProbeHalf), counts, &items, true)
        : doubles
        ? (optimized
               ? runPreciseProbe<double>(*m_context, PreciseProbeDouble, sizeof(PreciseProbeDouble), counts, &items)
               : runPreciseProbe<double>(*m_context,
                                         PreciseProbeDoubleUnoptimized,
                                         sizeof(PreciseProbeDoubleUnoptimized),
                                         counts,
                                         &items))
        : (optimized ? runPreciseProbe<float>(*m_context, PreciseProbeFloat, sizeof(PreciseProbeFloat), counts, &items)
                     : runPreciseProbe<float>(*m_context,
                                              PreciseProbeFloatUnoptimized,
                                              sizeof(PreciseProbeFloatUnoptimized),
                                              counts,
                                              &items));
    QVERIFY(ran);
    qInfo().noquote() << QStringLiteral(
                             "unfused results of %1 items: single precise expression %2, precise product "
                             "variable %3, vector %4, roundedProduct %5, explicit fma %6, stretch "
                             "function %7, vector stretch function %8, tone mapping %9")
                             .arg(items)
                             .arg(counts[0])
                             .arg(counts[1])
                             .arg(counts[2])
                             .arg(counts[3])
                             .arg(counts[4])
                             .arg(counts[5])
                             .arg(counts[6])
                             .arg(counts[7]);
    QCOMPARE(counts[3], items);
    QCOMPARE(counts[4], 0);
}

void KisGpuEngineTest::testBlendProbe()
{
    // Phase 4.108/4.109: an RGBA16F Increase Saturation (HSL) dab whose
    // blended green lands exactly on a half rounding tie on the CPU
    // (0.980712890625). With the precise multiply-add of hsxToneMapping()
    // the NVIDIA driver returned the next float (fused, despite
    // NoContraction); roundedProduct() keeps the CPU's rounding.
    REQUIRE_GPU();
    const float items[] = {0.151000977f,
                           0.225219727f,
                           0.674316406f,
                           0.645507813f,
                           0.877929688f,
                           0.864746094f,
                           0.350585938f,
                           0.276367188f};
    for (bool half : {false, true}) {
        auto input = KisGpuBuffer::create(*m_context, sizeof(items), KisGpuBuffer::Location::Upload);
        auto output = KisGpuBuffer::create(*m_context, sizeof(items), KisGpuBuffer::Location::Readback);
        auto pipeline = half
            ? KisGpuComputePipeline::create(*m_context, BlendProbeRgba16f, sizeof(BlendProbeRgba16f), 24)
            : KisGpuComputePipeline::create(*m_context, BlendProbeRgba32f, sizeof(BlendProbeRgba32f), 24);
        QVERIFY(input && output && pipeline);
        std::memcpy(input->mapped(), items, sizeof(items));
        struct {
            VkDeviceAddress source;
            VkDeviceAddress target;
            quint32 op;
            quint32 count;
        } constants{input->deviceAddress(), output->deviceAddress(), quint32(KisGpuBlendOp::IncreaseSaturationHsl), 1};
        KisGpuCommandList commands(*m_context);
        commands.begin();
        pipeline->dispatchRaw(commands.commandBuffer(), &constants, 24, 1, 1, 1);
        commands.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_HOST_BIT,
                         VK_ACCESS_2_HOST_READ_BIT);
        QVERIFY(commands.submit() && commands.wait());
        const float green = static_cast<const float *>(output->mapped())[1];
        // bit for bit, not QCOMPARE's fuzzy float comparison
        QVERIFY2(green == 0.980712890625f, qPrintable(QString::number(double(green), 'g', 9)));
    }
}

void KisGpuEngineTest::testDeviceInfo()
{
    REQUIRE_GPU();
    const KisGpuDeviceInfo &info = m_context->deviceInfo();
    QVERIFY(!info.name.isEmpty());
    QCOMPARE(info.deviceUuid.size(), int(VK_UUID_SIZE));
    QVERIFY(info.apiVersion >= VK_API_VERSION_1_3);
    QVERIFY(info.timestampPeriodNs > 0.0f);
    QVERIFY(info.deviceLocalBytes > 0);
}

void KisGpuEngineTest::testSoftwareDevicesRefused()
{
    // Needs no GPU: the filter used by device selection.
    QVERIFY(!KisGpuContext::acceptsDeviceType(VK_PHYSICAL_DEVICE_TYPE_CPU, false));
    QVERIFY(KisGpuContext::acceptsDeviceType(VK_PHYSICAL_DEVICE_TYPE_CPU, true));
    for (VkPhysicalDeviceType type : {VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                                      VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                                      VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
                                      VK_PHYSICAL_DEVICE_TYPE_OTHER}) {
        QVERIFY(KisGpuContext::acceptsDeviceType(type, false));
    }
}

void KisGpuEngineTest::testTilePoolAllocation()
{
    REQUIRE_GPU();
    KisGpuTilePool pool(*m_context, KisGpuTileFormat::RGBA32F, 64);
    QCOMPARE(pool.tileBytes(), VkDeviceSize(64 * 64 * 16));

    QVector<quint32> slots;
    for (int i = 0; i < 200; i++) {
        const quint32 slot = pool.allocate();
        QVERIFY(slot != KisGpuTilePool::InvalidSlot);
        slots << slot;
    }
    QCOMPARE(pool.allocatedSlots(), 200u);
    QCOMPARE(pool.capacitySlots(), 256u); // four chunks of 64

    // Slots are unique and their addresses are tile-aligned and distinct.
    QVector<quint32> sorted = slots;
    std::sort(sorted.begin(), sorted.end());
    QVERIFY(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    QCOMPARE(pool.deviceAddress(slots[1]) - pool.deviceAddress(slots[0]), pool.tileBytes());

    for (quint32 slot : slots) {
        pool.release(slot);
    }
    QCOMPARE(pool.allocatedSlots(), 0u);

    // Released slots are reused before the pool grows.
    for (int i = 0; i < 256; i++) {
        pool.allocate();
    }
    QCOMPARE(pool.capacitySlots(), 256u);
    for (quint32 slot = 0; slot < 256; slot++) {
        pool.release(slot);
    }
    pool.trim();
    QCOMPARE(pool.reservedBytes(), VkDeviceSize(0));
    QCOMPARE(pool.capacitySlots(), 0u);

    // A budget smaller than a full chunk must still work and never overshoot.
    const VkDeviceSize budget = pool.tileBytes() * 3;
    const quint32 first = pool.allocate(nullptr, budget);
    const quint32 second = pool.allocate(nullptr, budget);
    const quint32 third = pool.allocate(nullptr, budget);
    QVERIFY(first != KisGpuTilePool::InvalidSlot);
    QVERIFY(second != KisGpuTilePool::InvalidSlot);
    QVERIFY(third != KisGpuTilePool::InvalidSlot);
    QCOMPARE(pool.reservedBytes(), budget);
    QCOMPARE(pool.allocate(nullptr, budget), KisGpuTilePool::InvalidSlot);
    const VkDeviceAddress liveAddress = pool.deviceAddress(second);
    pool.release(first);
    pool.trim();
    QCOMPARE(pool.deviceAddress(second), liveAddress);
    QCOMPARE(pool.allocate(nullptr, budget), first);
    pool.release(first);
    pool.release(second);
    pool.release(third);
    pool.trim();
    QCOMPARE(pool.reservedBytes(), VkDeviceSize(0));
}

void KisGpuEngineTest::testUploadReadbackRoundTrip()
{
    REQUIRE_GPU();
    KisGpuTilePool pool(*m_context, KisGpuTileFormat::RGBA32F, 16);
    const int tileCount = 40; // spans three chunks
    const VkDeviceSize bytes = pool.tileBytes() * tileCount;

    std::unique_ptr<KisGpuBuffer> staging = KisGpuBuffer::create(*m_context, bytes, KisGpuBuffer::Location::Upload);
    std::unique_ptr<KisGpuBuffer> readback = KisGpuBuffer::create(*m_context, bytes, KisGpuBuffer::Location::Readback);
    QVERIFY(staging && readback);

    std::mt19937 random(7);
    quint32 *words = static_cast<quint32 *>(staging->mapped());
    for (VkDeviceSize i = 0; i < bytes / 4; i++) {
        words[i] = random();
    }

    // Allocate in a scattered order so that slot order differs from tile order.
    QVector<quint32> slots;
    for (int i = 0; i < 48; i++) {
        slots << pool.allocate();
    }
    std::shuffle(slots.begin(), slots.end(), random);
    const QVector<quint32> used = slots.mid(0, tileCount);

    KisGpuCommandList commands(*m_context);
    commands.begin();
    pool.recordUpload(commands, *staging, 0, used);
    commands.computeBarrier();
    pool.recordReadback(commands, used, *readback, 0);
    commands.barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_HOST_BIT,
                     VK_ACCESS_2_HOST_READ_BIT);
    QVERIFY(commands.submit());
    QVERIFY(commands.wait());

    QVERIFY(std::memcmp(staging->mapped(), readback->mapped(), size_t(bytes)) == 0);

    for (quint32 slot : slots) {
        pool.release(slot);
    }
}

void KisGpuEngineTest::testEarlyMainFinish()
{
    REQUIRE_GPU();
    auto source = KisGpuBuffer::create(*m_context, 16, KisGpuBuffer::Location::Upload);
    auto readback = KisGpuBuffer::create(*m_context, 16, KisGpuBuffer::Location::Readback);
    QVERIFY(source && readback);
    KisGpuCommandList commands(*m_context);
    for (quint32 value : {11u, 22u}) {
        commands.begin();
        commands.copyBuffer(*source, 0, *readback, 0, 16);
        commands.barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_HOST_BIT,
                         VK_ACCESS_2_HOST_READ_BIT);
        QVERIFY(commands.finishMainRecording());
        QVERIFY(commands.finishMainRecording());
        // Late upload must execute before the already-ended main buffer.
        m_context->vk().vkCmdFillBuffer(commands.preamble(), source->handle(), 0, 16, value);
        QVERIFY(commands.submit());
        QVERIFY(commands.wait());
        for (int i = 0; i < 4; ++i)
            QCOMPARE(static_cast<const quint32 *>(readback->mapped())[i], value);
    }
    commands.begin();
    QVERIFY(commands.finishMainRecording());
    commands.abandon();
    QVERIFY(!commands.isRecording());
    commands.begin();
    QVERIFY(commands.finishMainRecording());
    m_context->injectSubmitFailuresForTesting(1);
    QCOMPARE(commands.submit(), quint64(0));
    commands.abandon();
    commands.begin();
    QVERIFY(commands.submit());
    QVERIFY(commands.wait());
}

void KisGpuEngineTest::testConcurrentSubmissionTiming()
{
    REQUIRE_GPU();
    const int errorsBefore = m_context->validationErrorCount();
    struct Result {
        bool ok = true;
        QVector<quint64> values;
        QVector<KisGpuSubmitTiming> timings;
    };
    std::vector<Result> results(4);
    QVector<int> workers{0, 1, 2, 3};
    QThreadPool pool;
    pool.setMaxThreadCount(4);
    QtConcurrent::blockingMap(&pool, workers, [&](int worker) {
        Result &result = results[worker];
        auto output = KisGpuBuffer::create(*m_context, 16, KisGpuBuffer::Location::Readback);
        KisGpuCommandList commands(*m_context);
        if (!output || !commands.isValid()) {
            result.ok = false;
            return;
        }
        for (int iteration = 0; iteration < 32; ++iteration) {
            const quint32 expected = quint32(worker * 32 + iteration + 1);
            commands.begin();
            m_context->vk().vkCmdFillBuffer(commands.commandBuffer(), output->handle(), 0, 16, expected);
            commands.barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_HOST_BIT,
                             VK_ACCESS_2_HOST_READ_BIT);
            KisGpuSubmitTiming timing;
            const quint64 value = commands.submit({}, {}, &timing);
            if (!value || !commands.wait()) {
                result.ok = false;
                return;
            }
            for (int word = 0; word < 4; ++word)
                result.ok = result.ok && static_cast<const quint32 *>(output->mapped())[word] == expected;
            result.values << value;
            result.timings << timing;
        }
    });
    std::set<quint64> submitted;
    for (const Result &result : results) {
        QVERIFY(result.ok);
        QCOMPARE(result.values.size(), 32);
        for (quint64 value : result.values)
            QVERIFY(submitted.insert(value).second);
        for (const KisGpuSubmitTiming &timing : result.timings) {
            QVERIFY(timing.lockStartNs > 0);
            QVERIFY(timing.lockStartNs <= timing.lockAcquiredNs);
            QVERIFY(timing.lockAcquiredNs <= timing.driverStartNs);
            QVERIFY(timing.driverStartNs <= timing.driverEndNs);
        }
    }
    KisGpuCommandList commands(*m_context);
    commands.begin();
    m_context->injectSubmitFailuresForTesting(1);
    KisGpuSubmitTiming refused;
    refused.driverEndNs = 1;
    QCOMPARE(commands.submit({}, {}, &refused), quint64(0));
    QVERIFY(refused.lockStartNs > 0);
    QVERIFY(refused.lockAcquiredNs >= refused.lockStartNs);
    QCOMPARE(refused.driverStartNs, qint64(0));
    QCOMPARE(refused.driverEndNs, qint64(0));
    commands.begin();
    QVERIFY(commands.submit());
    QVERIFY(commands.wait());
    QCOMPARE(m_context->validationErrorCount(), errorsBefore);
}

void KisGpuEngineTest::testCompositeOverMatchesKoCompositeOp()
{
    REQUIRE_GPU();
    const KoCompositeOp *op = overOp(false);
    QVERIFY2(op, "RGBA F32 color space is not available");

    const LayerStack stack = generateLayerStack(64, 6, 1234);
    const std::vector<float> expected = compositeOnCpu(stack, op, 1);
    const std::vector<float> actual = compositeOnGpu(*m_context, KisGpuTileFormat::RGBA32F, stack);
    QCOMPARE(actual.size(), expected.size());

    size_t worstIndex = 0;
    const float difference = maxAbsDifference(expected, actual, &worstIndex);
    QVERIFY2(difference <= 1e-5f,
             qPrintable(QStringLiteral("max difference %1 at float %2 (expected %3, got %4)")
                            .arg(difference)
                            .arg(worstIndex)
                            .arg(expected[worstIndex])
                            .arg(actual[worstIndex])));
}

void KisGpuEngineTest::testCompositeOverF16()
{
    REQUIRE_GPU();
    const KoCompositeOp *op = overOp(true);
    QVERIFY2(op, "RGBA F16 color space is not available");

    LayerStack stack = generateLayerStack(32, 6, 99);
    roundToHalf(stack.pixels);

    // Krita's F16 op rounds after every layer, the GPU only once at the end,
    // so the GPU result is the more precise one. Allow a few half ULPs.
    const std::vector<float> expected = compositeOnCpuF16(stack, op);
    const std::vector<float> actual = compositeOnGpu(*m_context, KisGpuTileFormat::RGBA16F, stack);
    QCOMPARE(actual.size(), expected.size());

    const float difference = maxAbsDifference(expected, actual);
    QVERIFY2(difference <= 4e-3f, qPrintable(QStringLiteral("max difference %1").arg(difference)));
}

void KisGpuEngineTest::benchmarkCompositeStack()
{
    REQUIRE_GPU();
    const KoCompositeOp *op = overOp(false);
    QVERIFY(op);

    const int size =
        qEnvironmentVariableIsSet("KRITA_GPU_BENCH_SIZE") ? qEnvironmentVariableIntValue("KRITA_GPU_BENCH_SIZE") : 4096;
    const int layers = qEnvironmentVariableIsSet("KRITA_GPU_BENCH_LAYERS")
        ? qEnvironmentVariableIntValue("KRITA_GPU_BENCH_LAYERS")
        : 8;
    const int tilesPerSide = size / TileSize;
    const int tileCount = tilesPerSide * tilesPerSide;

    const LayerStack stack = generateLayerStack(tileCount, layers, 42);

    QElapsedTimer timer;
    timer.start();
    const std::vector<float> cpuSingle = compositeOnCpu(stack, op, 1);
    const double cpuSingleMs = timer.nsecsElapsed() / 1.0e6;

    const int threads = QThread::idealThreadCount();
    timer.restart();
    const std::vector<float> cpuThreaded = compositeOnCpu(stack, op, threads);
    const double cpuThreadedMs = timer.nsecsElapsed() / 1.0e6;

    GpuTimings f32;
    const std::vector<float> gpu32 = compositeOnGpu(*m_context, KisGpuTileFormat::RGBA32F, stack, &f32, 10);
    GpuTimings f16;
    const std::vector<float> gpu16 = compositeOnGpu(*m_context, KisGpuTileFormat::RGBA16F, stack, &f16, 10);
    QVERIFY(!gpu32.empty() && !gpu16.empty());
    QVERIFY(maxAbsDifference(cpuSingle, gpu32) <= 1e-5f);

    const double megapixels = double(size) * size / 1.0e6;
    qInfo().noquote() << QStringLiteral("Composite %1 layers of %2x%2 (%3 MPix/layer), normal blend:")
                             .arg(layers)
                             .arg(size)
                             .arg(megapixels, 0, 'f', 1);
    qInfo().noquote() << QStringLiteral("  CPU KoCompositeOp, 1 thread:   %1 ms").arg(cpuSingleMs, 0, 'f', 2);
    qInfo().noquote()
        << QStringLiteral("  CPU KoCompositeOp, %1 threads: %2 ms").arg(threads).arg(cpuThreadedMs, 0, 'f', 2);
    qInfo().noquote() << QStringLiteral("  GPU RGBA32F kernel: %1 ms (upload %2 ms, readback %3 ms)")
                             .arg(f32.kernelMs, 0, 'f', 3)
                             .arg(f32.uploadMs, 0, 'f', 2)
                             .arg(f32.readbackMs, 0, 'f', 2);
    qInfo().noquote() << QStringLiteral("  GPU RGBA16F kernel: %1 ms (upload %2 ms, readback %3 ms)")
                             .arg(f16.kernelMs, 0, 'f', 3)
                             .arg(f16.uploadMs, 0, 'f', 2)
                             .arg(f16.readbackMs, 0, 'f', 2);
    qInfo().noquote() << QStringLiteral("  Kernel speed-up vs threaded CPU: F32 x%1, F16 x%2")
                             .arg(cpuThreadedMs / qMax(1e-6, f32.kernelMs), 0, 'f', 1)
                             .arg(cpuThreadedMs / qMax(1e-6, f16.kernelMs), 0, 'f', 1);
}

void KisGpuEngineTest::testSharedComputePipelines()
{
    REQUIRE_GPU();
    // Phase 4.91: every work context used to compile its own compositor
    // pipelines; the first parallel Wash merges each waited about 60ms. A
    // fresh context compiles each pipeline once, also under concurrency.
    QString error;
    std::unique_ptr<KisGpuContext> context = KisGpuContext::create(&error);
    QVERIFY2(context, qPrintable(error));
    QCOMPARE(context->sharedComputePipelineCompileCount(), 0);

    constexpr int Threads = 12;
    std::vector<std::unique_ptr<KisGpuLayerCompositor>> compositors(Threads);
    QElapsedTimer timer;
    timer.start();
    {
        std::vector<std::thread> threads;
        for (int i = 0; i < Threads; ++i)
            threads.emplace_back([&, i]() {
                compositors[size_t(i)] = KisGpuLayerCompositor::create(*context, KisGpuTileFormat::RGBA32F);
            });
        for (auto &thread : threads)
            thread.join();
    }
    const double concurrentMs = timer.nsecsElapsed() / 1e6;
    for (const auto &compositor : compositors)
        QVERIFY(compositor);
    QCOMPARE(context->sharedComputePipelineCompileCount(), 1);

    timer.restart();
    QVERIFY(KisGpuLayerCompositor::preparePipelines(*context, KisGpuTileFormat::RGBA32F, true, &error));
    const double extendedMs = timer.nsecsElapsed() / 1e6;
    QCOMPARE(context->sharedComputePipelineCompileCount(), 2);
    timer.restart();
    QVERIFY(KisGpuLayerCompositor::preparePipelines(*context, KisGpuTileFormat::RGBA16F, true, &error));
    const double halfMs = timer.nsecsElapsed() / 1e6;
    QCOMPARE(context->sharedComputePipelineCompileCount(), 4);
    // Prepared pipelines are reused without compiling again.
    QVERIFY(KisGpuLayerCompositor::create(*context, KisGpuTileFormat::RGBA16F));
    QVERIFY(KisGpuLayerCompositor::preparePipelines(*context, KisGpuTileFormat::RGBA32F, true, &error));
    QCOMPARE(context->sharedComputePipelineCompileCount(), 4);
    // Dab compositors share their pipeline as well.
    auto firstDab = KisGpuDabCompositor::create(*context);
    auto secondDab = KisGpuDabCompositor::create(*context);
    QVERIFY(firstDab && secondDab);
    QCOMPARE(context->sharedComputePipelineCompileCount(), 5);
    qInfo() << "Shared compositor pipelines: 12 concurrent creates (one compile) ms" << concurrentMs
            << "F32 extended ms" << extendedMs << "F16 basic+extended ms" << halfMs
            << "(driver shader caches may shorten repeated runs)";
    compositors.clear();
    firstDab.reset();
    secondDab.reset();
    QCOMPARE(context->validationErrorCount(), 0);
}

SIMPLE_TEST_MAIN(KisGpuEngineTest)
