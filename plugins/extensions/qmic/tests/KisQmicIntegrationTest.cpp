/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../gmic.h"
#include "../kis_qmic_import_tools.h"
#include "../kis_qmic_interface.h"
#include "../kis_qmic_plugin_interface.h"
#include "../kis_qmic_simple_convertor.h"
#include "../kis_qmic_synchronize_layers_command.h"
#include <KConfig>
#include <KPluginFactory>
#include <KisApplication.h>
#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisSolsticePaths.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoColorSpaceTraits.h>
#include <KoJsonTrader.h>
#include <QClipboard>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <kis_action.h>
#include <kis_action_manager.h>
#include <kis_config.h>
#include <kis_group_layer.h>
#include <kis_node_manager.h>
#include <kis_paint_layer.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_selection_mask.h>
#include <kis_undo_stores.h>
#include <testui.h>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{
template<typename T, typename Traits>
void checkPlanarConversion(const KoColorSpace *cs)
{
    // Independent interleaved reference, covering partial tiles, moved devices,
    // negative origins, every G'MIC channel count and both common unit values.
    const QRect rect(-19, 11, 137, 91);
    const int count = rect.width() * rect.height();
    for (int spectrum = 1; spectrum <= 4; ++spectrum) {
        for (float unit : {1.0f, 255.0f}) {
            auto image = KisQMicImageSP::create("", rect.width(), rect.height(), spectrum);
            for (int c = 0; c < spectrum; ++c) {
                for (int i = 0; i < count; ++i) {
                    image->m_data[c * count + i] = unit * (float((i * 37 + c * 53) % 601) / 400.0f - 0.25f);
                }
            }
            QByteArray expected(count * cs->pixelSize(), '\0');
            auto *native = reinterpret_cast<typename Traits::Pixel *>(expected.data());
            const float factor = 1.0f / unit;
            for (int i = 0; i < count; ++i) {
                auto scale = [&](float x) {
                    return KoColorSpaceMaths<float, T>::scaleToA(x * factor);
                };
                native[i].red = scale(image->m_data[i]);
                native[i].green = scale(image->m_data[(spectrum < 3 ? 0 : count) + i]);
                native[i].blue = scale(image->m_data[(spectrum < 3 ? 0 : 2 * count) + i]);
                native[i].alpha = scale(spectrum == 2       ? image->m_data[count + i]
                                            : spectrum == 4 ? image->m_data[3 * count + i]
                                                            : unit);
            }
            KisPaintDeviceSP dst = new KisPaintDevice(cs);
            dst->moveTo(9, -7);
            dst->fill(QRect(-30, -30, 200, 200), KoColor(Qt::white, cs));
            KisQmicSimpleConvertor::convertFromGmicFast(*image, dst, unit);
            QByteArray actual(expected.size(), '\0');
            dst->readBytes(reinterpret_cast<quint8 *>(actual.data()), 0, 0, rect.width(), rect.height());
            QCOMPARE(actual, expected);
            QCOMPARE(dst->x(), 0);
            QCOMPARE(dst->y(), 0);
            QCOMPARE(dst->exactBounds(), QRect(0, 0, rect.width(), rect.height()));
            KisPaintDeviceSP src = new KisPaintDevice(cs);
            src->moveTo(7, -9);
            src->writeBytes(reinterpret_cast<const quint8 *>(expected.constData()),
                            rect.x(),
                            rect.y(),
                            rect.width(),
                            rect.height());
            auto result = KisQMicImageSP::create("", rect.width(), rect.height(), 4);
            KisQmicSimpleConvertor::convertToGmicImageFast(src, *result, rect);
            for (int i = 0; i < count; ++i) {
                const T channels[] = {native[i].red, native[i].green, native[i].blue, native[i].alpha};
                for (int c = 0; c < 4; ++c) {
                    const float reference = KoColorSpaceMaths<T, float>::scaleToA(channels[c]) * 255.0f;
                    QCOMPARE(result->m_data[c * count + i], reference);
                }
            }
        }
    }
}
QByteArray pixels(KisPaintDeviceSP device, QRect rect)
{
    QByteArray data(rect.width() * rect.height() * device->pixelSize(), '\0');
    device->readBytes(reinterpret_cast<quint8 *>(data.data()), rect.x(), rect.y(), rect.width(), rect.height());
    return data;
}
KisQMicImageSP solid(const QString &name, int w, int h, float r)
{
    auto result = KisQMicImageSP::create(name, w, h, 4);
    const size_t n = size_t(w) * h;
    for (size_t i = 0; i < n; ++i) {
        result->m_data[i] = r;
        result->m_data[n + i] = 30;
        result->m_data[2 * n + i] = 90;
        result->m_data[3 * n + i] = 255;
    }
    return result;
}
} // namespace
class KisQmicIntegrationTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testPlanarReference()
    {
        auto *registry = KoColorSpaceRegistry::instance();
        checkPlanarConversion<quint8, KoBgrU8Traits>(registry->rgb8());
        checkPlanarConversion<quint16, KoBgrU16Traits>(registry->colorSpace("RGBA", "U16", QString()));
#ifdef HAVE_OPENEXR
        checkPlanarConversion<half, KoRgbF16Traits>(registry->colorSpace("RGBA", "F16", QString()));
#endif
        checkPlanarConversion<float, KoRgbF32Traits>(registry->colorSpace("RGBA", "F32", QString()));
    }
    void benchmarkTransfer()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_GMIC_TRANSFER"))
            QSKIP("Opt-in transfer benchmark");
        for (const auto &depth : {"U8", "U16", "F16", "F32"}) {
            const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
            QVERIFY(cs);
            for (int width : {2048, 4096}) {
                const QRect rect(-19, 11, width, width);
                KisPaintDeviceSP src = new KisPaintDevice(cs);
                src->fill(rect, KoColor(QColor(41, 133, 217, 173), cs));
                auto buffer = KisQMicImageSP::create("", width, width, 4);
                KisPaintDeviceSP dst = new KisPaintDevice(cs);
                QList<double> input, output;
                for (int trial = 0; trial < 4; ++trial) {
                    QElapsedTimer timer;
                    timer.start();
                    KisQmicSimpleConvertor::convertToGmicImageFast(src, *buffer, rect);
                    const double in = timer.nsecsElapsed() / 1e6;
                    timer.restart();
                    KisQmicSimpleConvertor::convertFromGmicFast(*buffer, dst, 255.0f);
                    const double out = timer.nsecsElapsed() / 1e6;
                    if (trial) {
                        input << in;
                        output << out;
                    }
                }
                std::sort(input.begin(), input.end());
                std::sort(output.begin(), output.end());
                qInfo() << "TRANSFER" << depth << width << "input_ms" << input[1] << "output_ms" << output[1];
            }
        }
    }
    void testSizes()
    {
        QCOMPARE(KisQMicImage::checkedElementCount(8, 9, 4), size_t(288));
        QVERIFY_THROWS_EXCEPTION(std::length_error, KisQMicImage::checkedElementCount(-1, 1, 4));
        QVERIFY_THROWS_EXCEPTION(std::length_error, KisQMicImage::checkedElementCount(0, 1, 4));
        QVERIFY_THROWS_EXCEPTION(std::length_error, KisQMicImage::checkedElementCount(INT_MAX, INT_MAX, 4));
        QVERIFY_THROWS_EXCEPTION(std::length_error, KisQMicImage::checkedElementCount(2, 2, 5));
    }
    void testRoundTrip_data()
    {
        QTest::addColumn<QString>("depth");
        for (const auto &d : {"U8", "U16", "F16", "F32"})
            QTest::newRow(d) << QString(d);
    }
    void testRoundTrip()
    {
        QFETCH(QString, depth);
        auto *cs = KoColorSpaceRegistry::instance()->colorSpace("RGBA", depth, QString());
        QVERIFY(cs);
        KisPaintDeviceSP dev = new KisPaintDevice(cs);
        const QRect rect(-3, 5, 11, 9);
        dev->fill(rect, KoColor(QColor(37, 130, 240, 173), cs));
        auto buffer = KisQMicImageSP::create("", 11, 9, 4);
        KisQmicSimpleConvertor::convertToGmicImageFast(dev, *buffer, rect);
        KisPaintDeviceSP back = new KisPaintDevice(cs);
        KisQmicSimpleConvertor::convertFromGmicFast(*buffer, back, 255.0f);
        if (depth == "F32") {
            // The planar 0..255 round trip has normal floating-point rounding.
            auto before = pixels(dev, rect), after = pixels(back, QRect(0, 0, 11, 9));
            const auto *a = reinterpret_cast<const float *>(before.constData());
            const auto *b = reinterpret_cast<const float *>(after.constData());
            for (int i = 0; i < before.size() / int(sizeof(float)); ++i)
                QVERIFY(qAbs(a[i] - b[i]) < 2e-7f);
        } else {
            QCOMPARE(pixels(dev, rect), pixels(back, QRect(0, 0, 11, 9)));
        }
        if (depth == "F32") {
            buffer->m_data[0] = -127.5f;
            buffer->m_data[99] = 510.0f;
            KisQmicSimpleConvertor::convertFromGmicFast(*buffer, back, 255.0f);
            auto again = KisQMicImageSP::create("", 11, 9, 4);
            KisQmicSimpleConvertor::convertToGmicImageFast(back, *again, QRect(0, 0, 11, 9));
            QCOMPARE(again->m_data[0], -127.5f);
            QCOMPARE(again->m_data[99], 510.0f);
        }
    }
    void testLayerSynchronization()
    {
        KisImageSP image =
            new KisImage(new KisSurrogateUndoStore, 32, 24, KoColorSpaceRegistry::instance()->rgb8(), "qmic");
        KisPaintLayerSP layer = new KisPaintLayer(image, "original", 123);
        image->addNode(layer, image->root());
        layer->setX(-4);
        layer->setY(3);
        layer->paintDevice()->fill(QRect(-4, 3, 20, 10), KoColor(Qt::green, image->colorSpace()));
        const QRect bounds(-10, -10, 60, 60);
        const auto before = pixels(layer->paintDevice(), bounds);
        auto nodes = KisNodeListSP(new KisNodeList{layer});
        QVector<KisQMicImageSP> output{solid("name(result),pos(-2,5),opacity(70),mode(normal)", 16, 12, 200),
                                       solid("name(extra),pos(1,2)", 8, 6, 80)};
        KisQmicSynchronizeLayersCommand command(nodes, output, image, image->bounds());
        command.redo();
        image->waitForDone();
        QCOMPARE(image->root()->childCount(), 2U);
        QCOMPARE(layer->name(), QString("result"));
        QCOMPARE(layer->x(), -2);
        QCOMPARE(layer->y(), 5);
        const auto after = pixels(layer->paintDevice(), bounds);
        QVERIFY(after != before);
        for (int i = 0; i < 3; ++i) {
            command.undo();
            image->waitForDone();
            QCOMPARE(image->root()->childCount(), 1U);
            QCOMPARE(layer->name(), QString("original"));
            QCOMPARE(layer->opacity(), quint8(123));
            QCOMPARE(layer->x(), -4);
            QCOMPARE(layer->y(), 3);
            QCOMPARE(pixels(layer->paintDevice(), bounds), before);
            command.redo();
            image->waitForDone();
            QCOMPARE(image->root()->childCount(), 2U);
            QCOMPARE(pixels(layer->paintDevice(), bounds), after);
        }
        auto visible = KisQmicImportTools::inputNodes(image, InputLayerMode::AllVisible, layer);
        QCOMPARE(visible->size(), 2);
        layer->setVisible(false);
        QCOMPARE(KisQmicImportTools::inputNodes(image, InputLayerMode::AllInvisible, layer)->size(), 1);
    }
    void testSelectionAndLayerRemoval()
    {
        KisImageSP image =
            new KisImage(new KisSurrogateUndoStore, 24, 20, KoColorSpaceRegistry::instance()->rgb8(), "selection");
        KisPaintLayerSP layer = new KisPaintLayer(image, "keep", 255);
        KisPaintLayerSP extra = new KisPaintLayer(image, "remove", 255);
        image->addNode(layer, image->root());
        image->addNode(extra, image->root());
        layer->paintDevice()->fill(image->bounds(), KoColor(QColor(40, 30, 90), image->colorSpace()));
        extra->paintDevice()->fill(image->bounds(), KoColor(Qt::red, image->colorSpace()));
        KisSelectionSP selection = new KisSelection;
        selection->pixelSelection()->select(QRect(5, 4, 6, 8), 128);
        selection->pixelSelection()->select(QRect(11, 4, 6, 8), 255);
        KisSelectionMaskSP mask = new KisSelectionMask(image);
        mask->setSelection(selection);
        image->addNode(mask, layer);
        mask->setActive(true);
        const auto before = pixels(layer->paintDevice(), image->bounds());
        auto nodes = KisNodeListSP(new KisNodeList{layer, extra});
        KisQmicSynchronizeLayersCommand command(nodes,
                                                {solid("name(keep)", 12, 8, 200)},
                                                image,
                                                QRect(5, 4, 12, 8),
                                                selection);
        command.redo();
        image->waitForDone();
        QCOMPARE(image->root()->childCount(), 1U);
        KoColor pixel(image->colorSpace());
        layer->paintDevice()->pixel(2, 2, &pixel);
        QCOMPARE(pixel.toQColor().red(), 40);
        layer->paintDevice()->pixel(7, 6, &pixel);
        QVERIFY(qAbs(pixel.toQColor().red() - 120) <= 1);
        layer->paintDevice()->pixel(13, 6, &pixel);
        QCOMPARE(pixel.toQColor().red(), 200);
        const auto after = pixels(layer->paintDevice(), image->bounds());
        for (int i = 0; i < 3; ++i) {
            command.undo();
            image->waitForDone();
            QCOMPARE(image->root()->childCount(), 2U);
            QCOMPARE(pixels(layer->paintDevice(), image->bounds()), before);
            command.redo();
            image->waitForDone();
            QCOMPARE(image->root()->childCount(), 1U);
            QCOMPARE(pixels(layer->paintDevice(), image->bounds()), after);
        }
    }
    void testGrayFallback()
    {
        const auto *cs = KoColorSpaceRegistry::instance()->colorSpace("GRAYA", "U16", QString());
        QVERIFY(cs);
        KisPaintDeviceSP dev = new KisPaintDevice(cs);
        dev->moveTo(-4, 6);
        dev->fill(QRect(-4, 6, 30, 30), KoColor(Qt::white, cs));
        auto buffer = KisQMicImageSP::create("", 6, 5, 2);
        std::fill(buffer->m_data, buffer->m_data + 30, 100.0f);
        std::fill(buffer->m_data + 30, buffer->m_data + 60, 127.0f);
        KisQmicSimpleConvertor::convertFromGmicFast(*buffer, dev, 255.0f);
        QCOMPARE(dev->x(), 0);
        QCOMPARE(dev->y(), 0);
        QCOMPARE(dev->exactBounds(), QRect(0, 0, 6, 5));
        auto back = KisQMicImageSP::create("", 6, 5, 4);
        KisQmicSimpleConvertor::convertToGmicImageFast(dev, *back, QRect(0, 0, 6, 5));
        for (int i = 0; i < 30; ++i) {
            QVERIFY(qAbs(back->m_data[i] - 100.0f) < 1.0f);
            QVERIFY(qAbs(back->m_data[90 + i] - 127.0f) < 1.0f);
        }
    }
    void testInstalledRuntime()
    {
        if (!qEnvironmentVariableIsSet("SOLSTICE_TEST_GMIC_RUNTIME"))
            QSKIP("Opt-in installed runtime test");
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        if (QGuiApplication::platformName() == "offscreen")
            QApplication::clipboard()->setMimeData(new QMimeData);
        KisSynchronizedConnectionBase::setAutoModeForUnittestsEnabled(true);
        QTimer timeout;
        connect(&timeout, &QTimer::timeout, this, [] {
            qFatal("G'MIC integration test timed out");
        });
        timeout.start(60000);
        // The minimal test resource installation can report missing bundles.
        // Dismiss that host-only warning before closing its parent window.
        QTimer hostWarnings;
        connect(&hostWarnings, &QTimer::timeout, this, [] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (box && qobject_cast<KisMainWindow *>(box->parentWidget()))
                    box->accept();
            }
        });
        hostWarnings.start(50);
        qputenv("GMIC_PATH", profile.path().toUtf8());
        qputenv("SOLSTICE_GMIC_SETTINGS", (profile.path() + "/gmic.ini").toUtf8());
        const auto offers = KoJsonTrader::instance()->query("Krita/GMic", QString());
        QVERIFY(!offers.isEmpty());
        auto *factory = qobject_cast<KPluginFactory *>(offers.first().instance());
        QVERIFY(factory);
        std::unique_ptr<QObject> object(factory->create<QObject>());
        auto *plugin = qobject_cast<KisQmicPluginInterface *>(object.get());
        QVERIFY(plugin);
        auto *doc = KisPart::instance()->createDocument();
        KisConfig(true).disableOpenGL();
        KisImageSP image =
            new KisImage(doc->createUndoStore(), 64, 48, KoColorSpaceRegistry::instance()->rgb8(), "runtime");
        doc->setCurrentImage(image);
        auto *window = KisPart::instance()->createMainWindow();
        auto *view = new KisView(doc, window->viewManager(), window);
        KisPart::instance()->addView(view);
        window->showView(view);
        auto *manager = window->viewManager();
        manager->setCurrentView(view);
        KisPaintLayerSP layer = new KisPaintLayer(image, "input", 255);
        image->addNode(layer, image->root());
        layer->paintDevice()->fill(image->bounds(), KoColor(QColor(40, 80, 120), image->colorSpace()));
        manager->nodeManager()->slotUiActivatedNode(layer);
        image->waitForDone();
        auto iface = std::make_shared<KisImageInterface>(manager);
        QSignalSpy applied(iface.get(), &KisImageInterface::outputApplied);
        const auto before = pixels(layer->paintDevice(), image->bounds());
        iface->gmic_qt_output_images(int(OutputMode::NewImage), {solid("", 64, 48, 200)});
        iface->gmic_qt_output_images(int(OutputMode::InPlace), {});
        QCOMPARE(applied.count(), 0);
        QCOMPARE(pixels(layer->paintDevice(), image->bounds()), before);
        {
            QSettings settings(QString::fromUtf8(qgetenv("SOLSTICE_GMIC_SETTINGS")), QSettings::IniFormat);
            settings.setValue("Config/PreviewEnabled", false);
            settings.setValue("LastExecution/host_krita-plugin/Command", "add 10");
            settings.setValue("LastExecution/host_krita-plugin/InputMode", 1);
            settings.setValue("LastExecution/host_krita-plugin/OutputMode", 0);
            settings.sync();
        }
        QCOMPARE(plugin->launch(iface, true), 0);
        image->waitForDone();
        QCOMPARE(applied.count(), 1);
        const auto after = pixels(layer->paintDevice(), image->bounds());
        QVERIFY(after != before);
        KoColor actual(image->colorSpace());
        layer->paintDevice()->pixel(3, 3, &actual);
        QCOMPARE(actual.toQColor(), QColor(50, 90, 130));
        doc->undoStack()->undo();
        image->waitForDone();
        QCOMPARE(pixels(layer->paintDevice(), image->bounds()), before);
        doc->undoStack()->redo();
        image->waitForDone();
        QCOMPARE(pixels(layer->paintDevice(), image->bounds()), after);
        // The native bridge must copy the first z slice of each channel,
        // including volumes whose channel stride exceeds a single plane.
        for (const int depth : {1, 3}) {
            for (int channels = 1; channels <= 4; ++channels) {
                QSettings settings(QString::fromUtf8(qgetenv("SOLSTICE_GMIC_SETTINGS")), QSettings::IniFormat);
                settings.setValue("LastExecution/host_krita-plugin/Command",
                                  QString("resize 7,5,%1,%2 fill x+2*y+20*z+40*c").arg(depth).arg(channels));
                settings.remove("LastExecution/host_krita-plugin/Arguments");
                settings.sync();
                QCOMPARE(plugin->launch(iface, true), 0);
                image->waitForDone();
                layer->paintDevice()->pixel(3, 2, &actual);
                QCOMPARE(actual.toQColor(),
                         QColor(7,
                                channels < 3 ? 7 : 47,
                                channels < 3 ? 7 : 87,
                                channels == 2       ? 47
                                    : channels == 4 ? 127
                                                    : 255));
                doc->undoStack()->undo();
                image->waitForDone();
                QCOMPARE(pixels(layer->paintDevice(), image->bounds()), after);
            }
        }
        // The synthetic engine command has no matching catalog filter.
        // Launch the dialog as a fresh session rather than restoring it.
        QSettings(QString::fromUtf8(qgetenv("SOLSTICE_GMIC_SETTINGS")), QSettings::IniFormat)
            .remove("LastExecution/host_krita-plugin");
        // Exercise the real dialog and offline catalog without changing pixels.
        bool sawDialog = false;
        QTimer closer;
        connect(&closer, &QTimer::timeout, this, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (QString::fromLatin1(widget->metaObject()->className()) == "GmicQt::MainWindow") {
                    sawDialog = true;
                    if (QApplication::activeModalWidget() != widget)
                        continue;
                    const QString artifacts = qEnvironmentVariable("SOLSTICE_GMIC_ARTIFACT_DIR");
                    if (!artifacts.isEmpty())
                        widget->grab().save(artifacts + "/gmic-dialog.png");
                    widget->close();
                }
            }
        });
        closer.start(2000);
        QCOMPARE(plugin->launch(iface, false), 0);
        closer.stop();
        QVERIFY(sawDialog);
        image->waitForDone();
        QCOMPARE(pixels(layer->paintDevice(), image->bounds()), after);
        auto *startAction = manager->actionManager()->actionByName("QMic");
        auto *againAction = manager->actionManager()->actionByName("QMicAgain");
        QVERIFY(startAction);
        QVERIFY(againAction);
        manager->actionManager()->updateGUI();
        QVERIFY(startAction->isEnabled());
        QVERIFY(!againAction->isEnabled());
        // Launch/cancel through the actual installed menu plugin, twice.
        for (int i = 0; i < 2; ++i) {
            sawDialog = false;
            closer.start(1500);
            startAction->trigger();
            closer.stop();
            QVERIFY(sawDialog);
            QVERIFY(startAction->isEnabled());
            QVERIFY(!againAction->isEnabled());
            QCOMPARE(pixels(layer->paintDevice(), image->bounds()), after);
        }
