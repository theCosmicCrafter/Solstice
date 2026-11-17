/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <simpletest.h>

#include <QBuffer>
#include <QElapsedTimer>
#include <QFloat16>
#include <QObject>
#include <QPainter>
#include <QScopeGuard>
#include <QSemaphore>
#include <QThread>
#include <QThreadPool>

#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>

#include "filter/kis_filter.h"
#include "filter/kis_filter_configuration.h"
#include "filter/kis_filter_registry.h"
#include "kis_pixel_selection.h"
#include "kis_selection.h"
#include <KisGlobalResourcesInterface.h>
#include <KisGpuCommandList.h>
#include <KisGpuContext.h>
#include <KisGpuLayerCompositor.h>
#include <KisGpuLayerStackCompositor.h>
#include <KisGpuTileFill.h>

#include "KisPuppetTransformWorker.h"
#include "gpu/KisGpuConvolutionWorker.h"
#include "gpu/KisGpuGridWarpWorker.h"
#include "gpu/KisGpuMergeBatch.h"
#include "gpu/KisGpuTileAccess.h"
#include "gpu/KisGpuTileBackend.h"
#include "gpu/KisGpuTransformWorker.h"
#include "kis_datamanager.h"
#include "kis_filter_strategy.h"
#include "kis_gaussian_kernel.h"
#include "kis_liquify_transform_worker.h"
#include "kis_paint_device.h"
#include "kis_paint_device_writer.h"
#include "kis_painter.h"
#include "kis_transaction.h"
#include "kis_transform_worker.h"
#include "kis_types.h"
#include "kis_warptransform_worker.h"
#include "testing_timed_default_bounds.h"
#include "tiles3/kis_tile.h"
#include "tiles3/kis_tile_data_pooler.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/kis_tile_data_store_iterators.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
#include <vector>

/**
 * GPU engine phase 1: GPU-resident tiles of KisPaintDevice
 * (docs/agent/gpu-engine.md).
 */
class KisGpuPaintDeviceTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();

    void testSupport();
    void testSharedUploadArena_data();
    void testSharedUploadArena();
    void testRetiredResourcesReleasedOutsideLock();
    void testReadOnlyAccessKeepsTileDataAlive();
    void testTileAllocationBoundsResourceReclamation();
    void testGpuFillVisibleOnCpu();
    void testExactBoundsReadsBorderTilesOnly();
    void testBulkReadback_data();
    void testBulkReadback();
    void testCpuFiltersBatchReadback_data();
    void testCpuFiltersBatchReadback();
    void testCpuTransformsBatchReadback_data();
    void testCpuTransformsBatchReadback();
    void testTransformSequenceUndo();
    void testGpuTransformMatchesCpu_data();
    void testGpuTransformMatchesCpu();
    void testGpuLiquifyMatchesCpu_data();
    void testGpuLiquifyMatchesCpu();
    void testGpuPuppetMatchesCpu_data();
    void testGpuPuppetMatchesCpu();
    void testGpuGaussianMatchesCpu_data();
    void testGpuGaussianMatchesCpu();
    void testGpuGaussianPatchesMatchCpu();
    void testGpuGaussianFiltersMatchCpu_data();
    void testGpuGaussianFiltersMatchCpu();
    void testCompositeMatchesCpu();
    void testUndoRedo();
    void testCopyOnWriteIsolation();
    void testGpuCopyKeepsCpuSnapshot_data();
    void testGpuCopyKeepsCpuSnapshot();
    void testWholeTileCopyStaysOnGpu_data();
    void testWholeTileCopyStaysOnGpu();
    void testPartialClearBatchesReadback_data();
    void testPartialClearBatchesReadback();
    void testPoolerKeepsGpuTilesResident();
    void testCpuWriteAfterGpuWrite();
    void testSaveLoadRoundTrip();
    void testF16Device();
    void testSlotsReleased();
    void testSaveWithoutSync();
    void testLockedTileRejectsGpuWrite();
    void testDownloadFailureIsRetried();
    void testPersistentDownloadFailureIsReported();
    void testRetryAllocatesOnlyWhatItNeeds();
    void testNoSubmissionAfterFailure();
    void testFailedSubmissionKeepsContent();
    void testGpuValidCannotBePublishedAfterCpuWrite();
    void testOlderUploadDoesNotOverwriteNewer();
    void testSnapshotDuringCpuWriteIsNotPublished();
    void testDeferredSlotRelease();
    void testMemoryBudgetEviction();
    void testBatchedEviction_data();
    void testBatchedEviction();
    void testMixedEvictionBatch();
    void testPreparedAccessPreventsEviction();
    void testEvictionFailureKeepsContent();
    void testGpuTileDiskSwapRoundTrip();
    void testEvictionPreservesUndoRedo();
    void testCopySourcePreventsEviction();
    void testConcurrentEviction();
    void benchmarkTransfers();
    void benchmarkFiltersAndTransforms();

private:
    bool gpuFill(KisPaintDeviceSP device, const QRect &rect, const QVector<float> &color);
    bool gpuCompositeOver(KisPaintDeviceSP dst, KisPaintDeviceSP src, const QRect &rect, float opacity);

    KisGpuTileBackend *m_backend = nullptr;
    std::unique_ptr<KisGpuCommandList> m_commands;
    std::unique_ptr<KisGpuTileFill> m_fill32;
    std::unique_ptr<KisGpuTileFill> m_fill16;
    std::unique_ptr<KisGpuLayerStackCompositor> m_compositor32;
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

void fillRandom(KisPaintDeviceSP device, const QRect &rect, quint32 seed)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::vector<float> pixels(size_t(rect.width()) * rect.height() * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = unit(random);
        pixels[i + 1] = unit(random);
        pixels[i + 2] = unit(random);
        const float a = unit(random);
        pixels[i + 3] = a < 0.2f ? 0.0f : (a < 0.4f ? 1.0f : unit(random));
    }
    device->writeBytes(reinterpret_cast<const quint8 *>(pixels.data()), rect);
}

std::vector<float> readPixels(KisPaintDeviceSP device, const QRect &rect)
{
    std::vector<float> pixels(size_t(rect.width()) * rect.height() * 4);
    device->readBytes(reinterpret_cast<quint8 *>(pixels.data()), rect);
    return pixels;
}

/// Pixels as floats, from RGBA32F or RGBA16F devices.
std::vector<float> readFloats(KisPaintDeviceSP device, const QRect &rect)
{
    if (device->pixelSize() != 8)
        return readPixels(device, rect);
    std::vector<qfloat16> halves(size_t(rect.width()) * rect.height() * 4);
    device->readBytes(reinterpret_cast<quint8 *>(halves.data()), rect);
    std::vector<float> pixels(halves.size());
    for (size_t i = 0; i < halves.size(); ++i)
        pixels[i] = float(halves[i]);
    return pixels;
}

/// fillRandom() for RGBA32F and RGBA16F devices; colors become color * scale + offset.
void fillRandomAny(KisPaintDeviceSP device, const QRect &rect, quint32 seed, float scale = 1.0f, float offset = 0.0f)
{
    const KoColorSpace *f32 = rgbaFloat(false);
    KisPaintDeviceSP floats = new KisPaintDevice(f32);
    fillRandom(floats, rect, seed);
    std::vector<float> pixels = readPixels(floats, rect);
    for (size_t i = 0; i < pixels.size(); ++i)
        if (i % 4 != 3)
            pixels[i] = pixels[i] * scale + offset;
    if (device->pixelSize() != 8) {
        device->writeBytes(reinterpret_cast<const quint8 *>(pixels.data()), rect);
        return;
    }
    QByteArray raw(int(pixels.size() / 4) * device->pixelSize(), Qt::Uninitialized);
    f32->convertPixelsTo(reinterpret_cast<const quint8 *>(pixels.data()),
                         reinterpret_cast<quint8 *>(raw.data()),
                         device->colorSpace(),
                         rect.width() * rect.height(),
                         KoColorConversionTransformation::internalRenderingIntent(),
                         KoColorConversionTransformation::internalConversionFlags());
    device->writeBytes(reinterpret_cast<const quint8 *>(raw.constData()), rect);
}

/**
 * Results that cannot be bit-identical (the FFT-based Gaussian blur of phase
 * 4.98, Puppet Warp's "over" composite of phase 4.96): channels within a
 * relative tolerance (1e-5 for F32, 1e-3 for F16, about one half ulp),
 * colors ignored where either alpha is near the FFT worker's null threshold.
 * Returns the number of failing channels; logs the first.
 */
int toleranceMismatches(const std::vector<float> &expected,
                        const std::vector<float> &actual,
                        const QRect &rect,
                        bool f16,
                        double *worstRelative = nullptr)
{
    const double tolerance = f16 ? 1e-3 : 1e-5;
    const float nullAlpha = f16 ? 2.0f / 1024 : 2.4e-7f;
    int failures = 0;
    double worst = 0.0;
    for (size_t pixel = 0; pixel < expected.size() / 4; ++pixel) {
        const bool alphaOnly = qMin(expected[pixel * 4 + 3], actual[pixel * 4 + 3]) < nullAlpha;
        for (size_t c = alphaOnly ? 3 : 0; c < 4; ++c) {
            const double e = expected[pixel * 4 + c];
            const double a = actual[pixel * 4 + c];
            const double relative = std::abs(a - e) / qMax(1.0, std::abs(e));
            if (!(relative <= tolerance)) {
                if (failures++ < 5)
                    qInfo() << "mismatch at"
                            << rect.topLeft() + QPoint(int(pixel) % rect.width(), int(pixel) / rect.width())
                            << "channel" << c << "CPU" << e << "GPU" << a;
            } else {
                worst = qMax(worst, relative);
            }
        }
    }
    if (worstRelative)
        *worstRelative = worst;
    return failures;
}

/// Largest channel difference; the color of fully transparent pixels is ignored.
float maxDifference(const std::vector<float> &a, const std::vector<float> &b)
{
    float worst = 0.0f;
    for (size_t pixel = 0; pixel < a.size() / 4; pixel++) {
        const bool transparent = a[pixel * 4 + 3] == 0.0f && b[pixel * 4 + 3] == 0.0f;
        for (size_t c = transparent ? 3 : 0; c < 4; c++) {
            const float difference = std::abs(a[pixel * 4 + c] - b[pixel * 4 + c]);
            worst = std::isnan(difference) ? INFINITY : qMax(worst, difference);
        }
    }
    return worst;
}

class BufferWriter : public KisPaintDeviceWriter
{
public:
    explicit BufferWriter(QBuffer *buffer)
        : m_buffer(buffer)
    {
    }
    bool write(const QByteArray &data) override
    {
        return m_buffer->write(data) == data.size();
    }
    bool write(const char *data, qint64 length) override
    {
        return m_buffer->write(data, length) == length;
    }

private:
    QBuffer *m_buffer;
};
} // namespace

#define REQUIRE_GPU()                                                                                                  \
    if (!m_backend) {                                                                                                  \
        QSKIP(qPrintable(QStringLiteral("GPU engine unavailable: %1").arg(KisGpuTileBackend::unavailableReason())));   \
    }

void KisGpuPaintDeviceTest::initTestCase()
{
    m_backend = KisGpuTileBackend::instance();
    if (!m_backend) {
        return;
    }
    QString error;
    m_commands.reset(new KisGpuCommandList(m_backend->context()));
    m_fill32 = KisGpuTileFill::create(m_backend->context(), KisGpuTileFormat::RGBA32F, &error);
    m_fill16 = KisGpuTileFill::create(m_backend->context(), KisGpuTileFormat::RGBA16F, &error);
    m_compositor32 = KisGpuLayerStackCompositor::create(m_backend->context(), KisGpuTileFormat::RGBA32F, &error);
    QVERIFY2(m_fill32 && m_fill16 && m_compositor32, qPrintable(error));
}

void KisGpuPaintDeviceTest::cleanupTestCase()
{
    if (m_backend) {
        m_backend->flush();
        QCOMPARE(m_backend->context().validationErrorCount(), 0);
    }
    m_compositor32.reset();
    m_fill16.reset();
    m_fill32.reset();
    m_commands.reset();
}

void KisGpuPaintDeviceTest::init()
{
    // Failure injection must not leak from one test into the next.
    if (m_backend) {
        m_backend->injectDownloadFailuresForTesting(0);
        m_backend->injectReadbackLimitForTesting(~quint64(0));
        m_backend->resetFailureForTesting();
        m_backend->context().injectSubmitFailuresForTesting(0);
    }
}

bool KisGpuPaintDeviceTest::gpuFill(KisPaintDeviceSP device, const QRect &rect, const QVector<float> &color)
{
    KisGpuTileFill *fill = device->pixelSize() == 8 ? m_fill16.get() : m_fill32.get();
    KisGpuTileAccess access(device, rect, KisGpuTileAccess::WriteOnly);
    m_commands->begin();
    QString error;
    if (!access.prepare(*m_commands, &error)
        || !fill->record(*m_commands, access.addresses(), color.constData(), &error)) {
        qWarning() << error;
        return false;
    }
    const quint64 value = KisGpuTileAccess::submitAndFinish(*m_commands, {&access});
    device->setDirty(rect);
    return value && m_commands->wait();
}

bool KisGpuPaintDeviceTest::gpuCompositeOver(KisPaintDeviceSP dst,
                                             KisPaintDeviceSP src,
                                             const QRect &rect,
                                             float opacity)
{
    KisGpuTileAccess srcAccess(src, rect, KisGpuTileAccess::ReadOnly);
    KisGpuTileAccess dstAccess(dst, rect, KisGpuTileAccess::ReadWrite);
    if (srcAccess.tileGrid() != dstAccess.tileGrid()) {
        qWarning() << "devices are not tile-aligned with each other";
        return false;
    }

    QString error;
    m_commands->begin();
    if (!srcAccess.prepare(*m_commands, &error) || !dstAccess.prepare(*m_commands, &error)) {
        qWarning() << error;
        return false;
    }

    // Stack: the destination as the bottom layer, the source over it.
    const QVector<VkDeviceAddress> dstTiles = dstAccess.addresses();
    const QVector<VkDeviceAddress> layerTiles = dstTiles + srcAccess.addresses();
    if (!m_compositor32->record(*m_commands, layerTiles, dstTiles, {1.0f, opacity}, &error)) {
        qWarning() << error;
        return false;
    }

    const quint64 value = KisGpuTileAccess::submitAndFinish(*m_commands, {&srcAccess, &dstAccess});
    dst->setDirty(rect);
    return value && m_commands->wait();
}

void KisGpuPaintDeviceTest::testRetiredResourcesReleasedOutsideLock()
{
    REQUIRE_GPU();
    m_backend->flush();
    QSemaphore entered, release, published;
    m_backend->retireAfter(0, std::shared_ptr<int>(new int, [&](int *p) {
                               entered.release();
                               release.acquire();
                               delete p;
                           }));
    std::thread collector([&] {
        m_backend->collectGarbage();
    });
    const bool destroying = entered.tryAcquire(1, 5000);
    std::thread publisher([&] {
        m_backend->retireAfter(0, std::make_shared<int>(42));
        published.release();
    });
    const bool unblocked = published.tryAcquire(1, 5000);
    // Always release/join before assertions so an old implementation fails
    // cleanly instead of leaving a blocked worker behind.
    release.release();
    publisher.join();
    collector.join();
    m_backend->flush();
    QVERIFY(destroying);
    QVERIFY2(unblocked, "retiring another upload must not wait for a slow resource destructor");
}

void KisGpuPaintDeviceTest::testTileAllocationBoundsResourceReclamation()
{
    REQUIRE_GPU();
    m_backend->flush();
    std::atomic<int> destroyed{0};
    for (int i = 0; i < 40; ++i) {
        m_backend->retireAfter(0, std::shared_ptr<int>(new int, [&](int *p) {
                                   ++destroyed;
                                   delete p;
                               }));
    }
    const QRect rect(0, 0, 64, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    fillRandom(device, rect, 937);
    KisGpuTileAccess access(device, rect, KisGpuTileAccess::ReadOnly);
    m_commands->begin();
    const bool prepared = access.prepare(*m_commands);
    const int reclaimed = destroyed.load();
    KisGpuTileAccess::finishUnsubmitted(*m_commands, {&access});
    m_backend->flush();
    QVERIFY(prepared);
    QCOMPARE(reclaimed, 16);
    QCOMPARE(destroyed.load(), 40);
}

void KisGpuPaintDeviceTest::testSharedUploadArena_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<bool>("failSubmit");
    QTest::addColumn<bool>("oversized");
    for (bool f16 : {false, true})
        for (bool fail : {false, true})
            for (bool oversized : {false, true})
                QTest::newRow(qPrintable(QString("f16%1-fail%2-oversized%3").arg(f16).arg(fail).arg(oversized)))
                    << f16 << fail << oversized;
}

