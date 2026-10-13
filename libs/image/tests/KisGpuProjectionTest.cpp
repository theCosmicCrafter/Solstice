/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <simpletest.h>

#include <QElapsedTimer>
#include <QFloat16>
#include <QObject>
#include <QScopeGuard>

#include <KisGlobalResourcesInterface.h>
#include <KisGpuContext.h>
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorProfile.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>

#include "filter/kis_filter.h"
#include "filter/kis_filter_configuration.h"
#include "filter/kis_filter_registry.h"
#include "gpu/KisGpuEngineSettings.h"
#include "gpu/KisGpuMergeBatch.h"
#include "gpu/KisGpuProjectionCompositor.h"
#include "gpu/KisGpuTileAccess.h"
#include "gpu/KisGpuTileBackend.h"
#include "kis_adjustment_layer.h"
#include "kis_datamanager.h"
#include "kis_filter_mask.h"
#include "kis_group_layer.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_painter.h"
#include "kis_selection.h"
#include "tiles3/kis_tile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

/**
 * GPU engine phase 2: projection compositing on the GPU must match the CPU
 * projection (docs/agent/gpu-engine.md).
 */
class KisGpuProjectionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanup();

    void testFullRefreshMatchesCpu();
    void testBlendModesMatchCpu_data();
    void testBlendModesMatchCpu();
    void testExtendedBlendBoundaries_data();
    void testExtendedBlendBoundaries();
    void testLayerChannelFlags_data();
    void testLayerChannelFlags();
    void testChannelFlagBoundaries_data();
    void testChannelFlagBoundaries();
    void testPartialUpdateMatchesCpu();
    void testF16Image();
    void testProjectionStaysOnGpu();
    void testConcurrentUpdatesSharingTilesMatchCpu();
    void testMemoryBudgetFallsBackToCpu();
    void testFloatGroupInIntegerImageStaysOnCpu();
    void testConvertedDocumentUsesGpu();
    void benchmarkRefresh();
};

namespace
{
const KoColorSpace *rgbaFloat(bool f16 = false)
{
    return KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(),
                                                        f16 ? Float16BitsColorDepthID.id()
                                                            : Float32BitsColorDepthID.id(),
                                                        QString());
}

void fillRandom(KisPaintDeviceSP device, const QRect &rect, quint32 seed, bool hdr = false)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    QVector<float> floats(rect.width() * rect.height() * 4);
    for (int i = 0; i < floats.size(); i += 4) {
        const float scale = hdr ? 2.0f : 1.0f;
        floats[i] = unit(random) * scale;
        floats[i + 1] = unit(random) * scale;
        floats[i + 2] = unit(random);
        const float a = unit(random);
        floats[i + 3] = a < 0.2f ? 0.0f : (a < 0.4f ? 1.0f : unit(random));
    }

    if (device->pixelSize() == 16) {
        device->writeBytes(reinterpret_cast<const quint8 *>(floats.constData()), rect);
    } else {
        QVector<qfloat16> half(floats.size());
        qFloatToFloat16(half.data(), floats.constData(), floats.size());
        device->writeBytes(reinterpret_cast<const quint8 *>(half.constData()), rect);
    }
}

std::vector<float> readFloats(KisPaintDeviceSP device, const QRect &rect)
{
    const size_t count = size_t(rect.width()) * rect.height() * 4;
    std::vector<float> floats(count);
    if (device->pixelSize() == 16) {
        device->readBytes(reinterpret_cast<quint8 *>(floats.data()), rect);
    } else {
        std::vector<qfloat16> half(count);
        device->readBytes(reinterpret_cast<quint8 *>(half.data()), rect);
        qFloatFromFloat16(floats.data(), half.data(), qsizetype(count));
    }
    return floats;
}

/// Largest channel difference; the color of fully transparent pixels is ignored.
float maxDifference(const std::vector<float> &a, const std::vector<float> &b, QPoint *where, int width)
{
    float worst = 0.0f;
    for (size_t pixel = 0; pixel < a.size() / 4; pixel++) {
        const bool transparent = a[pixel * 4 + 3] == 0.0f && b[pixel * 4 + 3] == 0.0f;
        for (size_t c = transparent ? 3 : 0; c < 4; c++) {
            const float difference = std::abs(a[pixel * 4 + c] - b[pixel * 4 + c]);
            if (difference > worst || std::isnan(difference)) {
                worst = std::isnan(difference) ? INFINITY : difference;
                *where = QPoint(int(pixel % width), int(pixel / width));
            }
        }
    }
    return worst;
}

/**
 * Size of one half-float ULP at magnitude max(|value|, 1). Rounding errors of
 * blending come from the operands (about 1 for normalized colors), so values
 * near 0 produced by cancellation (subtract, difference) are judged at 1.
 */
float halfUlp(float value)
{
    const float magnitude = qMax(std::abs(value), 1.0f);
    return std::ldexp(1.0f, std::ilogb(magnitude) - 10);
}

/// Largest channel difference in half ULPs (see halfUlp()); transparent pixel colors ignored.
float maxHalfUlpDifference(const std::vector<float> &a, const std::vector<float> &b, QPoint *where, int width)
{
    float worst = 0.0f;
    for (size_t pixel = 0; pixel < a.size() / 4; pixel++) {
        const bool transparent = a[pixel * 4 + 3] == 0.0f && b[pixel * 4 + 3] == 0.0f;
        for (size_t c = transparent ? 3 : 0; c < 4; c++) {
            const float x = a[pixel * 4 + c];
            const float y = b[pixel * 4 + c];
            const float ulps = std::abs(x - y) / halfUlp(qMax(std::abs(x), std::abs(y)));
            if (ulps > worst || std::isnan(ulps)) {
                worst = std::isnan(ulps) ? INFINITY : ulps;
                *where = QPoint(int(pixel % width), int(pixel / width));
            }
        }
    }
    return worst;
}

