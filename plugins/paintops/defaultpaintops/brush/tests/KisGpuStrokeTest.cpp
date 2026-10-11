/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../KisBrushOpSettings.h"
#include "../KisDabRenderingJob.h"
#include "../kis_brushop.h"
#include "../kis_brushop_settings_widget.h"
#include <KisAsynchronousStrokeUpdateHelper.h>
#include <KisGpuContext.h>
#include <KisLocalStrokeResources.h>
#include <KisPaintingModeOptionData.h>
#include <KisStandardOptionData.h>
#include <KisTextureOptionData.h>
#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <algorithm>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <cmath>
#include <gpu/KisGpuBrushPainter.h>
#include <gpu/KisGpuMaskingWorker.h>
#include <gpu/KisGpuMergeBatch.h>
#include <gpu/KisGpuTileBackend.h>
#include <kis_canvas_resource_provider.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_pixel_selection.h>
#include <kis_resources_snapshot.h>
#include <kis_selection.h>
#include <kis_simple_paintop_factory.h>
#include <kis_undo_stores.h>
#include <simpletest.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/KisMaskingBrushCompositeOpBase.h>
#include <strokes/KisMaskingBrushCompositeOpFactory.h>
#include <strokes/freehand_stroke.h>
#include <vector>

namespace
{
const QRect imageRect(0, 0, 1024, 1024);
std::vector<float> pixels(KisPaintDeviceSP device)
{
    std::vector<float> result(size_t(imageRect.width()) * imageRect.height() * 4);
    if (device->pixelSize() == 8) {
        QByteArray raw(imageRect.width() * imageRect.height() * 8, Qt::Uninitialized);
        device->readBytes(reinterpret_cast<quint8 *>(raw.data()), imageRect);
        const auto *target = KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(),
                                                                          Float32BitsColorDepthID.id(),
                                                                          QString());
        device->colorSpace()->convertPixelsTo(reinterpret_cast<const quint8 *>(raw.constData()),
                                              reinterpret_cast<quint8 *>(result.data()),
                                              target,
                                              imageRect.width() * imageRect.height(),
                                              KoColorConversionTransformation::internalRenderingIntent(),
                                              KoColorConversionTransformation::internalConversionFlags());
    } else {
        device->readBytes(reinterpret_cast<quint8 *>(result.data()), imageRect);
    }
    return result;
}
float difference(const std::vector<float> &a, const std::vector<float> &b)
{
    if (a.size() != b.size())
        return INFINITY;
    float result = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]))
            return INFINITY;
        result = qMax(result, std::abs(a[i] - b[i]));
    }
    return result;
}
} // namespace

