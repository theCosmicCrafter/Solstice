/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisChromaticAberrationFilter.h"
#include "KisChromaticAberrationConfigWidget.h"

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
#include <kis_transaction.h>
#include <simpletest.h>

#include <algorithm>
#include <cmath>
#include <future>
#include <memory>

namespace
{
QByteArray pixels(KisPaintDeviceSP device, const QRect &rect)
{
    QByteArray result(rect.width() * rect.height() * device->pixelSize(), '\0');
    device->readBytes(reinterpret_cast<quint8 *>(result.data()), rect);
    return result;
}

// A varying source with alpha edges and nonzero hidden RGB. Values stay in
// gamut so integer formats and profile-conversion fixtures are meaningful.
KisPaintDeviceSP pattern(const KoColorSpace *cs, const QRect &rect)
{
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    // Exercise moved layers, including their raw-copy path for float/HDR.
    device->setX(13);
    device->setY(-11);
    QByteArray data(rect.width() * rect.height() * cs->pixelSize(), '\0');
    QVector<float> values(cs->channelCount());
    for (int y = 0; y < rect.height(); ++y) {
        for (int x = 0; x < rect.width(); ++x) {
            for (int c = 0; c < values.size(); ++c) {
                values[c] = cs->channels()[c]->channelType() == KoChannelInfo::ALPHA
                    ? float((x + y) % 4) / 3.0f
                    : float((x * 7 + y * 13 + c * 17) % 101) / 100.0f;
            }
            cs->fromNormalisedChannelsValue(reinterpret_cast<quint8 *>(data.data())
                                                + (y * rect.width() + x) * cs->pixelSize(),
                                            values);
        }
    }
    device->writeBytes(reinterpret_cast<const quint8 *>(data.constData()), rect);
    return device;
}

KisFilterConfigurationSP settings(KisChromaticAberrationFilter &filter, int mode = 0)
{
    auto config = filter.defaultConfiguration(KisGlobalResourcesInterface::instance());
    config->setProperty("mode", mode);
    config->setProperty("strength", 3.75);
    config->setProperty("angle", 37.0);
    config->setProperty("centerX", -7.25);
    config->setProperty("centerY", 8.5);
    config->setProperty("radius", 40.0);
    return config->cloneWithResourcesSnapshot();
}

QVector<float> valueAt(KisPaintDeviceSP device, int x, int y)
{
    const auto data = pixels(device, QRect(x, y, 1, 1));
    QVector<float> values(device->colorSpace()->channelCount());
    device->colorSpace()->normalisedChannelsValue(reinterpret_cast<const quint8 *>(data.constData()), values);
    return values;
}
} // namespace

class KisChromaticAberrationFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testInstalledPlugin()
    {
        auto filter = KisFilterRegistry::instance()->value("solstice_chromatic_aberration");
        QVERIFY2(filter, "Install kritasolsticeeffectsfilter before running this integration test.");
        QVERIFY(filter->supportsAdjustmentLayers());
        QVERIFY(filter->supportsThreading());
        QVERIFY(
            !filter->supportsLevelOfDetail(filter->defaultConfiguration(KisGlobalResourcesInterface::instance()), 1));
        const QRect rect(0, 0, 32, 32);
        auto device = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
        const auto before = pixels(device, rect);
        filter->process(
            device,
            rect,
            filter->defaultConfiguration(KisGlobalResourcesInterface::instance())->cloneWithResourcesSnapshot());
        QVERIFY(before != pixels(device, rect));
    }

    void testPatches_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<QString>("depth");
        QTest::addColumn<int>("mode");
        for (const QString &model : {QString("RGBA"), QString("GRAYA")}) {
            for (const QString &depth : {QString("U8"), QString("U16"), QString("F16"), QString("F32")}) {
                for (int mode : {0, 1}) {
                    QTest::newRow(qPrintable(model + depth + QString::number(mode))) << model << depth << mode;
                }
            }
        }
        QTest::newRow("CMYKA-U16-radial") << QString("CMYKA") << QString("U16") << 1;
        QTest::newRow("LABA-U16-translation") << QString("LABA") << QString("U16") << 0;
    }

    void testPatches()
    {
        QFETCH(QString, model);
        QFETCH(QString, depth);
        QFETCH(int, mode);
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace(model, depth, QString());
        QVERIFY(cs);
        const QRect rect(-69, -37, 137, 83);
        KisChromaticAberrationFilter filter;
        auto config = settings(filter, mode);
        auto whole = pattern(cs, rect);
        auto tiled = pattern(cs, rect);
        const auto before = pixels(whole, rect);
        filter.process(whole, rect, config); // Also safe without an outer transaction.
        {
            KisTransaction transaction(tiled);
            // Parallel strips with non-tile-aligned boundaries, subdivided into
            // sequential patches. The source transaction must span all jobs.
            std::vector<std::future<void>> jobs;
            for (int stripe = 0; stripe < 4; ++stripe) {
                jobs.push_back(std::async(std::launch::async, [&, stripe]() {
                    for (int y = rect.top() + stripe * 17; y <= rect.bottom(); y += 68) {
                        for (int x = rect.left(); x <= rect.right(); x += 23) {
                            filter.process(tiled, rect.intersected(QRect(x, y, 23, 17)), config);
                        }
                    }
                }));
            }
            for (auto &job : jobs)
                job.get();
        }
        const auto after = pixels(whole, rect);
        QVERIFY(after != before);
        QCOMPARE(pixels(tiled, rect), after);
        for (int offset = 0; offset < after.size(); offset += cs->pixelSize()) {
            const auto *pixel = reinterpret_cast<const quint8 *>(before.constData() + offset);
            if (cs->opacityF(pixel) == 0.0) {
                QCOMPARE(after.mid(offset, cs->pixelSize()), before.mid(offset, cs->pixelSize()));
            }
            for (const auto *channel : cs->channels()) {
                if (channel->channelType() == KoChannelInfo::ALPHA
                    || (model == "RGBA" && channel->displayPosition() == 1)) {
                    QCOMPARE(after.mid(offset + channel->pos(), channel->size()),
                             before.mid(offset + channel->pos(), channel->size()));
                }
            }
        }
        auto zero = pattern(cs, rect);
        config->setProperty("strength", 0.0);
        filter.process(zero, rect, config);
        QCOMPARE(pixels(zero, rect), before);
        QCOMPARE(filter.neededRect(rect, config, 0), rect);
    }

    void testAnalyticSampling()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        QVERIFY(cs);
        const QRect rect(-32, -32, 65, 65);
        KisPaintDeviceSP source = new KisPaintDevice(cs);
        QVector<float> values(4);
        KoColor color(cs);
        // RGB float channel order is RGB, unlike integer BGRA. A linear field
        // makes fractional sampling analytically predictable, including HDR.
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            for (int x = rect.left(); x <= rect.right(); ++x) {
                values = {float(x) / 8.0f, 2.0f, float(y) / 8.0f, 0.5f};
                cs->fromNormalisedChannelsValue(color.data(), values);
                source->fill(QRect(x, y, 1, 1), color);
            }
        }
        KisChromaticAberrationFilter filter;
        auto config = settings(filter);
        config->setProperty("strength", 1.5);
        config->setProperty("angle", 0.0);
        auto output = new KisPaintDevice(*source);
        KisPaintDeviceSP result = output;
        filter.process(result, QRect(-16, -16, 33, 33), config);
        auto sample = valueAt(result, -8, 12);
        QVERIFY(std::abs(sample[0] - (-9.5f / 8.0f)) < 1e-6f);
        QCOMPARE(sample[1], 2.0f);
        QCOMPARE(sample[2], 1.5f);
        QCOMPARE(sample[3], 0.5f);

        result = new KisPaintDevice(*source);
        config->setProperty("angle", 90.0);
        filter.process(result, QRect(-16, -16, 33, 33), config);
        sample = valueAt(result, -8, 12);
        QVERIFY(std::abs(sample[2] - (13.5f / 8.0f)) < 1e-6f);

        result = new KisPaintDevice(*source);
        config->setProperty("mode", 1);
        config->setProperty("centerX", 0.0);
        config->setProperty("centerY", 0.0);
        config->setProperty("radius", 10.0);
        config->setProperty("strength", 2.5);
        filter.process(result, QRect(-16, -16, 33, 33), config);
        QCOMPARE(pixels(result, QRect(0, 0, 1, 1)), pixels(source, QRect(0, 0, 1, 1)));
        sample = valueAt(result, 3, 4);
        QVERIFY(std::abs(sample[0] - (2.25f / 8.0f)) < 1e-6f);
        QVERIFY(std::abs(sample[2] - (5.0f / 8.0f)) < 1e-6f);
        sample = valueAt(result, 12, 16); // displacement capped beyond radius
        QVERIFY(std::abs(sample[0] - (10.5f / 8.0f)) < 1e-6f);
        QVERIFY(std::abs(sample[2] - (18.0f / 8.0f)) < 1e-6f);
    }

    void testTransparentSamplingAndChannels()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        KisPaintDeviceSP device = new KisPaintDevice(cs);
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{100.0f, 100.0f, 100.0f, 0.0f});
        device->fill(QRect(-10, -10, 21, 21), color);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{0.8f, 0.5f, 0.3f, 0.5f});
        device->fill(QRect(0, 0, 1, 1), color);
        const auto before = pixels(device, QRect(-10, -10, 21, 21));
        KisChromaticAberrationFilter filter;
        auto config = settings(filter);
        config->setProperty("strength", 0.5);
        config->setProperty("angle", 0.0);
        filter.process(device, QRect(-10, -10, 21, 21), config);
        QCOMPARE(pixels(device, QRect(-10, -10, 21, 21)), before);
        config->setProperty("strength", 5.0); // fully transparent sample footprint
        filter.process(device, QRect(-10, -10, 21, 21), config);
        QCOMPARE(pixels(device, QRect(-10, -10, 21, 21)), before);

        const QRect rect(0, 0, 32, 32);
        device = pattern(cs, rect);
        const auto original = pixels(device, rect);
        QBitArray flags(4, false);
        config->setChannelFlags(flags);
        filter.process(device, rect, config);
        QCOMPARE(pixels(device, rect), original);
        flags.setBit(0, true);
        config->setChannelFlags(flags);
        filter.process(device, rect, config);
        const auto changed = pixels(device, rect);
        QVERIFY(changed != original);
        for (int offset = 0; offset < changed.size(); offset += cs->pixelSize()) {
            QCOMPARE(changed.mid(offset + 4, 12), original.mid(offset + 4, 12));
        }
    }

    void testPartialSelectionAndUndo()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(-32, -32, 64, 64);
        const QRect part(-8, -8, 13, 15);
        KisChromaticAberrationFilter filter;
        for (int mode : {0, 1}) {
            auto config = settings(filter, mode);
            auto source = pattern(cs, rect);
            auto full = new KisPaintDevice(*source);
            KisPaintDeviceSP complete = full;
            filter.process(complete, rect, config);
            KisPaintDeviceSP partial = new KisPaintDevice(*source);
            filter.process(source, partial, nullptr, part, config);
            QCOMPARE(pixels(partial, part), pixels(complete, part));
            const auto before = pixels(source, rect);
            auto selection = KisSelectionSP(new KisSelection());
            selection->pixelSelection()->select(part, 128);
            KisPaintDeviceSP selected = new KisPaintDevice(*source);
            KisTransaction transaction(selected);
            filter.process(source, selected, selection, rect, config);
            const auto after = pixels(selected, rect);
            QVERIFY(after != before);
            QCOMPARE(pixels(selected, QRect(-32, -32, 64, 20)), pixels(source, QRect(-32, -32, 64, 20)));
            std::unique_ptr<KUndo2Command> command(transaction.endAndTake());
            // The host pushes an already executed transaction through redo()
            // once before the first user Undo (KisTransactionData::firstRedo).
            command->redo();
            command->undo();
            QCOMPARE(pixels(selected, rect), before);
            command->redo();
            QCOMPARE(pixels(selected, rect), after);
            QCOMPARE(pixels(source, rect), before);
            QVERIFY(filter.neededRect(part, config, 0).contains(part.adjusted(-4, -4, 4, 4)));
        }
    }

    void testConfiguration()
    {
        KisChromaticAberrationFilter filter;
        auto config = settings(filter, 1);
        KisChromaticAberrationConfigWidget widget(nullptr, nullptr);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        auto saved = widget.configuration();
        QCOMPARE(saved->getDouble("centerX"), -7.25);
        QCOMPARE(saved->getDouble("centerY"), 8.5);
        QCOMPARE(saved->getDouble("radius"), 40.0);
        auto restored = filter.factoryConfiguration(KisGlobalResourcesInterface::instance());
        restored->fromXML(saved->toXML());
        QCOMPARE(restored->name(), QString("solstice_chromatic_aberration"));
        const QRect rect(-16, -16, 32, 32);
        auto first = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
        auto second = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
        filter.process(first, rect, config);
        filter.process(second, rect, restored->cloneWithResourcesSnapshot());
        QCOMPARE(pixels(first, rect), pixels(second, rect));
    }

    void benchmarkCpu()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_CHROMATIC_ABERRATION")) {
            QSKIP("Set SOLSTICE_BENCHMARK_CHROMATIC_ABERRATION=1 for CPU timings.");
        }
        KisChromaticAberrationFilter filter;
        for (const QString &depth : {QString("U8"), QString("F32")}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
            for (const QSize size : {QSize(2480, 3508), QSize(4096, 4096)}) {
                for (int mode : {0, 1}) {
                    QList<qint64> times;
                    for (int run = 0; run < 3; ++run) {
                        KisPaintDeviceSP device = new KisPaintDevice(cs);
                        device->fill(QRect(QPoint(), size), KoColor(Qt::gray, cs));
                        auto config = settings(filter, mode);
                        config->setProperty("centerX", size.width() / 2.0);
                        config->setProperty("centerY", size.height() / 2.0);
                        config->setProperty("radius", std::hypot(size.width(), size.height()) / 2.0);
                        QElapsedTimer timer;
                        timer.start();
                        filter.process(device, QRect(QPoint(), size), config);
                        times.append(timer.elapsed());
                    }
                    std::sort(times.begin(), times.end());
                    qInfo() << "Chromatic Aberration CPU single call" << depth << size << "mode" << mode << "median ms"
                            << times[1];
                }
            }
        }
    }
};

SIMPLE_TEST_MAIN(KisChromaticAberrationFilterTest)
#include "KisChromaticAberrationFilterTest.moc"
