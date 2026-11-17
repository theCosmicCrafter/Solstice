/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisLensBlurHighlights.h"
#include "kis_lens_blur_filter.h"
#include "kis_wdg_lens_blur.h"
#include <KisGlobalResourcesInterface.h>
#include <KoChannelInfo.h>
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
#include <kis_convolution_kernel.h>
#include <kis_convolution_painter.h>
#include <kis_default_bounds.h>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_transaction.h>
#include <memory>
#include <simpletest.h>

namespace
{
class CanvasBounds : public KisDefaultBounds
{
public:
    CanvasBounds(QRect rect, int lod = 0)
        : m_rect(rect)
        , m_lod(lod)
    {
    }
    QRect bounds() const override
    {
        return m_rect;
    }
    int currentLevelOfDetail() const override
    {
        return m_lod;
    }

private:
    QRect m_rect;
    int m_lod;
};
QByteArray pixels(KisPaintDeviceSP device, const QRect &rect)
{
    QByteArray data(rect.width() * rect.height() * device->pixelSize(), '\0');
    device->readBytes(reinterpret_cast<quint8 *>(data.data()), rect);
    return data;
}
KisPaintDeviceSP pattern(const KoColorSpace *cs, const QRect &rect, int lod = 0)
{
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    device->setDefaultBounds(new CanvasBounds(rect, lod));
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
    KisLensBlurFilter filter;
    auto config = filter.defaultConfiguration(KisGlobalResourcesInterface::instance());
    config->setProperty("irisRadius", 7);
    config->setProperty("irisRotation", 37);
    config->setProperty("irisRoundness", 55.0);
    config->setProperty("highlightStrength", 60.0);
    return config->cloneWithResourcesSnapshot();
}
const QStringList shapes =
    {"Triangle", "Quadrilateral (4)", "Pentagon (5)", "Hexagon (6)", "Heptagon (7)", "Octagon (8)"};

// Frozen pre-extension aperture construction, independent of the new code.
KisConvolutionKernelSP legacyKernel(int sides, int radius, int rotation)
{
    QPolygonF polygon;
    qreal angle = 0;
    for (int i = 0; i < sides; ++i) {
        polygon << QPointF(0.5 * cos(angle), 0.5 * sin(angle));
        angle += 2 * M_PI / sides;
    }
    QTransform transform;
    transform.rotate(rotation);
    transform.scale(radius * 2, radius * 2);
    polygon = transform.map(polygon);
    const QRectF bounds = polygon.boundingRect();
    QImage image(bounds.toAlignedRect().size(), QImage::Format_RGB32);
    image.fill(0);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor::fromRgb(255, 255, 255));
    QTransform offset;
    offset.translate(-bounds.x(), -bounds.y());
    painter.setTransform(offset);
    painter.drawPolygon(polygon, Qt::WindingFill);
    Eigen::Matrix<qreal, Eigen::Dynamic, Eigen::Dynamic> matrix(image.height(), image.width());
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            matrix(y, x) = qRed(image.pixel(x, y));
    return KisConvolutionKernel::fromMatrix(matrix, 0, matrix.sum());
}
double maxError(KisPaintDeviceSP a, KisPaintDeviceSP b, const QRect &rect)
{
    const auto *cs = a->colorSpace();
    const auto first = pixels(a, rect), second = pixels(b, rect);
    QVector<float> va(cs->channelCount()), vb(cs->channelCount());
    double error = 0;
    for (int p = 0; p < first.size(); p += cs->pixelSize()) {
        cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(first.constData() + p), va);
        cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(second.constData() + p), vb);
        for (int c = 0; c < va.size(); ++c)
            error = qMax(error, double(std::abs(va[c] - vb[c])));
    }
    return error;
}
} // namespace

class KisLensBlurFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testInstalledPlugin()
    {
        auto filter = KisFilterRegistry::instance()->value("lens blur");
        QVERIFY(filter);
        auto config = filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
        QVERIFY(config->hasProperty("irisRoundness"));
        QCOMPARE(config->getDouble("irisRoundness"), 0.0);
        QCOMPARE(config->getDouble("highlightStrength"), 0.0);
        QVERIFY(filter->supportsAdjustmentLayers());
        QVERIFY(filter->supportsLevelOfDetail(config, 1));
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
        const QRect rect(-33, -21, 73, 57);
        KisLensBlurFilter filter;
        for (double enhancement : {0.0, 60.0}) {
            auto config = settings();
            config->setProperty("highlightStrength", enhancement);
            auto whole = pattern(cs, rect), tiled = pattern(cs, rect);
            const auto before = pixels(whole, rect);
            {
                KisTransaction tx(whole);
                filter.process(whole, rect, config);
            }
            {
                KisTransaction tx(tiled);
                std::vector<std::future<void>> jobs;
                for (int stripe = 0; stripe < 3; ++stripe) {
                    jobs.push_back(std::async(std::launch::async, [&, stripe]() {
                        for (int y = rect.top() + stripe * 13; y <= rect.bottom(); y += 39)
                            for (int x = rect.left(); x <= rect.right(); x += 19)
                                filter.process(tiled, rect.intersected(QRect(x, y, 19, 13)), config);
                    }));
                }
                for (auto &job : jobs)
                    job.get();
            }
            QVERIFY(pixels(whole, rect) != before);
            const double error = maxError(whole, tiled, rect);
            const double tolerance = depth == "U8" ? 1.0 / 255.0 + 1e-6 : depth == "F16" ? 0.002 : 0.0001;
            QVERIFY2(error <= tolerance, qPrintable(QString("Parallel patch error %1").arg(error, 0, 'g', 12)));
            auto zero = pattern(cs, rect);
            config->setProperty("irisRadius", 0);
            filter.process(zero, rect, config);
            QCOMPARE(pixels(zero, rect), before);
        }
    }
    void testLegacyCompatibility()
    {
        KisLensBlurFilter filter;
        const QRect rect(-13, -9, 39, 31);
        for (const QString &depth : {QString("U8"), QString("U16"), QString("F16"), QString("F32")}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
            for (int sides = 3; sides <= 8; ++sides) {
                for (int radius : {2, 9}) {
                    auto config = filter.factoryConfiguration(KisGlobalResourcesInterface::instance());
                    config->setProperty("irisShape", shapes[sides - 3]);
                    config->setProperty("irisRadius", radius);
                    config->setProperty("irisRotation", 37);
                    auto old = pattern(cs, rect), current = pattern(cs, rect);
                    {
                        KisTransaction tx(old);
                        KisConvolutionPainter painter(old);
                        painter.applyMatrix(legacyKernel(sides, radius, 37),
                                            old,
                                            rect.topLeft(),
                                            rect.topLeft(),
                                            rect.size(),
                                            BORDER_REPEAT);
                    }
                    {
                        KisTransaction tx(current);
                        filter.process(current, rect, config->cloneWithResourcesSnapshot());
                    }
                    QCOMPARE(pixels(current, rect), pixels(old, rect));
                    config->setProperty("irisRoundness", 0.0);
                    config->setProperty("highlightStrength", 0.0);
                    current = pattern(cs, rect);
                    {
                        KisTransaction tx(current);
                        filter.process(current, rect, config->cloneWithResourcesSnapshot());
                    }
                    QCOMPARE(pixels(current, rect), pixels(old, rect));
                }
            }
        }
    }
    void testCircleBoundsAndLod()
    {
        KisLensBlurFilter filter;
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(-20, -20, 64, 64);
        auto config = settings();
        config->setProperty("irisRoundness", 100.0);
        auto reference = pattern(cs, rect);
        filter.process(reference, rect, config);
        for (const QString &shape : shapes) {
            config->setProperty("irisShape", shape);
            config->setProperty("irisRotation", 89);
            auto device = pattern(cs, rect);
            filter.process(device, rect, config);
            QCOMPARE(pixels(device, rect), pixels(reference, rect));
        }
        config->setProperty("halfWidth", 1);
        config->setProperty("halfHeight", 1);
        QCOMPARE(filter.changedRect(rect, config, 0), rect.adjusted(-7, -7, 7, 7));
        QCOMPARE(filter.neededRect(rect, config, 0), rect.adjusted(-14, -14, 14, 14));
        QCOMPARE(filter.changedRect(rect, config, 1), rect.adjusted(-3, -3, 3, 3));
        auto lod = pattern(cs, rect, 1);
        filter.process(lod, rect, config);
        config->setProperty("irisRadius", 3);
        auto scaled = pattern(cs, rect);
        filter.process(scaled, rect, config);
        QCOMPARE(pixels(lod, rect), pixels(scaled, rect));
    }
    void testHighlightAlphaHdrAndConstants()
    {
        KisLensBlurFilter filter;
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        const QRect rect(0, 0, 64, 64);
        auto config = settings();
        config->setProperty("highlightStrength", 100.0);
        KisPaintDeviceSP constant = new KisPaintDevice(cs);
        constant->setDefaultBounds(new CanvasBounds(rect));
        KoColor color(cs);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{-2.0f, 0.8f, 3.0f, 0.375f});
        constant->fill(rect, color);
        KisPaintDeviceSP expected = new KisPaintDevice(*constant);
        filter.process(constant, rect, config);
        QVERIFY(maxError(constant, expected, rect) < 2e-5);

        auto source = pattern(cs, rect);
        KisPaintDeviceSP enhanced = new KisPaintDevice(*source), normal = new KisPaintDevice(*source);
        filter.process(enhanced, rect, config);
        config->setProperty("highlightStrength", 0.0);
        filter.process(normal, rect, config);
        const auto a = pixels(enhanced, rect), b = pixels(normal, rect);
        QVector<float> va(4), vb(4);
        bool brighter = false;
        for (int offset = 0; offset < a.size(); offset += cs->pixelSize()) {
            cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(a.constData() + offset), va);
            cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(b.constData() + offset), vb);
            QVERIFY(std::abs(va[3] - vb[3]) < 2e-6);
            for (int c = 0; c < 3; ++c) {
                QVERIFY(va[c] + 2e-5 >= vb[c]);
                brighter |= va[c] > vb[c] + 0.01;
            }
        }
        QVERIFY(brighter);
        // Hidden bright colors must not enter a visible bokeh footprint.
        config->setProperty("highlightStrength", 100.0);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{100.0f, 0.0f, 0.0f, 0.0f});
        source->fill(rect, color);
        cs->fromNormalisedChannelsValue(color.data(), QVector<float>{0.0f, 1.0f, 0.0f, 1.0f});
        source->fill(QRect(24, 24, 16, 16), color);
        filter.process(source, rect, config);
        const auto data = pixels(source, QRect(20, 20, 24, 24));
        for (int offset = 0; offset < data.size(); offset += cs->pixelSize()) {
            cs->normalisedChannelsValue(reinterpret_cast<const quint8 *>(data.constData() + offset), va);
            if (va[3] > 1e-5) {
                QVERIFY(std::abs(va[0]) < 1e-5);
                QVERIFY(std::abs(va[1] - 1.0f) < 1e-4);
            }
        }
    }
    void testSelectionChannelsUndoCancellation()
    {
        KisLensBlurFilter filter;
        auto config = settings();
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        const QRect rect(-20, -20, 64, 64), part(-5, -5, 20, 20);
        auto source = pattern(cs, rect);
        const auto before = pixels(source, rect);
        KisPaintDeviceSP full = new KisPaintDevice(*source), partial = new KisPaintDevice(*source);
        filter.process(full, rect, config);
        filter.process(source, partial, nullptr, part, config);
        QVERIFY(maxError(full, partial, part) <= 1.0 / 255.0 + 1e-6);
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(part, 128);
        KisPaintDeviceSP selected = new KisPaintDevice(*source);
        KisTransaction tx(selected);
        filter.process(source, selected, selection, rect, config);
        const auto after = pixels(selected, rect);
        QVERIFY(after != before);
        QCOMPARE(pixels(selected, QRect(-20, -20, 64, 10)), pixels(source, QRect(-20, -20, 64, 10)));
        std::unique_ptr<KUndo2Command> command(tx.endAndTake());
        command->redo();
        command->undo();
        QCOMPARE(pixels(selected, rect), before);
        command->redo();
        QCOMPARE(pixels(selected, rect), after);
        QBitArray flags(cs->channelCount(), false);
        flags.setBit(0, true);
        config->setChannelFlags(flags);
        filter.process(source, rect, config);
        const auto masked = pixels(source, rect), all = pixels(full, rect);
        for (int offset = 0; offset < before.size(); offset += cs->pixelSize()) {
            QCOMPARE(masked[offset], all[offset]);
            QCOMPARE(masked.mid(offset + 1, 3), before.mid(offset + 1, 3));
        }
        source = pattern(cs, rect);
        KoDummyUpdaterHolder progress;
        connect(progress.updater(), &KoUpdater::sigProgress, this, [&](int percent) {
            if (percent >= 10)
                QMetaObject::invokeMethod(progress.updater(),
                                          "setInterrupted",
                                          Qt::DirectConnection,
                                          Q_ARG(bool, true));
        });
        filter.process(source, rect, config, progress.updater());
        QVERIFY(progress.updater()->interrupted());
        QCOMPARE(pixels(source, rect), before);
    }
    void testCpuEnginesAndDirtyBounds()
    {
        KisLensBlurFilter filter;
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString());
        const QRect rect(-20, -20, 64, 64);
        auto config = settings();
        for (int radius : {2, 9}) {
            const auto old = legacyKernel(5, radius, 37);
            Eigen::Matrix<qreal, Eigen::Dynamic, Eigen::Dynamic> matrix(old->height() | 1, old->width() | 1);
            matrix.setZero();
            matrix.bottomRightCorner(old->height(), old->width()) = *old->data();
            const auto kernel = KisConvolutionKernel::fromMatrix(matrix, 0, matrix.sum());
            auto spatial = pattern(cs, rect), fft = pattern(cs, rect);
            const QRect needed = rect.adjusted(-2 * radius, -2 * radius, 2 * radius, 2 * radius);
            KisLensBlurHighlights::apply(spatial,
                                         rect,
                                         needed,
                                         config,
                                         kernel,
                                         nullptr,
                                         KisConvolutionPainter::SPATIAL);
            KisLensBlurHighlights::apply(fft, rect, needed, config, kernel, nullptr, KisConvolutionPainter::FFTW);
            QVERIFY(maxError(spatial, fft, rect) < 1e-5);
        }
        auto source = pattern(cs, rect);
        KisPaintDeviceSP result = new KisPaintDevice(*source);
        filter.process(result, rect, config);
        const QRect dirty(-3, -5, 4, 7);
        source->fill(dirty, KoColor(Qt::red, cs));
        KisPaintDeviceSP full = new KisPaintDevice(*source);
        filter.process(full, rect, config);
        filter.process(source, result, nullptr, filter.changedRect(dirty, config, 0), config);
        QVERIFY(maxError(full, result, rect) < 1e-5);
    }
    void testConfiguration()
    {
        KisLensBlurFilter filter;
        auto config = settings();
        config->setProperty("irisShape", "Triangle");
        KisWdgLensBlur widget(nullptr);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        const auto saved = widget.configuration();
        QCOMPARE(saved->getString("irisShape"), QString("Triangle"));
        QCOMPARE(saved->getDouble("irisRoundness"), 55.0);
        QCOMPARE(saved->getDouble("highlightStrength"), 60.0);
        auto restored = filter.factoryConfiguration(KisGlobalResourcesInterface::instance());
        restored->fromXML(saved->toXML());
        const QRect rect(0, 0, 32, 32);
        auto a = pattern(KoColorSpaceRegistry::instance()->rgb8(), rect), b = pattern(a->colorSpace(), rect);
        filter.process(a, rect, config);
        filter.process(b, rect, restored->cloneWithResourcesSnapshot());
        QCOMPARE(pixels(a, rect), pixels(b, rect));
        auto old = filter.factoryConfiguration(KisGlobalResourcesInterface::instance());
        widget.setConfiguration(old);
        QCOMPARE(widget.configuration()->getDouble("irisRoundness"), 0.0);
        QCOMPARE(widget.configuration()->getDouble("highlightStrength"), 0.0);
    }
    void benchmarkCpu()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_LENS"))
            QSKIP("Opt-in CPU timings.");
        KisLensBlurFilter filter;
        for (const QString &depth : {QString("U8"), QString("F32")}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
            for (const QSize size : {QSize(2480, 3508), QSize(4096, 4096)}) {
                for (int radius : {5, 50}) {
                    QList<qint64> times;
                    for (int run = 0; run < 3; ++run) {
                        const QRect rect(QPoint(), size);
                        KisPaintDeviceSP device = new KisPaintDevice(cs);
                        device->setDefaultBounds(new CanvasBounds(rect));
                        device->fill(rect, KoColor(Qt::gray, cs));
                        auto config = settings();
                        config->setProperty("irisRadius", radius);
                        QElapsedTimer timer;
                        timer.start();
                        filter.process(device, rect, config);
                        times.append(timer.elapsed());
                    }
                    std::sort(times.begin(), times.end());
                    qInfo() << "Lens CPU single call, FFTW" << KisConvolutionPainter::supportsFFTW() << depth << size
                            << "radius" << radius << "median ms" << times[1];
                }
            }
        }
    }
    void renderReviewArtifacts()
    {
        const QString directory = qEnvironmentVariable("SOLSTICE_LENS_ARTIFACT_DIR");
        if (directory.isEmpty())
            QSKIP("Opt-in review images.");
        QImage source(320, 240, QImage::Format_ARGB32);
        source.fill(QColor(15, 30, 45));
        {
            QPainter painter(&source);
            painter.setPen(Qt::NoPen);
            for (int i = 0; i < 35; ++i) {
                painter.setBrush(QColor::fromHsv(i * 47 % 360, i % 3 ? 70 : 180, 255));
                painter.drawEllipse(QPoint(15 + i * 83 % 290, 15 + i * 61 % 210), 3 + i % 5, 3 + i % 5);
            }
        }
        QImage sheet(640, 544, QImage::Format_ARGB32);
        sheet.fill(Qt::white);
        QPainter painter(&sheet);
        KisLensBlurFilter filter;
        for (int i = 0; i < 4; ++i) {
            KisPaintDeviceSP device = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
            device->convertFromQImage(source, nullptr);
            device->setDefaultBounds(new CanvasBounds(source.rect()));
            auto config = settings();
            config->setProperty("irisRadius", 14);
            config->setProperty("irisShape", "Triangle");
            config->setProperty("irisRoundness", i >= 2 ? 100.0 : 0.0);
            config->setProperty("highlightStrength", i == 3 ? 100.0 : 0.0);
            if (i)
                filter.process(device, source.rect(), config);
            const QPoint origin((i % 2) * 320, (i / 2) * 272);
            painter.setPen(Qt::black);
            painter.drawText(QRect(origin, QSize(320, 32)),
                             Qt::AlignCenter,
                             QStringList{"Original", "Legacy triangle", "Circle", "Circle + highlights"}[i]);
            painter.drawImage(origin + QPoint(0, 32), device->convertToQImage(nullptr, source.rect()));
            if (i == 3) {
                KisWdgLensBlur widget(nullptr);
                widget.setConfiguration(config);
                widget.resize(widget.sizeHint());
                widget.show();
                QApplication::processEvents();
                QVERIFY(widget.grab().save(QDir(directory).filePath("lens-widget.png")));
            }
        }
        painter.end();
        QVERIFY(sheet.save(QDir(directory).filePath("lens-comparison.png")));
    }
};
SIMPLE_TEST_MAIN(KisLensBlurFilterTest)
#include "KisLensBlurFilterTest.moc"
