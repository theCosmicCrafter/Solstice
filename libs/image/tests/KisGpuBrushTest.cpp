/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisRenderedDab.h"
#include "gpu/KisGpuBrushPainter.h"
#include "gpu/KisGpuMergeBatch.h"
#include "gpu/KisGpuProjectionCompositor.h"
#include "gpu/KisGpuTileAccess.h"
#include "gpu/KisGpuTileBackend.h"
#include "kis_brush_mask_applicator_base.h"
#include "kis_circle_mask_generator.h"
#include "kis_cubic_curve.h"
#include "kis_curve_circle_mask_generator.h"
#include "kis_default_bounds.h"
#include "kis_gauss_circle_mask_generator.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_painter.h"
#include "kis_pixel_selection.h"
#include "kis_selection.h"
#include "kis_transaction.h"
#include <KisGpuCommandList.h>
#include <KisGpuContext.h>
#include <KisGpuDabCompositor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <KoRgbColorSpaceTraits.h>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <atomic>
#include <chrono>
#include <cmath>
#include <compositeops/KoAlphaDarkenParamsWrapper.h>
#include <compositeops/KoCompositeOpAlphaDarken.h>
#include <compositeops/KoOptimizedCompositeOpFactory.h>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <random>
#include <simpletest.h>
#include <thread>
#include <vector>

