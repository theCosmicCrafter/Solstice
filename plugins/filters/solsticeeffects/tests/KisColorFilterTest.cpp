/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisColorFilter.h"
#include "KisColorFilterConfigWidget.h"
#include "KisSharpnessFilter.h"
#include <KisGlobalResourcesInterface.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <future>
#include <kis_filter_mask.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_slider_spin_box.h>
#include <kis_transaction.h>
#include <memory>
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
KisPaintDeviceSP pattern(const KoColorSpace *cs, const QRect &rect)
{
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    dev->moveTo(13, -11);
    QByteArray data(rect.width() * rect.height() * cs->pixelSize(), '\0');
    QVector<float> values(cs->channelCount());
    for (int y = 0; y < rect.height(); ++y)
        for (int x = 0; x < rect.width(); ++x) {
            for (int c = 0; c < values.size(); ++c)
                values[c] = cs->channels()[c]->channelType() == KoChannelInfo::ALPHA
                    ? float((x + y) % 4) / 3.0f
                    : float((x * 7 + y * 13 + c * 17) % 101) / 100.0f;
            cs->fromNormalisedChannelsValue(reinterpret_cast<quint8 *>(data.data())
                                                + (y * rect.width() + x) * cs->pixelSize(),
                                            values);
        }
    dev->writeBytes(reinterpret_cast<const quint8 *>(data.constData()), rect);
    return dev;
}
KisFilterConfigurationSP settings()
{
    return KisColorFilter().defaultConfiguration(KisGlobalResourcesInterface::instance())->cloneWithResourcesSnapshot();
}
KisFilterConfigurationSP graded()
{
    auto config = settings();
    KisColorFilter::applyPreset(config, 7);
    config->setProperty("adjustContrast", 12.0);
    config->setProperty("adjustSharpness", 53.0);
    return config;
}
} // namespace
class KisColorFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testFormats_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<QString>("depth");
        for (const QString &model : {QString("RGBA"), QString("GRAYA")})
            for (const QString &depth : {QString("U8"), QString("U16"), QString("F16"), QString("F32")})
                QTest::newRow(qPrintable(model + depth)) << model << depth;
        QTest::newRow("CMYKAU16") << QString("CMYKA") << QString("U16");
        QTest::newRow("LABAU16") << QString("LABA") << QString("U16");
    }
    void testFormats()
    {
        QFETCH(QString, model);
        QFETCH(QString, depth);
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace(model, depth, QString());
        QVERIFY(cs);
        const QRect rect(-69, -37, 137, 83);
        KisColorFilter filter;
        auto whole = pattern(cs, rect), tiled = pattern(cs, rect);
        const auto before = pixels(whole, rect);
        filter.process(whole, rect, settings());
        QCOMPARE(pixels(whole, rect), before);
        auto config = graded();
        filter.process(whole, rect, config);
        {
            KisTransaction transaction(tiled);
            std::vector<std::future<void>> jobs;
            for (int stripe = 0; stripe < 4; ++stripe)
                jobs.push_back(std::async(std::launch::async, [&, stripe] {
                    for (int y = rect.top() + stripe * 17; y <= rect.bottom(); y += 68)
                        for (int x = rect.left(); x <= rect.right(); x += 23)
                            filter.process(tiled, rect.intersected(QRect(x, y, 23, 17)), config);
                }));
            for (auto &job : jobs)
                job.get();
        }
        const auto after = pixels(whole, rect);
        QVERIFY(after != before);
        QCOMPARE(pixels(tiled, rect), after);
        for (int p = 0; p < before.size(); p += cs->pixelSize()) {
            if (cs->opacityF(reinterpret_cast<const quint8 *>(before.constData() + p)) == 0)
                QCOMPARE(after.mid(p, cs->pixelSize()), before.mid(p, cs->pixelSize()));
            for (const auto *c : cs->channels())
                if (c->channelType() == KoChannelInfo::ALPHA)
                    QCOMPARE(after.mid(p + c->pos(), c->size()), before.mid(p + c->pos(), c->size()));
        }
    }
    void testAmountAndAdjust()
    {
        const QRect rect(0, 0, 30, 20);
        KisColorFilter filter;
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const auto original = pattern(cs, rect);
        const auto before = pixels(original, rect);
        QList<QByteArray> looks;
        for (int preset = 0; preset < 10; ++preset) {
            auto config = settings();
            KisColorFilter::applyPreset(config, preset);
            KisPaintDeviceSP dev = new KisPaintDevice(*original);
            filter.process(dev, rect, config);
            const auto result = pixels(dev, rect);
            if (preset == 0)
                QCOMPARE(result, before);
            else {
                QVERIFY(result != before);
                QVERIFY(!looks.contains(result));
            }
            looks << result;
            config->setProperty("amount", 0.0);
            dev = new KisPaintDevice(*original);
            filter.process(dev, rect, config);
            QCOMPARE(pixels(dev, rect), before);
            config->setProperty("adjustBrightness", 20.0);
            filter.process(dev, rect, config);
            auto neutral = settings();
            neutral->setProperty("adjustBrightness", 20.0);
            KisPaintDeviceSP expected = new KisPaintDevice(*original);
            filter.process(expected, rect, neutral);
            QCOMPARE(pixels(dev, rect), pixels(expected, rect));
            QVERIFY(pixels(dev, rect) != before);
        }
        auto config = graded();
        QVERIFY(!filter.supportsLevelOfDetail(config, 1));
        QCOMPARE(filter.neededRect(rect, config, 0), rect.adjusted(-3, -3, 3, 3));
        config->setProperty("adjustSharpness", 0.0);
        QVERIFY(filter.supportsLevelOfDetail(config, 1));
        QCOMPARE(filter.neededRect(rect, config, 1), rect);
        // Gray ignores all Look parameters, including contrast inside a Look.
        auto gray = pattern(KoColorSpaceRegistry::instance()->colorSpace("GRAYA", "U16", QString()), rect);
        const auto grayBefore = pixels(gray, rect);
        config = settings();
        KisColorFilter::applyPreset(config, 9);
        config->setProperty("adjustHue", 80.0);
        config->setProperty("adjustTemperature", 90.0);
        filter.process(gray, rect, config);
        QCOMPARE(pixels(gray, rect), grayBefore);
    }
    void testHdrAndStageOrder()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        const QRect rect(-8, -9, 32, 33);
        KisPaintDeviceSP dev = new KisPaintDevice(cs);
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{-2, .5f, 3, .375f});
        dev->fill(rect, color);
        auto config = settings();
        config->setProperty("adjustBrightness", 20.0);
        KisColorFilter().process(dev, rect, config);
        KoColor out(cs);
        dev->pixel(0, 0, &out);
        QVector<float> values(4);
        cs->normalisedChannelsValue(out.data(), values);
        QVERIFY(std::abs(values[0] + 1.9f) < 1e-6);
        QVERIFY(std::abs(values[2] - 3.1f) < 1e-6);
        QCOMPARE(values[3], .375f);
        // Color then Sharpness is equivalent at F32, including transformed halo.
        config = graded();
        auto combined = pattern(cs, rect.adjusted(-3, -3, 3, 3)), sequential = pattern(cs, rect.adjusted(-3, -3, 3, 3));
        KisColorFilter().process(combined, rect, config);
        const double strength = config->getDouble("adjustSharpness");
        config->setProperty("adjustSharpness", 0.0);
        KisColorFilter().process(sequential, rect.adjusted(-3, -3, 3, 3), config);
        auto sharp = KisSharpnessFilter()
                         .defaultConfiguration(KisGlobalResourcesInterface::instance())
                         ->cloneWithResourcesSnapshot();
        sharp->setProperty("strength", strength);
        KisSharpnessFilter().process(sequential, rect, sharp);
        QCOMPARE(pixels(combined, rect), pixels(sequential, rect));
    }
    void testExpandedValuesAndAmountReference()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        const QRect rect(0, 0, 1, 1);
        auto config = settings();
        config->setProperty("preset", 0); // Metadata must never control rendering.
        config->setProperty("lookEnabled", true);
        config->setProperty("lookBrightness", 40.0);
        config->setProperty("lookContrast", 50.0);
        config->setProperty("adjustBrightness", 10.0);
        config->setProperty("adjustContrast", -30.0);
        for (double amount : {0.0, 25.0, 50.0, 100.0}) {
            config->setProperty("amount", amount);
            KisPaintDeviceSP dev = new KisPaintDevice(cs);
            KoColor input(cs);
            const QVector<float> original = {.1f, .3f, .7f, .375f};
            cs->fromNormalisedChannelsValue(input.data(), original);
            dev->fill(rect, input);
            KisColorFilter().process(dev, rect, config);
            KoColor result(cs);
            dev->pixel(0, 0, &result);
            QVector<float> values(4);
            cs->normalisedChannelsValue(result.data(), values);
            for (int i = 0; i < 3; ++i) {
                const double looked = .5 + (original[i] + .2 - .5) * std::exp2(.5);
                const double blended = original[i] + amount / 100 * (looked - original[i]);
                const double expected = .5 + (blended + .05 - .5) * std::exp2(-.3);
                QVERIFY(std::abs(values[i] - expected) < 1e-6);
            }
            QCOMPARE(values[3], original[3]);
        }
    }
    void testSelectionUndoChannelsAndUpdates()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(-32, -32, 64, 64), part(-8, -8, 13, 15);
        KisColorFilter filter;
        auto config = graded();
        auto src = pattern(cs, rect);
        const auto before = pixels(src, rect);
        KisPaintDeviceSP full = new KisPaintDevice(*src), selected = new KisPaintDevice(*src);
        filter.process(full, rect, config);
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(part, 128);
        KisTransaction transaction(selected);
        filter.process(src, selected, selection, rect, config);
        const auto after = pixels(selected, rect);
        QVERIFY(after != before);
        QCOMPARE(pixels(selected, QRect(-32, -32, 64, 20)), pixels(src, QRect(-32, -32, 64, 20)));
        std::unique_ptr<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        command->undo();
        QCOMPARE(pixels(selected, rect), before);
        command->redo();
        QCOMPARE(pixels(selected, rect), after);
        QBitArray flags(cs->channelCount(), false);
        config->setChannelFlags(flags);
        filter.process(src, rect, config);
        QCOMPARE(pixels(src, rect), before);
        flags.setBit(0);
        config->setChannelFlags(flags);
        filter.process(src, rect, config);
        const auto masked = pixels(src, rect), all = pixels(full, rect);
        for (int p = 0; p < masked.size(); p += 4) {
            QCOMPARE(masked[p], all[p]);
            QCOMPARE(masked.mid(p + 1, 3), before.mid(p + 1, 3));
        }
        src = pattern(cs, rect);
        config = graded();
        src->fill(part, KoColor(Qt::red, cs));
        KisPaintDeviceSP updated = new KisPaintDevice(*src);
        filter.process(updated, rect, config);
        filter.process(src, full, nullptr, filter.changedRect(part, config, 0), config);
        QCOMPARE(pixels(full, rect), pixels(updated, rect));
    }
    void testConfigurationAndThumbnailLifetime()
    {
        auto config = graded();
        config->setProperty("lookTemperature", 13.25);
        config->setProperty("amount", 63.0);
        KisColorFilterConfigWidget widget(nullptr, nullptr, false);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        QCOMPARE(widget.configuration()->getDouble("lookTemperature"), 13.25);
        auto restored = settings();
        restored->fromXML(widget.configuration()->toXML());
        QCOMPARE(restored->getProperties(), config->getProperties());
        widget.findChild<KisDoubleSliderSpinBox *>("adjustTint")->setValue(10);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(widget.configuration()->getInt("preset"), -1);
        QCOMPARE(widget.configuration()->getDouble("lookTemperature"), 13.25);
        widget.findChild<QToolButton *>("colorLook2")->click();
        QCOMPARE(widget.configuration()->getDouble("adjustTint"), 10.0);
        QCOMPARE(widget.configuration()->getDouble("amount"), 63.0);
        QCOMPARE(widget.configuration()->getInt("preset"), 2);
        QTRY_VERIFY(!widget.findChild<QToolButton *>("colorLook9")->icon().isNull());
        QCOMPARE(spy.count(), 2); // thumbnails must not change the configuration
        widget.findChild<QPushButton *>("colorFilterReset")->click();
        QCOMPARE(widget.configuration()->getDouble("adjustTint"), 0.0);
        QCOMPARE(widget.configuration()->getDouble("amount"), 100.0);
        QCOMPARE(widget.configuration()->getInt("preset"), 0);
        auto *transient = new KisColorFilterConfigWidget(nullptr, nullptr, true);
        delete transient;
        QApplication::processEvents(); // queued callbacks cannot outlive the widget
    }
    void testCancellation()
    {
        const QRect rect(-20, -20, 64, 64);
        for (int threshold : {10, 40, 70, 90}) {
            auto dev = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
            const auto before = pixels(dev, rect);
            KoDummyUpdaterHolder progress;
            int previous = 0;
            connect(progress.updater(), &KoUpdater::sigProgress, this, [&](int percent) {
                QVERIFY(percent >= previous);
                previous = percent;
                if (percent >= threshold)
                    QMetaObject::invokeMethod(progress.updater(),
                                              "setInterrupted",
                                              Qt::DirectConnection,
                                              Q_ARG(bool, true));
            });
            KisColorFilter().process(dev, rect, graded(), progress.updater());
            QVERIFY(progress.updater()->interrupted());
            QCOMPARE(pixels(dev, rect), before);
        }
    }
    void testFilterMaskXmlRoundTrip()
    {
        QVERIFY(KisFilterRegistry::instance()->value("solstice_color_filter"));
        const QRect rect(0, 0, 64, 64);
        TestUtil::MaskParent parent(rect);
        auto config = graded();
        config->setProperty("lookTemperature", 11.25);
        KisFilterMaskSP mask = new KisFilterMask(parent.image, "Color Filter");
        parent.image->addNode(mask, parent.layer);
        auto restored = settings();
        restored->fromXML(config->toXML());
        mask->setFilter(restored->cloneWithResourcesSnapshot());
        mask->initSelection(parent.layer);
        mask->select(rect, MAX_SELECTED);
        auto original = pattern(parent.layer->colorSpace(), rect);
        KisPaintDeviceSP expected = new KisPaintDevice(*original), actual = new KisPaintDevice(*original);
        KisColorFilter().process(expected, rect, config);
        mask->apply(actual, rect, rect, KisNode::N_FILTHY, KisRenderPassFlag::None);
        const auto result = pixels(actual, rect), reference = pixels(expected, rect);
        const auto *cs = actual->colorSpace();
        for (int p = 0; p < result.size(); p += cs->pixelSize()) {
            const auto *ref = reinterpret_cast<const quint8 *>(reference.constData() + p),
                       *out = reinterpret_cast<const quint8 *>(result.constData() + p);
            QCOMPARE(cs->opacityF(out), cs->opacityF(ref));
            if (cs->opacityF(ref) > 0)
                QCOMPARE(result.mid(p, cs->pixelSize()), reference.mid(p, cs->pixelSize()));
        }
        QCOMPARE(mask->filter()->getDouble("lookTemperature"), 11.25);
    }
    void benchmarkCpu()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_COLOR_FILTER"))
            QSKIP("Opt-in timings");
        for (const QString &depth : {QString("U8"), QString("F32")})
            for (const QSize size : {QSize(2480, 3508), QSize(4096, 4096)})
                for (double sharpness : {0.0, 50.0}) {
                    QList<qint64> times;
                    auto config = graded();
                    config->setProperty("adjustSharpness", sharpness);
                    for (int run = 0; run < 3; ++run) {
                        const QRect rect(QPoint(), size);
                        auto dev =
                            pattern(KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString()), rect);
                        QElapsedTimer timer;
                        timer.start();
                        KisColorFilter().process(dev, rect, config);
                        times << timer.elapsed();
                    }
                    std::sort(times.begin(), times.end());
                    qInfo() << "Color Filter CPU single call" << depth << size << "sharpness" << sharpness
                            << "median ms" << times[1];
                }
    }
    void renderReviewArtifacts()
    {
        const QString directory = qEnvironmentVariable("SOLSTICE_COLOR_FILTER_ARTIFACT_DIR");
        if (directory.isEmpty())
            QSKIP("Opt-in review images");
        KisColorFilterConfigWidget widget(nullptr, nullptr, false);
        widget.resize(widget.sizeHint());
        widget.show();
        QTRY_VERIFY(!widget.findChild<QToolButton *>("colorLook9")->icon().isNull());
        QVERIFY(widget.grab().save(QDir(directory).filePath("color-filter-look.png")));
        widget.findChild<QTabWidget *>("colorFilterTabs")->setCurrentIndex(1);
        QApplication::processEvents();
        QVERIFY(widget.grab().save(QDir(directory).filePath("color-filter-adjust.png")));
    }
};
SIMPLE_TEST_MAIN(KisColorFilterTest)
#include "KisColorFilterTest.moc"