KisPaintLayerSP addPaintLayer(KisImageSP image,
                              KisNodeSP parent,
                              const QString &compositeOp,
                              quint8 opacity,
                              quint32 seed,
                              const QRect &rect,
                              bool hdr = false)
{
    KisPaintLayerSP layer = new KisPaintLayer(image, QStringLiteral("layer %1").arg(seed), opacity);
    fillRandom(layer->paintDevice(), rect, seed, hdr);
    layer->setCompositeOpId(compositeOp);
    image->addNode(layer, parent);
    return layer;
}

/**
 *  root (background color)
 *    paint over                      (full image)
 *    paint multiply 60%
 *    group 78%
 *      paint screen
 *      paint overlay, alpha locked
 *      paint add (HDR values)
 *    paint hard light, moved by (128, 64)  (tile aligned)
 *    paint exclusion, moved by (10, 7)     (unaligned: CPU)
 *    blur adjustment layer                 (split point)
 *    paint over with a blur filter mask
 *    paint difference 80%
 *    paint darken
 *    paint lighten, subtract, HSL color (CPU split point)
 */
KisImageSP createTestImage(bool f16, QVector<KisPaintLayerSP> *layers)
{
    const QRect bounds(0, 0, 700, 500);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(f16), "gpu projection");

    const float background[4] = {0.2f, 0.3f, 0.4f, 1.0f};
    KoColor backgroundColor(
        KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(), QString()));
    std::memcpy(backgroundColor.data(), background, sizeof(background));
    backgroundColor.convertTo(rgbaFloat(f16));
    image->setDefaultProjectionColor(backgroundColor);

    KisNodeSP root = image->root();
    *layers << addPaintLayer(image, root, COMPOSITE_OVER, 255, 1, bounds);
    *layers << addPaintLayer(image, root, COMPOSITE_MULT, 153, 2, QRect(30, 20, 400, 300));

    KisGroupLayerSP group = new KisGroupLayer(image, "group", 200);
    image->addNode(group, root);
    *layers << addPaintLayer(image, group, COMPOSITE_SCREEN, 255, 3, QRect(100, 50, 500, 400));
    KisPaintLayerSP locked = addPaintLayer(image, group, COMPOSITE_OVERLAY, 230, 4, QRect(0, 0, 640, 480));
    QBitArray alphaLocked(4, true);
    alphaLocked.clearBit(3);
    locked->setChannelFlags(alphaLocked);
    *layers << locked;
    *layers << addPaintLayer(image, group, COMPOSITE_ADD, 128, 5, QRect(200, 100, 300, 300), true);

    KisPaintLayerSP moved = addPaintLayer(image, root, COMPOSITE_HARD_LIGHT, 255, 6, QRect(0, 0, 300, 200));
    moved->paintDevice()->moveTo(128, 64);
    *layers << moved;
    KisPaintLayerSP unaligned = addPaintLayer(image, root, COMPOSITE_EXCLUSION, 200, 7, QRect(0, 0, 250, 250));
    unaligned->paintDevice()->moveTo(10, 7);
    *layers << unaligned;

    KisFilterSP blur = KisFilterRegistry::instance()->value("blur");
    KIS_ASSERT(blur);
    KisFilterConfigurationSP config = blur->defaultConfiguration(KisGlobalResourcesInterface::instance());
    KisAdjustmentLayerSP adjustment =
        new KisAdjustmentLayer(image, "blur", config->cloneWithResourcesSnapshot(), nullptr);
    image->addNode(adjustment, root);

    KisPaintLayerSP masked = addPaintLayer(image, root, COMPOSITE_OVER, 180, 13, QRect(320, 128, 256, 256));
    KisFilterMaskSP mask = new KisFilterMask(image, "blur mask");
    mask->initSelection(masked);
    mask->setFilter(config->cloneWithResourcesSnapshot());
    image->addNode(mask, masked);
    *layers << masked;

    *layers << addPaintLayer(image, root, COMPOSITE_DIFF, 204, 8, QRect(64, 64, 512, 300));
    *layers << addPaintLayer(image, root, COMPOSITE_DARKEN, 255, 9, QRect(300, 200, 400, 300));
    *layers << addPaintLayer(image, root, COMPOSITE_LIGHTEN, 255, 10, QRect(0, 300, 300, 200));
    *layers << addPaintLayer(image, root, COMPOSITE_SUBTRACT, 100, 11, QRect(500, 0, 200, 500));
    *layers << addPaintLayer(image, root, COMPOSITE_COLOR_HSL, 255, 12, QRect(250, 250, 100, 100));
    return image;
}

std::vector<float> render(KisImageSP image, bool gpu)
{
    KisGpuMergeBatch::setEnabled(gpu);
    image->refreshGraphAsync();
    image->waitForDone();
    return readFloats(image->projection(), image->bounds());
}
} // namespace

#define REQUIRE_GPU()                                                                                                  \
    if (!KisGpuTileBackend::instance()) {                                                                              \
        QSKIP(qPrintable(QStringLiteral("GPU engine unavailable: %1").arg(KisGpuTileBackend::unavailableReason())));   \
    }

void KisGpuProjectionTest::initTestCase()
{
    QVERIFY(rgbaFloat() && rgbaFloat(true));
}

void KisGpuProjectionTest::cleanup()
{
    KisGpuMergeBatch::setEnabled(false);
    if (KisGpuTileBackend *backend = KisGpuTileBackend::existingInstance()) {
        backend->flush();
        QCOMPARE(backend->context().validationErrorCount(), 0);
    }
}

void KisGpuProjectionTest::testLayerChannelFlags_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("f16");
    for (const auto &mode : {COMPOSITE_OVER,
                             COMPOSITE_MULT,
                             COMPOSITE_SCREEN,
                             COMPOSITE_OVERLAY,
                             COMPOSITE_SOFT_LIGHT_SVG,
                             COMPOSITE_DODGE,
                             COMPOSITE_BURN,
                             COMPOSITE_COLOR}) {
        for (bool f16 : {false, true}) {
            QTest::newRow(qPrintable(QString("%1-f16%2").arg(mode).arg(f16))) << mode << f16;
        }
    }
}

