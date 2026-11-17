/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "QuickAccessLayoutEngine.h"
#include "QuickAccessProfileLoader.h"
#include "QuickAccessProfileRepository.h"
#include "QuickAccessSettingsStore.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <kconfiggroup.h>

using namespace QuickAccess;
namespace
{
bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
KSharedConfigPtr configFor(const QTemporaryDir &dir)
{
    return KSharedConfig::openConfig(dir.filePath("test.ini"), KConfig::SimpleConfig);
}
const QByteArray legacySettings = R"({
 "default":{"docker_icon_size":51,"header_button_color":"#123456","config_dialog_width":720,
            "huesvc_enabled":false,"quick_adjust_enabled":false},
 "popup":{"popup_icon_size":63},
 "huesvc":{"rgb_display_mode":"integer","popup_width":420,"popup_height":650,
           "controls_panel_width":230,"controls_panel_font_size":17},
 "quick_adjust":{"size_slider_enabled":false,"opacity_slider_enabled":false,
                 "flow_slider_enabled":false,"layer_opacity_slider_enabled":false,
                 "color_history_enabled":false,"color_history_total":9,"color_history_icon_size":25,
                 "brush_history_enabled":false,"brush_history_total":8,"brush_history_icon_size":29,
                 "font_size":"15px","alt_erase_key":"Alt + E","preserve_alpha_key":"F9",
                 "select_outline_key":"F10","tool_options_enabled":true,"tool_options_start_visible":true,
                 "tool_options_position":"right_align_top","blender_mode_list":["multiply","normal"],
                 "temp_brush_sets":[{"key":"Alt+B","brush":"saved preset","size_scale":1.5}]},
 "gesture":{"enabled":false,"minimum_pixels_to_move":37,"show_preview":false}
})";
} // namespace

class QuickAccessConfigTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void defaultsAndReadOnlyLoad()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto config = configFor(dir);
        QuickAccessSettingsStore store(config);
        const auto settings = store.load(5);
        QCOMPARE(settings.columns, 5);
        QCOMPARE(settings.dockerIconSize, 42);
        QCOMPARE(settings.dialogWidth, 550);
        QCOMPARE(settings.dialogHeight, 650);
        QCOMPARE(settings.headerButtonColor, QColor("#828282"));
        QVERIFY(settings.gestureEnabled && settings.hueSvcEnabled && settings.quickAdjustEnabled);
        QVERIFY(!settings.toolOptionsEnabled);
        QCOMPARE(settings.blendModes.size(), 10);
        QVERIFY(settings.tempBrushSets.isEmpty());
        QCOMPARE(store.migrationVersion(), 0);
        QVERIFY(config->groupList().isEmpty());
    }

    void migrationAndRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto config = configFor(dir);
        QuickAccessSettingsStore store(config);
        store.importLegacy(QJsonDocument::fromJson(legacySettings).object());
        const auto settings = store.load(3);
        QCOMPARE(store.migrationVersion(), 2);
        QCOMPARE(settings.dockerIconSize, 51);
        QCOMPARE(settings.popupIconSize, 63);
        QCOMPARE(settings.dialogWidth, 720);
        QCOMPARE(settings.dialogHeight, 650);
        QCOMPARE(settings.headerButtonColor, QColor("#123456"));
        QVERIFY(!settings.hueSvcEnabled && !settings.quickAdjustEnabled && !settings.gestureEnabled);
        QCOMPARE(settings.rgbDisplayMode, QStringLiteral("integer"));
        QCOMPARE(settings.huePopupWidth, 420);
        QCOMPARE(settings.huePopupHeight, 650);
        QCOMPARE(settings.hueControlsWidth, 230);
        QCOMPARE(settings.hueControlsFontSize, 17);
        QVERIFY(!settings.sizeSliderEnabled && !settings.opacitySliderEnabled && !settings.flowSliderEnabled);
        QVERIFY(!settings.layerOpacitySliderEnabled && !settings.colorHistoryEnabled && !settings.brushHistoryEnabled);
        QCOMPARE(settings.colorHistoryTotal, 9);
        QCOMPARE(settings.colorHistoryIconSize, 25);
        QCOMPARE(settings.brushHistoryTotal, 8);
        QCOMPARE(settings.brushHistoryIconSize, 29);
        QCOMPARE(settings.altEraseKey, QStringLiteral("Alt + E"));
        QCOMPARE(settings.preserveAlphaKey, QStringLiteral("F9"));
        QCOMPARE(settings.selectOutlineKey, QStringLiteral("F10"));
        QVERIFY(settings.toolOptionsEnabled);
        QCOMPARE(settings.toolOptionsPosition, QStringLiteral("right_align_top"));
        QCOMPARE(settings.blendModes, QStringList({"multiply", "normal"}));
        QCOMPARE(settings.tempBrushSets.size(), 1);
        QCOMPARE(config->group("QuickAccessGesture").readEntry("MinimumPixelsToMove", 0), 37);
        QVERIFY(!config->group("QuickAccessGesture").readEntry("ShowPreview", true));
        QCOMPARE(config->group("QuickAccessAdjust").readEntry("FontSize", QString()), QStringLiteral("15px"));
        QVERIFY(config->group("QuickAccessAdjust").readEntry("ToolOptionsStartVisible", false));

        auto updated = settings;
        updated.headerButtonColor = QColor(10, 20, 30, 40);
        updated.activeTabFontSize = 20;
        updated.inactiveTabFontColor = QColor("#998877");
        config->group("QuickAccessHueSVC").writeEntry("SelectorShape", "ring");
        store.save(updated);
        config->reparseConfiguration();
        const auto loaded = store.load(7);
        QCOMPARE(loaded.columns, 7); // Columns belong to the profile, not KConfig.
        QCOMPARE(loaded.headerButtonColor, updated.headerButtonColor);
        QCOMPARE(loaded.activeTabFontSize, 20);
        QCOMPARE(loaded.inactiveTabFontColor, updated.inactiveTabFontColor);
        QCOMPARE(loaded.tempBrushSets, settings.tempBrushSets);
        QCOMPARE(loaded.blendModes, settings.blendModes);
        QCOMPARE(config->group("QuickAccessHueSVC").readEntry("SelectorShape", QString()), QStringLiteral("ring"));
        QVERIFY(config->group("QuickAccessAdjust").readEntry("ToolOptionsStartVisible", false));
        QCOMPARE(config->group("QuickAccessGesture").readEntry("MinimumPixelsToMove", 0), 37);
        QVERIFY(!config->group("QuickAccess").hasKey("Columns"));
        config->group("QuickAccessAdjust").writeEntry("TempBrushSets", "{}");
        QVERIFY(store.load().tempBrushSets.isEmpty());
        config->group("QuickAccessAdjust").writeEntry("TempBrushSets", "invalid");
        QVERIFY(store.load().tempBrushSets.isEmpty());
    }

    void emptyLegacyBlendModesKeepConfiguredValue()
    {
        QTemporaryDir dir;
        auto config = configFor(dir);
        config->group("QuickAccessAdjust").writeEntry("BlendModes", QStringList({"screen"}));
        QuickAccessSettingsStore store(config);
        store.importLegacy({});
        QCOMPARE(store.load().blendModes, QStringList({"screen"}));
        QCOMPARE(store.load().dialogWidth, 550);
        QCOMPARE(store.load().dialogHeight, 650);
    }

    void profileImportAndUpgrade_data()
    {
        QTest::addColumn<bool>("existingNative");
        QTest::newRow("first-import") << false;
        QTest::newRow("upgrade-existing-profile") << true;
    }
    void profileImportAndUpgrade()
    {
        QFETCH(bool, existingNative);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto config = configFor(dir);
        QuickAccessSettingsStore store(config);
        Document legacy = Document::createDefault();
        legacy.tabs[0].name = "legacy";
        QVERIFY(write(dir.filePath("quick_access_palette.json"), QJsonDocument(legacy.toJson()).toJson()));
        QVERIFY(write(dir.filePath("settings.json"), legacySettings));
        const QString native = dir.filePath("default.kqap");
        if (existingNative) {
            Document current = legacy;
            current.tabs[0].name = "native";
            QVERIFY(ProfileRepository(native).save(current));
            config->group("QuickAccess").writeEntry("MigrationVersion", 1);
        }
        Document loaded;
        QStringList warnings;
        const auto warn = [&](const QString &, const QString &message) {
            warnings << message;
        };
        QuickAccessProfileLoader::load(&loaded, native, dir.path(), store, warn);
        QVERIFY(warnings.isEmpty());
        QCOMPARE(loaded.tabs[0].name, existingNative ? QStringLiteral("native") : QStringLiteral("legacy"));
        QCOMPARE(store.migrationVersion(), 2);
        QCOMPARE(store.load().dockerIconSize, 51);
        // Subsequent startup must not overwrite the user's newer preferences.
        config->group("QuickAccess").writeEntry("DockerIconSize", 68);
        QuickAccessProfileLoader::load(&loaded, native, dir.path(), store, warn);
        QCOMPARE(store.load().dockerIconSize, 68);
        QVERIFY(warnings.isEmpty());
    }

    void failedImportSaveDoesNotMarkMigrated()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto config = configFor(dir);
        QuickAccessSettingsStore store(config);
        Document legacy = Document::createDefault();
        legacy.tabs[0].name = "imported";
        QVERIFY(write(dir.filePath("quick_access_palette.json"), QJsonDocument(legacy.toJson()).toJson()));
        QVERIFY(write(dir.filePath("settings.json"), legacySettings));
        QVERIFY(write(dir.filePath("blocked"), "file, not directory"));
        Document loaded;
        QStringList warnings;
        QuickAccessProfileLoader::load(&loaded,
                                       dir.filePath("blocked/default.kqap"),
                                       dir.path(),
                                       store,
                                       [&](const QString &, const QString &message) {
                                           warnings << message;
                                       });
        QCOMPARE(loaded.tabs[0].name, QStringLiteral("imported"));
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(store.migrationVersion(), 0);
        QCOMPARE(store.load().dockerIconSize, 42);
    }

    void corruptNativeIsNotOverwritten()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("default.kqap");
        QVERIFY(write(path, "broken JSON"));
        QuickAccessSettingsStore store(configFor(dir));
        Document loaded;
        QStringList warnings;
        QuickAccessProfileLoader::load(&loaded, path, dir.path(), store, [&](const QString &, const QString &message) {
            warnings << message;
        });
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(read(path), QByteArray("broken JSON"));
        QVERIFY(!loaded.tabs.isEmpty());
        QCOMPARE(store.migrationVersion(), 0);
    }

    void defaultProfileAndBrushRepair()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("default.kqap");
        QuickAccessSettingsStore store(configFor(dir));
        Document loaded;
        QStringList warnings;
        const auto warn = [&](const QString &, const QString &message) {
            warnings << message;
        };
        QuickAccessProfileLoader::load(&loaded, path, dir.path(), store, warn);
        QVERIFY(warnings.isEmpty());
        QVERIFY(QFileInfo::exists(path));
        Grid &grid = loaded.tabs[0].grids[0];
        grid.columns = 4;
        Item brush;
        brush.id = "brush";
        brush.type = ItemType::Brush;
        brush.payload = QJsonObject{{"brush_name", "test"}, {"display", "stroke"}};
        brush.columnSpan = 1;
        Item neighbour;
        neighbour.id = "neighbour";
        neighbour.type = ItemType::Action;
        neighbour.payload = QJsonObject{{"action_id", "undo"}};
        neighbour.column = 1;
        grid.items = {brush, neighbour};
        // Write a historical one-cell stroke-preview profile directly.
        QVERIFY(write(path, QJsonDocument(loaded.toJson()).toJson()));
        QuickAccessProfileLoader::load(&loaded, path, dir.path(), store, warn);
        const Grid &repaired = loaded.tabs[0].grids[0];
        QVERIFY(LayoutEngine(4).validate(repaired.items).isValid());
        QCOMPARE(repaired.items.size(), 2);
        QCOMPARE(repaired.items[0].columnSpan, 2);
        Document saved;
        QVERIFY(ProfileRepository(path).load(&saved));
        QCOMPARE(saved, loaded);
        QVERIFY(warnings.isEmpty());
    }

    void wideIconActionLoadsAsOneCell()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString icon = dir.filePath("custom.png");
        QVERIFY(write(icon, "fixture"));
        const QString path = dir.filePath("default.kqap");
        Document document = Document::createDefault();
        Grid &grid = document.tabs[0].grids[0];
        grid.columns = 4;
        Item iconAction;
        iconAction.id = "icon";
        iconAction.type = ItemType::Action;
        iconAction.payload = QJsonObject{{"action_id", "undo"}};
        iconAction.columnSpan = 2;
        Item textAction = iconAction;
        textAction.id = "text";
        textAction.payload = QJsonObject{{"action_id", "redo"}};
        textAction.column = 2;
        grid.items = {iconAction, textAction};
        document.aliases = QJsonObject{{"actions", QJsonObject{{"undo", QJsonObject{{"icon_name", icon}}}}}};
        // A profile saved while an icon action could keep two cells.
        QVERIFY(write(path, QJsonDocument(document.toJson()).toJson()));
        QuickAccessSettingsStore store(configFor(dir));
        Document loaded;
        QStringList warnings;
        QuickAccessProfileLoader::load(&loaded, path, dir.path(), store, [&](const QString &, const QString &message) {
            warnings << message;
        });
        QVERIFY(warnings.isEmpty());
        const Grid &normalized = loaded.tabs[0].grids[0];
        QCOMPARE(normalized.items[0].columnSpan, 1);
        QCOMPARE(normalized.items[0].column, 0);
        QCOMPARE(normalized.items[1].columnSpan, 2);
        QCOMPARE(normalized.items[1].column, 2);
        Document saved;
        QVERIFY(ProfileRepository(path).load(&saved));
        QCOMPARE(saved, loaded);
    }
};
QTEST_GUILESS_MAIN(QuickAccessConfigTest)
#include "QuickAccessConfigTest.moc"
