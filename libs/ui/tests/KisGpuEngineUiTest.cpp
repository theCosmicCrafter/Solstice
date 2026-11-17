/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QMessageBox>
#include <QScopeGuard>
#include <QTimer>

#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorProfile.h>
#include <KoColorSpaceRegistry.h>
#include <kundo2stack.h>

#include "KisDocument.h"
#include "KisGpuEngineUi.h"
#ifdef HAVE_KRITA_GPU_ENGINE
#include "gpu/KisGpuTileBackend.h"
#include <KisGpuContext.h>
#endif
#include "KisPart.h"
#include "gpu/KisGpuMergeBatch.h"
#include "kis_image.h"
#include "kis_image_config.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include <testui.h>

class KisGpuEngineUiTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testUnverifiedDeviceStatus();
    void testPreferences();
    void testOpening_data();
    void testOpening();
};

void KisGpuEngineUiTest::testUnverifiedDeviceStatus()
{
    // Runs first: the settings have to name a graphics card the engine was
    // not verified with before any document has tried to start the engine.
#ifdef HAVE_KRITA_GPU_ENGINE
    if (KisGpuTileBackend::existingInstance()) {
        QSKIP("the GPU engine was started already");
    }
    const KisGpuDeviceInfo device = KisGpuContext::probeDevice();
    if (device.name.isEmpty()) {
        QSKIP("no usable GPU");
    }
    const bool oldUnverified = KisGpuEngineSettings::unverifiedDeviceAllowedInConfig();
    auto restore = qScopeGuard([&] {
        KisGpuEngineSettings::setUnverifiedDeviceAllowedInConfig(oldUnverified);
        qunsetenv("KRITA_GPU_TREAT_DEVICE_AS_UNVERIFIED");
    });
    auto statusText = [] {
        KisGpuEngineSettingsWidget widget;
        return widget.findChild<QLabel *>(QStringLiteral("gpuEngineStatus"))->text();
    };
    KisGpuMergeBatch::setEnabled(true);
    KisGpuEngineSettings::setUnverifiedDeviceAllowedInConfig(false);
    qputenv("KRITA_GPU_TREAT_DEVICE_AS_UNVERIFIED", "1");
    QCOMPARE(KisGpuTileBackend::unverifiedDeviceName(), device.name);
    QVERIFY2(statusText().startsWith(QStringLiteral("Not used:")), qPrintable(statusText()));
    QVERIFY2(statusText().contains(device.name), qPrintable(statusText()));

    // allowed by the user: nothing to report
    KisGpuEngineSettings::setUnverifiedDeviceAllowedInConfig(true);
    QVERIFY(KisGpuTileBackend::unverifiedDeviceName().isEmpty());
    QVERIFY2(statusText().startsWith(QStringLiteral("Enabled")), qPrintable(statusText()));

    // asking did not start the engine
    QVERIFY(!KisGpuTileBackend::existingInstance());
#else
    QSKIP("built without the GPU engine");
#endif
}

void KisGpuEngineUiTest::testPreferences()
{
    const auto oldPolicy = KisGpuEngineSettings::convertPolicy();
    const bool oldEnabled = KisGpuEngineSettings::enabledInConfig();
    const bool oldBrush = KisGpuEngineSettings::brushEnabledInConfig();
    const bool oldUnverified = KisGpuEngineSettings::unverifiedDeviceAllowedInConfig();
    auto restore = qScopeGuard([&] {
        KisGpuEngineSettings::setConvertPolicy(oldPolicy);
        KisGpuEngineSettings::setEnabledInConfig(oldEnabled);
        KisGpuEngineSettings::setBrushEnabledInConfig(oldBrush);
        KisGpuEngineSettings::setUnverifiedDeviceAllowedInConfig(oldUnverified);
    });
    KisGpuEngineSettingsWidget widget;

    // Graphics cards the engine was not verified with: off unless the user
    // checks the option, and off again with the defaults.
    auto *unverified = widget.findChild<QCheckBox *>(QStringLiteral("gpuEngineUnverifiedDevice"));
    QVERIFY(unverified);
    for (bool allowed : {true, false}) {
        KisGpuEngineSettings::setUnverifiedDeviceAllowedInConfig(allowed);
        widget.load(false);
        QCOMPARE(unverified->isChecked(), allowed);
        unverified->setChecked(!allowed);
        widget.save();
        QCOMPARE(KisGpuEngineSettings::unverifiedDeviceAllowedInConfig(), !allowed);
        widget.load(true);
        QVERIFY(!unverified->isChecked());
    }
    // the option belongs to the engine switch
    for (bool enabled : {false, true}) {
        KisGpuEngineSettings::setEnabledInConfig(enabled);
        widget.load(false);
        QCOMPARE(unverified->isEnabled(), enabled);
    }
    KisGpuEngineSettings::setUnverifiedDeviceAllowedInConfig(false);
    QVERIFY(!KisGpuEngineSettings::unverifiedDeviceAllowed());
    qputenv("KRITA_GPU_ALLOW_UNVERIFIED_DEVICE", "1");
    QVERIFY(KisGpuEngineSettings::unverifiedDeviceAllowed());
    qunsetenv("KRITA_GPU_ALLOW_UNVERIFIED_DEVICE");

    auto *combo = widget.findChild<QComboBox *>();
    QVERIFY(combo);
    // Check persisted numeric values, including legacy Convert=1 and Keep=2.
    for (int value : {0, 1, 2, 3}) {
        KisImageConfig(false).writeEntry<int>(KisGpuEngineSettings::convertPolicyKey(), value);
        widget.load(false);
        QCOMPARE(combo->currentData().toInt(), value);
        widget.load(true);
        QCOMPARE(combo->currentData().toInt(), 0);
        combo->setCurrentIndex(combo->findData(value));
        widget.save();
        QCOMPARE(int(KisGpuEngineSettings::convertPolicy()), value);
    }
    KisImageConfig(false).writeEntry<int>(KisGpuEngineSettings::convertPolicyKey(), 99);
    widget.load(false);
    QCOMPARE(combo->currentData().toInt(), 0);
}