void KisGpuProjectionTest::testLayerChannelFlags()
{
    REQUIRE_GPU();
    QFETCH(QString, mode);
    QFETCH(bool, f16);
    const QRect bounds(0, 0, 128, 128);
    const QRect changed(17, 29, 83, 71);
    KisImageSP image = new KisImage(nullptr, 128, 128, rgbaFloat(f16), "channel flags");
    addPaintLayer(image, image->root(), COMPOSITE_OVER, 255, 43, bounds);
    auto top = addPaintLayer(image, image->root(), mode, 179, 47, bounds);
    for (int bits = 0; bits < 16; ++bits) {
        QBitArray flags(4);
        for (int c = 0; c < 4; ++c)
            flags.setBit(c, bits & (1 << c));
        top->setChannelFlags(flags);
        const auto expected = render(image, false);
        // Prove the restricted layer itself is accepted, not just the background.
        KisGpuMergeBatch::setEnabled(true);
        KisGpuMergeBatch batch;
        QVERIFY(batch.tryAdd(top->projectionLeaf(), image->projection(), changed));
        batch.flush();
        const auto actual = render(image, true);
        QPoint where;
        const float error = f16 ? maxHalfUlpDifference(expected, actual, &where, bounds.width())
                                : maxDifference(expected, actual, &where, bounds.width());
        if (error > (f16 ? 4.0f : 2e-5f)) {
            const int p = (where.y() * bounds.width() + where.x()) * 4;
            qInfo() << "channels" << bits << "CPU" << expected[p] << expected[p + 1] << expected[p + 2]
                    << expected[p + 3] << "GPU" << actual[p] << actual[p + 1] << actual[p + 2] << actual[p + 3];
        }
        QVERIFY2(
            error <= (f16 ? 4.0f : 2e-5f),
            qPrintable(
                QString("channels %1, difference %2 at %3,%4").arg(bits).arg(error).arg(where.x()).arg(where.y())));
        fillRandom(top->paintDevice(), changed, 100 + bits);
        top->setDirty(changed);
        image->waitForDone();
        const auto partial = readFloats(image->projection(), bounds);
        const auto refreshed = render(image, false);
        const float partialError = f16 ? maxHalfUlpDifference(refreshed, partial, &where, bounds.width())
                                       : maxDifference(refreshed, partial, &where, bounds.width());
        QVERIFY2(partialError <= (f16 ? 4.0f : 2e-5f),
                 qPrintable(QString("partial channels %1, difference %2").arg(bits).arg(partialError)));
    }
}

void KisGpuProjectionTest::testChannelFlagBoundaries_data()
{
    testLayerChannelFlags_data();
}

void KisGpuProjectionTest::testChannelFlagBoundaries()
{
    REQUIRE_GPU();
    QFETCH(QString, mode);
    QFETCH(bool, f16);
    const QRect bounds(0, 0, 64, 64);
    const auto *cs = rgbaFloat(f16);
    QVERIFY(cs);
    KisPaintDeviceSP src = new KisPaintDevice(cs), initial = new KisPaintDevice(cs);
    const float alphas[] = {0.0f, 0.000005f, 0.0019f, 0.0021f, 0.3f, 0.9979f, 0.9981f, 1.0f};
    const float colors[] = {-0.25f, 0.0f, 0.25f, 0.4985f, 0.4995f, 0.5f, 0.5005f, 0.5015f, 1.0f, 1.5f};
    for (int source = 0; source < 2; ++source) {
        QVector<float> data(64 * 64 * 4);
        for (int p = 0; p < 64 * 64; ++p) {
            for (int c = 0; c < 3; ++c) {
                data[4 * p + c] = colors[((source ? p / 64 : p / 640) + c * 3) % 10];
            }
            data[4 * p + 3] = alphas[(source ? p : p / 8) % 8];
        }
        auto device = source ? src : initial;
        if (f16) {
            QVector<qfloat16> half(data.size());
            qFloatToFloat16(half.data(), data.constData(), data.size());
            device->writeBytes(reinterpret_cast<const quint8 *>(half.constData()), bounds);
        } else {
            device->writeBytes(reinterpret_cast<const quint8 *>(data.constData()), bounds);
        }
    }
    KisGpuBlendOp op;
    QVERIFY(KisGpuProjectionCompositor::blendOpForCompositeOp(mode, &op));
    const auto initialPixels = readFloats(initial, bounds);
    for (quint32 bits = 0; bits < 16; ++bits) {
        QBitArray flags(4);
        for (int c = 0; c < 4; ++c)
            flags.setBit(c, bits & (1u << c));
        for (quint8 opacity : {quint8(0), quint8(179), quint8(255)}) {
            KisPaintDeviceSP cpu = new KisPaintDevice(*initial), gpu = new KisPaintDevice(*initial);
            KisPainter painter(cpu);
            painter.setCompositeOpId(mode);
            painter.setChannelFlags(flags);
            painter.setOpacityU8(opacity);
            painter.bitBlt(bounds.topLeft(), src, bounds);
            QString error;
            QVERIFY2(
                KisGpuProjectionCompositor::composite(gpu, bounds, {{src, op, opacity / 255.0f, false, bits}}, &error),
                qPrintable(error));
            const auto expected = readFloats(cpu, bounds);
            const auto actual = readFloats(gpu, bounds);
            // Include hidden RGB at alpha zero: restricted channels must
            // preserve/clear it exactly as the corresponding CPU operation does.
            for (size_t c = 0; c < expected.size(); ++c) {
                float reference = expected[c];
                const size_t alpha = (c / 4) * 4 + 3;
                if (mode == COMPOSITE_OVER && bits == 15 && c % 4 != 3 && expected[alpha] == 0.0f
                    && actual[alpha] == 0.0f) {
                    // Optimized CPU Over may copy source hidden RGB when a
                    // whole SIMD batch has zero destination alpha, but its
                    // scalar path leaves it alone. Check the GPU's scalar
                    // preservation rule independently of CPU SIMD grouping.
                    reference = initialPixels[c];
                }
                const float tolerance = f16 ? 4.0f * halfUlp(qMax(std::abs(reference), std::abs(actual[c]))) : 2e-5f;
                QVERIFY2(std::abs(reference - actual[c]) <= tolerance,
                         qPrintable(QString("channels %1 opacity %2 component %3 CPU %4 GPU %5")
                                        .arg(bits)
                                        .arg(opacity)
                                        .arg(c)
                                        .arg(expected[c])
                                        .arg(actual[c])));
            }
        }
    }
}

