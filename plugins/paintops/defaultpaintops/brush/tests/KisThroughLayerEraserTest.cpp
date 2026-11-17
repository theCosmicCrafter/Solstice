/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../KisBrushOpSettings.h"
#include "../kis_brushop.h"
#include "../kis_brushop_settings_widget.h"
#include <KisAsynchronousStrokeUpdateHelper.h>
#include <KisBrushStrokeLayer.h>
#include <KisLocalStrokeResources.h>
#include <KisPaintingModeOptionData.h>
#include <KisStandardOptionData.h>
#include <KisThroughLayerEraserTargets.h>
#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <QScopeGuard>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <commands/kis_set_global_selection_command.h>
#include <kis_canvas_resource_provider.h>
#include <kis_group_layer.h>
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
#include <strokes/freehand_stroke.h>

#ifdef HAVE_KRITA_GPU_ENGINE
#include <KisGpuContext.h>
#include <gpu/KisGpuMergeBatch.h>
#include <gpu/KisGpuTileBackend.h>
#endif

namespace
{
const QRect imageRect(0, 0, 256, 256);

QVector<qreal> alphas(KisPaintDeviceSP device, const QRect &rect = imageRect)
{
    QVector<qreal> result(rect.width() * rect.height());
    const int pixelSize = device->pixelSize();
    QByteArray raw(rect.width() * rect.height() * pixelSize, Qt::Uninitialized);
    device->readBytes(reinterpret_cast<quint8 *>(raw.data()), rect);
    for (int i = 0; i < result.size(); ++i) {
        result[i] = device->colorSpace()->opacityF(reinterpret_cast<const quint8 *>(raw.constData()) + i * pixelSize);
    }
    return result;
}

qreal largestDifference(const QVector<qreal> &a, const QVector<qreal> &b)
{
    qreal result = 0;
    for (int i = 0; i < a.size(); ++i) {
        result = qMax(result, qAbs(a[i] - b[i]));
    }
    return result;
}

KisPaintOpPresetSP createPreset(bool wash)
{
    auto localResources = toQShared(new KisLocalStrokeResources());
    KisPaintOpSettingsSP settings = new KisBrushOpSettings(localResources);
    settings->setProperty("paintop", "paintbrush");
    settings->setProperty(
        "brush_definition",
        QStringLiteral("<Brush useAutoSpacing=\"0\" angle=\"0\" spacing=\"0.1\" density=\"1\" BrushVersion=\"2\" "
                       "type=\"auto_brush\" randomness=\"0\"><MaskGenerator spikes=\"2\" hfade=\"0.5\" ratio=\"1\" "
                       "diameter=\"40\" id=\"default\" type=\"circle\" antialiasEdges=\"1\" vfade=\"0.5\"/></Brush>"));
    KisPaintingModeOptionData paintingMode;
    paintingMode.paintingMode = wash ? enumPaintingMode::WASH : enumPaintingMode::BUILDUP;
    paintingMode.write(settings.data());
    KisOpacityOptionData opacity;
    opacity.useCurve = false;
    opacity.strengthValue = 0.6;
    opacity.write(settings.data());
    KisFlowOptionData flow;
    flow.useCurve = false;
    flow.strengthValue = 0.8;
    flow.write(settings.data());

    KisPaintOpPresetSP preset(new KisPaintOpPreset());
    preset->setSettings(settings);
    return preset;
}

struct Scene {
    Scene(const KoColorSpace *cs)
        : undoStore(new KisSurrogateUndoStore)
        , image(new KisImage(undoStore, imageRect.width(), imageRect.height(), cs, "through-layer eraser test"))
    {
        image->setWorkingThreadsLimit(4);

        // from the bottom: the order the stack is built in
        bottom = addLayer("bottom", image->root());
        alphaLocked = addLayer("alpha locked", image->root());
        locked = addLayer("locked", image->root());
        group = new KisGroupLayer(image, "group", OPACITY_OPAQUE_U8);
        image->addNode(group, image->root());
        inGroup = addLayer("in group", group);
        hiddenGroup = new KisGroupLayer(image, "hidden group", OPACITY_OPAQUE_U8);
        image->addNode(hiddenGroup, image->root());
        inHiddenGroup = addLayer("in hidden group", hiddenGroup);
        hidden = addLayer("hidden", image->root());
        top = addLayer("top", image->root());

        alphaLocked->setAlphaLocked(true);
        locked->setUserLocked(true);
        hiddenGroup->setVisible(false);
        hidden->setVisible(false);

        image->initialRefreshGraph();
        image->waitForDone();
        undoStore->clear();
    }

