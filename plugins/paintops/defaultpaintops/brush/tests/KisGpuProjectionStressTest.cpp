/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../KisBrushOpSettings.h"
#include "../kis_brushop.h"
#include "../kis_brushop_settings_widget.h"
#include <KisAsynchronousStrokeUpdateHelper.h>
#include <KisGpuContext.h>
#include <KisLocalStrokeResources.h>
#include <KisLodPreferences.h>
#include <KisPaintingModeOptionData.h>
#include <KisStandardOptionData.h>
#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QThread>
#include <atomic>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <cmath>
#include <gpu/KisGpuBrushPainter.h>
#include <gpu/KisGpuMergeBatch.h>
#include <gpu/KisGpuTileBackend.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_pixel_selection.h>
#include <kis_resources_snapshot.h>
#include <kis_selection.h>
#include <kis_simple_paintop_factory.h>
#include <kis_transparency_mask.h>
#include <kis_undo_stores.h>
#include <simpletest.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/freehand_stroke.h>
#include <vector>

/**
 * Reproduction test for a rare canvas artifact reported on 2026-11-11: while
 * painting with the GPU engine, a rectangle (the shape of an update rect, not
 * of the 64 px tile grid) sometimes shows plain white instead of the layers;
 * Undo and Redo repaint it correctly.
 *
 * The document imitates the reported layer stack: a white background, a
 * pass-through group holding a paint layer, and above it a 39% Multiply layer
 * with inherited alpha and a transparency mask. Many short strokes paint the
 * mask and the layers while projection updates run concurrently on all
 * worker threads. After each stroke the image's projection is compared with
 * a full refresh of the graph. The refresh recomposites everything, like the
 * Undo/Redo that removes the artifact, so a difference is a merge result that
 * the incremental updates lost.
 *
 * KRITA_GPU_STRESS_STROKES (default 60) sets the number of strokes per row.
 * This test has no canvas: an artifact of the canvas upload alone does not
 * show here.
 */
namespace
{
const QRect imageRect(0, 0, 1024, 1024);

/// Counts the strokes that got an instant preview (level of detail) clone.
class CountingFreehandStroke : public FreehandStrokeStrategy
{
public:
    using FreehandStrokeStrategy::FreehandStrokeStrategy;
    static std::atomic<int> lodClones;
    KisStrokeStrategy *createLodClone(int levelOfDetail) override
    {
        KisStrokeStrategy *clone = FreehandStrokeStrategy::createLodClone(levelOfDetail);
        if (clone)
            ++lodClones;
        return clone;
    }
};
std::atomic<int> CountingFreehandStroke::lodClones{0};

std::vector<float> pixels(KisPaintDeviceSP device)
{
    const KoColorSpace *f32 =
        KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(), QString());
    std::vector<float> result(size_t(imageRect.width()) * imageRect.height() * 4);
    QByteArray raw(imageRect.width() * imageRect.height() * device->pixelSize(), Qt::Uninitialized);
    device->readBytes(reinterpret_cast<quint8 *>(raw.data()), imageRect);
    device->colorSpace()->convertPixelsTo(reinterpret_cast<const quint8 *>(raw.constData()),
                                          reinterpret_cast<quint8 *>(result.data()),
                                          f32,
                                          imageRect.width() * imageRect.height(),
                                          KoColorConversionTransformation::internalRenderingIntent(),
                                          KoColorConversionTransformation::internalConversionFlags());
    return result;
}

/// Bounding rect of the pixels whose channels differ by more than @p tolerance.
QRect differingRect(const std::vector<float> &a, const std::vector<float> &b, float tolerance, float *worst)
{
    QRect result;
    *worst = 0;
    for (int y = 0; y < imageRect.height(); ++y) {
        for (int x = 0; x < imageRect.width(); ++x) {
            const size_t i = (size_t(y) * imageRect.width() + x) * 4;
            float error = 0;
            for (int c = 0; c < 4; ++c) {
                const float d = std::abs(a[i + c] - b[i + c]);
                error = std::isfinite(d) ? qMax(error, d) : INFINITY;
            }
            if (error > tolerance) {
                result |= QRect(x, y, 1, 1);
                *worst = qMax(*worst, error);
            }
        }
    }
    return result;
}

