/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <KisGlobalResourcesInterface.h>
#include <KisReferenceImage.h>
#include <KisReferenceFilterTemplates.h>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <KoColor.h>
#include <QDomDocument>
#include <filter/kis_filter.h>
#include <filter/kis_filter_registry.h>
#include <kis_coordinates_converter.h>
#include <kis_paint_device.h>
#include <simpletest.h>

class TestReferenceFilter : public KisFilter
{
public:
    TestReferenceFilter(QString id, bool multiply)
        : KisFilter(KoID(id, id), KoID("test", "test"), id)
        , m_multiply(multiply)
    {
        setSupportsAdjustmentLayers(true);
    }
    void
    processImpl(KisPaintDeviceSP device, const QRect &rect, const KisFilterConfigurationSP, KoUpdater *) const override
    {
        QColor color = device->convertToQImage(nullptr, rect).pixelColor(0, 0);
        color.setRed(m_multiply ? color.red() * 2 : color.red() + 20);
        device->fill(rect, KoColor(color, device->colorSpace()));
    }

private:
    bool m_multiply;
};

class KisReferenceImageFiltersTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        KisFilterRegistry::instance()->add(new TestReferenceFilter("reference-test-add", false));
        KisFilterRegistry::instance()->add(new TestReferenceFilter("reference-test-multiply", true));
    }
    void orderedRendering()
    {
        QImage source(8, 8, QImage::Format_ARGB32);
        source.fill(QColor(30, 40, 50));
        QVector<KisReferenceImage::FilterEffect> effects;
        for (const QString &id : {QString("reference-test-add"), QString("reference-test-multiply")}) {
            auto config =
                KisFilterRegistry::instance()->value(id)->factoryConfiguration(KisGlobalResourcesInterface::instance());
            effects.append({id, config->toXML(), true});
        }
        QCOMPARE(KisReferenceImage::renderFilterEffects(source, effects).pixelColor(0, 0).red(), 100);
        effects.swapItemsAt(0, 1);
        QCOMPARE(KisReferenceImage::renderFilterEffects(source, effects).pixelColor(0, 0).red(), 80);
        effects[0].enabled = false;
        QCOMPARE(KisReferenceImage::renderFilterEffects(source, effects).pixelColor(0, 0).red(), 50);
        QCOMPARE(source.pixelColor(0, 0).red(), 30);
    }
    void persistenceAndPreviewIsolation()
    {
        KisReferenceImage image;
        image.setSize(QSizeF(120, 240));
        image.setTransformation(QTransform().translate(32, 64).rotate(30));
        const QVector<KisReferenceImage::FilterEffect> effects = {{"missing-filter", "<params>&saved</params>", false},
                                                                  {"levels", "<params/>", true}};
        image.setFilterEffects(effects, false);
        image.previewFilterEffects({}, true);
        QDomDocument xml;
        auto root = xml.createElement("root");
        xml.appendChild(root);
        image.saveXml(xml, root, 0);
        QScopedPointer<KisReferenceImage> restored(KisReferenceImage::fromXml(root.firstChildElement()));
        QCOMPARE(restored->filterEffects().size(), 2);
        QCOMPARE(restored->filterEffects()[0].configuration, effects[0].configuration);
        QVERIFY(!restored->filterEffects()[0].enabled);
        QVERIFY(!restored->filtersEnabled());
        QCOMPARE(restored->size(), image.size());
        const QPointF originalPoint = image.transformation().map(QPointF(10, 20));
        const QPointF restoredPoint = restored->transformation().map(QPointF(10, 20));
        QVERIFY(QLineF(originalPoint, restoredPoint).length() < 0.0001);
        QScopedPointer<KisReferenceImage> clone(static_cast<KisReferenceImage *>(image.cloneShape()));
        clone->setFilterEffects({}, true);
        QCOMPARE(image.filterEffects().size(), 2);
    }
    void legacyLoading()
    {
        QDomDocument xml;
        QVERIFY(xml.setContent(QString("<referenceimage width='5' height='6'/>")));
        QScopedPointer<KisReferenceImage> image(KisReferenceImage::fromXml(xml.documentElement()));
        QVERIFY(image->filterEffects().isEmpty());
        QVERIFY(image->filtersEnabled());
    }
    void asynchronousInvalidation()
    {
        QImage source(8, 8, QImage::Format_ARGB32);
        source.fill(QColor(30, 40, 50));
        KisCoordinatesConverter converter;
        converter.setImageResolution(1.0, 1.0);
        QScopedPointer<KisReferenceImage> image(KisReferenceImage::fromQImage(converter, source));
        QVERIFY(image);
        auto config = KisFilterRegistry::instance()
                          ->value("reference-test-add")
                          ->factoryConfiguration(KisGlobalResourcesInterface::instance());
        QVector<KisReferenceImage::FilterEffect> effects = {{"reference-test-add", config->toXML(), true}};
        image->setFilterEffects(effects, true);
        image->getPixel(QPointF(0, 0)); // Start an asynchronous job.
        image->setFilterEffects({}, false); // Its result must not re-enable the effect.
        QTest::qWait(100);
        QCOMPARE(image->getPixel(QPointF(0, 0)).red(), 30);
        image->setFilterEffects(effects, true);
        QTRY_COMPARE(image->getPixel(QPointF(0, 0)).red(), 50);
        QCOMPARE(image->getImage(), source);
        image->previewFilterEffects({}, false);
        QCOMPARE(image->getPixel(QPointF(0, 0)).red(), 30);
        image->setFilterEffects(effects, true);
        image->getPixel(QPointF(0, 0));
        image.reset(); // The worker must not retain a pointer to the shape.
        QTest::qWait(100);
    }
    void templatesRoundTripAndReuse()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        KisReferenceFilterTemplates store(directory.path());
        KisReferenceFilterTemplates::Template original;
        original.name = QString::fromUtf8("../Study & 色調");
        original.enabled = false;
        original.effects = {{"missing-plugin", "<params>&saved</params>", false},
                            {"levels", "<params/>", true},
                            {"levels", "<different/>", true}};
        QString error;
        QVERIFY2(store.save(original, false, &error), qPrintable(error));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files).size(), 1);
        KisReferenceFilterTemplates reopened(directory.path());
        QCOMPARE(reopened.names(), QStringList{original.name});
        KisReferenceFilterTemplates::Template loaded;
        QVERIFY2(reopened.load(original.name, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.effects.size(), 3);
        QCOMPARE(loaded.effects[0].id, original.effects[0].id);
        QCOMPARE(loaded.effects[0].configuration, original.effects[0].configuration);
        QVERIFY(!loaded.effects[0].enabled);
        QCOMPARE(loaded.effects[2].configuration, original.effects[2].configuration);
        QVERIFY(!loaded.enabled);
        KisReferenceImage first, second;
        first.setFilterEffects(loaded.effects, loaded.enabled);
        second.setFilterEffects(loaded.effects, loaded.enabled);
        first.setFilterEffects({}, true);
        QCOMPARE(second.filterEffects().size(), 3);
        original.effects.clear();
        QVERIFY(!store.save(original, false, &error));
        QVERIFY(store.load(original.name, &loaded, &error));
        QCOMPARE(loaded.effects.size(), 3);
        QVERIFY(store.save(original, true, &error));
        QVERIFY(store.load(original.name, &loaded, &error));
        QVERIFY(loaded.effects.isEmpty());
        QVERIFY(store.remove(original.name, &error));
        QVERIFY(reopened.names().isEmpty());
        QCOMPARE(second.filterEffects().size(), 3);
    }
    void templatesInvalidDataAndNames()
    {
        QTemporaryDir directory;
        KisReferenceFilterTemplates store(directory.path());
        QString error;
        QVERIFY(!store.save({"  ", {}, true}, false, &error));
        QVERIFY(store.save({"CON", {}, true}, false, &error));
        QVERIFY(store.contains(" con "));
        QVERIFY(!store.save({"con", {}, true}, false, &error));
        const auto paths = QDir(directory.path()).entryList({"*.json"}, QDir::Files);
        QCOMPARE(paths.size(), 1);
        QFile file(QDir(directory.path()).filePath(paths.first()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"version\":99,\"name\":\"CON\",\"effects\":[],\"enabled\":true}");
        file.close();
        KisReferenceFilterTemplates::Template unchanged{"sentinel", {}, false};
        QVERIFY(!store.load("CON", &unchanged, &error));
        QCOMPARE(unchanged.name, QString("sentinel"));
        QVERIFY(store.names().isEmpty());
        QVERIFY(store.contains("CON")); // Unsupported files are never deleted.
        KisReferenceFilterTemplates blocked(file.fileName());
        QVERIFY(!blocked.save({"test", {}, true}, false, &error));
    }
};
SIMPLE_TEST_MAIN(KisReferenceImageFiltersTest)
#include "KisReferenceImageFiltersTest.moc"
