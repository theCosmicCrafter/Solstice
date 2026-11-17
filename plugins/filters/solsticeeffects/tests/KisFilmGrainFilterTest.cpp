/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisFilmGrainFilter.h"
#include "KisFilmGrainConfigWidget.h"

#include <KisGlobalResourcesInterface.h>
#include <KoChannelInfo.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <simpletest.h>

#include <algorithm>
#include <cmath>

namespace
{
QByteArray pixels(KisPaintDeviceSP device, const QRect &rect)
{
    QByteArray data(rect.width() * rect.height() * device->pixelSize(), '\0');
    device->readBytes(reinterpret_cast<quint8 *>(data.data()), rect);
    return data;
}

KisPaintDeviceSP filled(const KoColorSpace *cs, const QRect &rect, float alpha = 0.625f, float value = 0.5f)
{
    KoColor color(cs);
    QVector<float> values(cs->channelCount(), value);
    const auto channels = cs->channels();
    for (int i = 0; i < channels.size(); ++i) {
        if (channels[i]->channelType() == KoChannelInfo::ALPHA) {
            values[i] = alpha;
        }
    }
    cs->fromNormalisedChannelsValue(color.data(), values);
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    device->fill(rect, color);
    return device;
}

KisFilterConfigurationSP settings(KisFilmGrainFilter &filter)
{
    auto config = filter.defaultConfiguration(KisGlobalResourcesInterface::instance());
    config->setProperty("seed", 1234567);
    return config->cloneWithResourcesSnapshot();
}
} // namespace

class KisFilmGrainFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testInstalledPlugin()
    {
        const auto filter = KisFilterRegistry::instance()->value("solstice_film_grain");
        QVERIFY2(filter, "Install kritasolsticeeffectsfilter before running the integration test.");
        QCOMPARE(filter->menuCategory().id(), QString("other_filters"));
        QVERIFY(filter->supportsAdjustmentLayers());
        auto config = filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
        QVERIFY(!filter->supportsLevelOfDetail(config, 1));
        const QRect rect(0, 0, 8, 8);
        auto device = filled(KoColorSpaceRegistry::instance()->rgb8(), rect);
        const auto before = pixels(device, rect);
        filter->process(device, rect, config->cloneWithResourcesSnapshot());
        QVERIFY(pixels(device, rect) != before);
    }

    void testFormats_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<QString>("depth");
        for (const QString &model : {QString("RGBA"), QString("GRAYA")}) {
            for (const QString &depth : {QString("U8"), QString("U16"), QString("F16"), QString("F32")}) {
                QTest::newRow(qPrintable(model + '-' + depth)) << model << depth;
            }
        }
    }

    void testFormats()
    {
        QFETCH(QString, model);
        QFETCH(QString, depth);
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace(model, depth, QString());
        QVERIFY(cs);
        const QRect rect(-69, -37, 137, 83);
        KisFilmGrainFilter filter;
        auto config = settings(filter);
        auto whole = filled(cs, rect);
        const QByteArray before = pixels(whole, rect);
        auto tiled = filled(cs, rect);
        filter.process(whole, rect, config);
        // Deliberately non-tile-aligned patches and negative coordinates.
        for (int y = rect.top(); y <= rect.bottom(); y += 17) {
            for (int x = rect.left(); x <= rect.right(); x += 23) {
                filter.process(tiled, rect.intersected(QRect(x, y, 23, 17)), config);
            }
        }
        const QByteArray after = pixels(whole, rect);
        QCOMPARE(pixels(tiled, rect), after);
        QVERIFY(after != before);
        for (const auto *channel : cs->channels()) {
            if (channel->channelType() == KoChannelInfo::ALPHA) {
                for (int offset = 0; offset < after.size(); offset += cs->pixelSize()) {
                    QCOMPARE(after.mid(offset + channel->pos(), channel->size()),
                             before.mid(offset + channel->pos(), channel->size()));
                }
            }
        }

        auto zero = filled(cs, rect);
        config->setProperty("strength", 0.0);
        filter.process(zero, rect, config);
        QCOMPARE(pixels(zero, rect), before);

        auto transparent = filled(cs, rect, 0.0f);
        const QByteArray hidden = pixels(transparent, rect);
        config->setProperty("strength", 100.0);
        filter.process(transparent, rect, config);
        QCOMPARE(pixels(transparent, rect), hidden);
    }

    void testChannelsAndMonochrome()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(0, 0, 31, 19);
        KisFilmGrainFilter filter;
        auto config = settings(filter);
        config->setProperty("monochrome", true);
        auto device = filled(cs, rect);
        filter.process(device, rect, config);
        const QByteArray mono = pixels(device, rect);
        for (int offset = 0; offset < mono.size(); offset += cs->pixelSize()) {
            QCOMPARE(mono[offset], mono[offset + 1]);
            QCOMPARE(mono[offset], mono[offset + 2]);
        }

        device = filled(cs, rect);
        const QByteArray before = pixels(device, rect);
        QBitArray flags(cs->channelCount(), false);
        flags.setBit(0, true);
        config->setChannelFlags(flags);
        filter.process(device, rect, config);
        const QByteArray after = pixels(device, rect);
        QVERIFY(after != before);
        for (int offset = 0; offset < after.size(); offset += cs->pixelSize()) {
            QCOMPARE(after.mid(offset + 1, 3), before.mid(offset + 1, 3));
        }
        flags.fill(false);
        config->setChannelFlags(flags);
        filter.process(device, rect, config);
        QCOMPARE(pixels(device, rect), after);
    }

    void testFloatRange()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        QVERIFY(cs);
        const QRect rect(0, 0, 19, 7);
        KisFilmGrainFilter filter;
        auto config = settings(filter);
        for (float value : {-2.0f, 3.0f}) {
            auto device = filled(cs, rect, 0.625f, value);
            filter.process(device, rect, config);
            const QByteArray after = pixels(device, rect);
            QVector<float> values(cs->channelCount());
            for (int offset = 0; offset < after.size(); offset += cs->pixelSize()) {
                cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(after.constData() + offset), values);
                for (int i = 0; i < 3; ++i) {
                    QVERIFY(std::abs(values[i] - value) <= 0.15001f);
                }
                QCOMPARE(values[3], 0.625f);
            }
        }
    }

    void testSelectionAndSeed()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(-10, -10, 40, 40);
        KisFilmGrainFilter filter;
        auto config = settings(filter);
        auto source = filled(cs, rect);
        auto selected = filled(cs, rect);
        const QByteArray before = pixels(source, rect);
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(QRect(0, 0, 10, 10));
        filter.process(source, selected, selection, rect, config);
        QCOMPARE(pixels(source, rect), before);
        QCOMPARE(pixels(selected, QRect(-10, -10, 40, 10)), pixels(source, QRect(-10, -10, 40, 10)));
        QVERIFY(pixels(selected, QRect(0, 0, 10, 10)) != pixels(source, QRect(0, 0, 10, 10)));

        auto first = filled(cs, rect);
        auto second = filled(cs, rect);
        filter.process(first, rect, config);
        config->setProperty("seed", 7654321);
        filter.process(second, rect, config);
        QVERIFY(pixels(first, rect) != pixels(second, rect));
    }

    void testConfiguration()
    {
        KisFilmGrainFilter filter;
        auto config = settings(filter);
        config->setProperty("strength", 42.25);
        config->setProperty("monochrome", true);
        KisFilmGrainConfigWidget widget(nullptr);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        const auto saved = widget.configuration();
        QCOMPARE(saved->getInt("seed"), 1234567);
        QCOMPARE(saved->getDouble("strength"), 42.25);
        QVERIFY(saved->getBool("monochrome"));
        auto restored = filter.factoryConfiguration(KisGlobalResourcesInterface::instance());
        restored->fromXML(saved->toXML());
        QCOMPARE(restored->name(), QString("solstice_film_grain"));
        const QRect rect(-7, -9, 22, 29);
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        auto first = filled(cs, rect);
        auto second = filled(cs, rect);
        filter.process(first, rect, config);
        filter.process(second, rect, restored->cloneWithResourcesSnapshot());
        QCOMPARE(pixels(first, rect), pixels(second, rect));
    }

    void benchmarkCpu()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_FILM_GRAIN")) {
            QSKIP("Set SOLSTICE_BENCHMARK_FILM_GRAIN=1 for full-canvas CPU measurements.");
        }
        KisFilmGrainFilter filter;
        auto config = settings(filter);
        for (const QString &depth : {QString("U8"), QString("F32")}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
            QVERIFY(cs);
            for (const QSize size : {QSize(2480, 3508), QSize(4096, 4096)}) {
                QList<qint64> times;
                for (int run = 0; run < 3; ++run) {
                    auto device = filled(cs, QRect(QPoint(), size));
                    QElapsedTimer timer;
                    timer.start();
                    filter.process(device, QRect(QPoint(), size), config);
                    times.append(timer.elapsed());
                }
                std::sort(times.begin(), times.end());
                qInfo() << "Film Grain CPU single call" << depth << size << "median ms" << times[1];
            }
        }
    }
};

SIMPLE_TEST_MAIN(KisFilmGrainFilterTest)
#include "KisFilmGrainFilterTest.moc"