namespace
{
// Hold the real queue behind a host-signalled timeline semaphore. A watchdog
// releases it on regression so an accidental synchronous paint fails the test
// rather than hanging the suite. Production code needs no delay injection.
class QueueGate
{
public:
    explicit QueueGate(KisGpuContext &context)
        : m_context(context)
        , m_commands(context)
    {
        m_signal = reinterpret_cast<PFN_vkSignalSemaphore>(
            context.vk().vkGetDeviceProcAddr(context.device(), "vkSignalSemaphore"));
        if (!m_signal)
            return;
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = &type;
        if (context.vk().vkCreateSemaphore(context.device(), &info, nullptr, &m_semaphore) != VK_SUCCESS)
            return;
        m_commands.begin();
        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = m_semaphore;
        wait.value = 1;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        m_submission = m_commands.submit({wait});
        if (m_submission)
            m_watchdog = std::thread([this]() {
                std::unique_lock lock(m_mutex);
                if (!m_changed.wait_for(lock, std::chrono::seconds(10), [this]() {
                        return m_open;
                    })) {
                    m_timedOut = true;
                    signal();
                }
            });
    }
    ~QueueGate()
    {
        open();
        if (m_watchdog.joinable())
            m_watchdog.join();
        m_commands.wait();
        if (m_semaphore)
            m_context.vk().vkDestroySemaphore(m_context.device(), m_semaphore, nullptr);
    }
    bool isValid() const
    {
        return m_submission != 0;
    }
    bool timedOut() const
    {
        return m_timedOut.load();
    }
    bool open()
    {
        std::lock_guard lock(m_mutex);
        const bool ok = !m_semaphore || signal();
        m_changed.notify_all();
        return ok;
    }

private:
    bool signal()
    {
        if (m_open)
            return true;
        VkSemaphoreSignalInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        info.semaphore = m_semaphore;
        info.value = 1;
        m_open = m_signal(m_context.device(), &info) == VK_SUCCESS;
        return m_open;
    }
    KisGpuContext &m_context;
    KisGpuCommandList m_commands;
    PFN_vkSignalSemaphore m_signal = nullptr;
    VkSemaphore m_semaphore = VK_NULL_HANDLE;
    quint64 m_submission = 0;
    std::atomic<bool> m_timedOut{false};
    bool m_open = false;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::thread m_watchdog;
};
// The other HSX modes (phase 4.107), F32 and F16.
const QStringList hsxModes{COMPOSITE_TINT,
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
// Representative modes for the full selection/channel/lifecycle matrix.
const QStringList blendModes = hsxModes
    + QStringList{COMPOSITE_MULT,
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
                  COMPOSITE_DODGE,
                  COMPOSITE_BURN,
                  COMPOSITE_COLOR,
                  COMPOSITE_HUE,
                  COMPOSITE_SATURATION,
                  COMPOSITE_LUMINIZE,
                  // phase 4.107: the remaining modes, F32 and F16
                  COMPOSITE_SOFT_LIGHT_PHOTOSHOP,
                  COMPOSITE_DIVIDE,
                  COMPOSITE_VIVID_LIGHT,
                  COMPOSITE_HARD_MIX,
                  COMPOSITE_HARD_MIX_PHOTOSHOP,
                  COMPOSITE_HARD_MIX_SOFTER_PHOTOSHOP,
                  COMPOSITE_GRAIN_MERGE,
                  COMPOSITE_GRAIN_EXTRACT,
                  COMPOSITE_NEGATION,
                  COMPOSITE_ALLANON,
                  COMPOSITE_DARKER_COLOR,
                  COMPOSITE_LIGHTER_COLOR};
const QStringList channelModes = QStringList{COMPOSITE_OVER, COMPOSITE_ERASE} + blendModes;
const QStringList halfBlendModes = hsxModes
    + QStringList{COMPOSITE_MULT,
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
                  COMPOSITE_DODGE,
                  COMPOSITE_BURN,
                  COMPOSITE_COLOR,
                  COMPOSITE_HUE,
                  COMPOSITE_SATURATION,
                  COMPOSITE_LUMINIZE,
                  // phase 4.107: the remaining modes, F32 and F16
                  COMPOSITE_SOFT_LIGHT_PHOTOSHOP,
                  COMPOSITE_DIVIDE,
                  COMPOSITE_VIVID_LIGHT,
                  COMPOSITE_HARD_MIX,
                  COMPOSITE_HARD_MIX_PHOTOSHOP,
                  COMPOSITE_HARD_MIX_SOFTER_PHOTOSHOP,
                  COMPOSITE_GRAIN_MERGE,
                  COMPOSITE_GRAIN_EXTRACT,
                  COMPOSITE_NEGATION,
                  COMPOSITE_ALLANON,
                  COMPOSITE_DARKER_COLOR,
                  COMPOSITE_LIGHTER_COLOR};
const QStringList dabModes = channelModes + QStringList{COMPOSITE_ALPHA_DARKEN};
class PreviewPaintLayer : public KisPaintLayer
{
public:
    using KisPaintLayer::copyOriginalToProjection;
    using KisPaintLayer::KisPaintLayer;
};
const KoColorSpace *space(bool half = false)
{
    return KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(),
                                                        half ? Float16BitsColorDepthID.id()
                                                             : Float32BitsColorDepthID.id(),
                                                        QString());
}
std::vector<float> pixels(KisPaintDeviceSP device, QRect rect)
{
    std::vector<float> result(size_t(rect.width()) * rect.height() * 4);
    if (device->pixelSize() == 8) {
        QByteArray raw(rect.width() * rect.height() * 8, Qt::Uninitialized);
        device->readBytes(reinterpret_cast<quint8 *>(raw.data()), rect);
        device->colorSpace()->convertPixelsTo(reinterpret_cast<const quint8 *>(raw.constData()),
                                              reinterpret_cast<quint8 *>(result.data()),
                                              space(),
                                              rect.width() * rect.height(),
                                              KoColorConversionTransformation::internalRenderingIntent(),
                                              KoColorConversionTransformation::internalConversionFlags());
    } else {
        device->readBytes(reinterpret_cast<quint8 *>(result.data()), rect);
    }
    return result;
}
QList<KisRenderedDab> makeDabs(int count, int size, bool half = false)
{
    std::mt19937 random(15);
    std::uniform_real_distribution<float> unit(0, 1);
    QList<KisRenderedDab> result;
    for (int i = 0; i < count; ++i) {
        KisFixedPaintDeviceSP device = new KisFixedPaintDevice(space());
        device->setRect(QRect(9, -5, size, size)); // nonzero fixed-device origin
        device->initialize();
        auto *data = reinterpret_cast<float *>(device->data());
        for (int p = 0; p < size * size; ++p) {
            data[4 * p] = unit(random) * 2; // HDR color
            data[4 * p + 1] = unit(random);
            data[4 * p + 2] = unit(random);
            data[4 * p + 3] = p % 7 == 0 ? 0 : (p % 11 == 0 ? 1 : unit(random));
        }
        if (half)
            device->convertTo(space(true));
        KisRenderedDab dab(device);
        dab.offset = QPoint(-75 + i * 7, -21 + i * 3);
        dab.opacity = i % 5 == 0 ? 1 : 0.37;
        dab.flow = 0.43; // ignored by Normal, unlike Alpha Darken
        result << dab;
    }
    return result;
}
/// A default or Gaussian circle auto-brush dab generated by the CPU
/// (vectorized applicator, as KisAutoBrush does) and its GPU description.
struct CircleDabParameters {
    qreal diameter, ratio, hfade, vfade;
    bool antialias;
    qreal softness, scale, angle;
    QPointF subPixel;
    bool gauss = false;
    QString softCurve = QString(); // non-empty: a Soft (curve) circle with this curve
};
bool generateCircleDab(const CircleDabParameters &parameters,
                       const float color[4],
                       KisFixedPaintDeviceSP *device,
                       QSharedPointer<const KisProceduralCircleDab> *description,
                       bool halfDab = false)
{
    const half halfColor[4] = {half(color[0]), half(color[1]), half(color[2]), half(color[3])};
    QScopedPointer<KisCircleMaskGenerator> circleGenerator;
    QScopedPointer<KisGaussCircleMaskGenerator> gaussGenerator;
    QScopedPointer<KisCurveCircleMaskGenerator> softGenerator;
    KisMaskGenerator *generator = nullptr;
    if (!parameters.softCurve.isEmpty()) {
        softGenerator.reset(new KisCurveCircleMaskGenerator(parameters.diameter,
                                                            parameters.ratio,
                                                            parameters.hfade,
                                                            parameters.vfade,
                                                            2,
                                                            KisCubicCurve(parameters.softCurve),
                                                            parameters.antialias));
        generator = softGenerator.data();
    } else if (parameters.gauss) {
        gaussGenerator.reset(new KisGaussCircleMaskGenerator(parameters.diameter,
                                                             parameters.ratio,
                                                             parameters.hfade,
                                                             parameters.vfade,
                                                             2,
                                                             parameters.antialias));
        generator = gaussGenerator.data();
    } else {
        circleGenerator.reset(new KisCircleMaskGenerator(parameters.diameter,
                                                         parameters.ratio,
                                                         parameters.hfade,
                                                         parameters.vfade,
                                                         2,
                                                         parameters.antialias));
        generator = circleGenerator.data();
    }
    generator->setSoftness(parameters.softness);
    generator->setScale(parameters.scale, parameters.scale);
    if (!generator->shouldVectorize())
        return false;
    const int side = int(std::ceil(parameters.diameter * parameters.scale)) + 3;
    *device = new KisFixedPaintDevice(space(halfDab));
    (*device)->setRect(QRect(0, 0, side, side));
    (*device)->lazyGrowBufferWithoutInitialization();
    MaskProcessingData data(*device,
                            space(halfDab),
                            halfDab ? reinterpret_cast<const quint8 *>(halfColor)
                                    : reinterpret_cast<const quint8 *>(color),
                            0.0,
                            1.0,
                            side / 2.0 - 0.5 + parameters.subPixel.x(),
                            side / 2.0 - 0.5 + parameters.subPixel.y(),
                            parameters.angle);
    KisBrushMaskApplicatorBase *applicator = generator->applicator();
    applicator->initializeData(&data);
    applicator->process(QRect(0, 0, side, side));

    QSharedPointer<KisProceduralCircleDab> circle(new KisProceduralCircleDab());
    circle->centerX = float(data.centerX);
    circle->centerY = float(data.centerY);
    circle->cosa = float(data.cosa);
    circle->sina = float(data.sina);
    if (softGenerator) {
        const auto coefficients = softGenerator->vectorCoefficients();
        circle->kind = KisProceduralCircleDab::SoftCircle;
        circle->xcoef = float(coefficients.xcoef);
        circle->ycoef = float(coefficients.ycoef);
        circle->curveResolution = float(coefficients.curveResolution);
        circle->radius = float(coefficients.radius);
        circle->fadeStart = float(coefficients.fadeStart);
        circle->fadeStartValue = float(coefficients.fadeStartValue);
        circle->fadeCoeff = float(coefficients.fadeCoeff);
        circle->antialias = coefficients.antialias;
        QVector<float> table;
        for (qreal value : softGenerator->curveTable())
            table << float(value);
        circle->curveTable.reset(new QVector<float>(table));
    } else if (parameters.gauss) {
        const auto coefficients = gaussGenerator->vectorCoefficients();
        circle->kind = KisProceduralCircleDab::GaussCircle;
        circle->ycoef = float(coefficients.ycoef);
        circle->distfactor = float(coefficients.distfactor);
        circle->center = float(coefficients.center);
        circle->alphafactor = float(coefficients.alphafactor);
        circle->radius = float(coefficients.radius);
        circle->fadeStart = float(coefficients.fadeStart);
        circle->fadeStartValue = float(coefficients.fadeStartValue);
        circle->fadeCoeff = float(coefficients.fadeCoeff);
        circle->antialias = coefficients.antialias;
    } else {
        const auto coefficients = circleGenerator->vectorCoefficients();
        circle->xcoef = float(coefficients.xcoef);
        circle->ycoef = float(coefficients.ycoef);
        circle->fadeX = float(coefficients.fadeX);
        circle->fadeY = float(coefficients.fadeY);
        circle->antialias = coefficients.antialias;
    }
    for (int i = 0; i < 4; ++i)
        circle->color[i] = halfDab ? float(halfColor[i]) : color[i];
    circle->halfPixels = halfDab;
    *description = circle;
    return true;
}
QRect boundsOf(const QList<KisRenderedDab> &dabs)
{
    QRect bounds;
    for (const auto &dab : dabs)
        bounds |= dab.realBounds();
    return bounds;
}
float difference(const std::vector<float> &a, const std::vector<float> &b, bool ignoreTransparentRgb = true)
{
    float maximum = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        // As for GPU projection parity, transparent RGB is unspecified:
        // the CPU SIMD and scalar Over paths differ for these channels.
        const size_t alpha = i / 4 * 4 + 3;
        if (ignoreTransparentRgb && i % 4 != 3 && a[alpha] == 0 && b[alpha] == 0)
            continue;
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]))
            return INFINITY;
        maximum = qMax(maximum, std::abs(a[i] - b[i]));
    }
    return maximum;
}
} // namespace
class KisGpuBrushTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        if (!KisGpuTileBackend::instance())
            QSKIP("No GPU backend");
        qputenv("KRITA_GPU_BRUSH", "1");
        KisGpuMergeBatch::setEnabled(true);
    }
    void cleanupTestCase()
    {
        QVERIFY(KisGpuBrushPainter::resetStagingForTesting());
        if (auto *backend = KisGpuTileBackend::existingInstance()) {
            backend->flush();
            QCOMPARE(backend->context().validationErrorCount(), 0);
        }
    }
    void testProjectionWorkContexts_data()
    {
        QTest::addColumn<bool>("half");
        QTest::addColumn<bool>("alphaLocked");
        QTest::addColumn<int>("finish");
        for (bool half : {false, true})
            for (bool alphaLocked : {false, true})
                for (int finish = 0; finish < 3; ++finish)
                    QTest::newRow(qPrintable(QString("f16%1-lock%2-finish%3").arg(half).arg(alphaLocked).arg(finish)))
                        << half << alphaLocked << finish;
    }
    void testProjectionWorkContexts()
    {
        QFETCH(bool, half);
        QFETCH(bool, alphaLocked);
        QFETCH(int, finish);
        QVERIFY(KisGpuProjectionCompositor::resetWorkContextsForTesting());
        QCOMPARE(KisGpuProjectionCompositor::workContextCountForTesting(), 0);
        auto &context = KisGpuTileBackend::instance()->context();
        context.waitIdle();
        const auto *cs = space(half);
        const QRect bounds(-75, -21, 129, 97);
        KoColor background(cs);
        cs->fromNormalisedChannelsValue(background.data(), {1.2f, -0.2f, 0.7f, 0.6f});
        KisPaintDeviceSP cpu = new KisPaintDevice(cs);
        cpu->fill(bounds, background);
        KisPaintDeviceSP gpu = new KisPaintDevice(*cpu), snapshot = new KisPaintDevice(*gpu);
        const auto before = pixels(cpu, bounds);
        KisTransaction transaction(gpu);
        const QString ids[]{COMPOSITE_MULT, COMPOSITE_SCREEN, COMPOSITE_OVERLAY};
        const KisGpuBlendOp ops[]{KisGpuBlendOp::Multiply, KisGpuBlendOp::Screen, KisGpuBlendOp::Overlay};
        auto paintPass = [&](int index, bool failSubmit) {
            KisPaintDeviceSP source = new KisPaintDevice(cs);
            KoColor color(cs);
            cs->fromNormalisedChannelsValue(color.data(), {0.2f + index * 0.15f, 0.7f, 0.4f, 0.8f});
            source->fill(bounds, color);
            QByteArray coverage(bounds.width() * bounds.height(), Qt::Uninitialized);
            for (int i = 0; i < coverage.size(); ++i)
                coverage[i] = char((17 * i + 19 * index) % 256);
            KisSelectionSP selection = new KisSelection();
            selection->pixelSelection()->writeBytes(reinterpret_cast<const quint8 *>(coverage.constData()), bounds);
            const quint8 opacity = 80 + index * 31;
            auto paintCpu = [&](KisPaintDeviceSP device) {
                KisPainter painter(device);
                painter.setCompositeOpId(ids[index % 3]);
                painter.setOpacityU8(opacity);
                painter.setSelection(selection);
                QBitArray channels(4, true);
                channels.setBit(3, !alphaLocked);
                painter.setChannelFlags(channels);
                painter.bitBlt(bounds.topLeft(), source, bounds);
            };
            paintCpu(cpu);
            KisGpuProjectionCompositor::Layer layer{source, ops[index % 3], opacity / 255.0f};
            layer.channelMask = alphaLocked ? 7 : 15;
            KisGpuLayerCompositor::Mask mask{reinterpret_cast<const quint8 *>(coverage.constData()), bounds};
            if (failSubmit)
                context.injectSubmitFailuresForTesting(1);
            const bool ok = KisGpuProjectionCompositor::composite(gpu, bounds, {layer}, nullptr, &mask);
            if (failSubmit && !ok)
                paintCpu(gpu); // replay exactly once after the failed submission
            return failSubmit ? !ok : ok;
        };
        QueueGate gate(context);
        QVERIFY(gate.isValid());
        const auto completed = context.completedValue();
        for (int i = 0; i < 3; ++i) {
            QVERIFY(paintPass(i, false));
            QVERIFY2(!gate.timedOut(), "projection waited instead of retaining separate commands/tables/masks");
            QCOMPARE(context.completedValue(), completed);
            QCOMPARE(KisGpuProjectionCompositor::workContextCountForTesting(), i + 1);
            // Sources, layer descriptors and mask storage have already been destroyed.
        }
        std::vector<float> after;
        std::promise<void> entered;
        auto started = entered.get_future();
        if (finish) {
            auto fourth = std::async(std::launch::async, [&]() {
                entered.set_value();
                return paintPass(3, finish == 2);
            });
            started.wait();
            const bool waiting = fourth.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
            // These take the pool mutex: a waiting lease must not hold it.
            QElapsedTimer leaseTimer;
            leaseTimer.start();
            while (!KisGpuProjectionCompositor::leasedWorkContextCountForTesting() && leaseTimer.elapsed() < 2000)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const bool leased = KisGpuProjectionCompositor::leasedWorkContextCountForTesting() == 1;
            const int count = KisGpuProjectionCompositor::workContextCountForTesting();
            const bool reset = leased ? KisGpuProjectionCompositor::resetWorkContextsForTesting() : true;
            QVERIFY(gate.open());
            QVERIFY(fourth.get());
            QVERIFY(waiting);
            QVERIFY(leased);
            QCOMPARE(count, 3);
            QVERIFY(!reset);
            after = pixels(gpu, bounds);
        } else {
            auto reader = std::async(std::launch::async, [&]() {
                entered.set_value();
                return pixels(gpu, bounds);
            });
            started.wait();
            const bool waiting = reader.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
            QVERIFY(gate.open());
            after = reader.get();
            QVERIFY(waiting);
        }
        QVERIFY(!gate.timedOut());
        QVERIFY(difference(pixels(cpu, bounds), after, false) <= (half ? 0.004f : 2e-5f));
        QCOMPARE(pixels(snapshot, bounds), before);
        QScopedPointer<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        command->undo();
        QCOMPARE(pixels(gpu, bounds), before);
        command->redo();
        QCOMPARE(pixels(gpu, bounds), after);
        // Completed work reuses the cache without allocating more contexts.
        QVERIFY(paintPass(4, false));
        QCOMPARE(KisGpuProjectionCompositor::workContextCountForTesting(), 3);
        QVERIFY(difference(pixels(cpu, bounds), pixels(gpu, bounds), false) <= (half ? 0.004f : 2e-5f));
        QVERIFY(KisGpuProjectionCompositor::resetWorkContextsForTesting());
        QCOMPARE(KisGpuProjectionCompositor::workContextCountForTesting(), 0);
    }
    void testPendingBatches_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("wrapRing");
        QTest::addColumn<bool>("half");
        for (const auto &mode : {COMPOSITE_OVER, COMPOSITE_ALPHA_DARKEN, COMPOSITE_ERASE, COMPOSITE_MULT})
            for (bool wrapRing : {false, true})
                QTest::newRow(qPrintable(mode + (wrapRing ? "-reuse" : "-read"))) << mode << wrapRing << false;
        for (const auto &mode : {COMPOSITE_OVER,
                                 COMPOSITE_ERASE,
                                 COMPOSITE_ALPHA_DARKEN,
                                 COMPOSITE_MULT,
                                 COMPOSITE_SCREEN,
                                 COMPOSITE_OVERLAY,
                                 COMPOSITE_SOFT_LIGHT_SVG,
                                 COMPOSITE_DODGE,
                                 COMPOSITE_COLOR})
            for (bool wrapRing : {false, true})
                QTest::newRow(qPrintable(mode + (wrapRing ? "-f16-reuse" : "-f16-read"))) << mode << wrapRing << true;
    }
    void testHalfDabs_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("channels");
        QTest::addColumn<bool>("masked");
        for (const auto &mode : QStringList{COMPOSITE_OVER, COMPOSITE_ERASE} + halfBlendModes)
            for (int channels = -1; channels < 16; ++channels)
                for (bool masked : {false, true})
                    QTest::newRow(qPrintable(QString("%1-channels%2-mask%3").arg(mode).arg(channels).arg(masked)))
                        << mode << channels << masked;
    }
    void testHalfDabs()
    {
        QFETCH(QString, mode);
        QFETCH(int, channels);
        QFETCH(bool, masked);
        const auto *cs = space(true);
        QVERIFY(cs);
        const QRect bounds(-128, -64, 256, 192);
        auto read = [&](KisPaintDeviceSP device) {
            return pixels(device, bounds);
        };
        KisPaintDeviceSP cpu = new KisPaintDevice(cs);
        KoColor background(cs);
        cs->fromNormalisedChannelsValue(background.data(), {1.2f, -0.2f, 0.7f, 0.6f});
        cpu->fill(bounds, background);
        const float alphas[] = {0.001f, 0.0021f, 0.998f, 1.0f};
        for (int i = 0; i < 4; ++i) {
            cs->fromNormalisedChannelsValue(background.data(), {1.2f, -0.2f, 0.7f, alphas[i]});
            cpu->fill(QRect(bounds.x() + i * 64, bounds.y(), 64, bounds.height()), background);
        }
        cs->fromNormalisedChannelsValue(background.data(), {0.3f, 0.8f, -0.1f, 0.0f});
        cpu->fill(bounds.adjusted(21, 17, -73, -41), background);
        KisPaintDeviceSP gpu = new KisPaintDevice(*cpu), shared = new KisPaintDevice(*gpu);
        const auto before = read(cpu);
        KisPainter cp(cpu), gp(gpu);
        cp.setCompositeOpId(mode);
        gp.setCompositeOpId(mode);
        if (channels >= 0) {
            QBitArray flags(4);
            for (int i = 0; i < 4; ++i)
                flags.setBit(i, channels & (1 << i));
            cp.setChannelFlags(flags);
            gp.setChannelFlags(flags);
        }
        if (masked) {
            KisSelectionSP selection = new KisSelection();
            QByteArray mask(bounds.width() * bounds.height(), Qt::Uninitialized);
            for (int i = 0; i < mask.size(); ++i)
                mask[i] = char(i % 256);
            selection->pixelSelection()->writeBytes(reinterpret_cast<const quint8 *>(mask.constData()), bounds);
            cp.setSelection(selection);
            gp.setSelection(selection);
        }
        auto dabs = makeDabs(24, 73, true);
        KisTransaction transaction(gpu);
        const auto count = KisGpuBrushPainter::batchCount();
        cp.bltFixed(bounds, dabs);
        const QVector<QRect> clips{bounds};
        QVERIFY(KisGpuBrushPainter::paint(&gp, dabs, &clips));
        QCOMPARE(KisGpuBrushPainter::batchCount(), count + 1);
        const auto after = read(gpu);
        const float error = difference(read(cpu), after, false);
        qInfo() << "F16 dab maximum error" << error;
        // One half ULP at 1.0 for repeated HDR dabs; include hidden RGB.
        QVERIFY2(error <= 1.0f / 1024, qPrintable(QString::number(error)));
        QScopedPointer<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        QCOMPARE(read(shared), before);
        command->undo();
        QCOMPARE(read(gpu), before);
        command->redo();
        QCOMPARE(read(gpu), after);
        // A refused GPU write can be replayed once on the CPU, without losing
        // the original content or interpreting half pixels as float pixels.
        KisGpuTileBackend::instance()->context().injectSubmitFailuresForTesting(1);
        QVERIFY(!KisGpuBrushPainter::paint(&gp, dabs, &clips));
        QCOMPARE(read(gpu), after);
        cp.bltFixed(bounds, dabs);
        gp.bltFixed(bounds, dabs);
        QVERIFY(difference(read(cpu), read(gpu), false) <= 1.0f / 1024);
        gp.setCompositeOpId(COMPOSITE_DISSOLVE); // no GPU blend op
        QVERIFY(!KisGpuBrushPainter::supports(&gp));
        gp.setCompositeOpId(COMPOSITE_ALPHA_DARKEN);
        gp.setChannelFlags(QBitArray(4, false));
        QVERIFY(!KisGpuBrushPainter::supports(&gp));
    }
    void testPendingBatches()
    {
        QFETCH(QString, mode);
        QFETCH(bool, wrapRing);
        QFETCH(bool, half);
        const float tolerance = half ? 1.0f / 1024 : 2e-5f;
        QVERIFY(KisGpuBrushPainter::resetStagingForTesting());
        auto &context = KisGpuTileBackend::instance()->context();
        context.waitIdle();
        const QRect bounds(-128, -64, 256, 192);
        KisPaintDeviceSP cpu = new KisPaintDevice(space(half));
        KoColor background(space(half));
        space(half)->fromNormalisedChannelsValue(background.data(), {1.2f, -0.2f, 0.7f, 0.6f});
        cpu->fill(bounds, background);
        KisPaintDeviceSP gpu = new KisPaintDevice(*cpu), snapshot = new KisPaintDevice(*gpu);
        const auto before = pixels(cpu, bounds);
        KisSelectionSP selection = new KisSelection();
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        cpuPainter.setCompositeOpId(mode);
        gpuPainter.setCompositeOpId(mode);
        cpuPainter.setSelection(selection);
        gpuPainter.setSelection(selection);
        KisTransaction transaction(gpu);
        const quint64 completed = context.completedValue();
        QueueGate gate(context);
        QVERIFY(gate.isValid());
        // Distinct source pixels and masks are freed/overwritten immediately
        // after each submission, while none of the queued work can execute.
        for (int i = 0; i < 3; ++i) {
            selection->pixelSelection()->clear();
            selection->pixelSelection()->select(bounds.adjusted(13 * i, 7 * i, -9 * i, -5 * i), 71 + 70 * i);
            auto dabs = makeDabs(3 + i, 73 + i, half);
            for (auto &dab : dabs) {
                dab.opacity = 0.2 + 0.3 * i;
                dab.averageOpacity = 0.9;
            }
            cpuPainter.bltFixed(bounds, dabs);
            QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
            QVERIFY2(!gate.timedOut(), "painting waited for GPU completion instead of retaining the batch");
            QCOMPARE(context.completedValue(), completed);
        }
        QCOMPARE(KisGpuBrushPainter::stagingStatistics().contexts, 3);
        std::vector<float> after;
        std::promise<void> started;
        auto entered = started.get_future();
        if (wrapRing) {
            const auto fourth = makeDabs(4, 81, half);
            cpuPainter.bltFixed(bounds, fourth);
            auto pendingPaint = std::async(std::launch::async, [&]() {
                started.set_value();
                return KisGpuBrushPainter::paint(&gpuPainter, fourth);
            });
            entered.wait();
            // A fourth batch must wait before overwriting the first slot.
            QVERIFY(pendingPaint.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
            QVERIFY(gate.open());
            QVERIFY(pendingPaint.get());
            after = pixels(gpu, bounds);
        } else {
            auto pendingRead = std::async(std::launch::async, [&]() {
                started.set_value();
                return pixels(gpu, bounds);
            });
            entered.wait();
            QVERIFY(pendingRead.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
            QVERIFY(gate.open());
            after = pendingRead.get();
        }
        QScopedPointer<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        QVERIFY(difference(pixels(cpu, bounds), after) <= tolerance);
        QCOMPARE(pixels(snapshot, bounds), before);
        command->undo();
        QCOMPARE(pixels(gpu, bounds), before);
        command->redo();
        QCOMPARE(pixels(gpu, bounds), after);
        // Ring wraparound and failed submission must leave the latest image
        // intact so that the caller can safely replay the refused batch on CPU.
        const auto dabs = makeDabs(4, 81, half);
        context.injectSubmitFailuresForTesting(1);
        QVERIFY(!KisGpuBrushPainter::paint(&gpuPainter, dabs));
        gpuPainter.bltFixed(bounds, dabs);
        cpuPainter.bltFixed(bounds, dabs);
        QVERIFY(difference(pixels(cpu, bounds), pixels(gpu, bounds)) <= tolerance);
    }
    void testStagingBudgetAndReuse()
    {
        QVERIFY(KisGpuBrushPainter::resetStagingForTesting());
        KisPaintDeviceSP cpu = new KisPaintDevice(space()), gpu = new KisPaintDevice(space());
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        const QRect clip(-75, -21, 8, 8);
        const QVector<QRect> rects{clip};
        quint64 smallCapacity = 0;
        for (int size : {61, 62, 63, 64, 1400, 1400, 1400, 1600, 2000, 61}) {
            const auto dabs = makeDabs(1, size);
            cpuPainter.bltFixed(clip, dabs);
            QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs, &rects));
            const auto stats = KisGpuBrushPainter::stagingStatistics();
            QVERIFY(stats.bytes <= KisGpuDabCompositor::MaxUploadBytes);
            QVERIFY(stats.contexts <= 3);
            if (size == 63)
                smallCapacity = stats.bytes;
            if (size == 64)
                QCOMPARE(stats.bytes, smallCapacity); // same bucket, no new allocation
        }
        QVERIFY(difference(pixels(cpu, clip), pixels(gpu, clip)) <= 2e-5f);
        const auto before = pixels(gpu, clip);
        const auto stats = KisGpuBrushPainter::stagingStatistics();
        const auto oversized = makeDabs(1, 2048); // 64 MiB source plus metadata
        QVERIFY(!KisGpuBrushPainter::paint(&gpuPainter, oversized, &rects));
        QCOMPARE(KisGpuBrushPainter::stagingStatistics().bytes, stats.bytes);
        QCOMPARE(pixels(gpu, clip), before);
        QVERIFY(KisGpuBrushPainter::resetStagingForTesting());
        QCOMPARE(KisGpuBrushPainter::stagingStatistics().bytes, quint64(0));
        QCOMPARE(KisGpuBrushPainter::stagingStatistics().contexts, 0);
    }
    void testLargeStagingReusesFittingContexts()
    {
        QVERIFY(KisGpuBrushPainter::resetStagingForTesting());
        KisPaintDeviceSP cpu = new KisPaintDevice(space()), gpu = new KisPaintDevice(space());
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        const QRect clip(-75, -21, 8, 8);
        const QVector<QRect> rects{clip};
        quint64 capacity = 0;
        for (int i = 0; i < 6; ++i) {
            const auto dabs = makeDabs(1, 1400); // ~30 MiB: two fit, three do not.
            cpuPainter.bltFixed(clip, dabs);
            QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs, &rects));
            const auto stats = KisGpuBrushPainter::stagingStatistics();
            QCOMPARE(stats.contexts, qMin(i + 1, 2));
            QVERIFY(stats.bytes <= KisGpuDabCompositor::MaxUploadBytes);
            if (i == 1)
                capacity = stats.bytes;
            if (i > 1)
                QCOMPARE(stats.bytes, capacity);
        }
        QVERIFY(difference(pixels(cpu, clip), pixels(gpu, clip)) <= 2e-5f);
        QVERIFY(KisGpuBrushPainter::resetStagingForTesting());
    }
    void testParityUndoAndCpuWrite_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("normal") << COMPOSITE_OVER;
        QTest::newRow("alpha-darken") << COMPOSITE_ALPHA_DARKEN;
        QTest::newRow("erase") << COMPOSITE_ERASE;
        for (const auto &mode : blendModes)
            QTest::newRow(qPrintable(mode)) << mode;
    }
    void testParityUndoAndCpuWrite()
    {
        QFETCH(QString, mode);
        auto dabs = makeDabs(25, 89);
        for (int i = 0; i < dabs.size(); ++i) {
            dabs[i].flow = i % 3 == 0 ? 1 : 0.43;
            dabs[i].averageOpacity = i % 2 == 0 ? 0.85 : dabs[i].opacity;
        }
        KisRenderedDab repeated = dabs.first();
        repeated.offset += QPoint(15, -10);
        repeated.opacity = 0.23;
        dabs << repeated; // same source storage, different position and opacity
        const QRect bounds = boundsOf(dabs).adjusted(-70, -70, 70, 70);
        KisPaintDeviceSP cpu = new KisPaintDevice(space());
        cpu->moveTo(-13, 7);
        const float background[] = {0.8f, 0.2f, 0.3f, 0.6f};
        cpu->fill(bounds.x(),
                  bounds.y(),
                  bounds.width(),
                  bounds.height(),
                  reinterpret_cast<const quint8 *>(background));
        KisPaintDeviceSP gpu = new KisPaintDevice(*cpu);
        KisPaintDeviceSP snapshot = new KisPaintDevice(*gpu);
        const auto before = pixels(gpu, bounds);
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        cpuPainter.setCompositeOpId(mode);
        gpuPainter.setCompositeOpId(mode);
        cpuPainter.bltFixed(bounds, dabs);
        KisTransaction transaction(gpu);
        QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
        QScopedPointer<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        const auto after = pixels(gpu, bounds);
        const float error = difference(pixels(cpu, bounds), after);
        QVERIFY2(error <= 2e-5f, qPrintable(QString::number(error)));
        QCOMPARE(pixels(snapshot, bounds), before);
        command->undo();
        QCOMPARE(pixels(gpu, bounds), before);
        command->redo();
        QCOMPARE(pixels(gpu, bounds), after);
        gpuPainter.bltFixed(bounds, dabs);
        cpuPainter.bltFixed(bounds, dabs);
        const float repeatedError = difference(pixels(cpu, bounds), pixels(gpu, bounds));
        QVERIFY2(repeatedError <= 2e-5f, qPrintable(QString::number(repeatedError)));
    }
    void testRefusalAndFailure_data()
    {
        testParityUndoAndCpuWrite_data();
    }
    void testRefusalAndFailure()
    {
        QFETCH(QString, mode);
        const auto dabs = makeDabs(4, 73);
        const QRect bounds = boundsOf(dabs);
        KisPaintDeviceSP device = new KisPaintDevice(space());
        KisPainter painter(device);
        painter.setCompositeOpId(mode);
        const auto before = pixels(device, bounds);
        painter.setCompositeOpId(COMPOSITE_DODGE_HDR); // HDR variant still uses CPU.
        QVERIFY(!KisGpuBrushPainter::paint(&painter, dabs));
        painter.setCompositeOpId(mode);
        QBitArray flags(mode == COMPOSITE_ALPHA_DARKEN ? 4 : 3, true);
        flags.clearBit(flags.size() - 1); // malformed Normal flags; restricted Alpha Darken
        painter.setChannelFlags(flags);
        QVERIFY(!KisGpuBrushPainter::paint(&painter, dabs));
        painter.setChannelFlags(QBitArray());
        KisPaintDeviceSP integer = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
        KisPainter integerPainter(integer);
        QVERIFY(!KisGpuBrushPainter::paint(&integerPainter, dabs));
        if (mode == COMPOSITE_ALPHA_DARKEN) {
            auto invalid = dabs;
            invalid.last().flow = 1.1;
            QVERIFY(!KisGpuBrushPainter::paint(&painter, invalid));
            invalid.last().flow = 0.4;
            invalid.last().averageOpacity = NAN;
            QVERIFY(!KisGpuBrushPainter::paint(&painter, invalid));
            QCOMPARE(pixels(device, bounds), before);
        }
        auto *backend = KisGpuTileBackend::instance();
        backend->context().injectSubmitFailuresForTesting(1);
        QVERIFY(!KisGpuBrushPainter::paint(&painter, dabs));
        QCOMPARE(pixels(device, bounds), before);
        backend->flush();
        backend->evictTiles(~quint64(0));
        const auto budget = backend->memoryBudget();
        auto restore = qScopeGuard([&]() {
            backend->setMemoryBudgetForTesting(budget);
        });
        backend->setMemoryBudgetForTesting(backend->pool(16)->tileBytes());
        QVERIFY(!KisGpuBrushPainter::paint(&painter, dabs));
        QCOMPARE(pixels(device, bounds), before);
        QVERIFY(!backend->hasFailed());
    }
    void testMirroring_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("mirrors");
        QTest::addColumn<bool>("masked");
        QTest::addColumn<bool>("fractional");
        QTest::addColumn<bool>("locked");
        QTest::addColumn<bool>("failPass");
        for (const auto &mode : dabModes) {
            for (int mirrors : {1, 2, 3}) {
                for (bool masked : {false, true}) {
                    for (bool fractional : {false, true}) {
                        const auto name = QStringLiteral("%1-mirror%2-selection%3-fractional%4")
                                              .arg(mode)
                                              .arg(mirrors)
                                              .arg(masked)
                                              .arg(fractional);
                        QTest::newRow(qPrintable(name)) << mode << mirrors << masked << fractional << false << false;
                    }
                }
            }
        }
        QTest::newRow("alpha-locked-selected-both") << COMPOSITE_OVER << 3 << true << false << true << false;
        QTest::newRow("failed-mirror-submit") << COMPOSITE_OVER << 3 << true << false << false << true;
    }
    void testMirroring()
    {
        runMirroringTest(false);
    }
    void testCombinedMirroring_data()
    {
        testMirroring_data();
    }
    void testCombinedMirroring()
    {
        runMirroringTest(true);
    }

    void testGeneratedCircleDabs_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("mirrors"); // 0 none, 1 pixel-mirrored passes, 2 combined passes
        QTest::addColumn<bool>("halfDabs");
        QTest::newRow("normal") << COMPOSITE_OVER << 0 << false;
        QTest::newRow("alpha-darken") << COMPOSITE_ALPHA_DARKEN << 0 << false;
        QTest::newRow("erase") << COMPOSITE_ERASE << 0 << false;
        QTest::newRow("multiply") << COMPOSITE_MULT << 0 << false;
        QTest::newRow("normal-pixel-mirrors") << COMPOSITE_OVER << 1 << false;
        QTest::newRow("alpha-darken-combined-mirrors") << COMPOSITE_ALPHA_DARKEN << 2 << false;
        // Phase 4.86: RGBA16F devices.
        QTest::newRow("half-normal") << COMPOSITE_OVER << 0 << true;
        QTest::newRow("half-alpha-darken") << COMPOSITE_ALPHA_DARKEN << 0 << true;
        QTest::newRow("half-erase") << COMPOSITE_ERASE << 0 << true;
        QTest::newRow("half-normal-pixel-mirrors") << COMPOSITE_OVER << 1 << true;
        QTest::newRow("half-alpha-darken-combined-mirrors") << COMPOSITE_ALPHA_DARKEN << 2 << true;
    }
    void testGeneratedCircleDabs()
    {
        // Phase 4.83: the GPU evaluates described circle dabs instead of
        // reading their pixels. The GPU dabs carry zeroed pixels, so only a
        // correct evaluation can match the CPU blit of the real pixels.
        QFETCH(QString, mode);
        QFETCH(int, mirrors);
        QFETCH(bool, halfDabs);
        const int pixelSize = halfDabs ? 8 : 16;
        const QVector<CircleDabParameters> shapes = {
            {12.0, 1.0, 1.0, 1.0, true, 1.0, 1.0, 0.0, QPointF(0.0, 0.0)},
            {37.3, 0.6, 0.7, 0.6, true, 1.0, 1.0, 0.37, QPointF(0.25, 0.8)},
            {90.0, 1.0, 0.2, 0.9, false, 0.5, 1.0, 2.1, QPointF(0.5, 0.125)},
            {64.0, 0.45, 1.0, 0.3, true, 0.8, 1.3, -1.2, QPointF(0.9, 0.4)},
            {150.0, 1.0, 0.5, 0.5, true, 1.0, 0.75, 0.785, QPointF(0.33, 0.66)},
            // Gaussian: the measured preset (fades 1, a hard edge), soft and
            // asymmetric fades, with and without antialiasing.
            {256.0, 1.0, 1.0, 1.0, true, 1.0, 1.0, 0.61, QPointF(0.37, 0.52), true},
            {48.0, 1.0, 0.3, 0.3, true, 1.0, 1.0, 0.0, QPointF(0.0, 0.0), true},
            {97.5, 0.7, 0.8, 0.2, false, 1.0, 1.0, 1.1, QPointF(0.75, 0.25), true},
            {130.0, 1.0, 0.55, 0.55, true, 1.0, 0.9, -0.4, QPointF(0.1, 0.9), true},
            // Soft (curve): the default curve, and a custom one with softness.
            {256.0, 1.0, 1.0, 1.0, true, 1.0, 1.0, 0.3, QPointF(0.4, 0.6), false, QStringLiteral("0,1;1,0;")},
            {97.5,
             0.7,
             1.0,
             1.0,
             false,
             0.6,
             1.0,
             1.1,
             QPointF(0.75, 0.25),
             false,
             QStringLiteral("0,1;0.3,0.85;0.7,0.2;1,0;")},
        };
        const float colors[2][4] = {{1.7f, 0.3f, 0.05f, 1.0f}, {0.2f, 0.6f, 0.9f, 0.5f}};
        QList<KisRenderedDab> cpuDabs, gpuDabs;
        for (int i = 0; i < 22; ++i) {
            KisFixedPaintDeviceSP device;
            QSharedPointer<const KisProceduralCircleDab> circle;
            QVERIFY(generateCircleDab(shapes[i % shapes.size()], colors[i % 2], &device, &circle, halfDabs));
            // The reference repeats the AVX2+FMA kernel (and, for RGBA16F,
            // Imath's rounding) exactly.
            const QRect rect = device->bounds();
            for (int y = 0; y < rect.height(); ++y) {
                for (int x = 0; x < rect.width(); ++x) {
                    QVERIFY2(circle->matchesPixel(device->constData(), rect.width(), x, y, x, y),
                             qPrintable(QStringLiteral("shape %1 pixel %2,%3: reference alpha %4")
                                            .arg(i % shapes.size())
                                            .arg(x)
                                            .arg(y)
                                            .arg(double(circle->alphaAt(x, y)), 0, 'g', 9)));
                }
            }
            KisRenderedDab cpu(device);
            cpu.offset = QPoint(-60 + 11 * i, -40 + 7 * i);
            cpu.opacity = i % 3 == 0 ? 1.0 : 0.41;
            cpu.flow = i % 2 == 0 ? 1.0 : 0.47;
            cpu.averageOpacity = i % 2 == 0 ? 0.85 : cpu.opacity;
            KisRenderedDab gpu = cpu;
            gpu.device = new KisFixedPaintDevice(*device);
            std::fill_n(gpu.device->data(), rect.width() * rect.height() * pixelSize, quint8(0));
            gpu.procedural = circle;
            cpuDabs << cpu;
            gpuDabs << gpu;
        }
        const QRect original = boundsOf(cpuDabs);
        const QRect bounds(-420, -420, 840, 840);
        KisPaintDeviceSP cpu = new KisPaintDevice(space(halfDabs));
        cpu->moveTo(-13, 7);
        const float background[] = {0.8f, 0.2f, 0.3f, 0.6f};
        const half halfBackground[] = {half(0.8f), half(0.2f), half(0.3f), half(0.6f)};
        cpu->fill(bounds.x(),
                  bounds.y(),
                  bounds.width(),
                  bounds.height(),
                  halfDabs ? reinterpret_cast<const quint8 *>(halfBackground)
                           : reinterpret_cast<const quint8 *>(background));
        KisPaintDeviceSP gpu = new KisPaintDevice(*cpu);
        const auto before = pixels(gpu, bounds);
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        cpuPainter.setCompositeOpId(mode);
        gpuPainter.setCompositeOpId(mode);
        QVector<Qt::Orientation> directions;
        if (mirrors) {
            cpuPainter.setMirrorInformation(QPointF(7, -4), true, true);
            gpuPainter.setMirrorInformation(QPointF(7, -4), true, true);
            directions << Qt::Horizontal << Qt::Vertical << Qt::Horizontal; // original, H, HV, V
        }
        const auto generatedBefore = KisGpuBrushPainter::generatedDabCount();
        KisTransaction transaction(gpu);
        QRect rect = original;
        cpuPainter.bltFixed(rect, cpuDabs);
        for (const auto direction : directions) {
            for (auto &dab : cpuDabs)
                cpuPainter.mirrorDab(direction, &dab, false);
            cpuPainter.mirrorRect(direction, &rect);
            cpuPainter.bltFixed(rect, cpuDabs);
        }
        if (mirrors == 2) {
            QVERIFY(KisGpuBrushPainter::paintMirrored(&gpuPainter, gpuDabs, {original}));
        } else {
            rect = original;
            const QVector<QRect> originalRects{rect};
            QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, gpuDabs, mirrors ? &originalRects : nullptr));
            for (const auto direction : directions) {
                // As the brush op's CPU mirroring jobs: reflect the pixels
                // and record the reflection in the description.
                for (auto &dab : gpuDabs) {
                    gpuPainter.mirrorDab(direction, &dab, false);
                    dab.proceduralFlips ^= direction == Qt::Horizontal ? 1u : 2u;
                }
                gpuPainter.mirrorRect(direction, &rect);
                const QVector<QRect> passRects{rect};
                QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, gpuDabs, &passRects));
            }
        }
        QCOMPARE(KisGpuBrushPainter::generatedDabCount() - generatedBefore,
                 quint64(gpuDabs.size() * (directions.size() + 1)));
        QScopedPointer<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        const auto after = pixels(gpu, bounds);
        const float error = difference(pixels(cpu, bounds), after);
        // RGBA16F: the established half brush tolerance (per-dab rounding).
        QVERIFY2(error <= (halfDabs ? 1.0f / 1024 : 2e-5f), qPrintable(QString::number(error)));
        command->undo();
        QCOMPARE(pixels(gpu, bounds), before);
        command->redo();
        QCOMPARE(pixels(gpu, bounds), after);
    }
    void testGeneratedCircleDabsExact_data()
    {
        QTest::addColumn<bool>("halfDabs");
        QTest::newRow("rgba32f") << false;
        QTest::newRow("rgba16f") << true;
    }
    void testGeneratedCircleDabsExact()
    {
        // Over at full opacity on a transparent device copies the dab alpha,
        // so the GPU evaluation must reproduce every CPU-generated alpha
        // bit for bit, including the Gaussian thresholds.
        const QVector<CircleDabParameters> shapes = {
            {37.3, 0.6, 0.7, 0.6, true, 1.0, 1.0, 0.37, QPointF(0.25, 0.8)},
            {90.0, 1.0, 0.2, 0.9, false, 0.5, 1.0, 2.1, QPointF(0.5, 0.125)},
            {256.0, 1.0, 1.0, 1.0, true, 1.0, 1.0, 0.61, QPointF(0.37, 0.52), true},
            {48.0, 1.0, 0.3, 0.3, true, 1.0, 1.0, 0.0, QPointF(0.0, 0.0), true},
            {97.5, 0.7, 0.8, 0.2, false, 1.0, 1.0, 1.1, QPointF(0.75, 0.25), true},
            {130.0, 1.0, 0.55, 0.55, true, 1.0, 0.9, -0.4, QPointF(0.1, 0.9), true},
            {300.0, 1.0, 0.05, 0.05, true, 1.0, 1.0, 0.2, QPointF(0.6, 0.3), true},
            {256.0, 1.0, 1.0, 1.0, true, 1.0, 1.0, 0.3, QPointF(0.4, 0.6), false, QStringLiteral("0,1;1,0;")},
            {97.5,
             0.7,
             1.0,
             1.0,
             false,
             0.6,
             1.0,
             1.1,
             QPointF(0.75, 0.25),
             false,
             QStringLiteral("0,1;0.3,0.85;0.7,0.2;1,0;")},
            {40.0,
             1.0,
             1.0,
             1.0,
             true,
             0.35,
             1.2,
             -2.0,
             QPointF(0.05, 0.95),
             false,
             QStringLiteral("0,1;0.5,0.9;1,0;")},
        };
        QFETCH(bool, halfDabs);
        const int pixelSize = halfDabs ? 8 : 16;
        const float color[4] = {0.4f, 0.7f, 1.3f, 1.0f};
        for (int i = 0; i < shapes.size(); ++i) {
            KisFixedPaintDeviceSP device;
            QSharedPointer<const KisProceduralCircleDab> circle;
            QVERIFY(generateCircleDab(shapes[i], color, &device, &circle, halfDabs));
            KisRenderedDab dab(new KisFixedPaintDevice(*device));
            dab.offset = QPoint(-31, 17);
            std::fill_n(dab.device->data(),
                        device->bounds().width() * device->bounds().height() * pixelSize,
                        quint8(0));
            dab.procedural = circle;
            KisPaintDeviceSP target = new KisPaintDevice(space(halfDabs));
            KisPainter painter(target);
            painter.setCompositeOpId(COMPOSITE_OVER);
            QVERIFY(KisGpuBrushPainter::paint(&painter, {dab}));
            const QRect rect = dab.realBounds();
            const auto painted = pixels(target, rect);
            auto expectedAlpha = [&](int p) {
                return halfDabs ? float(reinterpret_cast<const half *>(device->constData())[4 * p + 3])
                                : reinterpret_cast<const float *>(device->constData())[4 * p + 3];
            };
            int mismatches = 0;
            for (int p = 0; p < rect.width() * rect.height(); ++p) {
                if (painted[size_t(4 * p + 3)] != expectedAlpha(p)) {
                    if (!mismatches++) {
                        qWarning() << "shape" << i << "pixel" << p % rect.width() << p / rect.width() << "GPU"
                                   << painted[size_t(4 * p + 3)] << "CPU" << expectedAlpha(p);
                    }
                }
            }
            QVERIFY2(mismatches == 0,
                     qPrintable(QStringLiteral("shape %1: %2 alpha mismatches").arg(i).arg(mismatches)));

            // Phase 4.85: pending pixels rendered from the description equal
            // the CPU generator's pixels, in every reflection.
            for (quint32 flips = 0; flips < 4; ++flips) {
                KisFixedPaintDeviceSP reference = new KisFixedPaintDevice(*device);
                reference->mirror(flips & 1, flips & 2);
                KisRenderedDab pending(new KisFixedPaintDevice(*device));
                std::fill_n(pending.device->data(), rect.width() * rect.height() * pixelSize, quint8(0xcd));
                pending.procedural = circle;
                pending.proceduralFlips = flips;
                pending.pixelsPending = true;
                pending.materialize();
                QVERIFY(!pending.pixelsPending);
                QVERIFY2(std::memcmp(pending.device->constData(),
                                     reference->constData(),
                                     rect.width() * rect.height() * pixelSize)
                             == 0,
                         qPrintable(QStringLiteral("shape %1 flips %2: materialized pixels differ").arg(i).arg(flips)));
            }
        }
    }
    void testGeneratedImageTipDabs()
    {
        // "Image tip dabs": the GPU evaluation of KisProceduralCircleDab::
        // ImageTip reproduces every CPU alpha bit for bit (Over at full
        // opacity on a transparent device copies the dab alpha), for an
        // identity tip, scaled and rotated rows, and positions outside the
        // level (Qt's edge clamping).
        QImage level(37, 29, QImage::Format_ARGB32);
        for (int y = 0; y < level.height(); ++y) {
            for (int x = 0; x < level.width(); ++x) {
                const int value = (x * 37 + y * 91 + x * y * 7) % 256;
                int alpha = (x * 13 + y * 29) % 256;
                if ((x + y) % 11 == 0)
                    alpha = 0;
                if ((x * y) % 7 == 3)
                    alpha = 255;
                level.setPixel(x, y, qRgba(value, (value * 3 + y) % 256, (x * 5) % 256, alpha));
            }
        }
        auto geometry = [&](QSize size, double scale, double angle, int startX, int startY) {
            QSharedPointer<KisImageTipGeometry> tip(new KisImageTipGeometry());
            tip->source = level;
            tip->premultiplied = KisImageTipSampler::premultiply(level);
            tip->size = size;
            tip->fdx = int(std::cos(angle) * scale * 65536);
            tip->fdy = int(std::sin(angle) * scale * 65536);
            for (int y = 0; y < size.height(); ++y) {
                KisImageTipGeometry::Row row;
                row.x = (y * 3) % 7;
                row.length = y % 9 == 5 ? 0 : size.width() - row.x - (y % 4);
                row.fx = startX + int(-std::sin(angle) * scale * 65536) * y + row.x * tip->fdx;
                row.fy = startY + int(std::cos(angle) * scale * 65536) * y + row.x * tip->fdy;
                tip->rows << row;
            }
            return tip;
        };
        QVector<QSharedPointer<KisImageTipGeometry>> tips;
        QSharedPointer<KisImageTipGeometry> identity(new KisImageTipGeometry());
        identity->source = level;
        identity->premultiplied = KisImageTipSampler::premultiply(level);
        identity->size = QSize(level.width() - 2, level.height() - 2);
        identity->identity = true;
        tips << identity;
        tips << geometry(QSize(50, 40), 0.7, 0.0, 12345, 23456); // scale only
        tips << geometry(QSize(30, 25), 1.3, 0.0, -70000, -50000); // starts outside
        tips << geometry(QSize(60, 60), 0.6, 0.83, 400000, -100000); // rotated
        tips << geometry(QSize(45, 50), 0.9, -2.4, 2000000, 1500000); // rotated, mostly outside
        // the applications' tables (KisProceduralCircleDab::ImageApplication):
        // distinct values per channel, alphas in (0, 1], and the gradient
        // map's mask opacities; RGBA16F tables hold exact half values
        auto makeTable = [](bool halfValues) {
            QSharedPointer<QVector<float>> table(new QVector<float>(1024 + 512));
            for (int i = 0; i < 1024; ++i) {
                const float value =
                    i < 768 ? float(std::sin(i * 0.37) * 1.7 + 0.2) : float((i - 767) / 256.0 + 1e-3 * std::sin(i));
                (*table)[i] = halfValues ? float(half(value)) : value;
            }
            for (int a = 0; a < 256; ++a) {
                const double maskOpacity = double(a) / 255.0;
                std::memcpy(table->data() + 1024 + 2 * a, &maskOpacity, sizeof(double));
            }
            return table;
        };
        const QSharedPointer<QVector<float>> tables[2] = {makeTable(false), makeTable(true)};
        const int runsPerDepth = tips.size() * 4;
        for (int run = 0; run < 2 * runsPerDepth; ++run) {
            const bool halfDab = run >= runsPerDepth;
            const int i = (run % runsPerDepth) / 4;
            const quint32 application = quint32(run % 4);
            const int pixelSize = halfDab ? 8 : 16;
            const QSize size = tips[i]->size;
            QSharedPointer<KisProceduralCircleDab> description(new KisProceduralCircleDab());
            description->kind = KisProceduralCircleDab::ImageTip;
            const float color[4] = {0.4f, 0.7f, 1.3f, 1.0f};
            for (int c = 0; c < 4; ++c) {
                description->color[c] = halfDab ? float(half(color[c])) : color[c];
            }
            description->halfPixels = halfDab;
            description->imageTip = tips[i];
            description->imageApplication = application;
            if (application) {
                description->imageTable = tables[halfDab ? 1 : 0];
            }
            KisFixedPaintDeviceSP device = new KisFixedPaintDevice(space(halfDab));
            device->setRect(QRect(QPoint(), size));
            device->lazyGrowBufferWithoutInitialization();
            description->render(device->data(), size.width(), size.height(), 0);
            auto cpuValue = [&](int index) {
                return halfDab ? float(reinterpret_cast<const half *>(device->constData())[index])
                               : reinterpret_cast<const float *>(device->constData())[index];
            };
            for (int p = 0; p < size.width() * size.height(); ++p) {
                QVERIFY(description->matchesPixel(device->constData(),
                                                  size.width(),
                                                  p % size.width(),
                                                  p / size.width(),
                                                  p % size.width(),
                                                  p / size.width()));
            }
            KisRenderedDab dab(new KisFixedPaintDevice(*device));
            dab.offset = QPoint(-31, 17);
            std::fill_n(dab.device->data(), size.width() * size.height() * pixelSize, quint8(0));
            dab.procedural = description;
            KisPaintDeviceSP target = new KisPaintDevice(space(halfDab));
            KisPainter painter(target);
            painter.setCompositeOpId(COMPOSITE_OVER);
            QVERIFY(KisGpuBrushPainter::paint(&painter, {dab}));
            // the raw channels: RGBA16F as halves, without a conversion
            QByteArray raw(size.width() * size.height() * pixelSize, Qt::Uninitialized);
            target->readBytes(reinterpret_cast<quint8 *>(raw.data()), dab.realBounds());
            auto paintedValue = [&](int index) {
                return halfDab ? float(reinterpret_cast<const half *>(raw.constData())[index])
                               : reinterpret_cast<const float *>(raw.constData())[index];
            };
            int mismatches = 0;
            int covered = 0;
            for (int p = 0; p < size.width() * size.height(); ++p) {
                const bool visible = cpuValue(4 * p + 3) > 0.0f;
                covered += visible ? 1 : 0;
                // the color of transparent pixels is not painted
                for (int c = visible ? 0 : 3; c < 4; ++c) {
                    if (paintedValue(4 * p + c) != cpuValue(4 * p + c) && !mismatches++) {
                        qWarning() << "tip" << i << "half" << halfDab << "application" << application << "pixel"
                                   << p % size.width() << p / size.width() << "channel" << c << "GPU"
                                   << paintedValue(4 * p + c) << "CPU" << cpuValue(4 * p + c);
                    }
                }
            }
            QVERIFY2(covered > 0, qPrintable(QStringLiteral("tip %1 is empty").arg(i)));
            QVERIFY2(mismatches == 0,
                     qPrintable(QStringLiteral("tip %1 half %2 application %3: %4 mismatches")
                                    .arg(i)
                                    .arg(halfDab)
                                    .arg(application)
                                    .arg(mismatches)));
        }
    }
    void testPendingDabsFallBackToMaterializedPixels()
    {
        // Phase 4.85: a refused GPU batch of pending dabs is painted on the CPU
        // after materialization, exactly like the generated pixels.
        const CircleDabParameters shape{130.0, 1.0, 0.55, 0.55, true, 1.0, 0.9, -0.4, QPointF(0.1, 0.9), true};
        const float color[4] = {0.9f, 0.2f, 0.4f, 1.0f};
        QList<KisRenderedDab> cpuDabs, pendingDabs;
        for (int i = 0; i < 4; ++i) {
            KisFixedPaintDeviceSP device;
            QSharedPointer<const KisProceduralCircleDab> circle;
            QVERIFY(generateCircleDab(shape, color, &device, &circle));
            KisRenderedDab cpu(device);
            cpu.offset = QPoint(-40 + 23 * i, 10 - 9 * i);
            cpu.opacity = 0.6;
            KisRenderedDab pending = cpu;
            pending.device = new KisFixedPaintDevice(*device);
            std::fill_n(pending.device->data(), device->bounds().width() * device->bounds().height() * 16, quint8(0));
            pending.procedural = circle;
            pending.pixelsPending = true;
            cpuDabs << cpu;
            pendingDabs << pending;
        }
        const QRect bounds = boundsOf(cpuDabs);
        KisPaintDeviceSP cpu = new KisPaintDevice(space());
        KisPaintDeviceSP gpu = new KisPaintDevice(space());
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        cpuPainter.setCompositeOpId(COMPOSITE_OVER);
        gpuPainter.setCompositeOpId(COMPOSITE_OVER);
        cpuPainter.bltFixed(bounds, cpuDabs);
        KisGpuTileBackend::instance()->context().injectSubmitFailuresForTesting(1);
        QVERIFY(!KisGpuBrushPainter::paint(&gpuPainter, pendingDabs));
        QVERIFY(KisRenderedDab::hasPendingPixels(pendingDabs));
        // What KisBrushOp::addDabPaintingJobs() does on a refused batch.
        KisRenderedDab::materialize(pendingDabs);
        QVERIFY(!KisRenderedDab::hasPendingPixels(pendingDabs));
        gpuPainter.bltFixed(bounds, pendingDabs);
        // Transparent RGB is unspecified (CPU SIMD and scalar Over differ);
        // every visible value must be identical.
        const float error = difference(pixels(cpu, bounds), pixels(gpu, bounds));
        QVERIFY2(error == 0.0f, qPrintable(QString::number(error)));
    }
    void testGeneratedCircleDabsUseCpuPixelsForHalf()
    {
        // RGBA16F devices keep the pixel upload path: the description is ignored.
        auto dabs = makeDabs(3, 41, true);
        auto described = dabs;
        QSharedPointer<KisProceduralCircleDab> circle(new KisProceduralCircleDab());
        for (auto &dab : described)
            dab.procedural = circle;
        const QRect bounds = boundsOf(dabs).adjusted(-10, -10, 10, 10);
        KisPaintDeviceSP reference = new KisPaintDevice(space(true));
        KisPaintDeviceSP device = new KisPaintDevice(space(true));
        KisPainter referencePainter(reference), painter(device);
        referencePainter.setCompositeOpId(COMPOSITE_OVER);
        painter.setCompositeOpId(COMPOSITE_OVER);
        const auto generatedBefore = KisGpuBrushPainter::generatedDabCount();
        QVERIFY(KisGpuBrushPainter::paint(&referencePainter, dabs));
        QVERIFY(KisGpuBrushPainter::paint(&painter, described));
        QCOMPARE(KisGpuBrushPainter::generatedDabCount(), generatedBefore);
        QCOMPARE(pixels(device, bounds), pixels(reference, bounds));
    }

