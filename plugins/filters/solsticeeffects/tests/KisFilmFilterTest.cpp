/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisFilmFilter.h"
#include "KisChromaticAberrationFilter.h"
#include "KisFilmConfigWidget.h"
#include "KisFilmGrainFilter.h"

#include <KisGlobalResourcesInterface.h>
#include <KoChannelInfo.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QSignalSpy>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_default_bounds.h>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_slider_spin_box.h>
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

KisFilterConfigurationSP settings(KisFilmFilter &filter)
{
    auto config = filter.defaultConfiguration(KisGlobalResourcesInterface::instance());
    config->setProperty("seed", 1234567);
    config->setProperty("aberrationAngle", 37.0);
    config->setProperty("lightLeakX", 40.25);
    config->setProperty("lightLeakY", -10.5);
    config->setProperty("lightLeakRadius", 60.0);
    return config->cloneWithResourcesSnapshot();
}

void neutral(KisFilterConfigurationSP config)
{
    config->setProperty("toneStrength", 0.0);
    config->setProperty("aberrationStrength", 0.0);
    config->setProperty("grainStrength", 0.0);
    config->setProperty("lightLeakStrength", 0.0);
}

class CanvasBounds : public KisDefaultBounds
{
public:
    explicit CanvasBounds(QRect rect)
        : m_rect(rect)
    {
    }
    QRect bounds() const override
    {
        return m_rect;
    }

private:
    QRect m_rect;
};
} // namespace

class KisFilmFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testInstalledPlugin()
    {
        auto filter = KisFilterRegistry::instance()->value("solstice_film");
        QVERIFY2(filter, "Install kritasolsticeeffectsfilter before running this test.");
        QVERIFY(filter->supportsThreading());
        QVERIFY(filter->supportsAdjustmentLayers());
        auto config = filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
        QVERIFY(!filter->supportsLevelOfDetail(config, 1));
        const QRect rect(0, 0, 32, 32);
        auto device = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
        auto before = pixels(device, rect);
        KoDummyUpdaterHolder progress;
        filter->process(device, rect, config->cloneWithResourcesSnapshot(), progress.updater());
        QVERIFY(pixels(device, rect) != before);
        QCOMPARE(progress.updater()->progress(), 100);
    }

    void testFormats_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<QString>("depth");
        for (const QString &model : {QString("RGBA"), QString("GRAYA")}) {
            for (const QString &depth : {QString("U8"), QString("U16"), QString("F16"), QString("F32")}) {
                QTest::newRow(qPrintable(model + depth)) << model << depth;
            }
        }
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
        KisFilmFilter filter;
        auto config = settings(filter);
        auto whole = pattern(cs, rect);
        auto tiled = pattern(cs, rect);
        const auto before = pixels(whole, rect);
        filter.process(whole, rect, config);
        {
            KisTransaction transaction(tiled);
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
        QVERIFY(before != after);
        QCOMPARE(pixels(tiled, rect), after);
        for (int offset = 0; offset < before.size(); offset += cs->pixelSize()) {
            if (cs->opacityF(reinterpret_cast<const quint8 *>(before.constData() + offset)) == 0.0) {
                QCOMPARE(after.mid(offset, cs->pixelSize()), before.mid(offset, cs->pixelSize()));
            }
            for (const auto *channel : cs->channels()) {
                if (channel->channelType() == KoChannelInfo::ALPHA) {
                    QCOMPARE(after.mid(offset + channel->pos(), channel->size()),
                             before.mid(offset + channel->pos(), channel->size()));
                }
            }
        }
        auto zero = pattern(cs, rect);
        neutral(config);
        filter.process(zero, rect, config);
        QCOMPARE(pixels(zero, rect), before);
        QCOMPARE(filter.neededRect(rect, config, 0), rect);
    }

    void testKernelComposition()
    {
        const QRect rect(-20, -20, 60, 60);
        KisFilmFilter film;
        KisChromaticAberrationFilter aberration;
        KisFilmGrainFilter grain;
        for (const QString &depth : {QString("U8"), QString("U16"), QString("F16"), QString("F32")}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
            for (int variant : {0, 1, 2}) {
                auto config = settings(film);
                neutral(config);
                if (variant != 0)
                    config->setProperty("aberrationStrength", 2.75);
                if (variant != 1)
                    config->setProperty("grainStrength", 25.0);
                config->setProperty("monochrome", false);
                auto combined = pattern(cs, rect);
                auto separate = pattern(cs, rect);
                film.process(combined, rect, config);
                auto sub = config->clone();
                sub->setProperty("mode", 0);
                sub->setProperty("strength", config->getDouble("aberrationStrength"));
                sub->setProperty("angle", config->getDouble("aberrationAngle"));
                aberration.process(separate, rect, sub);
                sub->setProperty("strength", config->getDouble("grainStrength"));
                grain.process(separate, rect, sub);
                QCOMPARE(pixels(combined, rect), pixels(separate, rect));
            }
        }
    }

    void testToneLeakAndHdr()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        QVERIFY(cs);
        const QRect rect(-8, -8, 17, 17);
        KisFilmFilter filter;
        auto config = settings(filter);
        neutral(config);
        config->setProperty("toneType", KisFilmFilter::Fade);
        config->setProperty("toneStrength", 100.0);
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{-2.0f, 0.5f, 3.0f, 0.375f});
        KisPaintDeviceSP device = new KisPaintDevice(cs);
        device->fill(rect, color);
        filter.process(device, rect, config);
        auto data = pixels(device, QRect(0, 0, 1, 1));
        QVector<float> values(4);
        cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(data.constData()), values);
        QVERIFY(std::abs(values[0] + 1.64f) < 1e-5f);
        QVERIFY(std::abs(values[1] - 0.51f) < 1e-5f);
        QVERIFY(std::abs(values[2] - 2.66f) < 1e-5f);
        QCOMPARE(values[3], 0.375f);

        neutral(config);
        config->setProperty("lightLeakStrength", 50.0);
        config->setProperty("lightLeakX", 0.0);
        config->setProperty("lightLeakY", 0.0);
        config->setProperty("lightLeakRadius", 2.0);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{0.2f, 0.2f, 3.0f, 0.375f});
        device->fill(rect, color);
        filter.process(device, rect, config);
        data = pixels(device, QRect(0, 0, 1, 1));
        cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(data.constData()), values);
        QVERIFY(std::abs(values[0] - 0.6f) < 1e-5f);
        QVERIFY(std::abs(values[1] - 0.352f) < 1e-5f);
        QCOMPARE(values[2], 3.0f);
        QCOMPARE(values[3], 0.375f);
        data = pixels(device, QRect(8, 8, 1, 1));
        cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(data.constData()), values);
        QVERIFY(std::abs(values[0] - 0.2f) < 1e-5f);

        // None disables tone even with its strength nonzero.
        config->setProperty("toneType", KisFilmFilter::None);
        config->setProperty("toneStrength", 100.0);
        config->setProperty("lightLeakStrength", 0.0);
        const auto before = pixels(device, rect);
        filter.process(device, rect, config);
        QCOMPARE(pixels(device, rect), before);
    }

    void testPartialSelectionChannelsAndUndo()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(-32, -32, 64, 64);
        const QRect part(-8, -8, 13, 15);
        KisFilmFilter filter;
        auto config = settings(filter);
        auto source = pattern(cs, rect);
        KisPaintDeviceSP complete = new KisPaintDevice(*source);
        filter.process(complete, rect, config);
        KisPaintDeviceSP partial = new KisPaintDevice(*source);
        filter.process(source, partial, nullptr, part, config);
        QCOMPARE(pixels(partial, part), pixels(complete, part));
        const auto before = pixels(source, rect);
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
        QCOMPARE(pixels(source, rect), before);

        QBitArray flags(cs->channelCount(), false);
        config->setChannelFlags(flags);
        filter.process(source, rect, config);
        QCOMPARE(pixels(source, rect), before);
        flags.setBit(0, true);
        config->setChannelFlags(flags);
        filter.process(source, rect, config);
        const auto masked = pixels(source, rect);
        const auto full = pixels(complete, rect);
        for (int offset = 0; offset < masked.size(); offset += cs->pixelSize()) {
            QCOMPARE(masked[offset], full[offset]);
            QCOMPARE(masked.mid(offset + 1, 3), before.mid(offset + 1, 3));
        }
    }

    void testConfigurationAndPresets()
    {
        KisFilmFilter filter;
        auto config = settings(filter);
        // A saved preset label must not reapply the preset during load/render.
        config->setProperty("toneStrength", 17.25);
        KisFilmConfigWidget widget(nullptr, nullptr);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        auto saved = widget.configuration();
        QCOMPARE(saved->getDouble("toneStrength"), 17.25);
        QCOMPARE(saved->getInt("seed"), 1234567);
        QCOMPARE(saved->getDouble("lightLeakX"), 40.25);
        auto restored = filter.factoryConfiguration(KisGlobalResourcesInterface::instance());
        restored->fromXML(saved->toXML());
        const QRect rect(-16, -16, 32, 32);
        auto first = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
        auto second = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
        filter.process(first, rect, config);
        filter.process(second, rect, restored->cloneWithResourcesSnapshot());
        QCOMPARE(pixels(first, rect), pixels(second, rect));
        auto *preset = widget.findChild<QComboBox *>("filmPreset");
        QVERIFY(preset);
        for (int index : {KisFilmFilter::Vintage, KisFilmFilter::Modern, KisFilmFilter::Warm}) {
            spy.clear();
            preset->setCurrentIndex(index);
            QVERIFY(QMetaObject::invokeMethod(preset, "activated", Q_ARG(int, index)));
            QCOMPARE(spy.count(), 1);
            saved = widget.configuration();
            QCOMPARE(saved->getInt("preset"), index);
            QCOMPARE(saved->getInt("seed"), 1234567);
            QCOMPARE(saved->getDouble("lightLeakX"), 40.25);
        }
        QCOMPARE(saved->getDouble("grainStrength"), 0.0);
        QCOMPARE(saved->getDouble("aberrationStrength"), 0.0);
        auto *tone = widget.findChild<KisDoubleSliderSpinBox *>("filmToneStrength");
        QVERIFY(tone);
        spy.clear();
        tone->setValue(42.0);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(widget.configuration()->getInt("preset"), int(KisFilmFilter::Custom));

        // First use anchors the leak to the canvas, then saved coordinates
        // survive opening the configuration on another canvas/partial device.
        KisPaintDeviceSP device = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
        device->setDefaultBounds(new CanvasBounds(QRect(0, 0, 100, 80)));
        KisFilmConfigWidget firstWidget(nullptr, device);
        firstWidget.setConfiguration(filter.defaultConfiguration(KisGlobalResourcesInterface::instance()));
        saved = firstWidget.configuration();
        QCOMPARE(saved->getDouble("lightLeakX"), 99.0);
        QCOMPARE(saved->getDouble("lightLeakY"), 39.5);
        device->setDefaultBounds(new CanvasBounds(QRect(-200, -100, 400, 200)));
        KisFilmConfigWidget nextWidget(nullptr, device);
        nextWidget.setConfiguration(saved);
        QCOMPARE(nextWidget.configuration()->getDouble("lightLeakX"), 99.0);
        QCOMPARE(nextWidget.configuration()->getDouble("lightLeakRadius"), saved->getDouble("lightLeakRadius"));
    }

    void testCancellation()
    {
        KisFilmFilter filter;
        auto config = settings(filter);
        const QRect rect(0, 0, 128, 128);
        auto device = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
        const auto before = pixels(device, rect);
        KoDummyUpdaterHolder progress;
        // Cancel during the first stage, then ensure no scratch pixels commit.
        connect(progress.updater(), &KoUpdater::sigProgress, this, [&](int percent) {
            if (percent >= 10) {
                QMetaObject::invokeMethod(progress.updater(),
                                          "setInterrupted",
                                          Qt::DirectConnection,
                                          Q_ARG(bool, true));
            }
        });
        filter.process(device, rect, config, progress.updater());
        QVERIFY(progress.updater()->interrupted());
        QCOMPARE(pixels(device, rect), before);
    }

    void benchmarkCpu()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_FILM"))
            QSKIP("Set SOLSTICE_BENCHMARK_FILM=1 for CPU timings.");
        KisFilmFilter filter;
        for (const QString &depth : {QString("U8"), QString("F32")}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
            for (const QSize size : {QSize(2480, 3508), QSize(4096, 4096)}) {
                QList<qint64> times;
                for (int run = 0; run < 3; ++run) {
                    KisPaintDeviceSP device = new KisPaintDevice(cs);
                    device->fill(QRect(QPoint(), size), KoColor(Qt::gray, cs));
                    auto config = settings(filter);
                    config->setProperty("lightLeakX", size.width() - 1.0);
                    config->setProperty("lightLeakY", (size.height() - 1.0) / 2.0);
                    config->setProperty("lightLeakRadius", std::hypot(size.width(), size.height()) / 2.0);
                    QElapsedTimer timer;
                    timer.start();
                    filter.process(device, QRect(QPoint(), size), config);
                    times.append(timer.elapsed());
                }
                std::sort(times.begin(), times.end());
                qInfo() << "Film CPU single call, Vintage" << depth << size << "median ms" << times[1];
            }
        }
    }

    void renderReviewArtifacts()
    {
        const QString directory = qEnvironmentVariable("SOLSTICE_FILM_ARTIFACT_DIR");
        if (directory.isEmpty())
            QSKIP("Set SOLSTICE_FILM_ARTIFACT_DIR to render review images.");
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QSize size(384, 256);
        QImage source(size, QImage::Format_ARGB32);
        {
            QPainter painter(&source);
            QLinearGradient background(0, 0, 384, 256);
            background.setColorAt(0.0, QColor(30, 100, 150));
            background.setColorAt(1.0, QColor(230, 185, 130));
            painter.fillRect(source.rect(), background);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor(35, 25, 50), 3));
            painter.setBrush(QColor(215, 70, 90));
            painter.drawEllipse(QRectF(30, 45, 155, 155));
            painter.setBrush(QColor(40, 180, 135));
            painter.drawRect(QRectF(220, 120, 130, 95));
            painter.setBrush(Qt::white);
            painter.drawEllipse(QRectF(280, 25, 42, 42));
            for (int i = 0; i < 8; ++i)
                painter.fillRect(QRect(i * 48, 230, 48, 26), QColor(i * 255 / 7, i * 255 / 7, i * 255 / 7));
        }
        QImage sheet(768, 576, QImage::Format_ARGB32);
        sheet.fill(Qt::white);
        QPainter painter(&sheet);
        KisFilmFilter filter;
        const QStringList labels = {"Original", "Vintage", "Modern", "Warm"};
        for (int index = 0; index < 4; ++index) {
            KisPaintDeviceSP device = new KisPaintDevice(cs);
            device->convertFromQImage(source, nullptr);
            device->setDefaultBounds(new CanvasBounds(source.rect()));
            KisFilmConfigWidget widget(nullptr, device);
            auto config = settings(filter);
            if (index > 0)
                KisFilmFilter::applyPreset(config, index);
            config->setProperty("lightLeakX", 383.0);
            config->setProperty("lightLeakY", 127.5);
            config->setProperty("lightLeakRadius", std::hypot(384.0, 256.0) / 2.0);
            widget.setConfiguration(config);
            if (index == 1) {
                widget.resize(widget.sizeHint());
                widget.show();
                QApplication::processEvents();
                QVERIFY(widget.grab().save(QDir(directory).filePath("film-widget.png")));
            }
            if (index > 0)
                filter.process(device, source.rect(), config);
            const QPoint origin((index % 2) * 384, (index / 2) * 288);
            painter.setPen(Qt::black);
            painter.drawText(QRect(origin, QSize(384, 32)), Qt::AlignCenter, labels[index]);
            painter.drawImage(origin + QPoint(0, 32), device->convertToQImage(nullptr, source.rect()));
        }
        painter.end();
        QVERIFY(sheet.save(QDir(directory).filePath("film-presets.png")));
    }
};

SIMPLE_TEST_MAIN(KisFilmFilterTest)
#include "KisFilmFilterTest.moc"
