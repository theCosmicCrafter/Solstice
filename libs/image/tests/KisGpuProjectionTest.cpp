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
#include <KoColorTransformation.h>
#include <KoCompositeOpRegistry.h>

#include "KisImageResolutionProxy.h"
#include "filter/kis_color_transformation_filter.h"
#include "filter/kis_filter.h"
#include "filter/kis_filter_configuration.h"
#include "filter/kis_filter_registry.h"
#include "gpu/KisGpuColorTransform.h"
#include "gpu/KisGpuEngineSettings.h"
#include "gpu/KisGpuMergeBatch.h"
#include "gpu/KisGpuProjectionCompositor.h"
#include "gpu/KisGpuTileAccess.h"
#include "gpu/KisGpuTileBackend.h"
#include "kis_abstract_projection_plane.h"
#include "kis_adjustment_layer.h"
#include "kis_clone_layer.h"
#include "kis_datamanager.h"
#include "kis_default_bounds.h"
#include "kis_filter_mask.h"
#include "kis_group_layer.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_painter.h"
#include "kis_pixel_selection.h"
#include "kis_psd_layer_style.h"
#include "kis_selection.h"
#include "kis_transparency_mask.h"
#include "layerstyles/kis_layer_style_projection_plane.h"
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
    void testClipStackMatchesCpu();
    void testBlendModesMatchCpu_data();
    void testBlendModesMatchCpu();
    void testExtendedBlendBoundaries_data();
    void testExtendedBlendBoundaries();
    void testLayerChannelFlags_data();
    void testLayerChannelFlags();
    void testChannelFlagBoundaries_data();
    void testChannelFlagBoundaries();
    void testPartialUpdateMatchesCpu();
    void testUnalignedUpdateDownloadsNothing_data();
    void testUnalignedUpdateDownloadsNothing();
    void testAdjustmentLayerFirstMatchesCpu();
    void testF16Image();
    void testProjectionStaysOnGpu();
    void testConcurrentUpdatesSharingTilesMatchCpu();
    void testMemoryBudgetFallsBackToCpu();
    void testFloatGroupInIntegerImageStaysOnCpu();
    void testConvertedDocumentUsesGpu_data();
    void testConvertedDocumentUsesGpu();
    void testShiftedCopyMatchesCpu_data();
    void testShiftedCopyMatchesCpu();
    void testCloneLayerMatchesCpu_data();
    void testCloneLayerMatchesCpu();
    void testMovedLayersMatchCpu_data();
    void testMovedLayersMatchCpu();
    void testMovedGroupMatchesCpu_data();
    void testMovedGroupMatchesCpu();
    void testTransparencyMaskMatchesCpu_data();
    void testTransparencyMaskMatchesCpu();
    void testStrokeStyleMatchesCpu_data();
    void testStrokeStyleMatchesCpu();
    void testFilterLayerMatchesCpu_data();
    void testFilterLayerMatchesCpu();
    void testFilterMaskMatchesCpu_data();
    void testFilterMaskMatchesCpu();
    void testColorReferenceTables();
    void testOffGridMaskedLayerConcurrentUpdates_data();
    void testOffGridMaskedLayerConcurrentUpdates();
    void testCopyModeMatchesCpu_data();
    void testCopyModeMatchesCpu();
    void testColorTransformTables();
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
// The same NaN or infinity on both sides: some modes (e.g. pow() of a
// negative HDR channel) produce them on the CPU too (phase 4.108).
bool sameNonFinite(float a, float b)
{
    return (std::isnan(a) && std::isnan(b)) || (std::isinf(a) && a == b);
}

float maxDifference(const std::vector<float> &a, const std::vector<float> &b, QPoint *where, int width)
{
    float worst = 0.0f;
    for (size_t pixel = 0; pixel < a.size() / 4; pixel++) {
        const bool transparent = a[pixel * 4 + 3] == 0.0f && b[pixel * 4 + 3] == 0.0f;
        for (size_t c = transparent ? 3 : 0; c < 4; c++) {
            if (sameNonFinite(a[pixel * 4 + c], b[pixel * 4 + c]))
                continue;
            // relative above 1: HDR modes reach FLT_MAX (phase 4.108)
            const float difference = std::abs(a[pixel * 4 + c] - b[pixel * 4 + c])
                / qMax(1.0f, qMax(std::abs(a[pixel * 4 + c]), std::abs(b[pixel * 4 + c])));
            if (difference > worst || std::isnan(difference)) {
                worst = std::isnan(difference) ? INFINITY : difference;
                *where = QPoint(int(pixel % width), int(pixel / width));
            }
        }
    }
    return worst;
}

// Both results at a pixel, for failure messages
QString pixelReport(const std::vector<float> &expected, const std::vector<float> &actual, QPoint where, int width)
{
    const size_t i = size_t(where.y() * width + where.x()) * 4;
    QString text = QStringLiteral("cpu");
    for (int c = 0; c < 4; ++c)
        text += QLatin1Char(' ') + QString::number(double(expected[i + c]), 'g', 9);
    text += QStringLiteral(", gpu");
    for (int c = 0; c < 4; ++c)
        text += QLatin1Char(' ') + QString::number(double(actual[i + c]), 'g', 9);
    return text;
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
            if (sameNonFinite(x, y))
                continue;
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
 *    paint exclusion, moved by (10, 7)     (unaligned: GPU since phase 4.112)
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
             qPrintable(QStringLiteral("max difference %1 at (%2, %3): %4")
                            .arg(difference)
                            .arg(where.x())
                            .arg(where.y())
                            .arg(pixelReport(expected, actual, where, image->width()))));
}

