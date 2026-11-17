/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessGridEditDialog.h"
#include "QuickAccessGridEditOperations.h"
#include "QuickAccessGridCanvas.h"
#include "QuickAccessGridItemButton.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <kconfig.h>
#include <klocalizedstring.h>
#include <testui.h>

using namespace QuickAccess;

namespace
{
Item item(const QString &id, int row, int column, int span = 1)
{
    Item result;
    result.id = id;
    result.type = ItemType::Label;
    result.row = row;
    result.column = column;
    result.columnSpan = span;
    result.payload = {{"text", id}, {"fontColor", "#102030"}, {"custom", "preserve"}};
    return result;
}

Document fixture()
{
    Document doc;
    doc.activeTabId = "one";
    doc.tabs = {{"one", "One", {{"grid-one", "", 4, {item("a", 0, 0), item("b", 0, 1), item("c", 1, 0)}}}},
                {"two", "Two", {{"grid-two", "", 4, {item("target", 2, 0)}}}}};
    doc.aliases = {{"custom", QJsonObject{{"keep", true}}}};
    doc.gesturePages = {QJsonObject{{"keep", "gesture"}}};
    return doc;
}

Item findItem(const Document &doc, const QString &id, int tab = 0)
{
    for (const Item &value : doc.tabs[tab].grids.first().items) {
        if (value.id == id)
            return value;
    }
    return {};
}

QuickAccessGridItemButton *button(QuickAccessGridEditDialog &dialog, const QString &id)
{
    for (auto *widget : dialog.findChildren<QPushButton *>()) {
        if (auto *value = dynamic_cast<QuickAccessGridItemButton *>(widget)) {
            if (value->itemId() == id)
                return value;
        }
    }
    return nullptr;
}

void mouse(QWidget *widget, QEvent::Type type, const QPoint &position, Qt::KeyboardModifiers modifiers = {})
{
    QMouseEvent event(type,
                      QPointF(position),
                      QPointF(widget->mapToGlobal(position)),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      modifiers);
    QApplication::sendEvent(widget, &event);
}

void drag(QWidget *widget, const QPoint &from, const QPoint &to, Qt::KeyboardModifiers modifiers = {})
{
    mouse(widget, QEvent::MouseButtonPress, from, modifiers);
    mouse(widget, QEvent::MouseMove, to, modifiers);
    mouse(widget, QEvent::MouseButtonRelease, to, modifiers);
    QCoreApplication::processEvents();
}

void undo(QuickAccessGridEditDialog &dialog)
{
    QMetaObject::invokeMethod(&dialog, "undoCurrentTab", Qt::DirectConnection);
    QCoreApplication::processEvents();
}
} // namespace

class QuickAccessGridEditTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void groupMovement()
    {
        using QuickAccessGridEditOperations::moveGroup;
        const QList<Item> source{item("a", 2, 1, 2), item("b", 4, 2), item("c", 0, 0)};
        QCOMPARE(moveGroup(source, {}, 5, 5, 4), source);
        QCOMPARE(moveGroup(source, {"missing"}, 5, 5, 4), source);
        auto result = moveGroup(source, {"a", "b"}, -99, -99, 4);
        QCOMPARE(result[0].row, 0);
        QCOMPARE(result[0].column, 0);
        QCOMPARE(result[1].row, 2);
        QCOMPARE(result[1].column, 1);
        QCOMPARE(result[2].row, 0);
        QCOMPARE(result[2].column, 2);
        QCOMPARE(result[0].payload, source[0].payload);
        result = moveGroup(source, {"a", "b"}, 2, 99, 4);
        QCOMPARE(result[0].column, 2);
        QCOMPARE(result[1].column, 3);
        QCOMPARE(result[1].row - result[0].row, 2);
        // Collision relocation wraps wide items without overlapping the group.
        result = moveGroup({item("moving", 1, 0, 2), item("wide", 0, 0, 3), item("last", 0, 3)}, {"moving"}, -1, 0, 4);
        QCOMPARE(result[1].row, 1);
        QCOMPARE(result[1].column, 0);
        QCOMPARE(result[2].row, 0);
        QCOMPARE(result[2].column, 3);
    }

    void movePreviewSelectionUndoAndCancel()
    {
        const Document original = fixture();
        QuickAccessGridEditDialog dialog(original);
        dialog.show();
        auto *a = button(dialog, "a");
        auto *b = button(dialog, "b");
        QVERIFY(a && b);
        QTest::mouseClick(a, Qt::LeftButton);
        QTest::mouseClick(b, Qt::LeftButton, Qt::ControlModifier);
        // A preview cannot change the document, and rebuilding waits for release.
        const QPoint start(12, 12);
        const QPoint end = start + QPoint(46, 46);
        mouse(a, QEvent::MouseButtonPress, start);
        mouse(a, QEvent::MouseMove, end);
        QCOMPARE(dialog.document(), original);
        QPointer<QuickAccessGridItemButton> oldButton = a;
        mouse(a, QEvent::MouseButtonRelease, end);
        QVERIFY(oldButton);
        QCoreApplication::processEvents();
        QVERIFY(oldButton.isNull());
        QCOMPARE(findItem(dialog.document(), "a").column, 1);
        QCOMPARE(findItem(dialog.document(), "b").column, 2);
        QCOMPARE(findItem(dialog.document(), "a").row, 1);
        QCOMPARE(dialog.document().aliases, original.aliases);
        QCOMPARE(dialog.document().gesturePages, original.gesturePages);
        undo(dialog);
        QCOMPARE(dialog.document(), original);
        drag(button(dialog, "a"), start, end);
        QTest::mouseClick(dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(original, fixture());
    }

    void rubberBandAndResize()
    {
        QuickAccessGridEditDialog dialog(fixture());
        dialog.show();
        auto *canvas = dynamic_cast<QuickAccessGridCanvas *>(button(dialog, "a")->parentWidget());
        QVERIFY(canvas);
        QCOMPARE(canvas->itemsInRect(QRect(4, 4, 88, 42)), QSet<QString>({"a", "b"}));
        drag(canvas, QPoint(2, 2), QPoint(93, 47));
        drag(button(dialog, "a"), QPoint(12, 12), QPoint(12, 58));
        QCOMPARE(findItem(dialog.document(), "a").row, 1);
        QCOMPARE(findItem(dialog.document(), "b").row, 1);
        undo(dialog);
        auto *a = button(dialog, "a");
        const QPoint edge(a->width() - 2, 12);
        drag(a, edge, edge + QPoint(46, 0));
        QCOMPARE(findItem(dialog.document(), "a").columnSpan, 2);
        QCOMPARE(findItem(dialog.document(), "b").column, 2);
        undo(dialog);
        QCOMPARE(dialog.document(), fixture());
    }

    void verticalResizeAndIcon()
    {
        Document doc = fixture();
        auto &items = doc.tabs[0].grids[0].items;
        items[0].type = ItemType::Separator;
        items[0].payload = {{"orientation", "vertical"}};
        items[1].type = ItemType::Action;
        items[1].payload = {{"action_id", "test"}};
        doc.aliases["actions"] =
            QJsonObject{{"test", QJsonObject{{"custom_name", "Alias"}, {"icon_name", "reset.png"}}}};
        QuickAccessGridEditDialog dialog(doc);
        dialog.show();
        auto *a = button(dialog, "a");
        const QPoint edge(12, a->height() - 2);
        drag(a, edge, edge + QPoint(0, 46));
        QCOMPARE(findItem(dialog.document(), "a").rowSpan, 2);
        undo(dialog);
        auto *b = button(dialog, "b");
        QVERIFY(!b->icon().isNull());
        QCOMPARE(b->toolTip(), QStringLiteral("Alias"));
        const QPoint iconEdge(b->width() - 2, 12);
        drag(b, iconEdge, iconEdge + QPoint(46, 0));
        QCOMPARE(findItem(dialog.document(), "b").column, 2);
        QCOMPARE(findItem(dialog.document(), "b").columnSpan, 1);
    }

    void contextOperations_data()
    {
        QTest::addColumn<int>("operation");
        QTest::newRow("remove") << 0;
        QTest::newRow("copy") << 1;
        QTest::newRow("move") << 2;
    }

    void contextOperations()
    {
        QFETCH(int, operation);
        QuickAccessGridEditDialog dialog(fixture());
        dialog.show();
        bool menuOpened = false;
        QTimer::singleShot(0, &dialog, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            menuOpened = true;
            if (operation == 0) {
                menu->setActiveAction(menu->actions()[0]);
                QTest::keyClick(menu, Qt::Key_Return);
            } else {
                menu->actions()[operation]->menu()->actions()[0]->trigger();
                menu->close();
            }
        });
        QTest::mouseClick(button(dialog, "a"), Qt::RightButton);
        QVERIFY(menuOpened);
        QCoreApplication::processEvents();
        const auto result = dialog.document();
        QCOMPARE(result.tabs[0].grids[0].items.size(), operation == 1 ? 3 : 2);
        if (operation) {
            const auto target = result.tabs[1].grids[0].items;
            QCOMPARE(target.size(), 2);
            const auto copied = target.first().id == "target" ? target.last() : target.first();
            QVERIFY(copied.id != "a");
            QVERIFY(copied.id != "target");
            QCOMPARE(copied.row, 3);
            QCOMPARE(copied.payload, findItem(fixture(), "a").payload);
            dialog.findChild<QTabWidget *>()->setCurrentIndex(1);
            undo(dialog);
            QCOMPARE(dialog.document().tabs[1].grids[0].items.size(), 1);
            dialog.findChild<QTabWidget *>()->setCurrentIndex(0);
        }
        if (operation != 1) {
            undo(dialog);
            QCOMPARE(dialog.document().tabs[0].grids[0], fixture().tabs[0].grids[0]);
        }
    }

    void historyLimitAndSave()
    {
        Document doc = fixture();
        doc.tabs[0].grids[0].items = {item("a", 0, 0)};
        QuickAccessGridEditDialog dialog(doc);
        dialog.show();
        for (int i = 0; i < 21; ++i)
            drag(button(dialog, "a"), QPoint(12, 12), QPoint(12, 58));
        for (int i = 0; i < 21; ++i)
            undo(dialog);
        QCOMPARE(findItem(dialog.document(), "a").row, 1);
        dialog.findChild<QTabWidget *>()->setCurrentIndex(1);
        QTest::mouseClick(dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save), Qt::LeftButton);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.document().activeTabId, QStringLiteral("two"));
        QCOMPARE(doc.tabs[0].grids[0].items.first().row, 0);
    }

    void destructionWithPendingRebuild()
    {
        auto *dialog = new QuickAccessGridEditDialog(fixture());
        auto *a = button(*dialog, "a");
        QPointer<QWidget> canvas = a->parentWidget();
        mouse(a, QEvent::MouseButtonPress, QPoint(12, 12));
        mouse(a, QEvent::MouseButtonRelease, QPoint(12, 58));
        delete dialog;
        QCoreApplication::processEvents();
        QVERIFY(canvas.isNull());
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
    QuickAccessGridEditTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "QuickAccessGridEditTest.moc"
