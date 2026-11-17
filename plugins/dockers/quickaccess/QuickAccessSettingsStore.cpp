/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessSettingsStore.h"
#include <QJsonDocument>
#include <kconfiggroup.h>
#include <utility>
namespace
{
constexpr int DefaultIconSize = 42;
// The settings dialog's size when none is stored: the legacy Python plugin's
// default, also used when its settings are imported without a size.
constexpr int DefaultSettingsDialogWidth = 550;
constexpr int DefaultSettingsDialogHeight = 650;

} // namespace
QuickAccessSettingsStore::QuickAccessSettingsStore(KSharedConfigPtr config)
    : m_config(std::move(config))
{
}
int QuickAccessSettingsStore::migrationVersion() const
{
    return m_config->group(QStringLiteral("QuickAccess")).readEntry("MigrationVersion", 0);
}
QuickAccessSettings QuickAccessSettingsStore::load(int columns) const
{
    KConfigGroup config = m_config->group(QStringLiteral("QuickAccess"));
    QuickAccessSettings current;
    current.columns = columns;
    current.dockerIconSize = config.readEntry("DockerIconSize", DefaultIconSize);
    current.headerButtonColor = QColor(config.readEntry("HeaderButtonColor", QStringLiteral("#828282")));
    current.headerButtonFontColor = QColor(config.readEntry("HeaderButtonFontColor", QStringLiteral("#ffffff")));
    current.activeTabFontSize = config.readEntry("ActiveTabFontSize", 12);
    current.activeTabFontColor = QColor(config.readEntry("ActiveTabFontColor", QStringLiteral("#ffffff")));
    current.activeTabBackgroundColor = QColor(config.readEntry("ActiveTabBackgroundColor", QStringLiteral("#3f3f3f")));
    current.inactiveTabFontSize = config.readEntry("InactiveTabFontSize", 12);
    current.inactiveTabFontColor = QColor(config.readEntry("InactiveTabFontColor", QStringLiteral("#a0a0a0")));
    current.inactiveTabBackgroundColor =
        QColor(config.readEntry("InactiveTabBackgroundColor", QStringLiteral("#2b2b2b")));
    current.dialogWidth = config.readEntry("SettingsDialogWidth", DefaultSettingsDialogWidth);
    current.dialogHeight = config.readEntry("SettingsDialogHeight", DefaultSettingsDialogHeight);
    const KConfigGroup gestureConfig = m_config->group(QStringLiteral("QuickAccessGesture"));
    current.gestureEnabled = gestureConfig.readEntry("Enabled", true);
    current.hueSvcEnabled = config.readEntry("HueSVCEnabled", true);
    current.quickAdjustEnabled = config.readEntry("QuickAdjustEnabled", true);
    current.popupIconSize = config.readEntry("PopupIconSize", DefaultIconSize);
    const KConfigGroup hueConfig = m_config->group(QStringLiteral("QuickAccessHueSVC"));
    current.rgbDisplayMode = hueConfig.readEntry("RgbDisplayMode", QStringLiteral("percentage"));
    current.huePopupWidth = hueConfig.readEntry("PopupWidth", 350);
    current.huePopupHeight = hueConfig.readEntry("PopupHeight", 600);
    current.hueControlsWidth = hueConfig.readEntry("ControlsPanelWidth", 220);
    current.hueControlsFontSize = hueConfig.readEntry("ControlsPanelFontSize", 16);
    const KConfigGroup adjustConfig = m_config->group(QStringLiteral("QuickAccessAdjust"));
    current.sizeSliderEnabled = adjustConfig.readEntry("SizeSliderEnabled", true);
    current.opacitySliderEnabled = adjustConfig.readEntry("OpacitySliderEnabled", true);
    current.flowSliderEnabled = adjustConfig.readEntry("FlowSliderEnabled", true);
    current.layerOpacitySliderEnabled = adjustConfig.readEntry("LayerOpacitySliderEnabled", true);
    current.colorHistoryEnabled = adjustConfig.readEntry("ColorHistoryEnabled", true);
    current.colorHistoryTotal = adjustConfig.readEntry("ColorHistoryTotal", 14);
    current.colorHistoryIconSize = adjustConfig.readEntry("ColorHistoryIconSize", 30);
    current.brushHistoryEnabled = adjustConfig.readEntry("BrushHistoryEnabled", true);
    current.brushHistoryTotal = adjustConfig.readEntry("BrushHistoryTotal", 14);
    current.brushHistoryIconSize = adjustConfig.readEntry("BrushHistoryIconSize", 34);
    current.altEraseKey = adjustConfig.readEntry("AltEraseKey", QString());
    current.preserveAlphaKey = adjustConfig.readEntry("PreserveAlphaKey", QString());
    current.selectOutlineKey = adjustConfig.readEntry("SelectOutlineKey", QString());
    current.toolOptionsEnabled = adjustConfig.readEntry("ToolOptionsEnabled", false);
    current.toolOptionsPosition = adjustConfig.readEntry("ToolOptionsPosition", QStringLiteral("left_align_top"));
    current.blendModes = adjustConfig.readEntry("BlendModes", current.blendModes);
    const QJsonDocument brushSets =
        QJsonDocument::fromJson(adjustConfig.readEntry("TempBrushSets", QStringLiteral("[]")).toUtf8());
    current.tempBrushSets = brushSets.isArray() ? brushSets.array() : QJsonArray();

    return current;
}
void QuickAccessSettingsStore::save(const QuickAccessSettings &updated)
{
    KConfigGroup config = m_config->group(QStringLiteral("QuickAccess"));
    config.writeEntry("DockerIconSize", updated.dockerIconSize);
    config.writeEntry("HeaderButtonColor", updated.headerButtonColor.name(QColor::HexArgb));
    config.writeEntry("HeaderButtonFontColor", updated.headerButtonFontColor.name(QColor::HexArgb));
    config.writeEntry("ActiveTabFontSize", updated.activeTabFontSize);
    config.writeEntry("ActiveTabFontColor", updated.activeTabFontColor.name(QColor::HexArgb));
    config.writeEntry("ActiveTabBackgroundColor", updated.activeTabBackgroundColor.name(QColor::HexArgb));
    config.writeEntry("InactiveTabFontSize", updated.inactiveTabFontSize);
    config.writeEntry("InactiveTabFontColor", updated.inactiveTabFontColor.name(QColor::HexArgb));
    config.writeEntry("InactiveTabBackgroundColor", updated.inactiveTabBackgroundColor.name(QColor::HexArgb));
    config.writeEntry("SettingsDialogWidth", updated.dialogWidth);
    config.writeEntry("SettingsDialogHeight", updated.dialogHeight);
    config.writeEntry("PopupIconSize", updated.popupIconSize);
    config.writeEntry("HueSVCEnabled", updated.hueSvcEnabled);
    config.writeEntry("QuickAdjustEnabled", updated.quickAdjustEnabled);
    KConfigGroup gestureWrite = m_config->group(QStringLiteral("QuickAccessGesture"));
    gestureWrite.writeEntry("Enabled", updated.gestureEnabled);
    gestureWrite.sync();
    KConfigGroup hueWrite = m_config->group(QStringLiteral("QuickAccessHueSVC"));
    hueWrite.writeEntry("RgbDisplayMode", updated.rgbDisplayMode);
    hueWrite.writeEntry("PopupWidth", updated.huePopupWidth);
    hueWrite.writeEntry("PopupHeight", updated.huePopupHeight);
    hueWrite.writeEntry("ControlsPanelWidth", updated.hueControlsWidth);
    hueWrite.writeEntry("ControlsPanelFontSize", updated.hueControlsFontSize);
    hueWrite.sync();
    KConfigGroup adjustWrite = m_config->group(QStringLiteral("QuickAccessAdjust"));
    adjustWrite.writeEntry("SizeSliderEnabled", updated.sizeSliderEnabled);
    adjustWrite.writeEntry("OpacitySliderEnabled", updated.opacitySliderEnabled);
    adjustWrite.writeEntry("FlowSliderEnabled", updated.flowSliderEnabled);
    adjustWrite.writeEntry("LayerOpacitySliderEnabled", updated.layerOpacitySliderEnabled);
    adjustWrite.writeEntry("ColorHistoryEnabled", updated.colorHistoryEnabled);
    adjustWrite.writeEntry("ColorHistoryTotal", updated.colorHistoryTotal);
    adjustWrite.writeEntry("ColorHistoryIconSize", updated.colorHistoryIconSize);
    adjustWrite.writeEntry("BrushHistoryEnabled", updated.brushHistoryEnabled);
    adjustWrite.writeEntry("BrushHistoryTotal", updated.brushHistoryTotal);
    adjustWrite.writeEntry("BrushHistoryIconSize", updated.brushHistoryIconSize);
    adjustWrite.writeEntry("AltEraseKey", updated.altEraseKey);
    adjustWrite.writeEntry("PreserveAlphaKey", updated.preserveAlphaKey);
    adjustWrite.writeEntry("SelectOutlineKey", updated.selectOutlineKey);
    adjustWrite.writeEntry("ToolOptionsEnabled", updated.toolOptionsEnabled);
    adjustWrite.writeEntry("ToolOptionsPosition", updated.toolOptionsPosition);
    adjustWrite.writeEntry("BlendModes", updated.blendModes);
    adjustWrite.writeEntry("TempBrushSets",
                           QString::fromUtf8(QJsonDocument(updated.tempBrushSets).toJson(QJsonDocument::Compact)));
    adjustWrite.sync();
    config.sync();
}

