/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSSETTINGSSTORE_H
#define QUICKACCESSSETTINGSSTORE_H
#include "QuickAccessSettings.h"
#include <QJsonObject>
#include <ksharedconfig.h>
// Owns persistence; callers own layout changes and controller reloads.
class QuickAccessSettingsStore
{
public:
    explicit QuickAccessSettingsStore(KSharedConfigPtr config = KSharedConfig::openConfig());
    QuickAccessSettings load(int columns = 8) const;
    void save(const QuickAccessSettings &updated);
    void importLegacy(const QJsonObject &settings);
    int migrationVersion() const;

private:
    KSharedConfigPtr m_config;
};
#endif
