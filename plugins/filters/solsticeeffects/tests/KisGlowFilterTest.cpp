/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisGlowFilter.h"
#include "KisCpuPlaneBlur.h"
#include "KisGlowConfigWidget.h"
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
#include <limits>
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
    return KisGlowFilter().defaultConfiguration(KisGlobalResourcesInterface::instance())->cloneWithResourcesSnapshot();
}
} // namespace
class KisGlowFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSoftBlurReference()
    {
        using namespace SolsticeEffects;
        const QRect rect(-19, -21, 11, 9);
        for (double sigma : {0.0, 1.0, 3.0, 8.0, 64.0}) {
            const int halo = softBlurHalo(sigma);
            FloatPlane input(rect.adjusted(-halo, -halo, halo, halo)), output(rect);
            for (int y = input.rect.top(); y <= input.rect.bottom(); ++y)
                for (int x = input.rect.left(); x <= input.rect.right(); ++x)
                    input.at(x, y) = float(.5 + .3 * std::sin(x * .03) + .2 * std::cos(y * .04));
            QVERIFY(softBlur(input, output, sigma));
            FloatPlane patch(QRect(-17, -19, 5, 4));
            QVERIFY(softBlur(input, patch, sigma));
            for (int y = patch.rect.top(); y <= patch.rect.bottom(); ++y)
                for (int x = patch.rect.left(); x <= patch.rect.right(); ++x)
                    QCOMPARE(patch.at(x, y), output.at(x, y));
            if (sigma == 3 || sigma == 8) {
                // Independent direct convolution of three known box kernels:
                // sigma 3 -> radii 2,3,3; sigma 8 -> radii 7,8,8.
                std::vector<double> kernel{1.0};
                for (int radius : {int(sigma) - 1, int(sigma), int(sigma)}) {
                    std::vector<double> next(kernel.size() + 2 * radius);
                    for (size_t i = 0; i < kernel.size(); ++i)
                        for (int k = 0; k < 2 * radius + 1; ++k)
                            next[i + k] += kernel[i] / (2 * radius + 1);
                    kernel.swap(next);
                }
                const int r = int(kernel.size() / 2);
                for (int y = rect.top(); y <= rect.bottom(); ++y)
                    for (int x = rect.left(); x <= rect.right(); ++x) {
                        double sum = 0;
                        for (int dy = -r; dy <= r; ++dy)
                            for (int dx = -r; dx <= r; ++dx)
                                sum += input.at(x + dx, y + dy) * kernel[dx + r] * kernel[dy + r];
                        QVERIFY(std::abs(output.at(x, y) - sum) < 2e-7);
                    }
            }
            const auto before = output.values;
            KoDummyUpdaterHolder progress;
            QMetaObject::invokeMethod(progress.updater(), "setInterrupted", Qt::DirectConnection, Q_ARG(bool, true));
            QVERIFY(!softBlur(input, output, sigma, progress.updater()));
            QVERIFY(output.values == before);
            if (halo) {
                FloatPlane incomplete(rect);
                QVERIFY(!softBlur(incomplete, output, sigma));
                QVERIFY(output.values == before);
            }
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
        KisGlowFilter filter;
        auto config = settings();
        // Include dark CMYK samples as emitters too.
        config->setProperty("effectArea", 100.0);
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
    void testBrightPassAlphaAndHdr()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        const QRect rect(0, 0, 1, 1);
        KisGlowFilter filter;
        auto config = settings();
        config->setProperty("blurRange", 0.0);
        for (float alpha : {0.0f, .25f, 1.0f}) {
            for (float v : {-.5f, .3f, .5f, .8f, 1.0f, 3.0f}) {
                KisPaintDeviceSP dev = new KisPaintDevice(cs);
                KoColor color(cs);
                cs->fromNormalisedChannelsValue(color.data(), QVector<float>{v, v, v, alpha});
                dev->fill(rect, color);
                const auto before = pixels(dev, rect);
                filter.process(dev, rect, config);
                KoColor result(cs);
                dev->pixel(0, 0, &result);
                QVector<float> values(4);
                cs->normalisedChannelsValue(result.data(), values);
                const double t = qBound(0.0, (double(v) - .4) / .2, 1.0);
                const double glow = qMax(0.0, double(v)) * t * t * (3 - 2 * t) * alpha;
                const double expected = v + glow - qBound(0.0, double(v), 1.0) * qMin(1.0, glow);
                QVERIFY(std::abs(values[0] - expected) < 2e-6);
                QCOMPARE(values[3], alpha);
                if (alpha == 0)
                    QCOMPARE(pixels(dev, rect), before);
            }
        }
        // Increasing Effect area must include progressively darker sources.
        float previous = .45f;
        for (double area : {0., 25., 50., 75., 100.}) {
            KisPaintDeviceSP dev = new KisPaintDevice(cs);
            KoColor color(cs);
            cs->fromNormalisedChannelsValue(color.data(), QVector<float>{.45f, .45f, .45f, 1});
            dev->fill(rect, color);
            config->setProperty("effectArea", area);
            filter.process(dev, rect, config);
            KoColor result(cs);
            dev->pixel(0, 0, &result);
            QVector<float> values(4);
            cs->normalisedChannelsValue(result.data(), values);
            QVERIFY(values[0] >= previous);
            previous = values[0];
        }
        QVERIFY(previous > .45f);
        // Bright hidden RGB cannot illuminate the visible layer.
        const QRect field(-20, -20, 40, 40);
        KisPaintDeviceSP dev = new KisPaintDevice(cs);
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{100, 100, 100, 0});
        dev->fill(field, color);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{.1f, .1f, .1f, .4f});
        dev->fill(QRect(-5, -5, 10, 10), color);
        config = settings();
        const auto before = pixels(dev, field);
        filter.process(dev, field, config);
        QCOMPARE(pixels(dev, field), before);
    }
    void testLargeRadiusPatches()
    {
        KisGlowFilter filter;
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        const QRect rect(-37, -29, 79, 71);
        for (double range : {0., 10., 30., 100.}) {
            auto config = settings();
            config->setProperty("blurRange", range);
            const QRect input = filter.neededRect(rect, config, 0);
            auto whole = pattern(cs, input), tiled = pattern(cs, input);
            filter.process(whole, rect, config);
            {
                KisTransaction transaction(tiled);
                for (int y = rect.top(); y <= rect.bottom(); y += 23)
                    for (int x = rect.left(); x <= rect.right(); x += 27)
                        filter.process(tiled, rect.intersected(QRect(x, y, 27, 23)), config);
            }
            QCOMPARE(pixels(tiled, rect), pixels(whole, rect));
        }
    }
    void testHalfOverflowAndNonfinite()
    {
        KisGlowFilter filter;
        auto config = settings();
        config->setProperty("blurRange", 0.0);
        config->setProperty("strength", 100.0);
        const QRect rect(0, 0, 2, 2);
        for (const QString &model : {QString("RGBA"), QString("GRAYA")}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace(model, "F16", QString());
            QVERIFY(cs);
            QVector<float> values(cs->channelCount(), 50000.f);
            values.back() = 1;
            KoColor color(cs);
            cs->fromNormalisedChannelsValue(color.data(), values);
            KisPaintDeviceSP dev = new KisPaintDevice(cs);
            dev->fill(rect, color);
            const auto before = pixels(dev, rect);
            filter.process(dev, rect, config);
            QCOMPARE(pixels(dev, rect), before); // do not encode infinity into half
        }
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{std::numeric_limits<float>::quiet_NaN(), 1, 1, 1});
        KisPaintDeviceSP dev = new KisPaintDevice(cs);
        dev->fill(rect, color);
        const auto before = pixels(dev, rect);
        filter.process(dev, rect, config);
        QCOMPARE(pixels(dev, rect), before);
    }
    void testSelectionUndoChannelsAndUpdates()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(-32, -32, 64, 64), part(-8, -8, 13, 15);
        KisGlowFilter filter;
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
        QCOMPARE(filter.changedRect(part, config, 0),
                 part.adjusted(-SolsticeEffects::softBlurHalo(5.76),
                               -SolsticeEffects::softBlurHalo(5.76),
                               SolsticeEffects::softBlurHalo(5.76),
                               SolsticeEffects::softBlurHalo(5.76)));
    }
    void testConfigurationAndCancellation()
    {
        KisGlowFilter filter;
        auto config = settings();
        config->setProperty("strength", 72.5);
        config->setProperty("effectArea", 63.5);
        config->setProperty("blurRange", 85.5);
        KisGlowConfigWidget widget(nullptr, nullptr);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        auto saved = widget.configuration();
        QCOMPARE(saved->getDouble("strength"), 72.5);
        QCOMPARE(saved->getDouble("effectArea"), 63.5);
        QCOMPARE(saved->getDouble("blurRange"), 85.5);
        auto restored = filter.factoryConfiguration(KisGlobalResourcesInterface::instance());
        restored->fromXML(saved->toXML());
        const QRect rect(-20, -20, 64, 64);
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        auto first = pattern(cs, rect), second = pattern(cs, rect);
        filter.process(first, rect, config);
        filter.process(second, rect, restored->cloneWithResourcesSnapshot());
        QCOMPARE(pixels(first, rect), pixels(second, rect));
        widget.findChild<KisDoubleSliderSpinBox *>("glowStrength")->setValue(50);
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
        auto filter = KisFilterRegistry::instance()->value("solstice_glow");
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
        KisFilterMaskSP mask = new KisFilterMask(parent.image, "Glow");
        parent.image->addNode(mask, parent.layer);
        auto restored = KisGlowFilter().factoryConfiguration(KisGlobalResourcesInterface::instance());
        restored->fromXML(config->toXML());
        mask->setFilter(restored->cloneWithResourcesSnapshot());
        mask->initSelection(parent.layer);
        mask->select(rect, MAX_SELECTED);
        auto original = pattern(parent.layer->colorSpace(), rect);
        KisPaintDeviceSP expected = new KisPaintDevice(*original), actual = new KisPaintDevice(*original);
        KisGlowFilter().process(expected, rect, config);
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
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_GLOW"))
            QSKIP("Opt-in timings");
        for (const QString &depth : {QString("U8"), QString("F32")}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
            for (const QSize size : {QSize(2480, 3508), QSize(4096, 4096)}) {
                for (double range : {10.0, 100.0}) {
                    QList<qint64> times;
                    auto config = settings();
                    config->setProperty("blurRange", range);
                    for (int run = 0; run < 3; ++run) {
                        const QRect rect(QPoint(), size);
                        auto dev = pattern(cs, rect);
                        QElapsedTimer timer;
                        timer.start();
                        KisGlowFilter().process(dev, rect, config);
                        times << timer.elapsed();
                    }
                    std::sort(times.begin(), times.end());
                    qInfo() << "Glow CPU single call" << depth << size << "range" << range << "median ms" << times[1];
                }
            }
        }
    }
    void compareBlurStrategies()
    {
        const QString directory = qEnvironmentVariable("SOLSTICE_GLOW_ARTIFACT_DIR");
        if (directory.isEmpty())
            QSKIP("Opt-in blur comparison");
        using namespace SolsticeEffects;
        const QRect rect(0, 0, 256, 256);
        QImage sheet(768, 576, QImage::Format_RGB32);
        sheet.fill(Qt::white);
        QPainter painter(&sheet);
        int row = 0;
        for (double sigma : {4.0, 32.0}) {
            const int halo = qMax(softBlurHalo(sigma * std::sqrt(1.75)), gaussianHalo(sigma));
            FloatPlane source(rect.adjusted(-halo, -halo, halo, halo));
            for (int y = source.rect.top(); y <= source.rect.bottom(); ++y)
                for (int x = source.rect.left(); x <= source.rect.right(); ++x) {
                    const bool disk = (x - 80) * (x - 80) + (y - 96) * (y - 96) < 100;
                    const bool square = x > 140 && x < 170 && y > 140 && y < 170;
                    source.at(x, y) = disk || square ? 1 : 0;
                }
            FloatPlane exact(rect), fast(rect), narrow(rect), broad(rect);
            QElapsedTimer timer;
            timer.start();
            QVERIFY(gaussianBlur(source, exact, sigma));
            const auto exactMs = timer.elapsed();
            timer.restart();
            QVERIFY(softBlur(source, fast, sigma));
            const auto fastMs = timer.elapsed();
            timer.restart();
            QVERIFY(softBlur(source, narrow, sigma * .5));
            QVERIFY(softBlur(source, broad, sigma * std::sqrt(1.75)));
            const auto mixedMs = timer.elapsed();
            double squaredError = 0;
            for (size_t i = 0; i < fast.values.size(); ++i)
                squaredError += std::pow(fast.values[i] - exact.values[i], 2);
            qInfo() << "Blur strategy sigma" << sigma << "Gaussian/boxes/two-scale ms" << exactMs << fastMs << mixedMs
                    << "boxes RMS error" << std::sqrt(squaredError / fast.values.size());
            for (int column = 0; column < 3; ++column) {
                QImage image(256, 256, QImage::Format_RGB32);
                for (int y = 0; y < 256; ++y)
                    for (int x = 0; x < 256; ++x) {
                        const double v = column == 0 ? exact.at(x, y)
                            : column == 1            ? fast.at(x, y)
                                                     : .5 * (narrow.at(x, y) + broad.at(x, y));
                        const int gray = qBound(0, qRound(255 * std::sqrt(qMax(0.0, v))), 255);
                        image.setPixel(x, y, qRgb(gray, gray, gray));
                    }
                painter.drawText(QRect(column * 256, row * 288, 256, 32),
                                 Qt::AlignCenter,
                                 QString("%1, sigma %2")
                                     .arg(QStringList{"Gaussian", "Three boxes", "Two scales"}[column])
                                     .arg(sigma));
                painter.drawImage(QPoint(column * 256, row * 288 + 32), image);
            }
            ++row;
        }
        painter.end();
        QVERIFY(sheet.save(QDir(directory).filePath("glow-blur-strategies.png")));
    }
    void renderReviewArtifacts()
    {
        const QString directory = qEnvironmentVariable("SOLSTICE_GLOW_ARTIFACT_DIR");
        if (directory.isEmpty())
            QSKIP("Opt-in review images");
        QImage source(384, 256, QImage::Format_ARGB32);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x) {
                const double spot = std::exp(-((x - 115.) * (x - 115.) + (y - 100.) * (y - 100.)) / 110.)
                    + std::exp(-((x - 265.) * (x - 265.) + (y - 160.) * (y - 160.)) / 340.);
                const double wave = qMin(1.0, .12 + .15 * y / 255. + spot);
                source.setPixelColor(x,
                                     y,
                                     QColor(int(220 * wave), int(240 * wave), int(255 * wave), y < 128 ? 255 : 128));
            }
        QImage sheet(1152, 288, QImage::Format_ARGB32);
        sheet.fill(Qt::white);
        QPainter painter(&sheet);
        KisGlowFilter filter;
        for (int i = 0; i < 3; ++i) {
            KisPaintDeviceSP dev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
            dev->convertFromQImage(source, nullptr);
            auto config = settings();
            config->setProperty("strength", i * 50.0);
            filter.process(dev, source.rect(), config);
            painter.drawText(QRect(i * 384, 0, 384, 32), Qt::AlignCenter, QString("Strength %1").arg(i * 50));
            painter.drawImage(QPoint(i * 384, 32), dev->convertToQImage(nullptr, source.rect()));
            if (i == 1) {
                KisGlowConfigWidget widget(nullptr, dev);
                widget.setConfiguration(config);
                widget.resize(widget.sizeHint());
                widget.show();
                QApplication::processEvents();
                QVERIFY(widget.grab().save(QDir(directory).filePath("glow-widget.png")));
            }
        }
        painter.end();
        QVERIFY(sheet.save(QDir(directory).filePath("glow-comparison.png")));
    }
};
SIMPLE_TEST_MAIN(KisGlowFilterTest)
#include "KisGlowFilterTest.moc"