void KisGpuPaintDeviceTest::testSharedUploadArena()
{
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(bool, failSubmit);
    QFETCH(bool, oversized);
    const auto *cs = rgbaFloat(f16);
    QVERIFY(cs);
    const QRect sourceRect(0, 0, 256, 128);
    const QRect bounds(0, 0, 256, 5 * 128);
    const VkDeviceSize bytes = VkDeviceSize(sourceRect.width()) * sourceRect.height() * cs->pixelSize();
    KisPaintDeviceSP output = new KisPaintDevice(cs), expected = new KisPaintDevice(cs);
    KoColor original(cs);
    cs->fromNormalisedChannelsValue(original.data(), {0.25f, 0.5f, 0.75f, 1.0f});
    output->fill(bounds, original);
    expected->fill(bounds, original);
    KisGpuTileAccess target(output, bounds, KisGpuTileAccess::WriteOnly);
    std::vector<std::unique_ptr<KisGpuTileAccess>> sources;
    QVector<KisGpuTileAccess *> accesses{&target};
    QVector<VkDeviceAddress> addresses;
    QString error;
    m_commands->begin();
    QVERIFY(target.prepare(*m_commands, &error));
    {
        KisGpuTileAccess::UploadArena arena(oversized ? bytes / 2 : 2 * bytes);
        for (int i = 0; i < 5; ++i) {
            KisPaintDeviceSP source = new KisPaintDevice(cs);
            KoColor color(cs);
            cs->fromNormalisedChannelsValue(color.data(), {i / 8.0f, (5 - i) / 8.0f, 0.125f, 1.0f});
            // writeBytes ensures independent tile data (fill may share tiles),
            // making the upload size and chunk-boundary assertions deterministic.
            QByteArray pixels(int(bytes), Qt::Uninitialized);
            for (int p = 0; p < sourceRect.width() * sourceRect.height(); ++p)
                std::memcpy(pixels.data() + p * cs->pixelSize(), color.data(), cs->pixelSize());
            source->writeBytes(reinterpret_cast<const quint8 *>(pixels.constData()), sourceRect);
            auto access = std::make_unique<KisGpuTileAccess>(source, sourceRect, KisGpuTileAccess::ReadOnly);
            QVERIFY2(access->prepare(*m_commands, arena, &error), qPrintable(error));
            addresses += access->addresses();
            accesses << access.get();
            sources.push_back(std::move(access));
            if (!failSubmit)
                expected->fill(sourceRect.translated(0, i * 128), color);
        }
        QCOMPARE(arena.allocationCount(), oversized ? 5 : 3);
        QCOMPARE(arena.reservedBytes(), bytes * 5);
    } // The arena dies before submission; each access must retain its storage.
    auto compositor = KisGpuLayerCompositor::create(m_backend->context(),
                                                    f16 ? KisGpuTileFormat::RGBA16F : KisGpuTileFormat::RGBA32F,
                                                    &error);
    QVERIFY2(compositor, qPrintable(error));
    QVERIFY2(compositor->record(*m_commands,
                                addresses,
                                target.addresses(),
                                {{KisGpuBlendOp::Over, 1.0f, false, 0xf}},
                                4,
                                bounds,
                                &error),
             qPrintable(error));
    if (failSubmit)
        m_backend->context().injectSubmitFailuresForTesting(1);
    const auto value = KisGpuTileAccess::submitAndFinish(*m_commands, accesses);
    QCOMPARE(value == 0, failSubmit);
    // Accesses also die before completion; deferred retirement owns all chunks.
    sources.clear();
    if (value)
        QVERIFY(m_commands->wait());
    QByteArray actual(bounds.width() * bounds.height() * cs->pixelSize(), Qt::Uninitialized);
    QByteArray reference(actual.size(), Qt::Uninitialized);
    output->readBytes(reinterpret_cast<quint8 *>(actual.data()), bounds);
    expected->readBytes(reinterpret_cast<quint8 *>(reference.data()), bounds);
    QCOMPARE(actual, reference);
}

void KisGpuPaintDeviceTest::testBulkReadback_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<bool>("planar");
    QTest::addColumn<bool>("retry");
    for (bool f16 : {false, true}) {
        for (bool planar : {false, true}) {
            for (bool retry : {false, true}) {
                const QByteArray name =
                    QByteArray(f16 ? "F16" : "F32") + (planar ? "-planar" : "-strided") + (retry ? "-retry" : "");
                QTest::newRow(name.constData()) << f16 << planar << retry;
            }
        }
    }
}

void KisGpuPaintDeviceTest::testBulkReadback()
{
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(bool, planar);
    QFETCH(bool, retry);
    const KoColorSpace *cs = rgbaFloat(f16);
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    KisPaintDeviceSP expected = new KisPaintDevice(cs);
    const QPoint offset(7, -11);
    device->moveTo(offset);
    expected->moveTo(offset);
    // 289 + 1 stale tiles cross the 256-tile batch boundary, with sparse holes,
    // negative coordinates, a device offset and an unaligned read rectangle.
    const QRect red = QRect(-128, -128, 17 * 64, 17 * 64).translated(offset);
    const QRect blue = QRect(18 * 64, 18 * 64, 64, 64).translated(offset);
    QVERIFY(gpuFill(device, red, {1, 0, 0, 1}));
    QVERIFY(gpuFill(device, blue, {0, 0, 1, 1}));
    KoColor redPixel(cs);
    KoColor bluePixel(cs);
    cs->fromNormalisedChannelsValue(redPixel.data(), {1, 0, 0, 1});
    cs->fromNormalisedChannelsValue(bluePixel.data(), {0, 0, 1, 1});
    expected->fill(red, redPixel);
    expected->fill(blue, bluePixel);
    const QRect rect = red.united(blue).adjusted(-3, -5, 9, 7);
    const QRect localRect = rect.translated(-offset);
    const QRect extent = device->extent();
    const int pixelSize = device->pixelSize();
    QByteArray reference(rect.width() * rect.height() * pixelSize, Qt::Uninitialized);
    expected->readBytes(reinterpret_cast<quint8 *>(reference.data()), rect);

    const quint64 before = m_backend->context().completedValue();
    device->dataManager()->readBytes(nullptr, -128, -128, 512, 512);
    quint8 unused = 0;
    device->dataManager()->readBytes(&unused, 0, 0, 0, 0);
    QCOMPARE(m_backend->context().completedValue(), before);
    if (retry) {
        m_backend->injectDownloadFailuresForTesting(1);
    }
    if (planar) {
        // The planar API takes data-manager coordinates (unlike readBytes).
        const QVector<quint8 *> planes =
            device->readPlanarBytes(localRect.x(), localRect.y(), rect.width(), rect.height());
        const auto cleanup = qScopeGuard([&]() {
            for (quint8 *plane : planes)
                delete[] plane;
        });
        QCOMPARE(planes.size(), 4);
        const int channelSize = pixelSize / 4;
        for (int i = 0; i < rect.width() * rect.height(); ++i) {
            for (int c = 0; c < 4; ++c) {
                QVERIFY(std::memcmp(planes[c] + i * channelSize,
                                    reference.constData() + i * pixelSize + c * channelSize,
                                    channelSize)
                        == 0);
            }
        }
    } else {
        const int rowBytes = rect.width() * pixelSize;
        const int stride = rowBytes + 37;
        QByteArray actual(stride * rect.height(), char(0x5a));
        device->dataManager()->readBytes(reinterpret_cast<quint8 *>(actual.data()),
                                         localRect.x(),
                                         localRect.y(),
                                         rect.width(),
                                         rect.height(),
                                         stride);
        for (int y = 0; y < rect.height(); ++y) {
            QCOMPARE(actual.mid(y * stride, rowBytes), reference.mid(y * rowBytes, rowBytes));
            QCOMPARE(actual.mid(y * stride + rowBytes, 37), QByteArray(37, char(0x5a)));
        }
    }
    // A failed batch retries its 256 tiles individually; otherwise two submissions.
    QCOMPARE(m_backend->context().completedValue() - before, quint64(retry ? 257 : 2));
    QCOMPARE(device->extent(), extent);
    QByteArray again(reference.size(), Qt::Uninitialized);
    const quint64 after = m_backend->context().completedValue();
    device->readBytes(reinterpret_cast<quint8 *>(again.data()), rect);
    QCOMPARE(again, reference);
    QCOMPARE(m_backend->context().completedValue(), after);
}

void KisGpuPaintDeviceTest::testCpuFiltersBatchReadback_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QString>("filterId");
    QTest::addColumn<int>("path");
    QTest::addColumn<bool>("retry");
    for (bool f16 : {false, true})
        for (const auto &id : {QStringLiteral("invert"), QStringLiteral("gaussian blur")})
            for (int path = 0; path < 3; ++path)
                for (bool retry : {false, true})
                    QTest::newRow(qPrintable(QString("%1-f16%2-path%3-retry%4").arg(id).arg(f16).arg(path).arg(retry)))
                        << f16 << id << path << retry;
}

void KisGpuPaintDeviceTest::testCpuFiltersBatchReadback()
{
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QString, filterId);
    QFETCH(int, path);
    QFETCH(bool, retry);
    auto filter = KisFilterRegistry::instance()->value(filterId);
    QVERIFY2(filter, qPrintable(filterId));
    auto config = filter->defaultConfiguration(KisGlobalResourcesInterface::instance())->cloneWithResourcesSnapshot();
    const auto *cs = rgbaFloat(f16);
    const QPoint offset(7, -11);
    const QRect bounds = QRect(-64, -64, 384, 384).translated(offset);
    const QRect patch = QRect(64, 64, 128, 128).translated(offset);
    const QRect applyRect = QRect(1, -3, 191, 185).translated(offset);
    KisPaintDeviceSP cpu = new KisPaintDevice(cs), gpu = new KisPaintDevice(cs);
    auto fillBoth = [&](KisPaintDeviceSP a, KisPaintDeviceSP b, const QRect &rect, const QVector<float> &channels) {
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), channels);
        a->fill(rect, color);
        return gpuFill(b, rect, channels);
    };
    cpu->moveTo(offset);
    gpu->moveTo(offset);
    cpu->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
    gpu->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
    QVERIFY(fillBoth(cpu, gpu, bounds, {0.2f, 0.4f, 0.8f, 0.7f}));
    QVERIFY(fillBoth(cpu, gpu, patch, {1.2f, -0.1f, 0.3f, 0.4f}));
    KisPaintDeviceSP cpuDst = cpu, gpuDst = gpu;
    if (path == 2) {
        cpuDst = new KisPaintDevice(cs);
        gpuDst = new KisPaintDevice(cs);
        cpuDst->moveTo(offset);
        gpuDst->moveTo(offset);
        cpuDst->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
        gpuDst->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
        QVERIFY(fillBoth(cpuDst, gpuDst, bounds, {0.7f, 0.2f, 0.5f, 0.8f}));
    }
    auto read = [&](KisPaintDeviceSP device) {
        QByteArray bytes(bounds.width() * bounds.height() * cs->pixelSize(), Qt::Uninitialized);
        device->readBytes(reinterpret_cast<quint8 *>(bytes.data()), bounds);
        return bytes;
    };
    const auto before = read(cpuDst);
    KisPaintDeviceSP snapshot = new KisPaintDevice(*gpuDst);
    KisSelectionSP selection;
    if (path) {
        selection = new KisSelection();
        QByteArray mask(applyRect.width() * applyRect.height(), Qt::Uninitialized);
        for (int i = 0; i < mask.size(); ++i)
            // Fractional coverage avoids the existing F32 CPU Copy SIMD/scalar
            // HDR-clamping difference at exactly opaque coverage.
            mask[i] = char(1 + i % 254);
        selection->pixelSelection()->writeBytes(reinterpret_cast<const quint8 *>(mask.constData()), applyRect);
    }
    filter->process(cpu, cpuDst, selection, applyRect, config);
    const auto expected = read(cpuDst);
    KisTransaction transaction(gpuDst);
    if (retry)
        m_backend->injectDownloadFailuresForTesting(1);
    const auto firstSubmission = m_backend->context().completedValue();
    filter->process(gpu, gpuDst, selection, applyRect, config);
    const auto downloads = m_backend->context().completedValue() - firstSubmission;
    qInfo() << "CPU filter readback submissions" << downloads;
    if (!retry) {
        const int limit = path == 2 || (path == 0 && filterId == "gaussian blur") ? 2 : 1;
        QVERIFY2(downloads > 0 && downloads <= quint64(limit), qPrintable(QString::number(downloads)));
    }
    QVERIFY(!m_backend->hasFailed());
    const auto after = read(gpuDst);
    float maximumError = 0;
    for (int i = 0; i < after.size(); i += f16 ? 2 : 4) {
        float a, b;
        if (f16) {
            qfloat16 ah, bh;
            std::memcpy(&ah, after.constData() + i, 2);
            std::memcpy(&bh, expected.constData() + i, 2);
            a = float(ah);
            b = float(bh);
        } else {
            std::memcpy(&a, after.constData() + i, 4);
            std::memcpy(&b, expected.constData() + i, 4);
        }
        QVERIFY(std::isfinite(a) && std::isfinite(b));
        maximumError = qMax(maximumError, std::abs(a - b));
    }
    qInfo() << "CPU filter maximum error" << maximumError;
    QVERIFY2(maximumError <= (f16 ? 1.0f / 1024 : 2e-5f), qPrintable(QString::number(maximumError)));
    QVERIFY(after != before);
    QCOMPARE(read(snapshot), before);
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    command->redo();
    command->undo();
    QCOMPARE(read(gpuDst), before);
    command->redo();
    QCOMPARE(read(gpuDst), after);
    // CPU writes must be uploaded before a later GPU operation sees them.
    KisGpuTileAccess access(gpuDst, bounds, KisGpuTileAccess::ReadOnly);
    m_commands->begin();
    QVERIFY(access.prepare(*m_commands));
    QVERIFY(KisGpuTileAccess::submitAndFinish(*m_commands, {&access}));
    QVERIFY(m_commands->wait());
    QCOMPARE(read(gpuDst), after);
}

void KisGpuPaintDeviceTest::testCpuTransformsBatchReadback_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<int>("operation");
    QTest::addColumn<bool>("retry");
    const QStringList operations = {"scale",
                                    "shear",
                                    "rotate90",
                                    "rotate",
                                    "mirrorX-axis",
                                    "mirrorY-axis",
                                    "translate",
                                    "scale-full",
                                    "mirrorX-center",
                                    "mirrorY-center",
                                    "scale-partial"};
    for (bool f16 : {false, true})
        for (int operation = 0; operation < operations.size(); ++operation)
            for (bool retry : {false, true})
                QTest::newRow(qPrintable(QString("f16%1-%2-retry%3").arg(f16).arg(operations[operation]).arg(retry)))
                    << f16 << operation << retry;
}

void KisGpuPaintDeviceTest::testCpuTransformsBatchReadback()
{
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(int, operation);
    QFETCH(bool, retry);
    const auto *cs = rgbaFloat(f16);
    const QPoint offset(7, -11);
    const QRect bounds = QRect(-64, -64, 256, 256).translated(offset);
    const QRect checkRect(-384, -384, 768, 768);
    KisPaintDeviceSP cpu = new KisPaintDevice(cs), gpu = new KisPaintDevice(cs);
    cpu->moveTo(offset);
    gpu->moveTo(offset);
    cpu->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
    gpu->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
    for (const auto &rect : {bounds, QRect(0, 64, 64, 64).translated(offset)}) {
        const QVector<float> channels =
            rect == bounds ? QVector<float>{0.2f, 0.4f, 0.8f, 0.7f} : QVector<float>{1.2f, -0.1f, 0.3f, 0.4f};
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), channels);
        cpu->fill(rect, color);
        QVERIFY(gpuFill(gpu, rect, channels));
    }
    auto read = [&](KisPaintDeviceSP device) {
        QByteArray bytes(checkRect.width() * checkRect.height() * cs->pixelSize(), Qt::Uninitialized);
        device->readBytes(reinterpret_cast<quint8 *>(bytes.data()), checkRect);
        return bytes;
    };
    auto transform = [&](KisPaintDeviceSP device) {
        if (operation == 8 || operation == 9) {
            if (operation == 8)
                KisTransformWorker::mirrorX(device);
            else
                KisTransformWorker::mirrorY(device);
            return true;
        }
        if (operation == 4 || operation == 5) {
            KisTransformWorker::mirror(device, 32.5, operation == 4 ? Qt::Horizontal : Qt::Vertical);
            return true;
        }
        KisBicubicFilterStrategy filter;
        const bool scale = operation == 0 || operation == 7 || operation == 10;
        KisTransformWorker worker(device,
                                  scale ? 0.75 : 1.0,
                                  scale ? 1.25 : 1.0,
                                  operation == 1 ? 0.2 : 0.0,
                                  operation == 1 ? -0.1 : 0.0,
                                  operation == 2       ? M_PI_2
                                      : operation == 3 ? 0.3
                                                       : 0.0,
                                  13,
                                  -9,
                                  KoUpdaterPtr(),
                                  &filter);
        return operation == 7 ? worker.run()
                              : worker.runPartial(operation == 10 ? bounds.adjusted(33, 17, -29, -19) : bounds);
    };
    const auto before = read(cpu);
    KisPaintDeviceSP snapshot = new KisPaintDevice(*gpu);
    QVERIFY(transform(cpu));
    const auto expected = read(cpu);
    KisTransaction transaction(gpu);
    if (retry)
        m_backend->injectDownloadFailuresForTesting(1);
    const auto firstSubmission = m_backend->context().completedValue();
    QVERIFY(transform(gpu));
    const auto downloads = m_backend->context().completedValue() - firstSubmission;
    qInfo() << "CPU transform readback submissions" << downloads;
    if (!retry)
        QCOMPARE(downloads, quint64(1));
    QVERIFY(!m_backend->hasFailed());
    const auto after = read(gpu);
    QVERIFY(after == expected);
    QVERIFY(after != before);
    QCOMPARE(gpu->exactBounds(), cpu->exactBounds());
    QVERIFY(read(snapshot) == before);
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    command->redo();
    command->undo();
    QVERIFY(read(gpu) == before);
    command->redo();
    QVERIFY(read(gpu) == after);
}

