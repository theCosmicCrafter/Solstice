/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisDropShadowFilter.h"
#include "KisCpuPlaneBlur.h"
#include "KisDropShadowConfigWidget.h"
#include <KisGlobalResourcesInterface.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
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
    return KisDropShadowFilter()
        .defaultConfiguration(KisGlobalResourcesInterface::instance())
        ->cloneWithResourcesSnapshot();
}
} // namespace

class KisDropShadowFilterTest : public QObject
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
        const QRect core(-23, -19, 48, 41), rect(-83, -79, 166, 158);
        KisDropShadowFilter filter;
        auto config = settings();
        config->setProperty("distance", 17.5);
        config->setProperty("angle", 127.3);
        auto whole = pattern(cs, core), tiled = pattern(cs, core);
        const auto before = pixels(whole, rect);
        filter.process(whole, rect, config);
        {
            KisTransaction transaction(tiled);
            std::vector<std::future<void>> jobs;
            for (int stripe = 0; stripe < 4; ++stripe)
                jobs.push_back(std::async(std::launch::async, [&, stripe] {
                    for (int y = rect.top() + stripe * 23; y <= rect.bottom(); y += 92)
                        for (int x = rect.left(); x <= rect.right(); x += 29)
                            filter.process(tiled, rect.intersected(QRect(x, y, 29, 23)), config);
                }));
            for (auto &job : jobs)
                job.get();
        }
        const auto after = pixels(whole, rect);
        QVERIFY(after != before);
        QCOMPARE(pixels(tiled, rect), after);
        bool grew = false;
        for (int p = 0; p < before.size(); p += cs->pixelSize()) {
            const double oldAlpha = cs->opacityF(reinterpret_cast<const quint8 *>(before.constData() + p));
            const double newAlpha = cs->opacityF(reinterpret_cast<const quint8 *>(after.constData() + p));
            QVERIFY(newAlpha >= oldAlpha);
            if (oldAlpha == 1)
                QCOMPARE(after.mid(p, cs->pixelSize()), before.mid(p, cs->pixelSize()));
            if (oldAlpha == 0 && newAlpha > 0)
                grew = true;
        }
        QVERIFY(grew);
        auto zero = pattern(cs, core);
        config->setProperty("opacity", 0.0);
        filter.process(zero, rect, config);
        QCOMPARE(pixels(zero, rect), before);
        QCOMPARE(filter.neededRect(rect, config, 0), rect);
        QCOMPARE(filter.changedRect(rect, config, 0), rect);
    }
    void testCompositeAndBilinearReference()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        KisDropShadowFilter filter;
        auto config = settings();
        config->setProperty("blur", 0.0);
        config->setProperty("angle", 0.0);
        config->setProperty("distance", 0.0);
        KoColor blue(cs);
        cs->fromNormalisedChannelsValue(blue.data(), QVector<float>{0, 0, 1, 1});
        config->setProperty("shadowColor", QVariant::fromValue(blue));
        KisPaintDeviceSP dev = new KisPaintDevice(cs);
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{.8f, .2f, .1f, .5f});
        dev->fill(QRect(0, 0, 1, 1), color);
        filter.process(dev, QRect(0, 0, 1, 1), config);
        KoColor out(cs);
        dev->pixel(0, 0, &out);
        QVector<float> v(4);
        cs->normalisedChannelsValue(out.data(), v);
        // Original over a half-opacity shadow of its own 0.5 coverage.
        const double a = .5 + .5 * .5 * .5;
        QVERIFY(std::abs(v[0] - .4 / a) < 1e-6);
        QVERIFY(std::abs(v[1] - .1 / a) < 1e-6);
        QVERIFY(std::abs(v[2] - (.05 + .125) / a) < 1e-6);
        QVERIFY(std::abs(v[3] - a) < 1e-6);
        // A 2.5-pixel translation splits coverage equally between two pixels.
        config->setProperty("distance", 2.5);
        dev = new KisPaintDevice(cs);
        dev->fill(QRect(0, 0, 1, 1), KoColor(Qt::white, cs));
        const QRect rect(-2, -2, 9, 5);
        filter.process(dev, rect, config);
        dev->pixel(2, 0, &out);
        cs->normalisedChannelsValue(out.data(), v);
        QCOMPARE(v[0], 0.f);
        QCOMPARE(v[2], 1.f);
        QCOMPARE(v[3], .25f);
        dev->pixel(3, 0, &out);
        cs->normalisedChannelsValue(out.data(), v);
        QCOMPARE(v[3], .25f);
        QCOMPARE(filter.changedRect(QRect(0, 0, 1, 1), config, 0), QRect(0, 0, 4, 1));
        QCOMPARE(filter.neededRect(QRect(3, 0, 1, 1), config, 0), QRect(0, 0, 4, 1));
        // Down is 90 degrees, with no spurious horizontal fractional footprint.
        config->setProperty("angle", 90.0);
        config->setProperty("distance", 3.0);
        QCOMPARE(filter.changedRect(QRect(0, 0, 1, 1), config, 0), QRect(0, 0, 1, 4));
        dev = new KisPaintDevice(cs);
        dev->fill(QRect(0, 0, 1, 1), KoColor(Qt::white, cs));
        filter.process(dev, QRect(-1, -1, 3, 6), config);
        dev->pixel(0, 3, &out);
        cs->normalisedChannelsValue(out.data(), v);
        QCOMPARE(v[3], .5f);
        // Hidden RGB, including HDR, is not mixed into a newly visible shadow.
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{100, -50, 30, 0});
        dev = new KisPaintDevice(cs);
        dev->fill(QRect(-1, -1, 3, 6), color);
        dev->fill(QRect(0, 0, 1, 1), KoColor(Qt::white, cs));
        filter.process(dev, QRect(-1, -1, 3, 6), config);
        dev->pixel(0, 3, &out);
        cs->normalisedChannelsValue(out.data(), v);
        QCOMPARE(v, QVector<float>({0, 0, 1, .5f}));
    }
    void testLargeBlurBoundsAndUpdates()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        KisDropShadowFilter filter;
        const QRect core(-8, -7, 16, 14);
        for (double blur : {0., 10., 100.}) {
            auto config = settings();
            config->setProperty("blur", blur);
            config->setProperty("distance", 23.5);
            config->setProperty("angle", 217.0);
            const QRect affected = filter.changedRect(core, config, 0);
            const QRect output = affected.adjusted(-2, -2, 2, 2);
            auto original = pattern(cs, core);
            KisPaintDeviceSP whole = new KisPaintDevice(*original), tiled = new KisPaintDevice(*original);
            filter.process(whole, output, config);
            {
                KisTransaction transaction(tiled);
                for (int y = output.top(); y <= output.bottom(); y += 113)
                    for (int x = output.left(); x <= output.right(); x += 127)
                        filter.process(tiled, output.intersected(QRect(x, y, 127, 113)), config);
            }
            QCOMPARE(pixels(tiled, output), pixels(whole, output));
            // No writes outside the declared changed rect (including numerical halo).
            QCOMPARE(pixels(whole, QRect(output.left(), output.top(), output.width(), 2)),
                     pixels(original, QRect(output.left(), output.top(), output.width(), 2)));
            original->clear(core);
            filter.process(original, whole, nullptr, affected, config);
            QCOMPARE(pixels(whole, output), pixels(original, output));
        }
    }
    void testSelectionUndoAndChannels()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect core(-10, -10, 20, 20), rect(-40, -40, 100, 100), selectedRect(0, 0, 35, 35);
        KisDropShadowFilter filter;
        auto config = settings();
        auto original = pattern(cs, core);
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
                    const auto *channel = cs->channels()[i];
                    QCOMPARE(result.mid(p + channel->pos(), channel->size()),
                             (i == enabled ? complete : before).mid(p + channel->pos(), channel->size()));
                }
        }
        config = settings();
        KisPaintDeviceSP selected = new KisPaintDevice(*original);
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(selectedRect, 128);
        KisTransaction transaction(selected);
        filter.process(original, selected, selection, rect, config);
        const auto after = pixels(selected, rect);
        QVERIFY(after != before);
        QCOMPARE(pixels(selected, QRect(-40, -40, 100, 40)), pixels(original, QRect(-40, -40, 100, 40)));
        std::unique_ptr<KUndo2Command> command(transaction.endAndTake());
        command->redo();
        command->undo();
        QCOMPARE(pixels(selected, rect), before);
        command->redo();
        QCOMPARE(pixels(selected, rect), after);
    }
    void testConfigurationPadAndCancellation()
    {
        KisDropShadowFilter filter;
        auto config = settings();
        config->setProperty("blur", 41.5);
        config->setProperty("distance", 87.5);
        config->setProperty("angle", 123.4);
        config->setProperty("opacity", 71.5);
        config->setProperty("shadowColor",
                            QVariant::fromValue(KoColor(Qt::magenta, KoColorSpaceRegistry::instance()->rgb8())));
        KisDropShadowConfigWidget widget(nullptr, nullptr);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        auto saved = widget.configuration();
        for (const QString &key : {"blur", "distance", "angle", "opacity"})
            QCOMPARE(saved->getDouble(key), config->getDouble(key));
        QCOMPARE(saved->getColor("shadowColor"), config->getColor("shadowColor"));
        auto restored = settings();
        restored->fromXML(saved->toXML());
        const QRect rect(-40, -40, 120, 120);
        auto a = pattern(KoColorSpaceRegistry::instance()->rgb8(), QRect(0, 0, 20, 20));
        KisPaintDeviceSP b = new KisPaintDevice(*a);
        filter.process(a, rect, config);
        filter.process(b, rect, restored->cloneWithResourcesSnapshot());
        QCOMPARE(pixels(a, rect), pixels(b, rect));
        widget.setConfiguration(settings());
        widget.show();
        QApplication::processEvents();
        auto *pad = widget.findChild<QWidget *>("dropShadowOffsetPad");
        QVERIFY(pad);
        QTest::mouseClick(pad, Qt::LeftButton, Qt::NoModifier, QPoint(158, 80));
        QCOMPARE(widget.configuration()->getDouble("angle"), 0.0);
        QCOMPARE(widget.configuration()->getDouble("distance"), 32.0);
        QTest::keyClick(pad, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(widget.configuration()->getDouble("distance"), 42.0);
        QTest::mouseClick(pad, Qt::RightButton, Qt::NoModifier, QPoint(88, 150));
        QCOMPARE(widget.configuration()->getDouble("angle"), 0.0); // other buttons do not change the offset
        QTest::mousePress(pad, Qt::LeftButton, Qt::NoModifier, QPoint(88, 150));
        QEvent lost(QEvent::UngrabMouse);
        QApplication::sendEvent(pad, &lost);
        const auto stopped = widget.configuration()->getDouble("distance");
        QMouseEvent move(QEvent::MouseMove,
                         QPointF(120, 110),
                         QPointF(120, 110),
                         Qt::NoButton,
                         Qt::LeftButton,
                         Qt::NoModifier);
        QApplication::sendEvent(pad, &move);
        QCOMPARE(widget.configuration()->getDouble("distance"), stopped);
        QTest::mouseRelease(pad, Qt::LeftButton);
        QVERIFY(spy.count() > 0);
        for (int threshold : {10, 40, 70}) {
            auto dev = pattern(KoColorSpaceRegistry::instance()->rgb8(), QRect(0, 0, 20, 20));
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
    void testFilterMaskXmlRoundTrip()
    {
        auto filter = KisFilterRegistry::instance()->value("solstice_drop_shadow");
        QVERIFY(filter);
        QVERIFY(filter->supportsAdjustmentLayers());
        QVERIFY(filter->supportsThreading());
        QVERIFY(filter->needsTransparentPixels(settings(), KoColorSpaceRegistry::instance()->rgb8()));
        QVERIFY(!filter->supportsLevelOfDetail(settings(), 1));
        const QRect rect(0, 0, 128, 128);
        TestUtil::MaskParent parent(rect);
        auto config = settings();
        config->setProperty("distance", 31.5);
        config->setProperty("angle", 78.0);
        KisFilterMaskSP mask = new KisFilterMask(parent.image, "Drop Shadow");
        parent.image->addNode(mask, parent.layer);
        auto restored = settings();
        restored->fromXML(config->toXML());
        mask->setFilter(restored->cloneWithResourcesSnapshot());
        mask->initSelection(parent.layer);
        mask->select(rect, MAX_SELECTED);
        auto original = pattern(parent.layer->colorSpace(), QRect(24, 24, 32, 32));
        KisPaintDeviceSP expected = new KisPaintDevice(*original), actual = new KisPaintDevice(*original);
        filter->process(expected, rect, config);
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
    }
    void benchmarkCpu()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_DROP_SHADOW"))
            QSKIP("Opt-in timings");
        for (const QString &depth : {QString("U8"), QString("F32")})
            for (const QSize size : {QSize(2480, 3508), QSize(4096, 4096)})
                for (double blur : {10.0, 100.0}) {
                    auto config = settings();
                    config->setProperty("blur", blur);
                    QList<qint64> times;
                    for (int run = 0; run < 3; ++run) {
                        const QRect rect(QPoint(), size);
                        auto dev =
                            pattern(KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString()), rect);
                        QElapsedTimer timer;
                        timer.start();
                        KisDropShadowFilter().process(dev, rect, config);
                        times << timer.elapsed();
                    }
                    std::sort(times.begin(), times.end());
                    qInfo() << "Drop Shadow CPU single call" << depth << size << "blur" << blur << "median ms"
                            << times[1];
                }
    }
    void renderReviewArtifacts()
    {
        const QString directory = qEnvironmentVariable("SOLSTICE_DROP_SHADOW_ARTIFACT_DIR");
        if (directory.isEmpty())
            QSKIP("Opt-in review images");
        QImage source(256, 256, QImage::Format_ARGB32);
        source.fill(Qt::transparent);
        QPainter sourcePainter(&source);
        sourcePainter.setRenderHint(QPainter::Antialiasing);
        QPolygonF star;
        for (int i = 0; i < 10; ++i) {
            const double a = (i * 36 - 90) * M_PI / 180, r = i % 2 ? 35 : 78;
            star << QPointF(110 + r * std::cos(a), 115 + r * std::sin(a));
        }
        sourcePainter.setPen(Qt::NoPen);
        sourcePainter.setBrush(QColor(80, 190, 200));
        sourcePainter.drawPolygon(star);
        sourcePainter.end();
        QImage sheet(768, 288, QImage::Format_RGB32);
        sheet.fill(Qt::white);
        QPainter painter(&sheet);
        for (int i = 0; i < 3; ++i) {
            KisPaintDeviceSP dev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
            dev->convertFromQImage(source, nullptr);
            auto config = settings();
            config->setProperty("blur", i == 1 ? 0.0 : 35.0);
            config->setProperty("distance", 30.0);
            if (i == 0)
                config->setProperty("opacity", 0.0);
            KisDropShadowFilter().process(dev, source.rect(), config);
            painter.drawText(QRect(i * 256, 0, 256, 32),
                             Qt::AlignCenter,
                             QStringList{"Original", "Hard shadow", "Soft shadow"}[i]);
            painter.drawImage(QPoint(i * 256, 32), dev->convertToQImage(nullptr, source.rect()));
            if (i == 2) {
                KisDropShadowConfigWidget widget(nullptr, dev);
                widget.setConfiguration(config);
                widget.resize(widget.sizeHint());
                widget.show();
                QApplication::processEvents();
                QVERIFY(widget.grab().save(QDir(directory).filePath("drop-shadow-widget.png")));
            }
        }
        painter.end();
        QVERIFY(sheet.save(QDir(directory).filePath("drop-shadow-comparison.png")));
    }
};
SIMPLE_TEST_MAIN(KisDropShadowFilterTest)
#include "KisDropShadowFilterTest.moc"