void KisGpuProjectionTest::testFullRefreshMatchesCpu()
{
    REQUIRE_GPU();

    QVector<KisPaintLayerSP> layers;
    KisImageSP image = createTestImage(false, &layers);

    const std::vector<float> expected = render(image, false);
    const quint64 before = KisGpuMergeBatch::gpuCompositeCount();
    const std::vector<float> actual = render(image, true);
    QVERIFY2(KisGpuMergeBatch::gpuCompositeCount() > before, "the GPU path was not used");

    QPoint where;
    const float difference = maxDifference(expected, actual, &where, image->width());
    QVERIFY2(difference <= 2e-5f,
             qPrintable(QStringLiteral("max difference %1 at (%2, %3)").arg(difference).arg(where.x()).arg(where.y())));
}

void KisGpuProjectionTest::testMemoryBudgetFallsBackToCpu()
{
    REQUIRE_GPU();
    auto *backend = KisGpuTileBackend::instance();
    backend->flush();
    backend->evictTiles(~quint64(0));
    const quint64 originalBudget = backend->memoryBudget();
    auto restore = qScopeGuard([&]() {
        backend->setMemoryBudgetForTesting(originalBudget);
    });
    // A merge needs source and destination tiles together. One slot forces
    // preparation to fail after allocating, exercising partial rollback.
    backend->setMemoryBudgetForTesting(backend->pool(16)->tileBytes());
    QVector<KisPaintLayerSP> layers;
    KisImageSP image = createTestImage(false, &layers);
    const auto expected = render(image, false);
    const quint64 before = KisGpuMergeBatch::gpuCompositeCount();
    const auto actual = render(image, true);
    QCOMPARE(KisGpuMergeBatch::gpuCompositeCount(), before);
    QPoint where;
    QCOMPARE(maxDifference(expected, actual, &where, image->width()), 0.0f);
    QVERIFY(backend->reservedTileBytes() <= backend->memoryBudget());
    QVERIFY(!backend->hasFailed());
    // Pressure is recoverable; the next refresh can accelerate again.
    backend->setMemoryBudgetForTesting(originalBudget);
    render(image, true);
    QVERIFY(KisGpuMergeBatch::gpuCompositeCount() > before);
}

void KisGpuProjectionTest::testBlendModesMatchCpu_data()
{
    QTest::addColumn<QString>("compositeOp");
    QTest::addColumn<bool>("alphaLocked");

    const QStringList ops = {COMPOSITE_OVER,
                             COMPOSITE_MULT,
                             COMPOSITE_SCREEN,
                             COMPOSITE_ADD,
                             COMPOSITE_LINEAR_DODGE,
                             COMPOSITE_SUBTRACT,
                             COMPOSITE_DARKEN,
                             COMPOSITE_LIGHTEN,
                             COMPOSITE_DIFF,
                             COMPOSITE_OVERLAY,
                             COMPOSITE_HARD_LIGHT,
                             COMPOSITE_EXCLUSION,
                             COMPOSITE_LINEAR_BURN,
                             COMPOSITE_LINEAR_LIGHT,
                             COMPOSITE_PIN_LIGHT,
                             COMPOSITE_SOFT_LIGHT_SVG,
                             COMPOSITE_SOFT_LIGHT_PHOTOSHOP,
                             COMPOSITE_DODGE,
                             COMPOSITE_BURN,
                             COMPOSITE_DIVIDE,
                             COMPOSITE_VIVID_LIGHT,
                             COMPOSITE_HARD_MIX,
                             COMPOSITE_HARD_MIX_PHOTOSHOP,
                             COMPOSITE_HARD_MIX_SOFTER_PHOTOSHOP,
                             COMPOSITE_GRAIN_MERGE,
                             COMPOSITE_GRAIN_EXTRACT,
                             COMPOSITE_NEGATION,
                             COMPOSITE_ALLANON,
                             COMPOSITE_HUE,
                             COMPOSITE_SATURATION,
                             COMPOSITE_COLOR,
                             COMPOSITE_LUMINIZE,
                             COMPOSITE_DARKER_COLOR,
                             COMPOSITE_LIGHTER_COLOR,
                             // phase 4.107: the other HSX modes
                             COMPOSITE_TINT,
                             COMPOSITE_INC_SATURATION,
                             COMPOSITE_DEC_SATURATION,
                             COMPOSITE_INC_LUMINOSITY,
                             COMPOSITE_DEC_LUMINOSITY,
                             COMPOSITE_COLOR_HSI,
                             COMPOSITE_HUE_HSI,
                             COMPOSITE_SATURATION_HSI,
                             COMPOSITE_INC_SATURATION_HSI,
                             COMPOSITE_DEC_SATURATION_HSI,
                             COMPOSITE_INTENSITY,
                             COMPOSITE_INC_INTENSITY,
                             COMPOSITE_DEC_INTENSITY,
                             COMPOSITE_COLOR_HSL,
                             COMPOSITE_HUE_HSL,
                             COMPOSITE_SATURATION_HSL,
                             COMPOSITE_INC_SATURATION_HSL,
                             COMPOSITE_DEC_SATURATION_HSL,
                             COMPOSITE_LIGHTNESS,
                             COMPOSITE_INC_LIGHTNESS,
                             COMPOSITE_DEC_LIGHTNESS,
                             COMPOSITE_COLOR_HSV,
                             COMPOSITE_HUE_HSV,
                             COMPOSITE_SATURATION_HSV,
                             COMPOSITE_INC_SATURATION_HSV,
                             COMPOSITE_DEC_SATURATION_HSV,
                             COMPOSITE_VALUE,
                             COMPOSITE_INC_VALUE,
                             COMPOSITE_DEC_VALUE};
    for (const QString &op : ops) {
        QTest::newRow(qPrintable(op)) << op << false;
        QTest::newRow(qPrintable(op + QStringLiteral(" alpha locked"))) << op << true;
    }
}