void KisGpuPaintDeviceTest::testGpuLiquifyMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QString>("stroke");
    QTest::addColumn<int>("precision");
    QTest::addColumn<QString>("variant");
    const char *strokes[] = {"push", "wash", "grow", "shrink", "rotate", "fold", "far", "undo", "none"};
    for (bool f16 : {false, true}) {
        for (const char *stroke : strokes) {
            for (const QString variant : {"plain", "offset", "opaque-default"}) {
                if (f16 && variant != "plain")
                    continue;
                QTest::newRow(qPrintable(QString("%1-%2-p8-%3").arg(f16 ? "f16" : "f32", stroke, variant)))
                    << f16 << QString(stroke) << 8 << variant;
            }
        }
        for (int precision : {1, 4, 16}) {
            QTest::newRow(qPrintable(QString("%1-push-p%2-plain").arg(f16 ? "f16" : "f32").arg(precision)))
                << f16 << QStringLiteral("push") << precision << QStringLiteral("plain");
        }
    }
}

void KisGpuPaintDeviceTest::testGpuLiquifyMatchesCpu()
{
    // Phase 4.97: KisLiquifyTransformWorker::run() with the polygons painted
    // on the GPU must equal the CPU bit for bit, including Undo.
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QString, stroke);
    QFETCH(int, precision);
    QFETCH(QString, variant);
    if (!m_backend->context().deviceInfo().supportsFloat64)
        QSKIP("shaderFloat64 is not supported");
    const bool previousProjection = KisGpuMergeBatch::isEnabled();
    KisGpuMergeBatch::setEnabled(true);
    const auto restore = qScopeGuard([&]() {
        KisGpuMergeBatch::setEnabled(previousProjection);
        qunsetenv("KRITA_GPU_LIQUIFY");
    });
    const auto *cs = rgbaFloat(f16);
    const QRect bounds(-64, -64, 1024, 1024);
    const QRect content(37, -21, 401, 363);
    KisPaintDeviceSP source = new KisPaintDevice(cs);
    source->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
    KoColor paper(cs);
    if (variant == "opaque-default") {
        cs->fromNormalisedChannelsValue(paper.data(), {0.95f, 0.9f, 0.85f, 1.0f});
        source->setDefaultPixel(paper);
    }
    if (f16) {
        KisPaintDeviceSP f32 = new KisPaintDevice(rgbaFloat(false));
        fillRandom(f32, content, 23);
        const auto floats = readPixels(f32, content);
        QByteArray raw(content.width() * content.height() * cs->pixelSize(), Qt::Uninitialized);
        rgbaFloat(false)->convertPixelsTo(reinterpret_cast<const quint8 *>(floats.data()),
                                          reinterpret_cast<quint8 *>(raw.data()),
                                          cs,
                                          content.width() * content.height(),
                                          KoColorConversionTransformation::internalRenderingIntent(),
                                          KoColorConversionTransformation::internalConversionFlags());
        source->writeBytes(reinterpret_cast<const quint8 *>(raw.constData()), content);
    } else {
        fillRandom(source, content, 23);
    }
    QRect srcBounds = content;
    if (variant == "offset") {
        source->moveTo(13, -7);
        srcBounds.translate(13, -7);
    }

    KisLiquifyTransformWorker worker(srcBounds, nullptr, precision);
    const QPointF center = QRectF(srcBounds).center();
    if (stroke == "push" || stroke == "undo") {
        for (int step = 0; step < 12; ++step)
            worker.translatePoints(center + QPointF(-120 + step * 18.3, -40 + step * 7.1),
                                   QPointF(6.5, 3.25),
                                   40,
                                   false,
                                   1.0);
        if (stroke == "undo")
            for (int step = 0; step < 6; ++step)
                worker.undoPoints(center + QPointF(-60 + step * 20.0, -10), 0.6, 35);
    } else if (stroke == "wash") {
        for (int step = 0; step < 12; ++step)
            worker.translatePoints(center + QPointF(-100 + step * 15.0, 20 - step * 4.0),
                                   QPointF(-4.0, 9.0),
                                   30,
                                   true,
                                   0.35);
    } else if (stroke == "grow" || stroke == "shrink") {
        for (int step = 0; step < 8; ++step)
            worker.scalePoints(center + QPointF(step * 3.7, -step * 2.9),
                               stroke == "grow" ? 0.12 : -0.15,
                               45,
                               false,
                               1.0);
    } else if (stroke == "rotate") {
        for (int step = 0; step < 8; ++step)
            worker.rotatePoints(center + QPointF(-step * 4.1, step * 1.3), 0.21, 50, false, 1.0);
    } else if (stroke == "fold") {
        // Large moves with a small radius: crossing, non-convex cells.
        worker.translatePoints(center, QPointF(90, -35), 12, false, 1.0);
        worker.translatePoints(center + QPointF(25, 10), QPointF(-110, 60), 9, false, 1.0);
        worker.rotatePoints(center + QPointF(-30, 30), 2.7, 15, false, 1.0);
    } else if (stroke == "far") {
        // Pushes content beyond the source bounds.
        worker.translatePoints(QPointF(srcBounds.left() + 10, srcBounds.top() + 20),
                               QPointF(-140, -90),
                               60,
                               false,
                               1.0);
    }

    auto makeDestination = [&]() {
        KisPaintDeviceSP dst = new KisPaintDevice(cs);
        dst->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
        if (variant == "opaque-default")
            dst->setDefaultPixel(paper);
        if (variant == "offset")
            dst->moveTo(13, -7); // the Transform Tool's devices share the offset
        return dst;
    };

    KisPaintDeviceSP cpu = makeDestination();
    qputenv("KRITA_GPU_LIQUIFY", "0");
    const quint64 before = KisGpuGridWarpWorker::runCount();
    worker.run(source, cpu);
    QCOMPARE(KisGpuGridWarpWorker::runCount(), before);

    KisPaintDeviceSP gpu = makeDestination();
    fillRandom(gpu, QRect(0, 0, 100, 100), 5); // run() clears the destination first
    qputenv("KRITA_GPU_LIQUIFY", "1");
    const QRect checkRect =
        (cpu->exactBounds() | source->exactBounds() | QRect(0, 0, 100, 100)).adjusted(-80, -80, 80, 80);
    const auto original = readPixels(gpu, checkRect);
    KisTransaction transaction(gpu);
    worker.run(source, gpu);
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    command->redo(); // the first redo after endAndTake() only arms the command
    QCOMPARE(KisGpuGridWarpWorker::runCount(), before + 1);

    const auto expected = readPixels(cpu, checkRect);
    const auto actual = readPixels(gpu, checkRect);
    if (expected != actual) {
        int mismatches = 0;
        for (size_t i = 0; i < expected.size(); ++i) {
            if (std::memcmp(&expected[i], &actual[i], sizeof(float))) {
                const int pixel = int(i / 4);
                if (mismatches++ < 5)
                    qInfo() << "mismatch at"
                            << checkRect.topLeft() + QPoint(pixel % checkRect.width(), pixel / checkRect.width())
                            << "channel" << i % 4 << "CPU" << expected[i] << "GPU" << actual[i];
            }
        }
        qInfo() << "mismatching channels" << mismatches;
    }
    QVERIFY(expected == actual);
    QCOMPARE(gpu->exactBounds(), cpu->exactBounds());
    command->undo();
    QVERIFY(readPixels(gpu, checkRect) == original);
    command->redo();
    QVERIFY(readPixels(gpu, checkRect) == expected);
}

void KisGpuPaintDeviceTest::testGpuPuppetMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<bool>("offset");
    for (bool f16 : {false, true})
        for (const char *scenario : {"move", "elbow", "fold", "orders", "elbow-back", "squeeze"})
            for (bool offset : {false, true}) {
                if (f16 && offset)
                    continue;
                QTest::newRow(
                    qPrintable(QString("%1-%2%3").arg(f16 ? "f16" : "f32", scenario, offset ? "-offset" : "")))
                    << f16 << QString(scenario) << offset;
            }
}

void KisGpuPaintDeviceTest::testGpuPuppetMatchesCpu()
{
    // Phase 4.96: KisPuppetTransformWorker::run() (the mesh ARAP model) with
    // the polygons painted on the GPU must equal the CPU bit for bit,
    // including the per-pin layers composited with "over", and Undo.
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QString, scenario);
    QFETCH(bool, offset);
    if (!m_backend->context().deviceInfo().supportsFloat64)
        QSKIP("shaderFloat64 is not supported");
    const bool previousProjection = KisGpuMergeBatch::isEnabled();
    KisGpuMergeBatch::setEnabled(true);
    const auto restore = qScopeGuard([&]() {
        KisGpuMergeBatch::setEnabled(previousProjection);
        qunsetenv("KRITA_GPU_PUPPET");
    });
    const auto *cs = rgbaFloat(f16);
    // A torso with an arm, as in KisPuppetTransformWorkerTest, scaled by 2.
    const QRectF meshBounds(0, 0, 800, 640);
    const QVector<QRect> parts{QRect(0, 100, 200, 400), QRect(200, 200, 280, 60), QRect(420, 260, 60, 320)};
    QImage mask(meshBounds.size().toSize(), QImage::Format_Grayscale8);
    mask.fill(0);
    {
        QPainter gc(&mask);
        for (const QRect &part : parts)
            gc.fillRect(part, Qt::white);
    }
    const QPointF torso(100, 300), shoulder(220, 230), elbow(450, 230);
    QVector<QPointF> original{torso, shoulder, elbow};
    QVector<QPointF> transformed = original;
    QVector<qreal> rotations{0.0, 0.0, 0.0};
    QVector<int> orders;
    if (scenario == "move") {
        original = {torso};
        transformed = {torso + QPointF(37.3, -21.6)};
        rotations = {0.3};
    } else if (scenario == "elbow") {
        rotations[2] = M_PI / 2;
    } else if (scenario == "fold") {
        rotations[2] = 2.4;
    } else if (scenario == "orders" || scenario == "elbow-back") {
        rotations[2] = M_PI / 2;
        orders = {0, 0, scenario == "orders" ? 1 : -1};
    } else if (scenario == "squeeze") {
        transformed[2] = elbow + QPointF(-160, 70);
        transformed[0] = torso + QPointF(30, -40);
        rotations[1] = -0.4;
    }
    const QPoint shift = offset ? QPoint(13, -7) : QPoint();
    KisPaintDeviceSP source = new KisPaintDevice(cs);
    source->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(QRect(-64, -64, 1024, 900)));
    for (int i = 0; i < parts.size(); ++i)
        fillRandomAny(source, parts[i], 71 + i);
    source->moveTo(shift);
    const auto mesh = KisPuppetTransformWorker::Mesh::build(mask, meshBounds.translated(shift), 0);
    for (QPointF &point : original)
        point += shift;
    for (QPointF &point : transformed)
        point += shift;
    KisPuppetTransformWorker worker(mesh, original, transformed, rotations, orders);
    QVERIFY(worker.isValid());
    if (scenario != "move")
        QVERIFY(worker.stackingGroups().size() > 1);

    auto makeDestination = [&]() {
        KisPaintDeviceSP dst = new KisPaintDevice(cs);
        dst->setDefaultBounds(source->defaultBounds());
        dst->moveTo(shift);
        return dst;
    };
    KisPaintDeviceSP cpu = makeDestination();
    qputenv("KRITA_GPU_PUPPET", "0");
    const quint64 before = KisGpuGridWarpWorker::runCount();
    worker.run(source, cpu);
    QCOMPARE(KisGpuGridWarpWorker::runCount(), before);

    KisPaintDeviceSP gpu = makeDestination();
    fillRandomAny(gpu, QRect(600, 0, 100, 100), 5); // run() clears the destination first
    qputenv("KRITA_GPU_PUPPET", "1");
    const QRect checkRect =
        (cpu->exactBounds() | source->exactBounds() | QRect(600, 0, 100, 100)).adjusted(-40, -40, 40, 40);
    const auto original0 = readFloats(gpu, checkRect);
    KisTransaction transaction(gpu);
    worker.run(source, gpu);
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    command->redo(); // the first redo after endAndTake() only arms the command
    QCOMPARE(KisGpuGridWarpWorker::runCount(), before + quint64(worker.stackingGroups().size()));

    const auto expected = readFloats(cpu, checkRect);
    const auto actual = readFloats(gpu, checkRect);
    // One group is painted straight into the destination: bit-identical.
    // Several groups are composited with "over" on the CPU in both paths.
    // For RGBA32F that op is not reproducible even on the CPU: F32 tiles are
    // malloc()ed (16-byte alignment), and the SIMD/scalar split of each row
    // follows the address, so repeated CPU runs differ by a few ulps in some
    // channels. The phase 0 compositing rule (1e-5) applies there.
    const bool exact = f16 || worker.stackingGroups().size() == 1;
    if (exact) {
        QVERIFY(expected == actual);
    } else {
        QCOMPARE(toleranceMismatches(expected, actual, checkRect, false), 0);
    }
    QCOMPARE(gpu->exactBounds(), cpu->exactBounds());
    command->undo();
    QVERIFY(readFloats(gpu, checkRect) == original0);
    command->redo();
    QVERIFY(readFloats(gpu, checkRect) == actual);
}

void KisGpuPaintDeviceTest::testGpuGaussianMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<double>("xRadius");
    QTest::addColumn<double>("yRadius");
    QTest::addColumn<QString>("variant");
    const QVector<QPair<double, double>> radii{{1, 1}, {5, 5}, {30, 30}, {100, 100}, {12, 3}, {0, 7}, {2.5, 40}};
    const QStringList
        variants{"offset", "partial", "ignore-border", "rgb-only", "alpha-only", "no-green", "opaque-default", "hdr"};
    for (bool f16 : {false, true}) {
        for (const auto &radius : radii)
            QTest::newRow(
                qPrintable(QString("%1-r%2x%3-plain").arg(f16 ? "f16" : "f32").arg(radius.first).arg(radius.second)))
                << f16 << radius.first << radius.second << QStringLiteral("plain");
        for (const QString &variant : variants)
            QTest::newRow(qPrintable(QString("%1-r9x9-%2").arg(f16 ? "f16" : "f32", variant)))
                << f16 << 9.0 << 9.0 << variant;
    }
}

void KisGpuPaintDeviceTest::testGpuGaussianMatchesCpu()
{
    // Phase 4.98: KisGaussianKernel::applyGaussian() on the GPU against the
    // CPU FFT convolution, with Undo/Redo and untouched pixels outside the rect.
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(double, xRadius);
    QFETCH(double, yRadius);
    QFETCH(QString, variant);
    if (!m_backend->context().deviceInfo().supportsFloat64)
        QSKIP("shaderFloat64 is not supported");
    const bool previousProjection = KisGpuMergeBatch::isEnabled();
    KisGpuMergeBatch::setEnabled(true);
    const auto restore = qScopeGuard([&]() {
        KisGpuMergeBatch::setEnabled(previousProjection);
        qunsetenv("KRITA_GPU_CONVOLUTION");
    });
    const auto *cs = rgbaFloat(f16);
    const QRect bounds(-64, -64, 760, 620);
    const QRect content(-30, -41, 610, 503);
    const QPoint shift = variant == "offset" ? QPoint(13, -7) : QPoint();
    const QRect applyRect = (variant == "partial" ? QRect(37, 11, 301, 257) : bounds).translated(shift);
    const KisConvolutionBorderOp borderOp = variant == "ignore-border" ? BORDER_IGNORE : BORDER_REPEAT;
    // Flags follow the color space's channel list: R, G, B, A for RGBA float.
    QBitArray channelFlags(4, true);
    if (variant == "rgb-only")
        channelFlags.clearBit(3);
    if (variant == "alpha-only") {
        channelFlags.fill(false);
        channelFlags.setBit(3);
    }
    if (variant == "no-green")
        channelFlags.clearBit(1);

    auto makeDevice = [&]() {
        KisPaintDeviceSP device = new KisPaintDevice(cs);
        device->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
        if (variant == "opaque-default") {
            KoColor paper(cs);
            cs->fromNormalisedChannelsValue(paper.data(), {0.95f, 0.9f, 0.85f, 1.0f});
            device->setDefaultPixel(paper);
        }
        fillRandomAny(device, content, 31, variant == "hdr" ? 40.0f : 1.0f);
        device->moveTo(shift);
        return device;
    };
    const QRect checkRect = bounds.adjusted(-40, -40, 40, 40).translated(shift);

    KisPaintDeviceSP cpu = makeDevice();
    qputenv("KRITA_GPU_CONVOLUTION", "0");
    const quint64 before = KisGpuConvolutionWorker::runCount();
    KisGaussianKernel::applyGaussian(cpu, applyRect, xRadius, yRadius, channelFlags, nullptr, false, borderOp);
    QCOMPARE(KisGpuConvolutionWorker::runCount(), before);

    KisPaintDeviceSP gpu = makeDevice();
    const auto original = readFloats(gpu, checkRect);
    qputenv("KRITA_GPU_CONVOLUTION", "1");
    KisTransaction transaction(gpu);
    KisGaussianKernel::applyGaussian(gpu, applyRect, xRadius, yRadius, channelFlags, nullptr, false, borderOp);
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    command->redo(); // the first redo after endAndTake() only arms the command
    QCOMPARE(KisGpuConvolutionWorker::runCount(), before + 1);

    const auto expected = readFloats(cpu, checkRect);
    const auto actual = readFloats(gpu, checkRect);
    double worst = 0.0;
    const int failures = toleranceMismatches(expected, actual, checkRect, f16, &worst);
    qInfo() << "worst relative difference" << worst;
    QCOMPARE(failures, 0);
    // Outside the apply rect, nothing changes (bit for bit).
    for (int y = 0; y < checkRect.height(); ++y)
        for (int x = 0; x < checkRect.width(); ++x) {
            if (applyRect.contains(checkRect.topLeft() + QPoint(x, y)))
                continue;
            const size_t i = (size_t(y) * checkRect.width() + x) * 4;
            QVERIFY(std::memcmp(&actual[i], &original[i], 4 * sizeof(float)) == 0);
        }
    command->undo();
    QVERIFY(readFloats(gpu, checkRect) == original);
    command->redo();
    QVERIFY(readFloats(gpu, checkRect) == actual);
}