// No canvas or tablet events: measure a queued stroke through final merge and
// projection completion, not input-to-display latency. Pixel readback is untimed.
class KisGpuStrokeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        if (!KisGpuTileBackend::instance())
            QSKIP("No GPU backend");
        QVERIFY(KisPaintOpRegistry::instance()->contains("paintbrush"));
        // Registry initialization loads installed plugins. Use the brush linked
        // into this test, so pre-install tests exercise the current build.
        KisPaintOpRegistry::instance()->add(
            new KisSimplePaintOpFactory<KisBrushOp, KisBrushOpSettings, KisBrushOpSettingsWidget>(
                "paintbrush",
                "Test pixel brush",
                KisPaintOpFactory::categoryStable(),
                QString(),
                QString(),
                QStringList(),
                1));
    }
    void cleanupTestCase()
    {
        qInfo() << "dab CPU generations skipped" << KisDabRenderingJobRunner::skippedGenerationCount()
                << "materialized for CPU use" << KisBrushOp::materializedDabCount();
        if (auto *backend = KisGpuTileBackend::existingInstance()) {
            backend->flush();
            QCOMPARE(backend->context().validationErrorCount(), 0);
        }
    }
    void testStroke_data()
    {
        QTest::addColumn<int>("diameter");
        QTest::addColumn<bool>("wash");
        QTest::addColumn<bool>("mirrors");
        QTest::addColumn<bool>("restricted");
        QTest::addColumn<bool>("distant");
        QTest::addColumn<bool>("alphaOnly");
        for (int diameter : {64, 256})
            for (bool wash : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(diameter).arg(wash ? "wash" : "buildup")))
                    << diameter << wash << false << false << false << false;
        for (bool wash : {false, true}) {
            QTest::newRow(wash ? "mirror-wash" : "mirror-buildup") << 128 << wash << true << false << false << false;
            QTest::newRow(wash ? "selected-locked-wash" : "selected-locked-buildup")
                << 128 << wash << true << true << false << false;
            QTest::newRow(wash ? "distant-wash" : "distant-buildup") << 128 << wash << true << false << true << false;
            QTest::newRow(wash ? "alpha-locked-wash" : "alpha-locked-buildup")
                << 256 << wash << true << false << true << true;
            QTest::newRow(wash ? "large-alpha-locked-wash" : "large-alpha-locked-buildup")
                << 1024 << wash << true << false << true << true;
            QTest::newRow(wash ? "dense-alpha-locked-wash" : "dense-alpha-locked-buildup")
                << 300 << wash << true << false << true << true;
        }
    }
    void testStroke()
    {
        runStroke(false);
    }
    void testGaussStroke_data()
    {
        testStroke_data();
    }
    void testRefusedPendingBatches_data()
    {
        testStroke_data();
    }
    void testRefusedPendingBatches()
    {
        // Phase 4.85: refused GPU batches of dabs without CPU pixels fall back
        // to the CPU after materialization, with unchanged parity.
        m_refusePendingBatches = 2;
        auto restore = qScopeGuard([this]() {
            m_refusePendingBatches = 0;
            KisGpuBrushPainter::refusePendingBatchesForTesting(0);
        });
        runStroke(false);
    }
    void testSoftStroke_data()
    {
        testStroke_data();
    }
    void testSoftStroke()
    {
        // Phase 4.87: the Soft (curve) circle mask.
        m_maskGenerator = QStringLiteral("soft");
        auto restore = qScopeGuard([this]() {
            m_maskGenerator = QStringLiteral("default");
        });
        runStroke(false);
    }
    void testTextureModes_data()
    {
        QTest::addColumn<int>("diameter");
        QTest::addColumn<bool>("wash");
        QTest::addColumn<bool>("mirrors");
        QTest::addColumn<bool>("restricted");
        QTest::addColumn<bool>("distant");
        QTest::addColumn<bool>("alphaOnly");
        QTest::addColumn<int>("textureMode");
        QTest::addColumn<bool>("soft");
        const QList<QPair<int, QString>> modes{
            {KisTextureOptionData::MULTIPLY, QStringLiteral("multiply")},
            {KisTextureOptionData::SUBTRACT, QStringLiteral("subtract")},
            {KisTextureOptionData::DARKEN, QStringLiteral("darken")},
            {KisTextureOptionData::OVERLAY, QStringLiteral("overlay")},
            {KisTextureOptionData::COLOR_DODGE, QStringLiteral("dodge")},
            {KisTextureOptionData::COLOR_BURN, QStringLiteral("burn")},
            {KisTextureOptionData::LINEAR_DODGE, QStringLiteral("linear_dodge")},
            {KisTextureOptionData::LINEAR_BURN, QStringLiteral("linear_burn")},
            {KisTextureOptionData::HARD_MIX_PHOTOSHOP, QStringLiteral("hard_mix")},
            {KisTextureOptionData::HARD_MIX_SOFTER_PHOTOSHOP, QStringLiteral("hard_mix_softer")},
            {KisTextureOptionData::HEIGHT, QStringLiteral("height")},
            {KisTextureOptionData::LINEAR_HEIGHT, QStringLiteral("linear_height")},
            {KisTextureOptionData::HEIGHT_PHOTOSHOP, QStringLiteral("height_photoshop")},
            {KisTextureOptionData::LINEAR_HEIGHT_PHOTOSHOP, QStringLiteral("linear_height_photoshop")},
            // CPU texturing (changes the dab color): never described
            {KisTextureOptionData::LIGHTNESS, QStringLiteral("lightness")}};
        int row = 0;
        for (const auto &mode : modes) {
            for (bool soft : {false, true}) {
                const bool mirrors = row % 5 == 2;
                QTest::newRow(qPrintable(
                    QStringLiteral("%1-%2%3").arg(mode.second, soft ? "soft" : "hard", mirrors ? "-mirrors" : "")))
                    << 80 << (row % 2 == 1) << mirrors << false << false << false << mode.first << soft;
                ++row;
            }
        }
    }
    void testTextureModes()
    {
        // GPU textured dabs ("Textured dabs"): every alpha-only texturing
        // mode, hard and soft, at strength 0.6 with a texture offset.
        QFETCH(int, textureMode);
        QFETCH(bool, soft);
        m_textureMode = textureMode;
        m_softTexturing = soft;
        m_textureStrength = 0.6;
        auto restore = qScopeGuard([this]() {
            m_textureMode = -1;
            m_softTexturing = false;
            m_textureStrength = 1.0;
        });
        runStroke(false, false, -1, QString(), false, true);
    }
    void testGaussStroke()
    {
        // Phase 4.84: the Gaussian circle mask (the measured Basic-4 preset family).
        m_maskGenerator = QStringLiteral("gauss");
        auto restore = qScopeGuard([this]() {
            m_maskGenerator = QStringLiteral("default");
        });
        runStroke(false);
    }
    void testEraseStroke_data()
    {
        QTest::addColumn<int>("diameter");
        QTest::addColumn<bool>("wash");
        QTest::addColumn<bool>("mirrors");
        QTest::addColumn<bool>("restricted");
        QTest::addColumn<bool>("distant");
        QTest::addColumn<bool>("alphaOnly");
        for (bool wash : {false, true}) {
            QTest::newRow(wash ? "wash" : "buildup") << 128 << wash << false << false << false << false;
            QTest::newRow(wash ? "mirrored-wash" : "mirrored-buildup")
                << 128 << wash << true << false << false << false;
            QTest::newRow(wash ? "restricted-wash" : "restricted-buildup")
                << 128 << wash << false << true << false << false;
        }
    }
    void testEraseStroke()
    {
        runStroke(true);
    }
    void testHalfStroke_data()
    {
        QTest::addColumn<int>("diameter");
        QTest::addColumn<bool>("wash");
        QTest::addColumn<bool>("mirrors");
        QTest::addColumn<bool>("restricted");
        QTest::addColumn<bool>("distant");
        QTest::addColumn<bool>("alphaOnly");
        QTest::addColumn<bool>("erase");
        for (bool erase : {false, true})
            for (bool wash : {false, true})
                for (bool restricted : {false, true})
                    QTest::newRow(qPrintable(
                        QString("erase%1-wash%2-selected-locked-mirrors%3").arg(erase).arg(wash).arg(restricted)))
                        << 128 << wash << restricted << restricted << false << false << erase;
    }
    void testHalfStroke()
    {
        QFETCH(bool, erase);
        runStroke(erase, false, -1, QString(), false, false, true);
    }
    void testSelectedWash_data()
    {
        QTest::addColumn<int>("diameter");
        QTest::addColumn<bool>("wash");
        QTest::addColumn<bool>("mirrors");
        QTest::addColumn<bool>("restricted");
        QTest::addColumn<bool>("distant");
        QTest::addColumn<bool>("alphaOnly");
        QTest::addColumn<bool>("erase");
        for (bool erase : {false, true})
            for (bool mirrors : {false, true})
                QTest::newRow(qPrintable(QString("erase%1-mirrors%2").arg(erase).arg(mirrors)))
                    << 128 << true << mirrors << false << false << false << erase;
    }
    void testSelectedWash()
    {
        QFETCH(bool, erase);
        runStroke(erase, true);
    }
    void testRestrictedChannels_data()
    {
        QTest::addColumn<int>("diameter");
        QTest::addColumn<bool>("wash");
        QTest::addColumn<bool>("mirrors");
        QTest::addColumn<bool>("restricted");
        QTest::addColumn<bool>("distant");
        QTest::addColumn<bool>("alphaOnly");
        QTest::addColumn<bool>("erase");
        QTest::addColumn<int>("channels");
        for (bool erase : {false, true})
            for (bool wash : {false, true})
                for (int channels : {7, 13, 5})
                    QTest::newRow(qPrintable(QString("erase%1-wash%2-channels%3").arg(erase).arg(wash).arg(channels)))
                        << 128 << wash << true << false << false << false << erase << channels;
    }
    void testRestrictedChannels()
    {
        QFETCH(bool, erase);
        QFETCH(int, channels);
        runStroke(erase, true, channels);
    }
    void testBlendModes_data()
    {
        QTest::addColumn<int>("diameter");
        QTest::addColumn<bool>("wash");
        QTest::addColumn<bool>("mirrors");
        QTest::addColumn<bool>("restricted");
        QTest::addColumn<bool>("distant");
        QTest::addColumn<bool>("alphaOnly");
        QTest::addColumn<int>("channels");
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {COMPOSITE_MULT,         COMPOSITE_SCREEN,     COMPOSITE_ADD,
                                 COMPOSITE_LINEAR_DODGE, COMPOSITE_SUBTRACT,   COMPOSITE_DARKEN,
                                 COMPOSITE_LIGHTEN,      COMPOSITE_DIFF,       COMPOSITE_OVERLAY,
                                 COMPOSITE_HARD_LIGHT,   COMPOSITE_EXCLUSION,  COMPOSITE_LINEAR_BURN,
                                 COMPOSITE_LINEAR_LIGHT, COMPOSITE_PIN_LIGHT,  COMPOSITE_SOFT_LIGHT_SVG,
                                 COMPOSITE_DODGE,        COMPOSITE_BURN,       COMPOSITE_COLOR,
                                 COMPOSITE_HUE,          COMPOSITE_SATURATION, COMPOSITE_LUMINIZE})
            for (bool wash : {false, true})
                for (int channels : {15, 7, 5})
                    QTest::newRow(qPrintable(QString("%1-wash%2-channels%3").arg(mode).arg(wash).arg(channels)))
                        << 128 << wash << true << false << false << false << channels << mode;
        // Phase 4.88: without mirrors the GPU batches at its own (shorter) period.
        for (const auto &mode : {COMPOSITE_OVERLAY, COMPOSITE_DODGE, COMPOSITE_SATURATION})
            for (bool wash : {false, true})
                QTest::newRow(qPrintable(QString("%1-wash%2-unmirrored").arg(mode).arg(wash)))
                    << 128 << wash << false << false << false << false << 15 << mode;
    }
    void testBlendModes()
    {
        QFETCH(int, channels);
        QFETCH(QString, mode);
        runStroke(false, true, channels, mode);
    }
    void testHalfBlendModes_data()
    {
        QTest::addColumn<int>("diameter");
        QTest::addColumn<bool>("wash");
        QTest::addColumn<bool>("mirrors");
        QTest::addColumn<bool>("restricted");
        QTest::addColumn<bool>("distant");
        QTest::addColumn<bool>("alphaOnly");
        QTest::addColumn<int>("channels");
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {COMPOSITE_MULT,         COMPOSITE_SCREEN,     COMPOSITE_ADD,
                                 COMPOSITE_LINEAR_DODGE, COMPOSITE_SUBTRACT,   COMPOSITE_DARKEN,
                                 COMPOSITE_LIGHTEN,      COMPOSITE_DIFF,       COMPOSITE_OVERLAY,
                                 COMPOSITE_HARD_LIGHT,   COMPOSITE_EXCLUSION,  COMPOSITE_LINEAR_BURN,
                                 COMPOSITE_LINEAR_LIGHT, COMPOSITE_PIN_LIGHT,  COMPOSITE_SOFT_LIGHT_SVG,
                                 COMPOSITE_DODGE,        COMPOSITE_BURN,       COMPOSITE_COLOR,
                                 COMPOSITE_HUE,          COMPOSITE_SATURATION, COMPOSITE_LUMINIZE})
            for (bool wash : {false, true})
                for (int channels : {15, 7, 5})
                    QTest::newRow(qPrintable(QString("%1-wash%2-channels%3").arg(mode).arg(wash).arg(channels)))
                        << 128 << wash << true << false << false << false << channels << mode;
    }
    void testHalfBlendModes()
    {
        QFETCH(int, channels);
        QFETCH(QString, mode);
        runStroke(false, true, channels, mode, false, false, true);
    }
    void testTexturedMaskedStroke_data()
    {
        QTest::addColumn<int>("diameter");
        QTest::addColumn<bool>("wash");
        QTest::addColumn<bool>("mirrors");
        QTest::addColumn<bool>("restricted");
        QTest::addColumn<bool>("distant");
        QTest::addColumn<bool>("alphaOnly");
        QTest::addColumn<bool>("masked");
        QTest::addColumn<bool>("textured");
        for (int diameter : {150, 300}) {
            for (bool textured : {false, true}) {
                QTest::newRow(qPrintable(QString("masked-%1-texture%2").arg(diameter).arg(textured)))
                    << diameter << true << false << false << false << false << true << textured;
            }
        }
        for (bool wash : {false, true}) {
            QTest::newRow(wash ? "texture-only-wash" : "texture-only-buildup")
                << 300 << wash << false << false << false << false << false << true;
        }
    }
    void testTexturedMaskedStroke()
    {
        QFETCH(bool, masked);
        QFETCH(bool, textured);
        runStroke(false, false, -1, QString(), masked, textured);
    }
    void testMaskingCompositeMatchesCpu_data()
    {
        QTest::addColumn<QString>("mode");
        for (const QString &mode : {QString(COMPOSITE_MULT),
                                    QString(COMPOSITE_DARKEN),
                                    QString(COMPOSITE_OVERLAY),
                                    QString(COMPOSITE_DODGE),
                                    QString(COMPOSITE_BURN),
                                    QString(COMPOSITE_LINEAR_BURN),
                                    QString(COMPOSITE_LINEAR_DODGE),
                                    QString(COMPOSITE_HARD_MIX_PHOTOSHOP),
                                    QString(COMPOSITE_HARD_MIX_SOFTER_PHOTOSHOP),
                                    QString(COMPOSITE_SUBTRACT)})
            QTest::newRow(qPrintable(mode)) << mode;
    }
    /// The GPU masking composite (KisGpuMaskingWorker) is bit-identical to the
    /// CPU one (KisMaskingBrushCompositeOp<float>) for every mode, and leaves
    /// the pixels outside the rect alone.
    void testMaskingCompositeMatchesCpu()
    {
        QFETCH(QString, mode);
        const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(),
                                                                              Float32BitsColorDepthID.id(),
                                                                              QString());
        const KoColorSpace *grayA = KoColorSpaceRegistry::instance()->colorSpace(GrayAColorModelID.id(),
                                                                                 Integer8BitsColorDepthID.id(),
                                                                                 QString());
        QVERIFY(KisGpuMaskingWorker::supports(cs, mode) || !KisGpuBrushPainter::isEnabled());
        const QRect area(10, 20, 150, 90); // spans tiles
        const QRect rect(25, 40, 100, 60);
        // alphas at the modes' edges (0, 0.5, 1, the fuzzy thresholds) and
        // in between; masks over the whole byte range
        const float alphas[] = {0.0f, 1.0f, 0.5f, 0.00001f, 0.000011f, 0.99999f, 0.5000001f, 0.25f, 0.75f, 0.33f};
        std::vector<float> stroke(size_t(area.width()) * area.height() * 4);
        std::vector<quint8> mask(size_t(area.width()) * area.height() * 2);
        quint32 seed = 7;
        auto next = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 8;
        };
        for (size_t i = 0; i < stroke.size() / 4; ++i) {
            for (int c = 0; c < 3; ++c)
                stroke[4 * i + c] = float(next() % 1000) / 999.0f;
            stroke[4 * i + 3] = i % 3 == 0 ? alphas[next() % 10] : float(next() % 100001) / 100000.0f;
            mask[2 * i] = quint8(i % 7 == 0 ? (next() % 2) * 255 : next() % 256);
            mask[2 * i + 1] = quint8(i % 5 == 0 ? 255 : next() % 256);
        }
        std::vector<float> background(stroke.size());
        for (size_t i = 0; i < background.size(); ++i)
            background[i] = 0.25f + float(i % 4) * 0.1f;

        KisPaintDeviceSP strokeDevice = new KisPaintDevice(cs);
        KisPaintDeviceSP maskDevice = new KisPaintDevice(grayA);
        KisPaintDeviceSP dstDevice = new KisPaintDevice(cs);
        strokeDevice->writeBytes(reinterpret_cast<const quint8 *>(stroke.data()), area);
        maskDevice->writeBytes(mask.data(), area);
        dstDevice->writeBytes(reinterpret_cast<const quint8 *>(background.data()), area);

        // the CPU reference: the stroke copied into the rect, then the
        // composite on its alpha
        std::vector<float> expected = background;
        QScopedPointer<KisMaskingBrushCompositeOpBase> op(
            KisMaskingBrushCompositeOpFactory::create(mode, KoChannelInfo::FLOAT32, 16, 12));
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            const size_t row = size_t(y - area.top()) * area.width();
            const size_t first = row + size_t(rect.left() - area.left());
            std::copy(stroke.begin() + 4 * first,
                      stroke.begin() + 4 * (first + rect.width()),
                      expected.begin() + 4 * first);
            op->composite(mask.data() + 2 * first,
                          0,
                          reinterpret_cast<quint8 *>(expected.data() + 4 * first),
                          0,
                          rect.width(),
                          1);
        }

        QVERIFY(KisGpuMaskingWorker::apply(strokeDevice, maskDevice, dstDevice, rect, mode));
        std::vector<float> actual(background.size());
        dstDevice->readBytes(reinterpret_cast<quint8 *>(actual.data()), area);
        int mismatches = 0;
        for (size_t i = 0; i < actual.size(); ++i) {
            if (std::memcmp(&actual[i], &expected[i], sizeof(float)) != 0) {
                if (mismatches++ < 5)
                    qWarning() << "pixel" << i / 4 << "channel" << i % 4 << "GPU" << actual[i] << "CPU" << expected[i]
                               << "stroke alpha" << stroke[(i / 4) * 4 + 3] << "mask" << mask[(i / 4) * 2]
                               << mask[(i / 4) * 2 + 1];
            }
        }
        QCOMPARE(mismatches, 0);
    }

