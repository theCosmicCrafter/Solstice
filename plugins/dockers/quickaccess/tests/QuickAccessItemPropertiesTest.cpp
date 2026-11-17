/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "QuickAccessItemProperties.h"
#include "QuickAccessItemPresentation.h"
#include "QuickAccessLayoutEngine.h"
#include "QuickAccessSettingsDialog.h"
#include "QuickAccessSettingsStore.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace QuickAccess;
class QuickAccessItemPropertiesTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void editItems_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<bool>("accept");
        for (auto type : {ItemType::Action,
                          ItemType::DockerToggle,
                          ItemType::Label,
                          ItemType::Separator,
                          ItemType::Color,
                          ItemType::BrushSize,
                          ItemType::BrushBlendMode,
                          ItemType::Script}) {
            for (bool accept : {false, true}) {
                const QByteArray name = itemTypeToString(type).toUtf8() + (accept ? "-accept" : "-cancel");
                QTest::newRow(name.constData()) << int(type) << accept;
            }
        }
    }
    void editItems()
    {
        QFETCH(int, type);
        QFETCH(bool, accept);
        Document document = Document::createDefault();
        Grid &grid = document.tabs[0].grids[0];
        Item item;
        item.id = "test";
        item.type = ItemType(type);
        item.payload = QJsonObject{{"text", "12"},
                                   {"action_id", "undo"},
                                   {"docker_id", "layers"},
                                   {"font_size", "21"},
                                   {"color", "#112233"},
                                   {"script_path", "original.py"},
                                   {"unknown", "keep"}};
        grid.items = {item};
        const QString category = item.type == ItemType::Action ? "actions" : "dockers";
        const QString aliasId = item.type == ItemType::Action ? "undo" : "layers";
        document.aliases = QJsonObject{{category,
                                        QJsonObject{{aliasId,
                                                     QJsonObject{{"customName", "legacy alias"},
                                                                 {"fontSize", "23"},
                                                                 {"backgroundColor", "#123456"},
                                                                 {"fontColor", "#abcdef"},
                                                                 {"icon_name", "old.png"},
                                                                 {"unknown", "keep"}}}}}};
        const QJsonObject before = document.toJson();
        QWidget parent;
        bool opened = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            opened = true;
            const bool parentMatches = dialog->parentWidget() == &parent;
            const auto edits = dialog->findChildren<QLineEdit *>(QString(), Qt::FindDirectChildrenOnly);
            if (!edits.isEmpty())
                edits.first()->setText(" changed ");
            for (auto *spin : dialog->findChildren<QSpinBox *>())
                spin->setValue(27);
            if (accept)
                dialog->accept();
            else
                dialog->reject();
            QVERIFY(parentMatches);
        });
        const bool changed = QuickAccessItemProperties::edit(document, grid, item.id, "fallback", &parent);
        QVERIFY(opened);
        QCOMPARE(changed, accept);
        if (!accept) {
            QCOMPARE(document.toJson(), before);
            return;
        }
        const auto payload = grid.items[0].payload;
        QCOMPARE(payload.value("unknown").toString(), QStringLiteral("keep"));
        if (item.type == ItemType::Action || item.type == ItemType::DockerToggle) {
            const auto alias = QuickAccessItemPresentation::aliasFor(document, category, aliasId);
            QCOMPARE(alias.value("custom_name").toString(), QStringLiteral("changed"));
            QCOMPARE(alias.value("font_size").toString(), QStringLiteral("27"));
            QCOMPARE(alias.value("background_color").toString(), QStringLiteral("#ff123456"));
            QCOMPARE(alias.value("font_color").toString(), QStringLiteral("#ffabcdef"));
            QCOMPARE(alias.value("icon_name").toString(), QStringLiteral("old.png"));
            QCOMPARE(alias.value("unknown").toString(), QStringLiteral("keep"));
            QCOMPARE(payload, item.payload); // Aliases never replace stored action/docker IDs.
        } else if (item.type == ItemType::Separator) {
            QCOMPARE(payload.value("thickness").toString(), QStringLiteral("27"));
            QCOMPARE(payload.value("color").toString(), QStringLiteral("#ff112233"));
        } else if (item.type == ItemType::Color) {
            QCOMPARE(payload.value("color").toString(), QStringLiteral("#ff112233"));
        } else if (item.type == ItemType::BrushSize) {
            QCOMPARE(payload.value("text").toString(), QStringLiteral("27"));
        } else if (item.type == ItemType::Script) {
            QCOMPARE(payload.value("custom_name").toString(), QStringLiteral("changed"));
            QCOMPARE(payload.value("script_path").toString(), QStringLiteral("original.py"));
        } else {
            QCOMPARE(payload.value("text").toString(), QStringLiteral("changed"));
            QCOMPARE(payload.value("fontSize").toString(), QStringLiteral("27"));
        }
    }

    void brushDisplay_data()
    {
        QTest::addColumn<int>("columns");
        QTest::newRow("one-column") << 1;
        QTest::newRow("push-neighbour") << 4;
    }
    void brushDisplay()
    {
        QFETCH(int, columns);
        Grid grid;
        grid.columns = columns;
        Item brush;
        brush.id = "brush";
        brush.type = ItemType::Brush;
        brush.payload = QJsonObject{{"brush_name", "test preset"}}; // Old profiles omit display.
        Item neighbour;
        neighbour.id = "next";
        neighbour.type = ItemType::Action;
        neighbour.row = columns == 1 ? 1 : 0;
        neighbour.column = columns == 1 ? 0 : 1;
        grid.items = {brush, neighbour};
        QWidget parent;
        const auto choose = [](int index, bool accept) {
            QTimer::singleShot(0, [index, accept] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                if (auto *combo = dialog->findChild<QComboBox *>())
                    combo->setCurrentIndex(index);
                if (accept)
                    dialog->accept();
                else
                    dialog->reject();
            });
        };
        const auto before = grid.toJson();
        choose(0, false);
        QVERIFY(!QuickAccessItemProperties::editBrush(grid, "brush", &parent));
        QCOMPARE(grid.toJson(), before);
        choose(1, true);
        QVERIFY(!QuickAccessItemProperties::editBrush(grid, "brush", &parent));
        QCOMPARE(grid.toJson(), before);
        choose(0, true);
        QVERIFY(QuickAccessItemProperties::editBrush(grid, "brush", &parent));
        QVERIFY(LayoutEngine(columns).validate(grid.items).isValid());
        QVERIFY(QuickAccessItemPresentation::showsStrokePreview(grid.items[0]));
        QCOMPARE(grid.items[0].columnSpan, qMin(2, columns));
        if (columns > 1)
            QCOMPARE(grid.items[1].column, 2);
        choose(1, true);
        QVERIFY(QuickAccessItemProperties::editBrush(grid, "brush", &parent));
        QCOMPARE(grid.items[0].columnSpan, 1);
        QVERIFY(!QuickAccessItemPresentation::showsStrokePreview(grid.items[0]));
    }

    void iconActionTakesOneCell_data()
    {
        QTest::addColumn<bool>("accept");
        QTest::newRow("cancel") << false;
        QTest::newRow("accept") << true;
    }
    void iconActionTakesOneCell()
    {
        QFETCH(bool, accept);
        Document document = Document::createDefault();
        document.tabs.append(document.tabs[0]);
        document.tabs[1].id = "second";
        const auto wide = [](const QString &id, ItemType type, const QJsonObject &payload, int column) {
            Item item;
            item.id = id;
            item.type = type;
            item.payload = payload;
            item.column = column;
            item.columnSpan = 2;
            return item;
        };
        Grid &grid = document.tabs[0].grids[0];
        grid.columns = 6;
        grid.items = {wide("action", ItemType::Action, {{"action_id", "undo"}}, 0),
                      wide("docker", ItemType::DockerToggle, {{"docker_id", "layers"}}, 2),
                      wide("script", ItemType::Script, {{"icon_name", "reset.png"}}, 4)};
        document.tabs[1].grids[0].items = {wide("same-action", ItemType::Action, {{"action_id", "undo"}}, 0)};
        // A configured icon that does not resolve is not drawn, so the docker keeps its width.
        document.aliases = QJsonObject{{"dockers", QJsonObject{{"layers", QJsonObject{{"icon_name", "missing.png"}}}}}};
        QWidget parent;
        QTimer::singleShot(0, [accept] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            for (auto *edit : dialog->findChildren<QLineEdit *>()) {
                // The icon field shares its row with a Browse button.
                if (edit->parentWidget() != dialog && edit->parentWidget()->findChild<QPushButton *>())
                    edit->setText("reset.png");
            }
            if (accept)
                dialog->accept();
            else
                dialog->reject();
        });
        QCOMPARE(QuickAccessItemProperties::edit(document, grid, "action", "Undo", &parent), accept);
        const int expected = accept ? 1 : 2;
        QCOMPARE(grid.items[0].columnSpan, expected);
        QCOMPARE(grid.items[0].rowSpan, 1);
        QCOMPARE(document.tabs[1].grids[0].items[0].columnSpan, expected);
        QCOMPARE(grid.items[1].columnSpan, 2);
        QCOMPARE(grid.items[2].columnSpan, 2);
        QVERIFY(LayoutEngine(grid.columns).validate(grid.items).isValid());
        if (accept) {
            Document saved;
            QVERIFY(Document::fromJson(document.toJson(), &saved));
            QCOMPARE(saved.tabs[0].grids[0].items[0].columnSpan, 1);
        }
    }

    void iconResolution()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString external = dir.filePath("reset.png");
        QFile file(external);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("fixture");
        file.close();
        using namespace QuickAccessItemPresentation;
        QCOMPARE(resolveDefaultIcon("reset.png"), QStringLiteral(":/quickaccess/default_icons/reset.png"));
        QCOMPARE(resolveDefaultIcon(external), external);
        QCOMPARE(resolveDefaultIcon(dir.filePath("missing/reset.png")),
                 QStringLiteral(":/quickaccess/default_icons/reset.png"));
        QVERIFY(resolveDefaultIcon("missing.png").isEmpty());
    }

    void settingsDialogRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QuickAccessSettingsStore store(KSharedConfig::openConfig(dir.filePath("settings.ini"), KConfig::SimpleConfig));
        auto settings = store.load(6);
        settings.altEraseKey = "Alt+E";
        settings.tempBrushSets =
            QJsonArray{QJsonObject{{"key", "Alt+B"}, {"brush", "test preset"}, {"size_scale", 1.5}}};
        QuickAccessSettingsDialog dialog(settings);
        const auto result = dialog.settings();
        QCOMPARE(result.columns, 6);
        QCOMPARE(result.dialogWidth, 550);
        QCOMPARE(result.dialogHeight, 650);
        QCOMPARE(result.altEraseKey, settings.altEraseKey);
        QCOMPARE(result.blendModes, settings.blendModes);
        QCOMPARE(result.tempBrushSets, settings.tempBrushSets);
    }
};
QTEST_MAIN(QuickAccessItemPropertiesTest)
#include "QuickAccessItemPropertiesTest.moc"