void QuickAccessSettingsStore::importLegacy(const QJsonObject &settings)
{
    KConfigGroup config = m_config->group(QStringLiteral("QuickAccess"));
    const QJsonObject defaults = settings.value(QStringLiteral("default")).toObject();
    config.writeEntry("DockerIconSize", defaults.value(QStringLiteral("docker_icon_size")).toInt(DefaultIconSize));
    config.writeEntry("HeaderButtonColor",
                      defaults.value(QStringLiteral("header_button_color")).toString(QStringLiteral("#828282")));
    config.writeEntry("HeaderButtonFontColor",
                      defaults.value(QStringLiteral("header_button_font_color")).toString(QStringLiteral("#ffffff")));
    config.writeEntry("ActiveTabFontSize", defaults.value(QStringLiteral("tab_active_font_size")).toInt(12));
    config.writeEntry("ActiveTabFontColor",
                      defaults.value(QStringLiteral("tab_active_font_color")).toString(QStringLiteral("#ffffff")));
    config.writeEntry(
        "ActiveTabBackgroundColor",
        defaults.value(QStringLiteral("tab_active_background_color")).toString(QStringLiteral("#3f3f3f")));
    config.writeEntry("InactiveTabFontSize", defaults.value(QStringLiteral("tab_inactive_font_size")).toInt(12));
    config.writeEntry("InactiveTabFontColor",
                      defaults.value(QStringLiteral("tab_inactive_font_color")).toString(QStringLiteral("#a0a0a0")));
    config.writeEntry(
        "InactiveTabBackgroundColor",
        defaults.value(QStringLiteral("tab_inactive_background_color")).toString(QStringLiteral("#2b2b2b")));
    config.writeEntry("SettingsDialogWidth",
                      defaults.value(QStringLiteral("config_dialog_width")).toInt(DefaultSettingsDialogWidth));
    config.writeEntry("SettingsDialogHeight",
                      defaults.value(QStringLiteral("config_dialog_height")).toInt(DefaultSettingsDialogHeight));
    config.writeEntry("PopupIconSize",
                      settings.value(QStringLiteral("popup"))
                          .toObject()
                          .value(QStringLiteral("popup_icon_size"))
                          .toInt(DefaultIconSize));
    config.writeEntry("HueSVCEnabled", defaults.value(QStringLiteral("huesvc_enabled")).toBool(true));
    config.writeEntry("QuickAdjustEnabled", defaults.value(QStringLiteral("quick_adjust_enabled")).toBool(true));
    config.writeEntry("MigrationVersion", 2);

    const QJsonObject hue = settings.value(QStringLiteral("huesvc")).toObject();
    KConfigGroup hueConfig = m_config->group(QStringLiteral("QuickAccessHueSVC"));
    hueConfig.writeEntry("RgbDisplayMode",
                         hue.value(QStringLiteral("rgb_display_mode")).toString(QStringLiteral("percentage")));
    hueConfig.writeEntry("PopupWidth", hue.value(QStringLiteral("popup_width")).toInt(350));
    hueConfig.writeEntry("PopupHeight", hue.value(QStringLiteral("popup_height")).toInt(600));
    hueConfig.writeEntry("ControlsPanelWidth", hue.value(QStringLiteral("controls_panel_width")).toInt(220));
    hueConfig.writeEntry("ControlsPanelFontSize", hue.value(QStringLiteral("controls_panel_font_size")).toInt(16));

    const QJsonObject adjust = settings.value(QStringLiteral("quick_adjust")).toObject();
    KConfigGroup adjustConfig = m_config->group(QStringLiteral("QuickAccessAdjust"));
    const QList<QPair<QString, QString>> booleans{
        {QStringLiteral("size_slider_enabled"), QStringLiteral("SizeSliderEnabled")},
        {QStringLiteral("opacity_slider_enabled"), QStringLiteral("OpacitySliderEnabled")},
        {QStringLiteral("flow_slider_enabled"), QStringLiteral("FlowSliderEnabled")},
        {QStringLiteral("layer_opacity_slider_enabled"), QStringLiteral("LayerOpacitySliderEnabled")},
        {QStringLiteral("color_history_enabled"), QStringLiteral("ColorHistoryEnabled")},
        {QStringLiteral("brush_history_enabled"), QStringLiteral("BrushHistoryEnabled")},
        {QStringLiteral("tool_options_enabled"), QStringLiteral("ToolOptionsEnabled")},
        {QStringLiteral("tool_options_start_visible"), QStringLiteral("ToolOptionsStartVisible")}};
    for (const auto &entry : booleans) {
        const bool defaultValue = !entry.first.startsWith(QStringLiteral("tool_options"));
        adjustConfig.writeEntry(entry.second, adjust.value(entry.first).toBool(defaultValue));
    }
    adjustConfig.writeEntry("ColorHistoryTotal", adjust.value(QStringLiteral("color_history_total")).toInt(14));
    adjustConfig.writeEntry("ColorHistoryIconSize", adjust.value(QStringLiteral("color_history_icon_size")).toInt(30));
    adjustConfig.writeEntry("BrushHistoryTotal", adjust.value(QStringLiteral("brush_history_total")).toInt(14));
    adjustConfig.writeEntry("BrushHistoryIconSize", adjust.value(QStringLiteral("brush_history_icon_size")).toInt(34));
    adjustConfig.writeEntry("FontSize", adjust.value(QStringLiteral("font_size")).toString(QStringLiteral("12px")));
    adjustConfig.writeEntry("AltEraseKey", adjust.value(QStringLiteral("alt_erase_key")).toString());
    adjustConfig.writeEntry("PreserveAlphaKey", adjust.value(QStringLiteral("preserve_alpha_key")).toString());
    adjustConfig.writeEntry("SelectOutlineKey", adjust.value(QStringLiteral("select_outline_key")).toString());
    adjustConfig.writeEntry(
        "ToolOptionsPosition",
        adjust.value(QStringLiteral("tool_options_position")).toString(QStringLiteral("left_align_top")));
    adjustConfig.writeEntry(
        "TempBrushSets",
        QString::fromUtf8(
            QJsonDocument(adjust.value(QStringLiteral("temp_brush_sets")).toArray()).toJson(QJsonDocument::Compact)));
    QStringList blendModes;
    for (const QJsonValue &value : adjust.value(QStringLiteral("blender_mode_list")).toArray())
        blendModes.append(value.toString());
    if (!blendModes.isEmpty())
        adjustConfig.writeEntry("BlendModes", blendModes);

    const QJsonObject gesture = settings.value(QStringLiteral("gesture")).toObject();
    KConfigGroup gestureConfig = m_config->group(QStringLiteral("QuickAccessGesture"));
    gestureConfig.writeEntry("Enabled", gesture.value(QStringLiteral("enabled")).toBool(true));
    gestureConfig.writeEntry("MinimumPixelsToMove", gesture.value(QStringLiteral("minimum_pixels_to_move")).toInt(20));
    gestureConfig.writeEntry("ShowPreview", gesture.value(QStringLiteral("show_preview")).toBool(true));
    config.sync();
    hueConfig.sync();
    adjustConfig.sync();
    gestureConfig.sync();
}
