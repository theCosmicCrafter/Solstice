/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisLineArtFilter.h"
#include "KisCpuPlaneBlur.h"
#include "KisLineArtConfigWidget.h"
#include <KisGlobalResourcesInterface.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
#include <QBuffer>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalSpy>
#include <algorithm>
#include <cmath>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <future>
#include <kis_color_button.h>
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
    return KisLineArtFilter()
        .defaultConfiguration(KisGlobalResourcesInterface::instance())
        ->cloneWithResourcesSnapshot();
}
} // namespace

class KisLineArtFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testMaximumReference()
    {
        using namespace SolsticeEffects;
        for (int radius : {0, 1, 3, 8, 32, 256}) {
            const QRect rect(-17, -13, 37, 29);
            FloatPlane source(rect.adjusted(-radius, -radius, radius, radius)), result(rect);
            for (int y = source.rect.top(); y <= source.rect.bottom(); ++y)
                for (int x = source.rect.left(); x <= source.rect.right(); ++x)
                    source.at(x, y) = float(((x * 173 + y * 47) % 101 + 101) % 101) / 100.f;
            QVERIFY(maximumFilter(source, result, radius));
            // Independent brute force, including asymmetric patch boundaries.
            for (int y = rect.top(); y <= rect.bottom(); y += 7)
                for (int x = rect.left(); x <= rect.right(); x += 9) {
                    float expected = -1;
                    for (int dy = -radius; dy <= radius; ++dy)
                        for (int dx = -radius; dx <= radius; ++dx)
                            expected = qMax(expected, source.at(x + dx, y + dy));
                    QCOMPARE(result.at(x, y), expected);
                }
            FloatPlane patch(QRect(-9, -7, 11, 13));
            QVERIFY(maximumFilter(source, patch, radius));
            for (int y = patch.rect.top(); y <= patch.rect.bottom(); ++y)
                for (int x = patch.rect.left(); x <= patch.rect.right(); ++x)
                    QCOMPARE(patch.at(x, y), result.at(x, y));
            FloatPlane incomplete(rect), sentinel(rect);
            std::fill(sentinel.values.begin(), sentinel.values.end(), 42.f);
            if (radius > 0) {
                QVERIFY(!maximumFilter(incomplete, sentinel, radius));
                QCOMPARE(sentinel.at(-17, -13), 42.f);
            }
            KoDummyUpdaterHolder cancelled;
            QMetaObject::invokeMethod(cancelled.updater(), "setInterrupted", Qt::DirectConnection, Q_ARG(bool, true));
            QVERIFY(!maximumFilter(source, sentinel, radius, cancelled.updater()));
            QCOMPARE(sentinel.at(-17, -13), 42.f);
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
        const QRect rect(-23, -19, 48, 41);
        auto config = settings();
        KisLineArtFilter filter;
        for (const QString &mode : {QString("black"), QString("original")}) {
            config->setProperty("lineColor", mode);
            auto whole = pattern(cs, rect), tiled = pattern(cs, rect);
            const auto before = pixels(whole, rect);
            filter.process(whole, rect, config);
            {
                KisTransaction transaction(tiled);
                std::vector<std::future<void>> jobs;
                for (int stripe = 0; stripe < 4; ++stripe)
                    jobs.push_back(std::async(std::launch::async, [&, stripe] {
                        filter.process(tiled,
                                       rect.intersected(QRect(rect.x(), rect.y() + stripe * 11, rect.width(), 11)),
                                       config);
                    }));
                for (auto &job : jobs)
                    job.get();
            }
            const auto after = pixels(whole, rect);
            QVERIFY(after != before);
            QCOMPARE(pixels(tiled, rect), after);
            for (int p = 0; p < before.size(); p += cs->pixelSize()) {
                const auto *src = reinterpret_cast<const quint8 *>(before.constData() + p);
                const auto *dst = reinterpret_cast<const quint8 *>(after.constData() + p);
                QVERIFY(cs->opacityF(dst) <= cs->opacityF(src));
                if (cs->opacityF(src) == 0)
                    QCOMPARE(after.mid(p, cs->pixelSize()), before.mid(p, cs->pixelSize()));
                if (mode == "original")
                    for (const auto *ch : cs->channels())
                        if (ch->channelType() == KoChannelInfo::COLOR)
                            QCOMPARE(after.mid(p + ch->pos(), ch->size()), before.mid(p + ch->pos(), ch->size()));
            }
        }
    }
    void testPaperAndDensity()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        const QRect rect(-64, -64, 128, 128);
        KisLineArtFilter filter;
        auto config = settings();
        auto fill = [cs](double light, double alpha = 1) {
            KoColor color(cs);
            cs->fromNormalisedChannelsValue(color.data(),
                                            QVector<float>{float(light), float(light), float(light), float(alpha)});
            return color;
        };
        // Uniform paper must disappear, including its transparent boundary.
        for (double light : {1.0, .85, .7}) {
            KisPaintDeviceSP dev = new KisPaintDevice(cs);
            dev->fill(rect, fill(light));
            filter.process(dev, rect, config);
            const auto data = pixels(dev, rect);
            for (int p = 0; p < data.size(); p += cs->pixelSize())
                QCOMPARE(cs->opacityF(reinterpret_cast<const quint8 *>(data.constData() + p)), 0.0);
        }
        // A broad dark region has no trustworthy paper observation. Keep ink.
        for (double light : {0.0, .1, .25}) {
            KisPaintDeviceSP dev = new KisPaintDevice(cs);
            dev->fill(rect, fill(light, .5));
            filter.process(dev, rect, config);
            KoColor out(cs);
            dev->pixel(0, 0, &out);
            QVERIFY(cs->opacityF(out.data()) > .35);
        }
        // Faint lines on off-white paper: density monotonically increases alpha.
        KisPaintDeviceSP original = new KisPaintDevice(cs);
        original->fill(rect, fill(.8));
        original->fill(QRect(-5, -50, 3, 100), fill(.6, .5));
        double previous = -1;
        for (double d : {0., 25., 50., 100., 150., 200.}) {
            config->setProperty("density", d);
            KisPaintDeviceSP dev = new KisPaintDevice(*original);
            filter.process(dev, rect, config);
            KoColor out(cs);
            dev->pixel(-4, 0, &out);
            const double a = cs->opacityF(out.data());
            QVERIFY(a >= previous && a <= .5);
            previous = a;
            if (d == 0)
                QCOMPARE(a, 0.0);
            if (d == 100)
                QVERIFY(std::abs(a - .125) < 1e-6);
            if (d == 200)
                QVERIFY(std::abs(a - .25) < 1e-6);
        }
        // Missing pixels' hidden white/HDR/NaN values cannot alter the estimate.
        config = settings();
        KisPaintDeviceSP a = new KisPaintDevice(cs), b = new KisPaintDevice(cs);
        a->fill(rect, fill(100, 0));
        b->fill(rect, fill(std::numeric_limits<float>::quiet_NaN(), 0));
        a->fill(QRect(0, 0, 1, 1), fill(0, .5));
        b->fill(QRect(0, 0, 1, 1), fill(0, .5));
        filter.process(a, rect, config);
        filter.process(b, rect, config);
        KoColor ca(cs), cb(cs);
        a->pixel(0, 0, &ca);
        b->pixel(0, 0, &cb);
        QCOMPARE(cs->opacityF(ca.data()), .5);
        QCOMPARE(ca, cb);
        // Nonfinite visible samples are preserved, not propagated.
        auto invalid = fill(std::numeric_limits<float>::quiet_NaN(), 1);
        a->fill(QRect(0, 0, 1, 1), invalid);
        const auto bytes = pixels(a, QRect(0, 0, 1, 1));
        filter.process(a, rect, config);
        QCOMPARE(pixels(a, QRect(0, 0, 1, 1)), bytes);
        // Valid negative/HDR channels survive Keep original byte-for-byte.
        config->setProperty("lineColor", "original");
        KoColor hdr(cs);
        cs->fromNormalisedChannelsValue(hdr.data(), QVector<float>{2, -.4f, 0, 1});
        a = new KisPaintDevice(cs);
        a->fill(rect, fill(1));
        a->fill(QRect(0, 0, 1, 1), hdr);
        filter.process(a, rect, config);
        a->pixel(0, 0, &ca);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(ca.data()), 12),
                 QByteArray(reinterpret_cast<const char *>(hdr.data()), 12));
    }
    void testPhotographedPaper()
    {
        // Generated test chart: colored off-white paper, slow lighting gradient,
        // grain, pencil/ink lines. JPEG simulates scan/photo compression.
        QImage source(256, 192, QImage::Format_RGB32);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x) {
                int paper = 220 + x / 12 + y / 32 + ((x * 13 + y * 7) % 5) - 2;
                if ((x >= 40 && x < 44) || (x >= 112 && x < 116))
                    paper -= x < 80 ? 130 : 40;
                source.setPixelColor(x, y, QColor(paper, qMax(0, paper - 4), qMax(0, paper - 12)));
            }
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(source.save(&buffer, "JPG", 85));
        QImage decoded = QImage::fromData(jpeg, "JPG");
        QVERIFY(!decoded.isNull());
        KisPaintDeviceSP dev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
        dev->convertFromQImage(decoded, nullptr);
        KisLineArtFilter().process(dev, decoded.rect(), settings());
        auto output = dev->convertToQImage(nullptr, decoded.rect());
        double paper = 0, ink = 0, pencil = 0;
        for (int y = 20; y < 170; ++y) {
            paper += qAlpha(output.pixel(190, y));
            ink += qAlpha(output.pixel(42, y));
            pencil += qAlpha(output.pixel(114, y));
        }
        QVERIFY(paper / 150 < 3);
        QVERIFY(ink / 150 > 100);
        QVERIFY(pencil / 150 > 25);
    }
    void testBoundsAndChannels()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(-31, -29, 65, 61), core(-10, -10, 20, 20);
        KisLineArtFilter filter;
        for (int r : {8, 32, 256}) {
            auto config = settings();
            config->setProperty("paperRadius", r);
            auto original = pattern(cs, core);
            KisPaintDeviceSP whole = new KisPaintDevice(*original), tiled = new KisPaintDevice(*original);
            filter.process(whole, rect, config);
            {
                KisTransaction tx(tiled);
                filter.process(tiled, QRect(-31, -29, 65, 31), config);
                filter.process(tiled, QRect(-31, 2, 65, 30), config);
            }
            QCOMPARE(pixels(whole, rect), pixels(tiled, rect));
            const QRect affected = filter.changedRect(core, config, 0);
            QCOMPARE(affected, filter.neededRect(core, config, 0));
            original->clear(core);
            filter.process(original, whole, nullptr, affected, config);
            QCOMPARE(pixels(whole, rect), pixels(original, rect));
        }
        auto config = settings();
        auto original = pattern(cs, rect);
        const auto before = pixels(original, rect);
        KisPaintDeviceSP full = new KisPaintDevice(*original);
        filter.process(full, rect, config);
        const auto complete = pixels(full, rect);
        for (int enabled = -1; enabled < int(cs->channelCount()); ++enabled) {
            QBitArray flags(cs->channelCount(), false);
            if (enabled >= 0)
                flags.setBit(enabled);
            config->setChannelFlags(flags);
            KisPaintDeviceSP dev = new KisPaintDevice(*original);
            filter.process(dev, rect, config);
            const auto result = pixels(dev, rect);
            for (int p = 0; p < result.size(); p += cs->pixelSize())
                for (int i = 0; i < int(cs->channelCount()); ++i) {
                    const auto *ch = cs->channels()[i];
                    QCOMPARE(result.mid(p + ch->pos(), ch->size()),
                             (i == enabled ? complete : before).mid(p + ch->pos(), ch->size()));
                }
        }
        config = settings();
        KisPaintDeviceSP selected = new KisPaintDevice(*original);
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(QRect(0, 0, 20, 20), 128);
        KisTransaction tx(selected);
        filter.process(original, selected, selection, rect, config);
        const auto after = pixels(selected, rect);
        QVERIFY(after != before);
        QCOMPARE(pixels(selected, QRect(-31, -29, 65, 29)), pixels(original, QRect(-31, -29, 65, 29)));
        std::unique_ptr<KUndo2Command> command(tx.endAndTake());
        command->redo();
        command->undo();
        QCOMPARE(pixels(selected, rect), before);
        command->redo();
        QCOMPARE(pixels(selected, rect), after);
    }
    void testConfigurationMaskCancellation()
    {
        auto filter = KisFilterRegistry::instance()->value("solstice_line_art");
        QVERIFY(filter);
        QVERIFY(filter->supportsAdjustmentLayers());
        QVERIFY(filter->supportsThreading());
        QVERIFY(!filter->supportsLevelOfDetail(settings(), 1));
        QVERIFY(!filter->needsTransparentPixels(settings(), KoColorSpaceRegistry::instance()->rgb8()));
        auto config = settings();
        config->setProperty("density", 137.5);
        config->setProperty("paperRadius", 47);
        config->setProperty("lineColor", "original");
        KisLineArtConfigWidget widget(nullptr, nullptr);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        auto saved = widget.configuration();
        QCOMPARE(saved->getDouble("density"), 137.5);
        QCOMPARE(saved->getInt("paperRadius"), 47);
        QCOMPARE(saved->getString("lineColor"), QString("original"));
        auto restored = settings();
        restored->fromXML(saved->toXML());
        const QRect rect(0, 0, 64, 64);
        TestUtil::MaskParent parent(rect);
        KisFilterMaskSP mask = new KisFilterMask(parent.image, "Line Art");
        parent.image->addNode(mask, parent.layer);
        mask->setFilter(restored->cloneWithResourcesSnapshot());
        mask->initSelection(parent.layer);
        mask->select(rect, MAX_SELECTED);
        auto original = pattern(parent.layer->colorSpace(), rect);
        KisPaintDeviceSP expected = new KisPaintDevice(*original), actual = new KisPaintDevice(*original);
        filter->process(expected, rect, config);
        mask->apply(actual, rect, rect, KisNode::N_FILTHY, KisRenderPassFlag::None);
        const auto result = pixels(actual, rect), reference = pixels(expected, rect);
        const auto *cs = actual->colorSpace();
        for (int p = 0; p < result.size(); p += cs->pixelSize()) {
            const auto *ref = reinterpret_cast<const quint8 *>(reference.constData() + p);
            const auto *out = reinterpret_cast<const quint8 *>(result.constData() + p);
            QCOMPARE(cs->opacityF(out), cs->opacityF(ref));
            if (cs->opacityF(ref) > 0)
                QCOMPARE(result.mid(p, cs->pixelSize()), reference.mid(p, cs->pixelSize()));
        }
        widget.findChild<QComboBox *>("lineArtColor")->setCurrentIndex(0);
        QCOMPARE(spy.count(), 1);
        for (int threshold : {10, 25, 45, 60, 80}) {
            KisPaintDeviceSP dev = new KisPaintDevice(*original);
            const auto before = pixels(dev, rect);
            KoDummyUpdaterHolder progress;
            connect(progress.updater(), &KoUpdater::sigProgress, this, [&](int percent) {
                if (percent >= threshold)
                    QMetaObject::invokeMethod(progress.updater(),
                                              "setInterrupted",
                                              Qt::DirectConnection,
                                              Q_ARG(bool, true));
            });
            filter->process(dev, rect, config, progress.updater());
            QVERIFY(progress.updater()->interrupted());
            QCOMPARE(pixels(dev, rect), before);
        }
    }
    void benchmarkCpu()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_LINE_ART"))
            QSKIP("Opt-in timings");
        for (const QString &depth : {QString("U8"), QString("F32")})
            for (const QSize size : {QSize(2480, 3508), QSize(4096, 4096)})
                for (int r : {32, 256}) {
                    auto config = settings();
                    config->setProperty("paperRadius", r);
                    QList<qint64> times;
                    for (int run = 0; run < 3; ++run) {
                        const QRect rect(QPoint(), size);
                        auto dev =
                            pattern(KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString()), rect);
                        QElapsedTimer timer;
                        timer.start();
                        KisLineArtFilter().process(dev, rect, config);
                        times << timer.elapsed();
                    }
                    std::sort(times.begin(), times.end());
                    qInfo() << "Line Art CPU single call" << depth << size << "radius" << r << "median ms" << times[1];
                }
    }
    void renderReviewArtifacts()
    {
        const QString directory = qEnvironmentVariable("SOLSTICE_LINE_ART_ARTIFACT_DIR");
        if (directory.isEmpty())
            QSKIP("Opt-in review images");
        QImage source(320, 256, QImage::Format_RGB32);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x) {
                const int light = 218 + x / 15 + y / 30 + ((x * 17 + y * 11) % 5) - 2;
                source.setPixelColor(x, y, QColor(light, light - 4, light - 12));
            }
        QPainter drawing(&source);
        drawing.setRenderHint(QPainter::Antialiasing);
        drawing.setPen(QPen(QColor(65, 45, 40), 4));
        drawing.drawEllipse(QRectF(55, 30, 205, 190));
        drawing.drawArc(QRectF(90, 145, 130, 40), 180 * 16, 180 * 16);
        drawing.drawEllipse(QRectF(105, 88, 14, 22));
        drawing.drawEllipse(QRectF(195, 88, 14, 22));
        drawing.setPen(QPen(QColor(155, 125, 125), 2));
        drawing.drawLine(75, 140, 105, 126);
        drawing.drawLine(224, 126, 248, 140);
        drawing.end();
        QImage sheet(960, 576, QImage::Format_RGB32);
        sheet.fill(Qt::white);
        QPainter p(&sheet);
        for (int mode = 0; mode < 2; ++mode)
            for (int d = 0; d < 3; ++d) {
                KisPaintDeviceSP dev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
                dev->convertFromQImage(source, nullptr);
                auto config = settings();
                config->setProperty("density", d == 1 ? 100. : 200.);
                config->setProperty("lineColor", mode ? "original" : "black");
                if (d > 0)
                    KisLineArtFilter().process(dev, source.rect(), config);
                const QPoint origin(d * 320, mode * 288 + 32);
                for (int y = 0; y < 256; y += 16)
                    for (int x = 0; x < 320; x += 16)
                        p.fillRect(QRect(origin + QPoint(x, y), QSize(16, 16)),
                                   ((x + y) / 16) % 2 ? QColor(210, 210, 210) : QColor(240, 240, 240));
                p.drawText(
                    QRect(d * 320, mode * 288, 320, 32),
                    Qt::AlignCenter,
                    d == 0 ? "Original"
                           : QString("%1 / density %2").arg(mode ? "Keep original" : "Black").arg(d == 1 ? 100 : 200));
                p.drawImage(origin, dev->convertToQImage(nullptr, source.rect()));
            }
        p.end();
        QVERIFY(sheet.save(QDir(directory).filePath("line-art-comparison.png")));
        KisLineArtConfigWidget widget(nullptr, nullptr);
        widget.setConfiguration(settings());
        widget.resize(widget.sizeHint());
        widget.show();
        QApplication::processEvents();
        QVERIFY(widget.grab().save(QDir(directory).filePath("line-art-widget.png")));
    }
};
SIMPLE_TEST_MAIN(KisLineArtFilterTest)
#include "KisLineArtFilterTest.moc"
