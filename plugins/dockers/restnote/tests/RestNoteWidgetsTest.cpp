/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RestNoteConfig.h"
#include "RestNoteDock.h"
#include "RestNoteNotifications.h"
#include "RestNoteSettingsDialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QKeyEvent>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScopeGuard>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <kconfig.h>
#include <kis_config.h>
#include <kis_config_notifier.h>
#include <klocalizedstring.h>
#include <memory>

namespace
{
QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QPushButton *button(QWidget *widget, const QString &text)
{
    for (auto *candidate : widget->findChildren<QPushButton *>()) {
        if (candidate->text() == text || candidate->toolTip() == text)
            return candidate;
    }
    return nullptr;
}

QLabel *label(QWidget *widget, const QString &text)
{
    for (auto *candidate : widget->findChildren<QLabel *>()) {
        if (candidate->text() == text)
            return candidate;
    }
    return nullptr;
}

void setEnabled(bool value)
{
    KisConfig(false).writeEntry("Solstice/RestNoteEnabled", value);
    KisConfigNotifier::instance()->notifyConfigChanged();
}
} // namespace

class RestNoteWidgetsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void settingsAcceptCancel_data()
    {
        QTest::addColumn<bool>("accept");
        QTest::newRow("cancel") << false;
        QTest::newRow("accept") << true;
    }

    void settingsAcceptCancel()
    {
        QFETCH(bool, accept);
        QTemporaryDir dir;
        const QString path = dir.filePath("main.json");
        RestNoteConfig config;
        config.microEnabled = false;
        config.idleEnabled = false;
        QVERIFY(config.save(path));
        const auto before = bytes(path);
        QWidget parent;
        bool inspected = false;
        // The order follows the existing form: work/break, eye break, idle,
        // overlay fonts, then toast geometry/fonts. Every field has a distinct value.
        const QList<int> values{41, 8, 12, 23, 94, 63, 51, 33, 19, 81, 441, 211, 29, 25};
        QTimer::singleShot(0, &parent, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto closeOnFailure = qScopeGuard([dialog] {
                dialog->reject();
            });
            QCOMPARE(dialog->parentWidget(), &parent);
            const auto spins = dialog->findChildren<QSpinBox *>();
            const auto checks = dialog->findChildren<QCheckBox *>();
            QCOMPARE(spins.size(), values.size());
            QCOMPARE(checks.size(), 2);
            QVERIFY(!spins[2]->isEnabled());
            QVERIFY(!spins[3]->isEnabled());
            QVERIFY(!spins[4]->isEnabled());
            QVERIFY(!spins[5]->isEnabled());
            for (auto *check : checks)
                check->setChecked(true);
            for (int i = 0; i < spins.size(); ++i) {
                QVERIFY(spins[i]->isEnabled());
                spins[i]->setValue(values[i]);
            }
            inspected = true;
            closeOnFailure.dismiss();
            dialog->done(accept ? QDialog::Accepted : QDialog::Rejected);
        });
        QCOMPARE(RestNoteSettingsDialog::edit(config, &parent), accept);
        QVERIFY(inspected);
        QCOMPARE(bytes(path), before); // The dialog never persists by itself.
        if (!accept) {
            QVERIFY(config.save(path));
            QCOMPARE(bytes(path), before);
        } else {
            const QList<int> actual{config.workMinutes,
                                    config.breakMinutes,
                                    config.microIntervalMinutes,
                                    config.microDurationSeconds,
                                    config.microSkipThresholdSeconds,
                                    config.idleThresholdSeconds,
                                    config.overlayTitleFontSize,
                                    config.overlayMessageFontSize,
                                    config.overlaySkipFontSize,
                                    config.toastMargin,
                                    config.toastWidth,
                                    config.toastHeight,
                                    config.toastTitleFontSize,
                                    config.toastMessageFontSize};
            QCOMPARE(actual, values);
            QVERIFY(config.microEnabled);
            QVERIFY(config.idleEnabled);
        }
    }

    void overlayOwnershipAndCompletion()
    {
        RestNoteConfig config;
        config.breakMinutes = 0; // The existing progress button clamps to one second.
        auto host = std::make_unique<QMainWindow>();
        host->resize(800, 600);
        host->show();
        int completions = 0;
        QPointer<QWidget> overlay = RestNoteNotifications::createBreakOverlay(config, host.get(), [&] {
            ++completions;
        });
        overlay->show();
        QCOMPARE(overlay->parentWidget(), host.get());
        QVERIFY(!overlay->isWindow());
        QCOMPARE(overlay->geometry(), host->rect());
        host->resize(910, 710);
        QTRY_COMPARE(overlay->geometry(), host->rect());
        auto *resume = button(overlay, "Resume work");
        auto *skip = button(overlay, "Skip");
        QVERIFY(resume);
        QVERIFY(skip);
        QVERIFY(!resume->isEnabled());
        // Deliver the same five progress ticks without waiting for real break time.
        auto *progress = resume->findChild<QTimer *>();
        QVERIFY(progress);
        progress->stop();
        for (int i = 0; i < 5; ++i)
            QVERIFY(QMetaObject::invokeMethod(progress, "timeout", Qt::DirectConnection));
        QVERIFY(resume->isEnabled());
        resume->click();
        QCOMPARE(completions, 1);
        QTRY_VERIFY(overlay.isNull());

        overlay = RestNoteNotifications::createBreakOverlay(config, host.get(), [&] {
            ++completions;
        });
        button(overlay, "Skip")->click();
        QCOMPARE(completions, 2);
        QTRY_VERIFY(overlay.isNull());
        overlay = RestNoteNotifications::createBreakOverlay(config, host.get(), [&] {
            ++completions;
        });
        host.reset();
        QVERIFY(overlay.isNull());
        QCOMPARE(completions, 2); // Host destruction does not resume a deleted docker.
    }

    void eyeBreakCompletion_data()
    {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("normal") << false;
        QTest::newRow("cancel") << true;
        QTest::newRow("cancel-during-fade") << true;
    }

    void eyeBreakCompletion()
    {
        QFETCH(bool, cancel);
        RestNoteConfig config;
        config.microDurationSeconds = 0;
        int completions = 0;
        auto *screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        QPointer<QWidget> toast = RestNoteNotifications::createEyeBreak(config, screen, [&] {
            ++completions;
        });
        QVERIFY(toast->isWindow());
        QVERIFY(toast->testAttribute(Qt::WA_ShowWithoutActivating));
        QVERIFY(toast->windowFlags().testFlag(Qt::WindowTransparentForInput));
        QVERIFY(toast->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QCOMPARE(toast->size(), QSize(config.toastWidth, config.toastHeight));
        const QRect available = screen->availableGeometry();
        QCOMPARE(toast->pos(),
                 QPoint(available.right() - config.toastWidth - config.toastMargin,
                        available.bottom() - config.toastHeight - config.toastMargin));
        auto *timer = toast->findChild<QTimer *>();
        QVERIFY(timer);
        timer->stop();
        if (!cancel || QByteArray(QTest::currentDataTag()) == "cancel-during-fade") {
            for (int i = 0; i < 10; ++i)
                QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        }
        if (cancel) {
            RestNoteNotifications::cancelEyeBreak(toast);
            RestNoteNotifications::cancelEyeBreak(toast); // Repeated cancellation is harmless.
        }
        QTRY_VERIFY_WITH_TIMEOUT(toast.isNull(), 4000);
        QCOMPARE(completions, cancel ? 0 : 1);
    }

    void dockEnableResetAndCleanup()
    {
        RestNoteConfig config;
        config.workMinutes = 1;
        config.idleEnabled = false;
        config.microEnabled = false;
        QVERIFY(config.save());
        setEnabled(false);
        QMainWindow host;
        auto *dock = new RestNoteDock(&host);
        host.addDockWidget(Qt::RightDockWidgetArea, dock);
        auto *timer = dock->findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(timer);
        QVERIFY(!timer->isActive());
        setEnabled(true);
        QVERIFY(timer->isActive());
        timer->stop();
        for (int i = 0; i < 60; ++i)
            QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QVERIFY(label(dock, "ON BREAK"));
        auto *title = label(&host, "Time for a break");
        QVERIFY(title);
        QPointer<QWidget> overlay = title->parentWidget();
        QCOMPARE(overlay->parentWidget(), &host);
        auto *pause = button(dock, "Pause / Resume");
        QVERIFY(pause);
        QVERIFY(!pause->isEnabled());
        setEnabled(false);
        QVERIFY(!timer->isActive());
        QVERIFY(!dock->toggleViewAction()->isEnabled());
        QVERIFY(dock->isHidden());
        QTRY_VERIFY(overlay.isNull());
        setEnabled(true);
        QVERIFY(timer->isActive());
        timer->stop();
        QVERIFY(label(dock, "WORKING"));
        QVERIFY(label(dock, "01:00"));
        pause->click();
        QVERIFY(label(dock, "PAUSED"));
        button(dock, "Reset")->click();
        QVERIFY(label(dock, "WORKING"));
        button(dock, "Take a break now")->click();
        title = label(&host, "Time for a break");
        QVERIFY(title);
        overlay = title->parentWidget();
        delete dock;
        QTRY_VERIFY(overlay.isNull());
        QKeyEvent activity(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
        QApplication::sendEvent(&host, &activity); // No dangling input-idle filter.
        setEnabled(false);
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
    QApplication app(argc, argv);
    KConfig::setMainConfigName(profile.filePath("test-kritarc"));
    KLocalizedString::setApplicationDomain("krita");
    RestNoteWidgetsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "RestNoteWidgetsTest.moc"