private:
    void runMirroringTest(bool combined)
    {
        QFETCH(QString, mode);
        QFETCH(int, mirrors);
        QFETCH(bool, masked);
        QFETCH(bool, fractional);
        QFETCH(bool, locked);
        QFETCH(bool, failPass);
        auto makeSharedDabs = []() {
            QList<KisRenderedDab> result;
            for (auto dab : makeDabs(7, 73)) {
                dab.averageOpacity = 0.85;
                result << dab;
                dab.offset += QPoint(3, 1);
                result << dab; // adjacent dabs share pixels, reflected only once
            }
            return result;
        };
        auto cpuDabs = makeSharedDabs(), gpuDabs = makeSharedDabs();
        const QRect bounds(-220, -220, 440, 440);
        const QRect original = boundsOf(cpuDabs);
        QVector<QRect> rects{QRect(original.topLeft(), QSize(original.width() / 2, original.height())),
                             QRect(original.x() + original.width() / 2,
                                   original.y(),
                                   original.width() - original.width() / 2,
                                   original.height())};
        KisPaintDeviceSP cpu = new KisPaintDevice(space());
        cpu->moveTo(-13, 7);
        const float background[] = {1.2f, -0.1f, 0.6f, 0.4f};
        cpu->fill(bounds.x(),
                  bounds.y(),
                  bounds.width(),
                  bounds.height(),
                  reinterpret_cast<const quint8 *>(background));
        KisPaintDeviceSP gpu = new KisPaintDevice(*cpu), snapshot = new KisPaintDevice(*gpu);
        const auto before = pixels(gpu, bounds);
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        cpuPainter.setCompositeOpId(mode);
        gpuPainter.setCompositeOpId(mode);
        const QPointF axes = fractional ? QPointF(7.5, -4.75) : QPointF(7, -4);
        cpuPainter.setMirrorInformation(axes, mirrors & 1, mirrors & 2);
        gpuPainter.setMirrorInformation(axes, mirrors & 1, mirrors & 2);
        if (locked) {
            QBitArray flags(4, true);
            flags.clearBit(3);
            cpuPainter.setChannelFlags(flags);
            gpuPainter.setChannelFlags(flags);
        }
        if (masked) {
            KisSelectionSP selection = new KisSelection();
            const QRect maskBounds(-97, -80, 140, 131);
            std::vector<quint8> mask(size_t(maskBounds.width()) * maskBounds.height());
            for (size_t i = 0; i < mask.size(); ++i)
                mask[i] = quint8(i * 17);
            selection->pixelSelection()->writeBytes(mask.data(), maskBounds);
            cpuPainter.setSelection(selection);
            gpuPainter.setSelection(selection);
        }
        QVector<Qt::Orientation> directions;
        if (mirrors & 1)
            directions << Qt::Horizontal;
        if (mirrors & 2)
            directions << Qt::Vertical;
        if (mirrors == 3)
            directions << Qt::Horizontal; // original, H, HV, V
        KisTransaction transaction(gpu);
        const auto count = KisGpuBrushPainter::batchCount();
        int completed = 0, fallbacks = 0;
        qint64 cpuNanos = 0, gpuNanos = 0;
        QElapsedTimer timer;
        bool combinedPainted = false;
        if (combined) {
            QList<QByteArray> sourceBytes;
            for (const auto &dab : gpuDabs)
                sourceBytes << QByteArray(reinterpret_cast<const char *>(dab.device->constData()),
                                          dab.device->bounds().width() * dab.device->bounds().height() * 16);
            const auto originalDabs = gpuDabs;
            if (failPass)
                KisGpuTileBackend::instance()->context().injectSubmitFailuresForTesting(1);
            timer.start();
            combinedPainted = KisGpuBrushPainter::paintMirrored(&gpuPainter, gpuDabs, rects);
            gpuNanos += timer.nsecsElapsed();
            for (int i = 0; i < gpuDabs.size(); ++i) {
                QCOMPARE(gpuDabs[i].offset, originalDabs[i].offset);
                QCOMPARE(
                    QByteArray(reinterpret_cast<const char *>(gpuDabs[i].device->constData()), sourceBytes[i].size()),
                    sourceBytes[i]);
            }
            if (!failPass)
                QVERIFY(combinedPainted);
            if (failPass)
                QVERIFY(!combinedPainted);
            if (combinedPainted) {
                completed = 1;
                QCOMPARE(KisGpuBrushPainter::batchCount(), count + 1);
            } else {
                QCOMPARE(KisGpuBrushPainter::batchCount(), count);
                QCOMPARE(pixels(gpu, bounds), before);
            }
        }
        for (int pass = 0; pass <= directions.size(); ++pass) {
            if (pass) {
                const auto direction = directions[pass - 1];
                auto reflect = [direction](KisPainter &painter, QList<KisRenderedDab> &dabs) {
                    KisFixedPaintDeviceSP previous;
                    for (auto &dab : dabs) {
                        painter.mirrorDab(direction, &dab, previous == dab.device);
                        previous = dab.device;
                    }
                };
                reflect(cpuPainter, cpuDabs);
                if (!combinedPainted)
                    reflect(gpuPainter, gpuDabs);
                for (auto &rect : rects)
                    cpuPainter.mirrorRect(direction, &rect);
            }
            timer.start();
            for (const QRect &rect : rects)
                cpuPainter.bltFixed(rect, cpuDabs);
            cpuNanos += timer.nsecsElapsed();
            if (combinedPainted)
                continue;
            const bool inject = failPass && pass == 1;
            if (inject)
                KisGpuTileBackend::instance()->context().injectSubmitFailuresForTesting(1);
            timer.restart();
            const bool painted = KisGpuBrushPainter::paint(&gpuPainter, gpuDabs, &rects);
            gpuNanos += timer.nsecsElapsed();
            if (inject)
                QVERIFY(!painted);
            if (painted) {
                ++completed;
            } else {
                ++fallbacks;
                for (const QRect &rect : rects)
                    gpuPainter.bltFixed(rect, gpuDabs);
            }
            const float error = difference(pixels(cpu, bounds), pixels(gpu, bounds));
            QVERIFY2(error <= 2e-5f, qPrintable(QStringLiteral("pass %1 error %2").arg(pass).arg(error)));
        }
        QCOMPARE(KisGpuBrushPainter::batchCount(), count + completed);
        const float finalError = difference(pixels(cpu, bounds), pixels(gpu, bounds));
        QVERIFY2(finalError <= 2e-5f, qPrintable(QString::number(finalError)));
        QCOMPARE(fallbacks, failPass ? 1 : 0);
        if (mode == COMPOSITE_OVER && mirrors == 3 && !masked && !fractional && !locked && !failPass)
            qInfo() << "14 x 73px dabs, combined" << combined << "four mirror passes, compositing only: CPU"
                    << cpuNanos / 1e6 << "ms, GPU" << gpuNanos / 1e6 << "ms";
        QScopedPointer<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        const auto after = pixels(gpu, bounds);
        QCOMPARE(pixels(snapshot, bounds), before);
        command->undo();
        QCOMPARE(pixels(gpu, bounds), before);
        command->redo();
        QCOMPARE(pixels(gpu, bounds), after);
    }
