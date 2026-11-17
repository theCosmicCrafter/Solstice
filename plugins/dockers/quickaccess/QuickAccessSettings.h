/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSSETTINGS_H
#define QUICKACCESSSETTINGS_H
#include <QColor>
#include <QJsonArray>
#include <QStringList>
struct QuickAccessSettings {
    int columns{8};
    int dockerIconSize{42};
    QColor headerButtonColor{QStringLiteral("#828282")};
    QColor headerButtonFontColor{Qt::white};
    int activeTabFontSize{12};
    QColor activeTabFontColor{Qt::white};
    QColor activeTabBackgroundColor{QStringLiteral("#3f3f3f")};
    int inactiveTabFontSize{12};
    QColor inactiveTabFontColor{QStringLiteral("#a0a0a0")};
    QColor inactiveTabBackgroundColor{QStringLiteral("#2b2b2b")};
    int dialogWidth{340};
    int dialogHeight{480};
    bool gestureEnabled{true};
    bool hueSvcEnabled{true};
    bool quickAdjustEnabled{true};
    int popupIconSize{42};
    QString rgbDisplayMode{QStringLiteral("percentage")};
    int huePopupWidth{350};
    int huePopupHeight{600};
    int hueControlsWidth{220};
    int hueControlsFontSize{16};
    bool sizeSliderEnabled{true};
    bool opacitySliderEnabled{true};
    bool flowSliderEnabled{true};
    bool layerOpacitySliderEnabled{true};
    bool colorHistoryEnabled{true};
    int colorHistoryTotal{14};
    int colorHistoryIconSize{30};
    bool brushHistoryEnabled{true};
    int brushHistoryTotal{14};
    int brushHistoryIconSize{34};
    QString altEraseKey;
    QString preserveAlphaKey;
    QString selectOutlineKey;
    bool toolOptionsEnabled{false};
    QString toolOptionsPosition{QStringLiteral("left_align_top")};
    QStringList blendModes{QStringLiteral("normal"),
                           QStringLiteral("multiply"),
                           QStringLiteral("screen"),
                           QStringLiteral("dodge"),
                           QStringLiteral("overlay"),
                           QStringLiteral("soft_light_svg"),
                           QStringLiteral("hard_light"),
                           QStringLiteral("darken"),
                           QStringLiteral("lighten"),
                           QStringLiteral("greater")};
    QJsonArray tempBrushSets;
};
#endif