void KisGpuProjectionTest::testClipStackMatchesCpu()
{
    REQUIRE_GPU();

    // Solstice: Clipping Mask. The base and its clipped layers composite
    // into the base's clip stack, which composites onto the root.
    KisImageSP image = new KisImage(0, 256, 192, rgbaFloat(), "clip stack");
    auto addLayer = [&image](const QString &name, const QRect &rect, quint32 seed, const QString &op, quint8 opacity) {
        KisPaintLayerSP layer = new KisPaintLayer(image, name, opacity, rgbaFloat());
        fillRandom(layer->paintDevice(), rect, seed);
        layer->setCompositeOpId(op);
        image->addNode(layer, image->root(), image->root()->lastChild());
        return layer;
    };
    addLayer("background", image->bounds(), 1, COMPOSITE_OVER, OPACITY_OPAQUE_U8);
    addLayer("base", QRect(20, 10, 150, 120), 2, COMPOSITE_MULT, 200);
    addLayer("clip1", QRect(70, 0, 180, 150), 3, COMPOSITE_OVER, OPACITY_OPAQUE_U8)->setClipped(true);
    addLayer("clip2", QRect(0, 60, 256, 50), 4, COMPOSITE_SCREEN, 128)->setClipped(true);
    addLayer("top", QRect(150, 100, 60, 60), 5, COMPOSITE_OVER, OPACITY_OPAQUE_U8);

    const std::vector<float> expected = render(image, false);
    const quint64 before = KisGpuMergeBatch::gpuCompositeCount();
    const std::vector<float> actual = render(image, true);
    QVERIFY2(KisGpuMergeBatch::gpuCompositeCount() > before, "the GPU path was not used");

    QPoint where;
    const float difference = maxDifference(expected, actual, &where, image->width());
    QVERIFY2(difference <= 2e-5f,
             qPrintable(QStringLiteral("max difference %1 at (%2, %3): %4")
                            .arg(difference)
                            .arg(where.x())
                            .arg(where.y())
                            .arg(pixelReport(expected, actual, where, image->width()))));
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
                             COMPOSITE_DEC_VALUE,
                             // phase 4.108
                             COMPOSITE_HARD_MIX_HDR,
                             COMPOSITE_DODGE_HDR,
                             COMPOSITE_VIVID_LIGHT_HDR,
                             COMPOSITE_HARD_OVERLAY,
                             COMPOSITE_HARD_OVERLAY_HDR,
                             COMPOSITE_GEOMETRIC_MEAN,
                             COMPOSITE_PARALLEL,
                             COMPOSITE_INTERPOLATION,
                             COMPOSITE_INTERPOLATIONB,
                             COMPOSITE_PENUMBRAA,
                             COMPOSITE_PENUMBRAB,
                             COMPOSITE_PENUMBRAC,
                             COMPOSITE_PENUMBRAD,
                             COMPOSITE_SOFT_LIGHT_IFS_ILLUSIONS,
                             COMPOSITE_SOFT_LIGHT_PEGTOP_DELPHI,
                             COMPOSITE_GAMMA_LIGHT,
                             COMPOSITE_GAMMA_ILLUMINATION,
                             COMPOSITE_FLAT_LIGHT,
                             COMPOSITE_PNORM_A,
                             COMPOSITE_PNORM_B,
                             COMPOSITE_SUPER_LIGHT,
                             COMPOSITE_TINT_IFS_ILLUSIONS,
                             COMPOSITE_FOG_LIGHTEN_IFS_ILLUSIONS,
                             COMPOSITE_EASY_DODGE,
                             COMPOSITE_GAMMA_DARK,
                             COMPOSITE_SHADE_IFS_ILLUSIONS,
                             COMPOSITE_FOG_DARKEN_IFS_ILLUSIONS,
                             COMPOSITE_EASY_BURN,
                             COMPOSITE_INVERSE_SUBTRACT,
                             COMPOSITE_MOD,
                             COMPOSITE_MODULO_SHIFT,
                             COMPOSITE_MODULO_SHIFT_CON,
                             COMPOSITE_ARC_TANGENT,
                             COMPOSITE_EQUIVALENCE,
                             COMPOSITE_ADDITIVE_SUBTRACTIVE,
                             COMPOSITE_REFLECT,
                             COMPOSITE_GLOW,
                             COMPOSITE_FREEZE,
                             COMPOSITE_HEAT,
                             COMPOSITE_GLEAT,
                             COMPOSITE_HELOW,
                             COMPOSITE_REEZE,
                             COMPOSITE_FRECT,
                             COMPOSITE_FHYRD,
                             COMPOSITE_LAMBERT_LIGHTING,
                             COMPOSITE_LAMBERT_LIGHTING_GAMMA_2_2,
                             COMPOSITE_TANGENT_NORMALMAP,
                             COMPOSITE_COMBINE_NORMAL,
                             COMPOSITE_LUMINOSITY_SAI,
                             COMPOSITE_BEHIND,
                             COMPOSITE_GREATER,
                             COMPOSITE_COPY_RED,
                             COMPOSITE_COPY_GREEN,
                             COMPOSITE_COPY_BLUE};
    for (const QString &op : ops) {
        // Luminosity SAI adds to the destination colour without normalizing,
        // so it shows the unspecified colour of transparent pixels the lower
        // layers leave; testExtendedBlendBoundaries covers it.
        if (op == COMPOSITE_LUMINOSITY_SAI)
            continue;
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

    // Glow, Heat and their combinations divide by 1 - dst or dst, and
    // Greater weights alpha with exp(-40 x): they amplify the one-ulp
    // differences the lower layers already have (2e-5 below) about 1e3
    // times. testExtendedBlendBoundaries holds them to 2e-5 on exact inputs.
    const QStringList amplifying{COMPOSITE_GLOW,
                                 COMPOSITE_HEAT,
                                 COMPOSITE_REFLECT,
                                 COMPOSITE_FREEZE,
                                 COMPOSITE_GLEAT,
                                 COMPOSITE_HELOW,
                                 COMPOSITE_REEZE,
                                 COMPOSITE_FRECT,
                                 COMPOSITE_FHYRD,
                                 COMPOSITE_GREATER};
    const float tolerance = amplifying.contains(compositeOp) ? 1e-2f : 2e-5f;
    QPoint where;
    const float difference = maxDifference(expected, actual, &where, image->width());
    QVERIFY2(difference <= tolerance,
             qPrintable(QStringLiteral("max difference %1 at (%2, %3): %4")
                            .arg(difference)
                            .arg(where.x())
                            .arg(where.y())
                            .arg(pixelReport(expected, actual, where, image->width()))));
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
                             COMPOSITE_DEC_VALUE,
                             // phase 4.108
                             COMPOSITE_HARD_MIX_HDR,
                             COMPOSITE_DODGE_HDR,
                             COMPOSITE_VIVID_LIGHT_HDR,
                             COMPOSITE_HARD_OVERLAY,
                             COMPOSITE_HARD_OVERLAY_HDR,
                             COMPOSITE_GEOMETRIC_MEAN,
                             COMPOSITE_PARALLEL,
                             COMPOSITE_INTERPOLATION,
                             COMPOSITE_INTERPOLATIONB,
                             COMPOSITE_PENUMBRAA,
                             COMPOSITE_PENUMBRAB,
                             COMPOSITE_PENUMBRAC,
                             COMPOSITE_PENUMBRAD,
                             COMPOSITE_SOFT_LIGHT_IFS_ILLUSIONS,
                             COMPOSITE_SOFT_LIGHT_PEGTOP_DELPHI,
                             COMPOSITE_GAMMA_LIGHT,
                             COMPOSITE_GAMMA_ILLUMINATION,
                             COMPOSITE_FLAT_LIGHT,
                             COMPOSITE_PNORM_A,
                             COMPOSITE_PNORM_B,
                             COMPOSITE_SUPER_LIGHT,
                             COMPOSITE_TINT_IFS_ILLUSIONS,
                             COMPOSITE_FOG_LIGHTEN_IFS_ILLUSIONS,
                             COMPOSITE_EASY_DODGE,
                             COMPOSITE_GAMMA_DARK,
                             COMPOSITE_SHADE_IFS_ILLUSIONS,
                             COMPOSITE_FOG_DARKEN_IFS_ILLUSIONS,
                             COMPOSITE_EASY_BURN,
                             COMPOSITE_INVERSE_SUBTRACT,
                             COMPOSITE_MOD,
                             COMPOSITE_MODULO_SHIFT,
                             COMPOSITE_MODULO_SHIFT_CON,
                             COMPOSITE_ARC_TANGENT,
                             COMPOSITE_EQUIVALENCE,
                             COMPOSITE_ADDITIVE_SUBTRACTIVE,
                             COMPOSITE_REFLECT,
                             COMPOSITE_GLOW,
                             COMPOSITE_FREEZE,
                             COMPOSITE_HEAT,
                             COMPOSITE_GLEAT,
                             COMPOSITE_HELOW,
                             COMPOSITE_REEZE,
                             COMPOSITE_FRECT,
                             COMPOSITE_FHYRD,
                             COMPOSITE_LAMBERT_LIGHTING,
                             COMPOSITE_LAMBERT_LIGHTING_GAMMA_2_2,
                             COMPOSITE_TANGENT_NORMALMAP,
                             COMPOSITE_COMBINE_NORMAL,
                             COMPOSITE_LUMINOSITY_SAI,
                             COMPOSITE_BEHIND,
                             COMPOSITE_GREATER,
                             COMPOSITE_COPY_RED,
                             COMPOSITE_COPY_GREEN,
                             COMPOSITE_COPY_BLUE})
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
        const auto before = readFloats(cpu, bounds);
        const auto gpuBefore = readFloats(gpu, bounds);
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
        // The inputs and both results at the worst pixel, for diagnosis
        const auto sourcePixels = readFloats(src, bounds);
        auto pixel = [&where](const std::vector<float> &values) {
            const size_t i = size_t(where.y() * 64 + where.x()) * 4;
            return QStringLiteral("(%1 %2 %3 %4)")
                .arg(double(values[i]), 0, 'g', 9)
                .arg(double(values[i + 1]), 0, 'g', 9)
                .arg(double(values[i + 2]), 0, 'g', 9)
                .arg(double(values[i + 3]), 0, 'g', 9);
        };
        QVERIFY2(
            delta <= (f16 ? 4.0f : 2e-5f),
            qPrintable(QStringLiteral("difference %1 at (%2, %3), opacity %4; src %5 dst %6 (gpu %7) cpu %8 gpu %9")
                           .arg(delta)
                           .arg(where.x())
                           .arg(where.y())
                           .arg(opacity)
                           .arg(pixel(sourcePixels), pixel(before), pixel(gpuBefore), pixel(expected), pixel(actual))));
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
             qPrintable(QStringLiteral("max difference %1 at (%2, %3): %4")
                            .arg(difference)
                            .arg(where.x())
                            .arg(where.y())
                            .arg(pixelReport(expected, actual, where, image->width()))));
}

void KisGpuProjectionTest::testUnalignedUpdateDownloadsNothing_data()
{
    QTest::addColumn<bool>("f16");
    QTest::newRow("f32") << false;
    QTest::newRow("f16") << true;
}

void KisGpuProjectionTest::testUnalignedUpdateDownloadsNothing()
{
    REQUIRE_GPU();
    QFETCH(bool, f16);
    // Phase 4.111: KisAsyncMerger::setupProjection() clears the dirty rect of
    // the root and the group projection with the first GPU composite. A CPU
    // clear of an unaligned rect downloaded its partly covered border tiles.
    const QRect bounds(0, 0, 700, 500);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(f16), "unaligned");
    const float background[4] = {0.2f, 0.3f, 0.4f, 1.0f};
    KoColor backgroundColor(rgbaFloat());
    std::memcpy(backgroundColor.data(), background, sizeof(background));
    backgroundColor.convertTo(rgbaFloat(f16));
    image->setDefaultProjectionColor(backgroundColor); // not transparent: the clear fills it
    addPaintLayer(image, image->root(), COMPOSITE_OVER, 200, 50, QRect(0, 0, 640, 448));
    KisGroupLayerSP group = new KisGroupLayer(image, "group", 230);
    image->addNode(group, image->root());
    KisPaintLayerSP painted = addPaintLayer(image, group, COMPOSITE_SCREEN, 255, 51, QRect(64, 32, 500, 400));
    addPaintLayer(image, group, COMPOSITE_MULT, 180, 52, QRect(100, 60, 300, 300));
    addPaintLayer(image, image->root(), COMPOSITE_OVER, 128, 53, QRect(200, 100, 256, 256));

    // A dab: unaligned rect, partly erased so the clear matters.
    const QRect dirty(123, 77, 150, 90);
    image->refreshGraphAsync();
    image->waitForDone();
    fillRandom(painted->paintDevice(), dirty, 54);
    painted->paintDevice()->clear(QRect(150, 100, 40, 30));

    KisGpuMergeBatch::setEnabled(false);
    painted->setDirty(dirty);
    image->waitForDone();
    const std::vector<float> expected = readFloats(image->projection(), image->bounds());

    KisGpuMergeBatch::setEnabled(true);
    image->refreshGraphAsync(); // the projections become GPU-resident
    image->waitForDone();
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    backend->flush();
    const quint64 downloadsBefore = backend->downloadedTileCount();
    const quint64 compositesBefore = KisGpuMergeBatch::gpuCompositeCount();
    painted->setDirty(dirty);
    image->waitForDone();
    backend->flush();
    QVERIFY(KisGpuMergeBatch::gpuCompositeCount() > compositesBefore);
    QCOMPARE(backend->downloadedTileCount(), downloadsBefore);

    // The partly covered border tiles stay GPU-valid and CPU-stale.
    for (KisPaintDeviceSP projection : {image->projection(), group->original()}) {
        KisTileSP tile = projection->dataManager()->getTile(dirty.left() / 64, dirty.top() / 64, false);
        KisTileGpuState *state = tile->tileData()->gpuState();
        QVERIFY(state);
        QVERIFY(state->gpuValid());
        QVERIFY(!state->cpuValid());
    }

    const std::vector<float> actual = readFloats(image->projection(), image->bounds());
    QVERIFY(backend->downloadedTileCount() > downloadsBefore);
    QPoint where;
    if (f16) {
        const float ulps = maxHalfUlpDifference(expected, actual, &where, image->width());
        QVERIFY2(
            ulps <= 4.0f,
            qPrintable(
                QStringLiteral("max difference %1 half ULPs at (%2, %3)").arg(ulps).arg(where.x()).arg(where.y())));
    } else {
        const float difference = maxDifference(expected, actual, &where, image->width());
        QVERIFY2(difference <= 2e-5f,
                 qPrintable(QStringLiteral("max difference %1 at (%2, %3): %4")
                                .arg(difference)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(pixelReport(expected, actual, where, image->width()))));
    }
}