void KisGpuPaintDeviceTest::testGpuGaussianPatchesMatchCpu()
{
    // A filter stroke's concurrent patches: each patch reads the old data of
    // its neighbours under one transaction, and writes only its own rect.
    REQUIRE_GPU();
    if (!m_backend->context().deviceInfo().supportsFloat64)
        QSKIP("shaderFloat64 is not supported");
    const bool previousProjection = KisGpuMergeBatch::isEnabled();
    KisGpuMergeBatch::setEnabled(true);
    const auto restore = qScopeGuard([&]() {
        KisGpuMergeBatch::setEnabled(previousProjection);
        qunsetenv("KRITA_GPU_CONVOLUTION");
    });
    const auto *cs = rgbaFloat();
    const QRect bounds(0, 0, 900, 700);
    const QRect processRect(3, 5, 890, 690);
    QVector<QRect> patches;
    for (int y = processRect.top(); y <= processRect.bottom(); y += 173)
        for (int x = processRect.left(); x <= processRect.right(); x += 211)
            patches << (QRect(x, y, 211, 173) & processRect);
    auto run = [&](bool gpuEnabled) {
        qputenv("KRITA_GPU_CONVOLUTION", gpuEnabled ? "1" : "0");
        KisPaintDeviceSP device = new KisPaintDevice(cs);
        device->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
        fillRandomAny(device, bounds, 41);
        KisTransaction transaction(device);
        QThreadPool pool;
        pool.setMaxThreadCount(6);
        for (const QRect &patch : patches)
            pool.start([device, patch]() {
                KisGaussianKernel::applyGaussian(device, patch, 15, 15, QBitArray(4, true), nullptr);
            });
        pool.waitForDone();
        transaction.end();
        return readFloats(device, bounds);
    };
    const auto expected = run(false);
    const quint64 before = KisGpuConvolutionWorker::runCount();
    const auto actual = run(true);
    // The narrow patches at the right and bottom edges are below the GPU's
    // minimum size and stay on the CPU, mixed with GPU patches.
    const quint64 gpuRuns = KisGpuConvolutionWorker::runCount() - before;
    QVERIFY2(gpuRuns > 0 && gpuRuns < quint64(patches.size()), qPrintable(QString::number(gpuRuns)));
    QCOMPARE(toleranceMismatches(expected, actual, bounds, false), 0);
}

void KisGpuPaintDeviceTest::testGpuGaussianFiltersMatchCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QString>("filterId");
    for (bool f16 : {false, true})
        for (const QString id : {"gaussian blur", "unsharp", "gaussianhighpass"})
            QTest::newRow(qPrintable(QString("%1-%2").arg(f16 ? "f16" : "f32", id))) << f16 << id;
}

void KisGpuPaintDeviceTest::testGpuGaussianFiltersMatchCpu()
{
    // The filters built on applyGaussian(), through KisFilter::process() with
    // a separate destination, with and without a selection.
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QString, filterId);
    if (!m_backend->context().deviceInfo().supportsFloat64)
        QSKIP("shaderFloat64 is not supported");
    auto filter = KisFilterRegistry::instance()->value(filterId);
    if (!filter)
        QSKIP(qPrintable(QString("filter %1 is not loaded").arg(filterId)));
    auto config = filter->defaultConfiguration(KisGlobalResourcesInterface::instance())->cloneWithResourcesSnapshot();
    if (filterId == "gaussian blur") {
        config->setProperty("horizRadius", 11);
        config->setProperty("vertRadius", 7);
    } else if (filterId == "unsharp") {
        config->setProperty("halfSize", 6);
    } else {
        config->setProperty("blurAmount", 8);
    }
    const bool previousProjection = KisGpuMergeBatch::isEnabled();
    KisGpuMergeBatch::setEnabled(true);
    const auto restore = qScopeGuard([&]() {
        KisGpuMergeBatch::setEnabled(previousProjection);
        qunsetenv("KRITA_GPU_CONVOLUTION");
    });
    const auto *cs = rgbaFloat(f16);
    const QRect bounds(0, 0, 520, 410);
    const QRect applyRect(17, 9, 471, 377);
    // Unsharp Mask results stay below 1: the CPU Copy composite op of the
    // selected copy clamps colors to 1 per SIMD batch only when some pixel of
    // the batch is not opaque, so a 1-ulp alpha difference would change the
    // clamping of whole batches.
    const float scale = filterId == "unsharp" ? 0.4f : 1.0f;
    const float offset = filterId == "unsharp" ? 0.25f : 0.0f;
    auto run = [&](bool gpuEnabled, bool withSelection) {
        qputenv("KRITA_GPU_CONVOLUTION", gpuEnabled ? "1" : "0");
        KisPaintDeviceSP src = new KisPaintDevice(cs), dst = new KisPaintDevice(cs);
        src->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
        dst->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
        fillRandomAny(src, bounds, 53, scale, offset);
        fillRandomAny(dst, bounds, 59, scale, offset);
        KisSelectionSP selection;
        if (withSelection) {
            selection = new KisSelection();
            QByteArray mask(applyRect.width() * applyRect.height(), Qt::Uninitialized);
            for (int i = 0; i < mask.size(); ++i)
                mask[i] = char(1 + i % 254);
            selection->pixelSelection()->writeBytes(reinterpret_cast<const quint8 *>(mask.constData()), applyRect);
        }
        filter->process(src, dst, selection, applyRect, config);
        return readFloats(dst, bounds);
    };
    for (bool withSelection : {false, true}) {
        const auto expected = run(false, withSelection);
        const quint64 before = KisGpuConvolutionWorker::runCount();
        const auto actual = run(true, withSelection);
        QVERIFY(KisGpuConvolutionWorker::runCount() > before);
        double worst = 0.0;
        QCOMPARE(toleranceMismatches(expected, actual, bounds, f16, &worst), 0);
        qInfo() << "selection" << withSelection << "worst relative difference" << worst;
    }
}

void KisGpuPaintDeviceTest::testGpuTransformMatchesCpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QString>("filterId");
    QTest::addColumn<double>("xscale");
    QTest::addColumn<double>("yscale");
    QTest::addColumn<double>("xshear");
    QTest::addColumn<double>("yshear");
    QTest::addColumn<double>("degrees");
    QTest::addColumn<double>("dx");
    QTest::addColumn<double>("dy");
    QTest::addColumn<QString>("variant");
    struct Case {
        const char *name;
        double xscale, yscale, xshear, yshear, degrees, dx, dy;
    };
    const Case cases[] = {
        {"scale-rotate", 0.9, 0.9, 0, 0, 10, 120.3, 80.6},
        {"upscale-rotate", 1.7, 1.7, 0, 0, -30, -15.2, 7.9},
        {"downscale-rotate", 0.33, 0.41, 0, 0, 44, 3.3, -2.1},
        {"rotate-only", 1, 1, 0, 0, 5, 0.37, 0.61},
        {"subpixel-scale", 1.0004, 0.9996, 0, 0, 0, 0.5, 0.25},
        {"nonuniform", 0.6, 1.4, 0, 0, -12, 40, -30},
        {"flip-scale", -1.3, 0.8, 0, 0, 20, 10, 10},
        {"quadrant", 1.1, 0.9, 0, 0, 100, 5, 6},
        {"shear-scale", 1.2, 0.85, 0.3, 0.1, 0, 4, 2},
        {"shear-both", 1, 1, -0.25, 0.4, 0, 0, 0},
        {"tiny", 0.02, 0.03, 0, 0, 7, 0, 0},
    };
    for (bool f16 : {false, true}) {
        for (const Case &c : cases) {
            for (const QString variant : {"plain", "offset", "opaque-default"}) {
                if (f16 && variant != "plain")
                    continue;
                QTest::newRow(qPrintable(QString("%1-%2-Bicubic-%3").arg(f16 ? "f16" : "f32", c.name, variant)))
                    << f16 << QStringLiteral("Bicubic") << c.xscale << c.yscale << c.xshear << c.yshear << c.degrees
                    << c.dx << c.dy << variant;
            }
        }
        for (const QString filterId :
             {"Hermite", "NearestNeighbor", "Bilinear", "Bell", "BSpline", "Lanczos3", "Mitchell"}) {
            QTest::newRow(qPrintable(QString("%1-scale-rotate-%2-plain").arg(f16 ? "f16" : "f32", filterId)))
                << f16 << filterId << 0.7 << 1.3 << 0.0 << 0.0 << 17.0 << 2.5 << -3.5 << QStringLiteral("plain");
        }
    }
}

void KisGpuPaintDeviceTest::testGpuTransformMatchesCpu()
{
    // Phase 4.94: KisTransformWorker's resampling passes on the GPU must equal
    // the CPU passes bit for bit, including the bound rect, borders and Undo.
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QString, filterId);
    QFETCH(double, xscale);
    QFETCH(double, yscale);
    QFETCH(double, xshear);
    QFETCH(double, yshear);
    QFETCH(double, degrees);
    QFETCH(double, dx);
    QFETCH(double, dy);
    QFETCH(QString, variant);
    if (!m_backend->context().deviceInfo().supportsFloat64)
        QSKIP("shaderFloat64 is not supported");
    const bool previousProjection = KisGpuMergeBatch::isEnabled();
    KisGpuMergeBatch::setEnabled(true);
    const auto restore = qScopeGuard([&]() {
        KisGpuMergeBatch::setEnabled(previousProjection);
        qunsetenv("KRITA_GPU_TRANSFORM");
    });
    KisFilterStrategy *filter = KisFilterStrategyRegistry::instance()->value(filterId);
    QVERIFY(filter);
    const auto *cs = rgbaFloat(f16);
    const QRect bounds(-64, -64, 1024, 1024);
    const QRect content(37, -21, 301, 263);
    KisPaintDeviceSP source = new KisPaintDevice(cs);
    source->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
    if (variant == "opaque-default") {
        KoColor paper(cs);
        cs->fromNormalisedChannelsValue(paper.data(), {0.95f, 0.9f, 0.85f, 1.0f});
        source->setDefaultPixel(paper);
    }
    if (f16) {
        // Random F32 values converted to half by the color space.
        KisPaintDeviceSP f32 = new KisPaintDevice(rgbaFloat(false));
        fillRandom(f32, content, 11);
        const auto floats = readPixels(f32, content);
        QByteArray raw(content.width() * content.height() * cs->pixelSize(), Qt::Uninitialized);
        rgbaFloat(false)->convertPixelsTo(reinterpret_cast<const quint8 *>(floats.data()),
                                          reinterpret_cast<quint8 *>(raw.data()),
                                          cs,
                                          content.width() * content.height(),
                                          KoColorConversionTransformation::internalRenderingIntent(),
                                          KoColorConversionTransformation::internalConversionFlags());
        source->writeBytes(reinterpret_cast<const quint8 *>(raw.constData()), content);
    } else {
        fillRandom(source, content, 11);
    }
    if (variant == "offset")
        source->moveTo(13, -7);

    auto transform = [&](KisPaintDeviceSP device) {
        KisTransformWorker
            worker(device, xscale, yscale, xshear, yshear, degrees * M_PI / 180.0, dx, dy, KoUpdaterPtr(), filter);
        return worker.run();
    };
    KisPaintDeviceSP cpu = new KisPaintDevice(*source);
    qputenv("KRITA_GPU_TRANSFORM", "0");
    const quint64 before = KisGpuTransformWorker::runCount();
    QVERIFY(transform(cpu));
    QCOMPARE(KisGpuTransformWorker::runCount(), before);

    KisPaintDeviceSP gpu = new KisPaintDevice(*source);
    qputenv("KRITA_GPU_TRANSFORM", "1");
    const QRect checkRect = (cpu->exactBounds() | source->exactBounds()).adjusted(-80, -80, 80, 80);
    const auto original = readPixels(gpu, checkRect);
    KisTransaction transaction(gpu);
    QVERIFY(transform(gpu));
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    command->redo(); // the first redo after endAndTake() only arms the command
    QCOMPARE(KisGpuTransformWorker::runCount(), before + 1);

    const auto expected = readPixels(cpu, checkRect);
    const auto actual = readPixels(gpu, checkRect);
    if (expected != actual) {
        for (size_t i = 0; i < expected.size(); ++i) {
            if (std::memcmp(&expected[i], &actual[i], sizeof(float))) {
                const int pixel = int(i / 4);
                qInfo() << "first mismatch at"
                        << checkRect.topLeft() + QPoint(pixel % checkRect.width(), pixel / checkRect.width())
                        << "channel" << i % 4 << "CPU" << expected[i] << "GPU" << actual[i];
                break;
            }
        }
    }
    QVERIFY(expected == actual);
    QCOMPARE(gpu->exactBounds(), cpu->exactBounds());
    command->undo();
    QVERIFY(readPixels(gpu, checkRect) == original);
    command->redo();
    QVERIFY(readPixels(gpu, checkRect) == expected);
}

void KisGpuPaintDeviceTest::testTransformSequenceUndo()
{
    REQUIRE_GPU();
    for (bool f16 : {false, true}) {
        for (bool gpuBetween : {false, true}) {
            const auto *cs = rgbaFloat(f16);
            const QRect bounds(-64, -64, 192, 192);
            const QRect checkRect(-256, -256, 512, 512);
            KisPaintDeviceSP device = new KisPaintDevice(cs);
            device->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
            QVERIFY(gpuFill(device, bounds, {0.2f, 0.3f, 0.5f, 0.7f}));
            QVERIFY(gpuFill(device, QRect(64, 64, 64, 64), {1.2f, 0.1f, 0.4f, 0.9f}));
            auto read = [&]() {
                QByteArray bytes(checkRect.width() * checkRect.height() * cs->pixelSize(), Qt::Uninitialized);
                device->readBytes(reinterpret_cast<quint8 *>(bytes.data()), checkRect);
                return bytes;
            };
            const auto initial = read();
            auto makeGpuAuthoritative = [&]() {
                KisGpuTileAccess access(device, bounds, KisGpuTileAccess::ReadWrite);
                m_commands->begin();
                if (!access.prepare(*m_commands))
                    return false;
                return KisGpuTileAccess::submitAndFinish(*m_commands, {&access}) && m_commands->wait();
            };
            QVERIFY(makeGpuAuthoritative());
            KisTransaction first(device);
            KisTransformWorker::mirrorX(device);
            QScopedPointer<KUndo2Command> firstCommand(first.endAndTake());
            firstCommand->redo();
            const auto mirrored = read();
            QVERIFY(mirrored != initial);
            KisTransaction second(device);
            if (gpuBetween)
                QVERIFY(makeGpuAuthoritative());
            KisBicubicFilterStrategy filter;
            KisTransformWorker worker(device, 1, 1, 0, 0, M_PI_2, 0, 0, KoUpdaterPtr(), &filter);
            QVERIFY(worker.run());
            QScopedPointer<KUndo2Command> secondCommand(second.endAndTake());
            secondCommand->redo();
            const auto rotated = read();
            QVERIFY(rotated != mirrored);
            secondCommand->undo();
            QVERIFY(read() == mirrored);
            firstCommand->undo();
            QVERIFY(read() == initial);
            firstCommand->redo();
            QVERIFY(read() == mirrored);
            secondCommand->redo();
            QVERIFY(read() == rotated);
            qInfo() << "Sequential mirror/rotation Undo passed: F16" << f16 << "GPU between" << gpuBetween;
        }
    }
}

