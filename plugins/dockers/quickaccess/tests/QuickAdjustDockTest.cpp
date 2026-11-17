/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAdjustDock.h"
#include "QuickAdjustToolOptionsPad.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <KisAngleSelector.h>
#include <KisGlobalResourcesInterface.h>
#include <kconfiggroup.h>
#include <kis_paintop_preset.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
#include <testui.h>

namespace
{
class Content : public QWidget
{
public:
    QSize hint{90, 60};
    QSize sizeHint() const override
    {
        return hint;
    }
};

struct Fixture {
    QTemporaryDir directory;
    KSharedConfigPtr config{KSharedConfig::openConfig(directory.filePath("test.ini"), KConfig::SimpleConfig)};
    QMainWindow window;
    QDockWidget *owner{new QDockWidget(&window)};
    QToolButton *toggle{new QToolButton(owner)};
    QDockWidget *source{new QDockWidget(&window)};
    Content *content{new Content};
    QuickAdjustToolOptionsPad::Context context{true, &window, true};

    Fixture()
    {
        window.setCentralWidget(new QWidget);
        window.resize(640, 480);
        owner->setWidget(toggle);
        toggle->setCheckable(true);
        source->setObjectName("sharedtooldocker");
        source->setWidget(content);
        window.show();
        owner->setGeometry(240, 80, 120, 130);
        owner->show();
        source->show();
        QCoreApplication::processEvents();
    }

    QuickAdjustToolOptionsPad *create(bool compact = false)
    {
        return new QuickAdjustToolOptionsPad(
            owner,
            toggle,
            compact,
            [this] {
                return context;
            },
            config);
    }

    QWidget *pad() const
    {
        return window.findChild<QWidget *>("quickAccessToolOptionsPad");
    }
};

void deferredDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

void applicationState(Qt::ApplicationState state)
{
    QMetaObject::invokeMethod(qGuiApp,
                              "applicationStateChanged",
                              Qt::DirectConnection,
                              Q_ARG(Qt::ApplicationState, state));
}
} // namespace

class QuickAdjustDockTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void borrowAndRestore_data()
    {
        QTest::addColumn<bool>("originallyVisible");
        QTest::newRow("visible") << true;
        QTest::newRow("hidden") << false;
    }

    void borrowAndRestore()
    {
        QFETCH(bool, originallyVisible);
        Fixture f;
        f.source->setVisible(originallyVisible);
        f.config->group("QuickAccessAdjust").writeEntry("Unrelated", 42);
        auto *controller = f.create();
        f.toggle->setChecked(true);
        controller->ensure();
        QPointer<QWidget> pad = f.pad();
        QVERIFY(pad);
        QCOMPARE(f.content->parentWidget(), pad.data());
        QVERIFY(f.source->widget() != f.content);
        QVERIFY(!f.source->isVisible());
        QVERIFY(!f.source->toggleViewAction()->isEnabled());
        QVERIFY(pad->windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(!pad->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        controller->ensure();
        QCOMPARE(f.pad(), pad.data());
        applicationState(Qt::ApplicationActive);
        QVERIFY(pad->isVisible());
        applicationState(Qt::ApplicationInactive);
        QVERIFY(!pad->isVisible());
        applicationState(Qt::ApplicationActive);
        QVERIFY(pad->isVisible());
        controller->hide();
        QVERIFY(!pad->isVisible());
        QVERIFY(f.config->group("QuickAccessAdjust").readEntry("ToolOptionsStartVisible", false));
        controller->setVisible(false);
        QVERIFY(!f.config->group("QuickAccessAdjust").readEntry("ToolOptionsStartVisible", true));
        QCOMPARE(f.config->group("QuickAccessAdjust").readEntry("Unrelated", 0), 42);
        // Queue a size/position update and destroy its context before delivery.
        controller->setVisible(true);
        delete controller;
        QCOMPARE(f.source->widget(), f.content);
        QCOMPARE(f.content->parentWidget(), f.source);
        QCOMPARE(f.source->isVisible(), originallyVisible);
        QVERIFY(f.source->toggleViewAction()->isEnabled());
        deferredDeletes();
        QVERIFY(pad.isNull());
    }

    void guards_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("compact") << 0;
        QTest::newRow("hidden-toggle") << 1;
        QTest::newRow("no-canvas") << 2;
        QTest::newRow("toolbar-mode") << 3;
        QTest::newRow("missing-docker") << 4;
    }

    void guards()
    {
        QFETCH(int, mode);
        Fixture f;
        if (mode == 1)
            f.toggle->hide();
        if (mode == 2)
            f.context.canvasAvailable = false;
        if (mode == 3)
            f.context.toolOptionsInDocker = false;
        if (mode == 4)
            f.source->setObjectName("other");
        auto *controller = f.create(mode == 0);
        controller->setVisible(true);
        QVERIFY(!f.pad());
        QCOMPARE(f.source->widget(), f.content);
        QVERIFY(f.source->toggleViewAction()->isEnabled());
        if (mode == 3 || mode == 4)
            QVERIFY(f.toggle->isHidden());
        // Existing startup persistence applies even when no pad can be opened.
        QVERIFY(f.config->group("QuickAccessAdjust").readEntry("ToolOptionsStartVisible", false));
        delete controller;
    }

    void geometryAndCanvasChange()
    {
        Fixture f;
        auto *scroll = new QScrollArea;
        scroll->setWidget(f.content);
        f.source->setWidget(scroll);
        auto *controller = f.create();
        controller->ensure();
        QWidget *pad = f.pad();
        QVERIFY(pad);
        const QSize small = pad->size();
        f.content->hint = QSize(2000, 2000);
        controller->resizeToContent();
        QVERIFY(pad->width() > small.width());
        QVERIFY(pad->height() > small.height());
        QVERIFY(pad->width() <= f.window.centralWidget()->width() - 8);
        QVERIFY(pad->height() <= f.window.centralWidget()->height() - 8);
        f.content->hint = QSize(90, 60);
        controller->resizeToContent();
        QCOMPARE(pad->size(), small);
        const QPoint origin = f.owner->mapToGlobal(QPoint());
        controller->position();
        QCOMPARE(pad->pos(), origin - QPoint(pad->width() + 5, 0));
        auto config = f.config->group("QuickAccessAdjust");
        config.writeEntry("ToolOptionsPosition", "right_align_top");
        controller->position();
        QCOMPARE(pad->pos(), origin + QPoint(f.owner->width() + 5, 0));
        config.writeEntry("ToolOptionsPosition", "bottom_left");
        controller->position();
        QCOMPARE(pad->pos(), origin + QPoint(0, f.owner->height() + 5));
        // Losing/switching canvas does not relinquish the original borrowed widget.
        f.context.canvasAvailable = false;
        controller->ensure();
        controller->resizeToContent();
        QCOMPARE(f.pad(), pad);
        QCOMPARE(scroll->parentWidget(), pad);
        delete controller;
        QCOMPARE(f.source->widget(), scroll);
        deferredDeletes();
    }

    void destroyedSourceAndOwner()
    {
        Fixture f;
        auto *controller = f.create();
        controller->ensure();
        QPointer<QWidget> content = f.content;
        QPointer<QWidget> pad = f.pad();
        delete f.source;
        f.source = nullptr;
        delete controller;
        deferredDeletes();
        QVERIFY(pad.isNull());
        QVERIFY(content.isNull());

        Fixture second;
        auto *ownedController = second.create();
        ownedController->ensure();
        QPointer<QObject> controllerGuard = ownedController;
        delete second.owner;
        second.owner = nullptr;
        QVERIFY(controllerGuard.isNull());
        QCOMPARE(second.source->widget(), second.content);
        deferredDeletes();
    }

    void dockLayoutAndBrushHistory()
    {
        auto config = KSharedConfig::openConfig()->group("QuickAccessAdjust");
        config.writeEntry("BrushHistoryTotal", 2);
        config.writeEntry("ColorHistoryTotal", 3);
        config.writeEntry("SizeSliderEnabled", false);
        config.writeEntry("ToolOptionsEnabled", false);
        QuickAdjustDock dock;
        dock.show();
        int controllers = 0;
        for (QObject *child : dock.children())
            controllers += child->inherits("QuickAdjustKeyController");
        QCOMPARE(controllers, 1);
        QVERIFY(dock.findChild<KisAngleSelector *>()->parentWidget()->isHidden());
        QVERIFY(dock.findChild<KisDoubleSliderSpinBox *>()->isHidden());
        QVERIFY(!dock.widget()->isEnabled());

        const QString path = QFINDTESTDATA("../../../../libs/ui/tests/data/autobrush_300px.kpp");
        QVERIFY(!path.isEmpty());
        auto addPreset = [&](const QString &name) {
            KisPaintOpPresetSP preset(new KisPaintOpPreset(path));
            if (!preset->load(KisGlobalResourcesInterface::instance()))
                return false;
            preset->setName(name);
            return QMetaObject::invokeMethod(&dock,
                                             "slotBrushPresetChanged",
                                             Qt::DirectConnection,
                                             Q_ARG(KisPaintOpPresetSP, preset));
        };
        QVERIFY(addPreset("A"));
        QVERIFY(addPreset("B"));
        QVERIFY(addPreset("A"));
        auto history = [&] {
            QStringList result;
            for (auto *button : dock.findChildren<QToolButton *>()) {
                if (button->toolTip().startsWith("Brush: "))
                    result << button->toolTip();
            }
            return result;
        };
        QCOMPARE(history(), QStringList({"Brush: A", "Brush: B"}));
        QVERIFY(addPreset("C"));
        QCOMPARE(history(), QStringList({"Brush: C", "Brush: A"}));
        dock.unsetCanvas();
        QCOMPARE(history(), QStringList({"Brush: C", "Brush: A"}));

        QuickAdjustDock popup(nullptr, true);
        popup.show();
        for (QObject *child : popup.children())
            QVERIFY(!child->inherits("QuickAdjustKeyController"));
        QVERIFY(!popup.findChild<KisAngleSelector *>()->parentWidget()->isHidden());
        QVERIFY(!popup.findChild<QWidget *>("quickAccessToolOptionsPad"));
    }

    void syncTimerRunsOnlyWhileShown()
    {
        QuickAdjustDock dock;
        QTimer *timer = dock.findChild<QTimer *>("quickAdjustSyncTimer");
        QVERIFY(timer);
        QCOMPARE(timer->interval(), 200);
        QVERIFY(!timer->isActive());
        dock.show();
        QVERIFY(timer->isActive());
        dock.hide();
        QVERIFY(!timer->isActive());
        dock.show();
        QVERIFY(timer->isActive());

        QuickAdjustDock popup(nullptr, true);
        QTimer *popupTimer = popup.findChild<QTimer *>("quickAdjustSyncTimer");
        QVERIFY(popupTimer);
        QVERIFY(!popupTimer->isActive());
        popup.show();
        QVERIFY(popupTimer->isActive());
        popup.hide();
        QVERIFY(!popupTimer->isActive());
    }

    void statusIconsChangeOnlyWithTheirState()
    {
        auto gestures = KSharedConfig::openConfig()->group("QuickAccessGesture");
        gestures.writeEntry("Enabled", true);
        QuickAdjustDock dock;
        dock.show();
        QToolButton *gesture = nullptr;
        QToolButton *eraser = nullptr;
        for (auto *button : dock.findChildren<QToolButton *>()) {
            if (button->toolTip() == "Toggle Quick Access gestures")
                gesture = button;
            if (button->toolTip() == "Toggle eraser mode")
                eraser = button;
        }
        QVERIFY(gesture);
        QVERIFY(eraser);
        auto update = [&] {
            return QMetaObject::invokeMethod(&dock, "updateStatusButtons", Qt::DirectConnection);
        };

        QVERIFY(update());
        QVERIFY(gesture->isChecked());
        QVERIFY(!gesture->icon().isNull());
        QVERIFY(!eraser->isChecked());
        const qint64 gestureOn = gesture->icon().cacheKey();
        const qint64 eraserOff = eraser->icon().cacheKey();

        // the same state again: the icons are not built again
        QVERIFY(update());
        QVERIFY(update());
        QCOMPARE(gesture->icon().cacheKey(), gestureOn);
        QCOMPARE(eraser->icon().cacheKey(), eraserOff);

        // a changed state shows
        gestures.writeEntry("Enabled", false);
        QVERIFY(update());
        QVERIFY(!gesture->isChecked());
        QVERIFY(gesture->icon().cacheKey() != gestureOn);
        QCOMPARE(eraser->icon().cacheKey(), eraserOff);

        // a click that changed nothing (no canvas here) is taken back
        eraser->setChecked(true);
        QVERIFY(update());
        QVERIFY(!eraser->isChecked());

        gestures.writeEntry("Enabled", true);
        QVERIFY(update());
        QVERIFY(gesture->isChecked());
    }
};

int main(int argc, char **argv)
{
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir profile;
    if (!profile.isValid())
        return 1;
    qputenv("SOLSTICE_PROFILE_ROOT", profile.path().toUtf8());
    qputenv("LANGUAGE", "en");
    qputenv("EXTRA_RESOURCE_DIRS", QByteArray(KRITA_RESOURCE_DIRS_FOR_TESTS));
    qputenv("KRITA_PLUGIN_PATH", QByteArray(KRITA_PLUGINS_DIR_FOR_TESTS));
    kisTestSetupFontconfig();
    QApplication app(argc, argv);
    KConfig::setMainConfigName(profile.filePath("test-kritarc"));
    KLocalizedString::setApplicationDomain("krita");
    registerResources();
    QuickAdjustDockTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "QuickAdjustDockTest.moc"