void KisGpuProjectionTest::testAdjustmentLayerFirstMatchesCpu()
{
    REQUIRE_GPU();
    // The bottom leaf of the group filters the cleared rect on the CPU: its
    // clear must not wait for the first GPU composite (phase 4.111).
    const QRect bounds(0, 0, 400, 300);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(), "adjustment first");
    addPaintLayer(image, image->root(), COMPOSITE_OVER, 255, 55, bounds);
    KisGroupLayerSP group = new KisGroupLayer(image, "group", 255);
    image->addNode(group, image->root());
    KisFilterSP blur = KisFilterRegistry::instance()->value("blur");
    QVERIFY(blur);
    KisAdjustmentLayerSP adjustment = new KisAdjustmentLayer(
        image,
        "blur",
        blur->defaultConfiguration(KisGlobalResourcesInterface::instance())->cloneWithResourcesSnapshot(),
        nullptr);
    image->addNode(adjustment, group);
    addPaintLayer(image, group, COMPOSITE_OVER, 255, 56, QRect(20, 10, 350, 260));

    const QRect dirty(37, 41, 150, 100);
    render(image, false);
    adjustment->setDirty(dirty);
    image->waitForDone();
    const std::vector<float> expected = readFloats(image->projection(), image->bounds());

    render(image, true);
    adjustment->setDirty(dirty);
    image->waitForDone();
    const std::vector<float> actual = readFloats(image->projection(), image->bounds());

    QPoint where;
    const float difference = maxDifference(expected, actual, &where, image->width());
    QVERIFY2(difference <= 2e-5f,
             qPrintable(QStringLiteral("max difference %1 at (%2, %3): %4")
                            .arg(difference)
                            .arg(where.x())
                            .arg(where.y())
                            .arg(pixelReport(expected, actual, where, image->width()))));
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
             qPrintable(QStringLiteral("max difference %1 at (%2, %3): %4")
                            .arg(difference)
                            .arg(where.x())
                            .arg(where.y())
                            .arg(pixelReport(expected, actual, where, image->width()))));
}

void KisGpuProjectionTest::testShiftedCopyMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QPoint>("offset");
    QTest::addColumn<QPoint>("sourceDeviceOffset");
    for (bool f16 : {false, true}) {
        for (const QPoint &offset : {QPoint(37, 23), QPoint(64, 128), QPoint(-50, -71), QPoint(0, 0), QPoint(700, 0)}) {
            for (const QPoint &deviceOffset : {QPoint(0, 0), QPoint(10, 7)}) {
                QTest::newRow(qPrintable(QString("f16%1-offset%2,%3-device%4,%5")
                                             .arg(f16)
                                             .arg(offset.x())
                                             .arg(offset.y())
                                             .arg(deviceOffset.x())
                                             .arg(deviceOffset.y())))
                    << f16 << offset << deviceOffset;
            }
        }
    }
}

void KisGpuProjectionTest::testShiftedCopyMatchesCpu()
{
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QPoint, offset);
    QFETCH(QPoint, sourceDeviceOffset);
    const KoColorSpace *cs = rgbaFloat(f16);

    KisPaintDeviceSP source = new KisPaintDevice(cs);
    fillRandom(source, QRect(0, 0, 300, 260), 61, true);
    source->moveTo(sourceDeviceOffset);
    // The destination keeps its content outside the copied rect.
    KisPaintDeviceSP expected = new KisPaintDevice(cs);
    fillRandom(expected, QRect(0, 0, 640, 448), 62);
    KisPaintDeviceSP actual = new KisPaintDevice(cs);
    fillRandom(actual, QRect(0, 0, 640, 448), 62);

    const QRect bounds(-128, -128, 1024, 768);
    // Partly or wholly outside the source: those pixels become its default pixel.
    for (const QRect &rect : {QRect(20, 30, 250, 200), QRect(150, 100, 260, 300), QRect(900, 600, 50, 40)}) {
        KisPainter::copyAreaOptimized(rect.topLeft(), source, expected, rect.translated(-offset));
        QString error;
        QVERIFY2(KisGpuProjectionCompositor::copyShifted(actual, rect, source, offset, &error), qPrintable(error));
        QPoint where;
        const float difference =
            maxDifference(readFloats(expected, bounds), readFloats(actual, bounds), &where, bounds.width());
        QVERIFY2(difference == 0.0f,
                 qPrintable(QString("rect %1,%2 %3x%4: difference %5 at %6,%7")
                                .arg(rect.x())
                                .arg(rect.y())
                                .arg(rect.width())
                                .arg(rect.height())
                                .arg(difference)
                                .arg(where.x() + bounds.x())
                                .arg(where.y() + bounds.y())));
    }

    // Unsupported cases refuse and leave the destination unchanged.
    KisPaintDeviceSP rgba8 = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    QVERIFY(!KisGpuProjectionCompositor::copyShifted(rgba8, QRect(0, 0, 64, 64), source, offset));
    QVERIFY(!KisGpuProjectionCompositor::copyShifted(source, QRect(0, 0, 64, 64), source, offset));
}

void KisGpuProjectionTest::testCloneLayerMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QPoint>("offset");
    QTest::addColumn<QString>("mask");
    for (bool f16 : {false, true}) {
        for (const QPoint &offset : {QPoint(37, 23), QPoint(64, 128), QPoint(-45, 70), QPoint(0, 0)}) {
            for (const QString &mask : {QString("none"), QString("transparency"), QString("blur")}) {
                if (offset.isNull() && mask == "none")
                    continue; // no clone projection: the source device is composited directly
                QTest::newRow(
                    qPrintable(QString("f16%1-offset%2,%3-%4").arg(f16).arg(offset.x()).arg(offset.y()).arg(mask)))
                    << f16 << offset << mask;
            }
        }
    }
}

void KisGpuProjectionTest::testCloneLayerMatchesCpu()
{
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QPoint, offset);
    QFETCH(QString, mask);
    const QRect bounds(0, 0, 512, 384);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(f16), "clone");
    addPaintLayer(image, image->root(), COMPOSITE_OVER, 255, 71, bounds);
    KisPaintLayerSP source = addPaintLayer(image, image->root(), COMPOSITE_OVER, 255, 72, QRect(40, 50, 300, 220));
    KisCloneLayerSP clone = new KisCloneLayer(source, image, "clone", 200);
    clone->setX(offset.x());
    clone->setY(offset.y());
    clone->setCompositeOpId(COMPOSITE_MULT);
    image->addNode(clone, image->root());
    if (mask == "transparency") {
        KisTransparencyMaskSP transparency = new KisTransparencyMask(image, "transparency");
        transparency->initSelection(KisLayerSP(clone));
        transparency->selection()->pixelSelection()->clear(QRect(100, 90, 120, 80));
        transparency->selection()->updateProjection();
        image->addNode(transparency, clone);
    } else if (mask == "blur") {
        KisFilterSP blur = KisFilterRegistry::instance()->value("blur");
        QVERIFY(blur);
        KisFilterMaskSP filterMask = new KisFilterMask(image, "blur");
        filterMask->initSelection(KisLayerSP(clone));
        filterMask->setFilter(
            blur->defaultConfiguration(KisGlobalResourcesInterface::instance())->cloneWithResourcesSnapshot());
        image->addNode(filterMask, clone);
    }

    auto compare = [&](const char *stage) {
        const std::vector<float> gpu = readFloats(image->projection(), bounds);
        const std::vector<float> cpu = render(image, false);
        QPoint where;
        const float error = f16 ? maxHalfUlpDifference(cpu, gpu, &where, bounds.width())
                                : maxDifference(cpu, gpu, &where, bounds.width());
        QVERIFY2(error <= (f16 ? 4.0f : 2e-5f),
                 qPrintable(QString("%1: difference %2 at %3,%4, %5")
                                .arg(stage)
                                .arg(error)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(pixelReport(cpu, gpu, where, bounds.width()))));
    };

    // Filter masks run on the CPU: the clone then copies on the CPU too.
    // Transparency masks are applied on the GPU (phase 4.113).
    const bool gpuCopy = mask != "blur";
    const quint64 copiesBefore = KisGpuProjectionCompositor::shiftedCopyCount();
    render(image, true);
    QCOMPARE(KisGpuProjectionCompositor::shiftedCopyCount() > copiesBefore, gpuCopy);
    compare("full refresh");

    // GPU strokes on the source: its tiles become GPU-valid and CPU-stale,
    // and the clone's GPU copy must not read them back.
    KisGpuMergeBatch::setEnabled(true);
    for (int i = 0; i < 3; ++i) {
        const QRect dab(64 + i * 64, 64, 128, 128);
        KisPaintDeviceSP dabDevice = new KisPaintDevice(rgbaFloat(f16));
        fillRandom(dabDevice, dab, 80 + i);
        KisGpuProjectionCompositor::Layer layer;
        layer.device = dabDevice;
        layer.opacity = 0.7f;
        QString error;
        QVERIFY2(KisGpuProjectionCompositor::composite(source->paintDevice(), dab, {layer}, &error), qPrintable(error));
        const quint64 copies = KisGpuProjectionCompositor::shiftedCopyCount();
        source->setDirty(dab);
        image->waitForDone();
        QCOMPARE(KisGpuProjectionCompositor::shiftedCopyCount() > copies, gpuCopy);
        if (gpuCopy) {
            KisTileSP tile = source->paintDevice()->dataManager()->getTile(dab.x() / 64 + 1, 1, false);
            KisTileGpuState *state = tile->tileData()->gpuState();
            QVERIFY(state);
            QVERIFY(!state->cpuValid());
        }
    }
    compare("partial updates");
}

void KisGpuProjectionTest::testMovedLayersMatchCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QPoint>("offset");
    for (bool f16 : {false, true}) {
        for (const QPoint &offset :
             {QPoint(37, 23), QPoint(-50, -71), QPoint(1, 0), QPoint(0, 63), QPoint(-1, -1), QPoint(300, -200)}) {
            QTest::newRow(qPrintable(QString("f16%1-offset%2,%3").arg(f16).arg(offset.x()).arg(offset.y())))
                << f16 << offset;
        }
    }
}

