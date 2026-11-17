/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisSharpnessFilter.h"
#include "KisCpuPlaneBlur.h"
#include "KisSharpnessConfigWidget.h"
#include <KisGlobalResourcesInterface.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QSignalSpy>
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
    return KisSharpnessFilter()
        .defaultConfiguration(KisGlobalResourcesInterface::instance())
        ->cloneWithResourcesSnapshot();
}
} // namespace
class KisSharpnessFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testGaussianReference()
    {
        using namespace SolsticeEffects;
        const QRect rect(-11, 7, 27, 23);
        for (double sigma : {0.0, .5, 1.0, 2.3}) {
            const int r = gaussianHalo(sigma);
            FloatPlane source(rect.adjusted(-r, -r, r, r)), output(rect);
            for (int y = source.rect.top(); y <= source.rect.bottom(); ++y)
                for (int x = source.rect.left(); x <= source.rect.right(); ++x)
                    source.at(x, y) = float(std::sin(x * .3) + std::cos(y * .4));
            QVERIFY(gaussianBlur(source, output, sigma));
            for (int y = rect.top(); y <= rect.bottom(); ++y)
                for (int x = rect.left(); x <= rect.right(); ++x) {
                    double sum = 0, total = 0;
                    for (int dy = -r; dy <= r; ++dy)
                        for (int dx = -r; dx <= r; ++dx) {
                            const double w = r ? std::exp(-.5 * (dx * dx + dy * dy) / (sigma * sigma)) : 1;
                            total += w;
                            sum += w * source.at(x + dx, y + dy);
                        }
                    QVERIFY(std::abs(output.at(x, y) - sum / total) < 3e-7);
                }
            FloatPlane patch(QRect(-5, 10, 9, 11));
            QVERIFY(gaussianBlur(source, patch, sigma));
            for (int y = patch.rect.top(); y <= patch.rect.bottom(); ++y)
                for (int x = patch.rect.left(); x <= patch.rect.right(); ++x)
                    QCOMPARE(patch.at(x, y), output.at(x, y));
            KoDummyUpdaterHolder progress;
            QMetaObject::invokeMethod(progress.updater(), "setInterrupted", Qt::DirectConnection, Q_ARG(bool, true));
            const auto before = output.values;
            QVERIFY(!gaussianBlur(source, output, sigma, progress.updater()));
            QVERIFY(output.values == before);
        }
    }
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
        KisSharpnessFilter filter;
        auto config = settings();
        auto whole = pattern(cs, rect), tiled = pattern(cs, rect);
        const auto before = pixels(whole, rect);
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
        auto zero = pattern(cs, rect);
        config->setProperty("strength", 0.0);
        filter.process(zero, rect, config);
        QCOMPARE(pixels(zero, rect), before);
        QCOMPARE(filter.neededRect(rect, config, 0), rect);
    }
    void testFlatHdrAndEdges()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        const QRect rect(-20, -20, 40, 40);
        KisSharpnessFilter filter;
        auto config = settings();
        config->setProperty("strength", 100.0);
        KisPaintDeviceSP dev = new KisPaintDevice(cs);
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{100, -50, 30, 0});
        dev->fill(rect, color);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{-2, .5f, 3, .375f});
        dev->fill(QRect(-8, -8, 16, 16), color);
        const auto before = pixels(dev, rect);
        filter.process(dev, rect, config);
        QCOMPARE(pixels(dev, rect), before); // no hidden-RGB or silhouette halos
        for (int y = -20; y < 20; ++y)
            for (int x = -20; x < 20; ++x) {
                const float value = float(.2 + .6 / (1 + std::exp(-x * 1.1)));
                cs->fromNormalisedChannelsValue(color.data(), QVector<float>{value, value, value, 1});
                dev->setPixel(x, y, color);
            }
        const auto soft = pixels(dev, QRect(-10, -10, 20, 20));
        filter.process(dev, rect, config);
        QVERIFY(pixels(dev, QRect(-10, -10, 20, 20)) != soft);
        QVector<float> values(4);
        for (int x = -10; x < 10; ++x) {
            KoColor p(cs);
            dev->pixel(x, 0, &p);
            cs->normalisedChannelsValue(p.data(), values);
            QVERIFY(values[0] >= .2f && values[0] <= .8f);
        }
        dev->fill(rect, KoColor(Qt::white, cs));
        dev->fill(QRect(-20, -20, 20, 40), KoColor(Qt::black, cs));
        const auto hard = pixels(dev, rect);
        filter.process(dev, rect, config);
        QCOMPARE(pixels(dev, rect), hard);
    }
    void testSelectionUndoChannelsAndUpdates()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(-32, -32, 64, 64), part(-8, -8, 13, 15);
        KisSharpnessFilter filter;
        auto config = settings();
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
        config = settings();
        src->fill(part, KoColor(Qt::red, cs));
        KisPaintDeviceSP updated = new KisPaintDevice(*src);
        filter.process(updated, rect, config);
        filter.process(src, full, nullptr, filter.changedRect(part, config, 0), config);
        QCOMPARE(pixels(full, rect), pixels(updated, rect));
        QCOMPARE(filter.changedRect(part, config, 0), part.adjusted(-3, -3, 3, 3));
    }
    void testConfigurationAndCancellation()
    {
        KisSharpnessFilter filter;
        auto config = settings();
        config->setProperty("strength", 72.5);
        KisSharpnessConfigWidget widget(nullptr, nullptr);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        auto saved = widget.configuration();
        QCOMPARE(saved->getDouble("strength"), 72.5);
        auto restored = filter.factoryConfiguration(KisGlobalResourcesInterface::instance());
        restored->fromXML(saved->toXML());
        const QRect rect(-20, -20, 64, 64);
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        auto first = pattern(cs, rect), second = pattern(cs, rect);
        filter.process(first, rect, config);
        filter.process(second, rect, restored->cloneWithResourcesSnapshot());
        QCOMPARE(pixels(first, rect), pixels(second, rect));
        widget.findChild<KisDoubleSliderSpinBox *>("sharpnessStrength")->setValue(50);
        QCOMPARE(spy.count(), 1);
        for (int threshold : {10, 45, 70}) {
            auto dev = pattern(cs, rect);
            const auto before = pixels(dev, rect);
            KoDummyUpdaterHolder progress;
            connect(progress.updater(), &KoUpdater::sigProgress, this, [&](int percent) {
                if (percent >= threshold)
                    QMetaObject::invokeMethod(progress.updater(),
                                              "setInterrupted",
                                              Qt::DirectConnection,
                                              Q_ARG(bool, true));
            });
            filter.process(dev, rect, config, progress.updater());
            QVERIFY(progress.updater()->interrupted());
            QCOMPARE(pixels(dev, rect), before);
        }
    }
    void testInstalledPlugin()
    {
        auto filter = KisFilterRegistry::instance()->value("solstice_sharpness");
        QVERIFY2(filter, "Install kritasolsticeeffectsfilter first.");
        QVERIFY(filter->supportsThreading());
        QVERIFY(filter->supportsAdjustmentLayers());
        QVERIFY(!filter->supportsLevelOfDetail(settings(), 1));
        const QRect rect(0, 0, 32, 32);
        auto dev = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
        const auto before = pixels(dev, rect);
        filter->process(dev, rect, settings());
        QVERIFY(pixels(dev, rect) != before);
    }
    void testFilterMaskXmlRoundTrip()
    {
        const QRect rect(0, 0, 64, 64);
        TestUtil::MaskParent parent(rect);
        auto config = settings();
        config->setProperty("strength", 73.0);
        KisFilterMaskSP mask = new KisFilterMask(parent.image, "Sharpness");
        parent.image->addNode(mask, parent.layer);
        auto restored = KisSharpnessFilter().factoryConfiguration(KisGlobalResourcesInterface::instance());
        restored->fromXML(config->toXML());
        mask->setFilter(restored->cloneWithResourcesSnapshot());
        mask->initSelection(parent.layer);
        mask->select(rect, MAX_SELECTED);
        auto original = pattern(parent.layer->colorSpace(), rect);
        KisPaintDeviceSP expected = new KisPaintDevice(*original), actual = new KisPaintDevice(*original);
        KisSharpnessFilter().process(expected, rect, config);
        mask->apply(actual, rect, rect, KisNode::N_FILTHY, KisRenderPassFlag::None);
        const auto result = pixels(actual, rect), reference = pixels(expected, rect);
        const auto *cs = actual->colorSpace();
        for (int p = 0; p < result.size(); p += cs->pixelSize()) {
            // The mask compositor can clear hidden RGB in fully transparent
            // pixels; compare visible colors and every pixel's coverage.
            const auto *ref = reinterpret_cast<const quint8 *>(reference.constData() + p);
            const auto *out = reinterpret_cast<const quint8 *>(result.constData() + p);
            QCOMPARE(cs->opacityF(out), cs->opacityF(ref));
            if (cs->opacityF(ref) > 0)
                QCOMPARE(result.mid(p, cs->pixelSize()), reference.mid(p, cs->pixelSize()));
        }
        QCOMPARE(mask->filter()->getDouble("strength"), 73.0);
    }
    void benchmarkCpu()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_SHARPNESS"))
            QSKIP("Opt-in timings");
        for (const QString &depth : {QString("U8"), QString("F32")}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
            for (const QSize size : {QSize(1024, 1024), QSize(2480, 3508), QSize(4096, 4096)}) {
                QList<qint64> times;
                for (int run = 0; run < 3; ++run) {
                    const QRect rect(QPoint(), size);
                    auto dev = pattern(cs, rect);
                    QElapsedTimer timer;
                    timer.start();
                    KisSharpnessFilter().process(dev, rect, settings());
                    times << timer.elapsed();
                }
                std::sort(times.begin(), times.end());
                qInfo() << "Sharpness CPU single call" << depth << size << "median ms" << times[1];
            }
        }
    }
    void renderReviewArtifacts()
    {
        const QString directory = qEnvironmentVariable("SOLSTICE_SHARPNESS_ARTIFACT_DIR");
        if (directory.isEmpty())
            QSKIP("Opt-in review images");
        QImage source(384, 256, QImage::Format_ARGB32);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x) {
                const double wave = .5 + .35 * std::tanh(1.5 * std::sin(x * .19 + y * .05));
                source.setPixelColor(x,
                                     y,
                                     QColor(int(255 * wave), int(220 * wave), int(180 * wave), y < 128 ? 255 : 128));
            }
        QImage sheet(1152, 288, QImage::Format_ARGB32);
        sheet.fill(Qt::white);
        QPainter painter(&sheet);
        KisSharpnessFilter filter;
        for (int i = 0; i < 3; ++i) {
            KisPaintDeviceSP dev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
            dev->convertFromQImage(source, nullptr);
            auto config = settings();
            config->setProperty("strength", i * 50.0);
            filter.process(dev, source.rect(), config);
            painter.drawText(QRect(i * 384, 0, 384, 32), Qt::AlignCenter, QString("Strength %1").arg(i * 50));
            painter.drawImage(QPoint(i * 384, 32), dev->convertToQImage(nullptr, source.rect()));
            if (i == 1) {
                KisSharpnessConfigWidget widget(nullptr, dev);
                widget.setConfiguration(config);
                widget.resize(widget.sizeHint());
                widget.show();
                QApplication::processEvents();
                QVERIFY(widget.grab().save(QDir(directory).filePath("sharpness-widget.png")));
            }
        }
        painter.end();
        QVERIFY(sheet.save(QDir(directory).filePath("sharpness-comparison.png")));
    }
};
SIMPLE_TEST_MAIN(KisSharpnessFilterTest)
#include "KisSharpnessFilterTest.moc"