private Q_SLOTS:
    void testErasePreservesRgb()
    {
        const auto dabs = makeDabs(5, 73);
        const auto bounds = boundsOf(dabs);
        KisPaintDeviceSP cpu = new KisPaintDevice(space());
        std::vector<float> before(size_t(bounds.width()) * bounds.height() * 4);
        for (size_t i = 0; i < before.size(); i += 4) {
            before[i] = 1.7f;
            before[i + 1] = -0.3f;
            before[i + 2] = 0.2f;
            before[i + 3] = (i / 4) % 3 == 0 ? 0.0f : ((i / 4) % 3 == 1 ? 1.0f : 0.4f);
        }
        cpu->writeBytes(reinterpret_cast<const quint8 *>(before.data()), bounds);
        KisPaintDeviceSP gpu = new KisPaintDevice(*cpu);
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        cpuPainter.setCompositeOpId(COMPOSITE_ERASE);
        gpuPainter.setCompositeOpId(COMPOSITE_ERASE);
        cpuPainter.bltFixed(bounds, dabs);
        QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
        const auto after = pixels(gpu, bounds);
        QVERIFY(difference(pixels(cpu, bounds), after, false) <= 2e-5f);
        for (size_t i = 0; i < before.size(); ++i) {
            if (i % 4 != 3)
                QCOMPARE(after[i], before[i]);
            else
                QVERIFY(after[i] <= before[i]);
        }
        // KoCompositeOpErase ignores channel flags, even when all are locked.
        for (int bits = 0; bits < 15; ++bits) {
            QBitArray flags(4);
            for (int c = 0; c < 4; ++c)
                flags.setBit(c, bits & (1 << c));
            gpuPainter.setChannelFlags(flags);
            cpuPainter.setChannelFlags(flags);
            cpuPainter.bltFixed(bounds, dabs);
            QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
            QVERIFY(difference(pixels(cpu, bounds), pixels(gpu, bounds), false) <= 2e-5f);
        }
    }
    void testClipping_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("masked");
        QTest::addColumn<QString>("shape");
        for (const auto &mode : {COMPOSITE_OVER, COMPOSITE_ALPHA_DARKEN, COMPOSITE_ERASE})
            for (bool masked : {false, true})
                for (const auto &shape : {QStringLiteral("holes"), QStringLiteral("empty"), QStringLiteral("outside")})
                    QTest::newRow(qPrintable(mode + shape + QString::number(masked))) << mode << masked << shape;
    }
    void testClipping()
    {
        QFETCH(QString, mode);
        QFETCH(bool, masked);
        QFETCH(QString, shape);
        const auto dabs = makeDabs(8, 61);
        const QRect bounds(-160, -160, 320, 320);
        KisPaintDeviceSP cpu = new KisPaintDevice(space());
        cpu->moveTo(-13, 7);
        const float background[] = {0.8f, 0.2f, 0.1f, 0.6f};
        cpu->fill(bounds.x(),
                  bounds.y(),
                  bounds.width(),
                  bounds.height(),
                  reinterpret_cast<const quint8 *>(background));
        KisPaintDeviceSP gpu = new KisPaintDevice(*cpu);
        KisPaintDeviceSP snapshot = new KisPaintDevice(*gpu);
        const auto before = pixels(gpu, bounds);
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        cpuPainter.setCompositeOpId(mode);
        gpuPainter.setCompositeOpId(mode);
        if (masked) {
            KisSelectionSP selection = new KisSelection();
            const QRect maskBounds(-68, -19, 95, 62);
            std::vector<quint8> mask(size_t(maskBounds.width()) * maskBounds.height());
            for (size_t i = 0; i < mask.size(); ++i)
                mask[i] = quint8(i * 17);
            selection->pixelSelection()->writeBytes(mask.data(), maskBounds);
            cpuPainter.setSelection(selection);
            gpuPainter.setSelection(selection);
        }
        QVector<QRect> rects;
        if (shape == "holes")
            rects = {QRect(-80, -35, 31, 100), QRect(-39, -35, 9, 100), QRect(-18, -35, 45, 100)};
        else if (shape == "outside")
            rects = {QRect(120, 120, 10, 10)};
        for (const auto &rect : rects)
            cpuPainter.bltFixed(rect, dabs);
        const auto count = KisGpuBrushPainter::batchCount();
        if (shape == "holes" && masked) {
            KisGpuTileBackend::instance()->context().injectSubmitFailuresForTesting(1);
            QVERIFY(!KisGpuBrushPainter::paint(&gpuPainter, dabs, &rects));
            QCOMPARE(pixels(gpu, bounds), before);
            QCOMPARE(KisGpuBrushPainter::batchCount(), count);
        }
        KisTransaction transaction(gpu);
        QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs, &rects));
        QCOMPARE(KisGpuBrushPainter::batchCount(), count + (shape == "holes" ? 1 : 0));
        QScopedPointer<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        const auto after = pixels(gpu, bounds);
        QVERIFY(difference(pixels(cpu, bounds), after, false) <= 2e-5f);
        QCOMPARE(pixels(snapshot, bounds), before);
        if (shape != "holes")
            QCOMPARE(after, before);
        command->undo();
        QCOMPARE(pixels(gpu, bounds), before);
        command->redo();
        QCOMPARE(pixels(gpu, bounds), after);
        // Overlap would apply a dab twice in the CPU rectangle loop. Refuse
        // before any write instead of silently replacing it by a region union.
        const QVector<QRect> overlap{QRect(-80, -35, 60, 100), QRect(-80, -35, 60, 100)};
        QVERIFY(!KisGpuBrushPainter::paint(&gpuPainter, dabs, &overlap));
        QCOMPARE(pixels(gpu, bounds), after);
    }
    void testSparseMirroring_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("masked");
        for (const auto &mode : {COMPOSITE_OVER, COMPOSITE_ALPHA_DARKEN, COMPOSITE_ERASE})
            for (bool masked : {false, true})
                QTest::newRow(qPrintable(mode + QString::number(masked))) << mode << masked;
    }
    void testSparseMirroring()
    {
        QFETCH(QString, mode);
        QFETCH(bool, masked);
        auto cpuDabs = makeDabs(4, 73);
        const auto gpuDabs = makeDabs(4, 73);
        KisPaintDeviceSP cpu = new KisPaintDevice(space());
        cpu->moveTo(-13, 7);
        KisPainter cpuPainter(cpu);
        cpuPainter.setCompositeOpId(mode);
        // The unmasked case spans hundreds of thousands of empty tiles.
        const QPointF axis = masked ? QPointF(300.5, 300.25) : QPointF(10000.5, 10000.25);
        cpuPainter.setMirrorInformation(axis, true, true);
        QVector<QRect> patches{boundsOf(cpuDabs)};
        for (auto direction : {Qt::Horizontal, Qt::Vertical, Qt::Horizontal}) {
            auto rect = patches.last();
            cpuPainter.mirrorRect(direction, &rect);
            patches << rect;
        }
        const float background[] = {0.8f, 0.2f, 0.1f, 0.6f};
        KisSelectionSP selection = new KisSelection();
        QRegion expectedTiles;
        for (const auto &rect : patches) {
            cpu->fill(rect.x(), rect.y(), rect.width(), rect.height(), reinterpret_cast<const quint8 *>(background));
            selection->pixelSelection()->select(rect.adjusted(3, 5, -7, -9), 173);
            expectedTiles += KisGpuMergeBatch::tileAligned(rect.translated(13, -7)).translated(-13, 7);
        }
        KisPaintDeviceSP gpu = new KisPaintDevice(*cpu), snapshot = new KisPaintDevice(*cpu);
        KisPainter gpuPainter(gpu);
        gpuPainter.setCompositeOpId(mode);
        gpuPainter.setMirrorInformation(axis, true, true);
        if (masked) {
            cpuPainter.setSelection(selection);
            gpuPainter.setSelection(selection);
        }
        const QVector<QRect> rects{patches.first()};
        const auto count = KisGpuBrushPainter::batchCount();
        KisGpuTileBackend::instance()->context().injectSubmitFailuresForTesting(1);
        QVERIFY(!KisGpuBrushPainter::paintMirrored(&gpuPainter, gpuDabs, rects));
        QCOMPARE(KisGpuBrushPainter::batchCount(), count);
        for (const auto &rect : patches)
            QCOMPARE(pixels(gpu, rect), pixels(snapshot, rect));
        KisTransaction transaction(gpu);
        QVERIFY(KisGpuBrushPainter::paintMirrored(&gpuPainter, gpuDabs, rects));
        QCOMPARE(KisGpuBrushPainter::batchCount(), count + 1);
        for (const auto &rect : gpu->region().rects())
            QVERIFY((QRegion(rect) - expectedTiles).isEmpty()); // no allocation in the gaps
        for (int pass = 0; pass < 4; ++pass) {
            if (pass)
                for (auto &dab : cpuDabs)
                    cpuPainter.mirrorDab(pass == 2 ? Qt::Vertical : Qt::Horizontal, &dab);
            cpuPainter.bltFixed(patches[pass], cpuDabs);
            QVERIFY(difference(pixels(cpu, patches[pass]), pixels(gpu, patches[pass])) <= 2e-5f);
        }
        QScopedPointer<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        command->undo();
        for (const auto &rect : patches)
            QCOMPARE(pixels(gpu, rect), pixels(snapshot, rect));
        command->redo();
        for (const auto &rect : patches)
            QVERIFY(difference(pixels(cpu, rect), pixels(gpu, rect)) <= 2e-5f);
    }
    void testCombinedMirroringRefusal()
    {
        const auto dabs = makeDabs(4, 73);
        const auto bounds = boundsOf(dabs);
        const QVector<QRect> rects{bounds};
        KisPaintDeviceSP device = new KisPaintDevice(space());
        KisPainter painter(device);
        painter.setCompositeOpId(COMPOSITE_OVER);
        painter.setMirrorInformation(QPointF(1000, 1000), true, true);
        const auto before = pixels(device, bounds);
        const auto count = KisGpuBrushPainter::batchCount();
        auto *backend = KisGpuTileBackend::instance();
        backend->flush();
        backend->evictTiles(~quint64(0));
        const auto budget = backend->memoryBudget();
        auto restore = qScopeGuard([&]() {
            backend->setMemoryBudgetForTesting(budget);
        });
        // Enough for the first sparse region, but not all four; roll back
        // earlier prepared regions when a later one cannot allocate.
        backend->setMemoryBudgetForTesting(16 * backend->pool(16)->tileBytes());
        QVERIFY(!KisGpuBrushPainter::paintMirrored(&painter, dabs, rects));
        QCOMPARE(KisGpuBrushPainter::batchCount(), count);
        QCOMPARE(pixels(device, bounds), before);
        QVERIFY(!backend->hasFailed());
        backend->setMemoryBudgetForTesting(budget);
        QVERIFY(KisGpuBrushPainter::paint(&painter, dabs, &rects));
    }
    void benchmarkCombinedMirrors_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<int>("size");
        QTest::addColumn<bool>("distant");
        QTest::newRow("small") << 14 << 73 << false;
        QTest::newRow("large") << 32 << 256 << false;
        QTest::newRow("distant-small") << 14 << 73 << true;
        QTest::newRow("distant-large") << 32 << 256 << true;
    }
    void benchmarkCombinedMirrors()
    {
        QFETCH(int, count);
        QFETCH(int, size);
        QFETCH(bool, distant);
        const QRect bounds = distant ? QRect(-200, -100, 1400, 1300) : QRect(-600, -600, 1200, 1200);
        std::vector<float> reference;
        double timings[3]{};
        for (int path = 0; path < 3; ++path) { // CPU, separate GPU, combined GPU
            auto dabs = makeDabs(count, size);
            const QVector<QRect> originalRects{boundsOf(dabs)};
            KisPaintDeviceSP device = new KisPaintDevice(space());
            KisPainter painter(device);
            painter.setCompositeOpId(COMPOSITE_OVER);
            painter.setMirrorInformation(distant ? QPointF(500, 500) : QPointF(7, -4), true, true);
            auto reflect = [&](Qt::Orientation direction, QVector<QRect> &rects) {
                for (auto &dab : dabs)
                    painter.mirrorDab(direction, &dab);
                for (auto &rect : rects)
                    painter.mirrorRect(direction, &rect);
            };
            for (int iteration = 0; iteration < 8; ++iteration) {
                auto rects = originalRects;
                QElapsedTimer timer;
                timer.start();
                if (path == 2) {
                    QVERIFY(KisGpuBrushPainter::paintMirrored(&painter, dabs, rects));
                } else {
                    for (int pass = 0; pass < 4; ++pass) {
                        if (pass)
                            reflect(pass == 2 ? Qt::Vertical : Qt::Horizontal, rects);
                        if (path == 1) {
                            QVERIFY(KisGpuBrushPainter::paint(&painter, dabs, &rects));
                        } else {
                            for (const auto &rect : rects)
                                painter.bltFixed(rect, dabs);
                        }
                    }
                }
                if (path)
                    KisGpuTileBackend::instance()->context().waitIdle();
                if (iteration >= 3) // warm every staging-ring slot before timing
                    timings[path] += timer.nsecsElapsed() / 5e6;
                // Restore source storage for the next independent update. This
                // is fixture maintenance, outside timing; combined leaves it intact.
                if (path != 2)
                    reflect(Qt::Vertical, rects);
            }
            const auto output = pixels(device, bounds);
            if (!path)
                reference = output;
            else
                QVERIFY(difference(reference, output) <= 2e-5f);
        }
        qInfo() << count << "x" << size << "px dabs, warmed four-pass update: CPU" << timings[0] << "ms, separate GPU"
                << timings[1] << "ms, combined GPU" << timings[2] << "ms";
    }
    void testChannelLocks_data()
    {
        QTest::addColumn<int>("channels");
        QTest::addColumn<bool>("masked");
        QTest::addColumn<QString>("mode");
        for (const auto &mode : channelModes)
            for (int channels = 0; channels < 16; ++channels) {
                for (bool masked : {false, true}) {
                    const auto name = QStringLiteral("%1-channels%2-selection%3").arg(mode).arg(channels).arg(masked);
                    QTest::newRow(qPrintable(name)) << channels << masked << mode;
                }
            }
    }
    void testChannelLocks()
    {
        QFETCH(int, channels);
        QFETCH(bool, masked);
        QFETCH(QString, mode);
        const auto dabs = makeDabs(9, 73);
        const auto bounds = boundsOf(dabs).adjusted(-11, -13, 9, 7);
        std::vector<float> before(size_t(bounds.width()) * bounds.height() * 4);
        for (size_t i = 0; i < before.size(); i += 4) {
            before[i] = 1.4f;
            before[i + 1] = -0.2f;
            before[i + 2] = 0.8f;
            before[i + 3] = i % 12 == 0 ? 0 : (i % 12 == 4 ? 1 : 0.3f);
        }
        KisPaintDeviceSP cpu = new KisPaintDevice(space());
        cpu->moveTo(-13, 7);
        cpu->writeBytes(reinterpret_cast<const quint8 *>(before.data()), bounds);
        KisPaintDeviceSP gpu = new KisPaintDevice(*cpu);
        KisPaintDeviceSP snapshot = new KisPaintDevice(*gpu);
        QBitArray flags(4);
        for (int i = 0; i < 4; ++i)
            flags.setBit(i, channels & (1 << i));
        KisSelectionSP selection = new KisSelection();
        std::vector<quint8> coverage(size_t(bounds.width()) * bounds.height());
        for (size_t i = 0; i < coverage.size(); ++i)
            coverage[i] = quint8(i * 17);
        selection->pixelSelection()->writeBytes(coverage.data(), bounds);
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        cpuPainter.setCompositeOpId(mode);
        gpuPainter.setCompositeOpId(mode);
        cpuPainter.setChannelFlags(flags);
        gpuPainter.setChannelFlags(flags);
        if (masked) {
            cpuPainter.setSelection(selection);
            gpuPainter.setSelection(selection);
        }
        cpuPainter.bltFixed(bounds, dabs);
        KisTransaction transaction(gpu);
        QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
        QScopedPointer<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        const auto after = pixels(gpu, bounds);
        // Restricted channels use the CPU scalar kernel, whose hidden RGB
        // behavior is defined and must also match on transparent pixels.
        const float error = difference(pixels(cpu, bounds), after, mode == COMPOSITE_OVER && channels == 15);
        QVERIFY2(error <= 2e-5f, qPrintable(QString::number(error)));
        QCOMPARE(pixels(snapshot, bounds), before);
        for (size_t i = 0; i < before.size(); ++i) {
            const size_t alpha = i / 4 * 4 + 3;
            if (mode == COMPOSITE_ERASE
                    ? i % 4 != 3
                    : (!(channels & (1 << (i % 4)))
                       && (i % 4 == 3 || before[alpha] != 0 || (mode == COMPOSITE_OVER && !(channels & 8)))))
                QCOMPARE(after[i], before[i]);
        }
        if (!channels && mode == COMPOSITE_OVER)
            QCOMPARE(after, before);
        command->undo();
        QCOMPARE(pixels(gpu, bounds), before);
        command->redo();
        QCOMPARE(pixels(gpu, bounds), after);
        if (channels == 7 && masked) {
            KisGpuTileBackend::instance()->context().injectSubmitFailuresForTesting(1);
            QVERIFY(!KisGpuBrushPainter::paint(&gpuPainter, dabs));
            QCOMPARE(pixels(gpu, bounds), after);
        }
        cpuPainter.bltFixed(bounds, dabs);
        gpuPainter.bltFixed(bounds, dabs);
        QVERIFY(difference(pixels(cpu, bounds), pixels(gpu, bounds), mode == COMPOSITE_OVER && channels == 15)
                <= 2e-5f);
    }
    void testSelectionCoverageRounding_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {COMPOSITE_LINEAR_BURN, COMPOSITE_LINEAR_LIGHT, COMPOSITE_PIN_LIGHT})
            QTest::newRow(qPrintable(mode)) << mode;
    }
    void testSelectionCoverageRounding()
    {
        QFETCH(QString, mode);
        auto dabs = makeDabs(1, 16);
        auto *source = reinterpret_cast<float *>(dabs.first().device->data());
        for (int i = 0; i < 256 * 4; ++i)
            source[i] = 1.0f;
        const QRect bounds = boundsOf(dabs);
        QVector<quint8> coverage(256);
        for (int i = 0; i < 256; ++i)
            coverage[i] = quint8(i);
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->writeBytes(coverage.constData(), bounds);
        KisPaintDeviceSP cpu = new KisPaintDevice(space()), gpu = new KisPaintDevice(space());
        KisPainter cpuPainter(cpu, selection), gpuPainter(gpu, selection);
        cpuPainter.setCompositeOpId(mode);
        gpuPainter.setCompositeOpId(mode);
        cpuPainter.bltFixed(bounds, dabs);
        QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
        const auto expected = pixels(cpu, bounds);
        auto actual = pixels(gpu, bounds);
        for (int i = 0; i < 256; ++i)
            QVERIFY2(actual[4 * i + 3] == expected[4 * i + 3], qPrintable(QString::number(i)));

        // Wash's layer shader uses the same byte conversion; require exact alpha
        // there too, not the looser whole-image color tolerance.
        KisPaintDeviceSP src = new KisPaintDevice(space()), merged = new KisPaintDevice(space());
        src->writeBytes(reinterpret_cast<const quint8 *>(source), bounds);
        const KisGpuLayerCompositor::Mask mask{coverage.constData(), bounds};
        KisGpuBlendOp op;
        QVERIFY(KisGpuProjectionCompositor::blendOpForCompositeOp(mode, &op));
        QVERIFY(KisGpuProjectionCompositor::composite(merged, bounds, {{src, op}}, nullptr, &mask));
        actual = pixels(merged, bounds);
        for (int i = 0; i < 256; ++i)
            QVERIFY2(actual[4 * i + 3] == expected[4 * i + 3], qPrintable(QString::number(i)));
    }
    void testSelection_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("shape");
        for (const auto &mode : dabModes) {
            for (const auto &shape : {QStringLiteral("soft"),
                                      QStringLiteral("inverted"),
                                      QStringLiteral("empty"),
                                      QStringLiteral("outside")}) {
                QTest::newRow(qPrintable(mode + '-' + shape)) << mode << shape;
            }
        }
    }
    void testSelection()
    {
        QFETCH(QString, mode);
        QFETCH(QString, shape);
        auto dabs = makeDabs(12, 89);
        for (auto &dab : dabs)
            dab.averageOpacity = 0.83;
        const auto bounds = boundsOf(dabs).adjusted(-70, -70, 70, 70);
        KisPaintDeviceSP cpu = new KisPaintDevice(space());
        cpu->moveTo(-13, 7);
        const float background[] = {1.3f, -0.2f, 0.7f, 0.6f};
        cpu->fill(bounds.x(),
                  bounds.y(),
                  bounds.width(),
                  bounds.height(),
                  reinterpret_cast<const quint8 *>(background));
        KisPaintDeviceSP gpu = new KisPaintDevice(*cpu);
        const auto before = pixels(gpu, bounds);
        KisSelectionSP selection = new KisSelection();
        selection->setDefaultBounds(new KisSelectionDefaultBounds(cpu));
        const QRect maskBounds(-64, -12, 67, 71);
        std::vector<quint8> mask(size_t(maskBounds.width()) * maskBounds.height());
        for (size_t i = 0; i < mask.size(); ++i)
            mask[i] = quint8(i * 17); // all 256 coverages, including 0, 1 and 255
        if (shape == "outside") {
            selection->pixelSelection()->select(QRect(5000, 5000, 9, 9));
        } else if (shape != "empty") {
            selection->pixelSelection()->writeBytes(mask.data(), maskBounds);
            selection->pixelSelection()->moveTo(9, -5);
            if (shape == "inverted")
                selection->pixelSelection()->invert();
        }
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        cpuPainter.setCompositeOpId(mode);
        gpuPainter.setCompositeOpId(mode);
        cpuPainter.setSelection(selection);
        gpuPainter.setSelection(selection);
        const auto count = KisGpuBrushPainter::batchCount();
        cpuPainter.bltFixed(bounds, dabs);
        KisTransaction transaction(gpu);
        QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
        QScopedPointer<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        const auto after = pixels(gpu, bounds);
        const float error = difference(pixels(cpu, bounds), after);
        QVERIFY2(error <= 2e-5f, qPrintable(QString::number(error)));
        if (shape == "empty" || shape == "outside") {
            QCOMPARE(after, before);
            QCOMPARE(KisGpuBrushPainter::batchCount(), count);
        } else {
            QCOMPARE(KisGpuBrushPainter::batchCount(), count + 1);
            QVERIFY(after != before);
            // A zero selection pixel must preserve visible pixels exactly.
            std::vector<quint8> coverage(size_t(bounds.width()) * bounds.height());
            selection->projection()->readBytes(coverage.data(), bounds);
            for (size_t i = 0; i < coverage.size(); ++i) {
                if (coverage[i] == 0) {
                    for (int c = 0; c < 4; ++c)
                        QCOMPARE(after[i * 4 + c], before[i * 4 + c]);
                }
            }
        }
        command->undo();
        QCOMPARE(pixels(gpu, bounds), before);
        command->redo();
        QCOMPARE(pixels(gpu, bounds), after);
        if (shape == "soft") {
            KisGpuTileBackend::instance()->context().injectSubmitFailuresForTesting(1);
            QVERIFY(!KisGpuBrushPainter::paint(&gpuPainter, dabs));
            QCOMPARE(pixels(gpu, bounds), after);
            gpuPainter.bltFixed(bounds, dabs); // original selected CPU fallback
            cpuPainter.bltFixed(bounds, dabs);
            const float fallbackError = difference(pixels(cpu, bounds), pixels(gpu, bounds));
            QVERIFY2(fallbackError <= 2e-5f, qPrintable(QString::number(fallbackError)));
        }
        // Reusing the uploader after deselection must not reuse its old mask.
        cpuPainter.setSelection(nullptr);
        gpuPainter.setSelection(nullptr);
        cpuPainter.bltFixed(bounds, dabs);
        QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
        const float deselectedError = difference(pixels(cpu, bounds), pixels(gpu, bounds));
        QVERIFY2(deselectedError <= 2e-5f, qPrintable(QString::number(deselectedError)));
    }
    void testAlphaDarkenVariants_data()
    {
        QTest::addColumn<bool>("half");
        QTest::addColumn<bool>("creamy");
        QTest::addColumn<float>("flow");
        QTest::addColumn<float>("opacity");
        QTest::addColumn<float>("average");
        QTest::addColumn<bool>("masked");
        const float pairs[][2] = {{0, 0}, {0.37f, 0.37f}, {0.37f, 0.9f}, {1, 0.2f}};
        for (bool half : {false, true})
            for (bool creamy : {false, true}) {
                for (float flow : {0.0f, 0.43f, 0.9999f, 1.0f}) {
                    for (const auto &pair : pairs) {
                        const auto name = QStringLiteral("%1-flow%2-opacity%3-average%4")
                                              .arg(creamy ? "creamy" : "hard")
                                              .arg(flow)
                                              .arg(pair[0])
                                              .arg(pair[1])
                            + (half ? "-f16" : "-f32");
                        QTest::newRow(qPrintable(name)) << half << creamy << flow << pair[0] << pair[1] << false;
                        QTest::newRow(qPrintable(name + "-masked"))
                            << half << creamy << flow << pair[0] << pair[1] << true;
                    }
                }
            }
    }
    void testAlphaDarkenVariants()
    {
        QFETCH(bool, half);
        QFETCH(bool, creamy);
        QFETCH(float, flow);
        QFETCH(float, opacity);
        QFETCH(float, average);
        QFETCH(bool, masked);
        const auto *cs = space(half);
        if (!cs)
            QSKIP("The requested floating-point color space is unavailable");
        const auto dabs = makeDabs(4, 73, half);
        const auto bounds = boundsOf(dabs).adjusted(-5, -3, 8, 10); // odd mask byte count, padded uint read
        std::vector<quint8> coverage(size_t(bounds.width()) * bounds.height());
        for (size_t i = 0; i < coverage.size(); ++i)
            coverage[i] = quint8(i * 17);
        KisGpuDabCompositor::Mask mask{coverage.data(), bounds};
        std::vector<float> expected(size_t(bounds.width()) * bounds.height() * 4);
        for (size_t i = 0; i < expected.size(); i += 4) {
            expected[i] = 1.2f;
            expected[i + 1] = -0.2f;
            expected[i + 2] = 0.7f;
            expected[i + 3] = i % 12 == 0 ? 0 : (i % 12 == 4 ? 1 : 0.2f);
        }
        QByteArray raw(bounds.width() * bounds.height() * cs->pixelSize(), Qt::Uninitialized);
        space()->convertPixelsTo(reinterpret_cast<const quint8 *>(expected.data()),
                                 reinterpret_cast<quint8 *>(raw.data()),
                                 cs,
                                 bounds.width() * bounds.height(),
                                 KoColorConversionTransformation::internalRenderingIntent(),
                                 KoColorConversionTransformation::internalConversionFlags());
        KisPaintDeviceSP gpu = new KisPaintDevice(cs), cpu = new KisPaintDevice(cs);
        gpu->moveTo(-13, 7);
        gpu->writeBytes(reinterpret_cast<const quint8 *>(raw.constData()), bounds);
        // Construct both actual CPU implementations without changing global settings.
        QScopedPointer<KoCompositeOp> reference(
            creamy ? KoOptimizedCompositeOpFactory::createAlphaDarkenOpCreamy128(space())
                   : KoOptimizedCompositeOpFactory::createAlphaDarkenOpHard128(space()));
        if (half) {
#ifdef HAVE_OPENEXR
            if (creamy)
                reference.reset(new KoCompositeOpAlphaDarken<KoRgbF16Traits, KoAlphaDarkenParamsWrapperCreamy>(cs));
            else
                reference.reset(new KoCompositeOpAlphaDarken<KoRgbF16Traits, KoAlphaDarkenParamsWrapperHard>(cs));
#else
            QSKIP("Scalar half reference requires OpenEXR");
#endif
        }
        QVector<KisGpuDabCompositor::Dab> inputs;
        for (const auto &dab : dabs) {
            const QPoint p = dab.offset - bounds.topLeft();
            KoCompositeOp::ParameterInfo parameters;
            parameters.setOpacityAndAverage(opacity, average);
            parameters.flow = flow;
            parameters.dstRowStart =
                reinterpret_cast<quint8 *>(raw.data()) + (p.y() * bounds.width() + p.x()) * cs->pixelSize();
            parameters.dstRowStride = bounds.width() * cs->pixelSize();
            parameters.srcRowStart = dab.device->constData();
            parameters.srcRowStride = dab.device->bounds().width() * cs->pixelSize();
            parameters.rows = dab.device->bounds().height();
            parameters.cols = dab.device->bounds().width();
            if (masked) {
                parameters.maskRowStart = coverage.data() + p.y() * bounds.width() + p.x();
                parameters.maskRowStride = bounds.width();
            }
            reference->composite(parameters);
            inputs << KisGpuDabCompositor::Dab{reinterpret_cast<const float *>(dab.device->constData()),
                                               dab.offset,
                                               dab.device->bounds().size(),
                                               opacity,
                                               flow,
                                               average};
        }
        auto &context = KisGpuTileBackend::instance()->context();
        auto compositor = KisGpuDabCompositor::create(context);
        QVERIFY(compositor);
        KisGpuCommandList commands(context);
        QVERIFY(commands.isValid());
        commands.begin();
        KisGpuTileAccess access(gpu, bounds, KisGpuTileAccess::ReadWrite);
        QVERIFY(access.prepare(commands));
        const auto grid = access.tileGrid();
        using Mode = KisGpuDabCompositor::CompositeMode;
        QVERIFY(compositor->record(commands,
                                   access.addresses(),
                                   grid.width(),
                                   access.tileOrigin(grid.left(), grid.top()),
                                   inputs,
                                   creamy ? Mode::AlphaDarkenCreamy : Mode::AlphaDarkenHard,
                                   masked ? &mask : nullptr,
                                   0xf,
                                   nullptr,
                                   {},
                                   cs->pixelSize()));
        QVERIFY(KisGpuTileAccess::submitAndFinish(commands, {&access}));
        QVERIFY(commands.wait());
        cpu->writeBytes(reinterpret_cast<const quint8 *>(raw.constData()), bounds);
        const float error = difference(pixels(cpu, bounds), pixels(gpu, bounds), !half);
        QVERIFY2(error <= (half ? 1.0f / 1024 : 2e-5f), qPrintable(QString::number(error)));
    }
    void testIndirectMerge_data()
    {
        QTest::addColumn<bool>("locked");
        QTest::addColumn<bool>("limitedReadback");
        QTest::addColumn<QString>("previewPath");
        QTest::addColumn<bool>("erase");
        for (bool erase : {false, true}) {
            const QString prefix = erase ? "erase-" : "normal-";
            QTest::newRow(qPrintable(prefix + "plain")) << false << false << QStringLiteral("gpu") << erase;
            QTest::newRow(qPrintable(prefix + "selection-and-alpha-lock"))
                << true << false << QStringLiteral("cpu") << erase;
            QTest::newRow(qPrintable(prefix + "selection-only"))
                << false << false << QStringLiteral("selection") << erase;
            for (const auto &path : {"selection-inverted",
                                     "selection-translated",
                                     "selection-empty",
                                     "selection-outside",
                                     "selection-failure",
                                     "selection-merge-failure"})
                QTest::newRow(qPrintable(prefix + path)) << false << false << QString::fromLatin1(path) << erase;
            QTest::newRow(qPrintable(prefix + "channel-lock-only"))
                << false << false << QStringLiteral("channels") << erase;
            QTest::newRow(qPrintable(prefix + "limited-readback")) << false << true << QStringLiteral("gpu") << erase;
            QTest::newRow(qPrintable(prefix + "limited-readback-selection-and-alpha-lock"))
                << true << true << QStringLiteral("cpu") << erase;
            QTest::newRow(qPrintable(prefix + "preview-submit-failure"))
                << false << false << QStringLiteral("failure") << erase;
            QTest::newRow(qPrintable(prefix + "preview-unaligned-source"))
                << false << false << QStringLiteral("offset") << erase;
            QTest::newRow(qPrintable(prefix + "preview-integer-owning-image"))
                << false << false << QStringLiteral("integer") << erase;
            QTest::newRow(qPrintable(prefix + "merge-submit-failure"))
                << false << false << QStringLiteral("merge-failure") << erase;
            QTest::newRow(qPrintable(prefix + "merge-low-budget"))
                << false << false << QStringLiteral("merge-budget") << erase;
        }
    }
    void testIndirectMerge()
    {
        runIndirectMerge();
    }
    void testHalfIndirectMerge_data()
    {
        testIndirectMerge_data();
    }
    void testHalfIndirectMerge()
    {
        runIndirectMerge(-1, QString(), true);
    }
    void testWashChannelLocks_data()
    {
        QTest::addColumn<bool>("locked");
        QTest::addColumn<bool>("limitedReadback");
        QTest::addColumn<QString>("previewPath");
        QTest::addColumn<bool>("erase");
        QTest::addColumn<int>("channels");
        for (bool erase : {false, true}) {
            for (int channels = 0; channels < 16; ++channels) {
                for (bool selected : {false, true}) {
                    const auto name = QString("erase%1-channels%2-selected%3").arg(erase).arg(channels).arg(selected);
                    QTest::newRow(qPrintable(name))
                        << false << false << (selected ? QStringLiteral("selection") : QStringLiteral("gpu")) << erase
                        << channels;
                }
                if (channels == 7 || channels == 5) {
                    const auto name = QString("erase%1-channels%2-failure").arg(erase).arg(channels);
                    QTest::newRow(qPrintable(name))
                        << false << false << QStringLiteral("selection-merge-failure") << erase << channels;
                }
            }
        }
    }
    void testWashChannelLocks()
    {
        QFETCH(int, channels);
        runIndirectMerge(channels);
    }
    void testHalfWashChannelLocks_data()
    {
        testWashChannelLocks_data();
    }
    void testHalfWashChannelLocks()
    {
        QFETCH(int, channels);
        runIndirectMerge(channels, QString(), true);
    }
    void testWashBlendModes_data()
    {
        QTest::addColumn<bool>("locked");
        QTest::addColumn<bool>("limitedReadback");
        QTest::addColumn<QString>("previewPath");
        QTest::addColumn<bool>("erase");
        QTest::addColumn<int>("channels");
        QTest::addColumn<QString>("mode");
        for (const auto &mode : blendModes) {
            for (int channels = 0; channels < 16; ++channels)
                for (bool selected : {false, true})
                    QTest::newRow(qPrintable(QString("%1-channels%2-selected%3").arg(mode).arg(channels).arg(selected)))
                        << false << false << (selected ? QStringLiteral("selection") : QStringLiteral("gpu")) << false
                        << channels << mode;
            for (const auto &path : {"selection-inverted",
                                     "selection-translated",
                                     "selection-empty",
                                     "selection-outside",
                                     "selection-failure",
                                     "selection-merge-failure",
                                     "merge-budget",
                                     "offset",
                                     "integer"})
                QTest::newRow(qPrintable(mode + '-' + path))
                    << false << false << QString::fromLatin1(path) << false << 5 << mode;
        }
    }
    void testWashBlendModes()
    {
        QFETCH(int, channels);
        QFETCH(QString, mode);
        runIndirectMerge(channels, mode);
    }
    void testHalfWashBlendModes_data()
    {
        QTest::addColumn<bool>("locked");
        QTest::addColumn<bool>("limitedReadback");
        QTest::addColumn<QString>("previewPath");
        QTest::addColumn<bool>("erase");
        QTest::addColumn<int>("channels");
        QTest::addColumn<QString>("mode");
        for (const auto &mode : halfBlendModes) {
            for (int channels = 0; channels < 16; ++channels)
                for (bool selected : {false, true})
                    QTest::newRow(qPrintable(QString("%1-channels%2-selected%3").arg(mode).arg(channels).arg(selected)))
                        << false << false << (selected ? QStringLiteral("selection") : QStringLiteral("gpu")) << false
                        << channels << mode;
            for (const auto &path : {"selection-inverted",
                                     "selection-translated",
                                     "selection-empty",
                                     "selection-outside",
                                     "selection-failure",
                                     "selection-merge-failure",
                                     "merge-budget",
                                     "offset",
                                     "integer"})
                QTest::newRow(qPrintable(mode + '-' + path))
                    << false << false << QString::fromLatin1(path) << false << 5 << mode;
        }
    }
    void testHalfWashBlendModes()
    {
        QFETCH(int, channels);
        QFETCH(QString, mode);
        runIndirectMerge(channels, mode, true);
    }