void KisGpuProjectionTest::testMovedLayersMatchCpu()
{
    // Phase 4.112: layers moved by a distance that is not a multiple of 64
    // are composited on the GPU (they went to the CPU before, which read the
    // projection back).
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QPoint, offset);
    const QRect bounds(0, 0, 700, 500);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(f16), "moved layers");
    KisNodeSP root = image->root();
    addPaintLayer(image, root, COMPOSITE_OVER, 255, 91, bounds);
    // Basic, extended and alpha-locked composites; one layer covers the
    // image, the others leave tiles without data inside it.
    QVector<KisPaintLayerSP> moved;
    moved << addPaintLayer(image, root, COMPOSITE_OVER, 200, 92, QRect(40, 30, 500, 380));
    moved << addPaintLayer(image, root, COMPOSITE_MULT, 255, 93, bounds);
    moved << addPaintLayer(image, root, COMPOSITE_SOFT_LIGHT_SVG, 230, 94, QRect(100, 100, 300, 300), true);
    KisPaintLayerSP locked = addPaintLayer(image, root, COMPOSITE_OVERLAY, 230, 95, QRect(0, 0, 640, 480));
    QBitArray alphaLocked(4, true);
    alphaLocked.clearBit(3);
    locked->setChannelFlags(alphaLocked);
    moved << locked;
    addPaintLayer(image, root, COMPOSITE_SCREEN, 128, 96, QRect(200, 50, 400, 400));

    auto moveLayers = [&](const QPoint &by) {
        for (int i = 0; i < moved.size(); ++i) {
            // different shifts in one composite
            const QPoint layerOffset = by + QPoint(i * 5, -i * 3);
            moved[i]->setX(layerOffset.x());
            moved[i]->setY(layerOffset.y());
        }
    };
    auto compare = [&](const char *stage) {
        const std::vector<float> gpu = readFloats(image->projection(), bounds);
        const std::vector<float> cpu = render(image, false);
        QPoint where;
        const float error = f16 ? maxHalfUlpDifference(cpu, gpu, &where, bounds.width())
                                : maxDifference(cpu, gpu, &where, bounds.width());
        QVERIFY2(error <= (f16 ? 4.0f : 2e-5f),
                 qPrintable(QString("%1: difference %2 at %3,%4, %5")
                                .arg(stage)
                                .arg(error)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(pixelReport(cpu, gpu, where, bounds.width()))));
    };

    moveLayers(offset);
    {
        // The moved layers themselves are accepted, not only the others.
        // (This composites them once more; the refresh below starts over.)
        KisGpuMergeBatch::setEnabled(true);
        KisGpuMergeBatch batch;
        for (KisPaintLayerSP layer : moved) {
            QVERIFY(batch.tryAdd(layer->projectionLeaf(), image->projection(), QRect(17, 29, 283, 171)));
        }
        batch.flush();
    }
    render(image, true);
    compare("full refresh");

    // What the Move Tool does for every pointer position, on a projection
    // that only the GPU holds: nothing may be read back.
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    render(image, true);
    backend->context().waitIdle();
    const quint64 downloadsBefore = backend->downloadedTileCount();
    const quint64 compositesBefore = KisGpuMergeBatch::gpuCompositeCount();
    for (int step = 1; step <= 3; ++step) {
        const QPoint by = offset + QPoint(11 * step, 7 * step);
        QRect dirtyRect;
        for (KisPaintLayerSP layer : moved) {
            dirtyRect |= layer->paintDevice()->extent();
        }
        moveLayers(by);
        for (KisPaintLayerSP layer : moved) {
            dirtyRect |= layer->paintDevice()->extent();
        }
        for (KisPaintLayerSP layer : moved) {
            image->refreshGraphAsync(layer, dirtyRect);
        }
        image->waitForDone();
    }
    backend->context().waitIdle();
    QCOMPARE(backend->downloadedTileCount(), downloadsBefore);
    QVERIFY(KisGpuMergeBatch::gpuCompositeCount() > compositesBefore);
    compare("move steps");
}

void KisGpuProjectionTest::testMovedGroupMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QPoint>("offset");
    for (bool f16 : {false, true}) {
        for (const QPoint &offset : {QPoint(37, 23), QPoint(-50, -71), QPoint(64, 128)}) {
            QTest::newRow(qPrintable(QString("f16%1-offset%2,%3").arg(f16).arg(offset.x()).arg(offset.y())))
                << f16 << offset;
        }
    }
}

void KisGpuProjectionTest::testMovedGroupMatchesCpu()
{
    // Phase 4.112: a moved group keeps its projection on the image grid
    // (KisGroupLayer::setX()), so the group and its children stay on the GPU.
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QPoint, offset);
    const QRect bounds(0, 0, 700, 500);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(f16), "moved group");
    KisNodeSP root = image->root();
    addPaintLayer(image, root, COMPOSITE_OVER, 255, 101, bounds);
    KisGroupLayerSP group = new KisGroupLayer(image, "group", 200);
    group->setCompositeOpId(COMPOSITE_MULT);
    image->addNode(group, root);
    QVector<KisNodeSP> moved;
    moved << group;
    moved << addPaintLayer(image, group, COMPOSITE_OVER, 255, 102, QRect(60, 40, 300, 260));
    moved << addPaintLayer(image, group, COMPOSITE_SCREEN, 180, 103, QRect(200, 150, 280, 220));
    addPaintLayer(image, root, COMPOSITE_OVERLAY, 128, 104, QRect(100, 0, 500, 500));

    // The Move Tool's step (MoveStrokeStrategy::doCanvasUpdate()).
    auto moveStep = [&](const QPoint &to) {
        QRect dirtyRect;
        for (KisNodeSP node : moved) {
            QRect rect = node.data() == group.data() ? node->projectionPlane()->looseUserVisibleBounds()
                                                     : node->projectionPlane()->tightUserVisibleBounds();
            rect |= rect.translated(to - QPoint(node->x(), node->y()));
            node->setX(to.x());
            node->setY(to.y());
            dirtyRect |= rect;
        }
        image->refreshGraphAsync(group, dirtyRect);
        image->waitForDone();
    };
    auto compareWithFullRefresh = [&](const char *stage, bool cpuOnly) {
        const std::vector<float> incremental = readFloats(image->projection(), bounds);
        const std::vector<float> full = render(image, false);
        QPoint where;
        const float error = f16 ? maxHalfUlpDifference(full, incremental, &where, bounds.width())
                                : maxDifference(full, incremental, &where, bounds.width());
        QVERIFY2(error <= (cpuOnly ? 0.0f : (f16 ? 4.0f : 2e-5f)),
                 qPrintable(QString("%1: difference %2 at %3,%4, %5")
                                .arg(stage)
                                .arg(error)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(pixelReport(full, incremental, where, bounds.width()))));
    };

    // On the CPU: the group's old pixels are replaced where the children
    // were and where they are now, although its projection did not move.
    render(image, false);
    moveStep(offset);
    QCOMPARE(QPoint(group->x(), group->y()), offset);
    QCOMPARE(group->projection()->offset(), QPoint());
    compareWithFullRefresh("CPU move", true);
    moveStep(offset * 2 + QPoint(5, -3));
    compareWithFullRefresh("CPU second move", true);

    // On the GPU: the children composite into the group's projection, and
    // the steps read nothing back.
    render(image, true);
    {
        KisGpuMergeBatch batch;
        QVERIFY(batch.tryAdd(moved[1]->projectionLeaf(), group->projection(), QRect(17, 29, 283, 171)));
        batch.flush();
    }
    render(image, true);
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    backend->context().waitIdle();
    const quint64 downloadsBefore = backend->downloadedTileCount();
    const quint64 compositesBefore = KisGpuMergeBatch::gpuCompositeCount();
    for (int step = 1; step <= 3; ++step) {
        moveStep(offset + QPoint(13 * step, 9 * step));
    }
    backend->context().waitIdle();
    QCOMPARE(backend->downloadedTileCount(), downloadsBefore);
    QVERIFY(KisGpuMergeBatch::gpuCompositeCount() > compositesBefore);
    compareWithFullRefresh("GPU moves", false);
}

void KisGpuProjectionTest::testTransparencyMaskMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<int>("maskCount");
    for (bool f16 : {false, true}) {
        for (const QString &kind : {QString("paint"), QString("group"), QString("clone")}) {
            for (int maskCount : {1, 2}) {
                QTest::newRow(qPrintable(QString("f16%1-%2-masks%3").arg(f16).arg(kind).arg(maskCount)))
                    << f16 << kind << maskCount;
            }
        }
    }
}