void KisGpuPaintDeviceTest::testSupport()
{
    KisPaintDeviceSP rgba8 = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    QString reason;
    QVERIFY(!KisGpuTileAccess::isSupported(rgba8, &reason));
    QVERIFY(reason.contains(QStringLiteral("not RGBA F32/F16")));

    REQUIRE_GPU();
    QVERIFY(KisGpuTileAccess::isSupported(new KisPaintDevice(rgbaFloat()), &reason));
    QVERIFY(KisGpuTileAccess::isSupported(new KisPaintDevice(rgbaFloat(true)), &reason));
}

void KisGpuPaintDeviceTest::testGpuFillVisibleOnCpu()
{
    REQUIRE_GPU();
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    const QRect content(0, 0, 256, 256);
    fillRandom(device, content, 1);
    const std::vector<float> before = readPixels(device, content);

    // Tiles (1,1)..(2,2), plus a tile outside the old extent.
    const QRect filled(64, 64, 128, 128);
    const QRect outside(320, 0, 64, 64);
    QVERIFY(gpuFill(device, filled, {0.25f, 0.5f, 0.75f, 1.0f}));
    QVERIFY(gpuFill(device, outside, {1.0f, 0.0f, 0.0f, 0.5f}));

    const std::vector<float> after = readPixels(device, content);
    for (int y = 0; y < content.height(); y++) {
        for (int x = 0; x < content.width(); x++) {
            const size_t i = (size_t(y) * content.width() + x) * 4;
            if (filled.contains(x, y)) {
                QCOMPARE(after[i + 0], 0.25f);
                QCOMPARE(after[i + 1], 0.5f);
                QCOMPARE(after[i + 2], 0.75f);
                QCOMPARE(after[i + 3], 1.0f);
            } else {
                for (int c = 0; c < 4; c++) {
                    QCOMPARE(after[i + c], before[i + c]);
                }
            }
        }
    }

    const std::vector<float> outsidePixels = readPixels(device, outside);
    QCOMPARE(outsidePixels[0], 1.0f);
    QCOMPARE(outsidePixels[3], 0.5f);
    QCOMPARE(device->extent(), QRect(0, 0, 384, 256));
}

void KisGpuPaintDeviceTest::testExactBoundsReadsBorderTilesOnly()
{
    REQUIRE_GPU();
    // The bounds scan downloads the tiles it reads. It must find the edges
    // of content held by the GPU without reading the tiles inside it.
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    const QRect solid(0, 0, 10 * 64, 8 * 64);
    QVERIFY(gpuFill(device, solid, {0.25f, 0.5f, 0.75f, 1.0f}));

    quint64 before = m_backend->downloadedTileCount();
    QCOMPARE(device->calculateExactBounds(false), solid);
    // one tile per edge at most
    QVERIFY2(m_backend->downloadedTileCount() - before <= 4,
             qPrintable(QStringLiteral("%1 tiles downloaded").arg(m_backend->downloadedTileCount() - before)));

    // A separate block, so the extent holds tiles that do not exist: they
    // are searched, the two blocks' inner tiles are not.
    KisPaintDeviceSP sparse = new KisPaintDevice(rgbaFloat());
    const QRect first(0, 0, 6 * 64, 6 * 64);
    const QRect second(12 * 64, 9 * 64, 6 * 64, 6 * 64);
    QVERIFY(gpuFill(sparse, first, {1.0f, 0.0f, 0.0f, 1.0f}));
    QVERIFY(gpuFill(sparse, second, {0.0f, 0.0f, 1.0f, 1.0f}));

    before = m_backend->downloadedTileCount();
    QCOMPARE(sparse->calculateExactBounds(false), first.united(second));
    const quint64 downloaded = m_backend->downloadedTileCount() - before;
    QVERIFY2(downloaded <= 6, qPrintable(QStringLiteral("%1 of 72 tiles downloaded").arg(downloaded)));
}

void KisGpuPaintDeviceTest::testCompositeMatchesCpu()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 320, 192);
    KisPaintDeviceSP src = new KisPaintDevice(rgbaFloat());
    KisPaintDeviceSP dst = new KisPaintDevice(rgbaFloat());
    fillRandom(src, rect, 2);
    fillRandom(dst, rect, 3);

    KisPaintDeviceSP expected = new KisPaintDevice(*dst);
    {
        KisPainter painter(expected);
        painter.setCompositeOpId(COMPOSITE_OVER);
        painter.setOpacityF(0.7);
        painter.bitBlt(rect.topLeft(), src, rect);
    }

    QVERIFY(gpuCompositeOver(dst, src, rect, 0.7f));
    const float difference = maxDifference(readPixels(expected, rect), readPixels(dst, rect));
    QVERIFY2(difference <= 1e-5f, qPrintable(QString::number(difference)));
}

void KisGpuPaintDeviceTest::testUndoRedo()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 192, 128);
    KisPaintDeviceSP src = new KisPaintDevice(rgbaFloat());
    KisPaintDeviceSP dst = new KisPaintDevice(rgbaFloat());
    fillRandom(src, rect, 4);
    fillRandom(dst, rect, 5);
    const std::vector<float> before = readPixels(dst, rect);

    KisTransaction transaction(dst);
    QVERIFY(gpuCompositeOver(dst, src, rect, 1.0f));
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    command->redo(); // the undo stack's initial redo() is a no-op
    const std::vector<float> after = readPixels(dst, rect);
    QVERIFY(maxDifference(before, after) > 0.0f);

    command->undo();
    QCOMPARE(maxDifference(before, readPixels(dst, rect)), 0.0f);

    command->redo();
    QCOMPARE(maxDifference(after, readPixels(dst, rect)), 0.0f);

    // A second GPU write on top of the redone state, then undo it again.
    KisTransaction second(dst);
    QVERIFY(gpuFill(dst, rect, {0.0f, 1.0f, 0.0f, 1.0f}));
    QScopedPointer<KUndo2Command> secondCommand(second.endAndTake());
    secondCommand->redo();
    secondCommand->undo();
    QCOMPARE(maxDifference(after, readPixels(dst, rect)), 0.0f);
}

void KisGpuPaintDeviceTest::testPoolerKeepsGpuTilesResident()
{
    REQUIRE_GPU();
    auto *store = KisTileDataStore::instance();
    store->testingSuspendPooler();
    const auto resume = qScopeGuard([&]() {
        store->testingResumePooler();
    });
    struct TestPooler : KisTileDataPooler {
        using KisTileDataPooler::KisTileDataPooler;
        using KisTileDataPooler::processLists;
    } pooler(store, 65536);
    const QRect rect(0, 0, 512, 512);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuFill(device, rect, {0.25f, 0.5f, 0.75f, 1.0f}));
    KisPaintDeviceSP snapshot = new KisPaintDevice(*device);
    QList<KisTileData *> candidates, donors;
    QVector<KisTileSP> tiles;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            bool existing = false;
            auto tile = device->dataManager()->getReadOnlyTileLazy(x, y, existing);
            QVERIFY(existing);
            tiles << tile;
            candidates << tile->tileData();
        }
    auto &context = m_backend->context();
    const auto before = context.completedValue();
    qint32 occupied = 0;
    QElapsedTimer timer;
    timer.start();
    pooler.processLists(candidates, donors, occupied);
    qInfo() << "Speculative pooler, 64 shared GPU tiles: ms" << timer.nsecsElapsed() / 1e6 << "submissions"
            << context.completedValue() - before << "clone memory metric" << occupied;
    QCOMPARE(context.completedValue(), before);
    QCOMPARE(occupied, 0);
    for (const auto *td : candidates)
        QVERIFY(!td->gpuState()->cpuValid());
    // A real CPU consumer still obtains current pixels and correct COW.
    const auto oldPixels = readPixels(snapshot, rect);
    for (size_t i = 0; i < oldPixels.size(); i += 4) {
        QCOMPARE(oldPixels[i], 0.25f);
        QCOMPARE(oldPixels[i + 3], 1.0f);
    }
    // Once CPU pixels are current, the ordinary speculative clone path works.
    pooler.processLists(candidates, donors, occupied);
    QVERIFY(occupied > 0);
    const float red[] = {1, 0, 0, 1};
    device->fill(0, 0, 64, 64, reinterpret_cast<const quint8 *>(red));
    QCOMPARE(readPixels(snapshot, rect), oldPixels);
    QCOMPARE(readPixels(device, QRect(0, 0, 1, 1))[0], 1.0f);
}

void KisGpuPaintDeviceTest::testCopyOnWriteIsolation()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 128, 128);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    fillRandom(device, rect, 6);
    const std::vector<float> original = readPixels(device, rect);

    // CPU-valid shared tiles: copy-on-write clones on the CPU, then uploads.
    KisPaintDeviceSP cpuCopy = new KisPaintDevice(*device);
    QVERIFY(gpuFill(device, rect, {1.0f, 1.0f, 1.0f, 1.0f}));
    QCOMPARE(maxDifference(original, readPixels(cpuCopy, rect)), 0.0f);

    // GPU-only shared tiles: copy-on-write clones with a GPU slot copy.
    QVERIFY(gpuFill(device, rect, {0.5f, 0.5f, 0.5f, 1.0f})); // tiles are now GPU-valid, CPU-stale
    KisPaintDeviceSP shared = new KisPaintDevice(*device);
    KisPaintDeviceSP overlay = new KisPaintDevice(rgbaFloat());
    fillRandom(overlay, rect, 7);
    QVERIFY(gpuCompositeOver(device, overlay, rect, 1.0f));

    const std::vector<float> gray = [&]() {
        std::vector<float> pixels(size_t(rect.width()) * rect.height() * 4, 0.5f);
        for (size_t i = 3; i < pixels.size(); i += 4) {
            pixels[i] = 1.0f;
        }
        return pixels;
    }();
    QCOMPARE(maxDifference(gray, readPixels(shared, rect)), 0.0f);
    QVERIFY(maxDifference(gray, readPixels(device, rect)) > 0.0f);
}

void KisGpuPaintDeviceTest::testGpuCopyKeepsCpuSnapshot_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<bool>("readWrite");
    QTest::addColumn<bool>("failSubmit");
    for (bool f16 : {false, true})
        for (bool readWrite : {false, true})
            for (bool failSubmit : {false, true})
                QTest::newRow(qPrintable(QString("f16%1-readWrite%2-fail%3").arg(f16).arg(readWrite).arg(failSubmit)))
                    << f16 << readWrite << failSubmit;
}

void KisGpuPaintDeviceTest::testGpuCopyKeepsCpuSnapshot()
{
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(bool, readWrite);
    QFETCH(bool, failSubmit);
    const QRect rect(0, 0, 64, 64);
    const auto *cs = rgbaFloat(f16);
    auto bytes = [&](KisPaintDeviceSP device) {
        QByteArray result(rect.width() * rect.height() * cs->pixelSize(), 0);
        device->readBytes(reinterpret_cast<quint8 *>(result.data()), rect);
        return result;
    };
    KisPaintDeviceSP reference = new KisPaintDevice(cs);
    QVERIFY(gpuFill(reference, rect, {0, 0, 1, 1}));
    const QByteArray blue = bytes(reference);
    QVERIFY(gpuFill(reference, rect, {0, 1, 0, 1}));
    const QByteArray green = bytes(reference);

    KisPaintDeviceSP device = new KisPaintDevice(cs);
    device->fill(rect, KoColor(Qt::red, cs));
    const QByteArray snapshot = bytes(device);
    QVERIFY(gpuFill(device, rect, {0, 0, 1, 1}));
    KisPaintDeviceSP shared = new KisPaintDevice(*device);
    auto original = shared->dataManager()->getTile(0, 0, false);
    QVERIFY(!original->tileData()->gpuState()->cpuValid());

    KisTransaction transaction(device);
    KisGpuTileAccess writer(device, rect, readWrite ? KisGpuTileAccess::ReadWrite : KisGpuTileAccess::WriteOnly);
    m_commands->begin();
    const quint64 before = m_backend->context().completedValue();
    QVERIFY(writer.prepare(*m_commands));
    // Preparing COW must not download the authoritative blue pixels or zero
    // the retained red CPU snapshot used by content-loss recovery.
    QCOMPARE(m_backend->context().completedValue(), before);
    QVERIFY(!original->tileData()->gpuState()->cpuValid());
    auto detached = device->dataManager()->getTile(0, 0, false);
    QVERIFY(detached->tileData() != original->tileData());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(detached->tileData()->data()), snapshot.size()), snapshot);
    const float color[] = {0, 1, 0, 1};
    QVERIFY((f16 ? m_fill16 : m_fill32)->record(*m_commands, writer.addresses(), color));
    if (failSubmit)
        m_backend->context().injectSubmitFailuresForTesting(1);
    const quint64 value = KisGpuTileAccess::submitAndFinish(*m_commands, {&writer});
    QCOMPARE(value == 0, failSubmit);
    QVERIFY(m_commands->wait());
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    command->redo(); // the undo stack normally consumes the initial redo
    const QByteArray expected = failSubmit ? blue : green;
    QCOMPARE(bytes(device), expected);
    QCOMPARE(bytes(shared), blue);
    command->undo();
    QCOMPARE(bytes(device), blue);
    command->redo();
    QCOMPARE(bytes(device), expected);
    QVERIFY(!m_backend->hasFailed());
}

void KisGpuPaintDeviceTest::testWholeTileCopyStaysOnGpu_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<bool>("oldData");
    QTest::addColumn<bool>("rough");
    for (bool f16 : {false, true})
        for (bool oldData : {false, true})
            for (bool rough : {false, true})
                QTest::newRow(qPrintable(QString("f16%1-old%2-rough%3").arg(f16).arg(oldData).arg(rough)))
                    << f16 << oldData << rough;
}

void KisGpuPaintDeviceTest::testWholeTileCopyStaysOnGpu()
{
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(bool, oldData);
    QFETCH(bool, rough);
    const QRect rect(0, 0, 128, 128);
    const auto *cs = rgbaFloat(f16);
    auto bytes = [&](KisPaintDeviceSP device) {
        QByteArray result(rect.width() * rect.height() * cs->pixelSize(), 0);
        device->readBytes(reinterpret_cast<quint8 *>(result.data()), rect);
        return result;
    };
    KisPaintDeviceSP reference = new KisPaintDevice(cs);
    QVERIFY(gpuFill(reference, rect, oldData ? QVector<float>{0, 0, 1, 1} : QVector<float>{0, 1, 0, 1}));
    const QByteArray expected = bytes(reference);
    KisPaintDeviceSP source = new KisPaintDevice(cs);
    QVERIFY(gpuFill(source, rect, {0, 0, 1, 1}));
    KisTransaction sourceTransaction(source);
    QVERIFY(gpuFill(source, rect, {0, 1, 0, 1}));
    auto current = source->dataManager()->getTile(0, 0, false);
    bool exists = false;
    auto old = source->dataManager()->getOldTile(0, 0, exists);
    QVERIFY(exists);
    QVERIFY(current->tileData() != old->tileData());

    KisPaintDeviceSP target = new KisPaintDevice(cs);
    target->fill(rect, KoColor(Qt::red, cs));
    const QByteArray before = bytes(target);
    KisTransaction transaction(target);
    const quint64 submissions = m_backend->context().completedValue();
    auto *dst = target->dataManager().data();
    auto *src = source->dataManager().data();
    const QRect region = rough ? QRect(7, 9, 75, 77) : rect;
    if (rough) {
        if (oldData)
            dst->bitBltRoughOldData(src, region);
        else
            dst->bitBltRough(src, region);
    } else {
        if (oldData)
            dst->bitBltOldData(src, region);
        else
            dst->bitBlt(src, region);
    }
    QCOMPARE(m_backend->context().completedValue(), submissions);
    QVERIFY(!current->tileData()->gpuState()->cpuValid());
    QVERIFY(!old->tileData()->gpuState()->cpuValid());
    QCOMPARE(target->dataManager()->getTile(0, 0, false)->tileData(), (oldData ? old : current)->tileData());
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    command->redo();
    QCOMPARE(bytes(target), expected);
    command->undo();
    QCOMPARE(bytes(target), before);
    command->redo();
    QCOMPARE(bytes(target), expected);
    QVERIFY(gpuFill(source, rect, {1, 0, 1, 1}));
    QCOMPARE(bytes(target), expected); // subsequent GPU COW cannot alter the copy
    sourceTransaction.end();

    // Partial tile copies still need the current pixels, preserving everything
    // outside the requested rectangle. Compare with a CPU-current source.
    QVERIFY(gpuFill(reference, rect, {1, 0, 1, 1}));
    bytes(reference);
    KisPaintDeviceSP partialReference = new KisPaintDevice(*target);
    const QRect partial(7, 9, 75, 77);
    partialReference->dataManager()->bitBlt(reference->dataManager().data(), partial);
    target->dataManager()->bitBlt(source->dataManager().data(), partial);
    QCOMPARE(bytes(target), bytes(partialReference));
}