QString pixelString(const std::vector<float> &p, const QPoint &pt)
{
    const size_t i = (size_t(pt.y()) * imageRect.width() + pt.x()) * 4;
    return QString("(%1 %2 %3 %4)").arg(p[i]).arg(p[i + 1]).arg(p[i + 2]).arg(p[i + 3]);
}
} // namespace

class KisGpuProjectionStressTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        if (!KisGpuTileBackend::instance())
            QSKIP("No GPU backend");
        QVERIFY(KisPaintOpRegistry::instance()->contains("paintbrush"));
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
        if (auto *backend = KisGpuTileBackend::existingInstance()) {
            backend->flush();
            QCOMPARE(backend->context().validationErrorCount(), 0);
        }
    }

    void testStrokesKeepProjection_data()
    {
        QTest::addColumn<bool>("half");
        QTest::addColumn<bool>("passThrough");
        QTest::addColumn<bool>("gpuBrush");
        QTest::addColumn<bool>("whiteCanvas");
        QTest::addColumn<int>("lod");
        for (bool half : {false, true}) {
            for (bool passThrough : {true, false}) {
                for (bool gpuBrush : {true, false}) {
                    QTest::newRow(qPrintable(QString("%1-%2-%3")
                                                 .arg(half ? "f16" : "f32")
                                                 .arg(passThrough ? "passthrough" : "group")
                                                 .arg(gpuBrush ? "gpubrush" : "cpubrush")))
                        << half << passThrough << gpuBrush << true << 0;
                }
            }
        }
        QTest::newRow("f32-passthrough-gpubrush-transparent-canvas") << false << true << true << false << 0;
        // Instant preview: each stroke runs at a level of detail first, then
        // at full resolution.
        for (int lod : {1, 2}) {
            for (bool half : {false, true}) {
                for (bool gpuBrush : {true, false}) {
                    QTest::newRow(qPrintable(QString("%1-passthrough-%2-lod%3")
                                                 .arg(half ? "f16" : "f32")
                                                 .arg(gpuBrush ? "gpubrush" : "cpubrush")
                                                 .arg(lod)))
                        << half << true << gpuBrush << true << lod;
                }
            }
        }
    }

    void testStrokesKeepProjection()
    {
        QFETCH(bool, half);
        QFETCH(bool, passThrough);
        QFETCH(bool, gpuBrush);
        QFETCH(bool, whiteCanvas);
        QFETCH(int, lod);

        const QByteArray previousBrush = qgetenv("KRITA_GPU_BRUSH");
        const bool previousProjection = KisGpuMergeBatch::isEnabled();
        const auto restore = qScopeGuard([&]() {
            if (previousBrush.isNull())
                qunsetenv("KRITA_GPU_BRUSH");
            else
                qputenv("KRITA_GPU_BRUSH", previousBrush);
            KisGpuMergeBatch::setEnabled(previousProjection);
        });
        KisGpuMergeBatch::setEnabled(true);
        qputenv("KRITA_GPU_BRUSH", gpuBrush ? "1" : "0");

        const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(),
                                                                              half ? Float16BitsColorDepthID.id()
                                                                                   : Float32BitsColorDepthID.id(),
                                                                              QString());
        QVERIFY(cs);

        auto *undo = new KisSurrogateUndoStore;
        KisImageSP image = new KisImage(undo, imageRect.width(), imageRect.height(), cs, "GPU projection stress");
        image->setWorkingThreadsLimit(qMax(4, QThread::idealThreadCount()));
        if (whiteCanvas)
            image->setDefaultProjectionColor(KoColor(Qt::white, cs));

        auto filled = [&](const QString &name, const QColor &color, const QRect &rect) {
            KisPaintLayerSP layer = new KisPaintLayer(image, name, OPACITY_OPAQUE_U8, cs);
            layer->paintDevice()->fill(rect, KoColor(color, cs));
            return layer;
        };
        // White background, a gray figure below the group, and the group with
        // a brown "pouch" layer and a Multiply shading layer with a mask.
        KisPaintLayerSP background = filled("background", Qt::white, imageRect);
        image->addNode(background, image->root());
        KisPaintLayerSP figure = filled("figure", QColor(70, 68, 74), QRect(200, 100, 500, 800));
        image->addNode(figure, image->root());
        KisGroupLayerSP group = new KisGroupLayer(image, "Group 44", OPACITY_OPAQUE_U8);
        group->setPassThroughMode(passThrough);
        image->addNode(group, image->root());
        KisPaintLayerSP pouch = filled("pouch", QColor(70, 35, 30), QRect(330, 250, 300, 500));
        image->addNode(pouch, group);
        KisPaintLayerSP shading = filled("Paint Layer 60", QColor(90, 90, 160), QRect(150, 50, 700, 900));
        shading->setOpacity(quint8(0.39 * 255));
        shading->setCompositeOpId(COMPOSITE_MULT);
        shading->disableAlphaChannel(true);
        image->addNode(shading, group);
        KisTransparencyMaskSP mask = new KisTransparencyMask(image, "Transparency Mask 1");
        mask->initSelection(KisLayerSP(shading));
        {
            std::vector<quint8> alpha(size_t(imageRect.width()) * imageRect.height());
            for (int y = 0; y < imageRect.height(); ++y)
                for (int x = 0; x < imageRect.width(); ++x)
                    alpha[size_t(y) * imageRect.width() + x] = quint8((x * 3 + y) % 256);
            mask->selection()->pixelSelection()->writeBytes(alpha.data(), imageRect);
        }
        image->addNode(mask, shading);
        KisPaintLayerSP above = filled("above", QColor(200, 200, 200, 0), QRect(0, 0, 64, 64));
        image->addNode(above, group);
        image->initialRefreshGraph();
        image->waitForDone();
        undo->clear();
        if (lod)
            image->setLodPreferences(KisLodPreferences(lod));
        const int firstLodClone = CountingFreehandStroke::lodClones;

        const QList<KisNodeSP> targets = {KisNodeSP(mask),
                                          KisNodeSP(shading),
                                          KisNodeSP(pouch),
                                          KisNodeSP(above),
                                          KisNodeSP(figure)};
        const int strokes = qEnvironmentVariableIsSet("KRITA_GPU_STRESS_STROKES")
            ? qMax(1, qEnvironmentVariableIntValue("KRITA_GPU_STRESS_STROKES"))
            : 60;
        QRandomGenerator random(20261111);
        const quint64 firstComposite = KisGpuMergeBatch::gpuCompositeCount();
        const quint64 firstBatch = KisGpuBrushPainter::batchCount();
        int failures = 0;

        for (int s = 0; s < strokes; ++s) {
            KisNodeSP target = targets[random.bounded(targets.size())];
            const bool wash = random.bounded(2);
            const int diameter = lod ? 100 + random.bounded(200) : 10 + random.bounded(140);

            auto localResources = toQShared(new KisLocalStrokeResources());
            KisPaintOpSettingsSP settings = new KisBrushOpSettings(localResources);
            settings->setProperty("paintop", "paintbrush");
            settings->setProperty(
                "brush_definition",
                QStringLiteral("<Brush useAutoSpacing=\"0\" angle=\"0\" spacing=\"0.08\" density=\"1\" "
                               "BrushVersion=\"2\" type=\"auto_brush\" randomness=\"0\"><MaskGenerator spikes=\"2\" "
                               "hfade=\"0.5\" ratio=\"1\" diameter=\"%1\" id=\"default\" type=\"circle\" "
                               "antialiasEdges=\"1\" vfade=\"0.5\"/></Brush>")
                    .arg(diameter));
            KisPaintingModeOptionData paintingMode;
            paintingMode.paintingMode = wash ? enumPaintingMode::WASH : enumPaintingMode::BUILDUP;
            paintingMode.write(settings.data());
            KisOpacityOptionData opacity;
            opacity.useCurve = false;
            opacity.strengthValue = 0.8;
            opacity.write(settings.data());
            KisPaintOpPresetSP preset(new KisPaintOpPreset());
            preset->setSettings(settings);

            KisResourcesSnapshotSP resources;
            if (lod) {
                // Only this constructor reads the preset's LOD availability.
                KoCanvasResourceProvider canvasResources;
                canvasResources.setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));
                canvasResources.setResource(KoCanvasResource::CurrentEffectiveCompositeOp, COMPOSITE_OVER);
                canvasResources.setResource(KoCanvasResource::Opacity, 1.0);
                canvasResources.setResource(KoCanvasResource::EffectiveLodAvailability, true);
                canvasResources.setResource(KoCanvasResource::ForegroundColor,
                                            QVariant::fromValue(KoColor(Qt::black, cs)));
                canvasResources.setResource(KoCanvasResource::BackgroundColor,
                                            QVariant::fromValue(KoColor(Qt::white, cs)));
                resources = new KisResourcesSnapshot(image, target, &canvasResources);
                QVERIFY(resources->presetAllowsLod());
            } else {
                resources = new KisResourcesSnapshot(image, target);
            }
            resources->setBrush(preset);
            const QColor color =
                QColor::fromHsv(random.bounded(360), 120 + random.bounded(100), 60 + random.bounded(150));
            resources->setFGColorOverride(KoColor(target == KisNodeSP(mask) ? QColor(Qt::white) : color, cs));
            resources->setBGColorOverride(KoColor(Qt::black, cs));

            const auto stroke = image->startStroke(
                new CountingFreehandStroke(resources, new KisFreehandStrokeInfo(), kundo2_noi18n("stress stroke")));
            QPointF previous(100 + random.bounded(800), 100 + random.bounded(800));
            const int segments = 6 + random.bounded(30);
            for (int i = 0; i < segments; ++i) {
                const QPointF next = previous + QPointF(random.bounded(61) - 30, random.bounded(21) - 10);
                image->addJob(
                    stroke,
                    new FreehandStrokeStrategy::Data(0,
                                                     KisPaintInformation(previous, 0.3 + 0.7 * random.generateDouble()),
                                                     KisPaintInformation(next, 0.3 + 0.7 * random.generateDouble())));
                // Updates between segments, as the canvas timer requests them:
                // their merges run while the next segments are painted.
                image->addJob(stroke, new KisAsynchronousStrokeUpdateHelper::UpdateData(false));
                previous = next;
            }
            image->addJob(stroke, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
            image->endStroke(stroke);
            image->waitForDone();

            const std::vector<float> incremental = pixels(image->projection());
            image->refreshGraphAsync();
            image->waitForDone();
            const std::vector<float> refreshed = pixels(image->projection());

            float worst = 0;
            const QRect bad = differingRect(incremental, refreshed, half ? 0.004f : 1e-4f, &worst);
            if (!bad.isEmpty()) {
                ++failures;
                const QPoint center = bad.center();
                qWarning().noquote() << QString(
                                            "stroke %1 on %2 (%3, diameter %4): projection differs from a full "
                                            "refresh in %5,%6 %7x%8 (tile aligned: %9), worst %10; at center "
                                            "incremental %11 refreshed %12")
                                            .arg(s)
                                            .arg(target->name())
                                            .arg(wash ? "wash" : "buildup")
                                            .arg(diameter)
                                            .arg(bad.x())
                                            .arg(bad.y())
                                            .arg(bad.width())
                                            .arg(bad.height())
                                            .arg(KisGpuMergeBatch::tileAligned(bad) == bad ? "yes" : "no")
                                            .arg(worst)
                                            .arg(pixelString(incremental, center))
                                            .arg(pixelString(refreshed, center));
            }
        }

        qInfo() << "strokes" << strokes << "GPU composites" << KisGpuMergeBatch::gpuCompositeCount() - firstComposite
                << "GPU brush batches" << KisGpuBrushPainter::batchCount() - firstBatch << "LOD strokes"
                << CountingFreehandStroke::lodClones - firstLodClone << "failures" << failures;
        QVERIFY(KisGpuMergeBatch::gpuCompositeCount() > firstComposite);
        if (lod)
            QVERIFY(CountingFreehandStroke::lodClones > firstLodClone);
        else
            QCOMPARE(int(CountingFreehandStroke::lodClones), firstLodClone);
        QVERIFY2(failures == 0,
                 qPrintable(QString("%1 of %2 strokes left a stale projection").arg(failures).arg(strokes)));
    }
};

SIMPLE_TEST_MAIN(KisGpuProjectionStressTest)
#include "KisGpuProjectionStressTest.moc"
