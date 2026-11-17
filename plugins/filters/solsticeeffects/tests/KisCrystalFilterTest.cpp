/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisCrystalFilter.h"
#include "KisCrystalConfigWidget.h"
#include "KisCrystalLattice.h"
#include <KisGlobalResourcesInterface.h>
#include <KoChannelInfo.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QSignalSpy>
#include <algorithm>
#include <cmath>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <future>
#include <kis_default_bounds.h>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_transaction.h>
#include <limits>
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
    KisCrystalFilter filter;
    auto config = filter.defaultConfiguration(KisGlobalResourcesInterface::instance());
    config->setProperty("seed", 1234567);
    config->setProperty("cellSize", 8.75);
    config->setProperty("tileX", -17);
    config->setProperty("tileY", -11);
    config->setProperty("tileWidth", 43);
    config->setProperty("tileHeight", 29);
    return config->cloneWithResourcesSnapshot();
}
int modulo(int x, int period)
{
    return (x % period + period) % period;
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

class KisCrystalFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testInstalledPlugin()
    {
        auto filter = KisFilterRegistry::instance()->value("solstice_crystal");
        QVERIFY2(filter, "Install kritasolsticeeffectsfilter before running this test.");
        QVERIFY(filter->supportsThreading());
        QVERIFY(filter->supportsAdjustmentLayers());
        QVERIFY(!filter->supportsLevelOfDetail(settings(), 1));
        auto device = pattern(KoColorSpaceRegistry::instance()->rgb8(), QRect(0, 0, 32, 32));
        const auto before = pixels(device, QRect(0, 0, 32, 32));
        KoDummyUpdaterHolder progress;
        filter->process(device, QRect(0, 0, 32, 32), settings(), progress.updater());
        QVERIFY(pixels(device, QRect(0, 0, 32, 32)) != before);
        QCOMPARE(progress.updater()->progress(), 100);
    }

    void testNearestAgainstExhaustiveSearch()
    {
        for (bool periodic : {false, true}) {
            for (QSize tile : {QSize(43, 29), QSize(1, 97), QSize(83, 1), QSize(2, 3)}) {
                for (double size : {1.0, 3.25, 20.0, 512.0}) {
                    for (int seed : {0, 1937, 2147483647}) {
                        auto config = settings();
                        config->setProperty("tiling", periodic);
                        config->setProperty("tileWidth", tile.width());
                        config->setProperty("tileHeight", tile.height());
                        config->setProperty("cellSize", size);
                        config->setProperty("seed", seed);
                        config->setProperty("irregularity", seed == 0 ? 0.0 : 100.0);
                        KisCrystalLattice lattice(config);
                        for (int y = -37; y < 46; y += 7) {
                            for (int x = -53; x < 59; x += 11) {
                                const double px = (periodic ? modulo(x + 17, tile.width()) : x) + 0.5;
                                const double py = (periodic ? modulo(y + 11, tile.height()) : y) + 0.5;
                                const int cx = int(std::floor(px / lattice.stepX()));
                                const int cy = int(std::floor(py / lattice.stepY()));
                                double distance = std::numeric_limits<double>::infinity();
                                KisCrystalLattice::Site expected;
                                for (int row = periodic ? 0 : cy - 5; row < (periodic ? lattice.rows() : cy + 6);
                                     ++row) {
                                    for (int col = periodic ? 0 : cx - 5; col < (periodic ? lattice.columns() : cx + 6);
                                         ++col) {
                                        const auto site = lattice.site(col, row);
                                        double dx = std::abs(px - site.position.x());
                                        double dy = std::abs(py - site.position.y());
                                        if (periodic) {
                                            dx = qMin(dx, tile.width() - dx);
                                            dy = qMin(dy, tile.height() - dy);
                                        }
                                        const double d = dx * dx + dy * dy;
                                        if (d < distance
                                            || (d == distance
                                                && (row < expected.row
                                                    || (row == expected.row && col < expected.column)))) {
                                            distance = d;
                                            expected = site;
                                        }
                                    }
                                }
                                const auto actual = lattice.nearestSite(x, y);
                                QCOMPARE(actual.column, expected.column);
                                QCOMPARE(actual.row, expected.row);
                                QVERIFY(lattice.dependencyRect(QRect(x, y, 1, 1)).contains(lattice.sampleAt(x, y)));
                            }
                        }
                    }
                }
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
        KisCrystalFilter filter;
        const QRect rect(-69, -37, 137, 83);
        for (bool periodic : {false, true}) {
            auto config = settings();
            config->setProperty("tiling", periodic);
            auto original = pattern(cs, rect);
            auto whole = pattern(cs, rect);
            auto tiled = pattern(cs, rect);
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
            const auto before = pixels(original, rect);
            const auto after = pixels(whole, rect);
            QVERIFY(after != before);
            QCOMPARE(pixels(tiled, rect), after);
            KisCrystalLattice lattice(config);
            for (int y = rect.top(); y <= rect.bottom(); ++y) {
                for (int x = rect.left(); x <= rect.right(); ++x) {
                    const int offset = ((y - rect.y()) * rect.width() + x - rect.x()) * cs->pixelSize();
                    auto expected = before.mid(offset, cs->pixelSize());
                    const auto sample = pixels(original, QRect(lattice.sampleAt(x, y), QSize(1, 1)));
                    if (cs->opacityF(reinterpret_cast<const quint8 *>(expected.constData())) > 0.0
                        && cs->opacityF(reinterpret_cast<const quint8 *>(sample.constData())) > 0.0) {
                        for (const auto *channel : cs->channels())
                            if (channel->channelType() == KoChannelInfo::COLOR)
                                expected.replace(channel->pos(),
                                                 channel->size(),
                                                 sample.mid(channel->pos(), channel->size()));
                    }
                    QCOMPARE(after.mid(offset, cs->pixelSize()), expected);
                }
            }
            config->setProperty("cellSize", 1.0);
            config->setProperty("irregularity", 0.0);
            filter.process(original, rect, config);
            QCOMPARE(pixels(original, rect), before);
            QCOMPARE(filter.neededRect(rect, config, 0), rect);
        }
    }

    void testPeriodicRepeatAndDirtyBounds()
    {
        KisCrystalFilter filter;
        auto config = settings();
        config->setProperty("tiling", true);
        const QRect tile(-17, -11, 43, 29);
        const QRect area(tile.x() - tile.width(), tile.y() - tile.height(), 3 * tile.width(), 3 * tile.height());
        auto base = pattern(KoColorSpaceRegistry::instance()->rgb8(), tile);
        const auto bytes = pixels(base, tile);
        KisPaintDeviceSP source = new KisPaintDevice(base->colorSpace());
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                source->writeBytes(reinterpret_cast<const quint8 *>(bytes.constData()),
                                   tile.translated(x * tile.width(), y * tile.height()));
        KisPaintDeviceSP result = new KisPaintDevice(*source);
        filter.process(result, area, config);
        const auto center = pixels(result, tile);
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                QCOMPARE(pixels(result, tile.translated(x * tile.width(), y * tile.height())), center);

        const QRect dirty(tile.topLeft(), QSize(3, 7));
        source->fill(dirty, KoColor(Qt::red, source->colorSpace()));
        KisPaintDeviceSP full = new KisPaintDevice(*source);
        filter.process(full, area, config);
        const QRect changed = filter.changedRect(dirty, config, 0);
        QCOMPARE(changed, tile);
        filter.process(source, result, nullptr, changed, config);
        QCOMPARE(pixels(result, area), pixels(full, area));
    }

    void testSelectionChannelsUndoAndHdr()
    {
        KisCrystalFilter filter;
        auto config = settings();
        const QRect rect(-32, -32, 64, 64);
        const QRect part(-8, -8, 13, 15);
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        auto source = pattern(cs, rect);
        KisPaintDeviceSP full = new KisPaintDevice(*source);
        filter.process(full, rect, config);
        KisPaintDeviceSP partial = new KisPaintDevice(*source);
        filter.process(source, partial, nullptr, part, config);
        QCOMPARE(pixels(partial, part), pixels(full, part));
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
        QBitArray flags(cs->channelCount(), false);
        config->setChannelFlags(flags);
        filter.process(source, rect, config);
        QCOMPARE(pixels(source, rect), before);
        flags.setBit(0, true);
        config->setChannelFlags(flags);
        filter.process(source, rect, config);
        const auto masked = pixels(source, rect);
        const auto all = pixels(full, rect);
        for (int offset = 0; offset < masked.size(); offset += cs->pixelSize()) {
            QCOMPARE(masked[offset], all[offset]);
            QCOMPARE(masked.mid(offset + 1, 3), before.mid(offset + 1, 3));
        }
        cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{-2.0f, 0.5f, 3.0f, 0.375f});
        KisPaintDeviceSP hdr = new KisPaintDevice(cs);
        hdr->fill(rect.adjusted(-40, -40, 40, 40), color);
        const auto hdrBefore = pixels(hdr, rect);
        filter.process(hdr, rect, settings());
        QCOMPARE(pixels(hdr, rect), hdrBefore);
    }

    void testConfiguration()
    {
        KisCrystalFilter filter;
        auto config = settings();
        config->setProperty("tiling", true);
        KisCrystalConfigWidget widget(nullptr, nullptr);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        auto saved = widget.configuration();
        QCOMPARE(saved->getDouble("cellSize"), 8.75);
        QCOMPARE(saved->getInt("seed"), 1234567);
        auto restored = filter.factoryConfiguration(KisGlobalResourcesInterface::instance());
        restored->fromXML(saved->toXML());
        const QRect rect(-20, -20, 64, 64);
        auto first = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
        auto second = pattern(first->colorSpace(), rect);
        filter.process(first, rect, config);
        filter.process(second, rect, restored->cloneWithResourcesSnapshot());
        QCOMPARE(pixels(first, rect), pixels(second, rect));
        auto *tiling = widget.findChild<QCheckBox *>("crystalTiling");
        QVERIFY(tiling);
        tiling->setChecked(false);
        QCOMPARE(spy.count(), 1);
        KisPaintDeviceSP device = new KisPaintDevice(first->colorSpace());
        device->setDefaultBounds(new CanvasBounds(QRect(-10, -20, 120, 80)));
        KisCrystalConfigWidget initial(nullptr, device);
        initial.setConfiguration(filter.defaultConfiguration(KisGlobalResourcesInterface::instance()));
        saved = initial.configuration();
        QCOMPARE(saved->getInt("tileX"), -10);
        QCOMPARE(saved->getInt("tileHeight"), 80);
        device->setDefaultBounds(new CanvasBounds(QRect(0, 0, 300, 200)));
        KisCrystalConfigWidget later(nullptr, device);
        later.setConfiguration(saved);
        QCOMPARE(later.configuration()->getInt("tileWidth"), 120);
        QCOMPARE(later.configuration()->getInt("tileX"), -10);
    }

    void testCancellation()
    {
        KisCrystalFilter filter;
        const QRect rect(0, 0, 128, 128);
        auto device = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect);
        const auto before = pixels(device, rect);
        KoDummyUpdaterHolder progress;
        connect(progress.updater(), &KoUpdater::sigProgress, this, [&](int percent) {
            if (percent >= 10)
                QMetaObject::invokeMethod(progress.updater(),
                                          "setInterrupted",
                                          Qt::DirectConnection,
                                          Q_ARG(bool, true));
        });
        filter.process(device, rect, settings(), progress.updater());
        QVERIFY(progress.updater()->interrupted());
        QCOMPARE(pixels(device, rect), before);
    }

    void benchmarkCpu()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_CRYSTAL"))
            QSKIP("Opt-in CPU timing.");
        KisCrystalFilter filter;
        for (const QString &depth : {QString("U8"), QString("F32")}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
            for (const QSize size : {QSize(2480, 3508), QSize(4096, 4096)}) {
                QList<qint64> times;
                for (int run = 0; run < 3; ++run) {
                    KisPaintDeviceSP device = new KisPaintDevice(cs);
                    const QRect rect(QPoint(), size);
                    device->fill(rect, KoColor(Qt::gray, cs));
                    auto config = settings();
                    config->setProperty("cellSize", 20.0);
                    QElapsedTimer timer;
                    timer.start();
                    filter.process(device, rect, config);
                    times.append(timer.elapsed());
                }
                std::sort(times.begin(), times.end());
                qInfo() << "Crystal CPU single call" << depth << size << "median ms" << times[1];
            }
        }
    }

    void renderReviewArtifacts()
    {
        const QString directory = qEnvironmentVariable("SOLSTICE_CRYSTAL_ARTIFACT_DIR");
        if (directory.isEmpty())
            QSKIP("Opt-in review images.");
        QImage source(257, 193, QImage::Format_ARGB32);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, QColor::fromHsv((x * 2 + y) % 360, 180, 120 + (x + y) % 136));
        KisPaintDeviceSP device = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
        device->convertFromQImage(source, nullptr);
        device->setDefaultBounds(new CanvasBounds(source.rect()));
        KisCrystalFilter filter;
        KisCrystalConfigWidget widget(nullptr, device);
        auto config = settings();
        config->setProperty("cellSize", 20.0);
        config->setProperty("tiling", true);
        config->setProperty("tileX", 0);
        config->setProperty("tileY", 0);
        config->setProperty("tileWidth", source.width());
        config->setProperty("tileHeight", source.height());
        widget.setConfiguration(config);
        widget.resize(widget.sizeHint());
        widget.show();
        QApplication::processEvents();
        QVERIFY(widget.grab().save(QDir(directory).filePath("crystal-widget.png")));
        filter.process(device, source.rect(), config);
        const QImage result = device->convertToQImage(nullptr, source.rect());
        QImage sheet(3 * source.width(), 3 * source.height(), QImage::Format_ARGB32);
        QPainter painter(&sheet);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                painter.drawImage(x * source.width(), y * source.height(), result);
        painter.end();
        QVERIFY(sheet.save(QDir(directory).filePath("crystal-3x3.png")));
        QVERIFY(source.save(QDir(directory).filePath("crystal-source.png")));
    }
};
SIMPLE_TEST_MAIN(KisCrystalFilterTest)
#include "KisCrystalFilterTest.moc"