void KisGpuPaintDeviceTest::testPartialClearBatchesReadback_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<QRect>("region");
    QTest::addColumn<bool>("failSubmit");
    QTest::addColumn<quint64>("submissions");
    for (bool f16 : {false, true}) {
        QTest::newRow(qPrintable(QString("aligned-f16%1").arg(f16)))
            << f16 << QRect(0, 0, 512, 512) << false << quint64(0);
        QTest::newRow(qPrintable(QString("edges-f16%1").arg(f16)))
            << f16 << QRect(-63, -61, 638, 636) << false << quint64(4);
        QTest::newRow(qPrintable(QString("row-f16%1").arg(f16)))
            << f16 << QRect(-63, -63, 638, 1) << false << quint64(1);
        QTest::newRow(qPrintable(QString("retry-f16%1").arg(f16)))
            << f16 << QRect(-63, -61, 638, 636) << true << quint64(13);
    }
}

void KisGpuPaintDeviceTest::testPartialClearBatchesReadback()
{
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(QRect, region);
    QFETCH(bool, failSubmit);
    QFETCH(quint64, submissions);
    const QRect bounds(-64, -64, 640, 640);
    const auto *cs = rgbaFloat(f16);
    auto bytes = [&](KisPaintDeviceSP device) {
        QByteArray result(bounds.width() * bounds.height() * cs->pixelSize(), 0);
        device->readBytes(reinterpret_cast<quint8 *>(result.data()), bounds);
        return result;
    };
    KisPaintDeviceSP reference = new KisPaintDevice(cs);
    QVERIFY(gpuFill(reference, bounds, {0, 0, 1, 1}));
    const QByteArray original = bytes(reference);
    reference->clear(region);
    const QByteArray expected = bytes(reference);
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    QVERIFY(gpuFill(device, bounds, {0, 0, 1, 1}));
    KisPaintDeviceSP shared = new KisPaintDevice(*device);
    KisTransaction transaction(device);
    const quint64 before = m_backend->context().completedValue();
    if (failSubmit)
        m_backend->context().injectSubmitFailuresForTesting(1);
    device->clear(region);
    m_backend->context().waitIdle();
    QCOMPARE(m_backend->context().completedValue() - before, submissions);
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    command->redo();
    QCOMPARE(bytes(device), expected);
    QCOMPARE(bytes(shared), original);
    command->undo();
    QCOMPARE(bytes(device), original);
    command->redo();
    QCOMPARE(bytes(device), expected);
    QVERIFY(!m_backend->hasFailed());
}

void KisGpuPaintDeviceTest::testCpuWriteAfterGpuWrite()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 128, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuFill(device, rect, {0.0f, 0.0f, 1.0f, 1.0f}));

    // CPU write: downloads the tile first, then makes the GPU copy stale.
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    device->setPixel(10, 10, KoColor(reinterpret_cast<const quint8 *>(red), device->colorSpace()));

    // A GPU read must see the CPU edit (re-upload), and the rest of the GPU fill.
    KisPaintDeviceSP copy = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuCompositeOver(copy, device, rect, 1.0f));
    const std::vector<float> pixels = readPixels(copy, rect);
    const size_t edited = (size_t(10) * rect.width() + 10) * 4;
    QCOMPARE(pixels[edited + 0], 1.0f);
    QCOMPARE(pixels[edited + 2], 0.0f);
    QCOMPARE(pixels[0 + 2], 1.0f);
    QCOMPARE(pixels[(size_t(5) * rect.width() + 100) * 4 + 2], 1.0f);
}

void KisGpuPaintDeviceTest::testSaveLoadRoundTrip()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 256, 192);
    KisPaintDeviceSP src = new KisPaintDevice(rgbaFloat());
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    fillRandom(src, rect, 8);
    fillRandom(device, rect, 9);
    QVERIFY(gpuCompositeOver(device, src, rect, 0.5f));

    KisGpuTileAccess::syncToCpu(device, rect);

    QBuffer buffer;
    buffer.open(QIODevice::ReadWrite);
    BufferWriter writer(&buffer);
    QVERIFY(device->write(writer));

    buffer.seek(0);
    KisPaintDeviceSP loaded = new KisPaintDevice(rgbaFloat());
    QVERIFY(loaded->read(&buffer));
    QCOMPARE(maxDifference(readPixels(device, rect), readPixels(loaded, rect)), 0.0f);
}

void KisGpuPaintDeviceTest::testF16Device()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 128, 128);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat(true));
    QVERIFY(gpuFill(device, rect, {0.25f, 0.5f, 0.75f, 1.0f}));

    KoColor color;
    device->pixel(70, 70, &color);
    QVector<float> channels(4);
    color.colorSpace()->normalisedChannelsValue(color.data(), channels);
    QCOMPARE(channels[0], 0.25f);
    QCOMPARE(channels[1], 0.5f);
    QCOMPARE(channels[2], 0.75f);
    QCOMPARE(channels[3], 1.0f);
}

void KisGpuPaintDeviceTest::testSlotsReleased()
{
    REQUIRE_GPU();
    m_backend->flush();
    const quint32 baseline = m_backend->allocatedSlots(16);

    {
        const QRect rect(0, 0, 512, 512);
        KisPaintDeviceSP src = new KisPaintDevice(rgbaFloat());
        KisPaintDeviceSP dst = new KisPaintDevice(rgbaFloat());
        fillRandom(src, rect, 10);
        fillRandom(dst, rect, 11);

        KisTransaction transaction(dst);
        QVERIFY(gpuCompositeOver(dst, src, rect, 1.0f));
        QScopedPointer<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        QVERIFY(m_backend->allocatedSlots(16) > baseline);
        command->undo();
        command->redo();
    }

    m_backend->flush();
    QCOMPARE(m_backend->allocatedSlots(16), baseline);
}

void KisGpuPaintDeviceTest::testSaveWithoutSync()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 192, 128);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    fillRandom(device, rect, 20);
    QVERIFY(gpuFill(device, QRect(64, 0, 64, 64), {0.9f, 0.1f, 0.2f, 0.7f}));

    // The tiles written by the GPU are stale on the CPU: writing the device
    // (the .kra tile compressor) must download them through the tile locks.
    QBuffer buffer;
    buffer.open(QIODevice::ReadWrite);
    BufferWriter writer(&buffer);
    QVERIFY(device->write(writer));

    buffer.seek(0);
    KisPaintDeviceSP loaded = new KisPaintDevice(rgbaFloat());
    QVERIFY(loaded->read(&buffer));
    const std::vector<float> pixels = readPixels(loaded, QRect(64, 0, 64, 64));
    QCOMPARE(pixels[0], 0.9f);
    QCOMPARE(pixels[3], 0.7f);
    QCOMPARE(maxDifference(readPixels(device, rect), readPixels(loaded, rect)), 0.0f);
}

void KisGpuPaintDeviceTest::testLockedTileRejectsGpuWrite()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 64, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    fillRandom(device, rect, 21);
    const std::vector<float> before = readPixels(device, rect);

    // Exclusive (not shared) tile data, held by a CPU reader.
    KisTileSP tile = device->dataManager()->getTile(0, 0, true);
    QVERIFY(tile->tileData()->numUsers() == 1);
    tile->lockForRead();

    KisGpuTileAccess access(device, rect, KisGpuTileAccess::WriteOnly);
    m_commands->begin();
    QString error;
    QVERIFY(!access.prepare(*m_commands, &error));
    QVERIFY(error.contains(QStringLiteral("locked")));
    KisGpuTileAccess::submitAndFinish(*m_commands, {&access});
    m_commands->wait();
    tile->unlockForRead();

    QCOMPARE(maxDifference(before, readPixels(device, rect)), 0.0f);
}

void KisGpuPaintDeviceTest::testDownloadFailureIsRetried()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 128, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuFill(device, rect, {0.25f, 0.5f, 0.75f, 1.0f}));

    const quint64 lostBefore = m_backend->contentLossCount();
    m_backend->injectDownloadFailuresForTesting(1);
    const std::vector<float> pixels = readPixels(device, rect);
    m_backend->injectDownloadFailuresForTesting(0);

    QCOMPARE(pixels[0], 0.25f);
    QCOMPARE(pixels[pixels.size() - 2], 0.75f);
    QCOMPARE(m_backend->contentLossCount(), lostBefore);
    QVERIFY(!m_backend->hasFailed());
}

void KisGpuPaintDeviceTest::testPersistentDownloadFailureIsReported()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 128, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    fillRandom(device, rect, 22);
    const std::vector<float> beforeGpuWork = readPixels(device, rect);

    // Tile (0,0): written in place. Tile (1,0): written through a GPU
    // copy-on-write clone (its source is GPU-only and shared with a copy).
    QVERIFY(gpuFill(device, QRect(64, 0, 64, 64), {0.0f, 0.0f, 1.0f, 1.0f}));
    KisPaintDeviceSP sharedCopy = new KisPaintDevice(*device);
    QVERIFY(gpuFill(device, rect, {1.0f, 0.0f, 0.0f, 1.0f}));
    // The CPU buffers hold what the CPU last had: for tile (1,0) that is
    // older than the first (blue) GPU write too, because that content only
    // ever existed on the GPU.
    const std::vector<float> &lastCpuContent = beforeGpuWork;
    Q_UNUSED(sharedCopy);
    // Another device with GPU-only content, read after the engine failed.
    KisPaintDeviceSP second = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuFill(second, QRect(0, 0, 64, 64), {0.0f, 1.0f, 0.0f, 1.0f}));

    // Every read-back attempt fails: the latest content is lost.
    const quint64 lostBefore = m_backend->contentLossCount();
    std::atomic<int> listenerCalls{0};
    KisGpuTileBackend::setFailureListener([&listenerCalls]() {
        listenerCalls++;
    });
    m_backend->injectDownloadFailuresForTesting(1000);
    const std::vector<float> pixels = readPixels(device, rect);
    readPixels(second, QRect(0, 0, 64, 64)); // a second loss, already failed
    m_backend->injectDownloadFailuresForTesting(0);
    KisGpuTileBackend::setFailureListener({});

    // The loss is reported (to the UI once, KisGpuEngineUi) and the engine
    // stops taking GPU work...
    QCOMPARE(listenerCalls.load(), 1);
    QCOMPARE(m_backend->contentLossCount(), lostBefore + 3);
    QVERIFY(m_backend->hasFailed());
    QVERIFY(!KisGpuTileAccess::isSupported(device));

    // ...and the tiles show the content the CPU last had: never zeros or
    // other meaningless data (also for the GPU copy-on-write clone).
    QCOMPARE(maxDifference(lastCpuContent, pixels), 0.0f);
    for (int col = 0; col < 2; col++) {
        KisTileGpuState *state = device->dataManager()->getTile(col, 0, false)->tileData()->gpuState();
        QVERIFY(state && state->cpuValid() && !state->gpuValid() && state->contentLost());
    }
}

void KisGpuPaintDeviceTest::testNoSubmissionAfterFailure()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 128, 64);

    // A GPU write prepared while the engine still works...
    KisPaintDeviceSP target = new KisPaintDevice(rgbaFloat());
    fillRandom(target, rect, 25);
    const std::vector<float> targetBefore = readPixels(target, rect);
    KisPaintDeviceSP shared = new KisPaintDevice(*target); // forces copy-on-write
    KisTransaction transaction(target);
    KisGpuTileAccess writer(target, rect, KisGpuTileAccess::WriteOnly);
    m_commands->begin();
    QVERIFY(writer.prepare(*m_commands));
    const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    QVERIFY(m_fill32->record(*m_commands, writer.addresses(), blue));

    // ...then another thread loses GPU content and the engine stops.
    {
        KisGpuCommandList commands(m_backend->context());
        KisPaintDeviceSP other = new KisPaintDevice(rgbaFloat());
        KisGpuTileAccess access(other, QRect(0, 0, 64, 64), KisGpuTileAccess::WriteOnly);
        commands.begin();
        QVERIFY(access.prepare(commands));
        const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        QVERIFY(m_fill32->record(commands, access.addresses(), red));
        QVERIFY(KisGpuTileAccess::submitAndFinish(commands, {&access}));
        commands.wait();
        m_backend->injectDownloadFailuresForTesting(1000);
        readPixels(other, QRect(0, 0, 64, 64));
        m_backend->injectDownloadFailuresForTesting(0);
    }
    QVERIFY(m_backend->hasFailed());

    // The prepared write is not submitted and leaves the device unchanged.
    QCOMPARE(KisGpuTileAccess::submitAndFinish(*m_commands, {&writer}), quint64(0));
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    QCOMPARE(maxDifference(targetBefore, readPixels(target, rect)), 0.0f);
    QCOMPARE(maxDifference(targetBefore, readPixels(shared, rect)), 0.0f);

    // New GPU work is refused at prepare() already.
    KisGpuTileAccess late(target, rect, KisGpuTileAccess::WriteOnly);
    m_commands->begin();
    QString error;
    QVERIFY(!late.prepare(*m_commands, &error));
    QVERIFY(error.contains(QStringLiteral("stopped")));
    QCOMPARE(KisGpuTileAccess::submitAndFinish(*m_commands, {&late}), quint64(0));
    QCOMPARE(maxDifference(targetBefore, readPixels(target, rect)), 0.0f);
}

void KisGpuPaintDeviceTest::testFailedSubmissionKeepsContent()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 128, 64);
    KisPaintDeviceSP target = new KisPaintDevice(rgbaFloat());
    fillRandom(target, rect, 26);
    const std::vector<float> targetBefore = readPixels(target, rect);
    KisPaintDeviceSP shared = new KisPaintDevice(*target); // forces copy-on-write

    KisTransaction transaction(target);
    KisGpuTileAccess writer(target, rect, KisGpuTileAccess::WriteOnly);
    m_commands->begin();
    QVERIFY(writer.prepare(*m_commands));
    const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    QVERIFY(m_fill32->record(*m_commands, writer.addresses(), blue));

    // submit() is attempted and fails: the recording was already ended by
    // submit(), and must not be ended a second time (validation would
    // report it in cleanupTestCase()).
    m_backend->context().injectSubmitFailuresForTesting(1);
    QCOMPARE(KisGpuTileAccess::submitAndFinish(*m_commands, {&writer}), quint64(0));
    QVERIFY(!m_commands->isRecording());
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());

    QCOMPARE(maxDifference(targetBefore, readPixels(target, rect)), 0.0f);
    QCOMPARE(maxDifference(targetBefore, readPixels(shared, rect)), 0.0f);
    QVERIFY(!m_backend->hasFailed());

    // The command list is usable again.
    QVERIFY(gpuFill(target, rect, {0.5f, 0.5f, 0.5f, 1.0f}));
    QCOMPARE(readPixels(target, rect)[0], 0.5f);
}

void KisGpuPaintDeviceTest::testRetryAllocatesOnlyWhatItNeeds()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 256, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuFill(device, rect, {0.1f, 0.2f, 0.3f, 1.0f}));

    // A batch buffer for the four tiles cannot be allocated, a one-tile
    // buffer can: the tile-by-tile retry must succeed.
    const quint64 lostBefore = m_backend->contentLossCount();
    m_backend->injectReadbackLimitForTesting(64 * 64 * 16);
    KisGpuTileAccess::syncToCpu(device, rect);
    m_backend->injectReadbackLimitForTesting(~quint64(0));

    QCOMPARE(m_backend->contentLossCount(), lostBefore);
    QVERIFY(!m_backend->hasFailed());
    const std::vector<float> pixels = readPixels(device, rect);
    QCOMPARE(pixels[0], 0.1f);
    QCOMPARE(pixels[pixels.size() - 2], 0.3f);
}

void KisGpuPaintDeviceTest::testGpuValidCannotBePublishedAfterCpuWrite()
{
    // Sequential: the interleaving that was possible before (check the
    // generation, CPU write, then publish) now fails the publication.
    {
        KisTileGpuState state;
        state.setValid(KisTileGpuState::CpuValid);
        const int generation = state.generation();
        state.notifyCpuWrite();
        QVERIFY(!state.markGpuValidIfGeneration(generation));
        QVERIFY(!state.gpuValid());
        QVERIFY(state.cpuValid());
        QVERIFY(state.markGpuValidIfGeneration(state.generation()));
        QVERIFY(state.gpuValid());
        state.notifyCpuWrite();
        QVERIFY(!state.gpuValid());
    }

    // Flags and generation share one atomic word and are updated by
    // compare-and-swap, so the check and the publication cannot be split by
    // a concurrent CPU write (KisTileGpuState).
}

namespace
{
std::vector<float> solidPixels(const QRect &rect, const float color[4])
{
    std::vector<float> pixels(size_t(rect.width()) * rect.height() * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        std::copy(color, color + 4, pixels.begin() + i);
    }
    return pixels;
}
} // namespace