void KisGpuProjectionTest::testBlendModesMatchCpu()
{
    REQUIRE_GPU();
    QFETCH(QString, compositeOp);
    QFETCH(bool, alphaLocked);

    // Two layers over a semi-transparent base: every branch of the generic
    // ops (dst alpha 0, 1, partial; src alpha 0, 1, partial) and HDR values
    // (the source clamp policies) are exercised.
    const QRect bounds(0, 0, 300, 200);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(), "blend mode");
    KisNodeSP root = image->root();
    addPaintLayer(image, root, COMPOSITE_OVER, 255, 30, QRect(0, 0, 250, 200), true);
    KisPaintLayerSP layer = addPaintLayer(image, root, compositeOp, 179, 31, QRect(40, 20, 260, 180), true);
    if (alphaLocked) {
        QBitArray flags(4, true);
        flags.clearBit(3);
        layer->setChannelFlags(flags);
    }
    // Interleave the established and extended shader families. In particular,
    // Color Burn's precision constraints must not change Linear Light math.
    addPaintLayer(image, root, COMPOSITE_LINEAR_LIGHT, 173, 33, QRect(80, 0, 180, 180));
    addPaintLayer(image, root, compositeOp, 255, 32, QRect(100, 0, 200, 120));

    const std::vector<float> expected = render(image, false);
    const quint64 before = KisGpuMergeBatch::gpuCompositeCount();
    const std::vector<float> actual = render(image, true);
    QVERIFY2(KisGpuMergeBatch::gpuCompositeCount() > before, "the GPU path was not used");

    QPoint where;
    const float difference = maxDifference(expected, actual, &where, image->width());
    QVERIFY2(difference <= 2e-5f,
             qPrintable(QStringLiteral("max difference %1 at (%2, %3)").arg(difference).arg(where.x()).arg(where.y())));
}

void KisGpuProjectionTest::testExtendedBlendBoundaries_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("f16");
    QTest::addColumn<bool>("locked");
    for (const auto &mode : {COMPOSITE_LINEAR_BURN,
                             COMPOSITE_LINEAR_LIGHT,
                             COMPOSITE_PIN_LIGHT,
                             COMPOSITE_SOFT_LIGHT_SVG,
                             COMPOSITE_SOFT_LIGHT_PHOTOSHOP,
                             COMPOSITE_DODGE,
                             COMPOSITE_BURN,
                             COMPOSITE_DIVIDE,
                             COMPOSITE_VIVID_LIGHT,
                             COMPOSITE_HARD_MIX,
                             COMPOSITE_HARD_MIX_PHOTOSHOP,
                             COMPOSITE_HARD_MIX_SOFTER_PHOTOSHOP,
                             COMPOSITE_GRAIN_MERGE,
                             COMPOSITE_GRAIN_EXTRACT,
                             COMPOSITE_NEGATION,
                             COMPOSITE_ALLANON,
                             COMPOSITE_HUE,
                             COMPOSITE_SATURATION,
                             COMPOSITE_COLOR,
                             COMPOSITE_LUMINIZE,
                             COMPOSITE_DARKER_COLOR,
                             COMPOSITE_LIGHTER_COLOR,
                             // phase 4.107: the other HSX modes
                             COMPOSITE_TINT,
                             COMPOSITE_INC_SATURATION,
                             COMPOSITE_DEC_SATURATION,
                             COMPOSITE_INC_LUMINOSITY,
                             COMPOSITE_DEC_LUMINOSITY,
                             COMPOSITE_COLOR_HSI,
                             COMPOSITE_HUE_HSI,
                             COMPOSITE_SATURATION_HSI,
                             COMPOSITE_INC_SATURATION_HSI,
                             COMPOSITE_DEC_SATURATION_HSI,
                             COMPOSITE_INTENSITY,
                             COMPOSITE_INC_INTENSITY,
                             COMPOSITE_DEC_INTENSITY,
                             COMPOSITE_COLOR_HSL,
                             COMPOSITE_HUE_HSL,
                             COMPOSITE_SATURATION_HSL,
                             COMPOSITE_INC_SATURATION_HSL,
                             COMPOSITE_DEC_SATURATION_HSL,
                             COMPOSITE_LIGHTNESS,
                             COMPOSITE_INC_LIGHTNESS,
                             COMPOSITE_DEC_LIGHTNESS,
                             COMPOSITE_COLOR_HSV,
                             COMPOSITE_HUE_HSV,
                             COMPOSITE_SATURATION_HSV,
                             COMPOSITE_INC_SATURATION_HSV,
                             COMPOSITE_DEC_SATURATION_HSV,
                             COMPOSITE_VALUE,
                             COMPOSITE_INC_VALUE,
                             COMPOSITE_DEC_VALUE})
        for (bool f16 : {false, true})
            for (bool locked : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-f16%2-locked%3").arg(mode).arg(f16).arg(locked)))
                    << mode << f16 << locked;
}

