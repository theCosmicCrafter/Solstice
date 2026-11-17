/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisColorReferenceFilter.h"
#include "KisColorReferenceConfigWidget.h"
#include "KisColorReferenceResources.h"
#include <KisDocument.h>
#include <KisGlobalResourcesInterface.h>
#include <KisPart.h>
#include <KoColorSpaceRegistry.h>
#include <KoColorSpaceTraits.h>
#include <KoStopGradient.h>
#include <KoUpdater.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <cmath>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <future>
#include <kis_adjustment_layer.h>
#include <kis_filter_mask.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_transaction.h>
#include <limits>
#include <testui.h>
#include <testutil.h>
#include <widgets/gradient/KisGenericGradientEditor.h>
#include <widgets/gradient/KisGradientChooser.h>

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
    return KisColorReferenceFilter()
        .defaultConfiguration(KisGlobalResourcesInterface::instance())
        ->cloneWithResourcesSnapshot();
}
} // namespace
class KisColorReferenceFilterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testTables()
    {
        using namespace ColorReference;
        QImage uniform(16, 16, QImage::Format_ARGB32);
        uniform.fill(QColor("#e04080"));
        const auto table = fromImage(uniform);
        QCOMPARE(table.size(), TableSize);
        for (const auto &c : table) {
            QVERIFY(std::abs(c[0] - 224.0 / 255) < 1e-6);
            QVERIFY(std::abs(c[1] - 64.0 / 255) < 1e-6);
        }
        auto decoded = decode(encode(table));
        QCOMPARE(decoded.size(), TableSize);
        QVERIFY(std::abs(decoded[500][2] - table[500][2]) < 1e-6);
        QVERIFY(decode("broken").isEmpty());
        auto malformed = table;
        malformed[5][0] = std::numeric_limits<double>::infinity();
        QVERIFY(encode(malformed).isEmpty());
        uniform.fill(Qt::transparent);
        QVERIFY(fromImage(uniform).isEmpty());
        QImage pair(2, 1, QImage::Format_ARGB32);
        pair.setPixel(0, 0, qRgba(32, 0, 0, 255));
        pair.setPixel(1, 0, qRgba(255, 255, 128, 64));
        const auto weighted = fromImage(pair);
        QCOMPARE(weighted, fromImage(pair));
        QVERIFY(weighted.front()[0] < .14);
        QVERIFY(weighted.back()[0] > .99);
        QVERIFY(brightness(lookup(weighted, .25)) < .2);
        // The reference's brightness distribution differs from source brightness.
        QVERIFY(std::abs(brightness(lookup(table, .1)) - .1) > .1);
        for (double l : {-0.2, 0.0, .1, .5, .95, 1.0, 2.0}) {
            const auto c = keepBrightness({1, .2, .6}, l);
            QVERIFY(std::abs(brightness(c) - l) < 1e-10);
            if (l >= 0 && l <= 1)
                for (double v : c)
                    QVERIFY(v >= -1e-10 && v <= 1 + 1e-10);
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
        const QRect rect(-19, -11, 71, 43);
        KisColorReferenceFilter filter;
        auto config = settings();
        auto whole = pattern(cs, rect), tiled = pattern(cs, rect);
        const auto before = pixels(whole, rect);
        config->setProperty("amount", 0.0);
        filter.process(whole, rect, config);
        QCOMPARE(pixels(whole, rect), before);
        config->setProperty("amount", 73.0);
        config->setProperty("keepBrightness", false);
        filter.process(whole, rect, config);
        {
            KisTransaction transaction(tiled);
            std::vector<std::future<void>> jobs;
            for (int i = 0; i < 3; ++i)
                jobs.push_back(std::async(std::launch::async, [&, i] {
                    for (int x = rect.left() + i * 11; x <= rect.right(); x += 33)
                        filter.process(tiled, rect.intersected(QRect(x, rect.top(), 11, rect.height())), config);
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
            for (const auto *ch : cs->channels())
                if (ch->channelType() == KoChannelInfo::ALPHA)
                    QCOMPARE(after.mid(p + ch->pos(), ch->size()), before.mid(p + ch->pos(), ch->size()));
        }
        QBitArray flags(cs->channelCount(), false);
        flags.setBit(0);
        config->setChannelFlags(flags);
        auto masked = pattern(cs, rect);
        filter.process(masked, rect, config);
        const auto partial = pixels(masked, rect);
        for (int p = 0; p < partial.size(); p += cs->pixelSize())
            for (int c = 1; c < int(cs->channelCount()); ++c) {
                const auto *ch = cs->channels()[c];
                QCOMPARE(partial.mid(p + ch->pos(), ch->size()), before.mid(p + ch->pos(), ch->size()));
            }
        QCOMPARE(filter.neededRect(rect, config, 2), rect);
        QCOMPARE(filter.changedRect(rect, config, 2), rect);
        QVERIFY(filter.supportsLevelOfDetail(config, 2));
    }
    void testGradientAndHdr()
    {
        using namespace ColorReference;
        const auto *cs = workingSpace();
        QVERIFY(cs);
        KoStopGradientSP gradient(new KoStopGradient());
        gradient->setStops(
            {KoGradientStop(0, KoColor(QColor("#263b85"), cs)), KoGradientStop(1, KoColor(QColor("#f2b75a"), cs))});
        gradient->setValid(true);
        const auto lut = fromGradient(gradient);
        QVERIFY(!lut.isEmpty());
        const auto restored = gradientFromXml(gradientXml(gradient));
        QVERIFY(restored);
        QCOMPARE(encode(fromGradient(restored)), encode(lut));
        for (int i = 0; i <= 100; ++i) {
            const double t = i / 100.0;
            KoColor expected(cs);
            gradient->colorAt(expected, t);
            expected.convertTo(cs);
            const auto *p = reinterpret_cast<const KoRgbF32Traits::Pixel *>(expected.data());
            const auto c = lookup(lut, t);
            // KoStopGradient quantizes interpolation weights to 15 bits;
            // the LUT interpolates between two such quantized samples.
            QVERIFY(std::abs(c[0] - p->red) < 4e-5);
            QVERIFY(std::abs(c[1] - p->green) < 4e-5);
            QVERIFY(std::abs(c[2] - p->blue) < 4e-5);
        }
        KisColorReferenceFilter filter;
        auto config = settings();
        config->setProperty("lut", encode(lut));
        for (double l : {-0.25, .1, .5, .9, 1.7}) {
            const QRect rect(0, 0, 1, 1);
            KisPaintDeviceSP dev = new KisPaintDevice(cs);
            KoRgbF32Traits::Pixel pixel{float(l), float(l), float(l), .43f};
            dev->writeBytes(reinterpret_cast<const quint8 *>(&pixel), rect);
            filter.process(dev, rect, config);
            auto bytes = pixels(dev, rect);
            const auto *out = reinterpret_cast<const KoRgbF32Traits::Pixel *>(bytes.constData());
            QVERIFY(std::abs(brightness({out->red, out->green, out->blue}) - l) < 1e-5);
            QCOMPARE(out->alpha, .43f);
        }
    }
    void testProfilesAndGradientMap()
    {
        const auto *work = ColorReference::workingSpace();
        const QRect rect(0, 0, 256, 1);
        KisPaintDeviceSP original = new KisPaintDevice(work);
        QByteArray bytes(256 * work->pixelSize(), '\0');
        for (int i = 0; i < 256; ++i) {
            auto *p = reinterpret_cast<KoRgbF32Traits::Pixel *>(bytes.data() + i * work->pixelSize());
            p->red = p->green = p->blue = i / 255.0f;
            p->alpha = .7f;
        }
        original->writeBytes(reinterpret_cast<const quint8 *>(bytes.constData()), rect);
        KisColorReferenceFilter filter;
        auto config = settings();
        config->setProperty("keepBrightness", false);
        config->setProperty("source", 1);
        config->setProperty("lut", config->getString("gradientLut"));
        auto gradientMap = KisFilterRegistry::instance()->value("gradientmap");
        QVERIFY(gradientMap);
        auto mapConfig = gradientMap->defaultConfiguration(KisGlobalResourcesInterface::instance());
        mapConfig->setProperty("gradientXML", config->getString("gradientXML"));
        mapConfig->setProperty("colorMode", 0);
        KisPaintDeviceSP expected = new KisPaintDevice(*original), actual = new KisPaintDevice(*original);
        gradientMap->process(expected, rect, mapConfig->cloneWithResourcesSnapshot());
        filter.process(actual, rect, config);
        const auto mapBytes = pixels(expected, rect), refBytes = pixels(actual, rect);
        for (int i = 0; i < 256; ++i) {
            const auto *a =
                reinterpret_cast<const KoRgbF32Traits::Pixel *>(mapBytes.constData() + i * work->pixelSize());
            const auto *b =
                reinterpret_cast<const KoRgbF32Traits::Pixel *>(refBytes.constData() + i * work->pixelSize());
            // Only neutral inputs with opaque gradients share the brightness
            // measure. Gradient Map's cache depends on device extent.
            QVERIFY(std::abs(a->red - b->red) < .005);
            QCOMPARE(b->alpha, .7f);
        }
        config = settings();
        config->setProperty("keepBrightness", false);
        expected = new KisPaintDevice(*original);
        filter.process(expected, rect, config);
        for (const QString &profile : {QString("sRGB-elle-V2-g10.icc"), QString("Rec2020-elle-V4-g10.icc")}) {
            const auto *space = KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", profile);
            QVERIFY2(space, qPrintable(profile));
            actual = new KisPaintDevice(*original);
            actual->convertTo(space);
            filter.process(actual, rect, config);
            actual->convertTo(work);
            const auto native = pixels(actual, rect), reference = pixels(expected, rect);
            for (int i = 0; i < 256; ++i) {
                const auto *a =
                    reinterpret_cast<const KoRgbF32Traits::Pixel *>(native.constData() + i * work->pixelSize());
                const auto *b =
                    reinterpret_cast<const KoRgbF32Traits::Pixel *>(reference.constData() + i * work->pixelSize());
                QVERIFY(std::abs(a->red - b->red) < .002);
                QVERIFY(std::abs(a->green - b->green) < .002);
                QVERIFY(std::abs(a->blue - b->blue) < .002);
            }
        }
    }
    void testLibraryAndWidget()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ColorReference::Library library(directory.path());
        QString error;
        const QImage reference = ColorReference::presetImage(4);
        QCOMPARE(library.add(reference, "sunset.psd", &error), QString("sunset.png"));
        QCOMPARE(library.add(reference, "sunset.psd", &error), QString("sunset-2.png"));
        QVERIFY(!library.image("sunset.png").isNull());
        QVERIFY(library.image("../outside.png").isNull());
        QVERIFY(!library.remove("../outside.png"));
        QVERIFY(!library.remove("preset:0"));
        const QString source = directory.filePath("sunset.png");
        const auto imported = ColorReference::Library::importImage(source, &error);
        QVERIFY2(!imported.isNull(), qPrintable(error));
        QFile invalid(directory.filePath("bad.png"));
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write("invalid image");
        invalid.close();
        QVERIFY(ColorReference::Library::importImage(invalid.fileName(), &error).isNull());
        QVERIFY(library.remove("bad.png"));
        auto config = settings();
        const QString lut = ColorReference::encode(ColorReference::fromImage(reference));
        config->setProperty("referenceId", "user:sunset.png");
        config->setProperty("lut", lut);
        config->setProperty("imageLut", lut);
        QVERIFY(library.remove("sunset.png"));
        KisColorReferenceConfigWidget widget(nullptr, nullptr, directory.path());
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        widget.setConfiguration(config);
        QCOMPARE(spy.count(), 0);
        QCOMPARE(widget.configuration()->getProperties(), config->getProperties());
        auto roundTrip = settings();
        roundTrip->fromXML(widget.configuration()->toXML());
        QCOMPARE(roundTrip->getProperties(), config->getProperties());
        auto *sourceMode = widget.findChild<QComboBox *>("colorReferenceSource");
        sourceMode->setCurrentIndex(1);
        sourceMode->setCurrentIndex(0);
        QCOMPARE(widget.configuration()->getString("lut"), lut);
        QCOMPARE(spy.count(), 2);
        widget.findChild<QListWidget *>("colorReferenceImages")->setCurrentRow(2);
        QVERIFY(widget.configuration()->getString("lut") != lut);
        for (int i = library.entries().size(); i < 50; ++i)
            QVERIFY(!library.add(reference, "copy.png", &error).isEmpty());
        QCOMPARE(library.entries().size(), 50);
        QVERIFY(library.add(reference, "extra.png", &error).isEmpty());
        QElapsedTimer timer;
        timer.start();
        KisColorReferenceConfigWidget full(nullptr, nullptr, directory.path());
        qInfo() << "Open 50 image references ms:" << timer.elapsed();
        QCOMPARE(full.findChild<QListWidget *>("colorReferenceImages")->count(), 58);
    }
    void testImageFileDrops()
    {
        QTemporaryDir sources, directory;
        QVERIFY(sources.isValid() && directory.isValid());
        const QString first = sources.filePath("first.PNG"), second = sources.filePath("second.png");
        QVERIFY(ColorReference::presetImage(0).save(first, "PNG"));
        QVERIFY(ColorReference::presetImage(1).save(second, "PNG"));
        QFile corrupt(sources.filePath("broken.png"));
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("not an image");
        corrupt.close();
        KisColorReferenceConfigWidget widget(nullptr, nullptr, directory.path());
        widget.resize(560, 430);
        widget.show();
        QApplication::processEvents();
        auto *list = widget.findChild<QListWidget *>("colorReferenceImages");
        auto *viewport = list->viewport();
        QVERIFY(list->acceptDrops());
        QVERIFY(viewport->acceptDrops());
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        ColorReference::Library library(directory.path());
        const auto sendDrop = [&](QMimeData *mime, QPoint point, Qt::DropActions actions) {
            QDragEnterEvent enter(point, actions, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &enter);
            if (!enter.isAccepted())
                return false;
            QDragMoveEvent move(point, actions, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &move);
            if (!move.isAccepted())
                return false;
            QDropEvent drop(QPointF(point), actions, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &drop);
            return drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
        };
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(first),
                      QUrl::fromLocalFile(corrupt.fileName()),
                      QUrl("https://example.invalid/image.png"),
                      QUrl::fromLocalFile(sources.path()),
                      QUrl::fromLocalFile(second)});
        // Dropping over an existing preset adds files, rather than replacing/reordering it.
        QVERIFY(sendDrop(&mime, QPoint(20, 20), Qt::CopyAction | Qt::MoveAction));
        QTRY_COMPARE(library.entries().size(), 2);
        QCOMPARE(list->count(), 10);
        QCOMPARE(widget.configuration()->getString("referenceId"), QString("user:second.png"));
        QCOMPARE(spy.count(), 1);
        QVERIFY(QFileInfo::exists(first) && QFileInfo::exists(second));
        const auto config = widget.configuration()->getProperties();
        QMimeData text;
        text.setText(first);
        QVERIFY(!sendDrop(&text, QPoint(20, 20), Qt::CopyAction));
        QMimeData remote;
        remote.setUrls({QUrl("https://example.invalid/image.png")});
        QVERIFY(!sendDrop(&remote, QPoint(20, 20), Qt::CopyAction));
        QVERIFY(!sendDrop(&mime, QPoint(20, 20), Qt::MoveAction));
        QCOMPARE(widget.configuration()->getProperties(), config);
        // Failed batches leave the active reference unchanged.
        QMimeData invalid;
        invalid.setUrls({QUrl::fromLocalFile(corrupt.fileName())});
        QVERIFY(sendDrop(&invalid, QPoint(20, 20), Qt::CopyAction));
        QApplication::processEvents();
        QCOMPARE(widget.configuration()->getProperties(), config);
        QCOMPARE(library.entries().size(), 2);
        QString error;
        for (int i = 2; i < 49; ++i)
            QVERIFY(!library.add(ColorReference::presetImage(2), "filler.png", &error).isEmpty());
        QMimeData pair;
        pair.setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)});
        QVERIFY(sendDrop(&pair, QPoint(20, viewport->height() - 10), Qt::CopyAction));
        QTRY_COMPARE(library.entries().size(), 50);
        QCOMPARE(list->count(), 58);
        QCOMPARE(widget.configuration()->getString("referenceId"), QString("user:first-2.png"));
        QCOMPARE(spy.count(), 2);
        // Queued imports are cancelled when their owning filter widget is destroyed.
        QTemporaryDir cancelledLibrary;
        auto *transient = new KisColorReferenceConfigWidget(nullptr, nullptr, cancelledLibrary.path());
        auto *target = transient->findChild<QListWidget *>("colorReferenceImages")->viewport();
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &pair, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &pair, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &drop);
        QVERIFY(drop.isAccepted());
        delete transient;
        QApplication::processEvents();
        QVERIFY(ColorReference::Library(cancelledLibrary.path()).entries().isEmpty());
    }
    void testGradientEditing()
    {
        QTemporaryDir library;
        KisColorReferenceConfigWidget widget(nullptr, nullptr, library.path());
        widget.findChild<QComboBox *>("colorReferenceSource")->setCurrentIndex(1);
        QSignalSpy spy(&widget, &KisConfigWidget::sigConfigurationItemChanged);
        const auto before = widget.configuration()->getProperties();
        auto gradient = ColorReference::defaultGradient().dynamicCast<KoStopGradient>();
        gradient->setStops({KoGradientStop(0, KoColor(Qt::blue, ColorReference::workingSpace())),
                            KoGradientStop(1, KoColor(Qt::yellow, ColorReference::workingSpace()))});
        for (bool accept : {false, true}) {
            bool dialogFound = false;
            QTimer::singleShot(0, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto *editor = dialog->findChild<KisGenericGradientEditor *>();
                dialogFound = bool(editor);
                if (editor)
                    editor->setGradient(gradient);
                if (accept)
                    dialog->accept();
                else
                    dialog->reject();
            });
            widget.findChild<QPushButton *>("colorReferenceEditGradient")->click();
            QVERIFY(dialogFound);
            if (!accept) {
                QCOMPARE(widget.configuration()->getProperties(), before);
                QCOMPARE(spy.count(), 0);
            }
        }
        QCOMPARE(spy.count(), 1);
        QCOMPARE(widget.configuration()->getString("lut"),
                 ColorReference::encode(ColorReference::fromGradient(gradient)));
        const auto saved = widget.configuration();
        widget.setConfiguration(saved);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(widget.configuration()->getProperties(), saved->getProperties());
        auto *chooser = widget.findChild<KisGradientChooser *>("colorReferenceGradients");
        chooser->resourceSelected(ColorReference::defaultGradient());
        QCOMPARE(spy.count(), 2);
        chooser->resourceClicked(ColorReference::defaultGradient());
        QCOMPARE(spy.count(), 2);
        QVERIFY(widget.configuration()->getString("lut") != saved->getString("lut"));
    }
    void testSelectionUndoAndCancellation()
    {
        KisColorReferenceFilter filter;
        const QRect rect(-10, -10, 64, 64), selectionRect(0, 0, 10, 15);
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        auto dev = pattern(cs, rect);
        const auto before = pixels(dev, rect);
        KisSelectionSP selection = new KisSelection();
        selection->pixelSelection()->select(selectionRect, MAX_SELECTED);
        KisTransaction transaction(dev);
        filter.process(dev, dev, selection, rect, settings());
        const auto after = pixels(dev, rect);
        QVERIFY(after != before);
        for (int y = rect.top(); y <= rect.bottom(); ++y)
            for (int x = rect.left(); x <= rect.right(); ++x)
                if (!selectionRect.contains(x, y)) {
                    const int offset = ((y - rect.y()) * rect.width() + x - rect.x()) * cs->pixelSize();
                    QCOMPARE(after.mid(offset, cs->pixelSize()), before.mid(offset, cs->pixelSize()));
                }
        std::unique_ptr<KUndo2Command> command(transaction.endAndTake());
        command->redo(); // Register the already-executed transaction's first redo.
        command->undo();
        QCOMPARE(pixels(dev, rect), before);
        command->redo();
        QCOMPARE(pixels(dev, rect), after);
        for (int threshold : {10, 60, 99}) {
            dev = pattern(cs, rect);
            KoDummyUpdaterHolder progress;
            connect(progress.updater(), &KoUpdater::sigProgress, this, [&](int percent) {
                if (percent >= threshold)
                    QMetaObject::invokeMethod(progress.updater(),
                                              "setInterrupted",
                                              Qt::DirectConnection,
                                              Q_ARG(bool, true));
            });
            filter.process(dev, rect, settings(), progress.updater());
            QVERIFY(progress.updater()->interrupted());
            QCOMPARE(pixels(dev, rect), before);
        }
        auto bad = settings();
        bad->setProperty("lut", "bad table");
        filter.process(dev, rect, bad);
        QCOMPARE(pixels(dev, rect), before);
    }
    void testNativePersistence()
    {
        QVERIFY(KisFilterRegistry::instance()->value("solstice_color_reference"));
        QTemporaryDir directory;
        const QRect rect(0, 0, 32, 32);
        QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());
        doc->setFileBatchMode(true);
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        KisImageSP image = new KisImage(doc->createUndoStore(), 32, 32, cs, "reference");
        doc->setCurrentImage(image);
        KisPaintLayerSP layer = new KisPaintLayer(image, "paint", 255);
        image->addNode(layer);
        layer->paintDevice()->fill(rect, KoColor(QColor("#806040"), cs));
        auto config = settings();
        config->setProperty("referenceId", "user:deleted.png");
        KisFilterMaskSP mask = new KisFilterMask(image, "reference mask");
        mask->setFilter(config->cloneWithResourcesSnapshot());
        image->addNode(mask, layer);
        mask->initSelection(layer);
        mask->select(rect, MAX_SELECTED);
        KisAdjustmentLayerSP adjustment =
            new KisAdjustmentLayer(image, "reference layer", config->cloneWithResourcesSnapshot(), nullptr);
        image->addNode(adjustment, image->root(), layer);
        image->refreshGraphAsync();
        image->waitForDone();
        const auto before = pixels(image->projection(), rect);
        const QString path = directory.filePath("reference.kra");
        QVERIFY(doc->exportDocumentSync(path, KisDocument::nativeFormatMimeType()));
        QScopedPointer<KisDocument> loaded(KisPart::instance()->createDocument());
        loaded->setFileBatchMode(true);
        QVERIFY(loaded->loadNativeFormat(path));
        loaded->image()->refreshGraphAsync();
        loaded->image()->waitForDone();
        QCOMPARE(pixels(loaded->image()->projection(), rect), before);
        KisAdjustmentLayer *savedLayer = nullptr;
        KisFilterMask *savedMask = nullptr;
        for (auto node = loaded->image()->root()->firstChild(); node; node = node->nextSibling()) {
            if (auto adjustment = dynamic_cast<KisAdjustmentLayer *>(node.data()))
                savedLayer = adjustment;
            if (auto paint = dynamic_cast<KisPaintLayer *>(node.data()))
                savedMask = dynamic_cast<KisFilterMask *>(paint->firstChild().data());
        }
        QVERIFY(savedLayer);
        QCOMPARE(savedLayer->filter()->getString("lut"), config->getString("lut"));
        QVERIFY(savedMask);
        QCOMPARE(savedMask->filter()->getString("lut"), config->getString("lut"));
    }
    void renderReviewArtifacts()
    {
        const QString directory = qEnvironmentVariable("SOLSTICE_COLOR_REFERENCE_ARTIFACT_DIR");
        if (directory.isEmpty())
            QSKIP("Opt-in review images");
        QTemporaryDir library;
        KisColorReferenceConfigWidget widget(nullptr, nullptr, library.path());
        widget.resize(560, 430);
        widget.show();
        QApplication::processEvents();
        QVERIFY(widget.grab().save(QDir(directory).filePath("color-reference-widget.png")));
        widget.findChild<QComboBox *>("colorReferenceSource")->setCurrentIndex(1);
        QApplication::processEvents();
        QVERIFY(widget.grab().save(QDir(directory).filePath("color-reference-gradient.png")));
        QImage original(256, 256, QImage::Format_ARGB32);
        original.fill(Qt::white);
        {
            QPainter p(&original);
            QLinearGradient sky(0, 0, 256, 256);
            sky.setColorAt(0, QColor("#152454"));
            sky.setColorAt(1, QColor("#a6d7da"));
            p.fillRect(original.rect(), sky);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor("#ffe29e"));
            p.drawEllipse(175, 20, 50, 50);
            p.setBrush(QColor("#284c42"));
            p.drawEllipse(-70, 140, 300, 170);
            p.setBrush(QColor("#75ab52"));
            p.drawEllipse(110, 170, 220, 150);
            p.setBrush(QColor("#452b40"));
            p.drawEllipse(65, 45, 115, 165);
            p.setBrush(QColor("#dba879"));
            p.drawEllipse(87, 67, 70, 105);
            p.setBrush(QColor("#314c8a"));
            p.drawEllipse(50, 165, 135, 160);
            p.setBrush(QColor("#cd573a"));
            p.drawEllipse(15, 205, 27, 30);
            p.setBrush(QColor("#e7d658"));
            p.drawEllipse(205, 210, 25, 25);
            p.setBrush(QColor("#292538"));
            p.drawEllipse(100, 100, 7, 9);
            p.drawEllipse(136, 100, 7, 9);
        }
        QImage sheet(1024, 3 * 306, QImage::Format_ARGB32);
        sheet.fill(QColor("#eeeeee"));
        QPainter painter(&sheet);
        for (int row = 0; row < 3; ++row) {
            const int preset = row == 0 ? 0 : row == 1 ? 1 : 5;
            painter.setPen(Qt::black);
            painter.drawText(8, row * 306 + 20, "Original / Reference / Brightness OFF / Brightness ON");
            painter.drawImage(0, row * 306 + 40, original);
            painter.drawImage(QRect(256, row * 306 + 40, 256, 256), ColorReference::presetImage(preset));
            for (int keep = 0; keep < 2; ++keep) {
                KisPaintDeviceSP dev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
                dev->convertFromQImage(original, nullptr);
                auto config = settings();
                config->setProperty(
                    "lut",
                    ColorReference::encode(ColorReference::fromImage(ColorReference::presetImage(preset))));
                config->setProperty("keepBrightness", bool(keep));
                KisColorReferenceFilter().process(dev, original.rect(), config);
                painter.drawImage((keep + 2) * 256, row * 306 + 40, dev->convertToQImage(nullptr, original.rect()));
            }
        }
        painter.end();
        QVERIFY(sheet.save(QDir(directory).filePath("color-reference-comparison.png")));
    }
};
KISTEST_MAIN(KisColorReferenceFilterTest)
#include "KisColorReferenceFilterTest.moc"