private:
    void runIndirectMerge(int channelBits = -1, const QString &modeOverride = QString(), bool half = false)
    {
        QFETCH(bool, locked);
        QFETCH(bool, limitedReadback);
        QFETCH(QString, previewPath);
        QFETCH(bool, erase);
        const QString mode = modeOverride.isEmpty() ? (erase ? COMPOSITE_ERASE : COMPOSITE_OVER) : modeOverride;
        const auto *cs = space(half);
        const float tolerance = half ? 0.002f : 2e-5f;
        auto dabs = makeDabs(12, 89, half);
        for (int i = 0; i < dabs.size(); ++i)
            dabs[i].averageOpacity = i % 2 ? 0.9 : dabs[i].opacity;
        const auto bounds = boundsOf(dabs).adjusted(-10, -10, 10, 10);
        KisImageSP image = new KisImage(nullptr,
                                        256,
                                        256,
                                        previewPath == "integer" ? KoColorSpaceRegistry::instance()->rgb8() : cs,
                                        "indirect GPU brush");
        KisSharedPtr<PreviewPaintLayer> cpu = new PreviewPaintLayer(image, "cpu", 255, cs),
                                        gpu = new PreviewPaintLayer(image, "gpu", 255, cs);
        KisPaintDeviceSP cpuTarget = new KisPaintDevice(cs), gpuTarget = new KisPaintDevice(cs);
        if (previewPath == "offset") {
            cpuTarget->moveTo(3, -5);
            gpuTarget->moveTo(3, -5);
        }
        KoColor background(cs);
        cs->fromNormalisedChannelsValue(background.data(), {0.8f, 0.2f, 0.3f, 0.6f});
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(bounds.adjusted(35, 21, -25, -13), 173);
        const bool selected = locked || previewPath.startsWith("selection");
        const bool noCoverage = previewPath == "selection-empty" || previewPath == "selection-outside";
        if (previewPath.startsWith("selection")) {
            const QRect maskRect = bounds.adjusted(9, 7, -8, -6);
            QByteArray coverage(maskRect.width() * maskRect.height(), char(0));
            for (qsizetype i = 0; i < coverage.size(); ++i)
                coverage[i] = char(i * 17 % 256);
            selection->pixelSelection()->writeBytes(reinterpret_cast<const quint8 *>(coverage.constData()), maskRect);
            if (previewPath == "selection-inverted")
                selection->pixelSelection()->invert();
            if (previewPath == "selection-translated")
                selection->pixelSelection()->moveTo(13, -11);
            if (noCoverage) {
                selection->pixelSelection()->clear();
                if (previewPath == "selection-outside")
                    selection->pixelSelection()->select(bounds.translated(4096, 4096));
            }
        }
        for (auto layer : {cpu, gpu}) {
            layer->paintDevice()->fill(bounds.x(), bounds.y(), bounds.width(), bounds.height(), background.data());
            if (erase || channelBits >= 0) {
                auto data = pixels(layer->paintDevice(), bounds);
                for (size_t i = 0; i < data.size(); i += 4) {
                    data[i] = 1.7f;
                    data[i + 1] = -0.3f;
                    data[i + 3] = i % 12 == 0 ? 0.0f : (i % 12 == 4 ? 1.0f : 0.6f);
                }
                QByteArray raw(bounds.width() * bounds.height() * cs->pixelSize(), Qt::Uninitialized);
                space()->convertPixelsTo(reinterpret_cast<const quint8 *>(data.data()),
                                         reinterpret_cast<quint8 *>(raw.data()),
                                         cs,
                                         bounds.width() * bounds.height(),
                                         KoColorConversionTransformation::internalRenderingIntent(),
                                         KoColorConversionTransformation::internalConversionFlags());
                layer->paintDevice()->writeBytes(reinterpret_cast<const quint8 *>(raw.constData()), bounds);
            }
            layer->setTemporaryTarget(layer == cpu ? cpuTarget : gpuTarget);
            layer->setTemporaryCompositeOp(mode);
            layer->setTemporaryOpacity(0.61);
            if (selected)
                layer->setTemporarySelection(selection);
            if (locked || previewPath == "channels" || channelBits >= 0) {
                QBitArray flags(4, true);
                if (channelBits >= 0) {
                    for (int i = 0; i < 4; ++i)
                        flags.setBit(i, channelBits & (1 << i));
                } else {
                    flags.clearBit(locked ? 3 : 1);
                }
                layer->setTemporaryChannelFlags(flags);
            }
        }
        KisPainter cpuPainter(cpuTarget), gpuPainter(gpuTarget);
        cpuPainter.setCompositeOpId(COMPOSITE_ALPHA_DARKEN);
        gpuPainter.setCompositeOpId(COMPOSITE_ALPHA_DARKEN);
        const auto before = pixels(gpu->paintDevice(), bounds);
        // Leave the GPU temporary target CPU-stale until the real final-merge API reads it.
        for (int i = 0; i < 2; ++i) {
            cpuPainter.bltFixed(bounds, dabs);
            QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
        }
        // Final Wash jobs enumerate allocated tiles, including transparent
        // pixels. Generic locked modes clear hidden RGB even at zero coverage,
        // so allocating tiles in gaps between dabs would change the result.
        QRegion cpuRegion, gpuRegion;
        for (const auto &tileRect : cpuTarget->region().rects())
            cpuRegion += tileRect;
        for (const auto &tileRect : gpuTarget->region().rects())
            gpuRegion += tileRect;
        QCOMPARE(gpuRegion, cpuRegion);
        auto *backend = KisGpuTileBackend::instance();
        auto &context = backend->context();
        const auto resetReadback = qScopeGuard([&]() {
            backend->injectReadbackLimitForTesting(~quint64(0));
        });
        if (limitedReadback)
            backend->injectReadbackLimitForTesting(64 * 1024);
        KisPaintDeviceSP cpuPreview = new KisPaintDevice(cs), gpuPreview = new KisPaintDevice(cs);
        qputenv("KRITA_GPU_BRUSH", "0");
        cpu->copyOriginalToProjection(cpu->paintDevice(), cpuPreview, bounds);
        qputenv("KRITA_GPU_BRUSH", "1");
        context.waitIdle();
        const auto beforePreview = context.completedValue();
        const auto beforeGpuPreview = KisGpuBrushPainter::washPreviewCount();
        const auto beforeBaseCopy = KisGpuBrushPainter::washBaseCopyCount();
        const bool previewFailure = previewPath == "failure" || previewPath == "selection-failure";
        // Phase 4.90: the preview with the base copy, then the plain GPU preview.
        if (previewFailure)
            context.injectSubmitFailuresForTesting(2);
        gpu->copyOriginalToProjection(gpu->paintDevice(), gpuPreview, bounds);
        context.waitIdle();
        const auto previewSubmissions = context.completedValue() - beforePreview;
        const bool previewGpu = !previewFailure && previewPath != "offset" && previewPath != "integer";
        QCOMPARE(KisGpuBrushPainter::washPreviewCount() - beforeGpuPreview, previewGpu ? quint64(1) : quint64(0));
        QCOMPARE(KisGpuBrushPainter::washBaseCopyCount() - beforeBaseCopy, previewGpu ? quint64(1) : quint64(0));
        if (!noCoverage)
            QVERIFY(previewSubmissions > 0);
        // Without coverage the GPU still submits the base copy.
        if (!limitedReadback)
            QCOMPARE(previewSubmissions, noCoverage && !previewGpu ? quint64(0) : quint64(1));
        const auto previewPixels = pixels(gpuPreview, bounds);
        const bool ignoreHiddenRgb = !half && mode == COMPOSITE_OVER && (channelBits < 0 || channelBits == 15);
        QVERIFY(difference(pixels(cpuPreview, bounds), previewPixels, ignoreHiddenRgb) <= tolerance);
        if (erase) {
            for (size_t i = 0; i < before.size(); i += 4) {
                for (int channel = 0; channel < 3; ++channel)
                    QCOMPARE(previewPixels[i + channel], before[i + channel]);
                QVERIFY(previewPixels[i + 3] <= before[i + 3]);
            }
        }
        // Make the temporary target CPU-stale again before the final merge.
        cpuPainter.bltFixed(bounds, dabs);
        QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
        KUndo2Command cpuCommand, gpuCommand;
        KisPaintDeviceSP snapshot = new KisPaintDevice(*gpu->paintDevice());
        qputenv("KRITA_GPU_BRUSH", "0");
        cpu->mergeToLayer(cpu, &cpuCommand, kundo2_noi18n("CPU wash"), -1);
        qputenv("KRITA_GPU_BRUSH", "1");
        context.waitIdle();
        const auto budget = backend->memoryBudget();
        const auto restoreBudget = qScopeGuard([&]() {
            backend->setMemoryBudgetForTesting(budget);
        });
        if (previewPath == "merge-budget") {
            backend->evictTiles(~quint64(0));
            backend->setMemoryBudgetForTesting(backend->pool(cs->pixelSize())->tileBytes());
        }
        const auto beforeMerge = context.completedValue();
        const auto beforeGpuMerge = KisGpuBrushPainter::washMergeCount();
        const auto mergeRects = gpuTarget->region().rects().size();
        const bool mergeFailure = previewPath == "merge-failure" || previewPath == "selection-merge-failure";
        if (mergeFailure)
            context.injectSubmitFailuresForTesting(1);
        gpu->mergeToLayer(gpu, &gpuCommand, kundo2_noi18n("GPU wash"), -1);
        context.waitIdle();
        const bool gpuMerge = previewPath != "offset" && previewPath != "integer" && previewPath != "merge-budget";
        QCOMPARE(KisGpuBrushPainter::washMergeCount() - beforeGpuMerge,
                 gpuMerge ? quint64(mergeRects - (mergeFailure ? 1 : 0)) : quint64(0));
        const auto mergeSubmissions = context.completedValue() - beforeMerge;
        if (previewPath != "merge-budget" && !noCoverage)
            QVERIFY(mergeSubmissions > 0);
        if (!limitedReadback)
            QVERIFY(mergeSubmissions <= quint64(mergeRects));
        cpuCommand.redo();
        gpuCommand.redo();
        QVERIFY(!gpu->hasTemporaryTarget());
        const auto after = pixels(gpu->paintDevice(), bounds);
        const auto expected = pixels(cpu->paintDevice(), bounds);
        const float finalError = difference(expected, after, ignoreHiddenRgb);
        if (finalError > tolerance) {
            for (size_t i = 0; i < after.size(); ++i) {
                if (std::abs(expected[i] - after[i]) > tolerance) {
                    qInfo() << "first mismatch"
                            << bounds.topLeft() + QPoint(int(i / 4) % bounds.width(), int(i / 4) / bounds.width())
                            << "channel" << i % 4 << "CPU/GPU" << expected[i] << after[i] << "alpha CPU/GPU"
                            << expected[i / 4 * 4 + 3] << after[i / 4 * 4 + 3] << "source regions CPU/GPU"
                            << cpuTarget->region().rects() << gpuTarget->region().rects();
                    break;
                }
            }
        }
        QVERIFY2(finalError <= tolerance, qPrintable(QString::number(finalError)));
        QCOMPARE(after != before, !noCoverage && (mode != COMPOSITE_OVER || channelBits != 0));
        QCOMPARE(pixels(snapshot, bounds), before);
        QVERIFY(!backend->hasFailed());
        gpuCommand.undo();
        QCOMPARE(pixels(gpu->paintDevice(), bounds), before);
        gpuCommand.redo();
        QCOMPARE(pixels(gpu->paintDevice(), bounds), after);
    }