void KisGpuProjectionTest::testExtendedBlendBoundaries()
{
    REQUIRE_GPU();
    QFETCH(QString, mode);
    QFETCH(bool, f16);
    QFETCH(bool, locked);
    const QRect bounds(0, 0, 64, 64);
    const auto *cs = rgbaFloat(f16);
    QVERIFY(cs);
    KisPaintDeviceSP src = new KisPaintDevice(cs), cpu = new KisPaintDevice(cs);
    // Cross the exact branch/clamp boundaries, including HDR and negative
    // source AND destination values, with transparent, partial and opaque alpha.
    const float colors[] = {-2.0f,
                            -0.25f,
                            0.0f,
                            0.000001f,
                            0.2499f,
                            0.25f,
                            0.2501f,
                            0.499f,
                            0.5f,
                            0.501f,
                            0.75f,
                            0.99999f,
                            1.0f,
                            1.001f,
                            1.5f,
                            4.0f};
    constexpr int colorCount = sizeof(colors) / sizeof(colors[0]);
    const float alphas[] = {0.0f, 0.000005f, 0.3f, 1.0f};
    for (int source = 0; source < 2; ++source) {
        QVector<float> data(64 * 64 * 4);
        for (int p = 0; p < 64 * 64; ++p) {
            for (int c = 0; c < 3; ++c) {
                // Include neutral colors (zero chroma), not only RGB permutations.
                const int offset = p % 7 == 0 ? 0 : c;
                data[4 * p + c] = colors[((source ? p : p / colorCount) + offset) % colorCount];
            }
            data[4 * p + 3] = alphas[(source ? p / (colorCount * colorCount) : p / (4 * colorCount * colorCount)) % 4];
        }
        auto device = source ? src : cpu;
        if (f16) {
            QVector<qfloat16> half(data.size());
            qFloatToFloat16(half.data(), data.constData(), data.size());
            device->writeBytes(reinterpret_cast<const quint8 *>(half.constData()), bounds);
        } else {
            device->writeBytes(reinterpret_cast<const quint8 *>(data.constData()), bounds);
        }
    }
    KisPaintDeviceSP gpu = new KisPaintDevice(*cpu);
    KisGpuBlendOp op;
    QVERIFY(KisGpuProjectionCompositor::blendOpForCompositeOp(mode, &op));
    KisPainter painter(cpu);
    painter.setCompositeOpId(mode);
    QBitArray flags(4, true);
    flags.setBit(3, !locked);
    painter.setChannelFlags(flags);
    for (quint8 opacity : {quint8(255), quint8(179), quint8(0)}) {
        painter.setOpacityU8(opacity);
        painter.bitBlt(bounds.topLeft(), src, bounds);
        // Direct success is required: a GPU background followed by CPU fallback
        // for this mode cannot accidentally pass the image-level counter check.
        QString error;
        QVERIFY2(KisGpuProjectionCompositor::composite(gpu, bounds, {{src, op, opacity / 255.0f, locked}}, &error),
                 qPrintable(error));
        const auto expected = readFloats(cpu, bounds);
        const auto actual = readFloats(gpu, bounds);
        QPoint where;
        const float delta =
            f16 ? maxHalfUlpDifference(expected, actual, &where, 64) : maxDifference(expected, actual, &where, 64);
        QVERIFY2(delta <= (f16 ? 4.0f : 2e-5f),
                 qPrintable(QStringLiteral("difference %1 at (%2, %3), opacity %4")
                                .arg(delta)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(opacity)));
    }
}

void KisGpuProjectionTest::testPartialUpdateMatchesCpu()
{
    REQUIRE_GPU();
    QVector<KisPaintLayerSP> layers;
    KisImageSP image = createTestImage(false, &layers);
    render(image, false);

    // Paint into two layers and update unaligned rects, first on the CPU path.
    const QRect dirty1(123, 77, 150, 90);
    const QRect dirty2(400, 333, 37, 41);
    fillRandom(layers[2]->paintDevice(), dirty1, 100);
    fillRandom(layers[0]->paintDevice(), dirty2, 101);

    KisGpuMergeBatch::setEnabled(false);
    layers[2]->setDirty(dirty1);
    layers[0]->setDirty(dirty2);
    image->waitForDone();
    const std::vector<float> expected = readFloats(image->projection(), image->bounds());

    // The same updates again, on the GPU path.
    const quint64 before = KisGpuMergeBatch::gpuCompositeCount();
    KisGpuMergeBatch::setEnabled(true);
    layers[2]->setDirty(dirty1);
    layers[0]->setDirty(dirty2);
    image->waitForDone();
    const std::vector<float> actual = readFloats(image->projection(), image->bounds());
    QVERIFY(KisGpuMergeBatch::gpuCompositeCount() > before);

    QPoint where;
    const float difference = maxDifference(expected, actual, &where, image->width());
    QVERIFY2(difference <= 2e-5f,
             qPrintable(QStringLiteral("max difference %1 at (%2, %3)").arg(difference).arg(where.x()).arg(where.y())));
}

void KisGpuProjectionTest::testF16Image()
{
    REQUIRE_GPU();
    QVector<KisPaintLayerSP> layers;
    KisImageSP image = createTestImage(true, &layers);

    const std::vector<float> expected = render(image, false);
    const quint64 before = KisGpuMergeBatch::gpuCompositeCount();
    const std::vector<float> actual = render(image, true);
    QVERIFY(KisGpuMergeBatch::gpuCompositeCount() > before);

    // Both round to half after every layer; within a layer the GPU computes
    // in float32 and Krita partly in half and double. Defined tolerance: 4 half
    // ULPs at max(|value|, 1), i.e. 4/1024 for normalized values
    // (docs/agent/gpu-engine.md).
    QPoint where;
    const float ulps = maxHalfUlpDifference(expected, actual, &where, image->width());
    qInfo() << "F16 max difference in half ULPs:" << ulps << "at" << where;
    QVERIFY2(
        ulps <= 4.0f,
        qPrintable(QStringLiteral("max difference %1 half ULPs at (%2, %3)").arg(ulps).arg(where.x()).arg(where.y())));
}

void KisGpuProjectionTest::testProjectionStaysOnGpu()
{
    REQUIRE_GPU();
    // Only GPU-composable layers: nothing in the merge needs the projection
    // on the CPU. (Adjustment layers, CPU-only blend modes, and unaligned
    // layers read it back during the merge, as they must.)
    const QRect bounds(0, 0, 320, 256);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(), "resident");
    addPaintLayer(image, image->root(), COMPOSITE_OVER, 255, 40, bounds);
    addPaintLayer(image, image->root(), COMPOSITE_MULT, 200, 41, QRect(32, 16, 200, 200));
    KisGroupLayerSP group = new KisGroupLayer(image, "group", 255);
    image->addNode(group, image->root());
    addPaintLayer(image, group, COMPOSITE_SCREEN, 255, 42, QRect(64, 64, 256, 192));
    KisGpuMergeBatch::setEnabled(true);
    image->refreshGraphAsync();
    image->waitForDone(); // no CPU read of the projection here (render() reads it)

    // Phase 3: the GPU-composited projection tiles are not read back by the
    // merge; they stay GPU-valid and CPU-stale until a CPU reader needs them.
    KisPaintDeviceSP projection = image->projection();
    KisTileSP tile = projection->dataManager()->getTile(2, 1, false);
    KisTileGpuState *state = tile->tileData()->gpuState();
    QVERIFY(state);
    QVERIFY(state->gpuValid());
    QVERIFY(!state->cpuValid());

    // A CPU read downloads it and gets the same pixels as the CPU path.
    const std::vector<float> gpu = readFloats(projection, image->bounds());
    QVERIFY(state->cpuValid());
    const std::vector<float> cpu = render(image, false);
    QPoint where;
    QVERIFY(maxDifference(cpu, gpu, &where, image->width()) <= 2e-5f);
}

