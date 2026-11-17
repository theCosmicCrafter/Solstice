/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessDock.h"
#include "QuickAccessGesture.h"
#include "QuickAccessGestureDialog.h"
#include "QuickAccessGesturePreview.h"
#include "QuickAccessItemDragController.h"
#include "QuickAccessProfileRepository.h"

#include <KoResourcePaths.h>
#include <QApplication>
#include <QCheckBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QDir>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QRubberBand>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <kconfiggroup.h>
#include <ksharedconfig.h>
#include <klocalizedstring.h>
#include <testui.h>

using namespace QuickAccess;

namespace
{
Item label(const QString &id, int column = 0)
{
    Item item;
    item.id = id;
    item.type = ItemType::Label;
    item.column = column;
    item.payload = {{"text", id}, {"fontColor", "#102030"}, {"keep", 42}};
    return item;
}

Document fixture()
{
    Document doc;
    doc.activeTabId = "tab";
    doc.tabs = {{"tab", "Tab", {{"grid", "", 4, {label("first"), label("second", 1)}}}}};
    doc.aliases = {{"unknown", QJsonObject{{"preserve", 1}}}};
    doc.gesturePages = {QJsonObject{{"gesture_key", "A"}, {"unknown", "preserve"}}};
    return doc;
}

QString profilePath()
{
    return QDir(KoResourcePaths::saveLocation("data", "quickaccess/", true)).filePath("default.kqap");
}

QWidget *itemWidget(QuickAccessDock &dock, const QString &id)
{
    for (auto *widget : dock.findChildren<QWidget *>()) {
        if (widget->property("quickaccess_item_id").toString() == id)
            return widget;
    }
    return nullptr;
}

void mouse(QWidget *widget,
           QEvent::Type type,
           const QPoint &delta,
           Qt::KeyboardModifiers modifiers = Qt::ControlModifier,
           Qt::MouseButton button = Qt::LeftButton)
{
    const QPoint local = QPoint(10, 10) + delta;
    QMouseEvent event(type,
                      QPointF(local),
                      QPointF(widget->mapToGlobal(local)),
                      type == QEvent::MouseMove ? Qt::NoButton : button,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : button,
                      modifiers);
    QApplication::sendEvent(widget, &event);
}

void key(QWidget &widget, QEvent::Type type, int code = Qt::Key_A, const QString &text = "a", bool repeat = false)
{
    QKeyEvent event(type, code, Qt::NoModifier, text, repeat);
    QApplication::sendEvent(&widget, &event);
}

void gestureMove(QWidget &widget, const QPoint &delta)
{
    QMouseEvent event(QEvent::MouseMove,
                      QPointF(10, 10),
                      QPointF(QCursor::pos() + delta),
                      Qt::NoButton,
                      Qt::NoButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

QJsonObject config(const QString &id)
{
    return {{"gesture_type", "docker_toggle"}, {"parameters", QJsonObject{{"docker_name", id}}}};
}

void flush()
{
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
} // namespace

class QuickAccessInteractionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init()
    {
        auto config = KSharedConfig::openConfig();
        config->group("QuickAccessGesture").deleteGroup();
        config->group("QuickAccessGesture").writeEntry("ShowPreview", false);
        config->group("QuickAccess").writeEntry("DockerIconSize", 42);
        config->group("QuickAccess").writeEntry("PopupIconSize", 54);
        config->sync();
        QVERIFY(ProfileRepository(profilePath()).save(fixture()));
    }

    void paletteMove_data()
    {
        QTest::addColumn<bool>("popup");
        QTest::newRow("docker") << false;
        QTest::newRow("popup") << true;
    }

    void paletteMove()
    {
        QFETCH(bool, popup);
        QuickAccessDock dock(nullptr, !popup);
        dock.setEnabled(true);
        dock.show();
        flush();
        QWidget *first = itemWidget(dock, "first");
        QVERIFY(first);
        QCOMPARE(first->contextMenuPolicy(), popup ? Qt::DefaultContextMenu : Qt::CustomContextMenu);
        const int step = popup ? 56 : 44;
        mouse(first, QEvent::MouseButtonPress, {});
        mouse(first, QEvent::MouseMove, QPoint(step, 0));
        auto *band = first->parentWidget()->findChild<QRubberBand *>();
        QVERIFY(band);
        QCOMPARE(band->geometry(), QRect(step, 0, step - 2, step - 2));
        Document before;
        QVERIFY(ProfileRepository(profilePath()).load(&before));
        QCOMPARE(before, fixture());
        mouse(first, QEvent::MouseButtonRelease, QPoint(step, 0));
        QVERIFY(ProfileRepository(profilePath()).load(&before));
        QCOMPARE(before, fixture()); // Saving/rebuilding must wait for release delivery.
        flush();
        Document after;
        QVERIFY(ProfileRepository(profilePath()).load(&after));
        QCOMPARE(after.tabs.first().grids.first().items.first().column, 1);
        QCOMPARE(after.tabs.first().grids.first().items.first().payload,
                 fixture().tabs.first().grids.first().items.first().payload);
        QCOMPARE(after.aliases, before.aliases);
        QCOMPARE(after.gesturePages, before.gesturePages);
        QVERIFY(itemWidget(dock, "first"));
        QVERIFY(after.tabs.first().grids.first().items[1].column != 1);
    }

    void dragCancelClickClampAndDestruction()
    {
        Grid grid{"g", "", 4, {label("a")}};
        grid.items[0].columnSpan = 2;
        QWidget owner;
        auto *button = new QPushButton("Button", &owner);
        owner.show();
        int commits = 0;
        QPoint target;
        auto *controller = new QuickAccessItemDragController(
            [&] {
                return &grid;
            },
            [] {
                return 42;
            },
            [&](const QString &id, const QPoint &value) {
                QCOMPARE(id, QString("a"));
                ++commits;
                target = value;
            },
            &owner);
        controller->attach(button, grid.items.first());
        QSignalSpy clicks(button, &QPushButton::clicked);
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(clicks.count(), 1);
        mouse(button, QEvent::MouseButtonPress, {});
        mouse(button, QEvent::MouseMove, QPoint(999, -999));
        mouse(button, QEvent::MouseButtonRelease, QPoint(999, -999));
        QCOMPARE(commits, 0);
        flush();
        QCOMPARE(commits, 1);
        QCOMPARE(target, QPoint(2, 0));
        QCOMPARE(clicks.count(), 1);
        mouse(button, QEvent::MouseButtonPress, {});
        mouse(button, QEvent::MouseButtonPress, {}, Qt::ControlModifier, Qt::RightButton);
        mouse(button, QEvent::MouseButtonRelease, QPoint(44, 44));
        flush();
        QCOMPARE(commits, 1);
        mouse(button, QEvent::MouseButtonPress, {});
        mouse(button, QEvent::MouseButtonRelease, QPoint(44, 44));
        delete controller;
        flush();
        QCOMPARE(commits, 1); // Destroying the owner of the deferred callback cancels it.
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(clicks.count(), 2);
    }

    void popupPinAndDrag()
    {
        QuickAccessDock popup(nullptr, false);
        popup.setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
        popup.setEnabled(true);
        popup.show();
        QToolButton *pin = nullptr;
        for (auto *button : popup.findChildren<QToolButton *>()) {
            if (button->isCheckable())
                pin = button;
        }
        QVERIFY(pin);
        const QPoint original = popup.pos();
        pin->click();
        QCOMPARE(popup.windowType(), Qt::Tool);
        QCOMPARE(popup.pos(), original);
        pin->click();
        // Preserve the original setWindowFlag sequence: clearing Tool also
        // clears bits shared with Popup, leaving Window after unpinning.
        QCOMPARE(popup.windowType(), Qt::Window);
        QWidget *bar = pin->parentWidget();
        mouse(bar, QEvent::MouseButtonPress, {}, Qt::NoModifier);
        mouse(bar, QEvent::MouseMove, QPoint(35, 20), Qt::NoModifier);
        mouse(bar, QEvent::MouseButtonRelease, {}, Qt::NoModifier);
        QCOMPARE(popup.pos(), original + QPoint(35, 20));
    }

    void directions_data()
    {
        QTest::addColumn<QPoint>("delta");
        QTest::addColumn<QString>("direction");
        const QList<QPair<QPoint, QString>> rows{{{0, 0}, "center"},
                                                 {{19, 0}, "center"},
                                                 {{20, 0}, "right"},
                                                 {{100, -100}, "right_up"},
                                                 {{0, -100}, "up"},
                                                 {{-100, -100}, "left_up"},
                                                 {{-100, 0}, "left"},
                                                 {{-100, 100}, "left_down"},
                                                 {{0, 100}, "down"},
                                                 {{100, 100}, "right_down"}};
        for (const auto &row : rows)
            QTest::newRow(qPrintable(row.second + QString::number(row.first.x()))) << row.first << row.second;
    }

    void directions()
    {
        QFETCH(QPoint, delta);
        QFETCH(QString, direction);
        Document doc;
        QJsonObject page{{"gesture_key", "a"}};
        for (const QString &name :
             {"center", "right", "right_up", "up", "left_up", "left", "left_down", "down", "right_down"})
            page.insert(name, config(name));
        doc.gesturePages = {page};
        QList<Item> executed;
        QWidget widget;
        widget.show();
        widget.setFocus();
        flush();
        QuickAccessGestureController controller(&doc, [&](const Item &item) {
            executed << item;
        });
        key(widget, QEvent::KeyPress);
        gestureMove(widget, delta);
        key(widget, QEvent::KeyRelease);
        QCOMPARE(executed.size(), 1);
        QCOMPARE(executed.first().type, ItemType::DockerToggle);
        QCOMPARE(executed.first().payload.value("docker_id").toString(), direction);
        key(widget, QEvent::KeyRelease);
        QCOMPARE(executed.size(), 1);
    }

    void gestureGuardsAndLifetime()
    {
        Document doc;
        doc.gesturePages = {QJsonObject{{"gesture_key", "F2"}, {"center", config("center")}}};
        QWidget widget;
        widget.show();
        flush();
        int count = 0;
        auto *controller = new QuickAccessGestureController(&doc, [&](const Item &) {
            ++count;
        });
        key(widget, QEvent::KeyPress, Qt::Key_F2, "", true);
        key(widget, QEvent::KeyRelease, Qt::Key_F2, "");
        QCOMPARE(count, 0);
        key(widget, QEvent::KeyPress, Qt::Key_F2, "");
        key(widget, QEvent::KeyRelease, Qt::Key_F3, "");
        QCOMPARE(count, 0);
        key(widget, QEvent::KeyRelease, Qt::Key_F2, "", true);
        QCOMPARE(count, 0);
        key(widget, QEvent::KeyRelease, Qt::Key_F2, "");
        QCOMPARE(count, 1);
        key(widget, QEvent::KeyPress, Qt::Key_F2, "");
        KSharedConfig::openConfig()->group("QuickAccessGesture").writeEntry("Enabled", false);
        controller->reloadSettings();
        key(widget, QEvent::KeyRelease, Qt::Key_F2, "");
        QCOMPARE(count, 1);
        KSharedConfig::openConfig()->group("QuickAccessGesture").writeEntry("Enabled", true);
        controller->reloadSettings();
        QLineEdit edit;
        edit.show();
        edit.activateWindow();
        edit.setFocus();
        flush();
        QCOMPARE(QApplication::focusWidget(), &edit);
        key(edit, QEvent::KeyPress, Qt::Key_F2, "");
        key(edit, QEvent::KeyRelease, Qt::Key_F2, "");
        QCOMPARE(count, 1);
        edit.hide();
        QDialog modal;
        modal.setModal(true);
        modal.show();
        flush();
        key(modal, QEvent::KeyPress, Qt::Key_F2, "");
        key(modal, QEvent::KeyRelease, Qt::Key_F2, "");
        QCOMPARE(count, 1);
        modal.hide();
        delete controller;
        key(widget, QEvent::KeyPress, Qt::Key_F2, "");
        key(widget, QEvent::KeyRelease, Qt::Key_F2, "");
        QCOMPARE(count, 1);
    }

    void previewAndDialogRoundTrip()
    {
        Document doc = fixture();
        const Document original = doc;
        auto *preview = new QuickAccessGesturePreview;
        preview->showPage(doc, QJsonObject{{"center", config("missing-docker")}}, QPoint(500, 400));
        flush();
        QCOMPARE(preview->findChildren<QToolButton *>().size(), 9);
        const QSize layoutSize = preview->layout()->sizeHint();
        QCOMPARE(preview->pos(), QPoint(500 - layoutSize.width() / 2, 400 - layoutSize.height() / 2));
        QVERIFY(preview->testAttribute(Qt::WA_TransparentForMouseEvents));
        QVERIFY(preview->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        preview->showPage(doc, {}, QPoint(200, 200));
        QCOMPARE(preview->findChildren<QToolButton *>().size(), 9);
        delete preview; // Includes a pending position callback.
        flush();
        QuickAccessGestureDialog dialog(doc);
        QCOMPARE(dialog.pages(), doc.gesturePages);
        QCOMPARE(dialog.movementThreshold(), 20);
        QVERIFY(dialog.gesturesEnabled());
        QVERIFY(!dialog.showPreview());
        auto *tabs = dialog.findChild<QTabWidget *>();
        tabs->currentWidget()->findChild<QLineEdit *>()->setText(" f2 ");
        QCOMPARE(dialog.pages().first().toObject().value("gesture_key").toString(), QString("F2"));
        QCOMPARE(dialog.pages().first().toObject().value("unknown").toString(), QString("preserve"));
        dialog.findChild<QSpinBox *>()->setValue(200);
        QCOMPARE(dialog.movementThreshold(), 200);
        for (auto *button : dialog.findChildren<QToolButton *>()) {
            if (button->text() == "+")
                button->click();
        }
        QCOMPARE(tabs->count(), 2);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(doc, original);
        QuickAccessGestureDialog accepted(doc);
        accepted.findChild<QTabWidget *>()->currentWidget()->findChild<QLineEdit *>()->setText("B");
        accepted.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QCOMPARE(accepted.result(), int(QDialog::Accepted));
        QCOMPARE(accepted.pages().first().toObject().value("gesture_key").toString(), QString("B"));
        QCOMPARE(doc, original); // Only the caller commits accepted values.
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
    QuickAccessInteractionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "QuickAccessInteractionTest.moc"
