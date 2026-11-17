/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisRemoveDustFilter.h"
#include "KisRemoveDustConfigWidget.h"
#include <KisGlobalResourcesInterface.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <QComboBox>
#include <QSignalSpy>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_color_button.h>
#include <kis_filter_mask.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_selection.h>
#include <kis_slider_spin_box.h>
#include <simpletest.h>
#include <testutil.h>
namespace
{
QByteArray pixels(KisPaintDeviceSP dev, const QRect &rect)
{
    QByteArray data(rect.width() * rect.height() * dev->pixelSize(), '\0');
    dev->readBytes(reinterpret_cast<quint8 *>(data.data()), rect);
    return data;
}
QColor pixel(KisPaintDeviceSP dev, int x, int y)
{
    QColor c;
    dev->pixel(x, y, &c);
    return c;
}
KisFilterConfigurationSP settings(const QString &mode)
{
    auto config = KisRemoveDustFilter().defaultConfiguration(KisGlobalResourcesInterface::instance());
    config->setProperty("mode", mode);
    return config->cloneWithResourcesSnapshot();
}
// Specks of 1..6 pixels on transparency, plus holes inside a red block.
void paintPattern(KisPaintDeviceSP dev)
{
    const auto *cs = dev->colorSpace();
    for (int i = 0; i < 6; ++i)
        dev->fill(QRect(4 + i * 10, 4, i + 1, i + 1), KoColor(Qt::black, cs));
    dev->fill(QRect(0, 30, 64, 34), KoColor(Qt::red, cs));
    dev->fill(QRect(10, 40, 2, 2), KoColor(Qt::transparent, cs));
    dev->fill(QRect(30, 50, 1, 1), KoColor(QColor(255, 0, 0, 90), cs));
}
} // namespace
class KisRemoveDustFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        // Filter masks look the filter up by id; do not depend on an installed plugin.
        if (!KisFilterRegistry::instance()->contains("solstice_remove_dust"))
            KisFilterRegistry::instance()->add(new KisRemoveDustFilter());
    }
    void testRemovesSpecks()
    {
        const QRect rect(0, 0, 64, 64);
        KisPaintDeviceSP dev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
        paintPattern(dev);
        KisRemoveDustFilter().process(dev, rect, settings("transparency"));
        for (int i = 0; i < 6; ++i)
            QCOMPARE(pixel(dev, 4 + i * 10, 4).alpha(), i < 4 ? 0 : 255);
        QCOMPARE(pixel(dev, 20, 60), QColor(Qt::red));
    }
    void testFillsGaps()
    {
        const QRect rect(0, 0, 64, 64);
        KisPaintDeviceSP surrounding = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
        paintPattern(surrounding);
        KisPaintDeviceSP colored = new KisPaintDevice(*surrounding);
        KisRemoveDustFilter().process(surrounding, rect, settings("gaps_surrounding"));
        QCOMPARE(pixel(surrounding, 10, 40), QColor(Qt::red));
        QCOMPARE(pixel(surrounding, 30, 50), QColor(Qt::red));
        QCOMPARE(pixel(surrounding, 4, 4), QColor(Qt::black));
        auto config = settings("gaps_color");
        config->setProperty("color", QVariant::fromValue(KoColor(Qt::green, KoColorSpaceRegistry::instance()->rgb8())));
        KisRemoveDustFilter().process(colored, rect, config);
        QCOMPARE(pixel(colored, 11, 41), QColor(Qt::green));
        QCOMPARE(pixel(colored, 30, 50), QColor(Qt::green));
    }
    void testProperties()
    {
        KisRemoveDustFilter filter;
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        QVERIFY(filter.supportsThreading());
        QVERIFY(filter.supportsAdjustmentLayers());
        QVERIFY(!filter.supportsLevelOfDetail(settings("transparency"), 1));
        QCOMPARE(settings("transparency")->getDouble("tolerance"), 2.0);
        QCOMPARE(KisRemoveDustFilter::options(settings("transparency")).tolerance, 0.02);
        QCOMPARE(KisRemoveDustConfigWidget(nullptr, nullptr).configuration()->getDouble("tolerance"), 2.0);
        QVERIFY(!filter.needsTransparentPixels(settings("transparency"), cs));
        QVERIFY(!filter.needsTransparentPixels(settings("white"), cs));
        QVERIFY(filter.needsTransparentPixels(settings("gaps_surrounding"), cs));
        QVERIFY(filter.needsTransparentPixels(settings("gaps_color"), cs));
        auto config = settings("unknown");
        QCOMPARE(KisRemoveDustFilter::options(config).mode, KisDustDetector::DustOnTransparency);
        config->setProperty("dustSize", 7);
        QCOMPARE(filter.neededRect(QRect(0, 0, 10, 10), config, 0), QRect(-8, -8, 26, 26));
        QCOMPARE(filter.changedRect(QRect(0, 0, 10, 10), config, 0), QRect(-8, -8, 26, 26));
        config->setProperty("tolerance", 250.0);
        QCOMPARE(KisRemoveDustFilter::options(config).tolerance, 1.0);
    }
    void testConfigurationWidget()
    {
        auto config = settings("gaps_color");
        config->setProperty("dustSize", 13);
        config->setProperty("tolerance", 12.5);
        config->setProperty("color", QVariant::fromValue(KoColor(Qt::blue, KoColorSpaceRegistry::instance()->rgb8())));
        KisRemoveDustConfigWidget widget(nullptr, nullptr);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        QVERIFY(widget.findChild<KisColorButton *>("removeDustColor")->isEnabled());
        auto saved = widget.configuration();
        QCOMPARE(saved->getString("mode"), QString("gaps_color"));
        QCOMPARE(saved->getInt("dustSize"), 13);
        QCOMPARE(saved->getDouble("tolerance"), 12.5);
        QCOMPARE(saved->getColor("color").toQColor(), QColor(Qt::blue));
        widget.findChild<QComboBox *>("removeDustMode")->setCurrentIndex(1);
        QCOMPARE(spy.count(), 1);
        QVERIFY(!widget.findChild<KisColorButton *>("removeDustColor")->isEnabled());
        QCOMPARE(widget.configuration()->getString("mode"), QString("white"));
    }
    void testFilterMask()
    {
        const QRect rect(0, 0, 64, 64);
        auto config = settings("transparency");
        TestUtil::MaskParent parent(rect);
        KisFilterMaskSP mask = new KisFilterMask(parent.image, "Remove Dust");
        parent.image->addNode(mask, parent.layer);
        mask->setFilter(config->cloneWithResourcesSnapshot());
        mask->initSelection(parent.layer);
        mask->select(rect, MAX_SELECTED);
        KisPaintDeviceSP original = new KisPaintDevice(parent.layer->colorSpace());
        paintPattern(original);
        KisPaintDeviceSP expected = new KisPaintDevice(*original), actual = new KisPaintDevice(*original);
        KisRemoveDustFilter().process(expected, rect, config);
        mask->apply(actual, rect, rect, KisNode::N_FILTHY, KisRenderPassFlag::None);
        QCOMPARE(pixels(actual, rect), pixels(expected, rect));
    }
};
SIMPLE_TEST_MAIN(KisRemoveDustFilterTest)
#include "KisRemoveDustFilterTest.moc"