void KisGpuProjectionTest::testTransparencyMaskMatchesCpu()
{
    // Phase 4.113: transparency masks are applied on the GPU, and the copy
    // of the original into the layer's projection stays there too.
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QString, kind);
    QFETCH(int, maskCount);
    const QRect bounds(0, 0, 512, 384);
    const QRect content(40, 50, 300, 220);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(f16), "transparency mask");
    KisNodeSP root = image->root();
    addPaintLayer(image, root, COMPOSITE_OVER, 255, 111, bounds);

    // The layer that is painted, and the layer that carries the masks.
    KisPaintLayerSP painted;
    KisLayerSP masked;
    if (kind == "paint") {
        painted = addPaintLayer(image, root, COMPOSITE_MULT, 200, 112, content);
        masked = painted;
    } else if (kind == "group") {
        KisGroupLayerSP group = new KisGroupLayer(image, "group", 220);
        image->addNode(group, root);
        addPaintLayer(image, group, COMPOSITE_OVER, 255, 113, QRect(100, 20, 300, 300));
        painted = addPaintLayer(image, group, COMPOSITE_SCREEN, 200, 112, content);
        masked = group;
    } else {
        painted = addPaintLayer(image, root, COMPOSITE_OVER, 255, 112, content);
        KisCloneLayerSP clone = new KisCloneLayer(painted, image, "clone", 200);
        clone->setX(37);
        clone->setY(23);
        clone->setCompositeOpId(COMPOSITE_MULT);
        image->addNode(clone, root);
        masked = clone;
    }

    QVector<KisTransparencyMaskSP> masks;
    for (int i = 0; i < maskCount; ++i) {
        // Every coverage value, a hole (0) and an untouched band (255).
        KisTransparencyMaskSP mask = new KisTransparencyMask(image, QStringLiteral("transparency %1").arg(i));
        mask->initSelection(masked);
        KisPixelSelectionSP pixels = mask->selection()->pixelSelection();
        const QRect maskRect(10, 10, 420, 330);
        QByteArray bytes(maskRect.width() * maskRect.height(), Qt::Uninitialized);
        for (int y = 0; y < maskRect.height(); ++y) {
            for (int x = 0; x < maskRect.width(); ++x) {
                bytes[y * maskRect.width() + x] = char((x * (i + 1) + y * 3) % 256);
            }
        }
        pixels->writeBytes(reinterpret_cast<const quint8 *>(bytes.constData()), maskRect);
        pixels->clear(QRect(100 + 40 * i, 90, 120, 80));
        pixels->select(QRect(230, 60 + 30 * i, 60, 200), 255);
        mask->selection()->updateProjection();
        image->addNode(mask, masked);
        masks << mask;
    }
    addPaintLayer(image, root, COMPOSITE_SCREEN, 128, 114, QRect(200, 0, 300, 384));

    auto compare = [&](const char *stage) {
        const std::vector<float> gpu = readFloats(image->projection(), bounds);
        const std::vector<float> cpu = render(image, false);
        QPoint where;
        const float error = f16 ? maxHalfUlpDifference(cpu, gpu, &where, bounds.width())
                                : maxDifference(cpu, gpu, &where, bounds.width());
        QVERIFY2(error <= (f16 ? 4.0f : 2e-5f),
                 qPrintable(QString("%1: difference %2 at %3,%4, %5")
                                .arg(stage)
                                .arg(error)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(pixelReport(cpu, gpu, where, bounds.width()))));
    };

    QVERIFY(masked->gpuAppliesEffectMasks() == false); // the GPU projection is off
    KisGpuMergeBatch::setEnabled(true);
    QVERIFY(masked->gpuAppliesEffectMasks());
    const quint64 coverageBefore = KisGpuProjectionCompositor::coverageCount();
    render(image, true);
    QVERIFY(KisGpuProjectionCompositor::coverageCount() >= coverageBefore + quint64(maskCount));
    compare("full refresh");

    // GPU strokes on the painted layer: its tiles become GPU-valid and
    // CPU-stale, and neither the copy nor the masks may read them back.
    render(image, true);
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    KisGpuMergeBatch::setEnabled(true);
    for (int i = 0; i < 3; ++i) {
        const QRect dab(64 + i * 70, 70 + i * 9, 128, 128);
        KisPaintDeviceSP dabDevice = new KisPaintDevice(rgbaFloat(f16));
        fillRandom(dabDevice, dab, 180 + i);
        KisGpuProjectionCompositor::Layer layer;
        layer.device = dabDevice;
        layer.opacity = 0.7f;
        QString error;
        QVERIFY2(KisGpuProjectionCompositor::composite(painted->paintDevice(), dab, {layer}, &error),
                 qPrintable(error));
        backend->context().waitIdle();
        const quint64 downloads = backend->downloadedTileCount();
        const quint64 coverage = KisGpuProjectionCompositor::coverageCount();
        painted->setDirty(dab);
        image->waitForDone();
        backend->context().waitIdle();
        QCOMPARE(backend->downloadedTileCount(), downloads);
        QVERIFY(KisGpuProjectionCompositor::coverageCount() > coverage);
    }
    compare("partial updates");

    // Editing a mask: the selection changes on the CPU and is applied again.
    render(image, true);
    KisGpuMergeBatch::setEnabled(true);
    const QRect edited(150, 120, 90, 70);
    masks.first()->selection()->pixelSelection()->select(edited, 96);
    masks.first()->selection()->updateProjection(edited);
    masks.first()->setDirty(edited);
    image->waitForDone();
    compare("mask edit");
}

void KisGpuProjectionTest::testStrokeStyleMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<int>("size");
    QTest::addColumn<QString>("strokeMode");
    for (bool f16 : {false, true}) {
        for (const QString &kind : {QString("paint"), QString("group")}) {
            for (int size : {1, 2, 3, 5, 10, 16}) {
                // the modes the user's documents use, and Normal
                const QString strokeMode = size == 2 ? COMPOSITE_LIGHTEN
                    : size == 3                      ? COMPOSITE_SCREEN
                    : size == 5                      ? COMPOSITE_OVER
                                                     : COMPOSITE_MULT;
                QTest::newRow(qPrintable(QString("f16%1-%2-size%3-%4").arg(f16).arg(kind).arg(size).arg(strokeMode)))
                    << f16 << kind << size << strokeMode;
            }
        }
    }
}

void KisGpuProjectionTest::testStrokeStyleMatchesCpu()
{
    // Phase 4.114: a layer whose style is a Stroke outside it with a solid
    // color is composited with the stroke on the GPU.
    REQUIRE_GPU();
    if (!KisGpuProjectionCompositor::supportsStrokeStyle()) {
        QSKIP("the GPU has no 64-bit float shaders");
    }
    QFETCH(bool, f16);
    QFETCH(QString, kind);
    QFETCH(int, size);
    QFETCH(QString, strokeMode);
    const QRect bounds(0, 0, 512, 384);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(f16), "stroke style");
    KisNodeSP root = image->root();
    addPaintLayer(image, root, COMPOSITE_OVER, 255, 121, bounds);

    // The painted layer: an opaque block, noise with every alpha, soft
    // gradients at the edges, and empty tiles around.
    KisPaintLayerSP painted;
    KisLayerSP styled;
    if (kind == "paint") {
        painted = addPaintLayer(image, root, COMPOSITE_MULT, 200, 122, QRect(150, 110, 120, 90));
        styled = painted;
    } else {
        KisGroupLayerSP group = new KisGroupLayer(image, "group", 220);
        image->addNode(group, root);
        addPaintLayer(image, group, COMPOSITE_OVER, 255, 123, QRect(300, 60, 90, 70));
        painted = addPaintLayer(image, group, COMPOSITE_OVER, 255, 122, QRect(150, 110, 120, 90));
        styled = group;
    }
    {
        KisPaintDeviceSP device = painted->paintDevice();
        const KoColorSpace *cs = device->colorSpace();
        device->fill(QRect(60, 60, 110, 130), KoColor(QColor(200, 80, 40, 255), cs));
        // a soft edge: alpha falls from 1 to 0 over 24 pixels
        const QRect soft(170, 220, 96, 60);
        QVector<float> floats(soft.width() * soft.height() * 4);
        for (int y = 0; y < soft.height(); ++y) {
            for (int x = 0; x < soft.width(); ++x) {
                const float edge =
                    qBound(0.0f, qMin(qMin(x, soft.width() - 1 - x), qMin(y, soft.height() - 1 - y)) / 24.0f, 1.0f);
                float *pixel = floats.data() + (y * soft.width() + x) * 4;
                pixel[0] = 0.2f;
                pixel[1] = 0.6f;
                pixel[2] = 0.9f;
                pixel[3] = edge;
            }
        }
        if (f16) {
            QVector<qfloat16> half(floats.size());
            qFloatToFloat16(half.data(), floats.constData(), floats.size());
            device->writeBytes(reinterpret_cast<const quint8 *>(half.constData()), soft);
        } else {
            device->writeBytes(reinterpret_cast<const quint8 *>(floats.constData()), soft);
        }
    }

    KisPSDLayerStyleSP style(new KisPSDLayerStyle());
    style->stroke()->setEffectEnabled(true);
    style->stroke()->setPosition(psd_stroke_outside);
    style->stroke()->setFillType(psd_fill_solid_color);
    style->stroke()->setColor(KoColor(QColor(40, 20, 90), rgbaFloat(f16)));
    style->stroke()->setBlendMode(strokeMode);
    style->stroke()->setOpacity(75);
    style->stroke()->setSize(size);
    styled->setLayerStyle(style);

    addPaintLayer(image, root, COMPOSITE_SCREEN, 128, 124, QRect(200, 0, 300, 384));

    auto compare = [&](const char *stage) {
        const std::vector<float> gpu = readFloats(image->projection(), bounds);
        const std::vector<float> cpu = render(image, false);
        QPoint where;
        const float error = f16 ? maxHalfUlpDifference(cpu, gpu, &where, bounds.width())
                                : maxDifference(cpu, gpu, &where, bounds.width());
        QVERIFY2(error <= (f16 ? 4.0f : 2e-5f),
                 qPrintable(QString("%1: difference %2 at %3,%4, %5")
                                .arg(stage)
                                .arg(error)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(pixelReport(cpu, gpu, where, bounds.width()))));
    };

    auto *stylePlane = dynamic_cast<KisLayerStyleProjectionPlane *>(styled->projectionPlane().data());
    QVERIFY(stylePlane);
    QVERIFY(!stylePlane->gpuCompositesStyles()); // the GPU projection is off
    KisGpuMergeBatch::setEnabled(true);
    QVERIFY(stylePlane->gpuCompositesStyles());
    const quint64 stylesBefore = KisGpuProjectionCompositor::strokeStyleCount();
    render(image, true);
    QVERIFY(KisGpuProjectionCompositor::strokeStyleCount() > stylesBefore);
    compare("full refresh");

    // GPU strokes on the painted layer: neither the layer nor the stroke
    // may read anything back.
    render(image, true);
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    KisGpuMergeBatch::setEnabled(true);
    for (int i = 0; i < 3; ++i) {
        const QRect dab(40 + i * 90, 150 + i * 13, 128, 128);
        KisPaintDeviceSP dabDevice = new KisPaintDevice(rgbaFloat(f16));
        fillRandom(dabDevice, dab, 190 + i);
        KisGpuProjectionCompositor::Layer layer;
        layer.device = dabDevice;
        layer.opacity = 0.7f;
        QString error;
        QVERIFY2(KisGpuProjectionCompositor::composite(painted->paintDevice(), dab, {layer}, &error),
                 qPrintable(error));
        backend->context().waitIdle();
        const quint64 downloads = backend->downloadedTileCount();
        const quint64 styles = KisGpuProjectionCompositor::strokeStyleCount();
        painted->setDirty(dab);
        image->waitForDone();
        backend->context().waitIdle();
        QCOMPARE(backend->downloadedTileCount(), downloads);
        QVERIFY(KisGpuProjectionCompositor::strokeStyleCount() > styles);
    }
    compare("partial updates");

    // A style the GPU pass does not have: the CPU computes it as before.
    render(image, true);
    KisPSDLayerStyleSP inside = style->clone().dynamicCast<KisPSDLayerStyle>();
    inside->stroke()->setPosition(psd_stroke_inside);
    styled->setLayerStyle(inside);
    KisGpuMergeBatch::setEnabled(true);
    stylePlane = dynamic_cast<KisLayerStyleProjectionPlane *>(styled->projectionPlane().data());
    QVERIFY(stylePlane && !stylePlane->gpuCompositesStyles());
    render(image, true);
    compare("inside stroke on the CPU");
}