void KisGpuEngineUiTest::testOpening_data()
{
    QTest::addColumn<int>("choice");
    QTest::addColumn<bool>("ask");
    QTest::addColumn<bool>("remember");
    for (int choice : {1, 2, 3}) {
        QTest::newRow(qPrintable(QString("automatic-%1").arg(choice))) << choice << false << false;
        for (bool remember : {false, true}) {
            QTest::newRow(qPrintable(QString("ask-%1-remember-%2").arg(choice).arg(remember)))
                << choice << true << remember;
        }
    }
    QTest::newRow("escape-keeps") << -1 << true << false;
}

void KisGpuEngineUiTest::testOpening()
{
    QFETCH(int, choice);
    QFETCH(bool, ask);
    QFETCH(bool, remember);
    const auto oldPolicy = KisGpuEngineSettings::convertPolicy();
    auto restore = qScopeGuard([&] {
        KisGpuEngineSettings::setConvertPolicy(oldPolicy);
    });
    KisGpuMergeBatch::setEnabled(true);
    const KoColorSpace *source = KoColorSpaceRegistry::instance()->rgb8();
    if (!KisGpuEngineUi::shouldOfferConversion(source)) {
        QSKIP("GPU engine unavailable");
    }
    const auto initial = ask ? KisGpuEngineSettings::Ask : KisGpuEngineSettings::ConvertPolicy(choice);
    KisGpuEngineSettings::setConvertPolicy(initial);
    QScopedPointer<KisDocument> document(KisPart::instance()->createDocument());
    KisImageSP image = new KisImage(document->createUndoStore(), 32, 32, source, "conversion");
    KisPaintLayerSP layer = new KisPaintLayer(image, "paint", OPACITY_OPAQUE_U8, source);
    image->addNode(layer, image->root());
    layer->paintDevice()->fill(image->bounds(), KoColor(Qt::red, source));
    document->setCurrentImage(image);
    image->waitForDone();

    bool dialogSeen = false;
    bool choiceFound = false;
    QTimer responder;
    connect(&responder, &QTimer::timeout, this, [&] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        dialogSeen = true;
        box->checkBox()->setChecked(remember);
        if (choice == -1) {
            choiceFound = true;
            QTest::keyClick(box, Qt::Key_Escape);
            return;
        }
        const QString text = choice == 3 ? QStringLiteral("16-bit") : QStringLiteral("32-bit");
        for (auto *button : box->buttons()) {
            if ((choice == 2 && box->buttonRole(button) == QMessageBox::RejectRole)
                || (choice != 2 && button->text().contains(text))) {
                choiceFound = button->isEnabled();
                if (choiceFound) {
                    button->click();
                } else {
                    box->reject();
                }
                return;
            }
        }
        box->reject();
    });
    responder.start(10);
    KisGpuEngineUi::offerConversion(document.data(), nullptr);
    responder.stop();
    image->waitForDone();
    QCOMPARE(dialogSeen, ask);
    if (ask)
        QVERIFY(choiceFound);
    QCOMPARE(int(KisGpuEngineSettings::convertPolicy()), remember ? choice : int(initial));
    if (choice == 1 || choice == 3) {
        QCOMPARE(image->colorSpace()->colorDepthId(), choice == 3 ? Float16BitsColorDepthID : Float32BitsColorDepthID);
        QCOMPARE(layer->colorSpace()->colorDepthId(), image->colorSpace()->colorDepthId());
        QCOMPARE(image->colorSpace()->profile()->name(), source->profile()->name());
        QVERIFY(!KisGpuEngineUi::shouldOfferConversion(image->colorSpace()));
        const KoColorSpace *converted = image->colorSpace();
        document->undoStack()->undo();
        image->waitForDone();
        QCOMPARE(image->colorSpace(), source);
        document->undoStack()->redo();
        image->waitForDone();
        QCOMPARE(image->colorSpace(), converted);
    } else {
        QCOMPARE(image->colorSpace(), source);
    }
}

KISTEST_MAIN(KisGpuEngineUiTest)
#include "KisGpuEngineUiTest.moc"
