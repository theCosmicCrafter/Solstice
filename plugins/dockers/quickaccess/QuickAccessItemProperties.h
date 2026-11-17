/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSITEMPROPERTIES_H
#define QUICKACCESSITEMPROPERTIES_H
#include "QuickAccessModel.h"
class QWidget;
class QuickAccessItemProperties
{
public:
    // Return true only when the caller should save and rebuild the palette.
    static bool edit(QuickAccess::Document &document,
                     QuickAccess::Grid &grid,
                     const QString &itemId,
                     const QString &displayText,
                     QWidget *parent);
    static bool editBrush(QuickAccess::Grid &grid, const QString &itemId, QWidget *parent);
};
#endif