void KisGpuPaintDeviceTest::testReadOnlyAccessKeepsTileDataAlive()
{
    // A read-only access looks a tile's data up without locking the tile.
    // A CPU writer in another thread may replace that data (copy-on-write)
    // and the last other owner may drop it: the access has to hold a
    // reference of its own (KisTile::referencedTileData()). Two merge jobs
    // share tiles of a layer that is off the image's tile grid, and one
    // applying a mask on the CPU did exactly this to the tile data the other
    // was uploading: a crash in KisTileData::blockSwapping() (2026-11-12).
    REQUIRE_GPU();
    const QRect rect(0, 0, 64, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    fillRandom(device, rect, 31);
    m_backend->flush();

    KisGpuCommandList commands(m_backend->context());
    KisGpuTileAccess access(device, rect, KisGpuTileAccess::ReadOnly);
    commands.begin();
    QVERIFY(access.prepare(commands));
    const qint32 tilesBefore = KisTileDataStore::instance()->numTiles();

    // What KisFilter::process() does in the other job: a temporary device
    // shares the tile's data...
    KisPaintDeviceSP temporary = new KisPaintDevice(rgbaFloat());
    temporary->makeCloneFromRough(device, rect);
    // ...the device's tile is written (copy-on-write: it gets new data, and
    // the data the access reads belongs to the temporary device alone)...
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const std::vector<float> redPixel = solidPixels(QRect(0, 0, 1, 1), red);
    device->writeBytes(reinterpret_cast<const quint8 *>(redPixel.data()), QRect(0, 0, 1, 1));
    // ...and the temporary device goes away.
    temporary = nullptr;
    // One tile data more than before: the tile's new one. The one the
    // access still reads must not have been freed.
    QCOMPARE(KisTileDataStore::instance()->numTiles(), tilesBefore + 1);

    QVERIFY(KisGpuTileAccess::submitAndFinish(commands, {&access}));
    commands.wait();
    // Finished: the access dropped its reference, and the old data is freed.
    QCOMPARE(KisTileDataStore::instance()->numTiles(), tilesBefore);

    // The same for the batched download of stale tiles (syncToCpu()): it
    // must leave no reference behind.
    QVERIFY(gpuFill(device, rect, {0.25f, 0.5f, 0.75f, 1.0f}));
    const qint32 tilesResident = KisTileDataStore::instance()->numTiles();
    KisGpuTileAccess::syncToCpu(device, rect);
    QCOMPARE(KisTileDataStore::instance()->numTiles(), tilesResident);
    device->clear();
    QCOMPARE(KisTileDataStore::instance()->numTiles(), tilesResident - 1);
}

void KisGpuPaintDeviceTest::testOlderUploadDoesNotOverwriteNewer()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 64, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    fillRandom(device, rect, 23);
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};

    // Access A snapshots the current CPU content for an upload...
    KisGpuCommandList commandsA(m_backend->context());
    KisGpuTileAccess accessA(device, rect, KisGpuTileAccess::ReadOnly);
    commandsA.begin();
    QVERIFY(accessA.prepare(commandsA));

    // ...the CPU changes the tile in place, and access B uploads and
    // publishes the newer content.
    const std::vector<float> redPixels = solidPixels(rect, red);
    device->writeBytes(reinterpret_cast<const quint8 *>(redPixels.data()), rect);
    KisPaintDeviceSP copy = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuCompositeOver(copy, device, rect, 1.0f));

    // A still runs, but must not upload its older snapshot over B's content.
    QVERIFY(KisGpuTileAccess::submitAndFinish(commandsA, {&accessA}));
    commandsA.wait();

    KisTileGpuState *state = device->dataManager()->getTile(0, 0, false)->tileData()->gpuState();
    QVERIFY(state->gpuValid());
    KisPaintDeviceSP check = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuCompositeOver(check, device, rect, 1.0f));
    QCOMPARE(maxDifference(redPixels, readPixels(check, rect)), 0.0f);
}

void KisGpuPaintDeviceTest::testSnapshotDuringCpuWriteIsNotPublished()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 64, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    fillRandom(device, rect, 27);
    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    const std::vector<float> greenPixels = solidPixels(rect, green);

    // A CPU writer holds the tile; half of its write is done when a GPU
    // reader takes its snapshot, the other half afterwards.
    KisTileSP tile = device->dataManager()->getTile(0, 0, true);
    tile->lockForWrite();
    const size_t half = size_t(64 * 32 * 16);
    std::memcpy(tile->data(), greenPixels.data(), half);

    KisGpuCommandList commands(m_backend->context());
    KisGpuTileAccess reader(device, rect, KisGpuTileAccess::ReadOnly);
    commands.begin();
    QVERIFY(reader.prepare(commands));

    std::memcpy(tile->data() + half, reinterpret_cast<const quint8 *>(greenPixels.data()) + half, half);
    tile->unlockForWrite();

    // The torn snapshot is used for this submission but never published as
    // the tile's GPU copy...
    QVERIFY(KisGpuTileAccess::submitAndFinish(commands, {&reader}));
    commands.wait();
    QVERIFY(!tile->tileData()->gpuState()->gpuValid());

    // ...so the next GPU reader uploads the complete content.
    KisPaintDeviceSP check = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuCompositeOver(check, device, rect, 1.0f));
    QCOMPARE(maxDifference(greenPixels, readPixels(check, rect)), 0.0f);
}

void KisGpuPaintDeviceTest::testDeferredSlotRelease()
{
    REQUIRE_GPU();
    m_backend->flush();
    const quint32 baseline = m_backend->allocatedSlots(16);
    quint64 submitted = 0;

    {
        const QRect rect(0, 0, 512, 512);
        KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
        KisGpuTileAccess access(device, rect, KisGpuTileAccess::WriteOnly);
        m_commands->begin();
        QVERIFY(access.prepare(*m_commands));
        const float color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        // Several fills keep the GPU busy after the device is gone.
        for (int i = 0; i < 50; i++) {
            m_fill32->record(*m_commands, access.addresses(), color);
            m_commands->computeBarrier();
        }
        submitted = KisGpuTileAccess::submitAndFinish(*m_commands, {&access});
        QVERIFY(submitted);
        // The device and its tiles die here, possibly before the GPU is done.
    }

    // Slots of in-flight work are not reused until the work has completed.
    m_backend->collectGarbage();
    const bool stillRunning = m_backend->context().completedValue() < submitted;
    const quint32 whileRunning = m_backend->allocatedSlots(16);
    if (stillRunning) {
        QVERIFY(whileRunning > baseline);
    } else {
        qInfo() << "GPU finished before the check; deferred release not observed this run";
    }
    m_commands->wait();
    m_backend->collectGarbage();
    QCOMPARE(m_backend->allocatedSlots(16), baseline);
}

void KisGpuPaintDeviceTest::testMemoryBudgetEviction()
{
    REQUIRE_GPU();
    m_backend->flush();
    m_backend->evictTiles(~quint64(0));
    const quint64 originalBudget = m_backend->memoryBudget();
    auto restore = qScopeGuard([&]() {
        m_backend->setMemoryBudgetForTesting(originalBudget);
    });
    const quint64 budget = 2 * m_backend->pool(16)->tileBytes();
    m_backend->setMemoryBudgetForTesting(budget);
    const QRect rect(0, 0, 64, 64);
    QVector<KisPaintDeviceSP> devices;
    for (int i = 0; i < 8; ++i) {
        KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
        devices << device;
        QVERIFY(gpuFill(device, rect, {float(i) / 8, 0.5f, 0.25f, 1}));
        QVERIFY(m_backend->reservedTileBytes() <= budget);
    }
    // Earlier GPU-only contents survived eviction; re-upload is also possible.
    for (int i = 0; i < devices.size(); ++i) {
        const auto pixels = readPixels(devices[i], rect);
        for (size_t p = 0; p < pixels.size(); p += 4) {
            QCOMPARE(pixels[p], float(i) / 8);
            QCOMPARE(pixels[p + 1], 0.5f);
            QCOMPARE(pixels[p + 3], 1.0f);
        }
    }
    QVERIFY(!m_backend->hasFailed());
}

void KisGpuPaintDeviceTest::testBatchedEviction_data()
{
    QTest::addColumn<bool>("f16");
    QTest::addColumn<int>("scenario");
    for (bool f16 : {false, true})
        for (int scenario = 0; scenario < 4; ++scenario)
            QTest::newRow(qPrintable(QString("f16%1-case%2").arg(f16).arg(scenario))) << f16 << scenario;
}

void KisGpuPaintDeviceTest::testBatchedEviction()
{
    REQUIRE_GPU();
    QFETCH(bool, f16);
    QFETCH(int, scenario);
    m_backend->flush();
    m_backend->evictTiles(~quint64(0));
    const QRect bounds(0, 0, 17 * 64, 17 * 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat(f16));
    QVERIFY(gpuFill(device, bounds, {0.25f, 0.5f, 0.75f, 1}));
    QVector<KisTileSP> tiles;
    for (int row = 0; row < 17; ++row)
        for (int col = 0; col < 17; ++col)
            tiles << device->dataManager()->getTile(col, row, false);
    bool locked = scenario == 2;
    if (locked)
        tiles[0]->tileData()->blockSwappingForReadback();
    auto unlock = qScopeGuard([&] {
        if (locked)
            tiles[0]->tileData()->unblockSwapping();
    });
    if (scenario == 1)
        m_backend->injectDownloadFailuresForTesting(1);
    if (scenario == 3)
        m_backend->injectReadbackLimitForTesting(0);
    const quint64 before = m_backend->context().completedValue();
    const quint64 evicted = m_backend->evictTiles(~quint64(0));
    const quint64 submissions = m_backend->context().completedValue() - before;
    int retained = 0;
    for (const auto &tile : tiles) {
        const auto *state = tile->tileData()->gpuState();
        if (state->slot != KisTileGpuState::InvalidSlot) {
            ++retained;
            QVERIFY(state->gpuValid());
        } else {
            QVERIFY(state->cpuValid());
        }
    }
    if (scenario == 0 || scenario == 2) {
        QCOMPARE(submissions, quint64(2)); // 289 (or 288 unlocked) tiles, not one submission per tile
        QCOMPARE(retained, scenario == 2 ? 1 : 0);
        QVERIFY(evicted >= quint64(289 - retained) * device->pixelSize() * 64 * 64);
    } else if (scenario == 1) {
        QCOMPARE(submissions, quint64(1));
        QVERIFY(retained > 0 && retained <= 256);
    } else {
        QCOMPARE(submissions, quint64(0));
        QCOMPARE(retained, 289);
    }
    QVERIFY(!m_backend->hasFailed());
    if (locked) {
        tiles[0]->tileData()->unblockSwapping();
        locked = false;
    }
    m_backend->injectDownloadFailuresForTesting(0);
    m_backend->injectReadbackLimitForTesting(~quint64(0));
    m_backend->evictTiles(~quint64(0));
    for (const auto &tile : tiles)
        QCOMPARE(tile->tileData()->gpuState()->slot.load(), KisTileGpuState::InvalidSlot);
    KoColor color(device->colorSpace());
    device->colorSpace()->fromNormalisedChannelsValue(color.data(), {0.25f, 0.5f, 0.75f, 1});
    QByteArray pixels(bounds.width() * bounds.height() * device->pixelSize(), Qt::Uninitialized);
    device->readBytes(reinterpret_cast<quint8 *>(pixels.data()), bounds);
    for (int offset = 0; offset < pixels.size(); offset += device->pixelSize())
        QCOMPARE(std::memcmp(pixels.constData() + offset, color.data(), device->pixelSize()), 0);
}

void KisGpuPaintDeviceTest::testMixedEvictionBatch()
{
    REQUIRE_GPU();
    KisPaintDeviceSP f32 = new KisPaintDevice(rgbaFloat()), f16 = new KisPaintDevice(rgbaFloat(true));
    QVERIFY(gpuFill(f32, QRect(0, 0, 128, 64), {0.25f, 0.5f, 0.75f, 1}));
    QVERIFY(gpuFill(f16, QRect(0, 0, 64, 64), {0.75f, 0.5f, 0.25f, 1}));
    const auto a = f32->dataManager()->getTile(0, 0, false);
    const auto b = f32->dataManager()->getTile(1, 0, false);
    const auto c = f16->dataManager()->getTile(0, 0, false);
    a->tileData()->blockSwapping(); // A CPU-current neighbor must not need a download.
    a->tileData()->unblockSwapping();
    auto *store = KisTileDataStore::instance();
    auto *iteration = store->beginIteration();
    // Pixel sizes are downloaded separately. Fail F16, but reclaim F32; the
    // duplicate must not double-count or recursively lock the same tile.
    m_backend->injectDownloadFailuresForTesting(1);
    const quint64 before = m_backend->context().completedValue();
    const quint64 released =
        store->tryEvictGpuTileDataBatch({a->tileData(), b->tileData(), c->tileData(), a->tileData()});
    store->endIteration(iteration);
    QCOMPARE(released, quint64(2 * 64 * 64 * 16));
    QCOMPARE(m_backend->context().completedValue() - before, quint64(1));
    QCOMPARE(a->tileData()->gpuState()->slot.load(), KisTileGpuState::InvalidSlot);
    QCOMPARE(b->tileData()->gpuState()->slot.load(), KisTileGpuState::InvalidSlot);
    QVERIFY(c->tileData()->gpuState()->slot != KisTileGpuState::InvalidSlot);
    QVERIFY(c->tileData()->gpuState()->gpuValid());
    QVERIFY(!m_backend->hasFailed());
    iteration = store->beginIteration();
    const quint64 remaining = store->tryEvictGpuTileDataBatch({a->tileData(), b->tileData(), c->tileData()});
    const quint64 alreadyEvicted = store->tryEvictGpuTileDataBatch({a->tileData(), b->tileData(), c->tileData()});
    store->endIteration(iteration);
    QCOMPARE(remaining, quint64(64 * 64 * 8));
    QCOMPARE(alreadyEvicted, quint64(0));
    QCOMPARE(readPixels(f32, QRect(0, 0, 128, 64))[0], 0.25f);
    KoColor color;
    f16->pixel(0, 0, &color);
    QVector<float> channels(4);
    color.colorSpace()->normalisedChannelsValue(color.data(), channels);
    QCOMPARE(channels[0], 0.75f);
}

void KisGpuPaintDeviceTest::testPreparedAccessPreventsEviction()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 64, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuFill(device, rect, {0.5f, 0.25f, 0.75f, 1}));
    KisGpuTileAccess access(device, rect, KisGpuTileAccess::ReadOnly);
    m_commands->begin();
    QVERIFY(access.prepare(*m_commands));
    auto tile = device->dataManager()->getTile(0, 0, false);
    KisTileGpuState *state = tile->tileData()->gpuState();
    const quint32 slot = state->slot;
    m_backend->evictTiles(~quint64(0));
    QCOMPARE(state->slot.load(), slot);
    KisGpuTileAccess::finishUnsubmitted(*m_commands, {&access});
    m_backend->evictTiles(~quint64(0));
    QCOMPARE(state->slot.load(), KisTileGpuState::InvalidSlot);
    QCOMPARE(readPixels(device, rect)[0], 0.5f);
}

void KisGpuPaintDeviceTest::testEvictionFailureKeepsContent()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 64, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuFill(device, rect, {0.75f, 0.5f, 0.25f, 1}));
    auto tile = device->dataManager()->getTile(0, 0, false);
    auto *state = tile->tileData()->gpuState();
    const quint32 slot = state->slot;
    m_backend->injectDownloadFailuresForTesting(1);
    auto *store = KisTileDataStore::instance();
    auto *iteration = store->beginIteration();
    const bool evicted = store->tryEvictGpuTileData(tile->tileData());
    store->endIteration(iteration);
    QVERIFY(!evicted);
    QCOMPARE(state->slot.load(), slot);
    QVERIFY(state->gpuValid());
    QVERIFY(!m_backend->hasFailed());
    QCOMPARE(readPixels(device, rect)[0], 0.75f);
}

void KisGpuPaintDeviceTest::testGpuTileDiskSwapRoundTrip()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 64, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuFill(device, rect, {0.25f, 0.5f, 0.75f, 1}));
    auto tile = device->dataManager()->getTile(0, 0, false);
    auto *td = tile->tileData();
    QVERIFY(!td->gpuState()->cpuValid());
    auto *store = KisTileDataStore::instance();
    auto *iteration = store->beginIteration();
    const bool swapped = iteration->trySwapOut(td);
    store->endIteration(iteration);
    QVERIFY(swapped);
    QVERIFY(!td->data());
    QCOMPARE(td->gpuState()->slot.load(), KisTileGpuState::InvalidSlot);
    QCOMPARE(readPixels(device, rect)[0], 0.25f);
    // Reload the same state into a fresh slot, then overwrite on the GPU.
    QVERIFY(gpuFill(device, rect, {0.75f, 0.5f, 0.25f, 1}));
    QCOMPARE(readPixels(device, rect)[0], 0.75f);
}

void KisGpuPaintDeviceTest::testEvictionPreservesUndoRedo()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 128, 128);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuFill(device, rect, {0.25f, 0.5f, 0.75f, 1}));
    KisTransaction transaction(device);
    QVERIFY(gpuFill(device, rect, {0.75f, 0.5f, 0.25f, 1}));
    QScopedPointer<KUndo2Command> command(transaction.endAndTake());
    command->redo();
    m_backend->evictTiles(~quint64(0));
    QCOMPARE(readPixels(device, rect)[0], 0.75f);
    command->undo();
    QCOMPARE(readPixels(device, rect)[0], 0.25f);
    command->redo();
    QCOMPARE(readPixels(device, rect)[0], 0.75f);
}