void KisGpuProjectionTest::testConcurrentUpdatesSharingTilesMatchCpu()
{
    REQUIRE_GPU();
    QVector<KisPaintLayerSP> layers;
    KisImageSP image = createTestImage(false, &layers);
    render(image, false);

    // Many small, disjoint, unaligned rects that share 64x64 tiles, issued
    // without waiting so that the scheduler may run them concurrently.
    QVector<QRect> rects;
    for (int y = 3; y < 480; y += 37) {
        for (int x = 5; x < 680; x += 29) {
            rects << QRect(x, y, 21, 30);
        }
    }

    auto updateAll = [&](bool gpu, quint32 seed) {
        KisGpuMergeBatch::setEnabled(gpu);
        for (int i = 0; i < rects.size(); i++) {
            fillRandom(layers[i % 3]->paintDevice(), rects[i], seed + i);
        }
        for (int i = 0; i < rects.size(); i++) {
            layers[i % 3]->setDirty(rects[i]);
        }
        image->waitForDone();
        return readFloats(image->projection(), image->bounds());
    };

    // Same content both times: the second pass repaints identical pixels.
    const std::vector<float> expected = updateAll(false, 500);
    const quint64 before = KisGpuMergeBatch::gpuCompositeCount();
    const std::vector<float> actual = updateAll(true, 500);
    QVERIFY(KisGpuMergeBatch::gpuCompositeCount() > before);

    QPoint where;
    const float difference = maxDifference(expected, actual, &where, image->width());
    QVERIFY2(difference <= 2e-5f,
             qPrintable(QStringLiteral("max difference %1 at (%2, %3)").arg(difference).arg(where.x()).arg(where.y())));
}

void KisGpuProjectionTest::benchmarkRefresh()
{
    REQUIRE_GPU();
    const int size =
        qEnvironmentVariableIsSet("KRITA_GPU_BENCH_SIZE") ? qEnvironmentVariableIntValue("KRITA_GPU_BENCH_SIZE") : 4096;
    const int layerCount = qEnvironmentVariableIsSet("KRITA_GPU_BENCH_LAYERS")
        ? qEnvironmentVariableIntValue("KRITA_GPU_BENCH_LAYERS")
        : 16;
    QVERIFY(size >= 576 && layerCount > 0);
    const int repeats = qBound(1,
                               qEnvironmentVariableIsSet("KRITA_GPU_BENCH_REPEATS")
                                   ? qEnvironmentVariableIntValue("KRITA_GPU_BENCH_REPEATS")
                                   : 3,
                               20);
    const QRect bounds(0, 0, size, size);

    KisImageSP image = new KisImage(nullptr, size, size, rgbaFloat(), "gpu projection benchmark");
    const QStringList ops = {COMPOSITE_OVER, COMPOSITE_MULT, COMPOSITE_SCREEN, COMPOSITE_OVERLAY};
    QVector<KisPaintLayerSP> layers;
    for (int i = 0; i < layerCount; i++) {
        layers << addPaintLayer(image, image->root(), ops[i % ops.size()], 220, 200 + i, bounds);
    }

    auto timeRefresh = [&](bool gpu) {
        KisGpuMergeBatch::setEnabled(gpu);
        auto &context = KisGpuTileBackend::instance()->context();
        context.waitIdle();
        const quint64 before = context.completedValue();
        QElapsedTimer timer;
        timer.start();
        image->refreshGraphAsync();
        image->waitForDone();
        context.waitIdle();
        const double elapsed = timer.nsecsElapsed() / 1.0e6;
        qInfo() << "refresh sample" << (gpu ? "GPU" : "CPU") << elapsed << "ms, submissions"
                << context.completedValue() - before << "reserved MiB"
                << KisGpuTileBackend::instance()->reservedTileBytes() / (1024 * 1024);
        return elapsed;
    };
    std::vector<float> pixels(size_t(size) * size * 4);
    auto timeCanvasRead = [&]() {
        QElapsedTimer timer;
        timer.start();
        image->projection()->readBytes(reinterpret_cast<quint8 *>(pixels.data()), bounds);
        return timer.nsecsElapsed() / 1.0e6;
    };
    auto timeStrokeUpdate = [&](bool gpu) {
        KisGpuMergeBatch::setEnabled(gpu);
        const QRect dab(size / 2, size / 2, 256, 256);
        std::vector<float> canvas(size_t(dab.width() + 32 * 20) * dab.height() * 4);
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 20; i++) {
            layers[layerCount / 2]->setDirty(dab.translated(i * 16, 0));
            image->waitForDone();
            // What the canvas does next: read the updated projection rect.
            const QRect updated = dab.translated(i * 16, 0);
            image->projection()->readBytes(reinterpret_cast<quint8 *>(canvas.data()), updated);
        }
        return timer.nsecsElapsed() / 1.0e6 / 20;
    };

    auto medianRefresh = [&](bool gpu) {
        std::vector<double> samples;
        for (int i = 0; i < repeats; ++i)
            samples.push_back(timeRefresh(gpu));
        std::sort(samples.begin(), samples.end());
        return (samples[(samples.size() - 1) / 2] + samples[samples.size() / 2]) / 2;
    };
    const double gpuInitial = timeRefresh(true);
    timeRefresh(false); // separate GPU-to-CPU transition from resident CPU samples
    const double cpuFull = medianRefresh(false);
    timeCanvasRead();
    const std::vector<float> expected = pixels;
    const quint64 beforeGpu = KisGpuMergeBatch::gpuCompositeCount();
    const double gpuAfterCpu = timeRefresh(true);
    const double gpuFull = medianRefresh(true);
    QVERIFY(KisGpuMergeBatch::gpuCompositeCount() > beforeGpu);
    const double gpuFullRead = timeCanvasRead();
    QPoint where;
    QVERIFY(maxDifference(expected, pixels, &where, size) <= 2e-5f);
    const double cpuStroke = timeStrokeUpdate(false);
    const double gpuStroke = timeStrokeUpdate(true);

    qInfo().noquote() << QStringLiteral("Image %1x%1 RGBA F32, %2 layers:").arg(size).arg(layerCount);
    qInfo() << "Initial GPU refresh" << gpuInitial << "ms; GPU after CPU refresh" << gpuAfterCpu
            << "ms; resident samples per path" << repeats;
    qInfo().noquote() << QStringLiteral(
                             "  resident full refresh (median): CPU %1 ms, GPU %2 ms (+ %3 ms full readBytes)")
                             .arg(cpuFull, 0, 'f', 1)
                             .arg(gpuFull, 0, 'f', 1)
                             .arg(gpuFullRead, 0, 'f', 1);
    qInfo().noquote() << QStringLiteral("  256px dab update + canvas read:  CPU %1 ms, GPU %2 ms")
                             .arg(cpuStroke, 0, 'f', 2)
                             .arg(gpuStroke, 0, 'f', 2);
}