private Q_SLOTS:
    void testPreparePipelines()
    {
        // Phase 4.91: compiling ahead is idempotent; afterwards, new work
        // contexts and Wash previews compile nothing.
        auto &context = KisGpuTileBackend::instance()->context();
        QVERIFY(KisGpuTileBackend::preparePipelines());
        const int compiled = context.sharedComputePipelineCompileCount();
        QVERIFY(KisGpuTileBackend::preparePipelines());
        QVERIFY(KisGpuProjectionCompositor::resetWorkContextsForTesting());
        const QRect bounds(-77, -29, 131, 93);
        KisPaintDeviceSP source = new KisPaintDevice(space()), destination = new KisPaintDevice(space());
        KisPainter sourcePainter(source), painter(destination);
        QVERIFY(KisGpuBrushPainter::paint(&sourcePainter, makeDabs(5, 89)));
        painter.setCompositeOpId(COMPOSITE_OVERLAY);
        QVERIFY(KisGpuBrushPainter::paintWashPreview(&painter, source, bounds));
        context.waitIdle();
        QCOMPARE(context.sharedComputePipelineCompileCount(), compiled);
    }
    void testWashPreviewBaseCopy_data()
    {
        QTest::addColumn<bool>("half");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("variant");
        for (bool half : {false, true}) {
            for (const QString &mode :
                 {QString(COMPOSITE_OVER), QString(COMPOSITE_ERASE), QString(COMPOSITE_OVERLAY)}) {
                for (const QString &variant : {"plain",
                                               "selection",
                                               "selection-outside",
                                               "empty-original",
                                               "opaque-default",
                                               "background",
                                               "empty-background",
                                               "stale-gpu-projection"})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(half ? "f16" : "f32", mode, variant)))
                        << half << mode << variant;
            }
        }
    }
    void testWashPreviewBaseCopy()
    {
        // Phase 4.90: copyOriginalToProjection copies the original on the GPU
        // in the preview submission. The projection holds unrelated pixels
        // inside and around the rect; the original covers only part of it.
        QFETCH(bool, half);
        QFETCH(QString, mode);
        QFETCH(QString, variant);
        const auto *cs = space(half);
        const QRect rect(-77, -29, 213, 141);
        const QRect around = rect.adjusted(-70, -70, 70, 70);
        const auto dabs = makeDabs(5, 89, half);
        KisImageSP image = new KisImage(nullptr, 512, 512, cs, "Wash base copy");
        KisSharedPtr<PreviewPaintLayer> layer = new PreviewPaintLayer(image, "paint", 255, cs);
        KoColor fill(cs), stale(cs);
        cs->fromNormalisedChannelsValue(fill.data(), {0.8f, 0.2f, 0.3f, 0.6f});
        cs->fromNormalisedChannelsValue(stale.data(), {0.1f, 0.9f, 0.4f, 0.7f});
        // "background": a Background raster layer (KisDocument::newImage) has an
        // opaque default pixel, which its projection clone shares.
        KoColor paper(cs);
        cs->fromNormalisedChannelsValue(paper.data(), {0.95f, 0.9f, 0.85f, 1.0f});
        const bool background = variant.endsWith("background");
        if (variant == "opaque-default" || background)
            layer->paintDevice()->setDefaultPixel(paper);
        if (variant != "empty-original" && variant != "empty-background")
            layer->paintDevice()->fill(rect.adjusted(40, 30, -90, 0), fill);
        KisPaintDeviceSP target = new KisPaintDevice(cs);
        KisPainter targetPainter(target);
        targetPainter.setCompositeOpId(COMPOSITE_ALPHA_DARKEN);
        QVERIFY(KisGpuBrushPainter::paint(&targetPainter, dabs));
        layer->setTemporaryTarget(target);
        layer->setTemporaryCompositeOp(mode);
        layer->setTemporaryOpacity(0.61);
        if (variant.startsWith("selection")) {
            KisSelectionSP selection = new KisSelection();
            if (variant == "selection")
                selection->pixelSelection()->select(rect.adjusted(35, 21, -25, -13), 173);
            else
                selection->pixelSelection()->select(rect.translated(4096, 4096));
            layer->setTemporarySelection(selection);
        }
        KisPaintDeviceSP cpuProjection = new KisPaintDevice(cs), gpuProjection = new KisPaintDevice(cs);
        for (auto projection : {cpuProjection, gpuProjection}) {
            if (background)
                projection->setDefaultPixel(paper);
            projection->fill(around, stale);
        }
        auto &context = KisGpuTileBackend::instance()->context();
        if (variant == "stale-gpu-projection") {
            // Leave the GPU projection's tiles GPU-valid and CPU-stale, as a
            // previous preview does, before the next base copy.
            KisPainter gpuPainter(gpuProjection), cpuPainter(cpuProjection);
            gpuPainter.setCompositeOpId(COMPOSITE_OVER);
            cpuPainter.setCompositeOpId(COMPOSITE_OVER);
            QVERIFY(KisGpuBrushPainter::paintWashPreview(&gpuPainter, target, around));
            cpuPainter.bitBlt(around.topLeft(), target, around);
        }
        qputenv("KRITA_GPU_BRUSH", "0");
        layer->copyOriginalToProjection(layer->paintDevice(), cpuProjection, rect);
        qputenv("KRITA_GPU_BRUSH", "1");
        context.waitIdle();
        const auto beforeBase = KisGpuBrushPainter::washBaseCopyCount();
        const auto beforeSubmissions = context.completedValue();
        layer->copyOriginalToProjection(layer->paintDevice(), gpuProjection, rect);
        context.waitIdle();
        const bool gpuBase = variant != "opaque-default";
        QCOMPARE(KisGpuBrushPainter::washBaseCopyCount() - beforeBase, gpuBase ? quint64(1) : quint64(0));
        if (gpuBase)
            QCOMPARE(context.completedValue() - beforeSubmissions, quint64(1));
        // The pixels around the rect must stay untouched; inside it the GPU
        // matches the CPU copy plus preview.
        const float tolerance = half ? 1.0f / 1024 : 2e-5f;
        const float error =
            difference(pixels(cpuProjection, around), pixels(gpuProjection, around), mode == COMPOSITE_OVER && !half);
        QVERIFY2(error <= tolerance, qPrintable(QString::number(error)));
        QVERIFY(!KisGpuTileBackend::instance()->hasFailed());
    }
    void testWashSelectionChanges()
    {
        const QRect bounds(-77, -29, 131, 93);
        const auto dabs = makeDabs(5, 89);
        KisPaintDeviceSP source = new KisPaintDevice(space());
        KisPainter sourcePainter(source);
        QVERIFY(KisGpuBrushPainter::paint(&sourcePainter, dabs));
        for (const auto &mode : channelModes) {
            KisPaintDeviceSP cpu = new KisPaintDevice(space()), gpu = new KisPaintDevice(space());
            const float color[] = {1.7f, -0.3f, 0.2f, 0.8f};
            for (auto device : {cpu, gpu})
                device->fill(bounds.x(),
                             bounds.y(),
                             bounds.width(),
                             bounds.height(),
                             reinterpret_cast<const quint8 *>(color));
            KisPainter cpuPainter(cpu), gpuPainter(gpu);
            cpuPainter.setCompositeOpId(mode);
            gpuPainter.setCompositeOpId(mode);
            cpuPainter.setOpacityF(0.43);
            gpuPainter.setOpacityF(0.43);
            KisSelectionSP selection = new KisSelection();
            // Reuse compositor resources across changed and removed selections.
            // Odd dimensions also exercise the final padded mask word.
            for (int step = 0; step < 3; ++step) {
                if (step < 2) {
                    selection->pixelSelection()->clear();
                    selection->pixelSelection()->select(bounds.adjusted(step * 11, 0, -step * 4, 0), step ? 219 : 61);
                }
                cpuPainter.setSelection(step < 2 ? selection : KisSelectionSP());
                gpuPainter.setSelection(step < 2 ? selection : KisSelectionSP());
                cpuPainter.bitBlt(bounds.topLeft(), source, bounds);
                QVERIFY(KisGpuBrushPainter::paintWashPreview(&gpuPainter, source, bounds));
                QVERIFY(difference(pixels(cpu, bounds), pixels(gpu, bounds), mode != COMPOSITE_ERASE) <= 2e-5f);
            }
        }
    }
    void testFinalMergeRejectsPartialTiles()
    {
        const auto dabs = makeDabs(4, 73);
        KisPaintDeviceSP source = new KisPaintDevice(space()), destination = new KisPaintDevice(space());
        KisPainter sourcePainter(source), painter(destination);
        sourcePainter.bltFixed(boundsOf(dabs), dabs);
        const QRect bounds(-128, -64, 256, 192);
        const auto before = pixels(destination, bounds);
        const auto count = KisGpuBrushPainter::washMergeCount();
        for (const QRect &rect : {QRect(),
                                  QRect(-127, -64, 128, 64),
                                  QRect(-128, -63, 128, 64),
                                  QRect(-128, -64, 127, 64),
                                  QRect(-128, -64, 128, 63)}) {
            QVERIFY(!KisGpuBrushPainter::mergeWash(&painter, source, rect));
            QCOMPARE(pixels(destination, bounds), before);
        }
        QCOMPARE(KisGpuBrushPainter::washMergeCount(), count);
    }
    void benchmarkBatch_data()
    {
        QTest::addColumn<bool>("masked");
        QTest::addColumn<bool>("alphaLocked");
        QTest::addColumn<bool>("erase");
        QTest::newRow("unselected") << false << false << false;
        QTest::newRow("selection") << true << false << false;
        QTest::newRow("alpha-lock") << false << true << false;
        QTest::newRow("selection-alpha-lock") << true << true << false;
        QTest::newRow("erase") << false << false << true;
        QTest::newRow("selected-erase") << true << false << true;
    }
    void benchmarkWashPreview_data()
    {
        QTest::addColumn<bool>("erase");
        QTest::addColumn<bool>("selected");
        QTest::newRow("normal") << false << false;
        QTest::newRow("erase") << true << false;
        QTest::newRow("selected-normal") << false << true;
        QTest::newRow("selected-erase") << true << true;
    }
    void benchmarkWashFinalMerge_data()
    {
        benchmarkWashPreview_data();
    }
    void benchmarkWashFinalMerge()
    {
        QFETCH(bool, erase);
        QFETCH(bool, selected);
        const QRect bounds(-128, -128, 1024, 1024);
        const auto dabs = makeDabs(12, 256);
        const QVector<QRect> rects{boundsOf(dabs)};
        auto &context = KisGpuTileBackend::instance()->context();
        std::vector<float> reference;
        for (bool gpuMerge : {false, true}) {
            std::vector<double> samples;
            for (int iteration = 0; iteration < 6; ++iteration) {
                KisImageSP image = new KisImage(nullptr, 1024, 1024, space(), "Wash final merge benchmark");
                KisPaintLayerSP layer = new KisPaintLayer(image, "paint", 255);
                KisPaintDeviceSP target = new KisPaintDevice(space());
                const float background[] = {0.8f, 0.2f, 0.3f, 0.6f};
                layer->paintDevice()->fill(bounds.x(),
                                           bounds.y(),
                                           bounds.width(),
                                           bounds.height(),
                                           reinterpret_cast<const quint8 *>(background));
                layer->setTemporaryTarget(target);
                layer->setTemporaryCompositeOp(erase ? COMPOSITE_ERASE : COMPOSITE_OVER);
                layer->setTemporaryOpacity(0.61);
                if (selected) {
                    KisSelectionSP selection = new KisSelection();
                    selection->pixelSelection()->select(bounds.adjusted(17, 13, -21, -19), 173);
                    layer->setTemporarySelection(selection);
                }
                KisPainter painter(target);
                painter.setCompositeOpId(COMPOSITE_ALPHA_DARKEN);
                painter.setMirrorInformation(QPointF(320, 320), true, true);
                QVERIFY(KisGpuBrushPainter::paintMirrored(&painter, dabs, rects));
                context.waitIdle();
                qputenv("KRITA_GPU_BRUSH", gpuMerge ? "1" : "0");
                KUndo2Command command;
                const auto firstMerge = KisGpuBrushPainter::washMergeCount();
                QElapsedTimer timer;
                timer.start();
                layer->mergeToLayer(layer, &command, kundo2_noi18n("Wash benchmark"), -1);
                context.waitIdle();
                if (iteration)
                    samples.push_back(timer.nsecsElapsed() / 1e6);
                QCOMPARE(KisGpuBrushPainter::washMergeCount() > firstMerge, gpuMerge);
                qputenv("KRITA_GPU_BRUSH", "1");
                const auto result = pixels(layer->paintDevice(), bounds);
                if (!gpuMerge)
                    reference = result;
                else
                    QVERIFY(difference(reference, result, !erase) <= 2e-5f);
            }
            std::sort(samples.begin(), samples.end());
            qInfo()
                << "Wash final merge, erase" << erase << "12 x 256px dabs, four mirror passes, GPU" << gpuMerge
                << "median ms" << samples[2] << "min" << samples.front() << "max" << samples.back()
                << "5 samples after warm-up; includes transaction and GPU completion; excludes brush jobs and canvas";
        }
    }
    void benchmarkWashPreview()
    {
        QFETCH(bool, erase);
        QFETCH(bool, selected);
        const QRect bounds(-128, -128, 1024, 1024);
        const auto dabs = makeDabs(12, 256);
        const QVector<QRect> rects{boundsOf(dabs)};
        std::vector<float> reference;
        auto &context = KisGpuTileBackend::instance()->context();
        for (bool gpuPreview : {false, true}) {
            KisImageSP image = new KisImage(nullptr, 1024, 1024, space(), "Wash preview benchmark");
            KisSharedPtr<PreviewPaintLayer> layer = new PreviewPaintLayer(image, "paint", 255);
            KisPaintDeviceSP target = new KisPaintDevice(space()), projection = new KisPaintDevice(space());
            const float background[] = {0.8f, 0.2f, 0.3f, 0.6f};
            layer->paintDevice()->fill(bounds.x(),
                                       bounds.y(),
                                       bounds.width(),
                                       bounds.height(),
                                       reinterpret_cast<const quint8 *>(background));
            layer->setTemporaryTarget(target);
            layer->setTemporaryCompositeOp(erase ? COMPOSITE_ERASE : COMPOSITE_OVER);
            layer->setTemporaryOpacity(0.61);
            if (selected) {
                KisSelectionSP selection = new KisSelection();
                selection->pixelSelection()->select(bounds.adjusted(17, 13, -21, -19), 173);
                layer->setTemporarySelection(selection);
            }
            KisPainter painter(target);
            painter.setCompositeOpId(COMPOSITE_ALPHA_DARKEN);
            painter.setMirrorInformation(QPointF(320, 320), true, true);
            std::vector<double> samples;
            for (int iteration = 0; iteration < 6; ++iteration) {
                // The same GPU brush produces fresh pixels for either display
                // path. Only preview composition is timed, including its wait.
                QVERIFY(KisGpuBrushPainter::paintMirrored(&painter, dabs, rects));
                context.waitIdle();
                qputenv("KRITA_GPU_BRUSH", gpuPreview ? "1" : "0");
                QElapsedTimer timer;
                timer.start();
                layer->copyOriginalToProjection(layer->paintDevice(), projection, bounds);
                context.waitIdle();
                if (iteration)
                    samples.push_back(timer.nsecsElapsed() / 1e6);
                qputenv("KRITA_GPU_BRUSH", "1");
            }
            const auto result = pixels(projection, bounds);
            if (!gpuPreview)
                reference = result;
            else
                QVERIFY(difference(reference, result, false) <= 2e-5f);
            std::sort(samples.begin(), samples.end());
            qInfo() << "Wash preview, erase" << erase << "12 x 256px dabs, four mirror passes, GPU preview"
                    << gpuPreview << "median ms" << samples[2] << "min" << samples.front() << "max" << samples.back()
                    << "5 samples after warm-up; excludes brush jobs and canvas";
        }
    }
    void benchmarkBatch()
    {
        QFETCH(bool, masked);
        QFETCH(bool, alphaLocked);
        QFETCH(bool, erase);
        const auto dabs = makeDabs(32, 256);
        const auto bounds = boundsOf(dabs);
        KisPaintDeviceSP cpu = new KisPaintDevice(space()), gpu = new KisPaintDevice(space());
        KisPainter cpuPainter(cpu), gpuPainter(gpu);
        cpuPainter.setCompositeOpId(erase ? COMPOSITE_ERASE : COMPOSITE_OVER);
        gpuPainter.setCompositeOpId(erase ? COMPOSITE_ERASE : COMPOSITE_OVER);
        if (alphaLocked) {
            QBitArray flags(4, true);
            flags.clearBit(3);
            cpuPainter.setChannelFlags(flags);
            gpuPainter.setChannelFlags(flags);
        }
        if (alphaLocked || erase) {
            const float background[] = {0.1f, 0.2f, 0.3f, 0.7f};
            cpu->fill(bounds.x(),
                      bounds.y(),
                      bounds.width(),
                      bounds.height(),
                      reinterpret_cast<const quint8 *>(background));
            gpu->fill(bounds.x(),
                      bounds.y(),
                      bounds.width(),
                      bounds.height(),
                      reinterpret_cast<const quint8 *>(background));
        }
        if (masked) {
            KisSelectionSP selection = new KisSelection();
            std::vector<quint8> mask(size_t(bounds.width()) * bounds.height());
            for (size_t i = 0; i < mask.size(); ++i)
                mask[i] = quint8(i * 17);
            selection->pixelSelection()->writeBytes(mask.data(), bounds);
            cpuPainter.setSelection(selection);
            gpuPainter.setSelection(selection);
        }
        // Warm pipeline and destination residency, then time completed work.
        for (int i = 0; i < 3; ++i) {
            cpuPainter.bltFixed(bounds, dabs);
            QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
        }
        KisGpuTileBackend::instance()->context().waitIdle();
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 5; ++i)
            cpuPainter.bltFixed(bounds, dabs);
        const double cpuMs = timer.nsecsElapsed() / 5e6;
        timer.restart();
        for (int i = 0; i < 5; ++i)
            QVERIFY(KisGpuBrushPainter::paint(&gpuPainter, dabs));
        KisGpuTileBackend::instance()->context().waitIdle();
        const double gpuMs = timer.nsecsElapsed() / 5e6;
        const float error = difference(pixels(cpu, bounds), pixels(gpu, bounds));
        QVERIFY2(error <= 2e-5f, qPrintable(QString::number(error)));
        qInfo() << "32 x 256px dabs, selection" << masked << "alpha lock" << alphaLocked << "completed batch: CPU"
                << cpuMs << "ms, GPU" << gpuMs << "ms";
    }
};
SIMPLE_TEST_MAIN(KisGpuBrushTest)
#include "KisGpuBrushTest.moc"