namespace
{
KisFilterConfigurationSP filterLayerConfig(const QString &id, const QString &params)
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(id);
    KIS_ASSERT(filter);
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    config->fromXML(QString("<!DOCTYPE params>\n<params version=\"1\">\n%1</params>\n").arg(params));
    return config->cloneWithResourcesSnapshot();
}

/// Per-channel curves in the current layout: RGBA, R, G, B, A, hue, saturation, lightness.
KisFilterConfigurationSP curvesConfig(const QVector<int> &curvedChannels)
{
    QString params = " <param name=\"nTransfers\">8</param>\n";
    for (int i = 0; i < 8; ++i) {
        const QString curve = curvedChannels.contains(i) ? QString("0,0;0.551807,0.415686;1,1;") : QString("0,0;1,1;");
        params += QString(" <param name=\"curve%1\">%2</param>\n").arg(i).arg(curve);
    }
    return filterLayerConfig("perchannel", params);
}

KisFilterConfigurationSP namedFilterConfig(const QString &name)
{
    if (name.startsWith("desaturate")) {
        return filterLayerConfig(
            "desaturate",
            QString(" <param name=\"type\" type=\"string\"><![CDATA[%1]]></param>\n").arg(name.mid(10).toInt()));
    }
    if (name.startsWith("posterize")) {
        return filterLayerConfig(
            "posterize",
            QString(" <param name=\"steps\" type=\"string\"><![CDATA[%1]]></param>\n").arg(name.mid(9).toInt()));
    }
    if (name == "curves-all") {
        return curvesConfig({0});
    }
    if (name == "curves-channels") {
        return curvesConfig({1, 4});
    }
    if (name.startsWith("colorreference")) {
        // The filter's defaults (the Amber preset, Amount 100, Keep
        // brightness), or Amount 60 without Keep brightness.
        KisFilterSP filter = KisFilterRegistry::instance()->value("solstice_color_reference");
        KIS_ASSERT(filter);
        KisFilterConfigurationSP config = filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
        if (name == "colorreference-free") {
            config->setProperty("keepBrightness", false);
            config->setProperty("amount", 60.0);
        }
        return config->cloneWithResourcesSnapshot();
    }
    KIS_ASSERT(name == "curves-lightness");
    return curvesConfig({7});
}

/// Filters with a GPU form in both float depths, or in RGBA F16 only (tables).
bool filterRunsOnGpu(const QString &name, bool f16)
{
    return name.startsWith("desaturate") || (f16 && name != "curves-lightness");
}
} // namespace

void KisGpuProjectionTest::testFilterLayerMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QString>("filter");
    QTest::addColumn<bool>("selection");
    for (bool f16 : {false, true}) {
        for (const QString &filter : {QString("desaturate0"),
                                      QString("desaturate1"),
                                      QString("desaturate2"),
                                      QString("desaturate3"),
                                      QString("desaturate4"),
                                      QString("desaturate5"),
                                      QString("posterize6"),
                                      QString("posterize16"),
                                      QString("curves-all"),
                                      QString("curves-channels"),
                                      QString("curves-lightness"),
                                      QString("colorreference"),
                                      QString("colorreference-free")}) {
            for (bool selection : {false, true}) {
                QTest::newRow(qPrintable(QString("f16%1-%2%3").arg(f16).arg(filter).arg(selection ? "-selection" : "")))
                    << f16 << filter << selection;
            }
        }
    }
}

void KisGpuProjectionTest::testFilterLayerMatchesCpu()
{
    // Phase 4.115: filter layers whose filter has a GPU form are filtered
    // and composited without reading the projection back.
    REQUIRE_GPU();
    if (!KisGpuProjectionCompositor::supportsColorTransform()) {
        QSKIP("the GPU has no 64-bit float shaders");
    }
    QFETCH(bool, f16);
    QFETCH(QString, filter);
    QFETCH(bool, selection);
    // The tables hold what the CPU transformation computes for every half
    // value; RGBA F32 has too many values for that.
    const bool expectGpu = filterRunsOnGpu(filter, f16);

    const QRect bounds(0, 0, 512, 384);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(f16), "filter layer");
    KisNodeSP root = image->root();
    // HDR colors next to a partly selected pixel are clamped by the CPU's
    // vectorized Copy op (docs/agent/gpu-engine.md): keep them out of the
    // selection rows.
    // One layer below the filter: the filter then reads the same pixels in
    // the GPU and the CPU run, and its result can be compared exactly.
    KisPaintLayerSP painted = addPaintLayer(image, root, COMPOSITE_OVER, 255, 131, QRect(0, 0, 450, 384), !selection);

    KisSelectionSP layerSelection;
    if (selection) {
        // A selected block, a soft band on its right, the rest unselected.
        layerSelection = new KisSelection(new KisDefaultBounds(image), toQShared(new KisImageResolutionProxy(image)));
        KisPixelSelectionSP pixels = layerSelection->pixelSelection();
        pixels->select(QRect(70, 40, 200, 250), MAX_SELECTED);
        for (int i = 0; i < 127; ++i) {
            pixels->select(QRect(270 + i, 40, 1, 250), quint8(254 - 2 * i));
        }
        layerSelection->updateProjection();
    }
    KisAdjustmentLayerSP adjustment = new KisAdjustmentLayer(image, filter, namedFilterConfig(filter), layerSelection);
    image->addNode(adjustment, root);

    addPaintLayer(image, root, COMPOSITE_MULT, 200, 132, QRect(100, 50, 300, 200));
    addPaintLayer(image, root, COMPOSITE_SCREEN, 128, 133, QRect(200, 0, 300, 384));

    auto compare = [&](const char *stage) {
        // The filtered pixels themselves, then the image.
        const std::vector<float> gpuOriginal = readFloats(adjustment->original(), bounds);
        const std::vector<float> gpu = readFloats(image->projection(), bounds);
        const std::vector<float> cpu = render(image, false);
        const std::vector<float> cpuOriginal = readFloats(adjustment->original(), bounds);
        QPoint where;
        float error = f16 ? maxHalfUlpDifference(cpuOriginal, gpuOriginal, &where, bounds.width())
                          : maxDifference(cpuOriginal, gpuOriginal, &where, bounds.width());
        // The GPU repeats the CPU exactly. Only the CPU's vectorized Copy
        // op for partly selected RGBA F32 pixels is matched within the
        // usual tolerance.
        const float originalTolerance = (f16 || !selection) ? 0.0f : 2e-5f;
        QVERIFY2(error <= originalTolerance,
                 qPrintable(QString("%1, filtered pixels: difference %2 at %3,%4, %5")
                                .arg(stage)
                                .arg(error)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(pixelReport(cpuOriginal, gpuOriginal, where, bounds.width()))));
        error = f16 ? maxHalfUlpDifference(cpu, gpu, &where, bounds.width())
                    : maxDifference(cpu, gpu, &where, bounds.width());
        QVERIFY2(error <= (f16 ? 4.0f : 2e-5f),
                 qPrintable(QString("%1: difference %2 at %3,%4, %5")
                                .arg(stage)
                                .arg(error)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(pixelReport(cpu, gpu, where, bounds.width()))));
    };

    quint64 transforms = KisGpuProjectionCompositor::colorTransformCount();
    render(image, true);
    QCOMPARE(KisGpuProjectionCompositor::colorTransformCount() > transforms, expectGpu);
    compare("full refresh");

    // GPU strokes below the filter layer: nothing may be read back.
    render(image, true);
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    KisGpuMergeBatch::setEnabled(true);
    for (int i = 0; i < 3; ++i) {
        const QRect dab(40 + i * 110, 30 + i * 70, 128, 128);
        KisPaintDeviceSP dabDevice = new KisPaintDevice(rgbaFloat(f16));
        fillRandom(dabDevice, dab, 290 + i);
        KisGpuProjectionCompositor::Layer layer;
        layer.device = dabDevice;
        layer.opacity = 0.7f;
        QString error;
        QVERIFY2(KisGpuProjectionCompositor::composite(painted->paintDevice(), dab, {layer}, &error),
                 qPrintable(error));
        backend->context().waitIdle();
        const quint64 downloads = backend->downloadedTileCount();
        transforms = KisGpuProjectionCompositor::colorTransformCount();
        painted->setDirty(dab);
        image->waitForDone();
        backend->context().waitIdle();
        if (expectGpu) {
            QCOMPARE(backend->downloadedTileCount(), downloads);
            QVERIFY(KisGpuProjectionCompositor::colorTransformCount() > transforms);
        }
    }
    compare("partial updates");
}

void KisGpuProjectionTest::testFilterMaskMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QString>("filter");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("selection");
    for (bool f16 : {false, true}) {
        for (const QString &filter : {QString("desaturate0"),
                                      QString("desaturate1"),
                                      QString("posterize6"),
                                      QString("curves-all"),
                                      QString("curves-lightness"),
                                      QString("colorreference"),
                                      QString("colorreference-free")}) {
            for (const QString &kind : {QString("paint"), QString("group")}) {
                for (bool selection : {false, true}) {
                    QTest::newRow(qPrintable(
                        QString("f16%1-%2-%3%4").arg(f16).arg(filter).arg(kind).arg(selection ? "-selection" : "")))
                        << f16 << filter << kind << selection;
                }
            }
        }
    }
}