void KisGpuPaintDeviceTest::testCopySourcePreventsEviction()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 64, 64);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    QVERIFY(gpuFill(device, rect, {0.25f, 0.5f, 0.75f, 1}));
    KisPaintDeviceSP copy = new KisPaintDevice(*device);
    auto sourceTile = device->dataManager()->getTile(0, 0, false);
    auto *sourceState = sourceTile->tileData()->gpuState();
    const quint32 sourceSlot = sourceState->slot;
    KisGpuTileAccess access(copy, rect, KisGpuTileAccess::ReadWrite);
    m_commands->begin();
    QVERIFY(access.prepare(*m_commands));
    m_backend->evictTiles(~quint64(0));
    QCOMPARE(sourceState->slot.load(), sourceSlot);
    const quint64 value = KisGpuTileAccess::submitAndFinish(*m_commands, {&access});
    QVERIFY(value);
    QCOMPARE(sourceState->lastUse, value);
    QVERIFY(m_commands->wait());
    m_backend->evictTiles(~quint64(0));
    QCOMPARE(readPixels(copy, rect), readPixels(device, rect));
    QCOMPARE(readPixels(copy, rect)[0], 0.25f);
}

void KisGpuPaintDeviceTest::testConcurrentEviction()
{
    REQUIRE_GPU();
    const QRect rect(0, 0, 128, 128);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    std::atomic<bool> stop{false};
    std::thread evictor([&]() {
        while (!stop.load()) {
            m_backend->evictTiles(~quint64(0));
            std::this_thread::yield();
        }
    });
    auto join = qScopeGuard([&]() {
        stop.store(true);
        evictor.join();
    });
    for (int i = 0; i < 32; ++i) {
        const float value = float(i) / 32;
        QVERIFY(gpuFill(device, rect, {value, 0.5f, 0.25f, 1}));
        const auto pixels = readPixels(device, rect);
        for (size_t p = 0; p < pixels.size(); p += 4) {
            QCOMPARE(pixels[p], value);
            QCOMPARE(pixels[p + 3], 1.0f);
        }
    }
}

void KisGpuPaintDeviceTest::benchmarkTransfers()
{
    REQUIRE_GPU();
    const int size =
        qEnvironmentVariableIsSet("KRITA_GPU_BENCH_SIZE") ? qEnvironmentVariableIntValue("KRITA_GPU_BENCH_SIZE") : 4096;
    const QRect rect(0, 0, size, size);
    KisPaintDeviceSP device = new KisPaintDevice(rgbaFloat());
    fillRandom(device, rect, 12);

    auto timeAccess = [&](KisGpuTileAccess::Mode mode, bool fill) {
        QElapsedTimer timer;
        timer.start();
        KisGpuTileAccess access(device, rect, mode);
        m_commands->begin();
        if (!access.prepare(*m_commands)) {
            return -1.0;
        }
        if (fill) {
            const float color[4] = {0.1f, 0.2f, 0.3f, 1.0f};
            m_fill32->record(*m_commands, access.addresses(), color);
        }
        KisGpuTileAccess::submitAndFinish(*m_commands, {&access});
        m_commands->wait();
        return timer.nsecsElapsed() / 1.0e6;
    };

    const double firstUpload = timeAccess(KisGpuTileAccess::ReadOnly, false);
    const double resident = timeAccess(KisGpuTileAccess::ReadOnly, false);
    const double gpuWrite = timeAccess(KisGpuTileAccess::WriteOnly, true);

    std::vector<float> pixels(size_t(size) * size * 4);
    QElapsedTimer timer;
    timer.start();
    device->readBytes(reinterpret_cast<quint8 *>(pixels.data()), rect);
    const double lazyDownload = timer.nsecsElapsed() / 1.0e6;

    timeAccess(KisGpuTileAccess::WriteOnly, true);
    timer.restart();
    KisGpuTileAccess::syncToCpu(device, rect);
    const double batchedDownload = timer.nsecsElapsed() / 1.0e6;
    timer.restart();
    device->readBytes(reinterpret_cast<quint8 *>(pixels.data()), rect);
    const double readAfterSync = timer.nsecsElapsed() / 1.0e6;

    const int tiles = (size / 64) * (size / 64);
    qInfo().noquote() << QStringLiteral("Device %1x%1 RGBA F32 (%2 tiles, %3 MiB):")
                             .arg(size)
                             .arg(tiles)
                             .arg(double(size) * size * 16 / (1 << 20), 0, 'f', 0);
    qInfo().noquote() << QStringLiteral("  first GPU read (upload all tiles):  %1 ms").arg(firstUpload, 0, 'f', 1);
    qInfo().noquote() << QStringLiteral("  GPU read when resident:             %1 ms").arg(resident, 0, 'f', 1);
    qInfo().noquote() << QStringLiteral("  GPU fill (write-only, in place):    %1 ms").arg(gpuWrite, 0, 'f', 1);
    qInfo().noquote() << QStringLiteral("  readBytes with automatic batching: %1 ms").arg(lazyDownload, 0, 'f', 1);
    qInfo().noquote() << QStringLiteral("  syncToCpu (batched downloads):      %1 ms").arg(batchedDownload, 0, 'f', 1);
    qInfo().noquote() << QStringLiteral("  readBytes after syncToCpu:          %1 ms").arg(readAfterSync, 0, 'f', 1);
    QVERIFY(firstUpload >= 0 && resident >= 0 && gpuWrite >= 0);
}

void KisGpuPaintDeviceTest::benchmarkFiltersAndTransforms()
{
    // GPU engine priority 4 (phase 4.93): CPU cost of the candidate filters and
    // transforms on a document-sized RGBA32F layer, to order the GPU work.
    // Opt-in: several seconds per row.
    if (!qEnvironmentVariableIsSet("KRITA_GPU_BENCHMARK_FILTERS"))
        QSKIP("set KRITA_GPU_BENCHMARK_FILTERS=1");
    REQUIRE_GPU();
    const auto *cs = rgbaFloat();
    const QRect bounds(0, 0, 2480, 3508);
    const int threads = qMax(1, QThread::idealThreadCount());
    KisPaintDeviceSP source = new KisPaintDevice(cs);
    source->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
    fillRandom(source, bounds, 7);
    auto median3 = [](std::vector<double> samples) {
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    };
    auto fresh = [&]() {
        KisPaintDeviceSP device = new KisPaintDevice(*source);
        device->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
        return device;
    };
    auto report = [&](const QString &name, double singleMs, double parallelMs = -1) {
        qInfo().noquote() << QString("%1: %2 ms single call").arg(name, -26).arg(singleMs, 8, 'f', 1)
                          << (parallelMs >= 0 ? QString("| %1 ms in %2 bands").arg(parallelMs, 7, 'f', 1).arg(threads)
                                              : QString());
    };

    // Readback: a whole GPU-written layer brought back to the CPU.
    {
        std::vector<double> samples;
        for (int i = 0; i < 3; ++i) {
            KisPaintDeviceSP device = new KisPaintDevice(cs);
            QVERIFY(gpuFill(device, bounds, {0.2f, 0.4f, 0.6f, 0.8f}));
            QElapsedTimer timer;
            timer.start();
            KisGpuTileAccess::syncToCpu(device, bounds);
            samples.push_back(timer.nsecsElapsed() / 1e6);
        }
        report("readback (whole layer)", median3(samples));
    }

    struct FilterRow {
        QString name;
        QString id;
        QVector<QPair<QString, QVariant>> properties;
        bool gpuConvolution = false; ///< also measured with KisGpuConvolutionWorker (phase 4.98)
    };
    const QVector<FilterRow> filters{
        {"gaussian blur r5", "gaussian blur", {{"horizRadius", 5}, {"vertRadius", 5}}, true},
        {"gaussian blur r30", "gaussian blur", {{"horizRadius", 30}, {"vertRadius", 30}}, true},
        {"gaussian blur r100", "gaussian blur", {{"horizRadius", 100}, {"vertRadius", 100}}, true},
        {"levels", "levels", {{"mode", "lightness"}, {"lightness", "0.08;0.92;1.4;0;1"}}},
        {"curves (all channels)",
         "perchannel",
         {{"curve0", "0,0;0.3,0.2;0.7,0.8;1,1;"},
          {"curve1", "0,0;0.3,0.2;0.7,0.8;1,1;"},
          {"curve2", "0,0;0.3,0.2;0.7,0.8;1,1;"},
          {"curve3", "0,0;0.3,0.2;0.7,0.8;1,1;"},
          {"curve4", "0,0;0.3,0.2;0.7,0.8;1,1;"}}},
        {"hsv adjust s+20", "hsvadjustment", {{"s", 20}}},
        {"unsharp mask", "unsharp", {}, true},
    };
    const bool previousProjection = KisGpuMergeBatch::isEnabled();
    for (const FilterRow &row : filters) {
        auto filter = KisFilterRegistry::instance()->value(row.id);
        if (!filter) {
            qInfo() << "filter not found:" << row.id;
            continue;
        }
        auto config =
            filter->defaultConfiguration(KisGlobalResourcesInterface::instance())->cloneWithResourcesSnapshot();
        for (const auto &property : row.properties)
            config->setProperty(property.first, property.second);
        for (bool gpuConvolution : {false, true}) {
            if (gpuConvolution && !row.gpuConvolution)
                continue;
            KisGpuMergeBatch::setEnabled(gpuConvolution);
            qputenv("KRITA_GPU_CONVOLUTION", gpuConvolution ? "1" : "0");
            const quint64 runs = KisGpuConvolutionWorker::runCount();
            std::vector<double> single, parallel;
            for (int i = 0; i < 3; ++i) {
                KisPaintDeviceSP device = fresh();
                QElapsedTimer timer;
                timer.start();
                filter->process(device, bounds, config);
                single.push_back(timer.nsecsElapsed() / 1e6);
                // Disjoint bands into a separate destination, like a filter stroke's
                // parallel patches.
                KisPaintDeviceSP src = fresh(), dst = fresh();
                const int band = (bounds.height() + threads - 1) / threads;
                timer.restart();
                // QThread workers, as Krita's own (device updates use Qt timers).
                QThreadPool pool;
                pool.setMaxThreadCount(threads);
                for (int t = 0; t < threads; ++t) {
                    const QRect rect = QRect(0, t * band, bounds.width(), band) & bounds;
                    pool.start([&, rect]() {
                        filter->process(src, dst, KisSelectionSP(), rect, config);
                    });
                }
                pool.waitForDone();
                parallel.push_back(timer.nsecsElapsed() / 1e6);
            }
            QCOMPARE(KisGpuConvolutionWorker::runCount() > runs, gpuConvolution);
            report(gpuConvolution ? row.name + " GPU" : row.name, median3(single), median3(parallel));
        }
    }
    // Phase 4.98: one applyGaussian() call on small rects, as projection
    // updates of filter masks and adjustment layers do.
    for (int size : {64, 128, 256, 512, 1024}) {
        for (qreal radius : {5.0, 30.0}) {
            double times[2] = {0.0, 0.0};
            for (bool gpuConvolution : {false, true}) {
                KisGpuMergeBatch::setEnabled(gpuConvolution);
                qputenv("KRITA_GPU_CONVOLUTION", gpuConvolution ? "1" : "0");
                std::vector<double> samples;
                for (int i = 0; i < 9; ++i) {
                    KisPaintDeviceSP device = fresh();
                    const QRect rect(1000, 1500, size, size);
                    QElapsedTimer timer;
                    timer.start();
                    KisGaussianKernel::applyGaussian(device, rect, radius, radius, QBitArray(4, true), nullptr);
                    samples.push_back(timer.nsecsElapsed() / 1e6);
                }
                times[gpuConvolution] = median3(samples);
            }
            qInfo().noquote() << QString("applyGaussian %1x%1 r%2: CPU %3 ms, GPU %4 ms")
                                     .arg(size)
                                     .arg(radius)
                                     .arg(times[0], 0, 'f', 2)
                                     .arg(times[1], 0, 'f', 2);
        }
    }
    qunsetenv("KRITA_GPU_CONVOLUTION");
    KisGpuMergeBatch::setEnabled(previousProjection);

    // Transform Tool final renders (also its live in-stack preview), single call.
    // Phase 4.94: the affine passes on the GPU (KisGpuTransformWorker).
    KisGpuMergeBatch::setEnabled(true);
    for (bool gpuTransform : {false, true}) {
        qputenv("KRITA_GPU_TRANSFORM", gpuTransform ? "1" : "0");
        std::vector<double> samples;
        const quint64 runs = KisGpuTransformWorker::runCount();
        for (int i = 0; i < 3; ++i) {
            KisPaintDeviceSP device = fresh();
            KisFilterStrategy *strategy = new KisBicubicFilterStrategy();
            KisTransformWorker worker(device, 0.9, 0.9, 0, 0, 10.0 * M_PI / 180.0, 120, 80, nullptr, strategy);
            QElapsedTimer timer;
            timer.start();
            worker.run();
            samples.push_back(timer.nsecsElapsed() / 1e6);
            delete strategy;
        }
        QCOMPARE(KisGpuTransformWorker::runCount() - runs, gpuTransform ? quint64(3) : quint64(0));
        report(gpuTransform ? "affine scale+rotate GPU" : "affine scale+rotate bicubic", median3(samples));
    }
    qunsetenv("KRITA_GPU_TRANSFORM");
    KisGpuMergeBatch::setEnabled(previousProjection);
    {
        QVector<QPointF> original, transformed;
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) {
                const QPointF point(bounds.width() * (0.2 + 0.3 * x), bounds.height() * (0.2 + 0.3 * y));
                original << point;
                transformed << (x == 1 && y == 1 ? point + QPointF(140, 90) : point);
            }
        std::vector<double> samples;
        for (int i = 0; i < 3; ++i) {
            KisPaintDeviceSP src = fresh(), dst = new KisPaintDevice(cs);
            dst->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
            KisWarpTransformWorker worker(KisWarpTransformWorker::RIGID_TRANSFORM, original, transformed, 1.0, nullptr);
            QElapsedTimer timer;
            timer.start();
            worker.run(src, dst);
            samples.push_back(timer.nsecsElapsed() / 1e6);
        }
        report("puppet warp (rigid MLS)", median3(samples));
    }
    // Phase 4.96: Puppet Warp's mesh model (the Transform Tool's current
    // model), the polygons on the GPU; three pins, the middle one turned.
    KisGpuMergeBatch::setEnabled(true);
    {
        QImage mask(bounds.size(), QImage::Format_Grayscale8);
        mask.fill(255);
        const auto mesh = KisPuppetTransformWorker::Mesh::build(mask, bounds, 0);
        const QVector<QPointF> pins{QPointF(600, 900), QPointF(1240, 1750), QPointF(1900, 2700)};
        KisPuppetTransformWorker worker(mesh, pins, pins, {0.0, 0.5, 0.0});
        for (bool gpuPuppet : {false, true}) {
            qputenv("KRITA_GPU_PUPPET", gpuPuppet ? "1" : "0");
            std::vector<double> samples;
            const quint64 runs = KisGpuGridWarpWorker::runCount();
            for (int i = 0; i < 3; ++i) {
                KisPaintDeviceSP src = fresh(), dst = new KisPaintDevice(cs);
                dst->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
                QElapsedTimer timer;
                timer.start();
                worker.run(src, dst);
                samples.push_back(timer.nsecsElapsed() / 1e6);
            }
            QCOMPARE(KisGpuGridWarpWorker::runCount() > runs, gpuPuppet);
            report(QString("puppet warp (mesh, %1 groups)%2")
                       .arg(worker.stackingGroups().size())
                       .arg(gpuPuppet ? " GPU" : ""),
                   median3(samples));
        }
        qunsetenv("KRITA_GPU_PUPPET");
    }
    KisGpuMergeBatch::setEnabled(previousProjection);
    // Phase 4.97: the Liquify polygons on the GPU (KisGpuGridWarpWorker).
    KisGpuMergeBatch::setEnabled(true);
    for (bool gpuLiquify : {false, true}) {
        qputenv("KRITA_GPU_LIQUIFY", gpuLiquify ? "1" : "0");
        std::vector<double> samples;
        const quint64 runs = KisGpuGridWarpWorker::runCount();
        for (int i = 0; i < 3; ++i) {
            KisLiquifyTransformWorker worker(bounds, nullptr, 8);
            for (int step = 0; step < 20; ++step)
                worker.translatePoints(QPointF(600 + step * 60, 1200 + step * 40), QPointF(15, 10), 200, false, 1.0);
            KisPaintDeviceSP src = fresh(), dst = new KisPaintDevice(cs);
            dst->setDefaultBounds(new TestUtil::TestingTimedDefaultBounds(bounds));
            QElapsedTimer timer;
            timer.start();
            worker.run(src, dst);
            samples.push_back(timer.nsecsElapsed() / 1e6);
        }
        QCOMPARE(KisGpuGridWarpWorker::runCount() - runs, gpuLiquify ? quint64(3) : quint64(0));
        report(gpuLiquify ? "liquify (20 moves, sigma 200) GPU" : "liquify (20 moves, sigma 200)", median3(samples));
    }
    qunsetenv("KRITA_GPU_LIQUIFY");
    KisGpuMergeBatch::setEnabled(previousProjection);
    qInfo() << "Layer" << bounds.size() << "RGBA32F, threads" << threads
            << "- CPU-resident source except the readback row; medians of 3";
}

SIMPLE_TEST_MAIN(KisGpuPaintDeviceTest)

#include "KisGpuPaintDeviceTest.moc"
