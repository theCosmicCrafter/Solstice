/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSPROFILELOADER_H
#define QUICKACCESSPROFILELOADER_H
#include "QuickAccessModel.h"
#include <functional>
class QuickAccessSettingsStore;
class QuickAccessProfileLoader
{
public:
    using WarningHandler = std::function<void(const QString &title, const QString &message)>;
    static void load(QuickAccess::Document *document,
                     const QString &profilePath,
                     const QString &legacyDirectory,
                     QuickAccessSettingsStore &settings,
                     const WarningHandler &warning);
};
#endif