void KisGpuProjectionTest::testFilterMaskMatchesCpu()
{
    // Phase 4.116: a filter mask whose filter has a GPU form filters its
    // layer's projection in place on the GPU, through the mask's selection.
    REQUIRE_GPU();
    if (!KisGpuProjectionCompositor::supportsColorTransform()) {
        QSKIP("the GPU has no 64-bit float shaders");
    }
    QFETCH(bool, f16);
    QFETCH(QString, filter);
    QFETCH(QString, kind);
    QFETCH(bool, selection);
    const bool expectGpu = filterRunsOnGpu(filter, f16);

    const QRect bounds(0, 0, 512, 384);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(f16), "filter mask");
    KisNodeSP root = image->root();
    addPaintLayer(image, root, COMPOSITE_OVER, 255, 151, bounds);

    // HDR colors next to a partly selected pixel are clamped by the CPU's
    // vectorized Copy op: keep them out of the selection rows.
    const bool hdr = !selection;
    KisPaintLayerSP painted;
    KisLayerSP masked;
    if (kind == "paint") {
        painted = addPaintLayer(image, root, COMPOSITE_OVER, 230, 152, QRect(30, 20, 420, 330), hdr);
        masked = painted;
    } else {
        KisGroupLayerSP group = new KisGroupLayer(image, "group", 230);
        image->addNode(group, root);
        // One child: the group's projection is then the same in the GPU
        // and the CPU run, and the filter's result can be compared exactly
        // (a tone mapping would magnify a rounding difference of its input).
        painted = addPaintLayer(image, group, COMPOSITE_OVER, 255, 152, QRect(30, 20, 420, 330), hdr);
        masked = group;
    }

    KisFilterMaskSP mask = new KisFilterMask(image, filter);
    mask->initSelection(masked);
    mask->setFilter(namedFilterConfig(filter));
    if (selection) {
        // A selected block, a soft band on its right, the rest unselected.
        KisPixelSelectionSP pixels = mask->selection()->pixelSelection();
        pixels->clear();
        pixels->select(QRect(70, 40, 200, 250), MAX_SELECTED);
        for (int i = 0; i < 127; ++i) {
            pixels->select(QRect(270 + i, 40, 1, 250), quint8(254 - 2 * i));
        }
        mask->selection()->updateProjection();
    }
    image->addNode(mask, masked);

    addPaintLayer(image, root, COMPOSITE_SCREEN, 128, 154, QRect(200, 0, 300, 384));

    auto compare = [&](const char *stage) {
        // The masked layer's projection, then the image.
        const std::vector<float> gpuMasked = readFloats(masked->projection(), bounds);
        const std::vector<float> gpu = readFloats(image->projection(), bounds);
        const std::vector<float> cpu = render(image, false);
        const std::vector<float> cpuMasked = readFloats(masked->projection(), bounds);
        QPoint where;
        float error = f16 ? maxHalfUlpDifference(cpuMasked, gpuMasked, &where, bounds.width())
                          : maxDifference(cpuMasked, gpuMasked, &where, bounds.width());
        // The filter gets the same pixels in both runs: the GPU repeats
        // the CPU exactly, except for partly selected RGBA F32 pixels (the
        // CPU's vectorized Copy op).
        const float maskedTolerance = (f16 || !selection) ? 0.0f : 2e-5f;
        QVERIFY2(error <= maskedTolerance,
                 qPrintable(QString("%1, masked layer: difference %2 at %3,%4, %5")
                                .arg(stage)
                                .arg(error)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(pixelReport(cpuMasked, gpuMasked, where, bounds.width()))));
        error = f16 ? maxHalfUlpDifference(cpu, gpu, &where, bounds.width())
                    : maxDifference(cpu, gpu, &where, bounds.width());
        QVERIFY2(error <= (f16 ? 4.0f : 2e-5f),
                 qPrintable(QString("%1: difference %2 at %3,%4, %5")
                                .arg(stage)
                                .arg(error)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(pixelReport(cpu, gpu, where, bounds.width()))));
    };

    KisGpuMergeBatch::setEnabled(true);
    QCOMPARE(mask->gpuAppliesFilter(), expectGpu);
    quint64 transforms = KisGpuProjectionCompositor::colorTransformCount();
    render(image, true);
    QCOMPARE(KisGpuProjectionCompositor::colorTransformCount() > transforms, expectGpu);
    compare("full refresh");

    // GPU strokes on the masked layer: nothing may be read back.
    render(image, true);
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    KisGpuMergeBatch::setEnabled(true);
    for (int i = 0; i < 3; ++i) {
        const QRect dab(40 + i * 110, 30 + i * 70, 128, 128);
        KisPaintDeviceSP dabDevice = new KisPaintDevice(rgbaFloat(f16));
        fillRandom(dabDevice, dab, 490 + i);
        KisGpuProjectionCompositor::Layer layer;
        layer.device = dabDevice;
        layer.opacity = 0.7f;
        QString error;
        QVERIFY2(KisGpuProjectionCompositor::composite(painted->paintDevice(), dab, {layer}, &error),
                 qPrintable(error));
        backend->context().waitIdle();
        const quint64 downloads = backend->downloadedTileCount();
        transforms = KisGpuProjectionCompositor::colorTransformCount();
        painted->setDirty(dab);
        image->waitForDone();
        backend->context().waitIdle();
        if (expectGpu) {
            QCOMPARE(backend->downloadedTileCount(), downloads);
            QVERIFY(KisGpuProjectionCompositor::colorTransformCount() > transforms);
        }
    }
    compare("partial updates");

    // Painting on the mask (its selection changes on the CPU): a
    // deselected block. In RGBA F32 the CPU's vectorized Copy op blends
    // (and clamps HDR colors of) whole batches at the block's edge, so the
    // exact rows stop here.
    if (!f16 && !selection) {
        return;
    }
    render(image, true);
    {
        KisPixelSelectionSP pixels = mask->selection()->pixelSelection();
        pixels->select(QRect(300, 250, 150, 100), MIN_SELECTED);
        mask->selection()->updateProjection();
        mask->setDirty(QRect(300, 250, 150, 100));
        image->waitForDone();
    }
    compare("mask painted");
}

void KisGpuProjectionTest::testColorReferenceTables()
{
    // Phase 4.116: the Color Reference filter has a GPU form in RGBA F16
    // only, and only while it would change pixels.
    const KoColorSpace *f16 = rgbaFloat(true);
    const KoColorSpace *f32 = rgbaFloat(false);
    KisFilterSP filter = KisFilterRegistry::instance()->value("solstice_color_reference");
    QVERIFY(filter);
    KisFilterConfigurationSP config = namedFilterConfig("colorreference");

    QElapsedTimer timer;
    timer.start();
    const KisGpuColorTransformSP transform = filter->gpuColorTransform(f16, config);
    qInfo() << "Color Reference GPU form, first call (builds the conversion tables):" << timer.elapsed() << "ms";
    QVERIFY(transform);
    QCOMPARE(transform->kind(), KisGpuColorTransform::ColorReference);
    QVERIFY(transform->tablesId() != 0);
    QVERIFY(transform->tableBytes() > 0 && transform->tableBytes() % 4 == 0);

    // cached for the same settings; a new one when a setting changes
    timer.start();
    QCOMPARE(filter->gpuColorTransform(f16, config).data(), transform.data());
    KisFilterConfigurationSP changed = namedFilterConfig("colorreference-free");
    const KisGpuColorTransformSP other = filter->gpuColorTransform(f16, changed);
    qInfo() << "Color Reference GPU form, cached and changed settings:" << timer.elapsed() << "ms";
    QVERIFY(other && other.data() != transform.data());
    QVERIFY(other->tablesId() != transform->tablesId());

    QVERIFY(!filter->gpuColorTransform(f32, config));
    QVERIFY(!filter->gpuColorTransform(KoColorSpaceRegistry::instance()->rgb8(), config));

    KisFilterConfigurationSP off = namedFilterConfig("colorreference");
    off->setProperty("amount", 0.0);
    QVERIFY(!filter->gpuColorTransform(f16, off));
    KisFilterConfigurationSP broken = namedFilterConfig("colorreference");
    broken->setProperty("lut", QString("not a table"));
    QVERIFY(!filter->gpuColorTransform(f16, broken));
}

void KisGpuProjectionTest::testOffGridMaskedLayerConcurrentUpdates_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QString>("mask");
    for (bool f16 : {false, true}) {
        for (const QString &mask : {QString("transparency"), QString("desaturate")}) {
            QTest::newRow(qPrintable(QString("f16%1-%2").arg(f16).arg(mask))) << f16 << mask;
        }
    }
}

void KisGpuProjectionTest::testOffGridMaskedLayerConcurrentUpdates()
{
    // A masked layer that is not on the image's tile grid keeps its masks
    // on the CPU, and its projection device is off that grid too: merge
    // jobs of neighbouring image tiles share its tiles. One job writes
    // such a tile on the CPU (copy-on-write, which frees the old data)
    // while another reads it for a GPU composite. The GPU access has to
    // keep the data it reads alive (KisTile::referencedTileData()). That
    // race crashed the application once (2026-11-12) but is too narrow to
    // show up here reliably:
    // KisGpuPaintDeviceTest::testReadOnlyAccessKeepsTileDataAlive checks the
    // lifetime itself; this test checks the result of such updates.
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QString, mask);
    const QRect bounds(0, 0, 768, 512);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(f16), "off grid");
    KisNodeSP root = image->root();
    addPaintLayer(image, root, COMPOSITE_OVER, 255, 171, bounds);

    KisGroupLayerSP group = new KisGroupLayer(image, "group", 255);
    image->addNode(group, root);
    KisPaintLayerSP painted = addPaintLayer(image, group, COMPOSITE_OVER, 255, 172, QRect(0, 0, 700, 480));
    painted->paintDevice()->moveTo(QPoint(-10, 0));
    addPaintLayer(image, group, COMPOSITE_MULT, 200, 173, QRect(100, 60, 400, 300));

    if (mask == "transparency") {
        KisTransparencyMaskSP transparency = new KisTransparencyMask(image, "mask");
        transparency->initSelection(painted);
        transparency->selection()->pixelSelection()->select(QRect(200, 100, 200, 200), 128);
        transparency->selection()->updateProjection();
        image->addNode(transparency, painted);
    } else {
        KisFilterMaskSP filterMask = new KisFilterMask(image, "mask");
        filterMask->initSelection(painted);
        filterMask->setFilter(namedFilterConfig("desaturate0"));
        image->addNode(filterMask, painted);
    }

    render(image, true);
    KisGpuMergeBatch::setEnabled(true);

    // Many small updates in neighbouring image tiles, queued without
    // waiting, with CPU dabs on the layer in between.
    std::mt19937 random(174);
    for (int round = 0; round < 60; ++round) {
        for (int i = 0; i < 24; ++i) {
            const int col = int(random() % 11);
            const int row = int(random() % 7);
            const QRect rect(col * 64, row * 64, 64, 64);
            if (i % 3 == 0) {
                KisPaintDeviceSP dab = new KisPaintDevice(rgbaFloat(f16));
                fillRandom(dab, rect, 500 + round * 24 + i);
                KisPainter gc(painted->paintDevice());
                gc.setOpacityF(0.5);
                gc.bitBlt(rect.topLeft(), dab, rect);
            }
            painted->setDirty(rect);
        }
        if (round % 4 == 3) {
            painted->setDirty(bounds);
        }
        image->waitForDone();
    }
    KisGpuTileBackend::instance()->context().waitIdle();

    const std::vector<float> gpu = readFloats(image->projection(), bounds);
    const std::vector<float> cpu = render(image, false);
    QPoint where;
    const float error =
        f16 ? maxHalfUlpDifference(cpu, gpu, &where, bounds.width()) : maxDifference(cpu, gpu, &where, bounds.width());
    QVERIFY2(error <= (f16 ? 4.0f : 2e-5f),
             qPrintable(QString("difference %1 at %2,%3, %4")
                            .arg(error)
                            .arg(where.x())
                            .arg(where.y())
                            .arg(pixelReport(cpu, gpu, where, bounds.width()))));
}

