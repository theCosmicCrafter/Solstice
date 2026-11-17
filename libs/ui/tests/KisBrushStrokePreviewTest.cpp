/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../../plugins/paintops/mypaint/MyPaintPaintOpFactory.h"
#include "KisPresetDockerFilters.h"
#include <KisDocument.h>
#include <KisGlobalResourcesInterface.h>
#include <KisPart.h>
#include <KisResourceItemChooser.h>
#include <KisResourceItemChooserSync.h>
#include <KisResourceItemListView.h>
#include <KisResourceLocator.h>
#include <KisResourceModel.h>
#include <KisResourceModelProvider.h>
#include <KisResourceStorage.h>
#include <KisStorageModel.h>
#include <KisTagFilterResourceProxyModel.h>
#include <KisViewManager.h>
#include <KoCanvasResourceProvider.h>
#include <KoCompositeOpRegistry.h>
#include <KoResourceBundle.h>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsPixmapItem>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QToolButton>
#include <brushengine/kis_paintop_registry.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_config.h>
#include <kis_canvas_resource_provider.h>
#include <kis_simple_stroke_strategy.h>
#include <numeric>
#include <simpletest.h>
#include <testui.h>
#include <widgets/KisBrushStrokePreviewCache.h>
#include <widgets/KisBrushStrokePreviewRenderer.h>
#include <widgets/kis_paintop_presets_chooser_popup.h>
#include <widgets/kis_preset_chooser.h>
#include <widgets/kis_preset_live_preview_view.h>

namespace
{
KisPaintOpPresetSP previewPreset(const QString &engine)
{
    const QString root = QString(FILES_DATA_DIR) + "../../../../plugins/paintops/";
    if (engine == "mypaintbrush") {
        auto preset =
            KisPaintOpRegistry::instance()->defaultPreset(KoID(engine), KisGlobalResourcesInterface::instance());
        QFile file(root + "mypaint/tests/data/basic.myb");
        if (!file.open(QIODevice::ReadOnly))
            return {};
        preset->settings()->setProperty("MyPaint/json", file.readAll());
        return preset;
    }
    KisPaintOpPresetSP preset(new KisPaintOpPreset(root + "defaultpresets/" + engine + ".kpp"));
    if (!preset->load(KisGlobalResourcesInterface::instance()))
        return {};
    return preset;
}
} // namespace

class KisBrushStrokePreviewTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        qputenv("KRITA_NO_ASSERT_MSG", "1");
        KisPaintOpRegistry::instance()->add(new KisMyPaintOpFactory());
        KisResourceModel model(ResourceType::PaintOpPresets);
        for (const QString &engine :
             {"paintbrush", "colorsmudge", "spraybrush", "sketchbrush", "mypaintbrush", "filter", "duplicate"}) {
            auto preset = previewPreset(engine);
            QVERIFY(preset);
            preset->setFilename("solstice-preview-test-" + engine + ".kpp");
            preset->setName(engine + " preview");
            preset->addMetaData("paintopid", engine);
            if (preset->image().isNull()) {
                QImage icon(32, 32, QImage::Format_ARGB32);
                icon.fill(Qt::white);
                preset->setImage(icon);
            }
            QVERIFY(model.addResource(preset));
        }
    }
    void testEditorBaseline_data()
    {
        QTest::addColumn<QString>("engine");
        for (const QString &id : {"paintbrush", "colorsmudge", "spraybrush", "sketchbrush", "mypaintbrush", "filter"})
            QTest::newRow(qPrintable(id)) << id;
    }
    void testEditorBaseline()
    {
        QFETCH(QString, engine);
        auto preset = previewPreset(engine);
        QVERIFY(preset);
        KoCanvasResourceProvider provider;
        KisViewManager::initializeResourceManager(&provider);
        provider.setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));
        provider.setResource(KoCanvasResource::ForegroundColor,
                             QVariant::fromValue(KoColor(Qt::black, KoColorSpaceRegistry::instance()->rgb8())));
        provider.setResource(KoCanvasResource::BackgroundColor,
                             QVariant::fromValue(KoColor(Qt::white, KoColorSpaceRegistry::instance()->rgb8())));
        provider.setResource(KoCanvasResource::CurrentCompositeOp, COMPOSITE_OVER);
        provider.setResource(KoCanvasResource::Opacity, 1.0);
        provider.setResource(KoCanvasResource::EffectiveZoom, 1.0);
        KisPresetLivePreviewView view(nullptr);
        view.resize(480, 160);
        view.setup(&provider);
        view.setCurrentPreset(preset);
        view.requestUpdateStroke();
        auto image = [&]() {
            for (auto *item : view.scene()->items())
                if (auto *pixmap = dynamic_cast<QGraphicsPixmapItem *>(item))
                    return pixmap->pixmap().toImage();
            return QImage();
        };
        QTRY_VERIFY_WITH_TIMEOUT(!image().isNull(), 15000);
        const QString path = qEnvironmentVariable("SOLSTICE_PREVIEW_BASELINE_DIR");
        if (!path.isEmpty()) {
            QDir().mkpath(path);
            QVERIFY(image().save(path + '/' + engine + ".png"));
        }
    }
    void testDockerRendering_data()
    {
        testEditorBaseline_data();
    }
    void testDockerRendering()
    {
        QFETCH(QString, engine);
        auto preset = previewPreset(engine);
        QVERIFY(preset);
        const auto original = preset->settings()->toXML();
        KisBrushStrokePreviewRenderer renderer;
        QSignalSpy spy(&renderer, &KisBrushStrokePreviewRenderer::finished);
        QElapsedTimer elapsed;
        elapsed.start();
        renderer.start(preset);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 15000);
        const QImage first = spy[0][0].value<QImage>();
        QCOMPARE(first.size(), QSize(480, 160));
        QVERIFY(!spy[0][1].toBool());
        for (int y = 0; y < first.height(); ++y) {
            const auto *pixels = reinterpret_cast<const QRgb *>(first.constScanLine(y));
            for (int x = 0; x < first.width(); ++x)
                QVERIFY(qAlpha(pixels[x]) || pixels[x] == 0);
        }
        qInfo() << engine << "first render ms" << elapsed.elapsed();
        renderer.start(preset);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 15000);
        if (first != spy[1][0].value<QImage>() && !qEnvironmentVariable("SOLSTICE_PREVIEW_BASELINE_DIR").isEmpty()) {
            first.save(qEnvironmentVariable("SOLSTICE_PREVIEW_BASELINE_DIR") + '/' + engine + "-docker-a.png");
            spy[1][0].value<QImage>().save(qEnvironmentVariable("SOLSTICE_PREVIEW_BASELINE_DIR") + '/' + engine
                                           + "-docker-b.png");
        }
        QCOMPARE(spy[1][0].value<QImage>(), first);
        QCOMPARE(preset->settings()->toXML(), original);
    }
    void testMyPaintRandomRepeat()
    {
        auto preset = previewPreset("mypaintbrush");
        auto json = QJsonDocument::fromJson(preset->settings()->getProperty("MyPaint/json").toByteArray()).object();
        auto settings = json["settings"].toObject();
        auto offset = settings["offset_by_random"].toObject();
        offset["base_value"] = 1.0;
        settings["offset_by_random"] = offset;
        json["settings"] = settings;
        preset->settings()->setProperty("MyPaint/json", QJsonDocument(json).toJson());
        KisBrushStrokePreviewRenderer renderer;
        QSignalSpy spy(&renderer, &KisBrushStrokePreviewRenderer::finished);
        renderer.start(preset);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 15000);
        renderer.start(preset);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 15000);
        QCOMPARE(spy[0][0].value<QImage>().size(), QSize(480, 160));
        QCOMPARE(spy[0][0].value<QImage>(), spy[1][0].value<QImage>());
    }
    void testCancelledAndExcluded()
    {
        auto preset = previewPreset("paintbrush");
        KisBrushStrokePreviewRenderer renderer;
        QSignalSpy spy(&renderer, &KisBrushStrokePreviewRenderer::finished);
        renderer.start(preset);
        renderer.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 15000);
        QVERIFY(spy[0][0].value<QImage>().isNull());
        QVERIFY(spy[0][1].toBool());
        QVERIFY(!renderer.isRunning());
        for (const QString &engine : {"roundmarker", "experimentbrush", "duplicate"})
            QVERIFY(!KisBrushStrokePreviewRenderer::supported(engine));
    }
    void testSavedCopy_data()
    {
        QTest::addColumn<bool>("memory");
        QTest::newRow("folder") << false;
        QTest::newRow("memory") << true;
    }
    void testSavedCopy()
    {
        QFETCH(bool, memory);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QDir().mkpath(directory.filePath("paintoppresets"));
        KisResourceStorageSP storage(new KisResourceStorage(memory ? "memory" : directory.path()));
        auto preset = previewPreset("paintbrush");
        preset->setFilename("saved.kpp");
        QImage icon(32, 32, QImage::Format_ARGB32);
        icon.fill(Qt::white);
        preset->setImage(icon);
        const qreal savedSize = preset->settings()->paintOpSize();
        QVERIFY(storage->addResource(preset));
        const QString md5 = storage->resourceMd5("paintoppresets/saved.kpp");
        QVERIFY(!md5.isEmpty());
        preset->settings()->setPaintOpSize(savedSize + 100);
        preset->setDirty(true);
        auto saved = KisResourceLocator::loadResourceSnapshot(storage, ResourceType::PaintOpPresets, "saved.kpp", md5)
                         .dynamicCast<KisPaintOpPreset>();
        QVERIFY(saved);
        QCOMPARE(saved->settings()->paintOpSize(), savedSize);
        QCOMPARE(preset->settings()->paintOpSize(), savedSize + 100);
        QVERIFY(preset->isDirty());
        QVERIFY(
            !KisResourceLocator::loadResourceSnapshot(storage, ResourceType::PaintOpPresets, "saved.kpp", "obsolete"));
    }
    void testBundleCopy()
    {
        const QString path =
            QString(FILES_DATA_DIR) + "../../../../krita/data/bundles/Krita_4_Default_Resources.bundle";
        KisResourceStorageSP storage(new KisResourceStorage(path));
        QVERIFY(storage->valid());
        auto iterator = storage->resources(ResourceType::PaintOpPresets);
        QVERIFY(iterator->hasNext());
        iterator->next();
        const QString filename = iterator->url().section('/', 1);
        const QString md5 = storage->resourceMd5(iterator->url());
        auto saved = KisResourceLocator::loadResourceSnapshot(storage, ResourceType::PaintOpPresets, filename, md5)
                         .dynamicCast<KisPaintOpPreset>();
        QVERIFY(saved);
        const auto original = saved->settings()->paintOpSize();
        saved->settings()->setPaintOpSize(original + 100);
        auto again = KisResourceLocator::loadResourceSnapshot(storage, ResourceType::PaintOpPresets, filename, md5)
                         .dynamicCast<KisPaintOpPreset>();
        QVERIFY(again);
        QCOMPARE(again->settings()->paintOpSize(), original);
    }
    void testChooserIsolation()
    {
        KisPresetChooser docker, popup;
        const int original = popup.iconSize();
        docker.setStrokePreviewMode(true);
        docker.setIconSize(70);
        QCOMPARE(docker.iconSize(), 70);
        QCOMPARE(popup.iconSize(), original);
        QCOMPARE(docker.itemChooser()->itemView()->gridSize().width(), 210);
        QVERIFY(docker.itemChooser()->itemView()->gridSize().height() < 210);
        docker.setStrokePreviewMode(false);
        QCOMPARE(docker.iconSize(), original);
    }
    void testDockerScrollToSelection()
    {
        // Solstice (docs/agent/brush-preset-scroll.md): with "Scroll to
        // Selected Preset" off, a brush selected elsewhere moves only the
        // highlight; on, the list scrolls to it as in Krita.
        const QString key = QStringLiteral("Solstice/BrushPresetScrollToSelection");
        const auto restore = qScopeGuard([key]() {
            KisConfig(false).writeEntry<bool>(key, false);
        });
        KisConfig(false).writeEntry<bool>(key, false);

        KisPaintOpPresetsChooserPopup docker;
        docker.enableStrokePreviewSetting();
        docker.enableScrollToSelectionSetting();
        docker.resize(260, 260);
        docker.show();
        auto *chooser = docker.findChild<KisPresetChooser *>();
        QVERIFY(chooser);
        auto *view = chooser->itemChooser()->itemView();
        auto *model = chooser->itemChooser()->tagFilterModel();
        QVERIFY(model->rowCount() > 2);
        QTRY_VERIFY(view->verticalScrollBar()->maximum() > 0);
        QVERIFY(!view->followCurrentItem());

        auto presetAt = [model](int row) {
            return model->resourceForIndex(model->index(row, 0)).dynamicCast<KisPaintOpPreset>();
        };
        const KisPaintOpPresetSP first = presetAt(0);
        const KisPaintOpPresetSP last = presetAt(model->rowCount() - 1);
        QVERIFY(first && last);

        docker.canvasResourceChanged(first);
        view->verticalScrollBar()->setValue(0);
        docker.canvasResourceChanged(last);
        QCOMPARE(chooser->currentResource(), KoResourceSP(last));
        QCOMPARE(view->verticalScrollBar()->value(), 0);
        docker.resize(260, 300);
        QTest::qWait(50);
        QCOMPARE(view->verticalScrollBar()->value(), 0);

        // the setting is read on every selection, also when another docker
        // changed it
        KisConfig(false).writeEntry<bool>(key, true);
        docker.canvasResourceChanged(first);
        view->verticalScrollBar()->setValue(0);
        docker.canvasResourceChanged(last);
        QVERIFY(view->followCurrentItem());
        QCOMPARE(chooser->currentResource(), KoResourceSP(last));
        QVERIFY(view->verticalScrollBar()->value() > 0);
    }
    void testClickAtBottomKeepsScroll()
    {
        // Clicking a preset at the bottom of a grouped list keeps the list
        // where it is (the selection used to throw it upwards)
        const QString key = QStringLiteral("Solstice/BrushPresetScrollToSelection");
        const auto restore = qScopeGuard([key]() {
            KisConfig(false).writeEntry<bool>(key, false);
        });
        KisConfig(false).writeEntry<bool>(key, false);

        for (int grouping : {int(KisPresetDockerFilters::NoGrouping),
                             int(KisPresetDockerFilters::GroupByBundle),
                             int(KisPresetDockerFilters::GroupByEngine)}) {
            KisPaintOpPresetsChooserPopup docker;
            docker.enableStrokePreviewSetting();
            docker.enableScrollToSelectionSetting();
            docker.resize(300, 260);
            docker.show();
            auto *chooser = docker.findChild<KisPresetChooser *>();
            QVERIFY(chooser);
            auto *items = chooser->itemChooser();
            auto *view = items->itemView();
            auto *combo = items->findChild<QComboBox *>("PresetGrouping");
            QVERIFY(combo);
            const int initial = combo->currentIndex();
            combo->setCurrentIndex(combo->findData(grouping));
            auto *bar = view->verticalScrollBar();
            QTRY_VERIFY(bar->maximum() > 0);
            QTest::qWait(100);
            bar->setValue(bar->maximum());
            QTest::qWait(100);
            const int bottom = bar->value();

            // the last visible item
            auto *model = view->model();
            QModelIndex target;
            for (int row = model->rowCount() - 1; row >= 0; --row) {
                const QModelIndex index = model->index(row, 0);
                if (view->viewport()->rect().contains(view->visualRect(index).center())) {
                    target = index;
                    break;
                }
            }
            QVERIFY(target.isValid());
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->visualRect(target).center());
            KisPaintOpPresetSP preset = model->data(target, Qt::UserRole + KisAbstractResourceModel::Id).isValid()
                ? items->tagFilterModel()->resourceForIndex(target).dynamicCast<KisPaintOpPreset>()
                : KisPaintOpPresetSP();
            if (preset) {
                // the application then sets the selected preset back
                docker.canvasResourceChanged(preset);
            }
            QTest::qWait(300);
            qInfo() << "grouping" << grouping << "bottom" << bottom << "after" << bar->value() << "max"
                    << bar->maximum();
            QCOMPARE(bar->value(), bottom);
            combo->setCurrentIndex(initial);
        }
    }
    void testResponsiveDockerLayout()
    {
        // Reproduce the real docker's initialization order, including entering
        // a horizontal strip before preview mode is enabled.
        KisPaintOpPresetsChooserPopup docker;
        auto *chooser = docker.findChild<KisPresetChooser *>();
        QVERIFY(chooser);
        auto *items = chooser->itemChooser();
        items->resize(900, 50);
        docker.setResponsiveness(true);
        QVERIFY(!items->itemView()->isWrapping());
        docker.enableStrokePreviewSetting();
        docker.resize(1100, 400);
        docker.show();
        auto *view = items->itemView();
        auto *bar = items->findChild<QWidget *>("ResourceChooserBottomBar");
        QVERIFY(bar);
        QWidget *tags = nullptr;
        QWidget *filter = nullptr;
        for (auto *widget : items->findChildren<QWidget *>()) {
            if (widget->inherits("KisTagChooserWidget"))
                tags = widget;
            if (widget->inherits("KisTagFilterWidget"))
                filter = widget;
        }
        QVERIFY(tags && filter);
        auto columns = [&]() {
            const int top = view->visualRect(view->model()->index(0, 0)).top();
            int result = 0;
            for (int row = 0; row < view->model()->rowCount(); ++row) {
                if (view->visualRect(view->model()->index(row, 0)).top() != top)
                    break;
                ++result;
            }
            return result;
        };
        auto check = [&]() {
            return view->isWrapping() && bar->isVisible() && tags->isVisible() && filter->isVisible()
                && bar->mapTo(items, QPoint()).y() >= view->mapTo(items, QPoint(0, view->height())).y()
                && filter->mapTo(items, QPoint(0, filter->height())).y() <= items->height()
                && bar->height() >= bar->minimumSizeHint().height()
                && view->gridSize().height() < 120;
        };
        QTRY_VERIFY(check());
        QTRY_VERIFY(columns() >= 5);
        const int wideColumns = columns();
        const QString output = qEnvironmentVariable("SOLSTICE_PREVIEW_LAYOUT_IMAGE");
        auto *cache = KisBrushStrokePreviewCache::instance();
        for (int row = 0; row < view->model()->rowCount(); ++row) {
            const auto request = KisBrushStrokePreviewCache::Request::fromIndex(view->model()->index(row, 0));
            QTRY_VERIFY_WITH_TIMEOUT(cache->ready(request), 15000);
        }
        if (!output.isEmpty())
            QVERIFY(docker.grab().save(output + ".wide.png"));
        docker.resize(420, 600);
        QTRY_VERIFY(check());
        QTRY_VERIFY(columns() < wideColumns);
        QVERIFY(view->visualRect(view->model()->index(wideColumns, 0)).top()
                > view->visualRect(view->model()->index(0, 0)).top());
        if (!output.isEmpty())
            QVERIFY(docker.grab().save(output + ".narrow.png"));
        docker.resize(280, 600);
        QTRY_VERIFY(check());
        QTRY_COMPARE(columns(), 1);
        QTRY_VERIFY(filter->mapTo(bar, QPoint()).y() >= tags->mapTo(bar, QPoint(0, tags->height())).y());
        if (!output.isEmpty())
            QVERIFY(docker.grab().save(output + ".compact.png"));
        docker.resize(1100, 100);
        QTRY_VERIFY(check());
        chooser->setStrokePreviewMode(false);
        QTRY_VERIFY(bar->isVisible()); // Docker filters also remain available in icon mode.
        chooser->setStrokePreviewMode(true);
        docker.resize(1100, 400);
        QTRY_VERIFY(check());
        QTRY_COMPARE(columns(), wideColumns);
        docker.hide();
        QTRY_VERIFY_WITH_TIMEOUT(!KisBrushStrokePreviewCache::instance()->isBusy(), 15000);
    }
    void testDockerGrouping()
    {
        // Grouping by engine or bundle stacks each group under a header in
        // one contiguous block; no grouping restores the plain grid.
        KisPaintOpPresetsChooserPopup docker;
        auto *chooser = docker.findChild<KisPresetChooser *>();
        QVERIFY(chooser);
        docker.enableStrokePreviewSetting();
        docker.resize(900, 700);
        docker.show();
        auto *items = chooser->itemChooser();
        auto *view = items->itemView();
        auto *filters = static_cast<KisPresetDockerFilters *>(items->findChild<QWidget *>("PresetDockerFilters"));
        auto *grouping = items->findChild<QComboBox *>("PresetGrouping");
        QVERIFY(filters && grouping);
        QVERIFY(grouping->isVisible());
        const int rows = view->model()->rowCount();
        QVERIFY(rows >= 2);
        auto checkGroups = [&]() {
            QVector<int> order(rows);
            std::iota(order.begin(), order.end(), 0);
            auto rect = [&](int row) {
                return view->visualRect(view->model()->index(row, 0));
            };
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                const QRect ra = rect(a);
                const QRect rb = rect(b);
                return ra.top() != rb.top() ? ra.top() < rb.top() : ra.left() < rb.left();
            });
            QStringList seen;
            int previousBottom = std::numeric_limits<int>::min();
            for (int i = 0; i < rows; ++i) {
                const QString group = filters->groupOf(view->model()->index(order[i], 0)).second;
                if (seen.isEmpty() || seen.last() != group) {
                    if (seen.contains(group))
                        return false; // a group split into two blocks
                    // A new group starts on a new line below a header gap.
                    if (!seen.isEmpty() && rect(order[i]).top() <= previousBottom)
                        return false;
                    seen << group;
                }
                previousBottom = qMax(previousBottom, rect(order[i]).bottom());
                for (int j = 0; j < i; ++j) {
                    if (rect(order[i]).intersects(rect(order[j])))
                        return false;
                }
            }
            return true;
        };
        const int initial = grouping->currentIndex();
        grouping->setCurrentIndex(KisPresetDockerFilters::GroupByEngine);
        QTRY_VERIFY(checkGroups());
        const QString output = qEnvironmentVariable("SOLSTICE_PREVIEW_LAYOUT_IMAGE");
        if (!output.isEmpty()) {
            QTest::qWait(300); // let the bottom bar settle
            QVERIFY(docker.grab().save(output + ".grouped-engine.png"));
        }
        docker.resize(420, 700);
        QTRY_VERIFY(checkGroups());
        grouping->setCurrentIndex(KisPresetDockerFilters::GroupByBundle);
        QTRY_VERIFY(checkGroups());
        if (!output.isEmpty())
            QVERIFY(docker.grab().save(output + ".grouped-bundle.png"));
        grouping->setCurrentIndex(KisPresetDockerFilters::NoGrouping);
        QTRY_VERIFY(view->visualRect(view->model()->index(1, 0)).top()
                    == view->visualRect(view->model()->index(0, 0)).top());
        grouping->setCurrentIndex(initial);
        docker.hide();
        QTRY_VERIFY_WITH_TIMEOUT(!KisBrushStrokePreviewCache::instance()->isBusy(), 15000);
    }
    void testBrushTipFilters()
    {
        // The brush tips of the Brush Editor get the bundle facet and
        // grouping by bundle, without the engine facet
        KisResourceItemChooser chooser(ResourceType::Brushes, false);
        auto *filters =
            new KisPresetDockerFilters(chooser.tagFilterModel(), chooser.itemView(), nullptr, ResourceType::Brushes);
        chooser.setBottomBarWidget(filters);
        auto *engines = filters->findChild<QToolButton *>("PresetEngineFilter");
        auto *bundles = filters->findChild<QToolButton *>("PresetBundleFilter");
        auto *grouping = filters->findChild<QComboBox *>("PresetGrouping");
        QVERIFY(engines && bundles && grouping);
        QVERIFY(engines->isHidden());
        QCOMPARE(grouping->count(), 2);
        QCOMPARE(grouping->itemData(0).toInt(), int(KisPresetDockerFilters::NoGrouping));
        QCOMPARE(grouping->itemData(1).toInt(), int(KisPresetDockerFilters::GroupByBundle));
        QCOMPARE(bundles->text(), i18n("Bundles: All"));

        auto *model = chooser.tagFilterModel();
        const int allCount = model->rowCount();
        // the bundles of this test's resources hold presets only
        if (allCount > 0) {
            bundles->menu()->popup(QPoint(20, 20));
            QTRY_VERIFY(bundles->menu()->isVisible());
            int bundleEntries = 0;
            for (auto *action : bundles->menu()->actions())
                bundleEntries += action->isCheckable();
            QVERIFY(bundleEntries > 0);
            bundles->menu()->findChild<QAction *>("ClearAll")->trigger();
            QCOMPARE(model->rowCount(), 0);
            bundles->menu()->findChild<QAction *>("SelectAll")->trigger();
            QCOMPARE(model->rowCount(), allCount);
            bundles->menu()->hide();
        }

        // the tips' grouping is kept apart from the presets'
        const int presetGrouping = KisConfig(true).readEntry<int>("Solstice/BrushPresetGrouping", 0);
        const int initial = grouping->currentIndex();
        grouping->setCurrentIndex(1);
        QCOMPARE(KisConfig(true).readEntry<int>("Solstice/brushesGrouping", 0),
                 int(KisPresetDockerFilters::GroupByBundle));
        QCOMPARE(KisConfig(true).readEntry<int>("Solstice/BrushPresetGrouping", 0), presetGrouping);
        if (allCount > 0)
            QVERIFY(!filters->groupOf(model->index(0, 0)).second.isEmpty());
        grouping->setCurrentIndex(initial);
    }
    void testFilterCombinations()
    {
        QStandardItemModel source;
        auto add = [&](const QString &name, const QString &engine, int storage) {
            auto *item = new QStandardItem(name);
            item->setData(name, Qt::UserRole + KisAbstractResourceModel::Name);
            item->setData(ResourceType::PaintOpPresets, Qt::UserRole + KisAbstractResourceModel::ResourceType);
            item->setData(storage, Qt::UserRole + KisAbstractResourceModel::StorageId);
            item->setData(QVariantMap{{"paintopid", engine}}, Qt::UserRole + KisAbstractResourceModel::MetaData);
            source.appendRow(item);
        };
        add("Soft Pixel", "paintbrush", 10);
        add("Hard Pixel", "paintbrush", 20);
        add("Soft MyPaint", "mypaintbrush", 20);
        add("Smudge", "colorsmudge", 30);
        KisTagFilterResourceProxyModel model(ResourceType::PaintOpPresets);
        model.setSourceModel(&source);
        QCOMPARE(model.rowCount(), 4);
        model.setAdditionalFilters({{"paintopid", {"paintbrush", "mypaintbrush"}}}, true, {10, 20});
        QCOMPARE(model.rowCount(), 3);
        model.setSearchText("Soft");
        model.setSourceModel(&source); // Search normally restores the real resource/tag source.
        QCOMPARE(model.rowCount(), 2);
        model.setAdditionalFilters({{"paintopid", {"paintbrush", "mypaintbrush"}}}, true, {20});
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.index(0, 0).data().toString(), QString("Soft MyPaint"));
        model.setAdditionalFilters({{"paintopid", {}}}, false, {});
        QCOMPARE(model.rowCount(), 0);
        model.setAdditionalFilters({}, true, {});
        QCOMPARE(model.rowCount(), 0);
        model.setAdditionalFilters({}, false, {});
        QCOMPARE(model.rowCount(), 2); // Search is still applied.
        model.setSearchText("");
        model.setSourceModel(&source);
        QCOMPARE(model.rowCount(), 4);
    }
    void testDockerFilterMenus()
    {
        KisPaintOpPresetsChooserPopup docker, toolbar;
        docker.enableStrokePreviewSetting();
        auto *engines = docker.findChild<QToolButton *>("PresetEngineFilter");
        auto *bundles = docker.findChild<QToolButton *>("PresetBundleFilter");
        QVERIFY(engines && bundles);
        QVERIFY(!toolbar.findChild<QToolButton *>("PresetEngineFilter"));
        auto *chooser = docker.findChild<KisPresetChooser *>();
        auto *model = chooser->itemChooser()->tagFilterModel();
        const int allCount = model->rowCount();
        QVERIFY(allCount >= 7);
        auto find = [](QMenu *menu, const QString &key) -> QAction * {
            for (auto *action : menu->actions())
                if (action->isCheckable() && action->data().toString() == key)
                    return action;
            return nullptr;
        };
        auto *menu = engines->menu();
        menu->popup(QPoint(20, 20));
        QTRY_VERIFY(menu->isVisible());
        auto *pixel = find(menu, "paintbrush");
        QVERIFY(pixel && pixel->isChecked());
        const QString output = qEnvironmentVariable("SOLSTICE_PREVIEW_LAYOUT_IMAGE");
        if (!output.isEmpty())
            QVERIFY(menu->grab().save(output + ".engines.png"));
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(pixel).center());
        QVERIFY(menu->isVisible());
        QVERIFY(!pixel->isChecked());
        QCOMPARE(model->rowCount(), allCount - 1);
        menu->setActiveAction(pixel);
        QTest::keyClick(menu, Qt::Key_Space);
        QVERIFY(menu->isVisible());
        QVERIFY(pixel->isChecked());
        QCOMPARE(model->rowCount(), allCount);
        auto *clearAll = menu->findChild<QAction *>("ClearAll");
        QVERIFY(clearAll);
        clearAll->trigger();
        QCOMPARE(model->rowCount(), 0);
        menu->hide();
        menu->popup(QPoint(20, 20));
        QTRY_VERIFY(menu->isVisible());
        pixel = find(menu, "paintbrush");
        auto *mypaint = find(menu, "mypaintbrush");
        QVERIFY(pixel);
        QVERIFY(mypaint);
        pixel->trigger();
        mypaint->trigger();
        QCOMPARE(model->rowCount(), 2);
        menu->hide();
        bundles->menu()->popup(QPoint(20, 20));
        auto *unbundled = find(bundles->menu(), QString());
        QVERIFY(unbundled);
        unbundled->trigger();
        QCOMPARE(model->rowCount(), 0);
        unbundled->trigger();
        QCOMPARE(model->rowCount(), 2);
        bundles->menu()->hide();
        auto *selectAll = menu->findChild<QAction *>("SelectAll");
        QVERIFY(selectAll);
        selectAll->trigger();
        QCOMPARE(model->rowCount(), allCount);
        chooser->setStrokePreviewMode(false);
        QCOMPARE(model->rowCount(), allCount);
        QVERIFY(engines->parentWidget()->parentWidget()->objectName() == "ResourceChooserBottomBar");
    }
    void testPrune()
    {
        QTemporaryDir directory;
        QFile old(directory.filePath("old.none"));
        QVERIFY(old.open(QIODevice::WriteOnly));
        QVERIFY(old.setFileTime(QDateTime::currentDateTimeUtc().addDays(-61), QFileDevice::FileModificationTime));
        old.close();
        QFile fresh(directory.filePath("fresh.none"));
        QVERIFY(fresh.open(QIODevice::WriteOnly));
        fresh.close();
        QFile unrelated(directory.filePath("keep.txt"));
        QVERIFY(unrelated.open(QIODevice::WriteOnly));
        unrelated.close();
        KisBrushStrokePreviewCache::prune(directory.path());
        QVERIFY(!old.exists());
        QVERIFY(fresh.exists());
        QVERIFY(unrelated.exists());
        const QString current = directory.filePath("brush-stroke-previews/v1");
        const QString obsolete = directory.filePath("brush-stroke-previews/v0");
        const QString unrelatedDirectory = directory.filePath("brush-stroke-previews/notes");
        QVERIFY(QDir().mkpath(current));
        QVERIFY(QDir().mkpath(obsolete));
        QVERIFY(QDir().mkpath(unrelatedDirectory));
        KisBrushStrokePreviewCache::prune(current);
        QVERIFY(QDir(current).exists());
        QVERIFY(!QDir(obsolete).exists());
        QVERIFY(QDir(unrelatedDirectory).exists());
    }
    void testDiskCache()
    {
        KisResourceModel model(ResourceType::PaintOpPresets);
        QModelIndex chosen;
        for (int i = 0; i < model.rowCount(); ++i) {
            const auto index = model.index(i, 0);
            auto preset = model.resourceForIndex(index).dynamicCast<KisPaintOpPreset>();
            if (preset && preset->paintOp().id() == "paintbrush") {
                chosen = index;
                break;
            }
        }
        QVERIFY(chosen.isValid());
        const auto request = KisBrushStrokePreviewCache::Request::fromIndex(chosen);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QImage generated;
        QObject firstConsumer, secondConsumer;
        {
            KisBrushStrokePreviewCache cache(directory.path());
            QSignalSpy rendered(&cache, &KisBrushStrokePreviewCache::renderStarted);
            cache.setRequests(&firstConsumer, {request});
            cache.setRequests(&secondConsumer, {request});
            cache.removeConsumer(&firstConsumer);
            QTRY_VERIFY_WITH_TIMEOUT(cache.ready(request), 15000);
            QTRY_VERIFY_WITH_TIMEOUT(!cache.isBusy(), 15000);
            generated = cache.preview(request);
            QCOMPARE(generated.size(), QSize(480, 160));
            QCOMPARE(rendered.size(), 1);
        }
        {
            KisBrushStrokePreviewCache cache(directory.path());
            QSignalSpy rendered(&cache, &KisBrushStrokePreviewCache::renderStarted);
            cache.setRequests(&firstConsumer, {request});
            QTRY_VERIFY_WITH_TIMEOUT(cache.ready(request), 15000);
            QCOMPARE(cache.preview(request), generated);
            QCOMPARE(rendered.size(), 0);
            auto obsolete = request;
            obsolete.md5 = "obsolete";
            cache.setRequests(&firstConsumer, {obsolete});
            QTRY_VERIFY_WITH_TIMEOUT(cache.ready(obsolete), 15000);
            QVERIFY(cache.preview(obsolete).isNull());
            QCOMPARE(QDir(directory.path()).entryList({"*.none"}, QDir::Files).size(), 0);
        }
    }
    void testPresetChangeKeepsOtherPreviews()
    {
        KisResourceModel model(ResourceType::PaintOpPresets);
        QModelIndex first, second;
        for (int i = 0; i < model.rowCount(); ++i) {
            const auto index = model.index(i, 0);
            auto preset = model.resourceForIndex(index).dynamicCast<KisPaintOpPreset>();
            if (!preset || !preset->filename().startsWith("solstice-preview-test-"))
                continue;
            if (preset->paintOp().id() == "paintbrush")
                first = index;
            else if (preset->paintOp().id() == "sketchbrush")
                second = index;
        }
        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        const auto firstRequest = KisBrushStrokePreviewCache::Request::fromIndex(first);
        const auto secondRequest = KisBrushStrokePreviewCache::Request::fromIndex(second);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QObject consumer;
        KisBrushStrokePreviewCache cache(directory.path());
        cache.setRequests(&consumer, {firstRequest, secondRequest});
        QTRY_VERIFY_WITH_TIMEOUT(cache.ready(firstRequest) && cache.ready(secondRequest), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!cache.isBusy(), 15000);
        const QImage firstPreview = cache.preview(firstRequest);
        const QImage secondPreview = cache.preview(secondRequest);
        QCOMPARE(firstPreview.size(), QSize(480, 160));
        QCOMPARE(secondPreview.size(), QSize(480, 160));
        QSignalSpy rendered(&cache, &KisBrushStrokePreviewCache::renderStarted);

        // Reload Original Preset: the model reports the preset's row without
        // roles. Its saved bytes are the same, so both previews stay, at
        // once and without a new lookup.
        auto preset = model.resourceForIndex(first);
        QVERIFY(preset);
        QVERIFY(model.reloadResource(preset));
        QVERIFY(cache.ready(firstRequest));
        QVERIFY(cache.ready(secondRequest));
        QCOMPARE(cache.preview(firstRequest), firstPreview);
        QCOMPARE(cache.preview(secondRequest), secondPreview);
        QTest::qWait(300);
        QVERIFY(cache.ready(firstRequest));
        QVERIFY(cache.ready(secondRequest));
        QCOMPARE(rendered.size(), 0);

        // A brush tip may be linked by any preset: every preview is looked up
        // again, and found on disk.
        auto *brushes = KisResourceModelProvider::resourceModel(ResourceType::Brushes);
        Q_EMIT brushes->dataChanged(QModelIndex(), QModelIndex());
        QVERIFY(!cache.ready(firstRequest));
        QVERIFY(!cache.ready(secondRequest));
        QTRY_VERIFY_WITH_TIMEOUT(cache.ready(firstRequest) && cache.ready(secondRequest), 30000);
        QCOMPARE(cache.preview(firstRequest), firstPreview);
        QCOMPARE(cache.preview(secondRequest), secondPreview);
        QCOMPARE(rendered.size(), 0);
    }
    void testNoPreviewCache()
    {
        KisResourceModel model(ResourceType::PaintOpPresets);
        QModelIndex chosen;
        for (int i = 0; i < model.rowCount(); ++i) {
            auto index = model.index(i, 0);
            auto preset = model.resourceForIndex(index).dynamicCast<KisPaintOpPreset>();
            if (preset && preset->paintOp().id() == "duplicate") {
                chosen = index;
                break;
            }
        }
        QVERIFY(chosen.isValid());
        const auto request = KisBrushStrokePreviewCache::Request::fromIndex(chosen);
        QTemporaryDir directory;
        QObject consumer;
        for (int pass = 0; pass < 2; ++pass) {
            KisBrushStrokePreviewCache cache(directory.path());
            QSignalSpy rendered(&cache, &KisBrushStrokePreviewCache::renderStarted);
            cache.setRequests(&consumer, {request});
            QTRY_VERIFY_WITH_TIMEOUT(cache.ready(request), 15000);
            QTRY_VERIFY_WITH_TIMEOUT(!cache.isBusy(), 15000);
            QVERIFY(cache.preview(request).isNull());
            QCOMPARE(rendered.size(), pass ? 0 : 1);
            QCOMPARE(QDir(directory.path()).entryList({"*.none"}, QDir::Files).size(), 1);
        }
    }
    void testPersistentCache()
    {
        const QString path = qEnvironmentVariable("SOLSTICE_PREVIEW_PERSISTENT_TEST_DIR");
        if (path.isEmpty())
            return; // The separate-process check explicitly supplies its scratch directory.
        KisResourceModel model(ResourceType::PaintOpPresets);
        const auto request = KisBrushStrokePreviewCache::Request::fromIndex(model.index(0, 0));
        QVERIFY(!request.md5.isEmpty());
        QObject consumer;
        KisBrushStrokePreviewCache cache(path);
        QSignalSpy rendered(&cache, &KisBrushStrokePreviewCache::renderStarted);
        cache.setRequests(&consumer, {request});
        QTRY_VERIFY_WITH_TIMEOUT(cache.ready(request), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!cache.isBusy(), 15000);
        if (qEnvironmentVariableIsSet("SOLSTICE_PREVIEW_EXPECT_CACHE_HIT"))
            QCOMPARE(rendered.size(), 0);
    }
    void testDockerLayout()
    {
        KisPresetChooser chooser;
        chooser.setStrokePreviewMode(true);
        chooser.resize(440, 440);
        chooser.show();
        auto *model = chooser.itemChooser()->itemView()->model();
        QVERIFY(model->rowCount() >= 6);
        auto *cache = KisBrushStrokePreviewCache::instance();
        for (int i = 0; i < model->rowCount(); ++i) {
            auto request = KisBrushStrokePreviewCache::Request::fromIndex(model->index(i, 0));
            QTRY_VERIFY_WITH_TIMEOUT(cache->ready(request), 15000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!cache->isBusy(), 15000);
        const QString output = qEnvironmentVariable("SOLSTICE_PREVIEW_LAYOUT_IMAGE");
        if (!output.isEmpty())
            QVERIFY(chooser.grab().save(output));
        chooser.hide();
    }
    void testPaintingPriority()
    {
        KisResourceModel model(ResourceType::PaintOpPresets);
        QModelIndex chosen;
        for (int i = 0; i < model.rowCount(); ++i) {
            const auto index = model.index(i, 0);
            auto preset = model.resourceForIndex(index).dynamicCast<KisPaintOpPreset>();
            if (preset && preset->paintOp().id() == "paintbrush") {
                chosen = index;
                break;
            }
        }
        QVERIFY(chosen.isValid());
        const auto request = KisBrushStrokePreviewCache::Request::fromIndex(chosen);
        QTemporaryDir directory;
        QObject consumer;
        auto *part = KisPart::instance();
        QScopedPointer<KisDocument> document(part->createDocument());
        KisImageSP userImage(new KisImage(nullptr, 64, 64, KoColorSpaceRegistry::instance()->rgb8(), "User image"));
        document->setCurrentImage(userImage);
        part->addDocument(document.data());
        auto remove = qScopeGuard([&]() {
            part->removeDocument(document.data(), false);
        });
        class HeldStroke : public KisSimpleStrokeStrategy
        {
        public:
            QSemaphore *gate;
            explicit HeldStroke(QSemaphore *gate)
                : KisSimpleStrokeStrategy(QLatin1String("preview-priority-test"))
                , gate(gate)
            {
                enableJob(JOB_INIT, true, KisStrokeJobData::BARRIER);
            }
            void initStrokeCallback() override
            {
                gate->acquire();
            }
        };
        QSemaphore gate;
        auto stroke = userImage->startStroke(new HeldStroke(&gate));
        userImage->endStroke(stroke);
        KisBrushStrokePreviewCache cache(directory.path());
        QSignalSpy rendered(&cache, &KisBrushStrokePreviewCache::renderStarted);
        cache.setRequests(&consumer, {request});
        QTest::qWait(200);
        const int countWhilePainting = rendered.size();
        gate.release(); // Always release before assertions/destructors.
        QTRY_VERIFY_WITH_TIMEOUT(userImage->isIdle(), 5000);
        QCOMPARE(countWhilePainting, 0);
        bool interrupted = false;
        connect(&cache, &KisBrushStrokePreviewCache::renderStarted, &cache, [&]() {
            if (interrupted)
                return;
            interrupted = true;
            auto otherStroke = userImage->startStroke(new KisSimpleStrokeStrategy(QLatin1String("interrupt-preview")));
            userImage->endStroke(otherStroke);
        });
        QTRY_VERIFY_WITH_TIMEOUT(cache.ready(request), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!cache.isBusy(), 15000);
        QVERIFY(interrupted);
        QCOMPARE(rendered.size(), 2);
        QCOMPARE(QDir(directory.path()).entryList({"*.none"}, QDir::Files).size(), 0);
    }
    void testRealBundleFilters()
    {
        QTemporaryDir directory;
        KisResourceModel resources(ResourceType::PaintOpPresets);
        auto preset = resources.resourceForIndex(resources.index(0, 0));
        QVERIFY(preset);
        auto *storages = KisStorageModel::instance();
        // KISTEST_MAIN isolates the resource directory/database from the user.
        for (int i = 0; i < 2; ++i) {
            const QString path = directory.filePath(QString("preview-filter-%1.bundle").arg(i));
            KoResourceBundle bundle(path);
            bundle.setMetaData(KisResourceStorage::s_meta_name, QString("Preview filter %1").arg(i));
            bundle.setThumbnail(preset->image());
            bundle.addResource(ResourceType::PaintOpPresets,
                               preset->filename(),
                               {},
                               preset->md5Sum(),
                               preset->resourceId());
            QVERIFY(bundle.save());
            QVERIFY(storages->importStorage(path, KisStorageModel::None));
        }
        KisPaintOpPresetsChooserPopup docker;
        docker.enableStrokePreviewSetting();
        auto *button = docker.findChild<QToolButton *>("PresetBundleFilter");
        auto *model = docker.findChild<KisPresetChooser *>()->itemChooser()->tagFilterModel();
        auto *menu = button->menu();
        menu->popup(QPoint(20, 20));
        QTRY_VERIFY(menu->isVisible());
        QList<QAction *> bundles;
        for (auto *action : menu->actions())
            if (action->isCheckable() && action->data().toString().contains("preview-filter-"))
                bundles << action;
        QCOMPARE(bundles.size(), 2);
        const QString output = qEnvironmentVariable("SOLSTICE_PREVIEW_LAYOUT_IMAGE");
        if (!output.isEmpty())
            QVERIFY(menu->grab().save(output + ".bundles.png"));
        auto *clearAll = menu->findChild<QAction *>("ClearAll");
        QVERIFY(clearAll);
        clearAll->trigger();
        menu->hide();
        menu->popup(QPoint(20, 20));
        bundles.clear();
        for (auto *action : menu->actions())
            if (action->isCheckable() && action->data().toString().contains("preview-filter-"))
                bundles << action;
        QCOMPARE(bundles.size(), 2);
        bundles[0]->trigger();
        QCOMPARE(model->rowCount(), 1);
        bundles[1]->trigger();
        QCOMPARE(model->rowCount(), 1); // Identical resources stay deduplicated.
        const auto selectedIds = KisTagFilterResourceProxyModel::activeStorageIdsForIndex(model->index(0, 0));
        QCOMPARE(selectedIds.size(), 3); // Local copy and the two bundles.
        for (int row = 0; row < storages->rowCount(); ++row)
            if (selectedIds.contains(storages->index(row, KisStorageModel::Id).data().toInt()))
                QVERIFY(storages->index(row, KisStorageModel::Active).data().toBool());
        menu->hide();
    }
};
KISTEST_MAIN(KisBrushStrokePreviewTest)
#include "KisBrushStrokePreviewTest.moc"