    KisPaintLayerSP addLayer(const QString &name, KisNodeSP parent)
    {
        KisPaintLayerSP layer = new KisPaintLayer(image, name, OPACITY_OPAQUE_U8, image->colorSpace());
        KoColor color(QColor(40, 120, 200, 180), image->colorSpace());
        layer->paintDevice()->fill(imageRect, color);
        image->addNode(layer, parent);
        return layer;
    }

    KisNodeList allTargets() const
    {
        return {top, inGroup, bottom};
    }

    KisNodeList untouched() const
    {
        return {hidden, inHiddenGroup, locked, alphaLocked};
    }

    KisResourcesSnapshotSP
    resources(KisPaintOpPresetSP preset, KisNodeSP node, const QString &compositeOp = COMPOSITE_OVER)
    {
        canvasResources.setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));
        canvasResources.setResource(KoCanvasResource::CurrentEffectiveCompositeOp, compositeOp);
        canvasResources.setResource(KoCanvasResource::Opacity, 1.0);
        canvasResources.setResource(KoCanvasResource::ForegroundColor,
                                    QVariant::fromValue(KoColor(Qt::black, image->colorSpace())));
        canvasResources.setResource(KoCanvasResource::BackgroundColor,
                                    QVariant::fromValue(KoColor(Qt::white, image->colorSpace())));
        KisResourcesSnapshotSP snapshot = new KisResourcesSnapshot(image, node, &canvasResources);
        snapshot->setBrush(preset);
        return snapshot;
    }

    KisStrokeId startStroke(KisResourcesSnapshotSP snapshot)
    {
        const KisStrokeId stroke = image->startStroke(
            new FreehandStrokeStrategy(snapshot, new KisFreehandStrokeInfo(), kundo2_noi18n("test stroke")));
        QPointF previous(60, 128);
        for (int segment = 1; segment <= 8; ++segment) {
            const QPointF next(60 + segment * 17, 128);
            image->addJob(stroke,
                          new FreehandStrokeStrategy::Data(0,
                                                           KisPaintInformation(previous, 1.0),
                                                           KisPaintInformation(next, 1.0)));
            previous = next;
        }
        image->addJob(stroke, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
        return stroke;
    }

    void finish()
    {
        image->waitForDone();
#ifdef HAVE_KRITA_GPU_ENGINE
        if (auto *backend = KisGpuTileBackend::existingInstance()) {
            backend->context().waitIdle();
        }
#endif
    }

    void stroke(KisResourcesSnapshotSP snapshot)
    {
        image->endStroke(startStroke(snapshot));
        finish();
    }

    KisSurrogateUndoStore *undoStore;
    KisImageSP image;
    KoCanvasResourceProvider canvasResources;
    KisPaintLayerSP top, hidden, inGroup, inHiddenGroup, locked, alphaLocked, bottom;
    KisGroupLayerSP group, hiddenGroup;
};
} // namespace

class KisThroughLayerEraserTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
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

    void testCollectAllLayers()
    {
        Scene scene(KoColorSpaceRegistry::instance()->rgb8());
        // nothing selected matters: hidden, locked and alpha locked layers stay out
        QCOMPARE(KisThroughLayerEraserTargets::collect(scene.image->root(), {scene.hidden}, false, false),
                 scene.allTargets());
    }

    void testCollectAlphaLockedLayers()
    {
        Scene scene(KoColorSpaceRegistry::instance()->rgb8());
        KisNodeSP root = scene.image->root();

        QCOMPARE(KisThroughLayerEraserTargets::collect(root, {}, false, true),
                 KisNodeList({scene.top, scene.inGroup, scene.alphaLocked, scene.bottom}));
        QCOMPARE(KisThroughLayerEraserTargets::collect(root, {scene.alphaLocked}, true, true),
                 KisNodeList({scene.alphaLocked}));
        QVERIFY(KisThroughLayerEraserTargets::collect(root, {scene.alphaLocked}, true, false).isEmpty());
        // the option does not reach a locked layer
        QVERIFY(KisThroughLayerEraserTargets::collect(root, {scene.locked}, true, true).isEmpty());
    }

    void testCollectSelectedLayers()
    {
        Scene scene(KoColorSpaceRegistry::instance()->rgb8());
        KisNodeSP root = scene.image->root();

        QVERIFY(KisThroughLayerEraserTargets::collect(root, {}, true, false).isEmpty());
        QCOMPARE(KisThroughLayerEraserTargets::collect(root, {scene.bottom, scene.top}, true, false),
                 KisNodeList({scene.top, scene.bottom}));
        // a group stands for its layers, and a layer selected with its group counts once
        QCOMPARE(KisThroughLayerEraserTargets::collect(root, {scene.group, scene.inGroup}, true, false),
                 KisNodeList({scene.inGroup}));
        // selecting what cannot be erased does not make it a target
        QVERIFY(KisThroughLayerEraserTargets::collect(root, scene.untouched(), true, false).isEmpty());
        QVERIFY(KisThroughLayerEraserTargets::collect(root, {scene.hiddenGroup}, true, false).isEmpty());
    }

    void testCollectInsideAGroup()
    {
        Scene scene(KoColorSpaceRegistry::instance()->rgb8());
        // the root of the isolated mode shows even when it is hidden
        QCOMPARE(KisThroughLayerEraserTargets::collect(scene.group, {}, false, false), KisNodeList({scene.inGroup}));
        QCOMPARE(KisThroughLayerEraserTargets::collect(scene.hiddenGroup, {}, false, false),
                 KisNodeList({scene.inHiddenGroup}));
    }

    void testErase_data()
    {
        QTest::addColumn<bool>("wash");
        QTest::addColumn<bool>("gpu");
        QTest::addColumn<bool>("alphaLocked");
        QTest::addRow("wash, cpu") << true << false << false;
        QTest::addRow("build-up, cpu") << false << false << false;
        QTest::addRow("wash, cpu, alpha locked") << true << false << true;
#ifdef HAVE_KRITA_GPU_ENGINE
        if (KisGpuTileBackend::instance()) {
            QTest::addRow("wash, gpu") << true << true << false;
            QTest::addRow("build-up, gpu") << false << true << false;
            QTest::addRow("wash, gpu, alpha locked") << true << true << true;
        }
#endif
    }

    void testErase()
    {
        QFETCH(bool, wash);
        QFETCH(bool, gpu);
        QFETCH(bool, alphaLocked);

        const auto previousBrush = qgetenv("KRITA_GPU_BRUSH");
        const auto restore = qScopeGuard([&]() {
            if (previousBrush.isNull()) {
                qunsetenv("KRITA_GPU_BRUSH");
            } else {
                qputenv("KRITA_GPU_BRUSH", previousBrush);
            }
#ifdef HAVE_KRITA_GPU_ENGINE
            KisGpuMergeBatch::setEnabled(false);
#endif
        });
        qputenv("KRITA_GPU_BRUSH", gpu ? "1" : "0");
#ifdef HAVE_KRITA_GPU_ENGINE
        KisGpuMergeBatch::setEnabled(gpu);
#endif

        const KoColorSpace *cs = gpu ? KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(),
                                                                                    Float32BitsColorDepthID.id(),
                                                                                    QString())
                                     : KoColorSpaceRegistry::instance()->rgb8();
        KisPaintOpPresetSP preset = createPreset(wash);

        // what an ordinary erase stroke does to one layer
        QVector<qreal> reference;
        {
            Scene scene(cs);
            KisResourcesSnapshotSP snapshot = scene.resources(preset, scene.top, COMPOSITE_ERASE);
            QCOMPARE(snapshot->compositeOpId(), QString(COMPOSITE_ERASE));
            scene.stroke(snapshot);
            reference = alphas(scene.top->paintDevice());
        }

        Scene scene(cs);
        const QVector<qreal> before = alphas(scene.top->paintDevice());

        KisNodeList targets = scene.allTargets();
        KisNodeList untouched = scene.untouched();
        if (alphaLocked) {
            // the tool lists an alpha locked layer only when told to erase it
            targets = {scene.top, scene.inGroup, scene.alphaLocked, scene.bottom};
            untouched.removeAll(KisNodeSP(scene.alphaLocked));
        }

        KisResourcesSnapshotSP snapshot = scene.resources(preset, scene.top);
        snapshot->setThroughLayerTargets(targets);
        scene.stroke(snapshot);

        // Build-up accumulates its dabs in eight bits on the CPU
        const qreal fuzziness = gpu ? 1.0 / 255 : 3.0 / 255;
        for (KisNodeSP node : targets) {
            QVERIFY2(largestDifference(alphas(node->paintDevice()), reference) <= fuzziness, qPrintable(node->name()));
            QVERIFY(!dynamic_cast<KisPaintLayer *>(node.data())->hasTemporaryTarget());
        }
        for (KisNodeSP node : untouched) {
            QVERIFY2(largestDifference(alphas(node->paintDevice()), before) == 0, qPrintable(node->name()));
        }
        // the stroke leaves the lock as it was
        QVERIFY(scene.alphaLocked->alphaLocked());

        // the updates during the stroke already showed the final state
        const QVector<qreal> projection = alphas(scene.image->projection());
        scene.image->refreshGraphAsync();
        scene.finish();
        QVERIFY(largestDifference(alphas(scene.image->projection()), projection) <= 1.0 / 255);

        // one undo step for all the layers
        scene.undoStore->undo();
        scene.finish();
        for (KisNodeSP node : targets) {
            QVERIFY2(largestDifference(alphas(node->paintDevice()), before) == 0, qPrintable(node->name()));
        }

        scene.undoStore->redo();
        scene.finish();
        for (KisNodeSP node : targets) {
            QVERIFY2(largestDifference(alphas(node->paintDevice()), reference) <= fuzziness, qPrintable(node->name()));
        }
    }

    void testBuildUpReachesFullErase()
    {
        // dabs accumulate past the stroke's opacity, unlike a Wash stroke
        Scene scene(KoColorSpaceRegistry::instance()->rgb8());
        KisResourcesSnapshotSP snapshot = scene.resources(createPreset(false), scene.top);
        snapshot->setThroughLayerTargets(scene.allTargets());
        scene.stroke(snapshot);

        const QRect center(128, 128, 1, 1);
        QVERIFY(alphas(scene.top->paintDevice(), center).first() < 0.05);
        QVERIFY(alphas(scene.bottom->paintDevice(), center).first() < 0.05);
    }

    void testCancelLeavesTheLayers()
    {
        Scene scene(KoColorSpaceRegistry::instance()->rgb8());
        const QVector<qreal> before = alphas(scene.top->paintDevice());
        const QVector<qreal> projection = alphas(scene.image->projection());

        KisResourcesSnapshotSP snapshot = scene.resources(createPreset(true), scene.top);
        snapshot->setThroughLayerTargets(scene.allTargets());
        scene.image->cancelStroke(scene.startStroke(snapshot));
        scene.finish();

        for (KisNodeSP node : scene.allTargets()) {
            QCOMPARE(alphas(node->paintDevice()), before);
            QVERIFY(!dynamic_cast<KisPaintLayer *>(node.data())->hasTemporaryTarget());
        }
        QCOMPARE(alphas(scene.image->projection()), projection);
    }

    void testBrushStrokeLayerDoesNotRecord()
    {
        Scene scene(KoColorSpaceRegistry::instance()->rgb8());
        KisBrushStrokeLayerSP layer =
            new KisBrushStrokeLayer(scene.image, "brush strokes", OPACITY_OPAQUE_U8, scene.image->colorSpace());
        layer->paintDevice()->fill(imageRect, KoColor(QColor(40, 120, 200, 180), scene.image->colorSpace()));
        scene.image->addNode(layer, scene.image->root());
        scene.finish();

        KisResourcesSnapshotSP snapshot = scene.resources(createPreset(true), layer);
        snapshot->setThroughLayerTargets({layer, scene.bottom});
        scene.stroke(snapshot);

        QVERIFY(alphas(layer->paintDevice(), QRect(128, 128, 1, 1)).first() < 0.05);
        QVERIFY(layer->strokes().isEmpty());
    }

    void testEachLayerKeepsItsColorSpaceAndSelection()
    {
        Scene scene(KoColorSpaceRegistry::instance()->rgb8());

        // a different color space than the shared device
        const KoColorSpace *rgb16 = KoColorSpaceRegistry::instance()->rgb16();
        scene.bottom->paintDevice()->convertTo(rgb16);
        scene.image->refreshGraphAsync();
        scene.finish();

        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(QRect(0, 0, 128, 256));
        KisSetGlobalSelectionCommand(scene.image, selection).redo();
        scene.finish();

        KisResourcesSnapshotSP snapshot = scene.resources(createPreset(true), scene.top);
        snapshot->setThroughLayerTargets(scene.allTargets());
        scene.stroke(snapshot);

        for (KisNodeSP node : scene.allTargets()) {
            KisPaintDeviceSP device = node->paintDevice();
            const qreal inside = alphas(device, QRect(100, 128, 1, 1)).first();
            const qreal outside = alphas(device, QRect(140, 128, 1, 1)).first();
            QVERIFY2(inside < 0.45, qPrintable(node->name()));
            QVERIFY2(outside > 0.65, qPrintable(node->name()));
        }
    }
};

SIMPLE_TEST_MAIN(KisThroughLayerEraserTest)
#include "KisThroughLayerEraserTest.moc"
