/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisPencilFilter.h"
#include "KisPencilConfigWidget.h"
#include <KisGlobalResourcesInterface.h>
#include <KoChannelInfo.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <algorithm>
#include <cmath>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <future>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_slider_spin_box.h>
#include <kis_transaction.h>
#include <memory>
#include <simpletest.h>

namespace
{
QByteArray pixels(KisPaintDeviceSP device, const QRect &rect)
{
    QByteArray data(rect.width() * rect.height() * device->pixelSize(), '\0');
    device->readBytes(reinterpret_cast<quint8 *>(data.data()), rect);
    return data;
}
KisPaintDeviceSP pattern(const KoColorSpace *cs, const QRect &rect)
{
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    device->setX(13);
    device->setY(-11);
    QByteArray data(rect.width() * rect.height() * cs->pixelSize(), '\0');
    QVector<float> values(cs->channelCount());
    for (int y = 0; y < rect.height(); ++y) {
        for (int x = 0; x < rect.width(); ++x) {
            for (int c = 0; c < values.size(); ++c)
                values[c] = cs->channels()[c]->channelType() == KoChannelInfo::ALPHA
                    ? float((x + y) % 4) / 3.0f
                    : float((x * 7 + y * 13 + c * 17) % 101) / 100.0f;
            cs->fromNormalisedChannelsValue(reinterpret_cast<quint8 *>(data.data())
                                                + (y * rect.width() + x) * cs->pixelSize(),
                                            values);
        }
    }
    device->writeBytes(reinterpret_cast<const quint8 *>(data.constData()), rect);
    return device;
}
KisFilterConfigurationSP settings()
{
    KisPencilFilter filter;
    auto config = filter.defaultConfiguration(KisGlobalResourcesInterface::instance());
    config->setProperty("seed", 1234567);
    config->setProperty("hatchSize", 4.75);
    config->setProperty("angle", 37.0);
    return config->cloneWithResourcesSnapshot();
}
} // namespace

class KisPencilFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testInstalledPlugin()
    {
        auto filter = KisFilterRegistry::instance()->value("solstice_pencil");
        QVERIFY2(filter, "Install kritasolsticeeffectsfilter before running this test.");
        QVERIFY(filter->supportsThreading());
        QVERIFY(filter->supportsAdjustmentLayers());
        QVERIFY(!filter->supportsLevelOfDetail(settings(), 1));
        const QRect rect(0, 0, 32, 32);
        auto device = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
        const auto before = pixels(device, rect);
        KoDummyUpdaterHolder progress;
        filter->process(device, rect, settings(), progress.updater());
        QVERIFY(pixels(device, rect) != before);
        QCOMPARE(progress.updater()->progress(), 100);
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
        KisPencilFilter filter;
        for (int mode : {0, 1, 2}) {
            auto config = settings();
            config->setProperty("outlines", mode != 1);
            config->setProperty("hatching", mode != 0);
            auto whole = pattern(cs, rect), tiled = pattern(cs, rect);
            const auto before = pixels(whole, rect);
            filter.process(whole, rect, config);
            {
                KisTransaction transaction(tiled);
                std::vector<std::future<void>> jobs;
                for (int stripe = 0; stripe < 4; ++stripe) {
                    jobs.push_back(std::async(std::launch::async, [&, stripe]() {
                        for (int y = rect.top() + stripe * 17; y <= rect.bottom(); y += 68)
                            for (int x = rect.left(); x <= rect.right(); x += 23)
                                filter.process(tiled, rect.intersected(QRect(x, y, 23, 17)), config);
                    }));
                }
                for (auto &job : jobs)
                    job.get();
            }
            const auto after = pixels(whole, rect);
            QVERIFY(before != after);
            QCOMPARE(pixels(tiled, rect), after);
            for (int offset = 0; offset < before.size(); offset += cs->pixelSize()) {
                if (cs->opacityF(reinterpret_cast<const quint8 *>(before.constData() + offset)) == 0.0)
                    QCOMPARE(after.mid(offset, cs->pixelSize()), before.mid(offset, cs->pixelSize()));
                for (const auto *channel : cs->channels())
                    if (channel->channelType() == KoChannelInfo::ALPHA)
                        QCOMPARE(after.mid(offset + channel->pos(), channel->size()),
                                 before.mid(offset + channel->pos(), channel->size()));
            }
            auto disabled = pattern(cs, rect);
            config->setProperty("outlines", false);
            config->setProperty("hatching", false);
            config->setProperty("grayscale", true);
            filter.process(disabled, rect, config);
            QCOMPARE(pixels(disabled, rect), before);
            QCOMPARE(filter.neededRect(rect, config, 0), rect);
            config->setProperty("outlines", true);
            config->setProperty("hatching", true);
            config->setProperty("outlineStrength", 0.0);
            config->setProperty("hatchStrength", 0.0);
            filter.process(disabled, rect, config);
            QCOMPARE(pixels(disabled, rect), before);
        }
    }
    void testOutlinesAndTransparentCoverage()
    {
        KisPencilFilter filter;
        auto config = settings();
        config->setProperty("hatching", false);
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        const QRect rect(-16, -16, 32, 32);
        KisPaintDeviceSP device = new KisPaintDevice(cs);
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{100, 0, 0, 0});
        device->fill(rect, color);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{0, .5f, 1, .5f});
        device->fill(QRect(-8, -8, 16, 16), color);
        const auto before = pixels(device, rect);
        filter.process(device, rect, config);
        QCOMPARE(pixels(device, rect), before); // no silhouette/hidden-RGB edge
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{.8f, .8f, .8f, 1});
        device->fill(rect, color);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{.2f, .2f, .2f, 1});
        device->fill(QRect(-16, -16, 16, 32), color);
        const auto flat = pixels(device, QRect(5, 0, 1, 1));
        filter.process(device, rect, config);
        QCOMPARE(pixels(device, QRect(5, 0, 1, 1)), flat);
        QVector<float> values(4);
        const auto edge = pixels(device, QRect(0, 0, 1, 1));
        cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(edge.constData()), values);
        QVERIFY(values[0] < .8f);
        QCOMPARE(values[3], 1.0f);
        QCOMPARE(filter.neededRect(rect, config, 0), rect.adjusted(-2, -2, 2, 2));
    }
    void testHatchingControls()
    {
        KisPencilFilter filter;
        auto config = settings();
        config->setProperty("outlines", false);
        config->setProperty("roughness", 0.0);
        config->setProperty("angle", 0.0);
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(-32, -32, 64, 64);
        const auto render = [&]() {
            KisPaintDeviceSP device = new KisPaintDevice(cs);
            device->fill(rect, KoColor(Qt::gray, cs));
            filter.process(device, rect, config);
            return device;
        };
        auto horizontal = render();
        for (int y = -32; y < 32; ++y)
            QCOMPARE(pixels(horizontal, QRect(-20, y, 1, 1)), pixels(horizontal, QRect(23, y, 1, 1)));
        config->setProperty("seed", 17);
        auto anotherSeed = render();
        QCOMPARE(pixels(horizontal, rect), pixels(anotherSeed, rect));
        config->setProperty("angle", 180.0);
        QCOMPARE(pixels(horizontal, rect), pixels(render(), rect));
        config->setProperty("angle", 90.0);
        auto vertical = render();
        for (int x = -32; x < 32; ++x)
            QCOMPARE(pixels(vertical, QRect(x, -20, 1, 1)), pixels(vertical, QRect(x, 23, 1, 1)));
        QVERIFY(pixels(horizontal, rect) != pixels(vertical, rect));
        config->setProperty("hatchSize", 8.25);
        QVERIFY(pixels(vertical, rect) != pixels(render(), rect));
        config->setProperty("roughness", 100.0);
        auto rough = render();
        config->setProperty("seed", 18);
        QVERIFY(pixels(rough, rect) != pixels(render(), rect));
        QCOMPARE(filter.neededRect(rect, config, 0), rect);
        KisPaintDeviceSP white = new KisPaintDevice(cs);
        white->fill(rect, KoColor(Qt::white, cs));
        const auto before = pixels(white, rect);
        filter.process(white, rect, config);
        QCOMPARE(pixels(white, rect), before);
    }
    void testGrayscaleAndHdr()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        const QRect rect(-10, -10, 32, 32);
        KisPencilFilter filter;
        auto config = settings();
        config->setProperty("grayscale", true);
        auto device = pattern(cs, rect);
        filter.process(device, rect, config);
        const auto data = pixels(device, rect);
        QVector<float> values(4);
        for (int p = 0; p < data.size(); p += cs->pixelSize()) {
            cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(data.constData() + p), values);
            if (values[3] > 0) {
                QCOMPARE(values[0], values[1]);
                QCOMPARE(values[1], values[2]);
            }
        }
        config->setProperty("grayscale", false);
        config->setProperty("hatching", false);
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{-2, .5f, 3, .375f});
        device->fill(rect, color);
        const auto before = pixels(device, rect);
        filter.process(device, rect, config);
        QCOMPARE(pixels(device, rect), before);
        config->setProperty("hatching", true);
        filter.process(device, rect, config);
        const auto sample = pixels(device, QRect(0, 0, 1, 1));
        cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(sample.constData()), values);
        QVERIFY(values[0] < 0);
        QVERIFY(values[2] > 1);
        QCOMPARE(values[3], .375f);
    }
    void testPartialDirtySelectionChannelsAndUndo()
    {
        KisPencilFilter filter;
        auto config = settings();
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(-32, -32, 64, 64), part(-8, -8, 13, 15);
        auto source = pattern(cs, rect);
        const auto before = pixels(source, rect);
        KisPaintDeviceSP full = new KisPaintDevice(*source), partial = new KisPaintDevice(*source);
        filter.process(full, rect, config);
        filter.process(source, partial, nullptr, part, config);
        QCOMPARE(pixels(partial, part), pixels(full, part));
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(part, 128);
        KisPaintDeviceSP selected = new KisPaintDevice(*source);
        KisTransaction transaction(selected);
        filter.process(source, selected, selection, rect, config);
        const auto after = pixels(selected, rect);
        QVERIFY(after != before);
        QCOMPARE(pixels(selected, QRect(-32, -32, 64, 20)), pixels(source, QRect(-32, -32, 64, 20)));
        std::unique_ptr<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        command->undo();
        QCOMPARE(pixels(selected, rect), before);
        command->redo();
        QCOMPARE(pixels(selected, rect), after);
        QBitArray flags(cs->channelCount(), false);
        config->setChannelFlags(flags);
        filter.process(source, rect, config);
        QCOMPARE(pixels(source, rect), before);
        flags.setBit(0, true);
        config->setChannelFlags(flags);
        filter.process(source, rect, config);
        const auto masked = pixels(source, rect), all = pixels(full, rect);
        for (int offset = 0; offset < masked.size(); offset += cs->pixelSize()) {
            QCOMPARE(masked[offset], all[offset]);
            QCOMPARE(masked.mid(offset + 1, 3), before.mid(offset + 1, 3));
        }
        source = pattern(cs, rect);
        config = settings();
        source->fill(part, KoColor(Qt::red, cs));
        KisPaintDeviceSP updated = new KisPaintDevice(*source);
        filter.process(updated, rect, config);
        filter.process(source, full, nullptr, filter.changedRect(part, config, 0), config);
        QCOMPARE(pixels(full, rect), pixels(updated, rect));
    }
    void testConfigurationAndCancellation()
    {
        KisPencilFilter filter;
        auto config = settings();
        KisPencilConfigWidget widget(nullptr, nullptr);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        const auto saved = widget.configuration();
        QCOMPARE(saved->getDouble("hatchSize"), 4.75);
        QCOMPARE(saved->getInt("seed"), 1234567);
        auto restored = filter.factoryConfiguration(KisGlobalResourcesInterface::instance());
        restored->fromXML(saved->toXML());
        const QRect rect(-20, -20, 64, 64);
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        auto first = pattern(cs, rect), second = pattern(cs, rect);
        filter.process(first, rect, config);
        filter.process(second, rect, restored->cloneWithResourcesSnapshot());
        QCOMPARE(pixels(first, rect), pixels(second, rect));
        auto *hatching = widget.findChild<QCheckBox *>("pencilHatching");
        QVERIFY(hatching);
        hatching->setChecked(false);
        QCOMPARE(spy.count(), 1);
        QVERIFY(!widget.findChild<KisDoubleSliderSpinBox *>("pencilHatchSize")->isEnabled());
        QVERIFY(!widget.findChild<QPushButton *>("pencilNewSeed")->isEnabled());
        QCOMPARE(widget.configuration()->getInt("seed"), 1234567);
        for (int threshold : {10, 35, 70}) {
            auto device = pattern(cs, rect);
            const auto before = pixels(device, rect);
            KoDummyUpdaterHolder progress;
            connect(progress.updater(), &KoUpdater::sigProgress, this, [&](int percent) {
                if (percent >= threshold)
                    QMetaObject::invokeMethod(progress.updater(),
                                              "setInterrupted",
                                              Qt::DirectConnection,
                                              Q_ARG(bool, true));
            });
            filter.process(device, rect, config, progress.updater());
            QVERIFY(progress.updater()->interrupted());
            QCOMPARE(pixels(device, rect), before);
        }
    }
    void benchmarkCpu()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_PENCIL"))
            QSKIP("Opt-in CPU timings.");
        KisPencilFilter filter;
        for (const QString &depth : {QString("U8"), QString("F32")}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
            for (const QSize size : {QSize(2480, 3508), QSize(4096, 4096)}) {
                QList<qint64> times;
                for (int run = 0; run < 3; ++run) {
                    const QRect rect(QPoint(), size);
                    KisPaintDeviceSP device = new KisPaintDevice(cs);
                    device->fill(rect, KoColor(Qt::gray, cs));
                    QElapsedTimer timer;
                    timer.start();
                    filter.process(device, rect, settings());
                    times.append(timer.elapsed());
                }
                std::sort(times.begin(), times.end());
                qInfo() << "Pencil CPU single call" << depth << size << "median ms" << times[1];
            }
        }
    }
    void renderReviewArtifacts()
    {
        const QString directory = qEnvironmentVariable("SOLSTICE_PENCIL_ARTIFACT_DIR");
        if (directory.isEmpty())
            QSKIP("Opt-in review images.");
        QImage source(384, 384, QImage::Format_ARGB32);
        {
            QPainter painter(&source);
            painter.setRenderHint(QPainter::Antialiasing);
            QLinearGradient bg(0, 0, 384, 384);
            bg.setColorAt(0, QColor(40, 75, 105));
            bg.setColorAt(1, QColor(205, 215, 180));
            painter.fillRect(source.rect(), bg);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(30, 40, 55));
            painter.drawEllipse(QRectF(90, 35, 210, 285));
            painter.setBrush(QColor(65, 105, 135));
            painter.drawEllipse(QRectF(50, 265, 310, 190));
            painter.setBrush(QColor(193, 133, 105));
            painter.drawRect(QRectF(165, 235, 60, 65));
            QRadialGradient face(167, 150, 155);
            face.setColorAt(0, QColor(255, 228, 192));
            face.setColorAt(.6, QColor(225, 175, 141));
            face.setColorAt(1, QColor(150, 97, 85));
            painter.setBrush(face);
            painter.drawEllipse(QRectF(114, 69, 162, 204));
            QPainterPath hair;
            hair.moveTo(100, 190);
            hair.cubicTo(65, 55, 140, 12, 228, 48);
            hair.cubicTo(300, 60, 303, 136, 270, 185);
            hair.lineTo(258, 104);
            hair.cubicTo(217, 151, 183, 136, 165, 85);
            hair.cubicTo(148, 148, 126, 145, 100, 190);
            painter.setBrush(QColor(35, 45, 60));
            painter.drawPath(hair);
            painter.setPen(QPen(QColor(70, 48, 47), 3));
            painter.setBrush(QColor(250, 242, 226));
            painter.drawEllipse(QRectF(137, 151, 33, 14));
            painter.drawEllipse(QRectF(212, 151, 33, 14));
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(58, 96, 91));
            painter.drawEllipse(QRectF(150, 151, 11, 14));
            painter.drawEllipse(QRectF(222, 151, 11, 14));
            painter.setPen(QPen(QColor(161, 101, 87), 2));
            painter.drawLine(QPointF(191, 166), QPointF(183, 197));
            painter.drawLine(QPointF(183, 197), QPointF(198, 200));
            painter.setPen(QPen(QColor(145, 60, 66), 3));
            painter.drawArc(QRectF(170, 208, 41, 20), 190 * 16, 160 * 16);
            painter.setPen(QPen(QColor(220, 230, 240), 1));
            for (int i = 0; i < 5; ++i)
                painter.drawLine(80 + i * 52, 308, 98 + i * 52, 350);
            for (int i = 0; i < 12; ++i)
                painter.fillRect(QRect(i * 32, 360, 32, 24), QColor(i * 255 / 11, i * 255 / 11, i * 255 / 11));
        }
        QImage sheet(1152, 832, QImage::Format_ARGB32);
        sheet.fill(Qt::white);
        QPainter painter(&sheet);
        const QStringList labels =
            {"Original", "Outlines only", "Hatching only", "Pencil (default)", "Grayscale", "Roughness 100 / size 8"};
        KisPencilFilter filter;
        for (int i = 0; i < 6; ++i) {
            KisPaintDeviceSP device = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
            device->convertFromQImage(source, nullptr);
            auto config = settings();
            config->setProperty("hatchSize", 4.0);
            config->setProperty("angle", 45.0);
            if (i == 1)
                config->setProperty("hatching", false);
            if (i == 2)
                config->setProperty("outlines", false);
            if (i == 4)
                config->setProperty("grayscale", true);
            if (i == 5) {
                config->setProperty("roughness", 100.0);
                config->setProperty("hatchSize", 8.0);
            }
            if (i)
                filter.process(device, source.rect(), config);
            const QPoint origin((i % 3) * 384, (i / 3) * 416);
            painter.setPen(Qt::black);
            painter.drawText(QRect(origin, QSize(384, 32)), Qt::AlignCenter, labels[i]);
            painter.drawImage(origin + QPoint(0, 32), device->convertToQImage(nullptr, source.rect()));
            if (i == 3) {
                KisPencilConfigWidget widget(nullptr, device);
                widget.setConfiguration(config);
                widget.resize(widget.sizeHint());
                widget.show();
                QApplication::processEvents();
                QVERIFY(widget.grab().save(QDir(directory).filePath("pencil-widget.png")));
            }
        }
        painter.end();
        QVERIFY(sheet.save(QDir(directory).filePath("pencil-comparison.png")));
    }
};
SIMPLE_TEST_MAIN(KisPencilFilterTest)
#include "KisPencilFilterTest.moc"
