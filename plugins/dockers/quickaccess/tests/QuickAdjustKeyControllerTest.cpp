/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "QuickAdjustKeyController.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMimeData>
#include <QScopedPointer>
#include <QTest>

#include <KisApplication.h>
#include <KisGlobalResourcesInterface.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisViewManager.h>
#include <kconfiggroup.h>
#include <kis_canvas_resource_provider.h>
#include <opengl/kis_opengl.h>
#include <ksharedconfig.h>
#include <testui.h>

class QuickAdjustKeyControllerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testWindowOwnership_data();
    void testWindowOwnership();
};

void QuickAdjustKeyControllerTest::testWindowOwnership_data()
{
    QTest::addColumn<bool>("alphaLock");
    QTest::newRow("eraser") << false;
    QTest::newRow("alpha-lock") << true;
}

void QuickAdjustKeyControllerTest::testWindowOwnership()
{
    QFETCH(bool, alphaLock);
    // The offscreen platform starts without any clipboard MIME data.
    if (QApplication::platformName() == QStringLiteral("offscreen")) {
        QApplication::clipboard()->setMimeData(new QMimeData);
    }
    KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    config.writeEntry("AltEraseKey", alphaLock ? QString() : QStringLiteral("F10"));
    config.writeEntry("PreserveAlphaKey", alphaLock ? QStringLiteral("F10") : QString());

    QScopedPointer<KisMainWindow> first(KisPart::instance()->createMainWindow());
    QScopedPointer<KisMainWindow> second(KisPart::instance()->createMainWindow());
    // Main-window construction may load installed docker plugins. Exercise
    // exactly one freshly compiled controller per window, not those copies.
    for (KisMainWindow *window : {first.data(), second.data()}) {
        for (QObject *child : window->findChildren<QObject *>()) {
            if (child->inherits("QuickAdjustKeyController")) {
                qApp->removeEventFilter(child);
            }
        }
    }
    auto *firstOwner = new QObject(first.data());
    auto *secondOwner = new QObject(second.data());
    new QuickAdjustKeyController(firstOwner);
    new QuickAdjustKeyController(secondOwner);
    auto *firstProvider = first->viewManager()->canvasResourceProvider();
    auto *secondProvider = second->viewManager()->canvasResourceProvider();
    // Eraser mode is derived from the current preset, not a standalone bool.
    const QString presetPath = QFINDTESTDATA("../../../../libs/ui/tests/data/autobrush_300px.kpp");
    QVERIFY(!presetPath.isEmpty());
    for (auto *provider : {firstProvider, secondProvider}) {
        KisPaintOpPresetSP preset(new KisPaintOpPreset(presetPath));
        QVERIFY(preset->load(KisGlobalResourcesInterface::instance()));
        provider->setPaintOpPreset(preset);
    }
    auto set = [alphaLock](KisCanvasResourceProvider *provider, bool value) {
        if (alphaLock)
            provider->setGlobalAlphaLock(value);
        else
            provider->setEraserMode(value);
    };
    auto get = [alphaLock](KisCanvasResourceProvider *provider) {
        return alphaLock ? provider->globalAlphaLock() : provider->eraserMode();
    };
    auto key = [](QObject *target, QEvent::Type type) {
        QKeyEvent event(type, Qt::Key_F10, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    };

    QApplication::setActiveWindow(first.data());
    QCOMPARE(KisPart::instance()->currentMainwindow(), first.data());
    set(firstProvider, false);
    set(secondProvider, false);
    key(firstOwner, QEvent::KeyPress);
    QVERIFY(get(firstProvider));
    QVERIFY(!get(secondProvider));
    key(firstOwner, QEvent::KeyRelease);
    QVERIFY(!get(firstProvider));
    QVERIFY(!get(secondProvider));

    // Releasing in another window must never restore into that window.
    key(firstOwner, QEvent::KeyPress);
    QApplication::setActiveWindow(second.data());
    key(secondOwner, QEvent::KeyRelease);
    QVERIFY(!get(firstProvider));
    QVERIFY(!get(secondProvider));

    // Losing application focus can swallow the physical key release.
    key(secondOwner, QEvent::KeyPress);
    QVERIFY(get(secondProvider));
    QEvent deactivate(QEvent::ApplicationDeactivate);
    QApplication::sendEvent(qApp, &deactivate);
    QVERIFY(!get(secondProvider));

    // Destroying the owning docker during a hold restores a live provider.
    QApplication::setActiveWindow(first.data());
    key(firstOwner, QEvent::KeyPress);
    QVERIFY(get(firstProvider));
    delete firstOwner;
    QVERIFY(!get(firstProvider));

    // A window destroys its view manager before its dockers/controllers.
    QApplication::setActiveWindow(second.data());
    key(secondOwner, QEvent::KeyPress);
    QVERIFY(get(secondProvider));
    second.reset();
}

int main(int argc, char **argv)
{
    qputenv("LANGUAGE", "en");
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    QStandardPaths::setTestModeEnabled(true);
    qputenv("EXTRA_RESOURCE_DIRS", QByteArray(KRITA_RESOURCE_DIRS_FOR_TESTS));
    qputenv("KRITA_PLUGIN_PATH", QByteArray(KRITA_PLUGINS_DIR_FOR_TESTS));
    kisTestSetupFontconfig();
    // KisInputManager requires KisApplication's modifier mapper, so the
    // QApplication created by KISTEST_MAIN cannot host real main windows.
    KisOpenGL::testingInitializeDefaultSurfaceFormat();
    KisApplication app(QStringLiteral("QuickAdjustKeyControllerTest"), argc, argv);
    registerResources();
    QuickAdjustKeyControllerTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "QuickAdjustKeyControllerTest.moc"