private:
    QString m_maskGenerator = QStringLiteral("default");
    // Texture settings of textured strokes (testTextureModes); -1: Multiply.
    int m_textureMode = -1;
    bool m_softTexturing = false;
    qreal m_textureStrength = 1.0;
    int m_refusePendingBatches = 0;
    void runStroke(bool erase,
                   bool selectionOnly = false,
                   int channelBits = -1,
                   const QString &modeOverride = QString(),
                   bool masked = false,
                   bool textured = false,
                   bool half = false)
    {
        QFETCH(int, diameter);
        QFETCH(bool, wash);
        QFETCH(bool, mirrors);
        QFETCH(bool, restricted);
        QFETCH(bool, distant);
        QFETCH(bool, alphaOnly);
        const auto previousBrush = qgetenv("KRITA_GPU_BRUSH");
        const bool previousProjection = KisGpuMergeBatch::isEnabled();
        // Each batch paints its dabs and then their reflections, so with
        // mirrors and a non-commutative blend mode the result depends on how
        // the stroke is split into batches. Give both paths the CPU period.
        if (mirrors && !modeOverride.isEmpty())
            KisBrushOp::setGpuMinimumUpdatePeriodForTesting(10);
        const auto restore = qScopeGuard([&]() {
            KisBrushOp::setGpuMinimumUpdatePeriodForTesting(-2);
            if (previousBrush.isNull())
                qunsetenv("KRITA_GPU_BRUSH");
            else
                qputenv("KRITA_GPU_BRUSH", previousBrush);
            KisGpuMergeBatch::setEnabled(previousProjection);
        });
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(),
                                                                      half ? Float16BitsColorDepthID.id()
                                                                           : Float32BitsColorDepthID.id(),
                                                                      QString());
        QVERIFY(cs);
        auto localResources = toQShared(new KisLocalStrokeResources());
        KisPaintOpSettingsSP settings = new KisBrushOpSettings(localResources);
        settings->setProperty("paintop", "paintbrush");
        settings->setProperty(
            "brush_definition",
            QStringLiteral(
                "<Brush useAutoSpacing=\"0\" angle=\"0.37\" spacing=\"%2\" density=\"1\" BrushVersion=\"2\" "
                "type=\"auto_brush\" randomness=\"0\"><MaskGenerator spikes=\"2\" hfade=\"0.7\" ratio=\"0.6\" "
                "diameter=\"%1\" id=\"%3\" type=\"circle\" antialiasEdges=\"1\" vfade=\"0.6\" "
                "softness_curve=\"0,1;0.3,0.85;0.7,0.2;1,0;\"/></Brush>")
                .arg(diameter)
                .arg(diameter == 300 ? 0.02 : 0.1)
                .arg(m_maskGenerator));
        KisPaintingModeOptionData paintingMode;
        paintingMode.paintingMode = wash ? enumPaintingMode::WASH : enumPaintingMode::BUILDUP;
        paintingMode.write(settings.data());
        KisOpacityOptionData opacity;
        opacity.useCurve = false;
        opacity.strengthValue = 0.63;
        opacity.write(settings.data());
        KisFlowOptionData flow;
        flow.useCurve = false;
        flow.strengthValue = 0.47;
        flow.write(settings.data());
        if (masked) {
            settings->setProperty("MaskingBrush/Enabled", true);
            settings->setProperty("MaskingBrush/MaskingCompositeOp", COMPOSITE_MULT);
            settings->setProperty("MaskingBrush/UseMasterSize", false);
            settings->setProperty("MaskingBrush/Preset/paintop", "paintbrush");
            settings->setProperty("MaskingBrush/Preset/brush_definition", settings->getString("brush_definition"));
        }
        if (textured) {
            QImage grain(64, 64, QImage::Format_RGB32);
            for (int y = 0; y < grain.height(); ++y) {
                for (int x = 0; x < grain.width(); ++x) {
                    const int value = (x * 73 + y * 151 + x * y * 13) % 256;
                    grain.setPixel(x, y, qRgb(value, value, value));
                }
            }
            KoPatternSP pattern(new KoPattern(grain, "GPU test grain", "gpu-test-grain.pat"));
            localResources->addResource(pattern);
            KisTextureOptionData texture;
            texture.isEnabled = true;
            texture.textureData = KisEmbeddedTextureData::fromPattern(pattern);
            if (m_textureMode >= 0) {
                texture.texturingMode = KisTextureOptionData::TexturingMode(m_textureMode);
                texture.useSoftTexturing = m_softTexturing;
                texture.offsetX = 5;
                texture.offsetY = 11;
            }
            texture.write(settings.data());
            if (m_textureStrength != 1.0) {
                KisStrengthOptionData strength;
                strength.useCurve = false;
                strength.strengthValue = m_textureStrength;
                strength.write(settings.data());
            }
        }
        KisPaintOpPresetSP preset(new KisPaintOpPreset());
        preset->setSettings(settings);
        QCOMPARE(preset->hasMaskingPreset(), masked);

        const int repeats = qBound(1, qEnvironmentVariableIntValue("KRITA_GPU_STROKE_REPEATS"), 20);
        std::vector<float> referenceLayer, referenceProjection;
        // Alternate order to reduce systematic CPU-first/GPU-last timing bias.
        std::vector<double> times[3];
        std::vector<quint64> submissions[3];
        for (int iteration = 0; iteration <= repeats; ++iteration) {
            for (int order = 0; order < 3; ++order) {
                const int path = iteration % 2 ? 2 - order : order;
                KisGpuMergeBatch::setEnabled(path != 0);
                qputenv("KRITA_GPU_BRUSH", path == 2 ? "1" : "0");
                auto *undo = new KisSurrogateUndoStore;
                KisImageSP image = new KisImage(undo, 1024, 1024, cs, "GPU stroke test");
                image->setWorkingThreadsLimit(4);
                KisPaintLayerSP layer;
                for (int i = 0; i < 4; ++i) {
                    layer = new KisPaintLayer(image, "paint", OPACITY_OPAQUE_U8, cs);
                    KoColor background(cs);
                    cs->fromNormalisedChannelsValue(background.data(), {0.1f + 0.1f * i, 0.15f, 0.3f, 0.4f});
                    layer->paintDevice()->fill(imageRect, background);
                    image->addNode(layer, image->root());
                }
                image->initialRefreshGraph();
                image->waitForDone();
                undo->clear();
                const auto before = pixels(layer->paintDevice());
                if (restricted || alphaOnly)
                    layer->setAlphaLocked(true);
                if (channelBits >= 0) {
                    QBitArray flags(4);
                    for (int i = 0; i < 4; ++i)
                        flags.setBit(i, channelBits & (1 << i));
                    layer->setChannelLockFlags(flags);
                }
                KoCanvasResourceProvider canvasResources;
                KisResourcesSnapshotSP resources;
                if (erase || !modeOverride.isEmpty()) {
                    const QString mode = erase ? COMPOSITE_ERASE : modeOverride;
                    canvasResources.setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));
                    canvasResources.setResource(KoCanvasResource::CurrentEffectiveCompositeOp, mode);
                    canvasResources.setResource(KoCanvasResource::Opacity, 1.0);
                    canvasResources.setResource(KoCanvasResource::ForegroundColor,
                                                QVariant::fromValue(KoColor(Qt::black, cs)));
                    canvasResources.setResource(KoCanvasResource::BackgroundColor,
                                                QVariant::fromValue(KoColor(Qt::white, cs)));
                    resources = new KisResourcesSnapshot(image, layer, &canvasResources);
                    QCOMPARE(resources->compositeOpId(), mode);
                } else {
                    resources = new KisResourcesSnapshot(image, layer);
                }
                resources->setBrush(preset);
                resources->setFGColorOverride(KoColor(QColor(70, 180, 240), cs));
                resources->setBGColorOverride(KoColor(Qt::white, cs));
                resources->setMirroring(mirrors, mirrors);
                if (restricted || selectionOnly) {
                    KisSelectionSP selection = new KisSelection();
                    std::vector<quint8> mask(512 * 512);
                    for (int y = 0; y < 512; ++y)
                        for (int x = 0; x < 512; ++x)
                            mask[y * 512 + x] = quint8((x + y) % 256);
                    selection->pixelSelection()->writeBytes(mask.data(), QRect(256, 256, 512, 512));
                    resources->setSelectionOverride(selection);
                }
                QCOMPARE(resources->needsIndirectPainting(), wash);
                auto &context = KisGpuTileBackend::instance()->context();
                context.waitIdle();
                const auto firstSubmission = context.completedValue();
                const auto firstComposite = KisGpuMergeBatch::gpuCompositeCount();
                const auto firstPreview = KisGpuBrushPainter::washPreviewCount();
                const auto firstBaseCopy = KisGpuBrushPainter::washBaseCopyCount();
                const auto firstMerge = KisGpuBrushPainter::washMergeCount();
                const auto firstBatch = KisGpuBrushPainter::batchCount();
                const auto firstGenerated = KisGpuBrushPainter::generatedDabCount();
                const auto firstSkipped = KisDabRenderingJobRunner::skippedGenerationCount();
                const auto firstTexturedDescriptions = KisDabRenderingJobRunner::texturedDescriptionCount();
                const auto firstMaterialized = KisBrushOp::materializedDabCount();
                const auto firstMaskingRuns = KisGpuMaskingWorker::runCount();
                KisGpuBrushPainter::refusePendingBatchesForTesting(path == 2 ? m_refusePendingBatches : 0);
                QElapsedTimer timer;
                timer.start();
                const auto stroke = image->startStroke(new FreehandStrokeStrategy(resources,
                                                                                  new KisFreehandStrokeInfo(),
                                                                                  kundo2_noi18n("GPU test stroke")));
                QPointF previous = distant ? QPointF(100, 180) : QPointF(300, 400);
                for (int segment = 0; segment < 24; ++segment) {
                    const QPointF next = distant
                        ? QPointF(100 + (segment + 1) * 8, 180 + 80 * std::sin((segment + 1) * 0.4))
                        : QPointF(300 + (segment + 1) * 17, 400 + 100 * std::sin((segment + 1) * 0.4));
                    image->addJob(stroke,
                                  new FreehandStrokeStrategy::Data(0,
                                                                   KisPaintInformation(previous, 0.8),
                                                                   KisPaintInformation(next, 0.8)));
                    previous = next;
                }
                image->addJob(stroke, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
                image->endStroke(stroke);
                image->waitForDone();
                // Projection jobs can finish before their Vulkan submissions.
                // Include actual GPU completion in the timed interval.
                if (path)
                    context.waitIdle();
                const double elapsed = timer.nsecsElapsed() / 1e6;
                if (iteration) {
                    times[path].push_back(elapsed);
                    submissions[path].push_back(context.completedValue() - firstSubmission);
                }
                if (path)
                    QVERIFY(KisGpuMergeBatch::gpuCompositeCount() > firstComposite);
                else
                    QCOMPARE(KisGpuMergeBatch::gpuCompositeCount(), firstComposite);
                // A stroke with a masking brush uses the GPU brush when its
                // masking composite runs on the GPU too (RGBA32F).
                const bool gpuBrush = path == 2 && !(masked && half);
                const auto maskingRuns = KisGpuMaskingWorker::runCount() - firstMaskingRuns;
                if (path == 2 && masked && !half)
                    QVERIFY(maskingRuns > 0);
                else
                    QCOMPARE(maskingRuns, quint64(0));
                const auto batches = KisGpuBrushPainter::batchCount() - firstBatch;
                if (gpuBrush && wash)
                    QVERIFY(KisGpuBrushPainter::washPreviewCount() > firstPreview);
                else
                    QCOMPARE(KisGpuBrushPainter::washPreviewCount(), firstPreview);
                // Phase 4.90: Wash previews copy the layer original on the GPU.
                if (gpuBrush && wash)
                    QVERIFY(KisGpuBrushPainter::washBaseCopyCount() > firstBaseCopy);
                else
                    QCOMPARE(KisGpuBrushPainter::washBaseCopyCount(), firstBaseCopy);
                if (gpuBrush && wash)
                    QVERIFY(KisGpuBrushPainter::washMergeCount() > firstMerge);
                else
                    QCOMPARE(KisGpuBrushPainter::washMergeCount(), firstMerge);
                // Refused batches (testRefusedPendingBatches) may be all of them.
                if (gpuBrush && !m_refusePendingBatches)
                    QVERIFY(batches > 0);
                else if (!gpuBrush)
                    QCOMPARE(batches, quint64(0));
                // Phase 4.83: the circle dabs of RGBA32F strokes are evaluated
                // on the GPU. "Textured dabs": RGBA32F dabs with an alpha-only
                // texture too; Lightness/Gradient and RGBA16F textures keep
                // their pixels.
                const bool gpuTexture = textured && !half && m_textureMode != KisTextureOptionData::LIGHTNESS
                    && m_textureMode != KisTextureOptionData::GRADIENT;
                const bool describable = !textured || gpuTexture;
                const auto generated = KisGpuBrushPainter::generatedDabCount() - firstGenerated;
                // Phase 4.86: RGBA16F dabs are generated too.
                if (gpuBrush && describable && !m_refusePendingBatches)
                    QVERIFY(generated > 0);
                else if (!gpuBrush || !describable)
                    QCOMPARE(generated, quint64(0));
                // Phase 4.85: after the first verified dabs of each mask kind,
                // their CPU generation is skipped; every CPU use materializes them.
                const auto skipped = KisDabRenderingJobRunner::skippedGenerationCount() - firstSkipped;
                if (gpuBrush && describable)
                    QVERIFY(skipped > 0);
                else
                    QCOMPARE(skipped, quint64(0));
                const auto texturedDescriptions =
                    KisDabRenderingJobRunner::texturedDescriptionCount() - firstTexturedDescriptions;
                if (gpuBrush && gpuTexture)
                    QVERIFY(texturedDescriptions > 0);
                else
                    QCOMPARE(texturedDescriptions, quint64(0));
                const auto materialized = KisBrushOp::materializedDabCount() - firstMaterialized;
                if (gpuBrush && describable && m_refusePendingBatches)
                    QVERIFY(materialized > 0);
                KisGpuBrushPainter::refusePendingBatchesForTesting(0);
                const auto after = pixels(layer->paintDevice());
                const auto projection = pixels(image->projection());
                QVERIFY(difference(before, after) > 0.01f);
                if (iteration == 0 && path == 0) {
                    referenceLayer = after;
                    referenceProjection = projection;
                } else {
                    const float error = difference(referenceLayer, after);
                    QVERIFY2(error <= (half ? 0.002f : 2e-5f),
                             qPrintable(QString("layer path %1: %2").arg(path).arg(error)));
                    const float projectionError = difference(referenceProjection, projection);
                    QVERIFY2(projectionError <= (half ? 0.004f : 2e-5f),
                             qPrintable(QString("projection path %1: %2").arg(path).arg(projectionError)));
                }
                if (!iteration) {
                    QVERIFY(undo->presentCommand());
                    undo->undo();
                    image->waitForDone();
                    QCOMPARE(pixels(layer->paintDevice()), before);
                    undo->redo();
                    image->waitForDone();
                    QCOMPARE(pixels(layer->paintDevice()), after);
                    QVERIFY(difference(pixels(image->projection()), projection) <= (half ? 0.004f : 2e-5f));
                }
            }
        }
        for (int path = 0; path < 3; ++path) {
            auto &samples = times[path];
            auto &counts = submissions[path];
            std::sort(samples.begin(), samples.end());
            std::sort(counts.begin(), counts.end());
            const double median = (samples[(samples.size() - 1) / 2] + samples[samples.size() / 2]) / 2;
            qInfo() << "Completed stroke ms, path" << path << "(0 CPU, 1 GPU projection, 2 GPU projection+brush)"
                    << "samples" << samples.size() << "median" << median << "min" << samples.front() << "max"
                    << samples.back() << "submissions median" << counts[counts.size() / 2]
                    << "4 image workers; no canvas; untimed verification readback";
        }
    }
};
SIMPLE_TEST_MAIN(KisGpuStrokeTest)
#include "KisGpuStrokeTest.moc"