#ifdef Q_OS_WIN
        if (qEnvironmentVariableIsSet("SOLSTICE_BENCHMARK_GMIC")) {
            layer->paintDevice()->fill(QRect(0, 0, 2048, 2048), KoColor(QColor(40, 80, 120), image->colorSpace()));
            QSettings settings(QString::fromUtf8(qgetenv("SOLSTICE_GMIC_SETTINGS")), QSettings::IniFormat);
            settings.setValue("LastExecution/host_krita-plugin/Command", "repeat 8 blur 20 done");
            settings.setValue("LastExecution/host_krita-plugin/InputMode", 1);
            settings.setValue("LastExecution/host_krita-plugin/OutputMode", 0);
            settings.sync();
            const auto cpuTime = [] {
                FILETIME creation, exit, kernel, user;
                GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
                return (quint64(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime
                    + (quint64(user.dwHighDateTime) << 32) + user.dwLowDateTime;
            };
            const auto cpuStart = cpuTime();
            QElapsedTimer elapsed;
            elapsed.start();
            QCOMPARE(plugin->launch(iface, true), 0);
            image->waitForDone();
            const double wallMs = elapsed.nsecsElapsed() / 1e6;
            const double cpuMs = (cpuTime() - cpuStart) / 1e4;
            qInfo() << "G'MIC CPU benchmark: 2048x2048 RGBA, 8x blur 20; wall ms" << wallMs << "process CPU ms" << cpuMs
                    << "CPU/wall" << cpuMs / wallMs;
        }
#endif
        // Merge removes the originally active input. A second request in the
        // same dialog must read the retained layer, not its detached sibling.
        KisPaintLayerSP extra = new KisPaintLayer(image, "merged away", 255);
        image->addNode(extra, image->root());
        extra->paintDevice()->fill(image->bounds(), KoColor(Qt::red, image->colorSpace()));
        auto all = KisQmicImportTools::inputNodes(image, InputLayerMode::All, layer);
        manager->nodeManager()->slotUiActivatedNode(all->last());
        auto mergeInterface = std::make_shared<KisImageInterface>(manager);
        QRectF crop(0, 0, 1, 1);
        QCOMPARE(mergeInterface->gmic_qt_get_cropped_images(int(InputLayerMode::All), crop).size(), 2);
        mergeInterface->gmic_qt_detach();
        mergeInterface->gmic_qt_output_images(int(OutputMode::InPlace), {solid("name(merged)", 16, 16, 33)});
        image->waitForDone();
        QCOMPARE(image->root()->childCount(), 1U);
        const auto retained = mergeInterface->gmic_qt_get_cropped_images(int(InputLayerMode::Active), crop);
        QCOMPARE(retained.size(), 1);
        QCOMPARE(retained.first()->m_data[0], 33.0f);
        doc->setModified(false);
        window->close();
    }
};
int main(int argc, char **argv)
{
    QTemporaryDir profile;
    qputenv("SOLSTICE_PROFILE_ROOT", profile.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    if (!qEnvironmentVariableIsSet("EXTRA_RESOURCE_DIRS"))
        qputenv("EXTRA_RESOURCE_DIRS", QByteArray(KRITA_RESOURCE_DIRS_FOR_TESTS));
    QTemporaryDir plugins;
    const QString pluginSource = qEnvironmentVariable("SOLSTICE_GMIC_TEST_PLUGIN_DIR", KRITA_PLUGINS_DIR_FOR_TESTS);
    // Isolate this integration fixture from unrelated Python/ML/dock plugins.
    for (const auto &file : QDir(pluginSource).entryInfoList(QDir::Files)) {
        const QString name = file.completeBaseName();
        if (QStringList{"kritalcmsengine",
                        "kritadefaultpaintops",
                        "kritadefaulttools",
                        "krita_shape_paths",
                        "krita_shape_image",
                        "krita_gmic_qt",
                        "kritaqmic"}
                .contains(name)) {
            QFile::copy(file.absoluteFilePath(), plugins.path() + QLatin1Char('/') + file.fileName());
        }
    }
    qputenv("KRITA_PLUGIN_PATH", plugins.path().toUtf8());
    qputenv("LANGUAGE", "en");
    KConfig::setMainConfigName(KisSolsticePaths::mainConfigName());
    kisTestSetupFontconfig();
    // Application components parse process arguments; QtTest's -o/-v options
    // belong only to QTest::qExec, not Solstice's command-line parser.
    int appArgc = 1;
    char *appArgv[] = {argv[0], nullptr};
    KisApplication app("solstice-gmic-integration", appArgc, appArgv);
    app.setQuitOnLastWindowClosed(false);
    registerResources();
    KisQmicIntegrationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "KisQmicIntegrationTest.moc"