void KisGpuProjectionTest::testCopyModeMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QString>("variant");
    QTest::addColumn<bool>("moved");
    for (bool f16 : {false, true}) {
        for (const QString &variant : {QString("plain"),
                                       QString("alpha-off"),
                                       QString("red-off"),
                                       QString("green-and-alpha-off"),
                                       QString("opacity")}) {
            for (bool moved : {false, true}) {
                QTest::newRow(qPrintable(QString("f16%1-%2%3").arg(f16).arg(variant).arg(moved ? "-moved" : "")))
                    << f16 << variant << moved;
            }
        }
    }
}

void KisGpuProjectionTest::testCopyModeMatchesCpu()
{
    // Phase 4.115: the Copy blend mode at full opacity replaces the rect
    // with the layer (its default pixel where it has no data). The CPU
    // honors channel flags only for a layer at another offset than the
    // projection (KisPainter copies whole pixels otherwise); then only the
    // enabled channels are replaced. Reduced opacity stays on the CPU.
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QString, variant);
    QFETCH(bool, moved);
    // With reduced opacity the CPU's vectorized Copy op clamps HDR colors
    // depending on the neighbours (docs/agent/gpu-engine.md).
    const bool hdr = variant != "opacity";
    const QRect bounds(0, 0, 384, 256);
    KisImageSP image = new KisImage(nullptr, bounds.width(), bounds.height(), rgbaFloat(f16), "copy mode");
    KisNodeSP root = image->root();
    addPaintLayer(image, root, COMPOSITE_OVER, 255, 141, QRect(0, 0, 300, 256), hdr);
    addPaintLayer(image, root, COMPOSITE_MULT, 220, 142, QRect(40, 30, 250, 150));
    // It leaves part of the image and of its own tiles empty.
    KisPaintLayerSP copied = addPaintLayer(image, root, COMPOSITE_COPY, 255, 143, QRect(90, 20, 200, 190), hdr);
    if (moved) {
        copied->paintDevice()->moveTo(QPoint(7, 5));
    }
    if (variant == "opacity") {
        copied->setOpacity(128);
    } else if (variant != "plain") {
        QBitArray flags(4, true);
        flags.setBit(3, variant != "alpha-off" && variant != "green-and-alpha-off");
        flags.setBit(0, variant != "red-off");
        flags.setBit(1, variant != "green-and-alpha-off");
        copied->setChannelFlags(flags);
    }
    addPaintLayer(image, root, COMPOSITE_SCREEN, 128, 144, QRect(150, 0, 230, 256));

    auto compare = [&](const char *stage) {
        const std::vector<float> gpu = readFloats(image->projection(), bounds);
        const std::vector<float> cpu = render(image, false);
        QPoint where;
        const float error = f16 ? maxHalfUlpDifference(cpu, gpu, &where, bounds.width())
                                : maxDifference(cpu, gpu, &where, bounds.width());
        QVERIFY2(error <= (f16 ? 4.0f : 2e-5f),
                 qPrintable(QString("%1: difference %2 at %3,%4, %5")
                                .arg(stage)
                                .arg(error)
                                .arg(where.x())
                                .arg(where.y())
                                .arg(pixelReport(cpu, gpu, where, bounds.width()))));
    };

    render(image, true);
    compare("full refresh");

    render(image, true);
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    KisGpuMergeBatch::setEnabled(true);
    for (int i = 0; i < 3; ++i) {
        const QRect dab(30 + i * 90, 20 + i * 50, 128, 128);
        KisPaintDeviceSP dabDevice = new KisPaintDevice(rgbaFloat(f16));
        fillRandom(dabDevice, dab, 390 + i);
        if (moved) {
            // a GPU dab needs the layer on the tile grid
            KisPainter gc(copied->paintDevice());
            gc.setOpacityF(0.7);
            gc.bitBlt(dab.topLeft(), dabDevice, dab);
        } else {
            KisGpuProjectionCompositor::Layer layer;
            layer.device = dabDevice;
            layer.opacity = 0.7f;
            QString error;
            QVERIFY2(KisGpuProjectionCompositor::composite(copied->paintDevice(), dab, {layer}, &error),
                     qPrintable(error));
        }
        backend->context().waitIdle();
        const quint64 downloads = backend->downloadedTileCount();
        copied->setDirty(dab);
        image->waitForDone();
        backend->context().waitIdle();
        if (variant != "opacity") {
            QCOMPARE(backend->downloadedTileCount(), downloads);
        }
    }
    compare("partial updates");
}

void KisGpuProjectionTest::testColorTransformTables()
{
    // Phase 4.115: channel tables exist only for RGBA F16 and only for
    // transformations that treat every channel on its own.
    const KoColorSpace *f16 = rgbaFloat(true);
    const KoColorSpace *f32 = rgbaFloat(false);

    KisFilterSP curves = KisFilterRegistry::instance()->value("perchannel");
    QVERIFY(curves);
    const KisColorTransformationFilter *curvesFilter =
        dynamic_cast<const KisColorTransformationFilter *>(curves.data());
    QVERIFY(curvesFilter);

    KisFilterConfigurationSP allColors = curvesConfig({0});
    const KisGpuColorTransformSP tables = curvesFilter->gpuColorTransform(f16, allColors);
    QVERIFY(tables);
    QCOMPARE(tables->kind(), KisGpuColorTransform::ChannelTables);
    QVERIFY(tables->tablesId() != 0);
    // cached in the configuration
    QCOMPARE(curvesFilter->gpuColorTransform(f16, allColors).data(), tables.data());
    QVERIFY(!curvesFilter->gpuColorTransform(f32, allColors));
    QVERIFY(!curvesFilter->gpuColorTransform(f16, curvesConfig({7}))); // lightness
    QVERIFY(!curvesFilter->gpuColorTransform(f16, curvesConfig({0, 5}))); // hue

    // The tables repeat the transformation, also for values outside 0..1.
    QScopedPointer<KoColorTransformation> transformation(curvesFilter->createTransformation(f16, allColors));
    QVERIFY(transformation);
    std::mt19937 random(7);
    QVector<quint16> src(4 * 8192);
    for (quint16 &bits : src) {
        bits = quint16(random());
        if ((bits & 0x7c00) == 0x7c00) {
            bits &= quint16(~0x0400); // finite
        }
    }
    QVector<quint16> expected(src.size());
    QVector<quint16> looked(src.size());
    transformation->transform(reinterpret_cast<const quint8 *>(src.constData()),
                              reinterpret_cast<quint8 *>(expected.data()),
                              src.size() / 4);
    tables->applyTables(src.constData(), looked.data(), src.size() / 4);
    QCOMPARE(looked, expected);

    // A transformation that mixes the channels is refused.
    QHash<QString, QVariant> params;
    params["type"] = 1;
    QScopedPointer<KoColorTransformation> desaturate(f16->createColorTransformation("desaturate_adjustment", params));
    QVERIFY(desaturate);
    QVERIFY(!KisGpuColorTransform::channelTables(f16, desaturate.data()));
    QVERIFY(!KisGpuColorTransform::channelTables(f32, transformation.data()));

    QVERIFY(KisGpuColorTransform::desaturate(f32, 5));
    QVERIFY(!KisGpuColorTransform::desaturate(f32, 6));
    QVERIFY(!KisGpuColorTransform::desaturate(KoColorSpaceRegistry::instance()->rgb8(), 0));
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

void KisGpuProjectionTest::testConvertedDocumentUsesGpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::newRow("RGBA32F") << false;
    QTest::newRow("RGBA16F") << true;
}

void KisGpuProjectionTest::testConvertedDocumentUsesGpu()
{
    QFETCH(bool, f16);
    REQUIRE_GPU();
    KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();

    // Conversion targets (phase 3.3, KisGpuEngineSettings).
    const KoColorSpace *rgb8 = registry->rgb8();
    const auto policy = f16 ? KisGpuEngineSettings::Convert16 : KisGpuEngineSettings::Convert;
    const KoColorSpace *target = KisGpuEngineSettings::conversionTarget(rgb8, policy);
    QVERIFY(target);
    QVERIFY(KisGpuEngineSettings::isGpuColorSpace(target));
    QCOMPARE(target->colorDepthId(), f16 ? Float16BitsColorDepthID : Float32BitsColorDepthID);
    QCOMPARE(target->profile()->name(), rgb8->profile()->name()); // same profile: same appearance
    QVERIFY(!KisGpuEngineSettings::conversionTarget(rgbaFloat(), policy));
    QVERIFY(!KisGpuEngineSettings::conversionTarget(rgbaFloat(true), policy));
    const KoColorSpace *labTarget = KisGpuEngineSettings::conversionTarget(registry->lab16(), policy);
    QVERIFY(labTarget && KisGpuEngineSettings::isGpuColorSpace(labTarget));
    QCOMPARE(labTarget->colorDepthId(), target->colorDepthId());

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