void KisGpuProjectionTest::testFloatGroupInIntegerImageStaysOnCpu()
{
    REQUIRE_GPU();
    // An RGBA F32 group inside an RGBA8 image (KRA files can contain this).
    // KisUpdaterContext only keeps merge jobs that share a 64x64 tile apart
    // in RGBA float images, so such a group must not composite on the GPU.
    const QRect bounds(0, 0, 256, 192);
    KisImageSP image =
        new KisImage(nullptr, bounds.width(), bounds.height(), KoColorSpaceRegistry::instance()->rgb8(), "mixed");
    KisGroupLayerSP group = new KisGroupLayer(image, "float group", 255, rgbaFloat());
    image->addNode(group, image->root());
    QVERIFY(*group->colorSpace() == *rgbaFloat());
    for (quint32 seed : {50u, 51u}) {
        KisPaintLayerSP layer = new KisPaintLayer(image, QStringLiteral("layer %1").arg(seed), 255, rgbaFloat());
        fillRandom(layer->paintDevice(), bounds, seed);
        image->addNode(layer, group);
    }

    KisGpuMergeBatch::setEnabled(true);
    const quint64 before = KisGpuMergeBatch::gpuCompositeCount();
    image->refreshGraphAsync();
    image->waitForDone();
    QCOMPARE(KisGpuMergeBatch::gpuCompositeCount(), before);
    QVERIFY(!KisGpuMergeBatch::mayCompositeOnGpu(group));
}

void KisGpuProjectionTest::testConvertedDocumentUsesGpu()
{
    REQUIRE_GPU();
    KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();

    // Conversion targets (phase 3.3, KisGpuEngineSettings).
    const KoColorSpace *rgb8 = registry->rgb8();
    const KoColorSpace *target = KisGpuEngineSettings::conversionTarget(rgb8);
    QVERIFY(target);
    QVERIFY(KisGpuEngineSettings::isGpuColorSpace(target));
    QCOMPARE(target->colorDepthId(), Float32BitsColorDepthID);
    QCOMPARE(target->profile()->name(), rgb8->profile()->name()); // same profile: same appearance
    QVERIFY(!KisGpuEngineSettings::conversionTarget(rgbaFloat()));
    QVERIFY(!KisGpuEngineSettings::conversionTarget(rgbaFloat(true)));
    const KoColorSpace *labTarget = KisGpuEngineSettings::conversionTarget(registry->lab16());
    QVERIFY(labTarget && KisGpuEngineSettings::isGpuColorSpace(labTarget));

    // An RGBA8 document composites on the CPU; after the conversion that
    // KisGpuEngineUi offers on opening, the same document uses the GPU.
    const QRect bounds(0, 0, 256, 192);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgb8, "converted");
    for (int i = 0; i < 2; i++) {
        KisPaintLayerSP layer = new KisPaintLayer(image, QStringLiteral("layer %1").arg(i), 200);
        layer->paintDevice()->fill(QRect(i * 40, i * 30, 160, 120), KoColor(i ? Qt::red : Qt::blue, rgb8));
        image->addNode(layer, image->root());
    }
    KisGpuMergeBatch::setEnabled(true);
    quint64 before = KisGpuMergeBatch::gpuCompositeCount();
    image->refreshGraphAsync();
    image->waitForDone();
    QCOMPARE(KisGpuMergeBatch::gpuCompositeCount(), before);
    const QImage original = image->projection()->convertToQImage(nullptr, bounds);

    image->convertImageColorSpace(target,
                                  KoColorConversionTransformation::internalRenderingIntent(),
                                  KoColorConversionTransformation::internalConversionFlags());
    image->waitForDone();
    QVERIFY(*image->colorSpace() == *target);
    KisNodeSP node = image->root()->firstChild();
    while (node) {
        QVERIFY2(*node->colorSpace() == *target, qPrintable(node->name()));
        node = node->nextSibling();
    }

    before = KisGpuMergeBatch::gpuCompositeCount();
    image->refreshGraphAsync();
    image->waitForDone();
    QVERIFY(KisGpuMergeBatch::gpuCompositeCount() > before);
    // The converted document looks the same (8-bit display of the result).
    QCOMPARE(image->projection()->convertToQImage(nullptr, bounds), original);
}

SIMPLE_TEST_MAIN(KisGpuProjectionTest)

#include "KisGpuProjectionTest.moc"
