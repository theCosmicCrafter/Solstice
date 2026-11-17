/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_LAZY_TOOLS_RENAME_PRESETS_H
#define KIS_LAZY_TOOLS_RENAME_PRESETS_H

#include <QList>
#include <QString>
#include <kritaui_export.h>

namespace KisLazyToolsRenamePresets
{
struct Preset {
    QString name;
    int label{0};
};

KRITAUI_EXPORT int colorLabelIndex(const QString &name);
KRITAUI_EXPORT QString colorLabelName(int index);
KRITAUI_EXPORT QString filePath();
KRITAUI_EXPORT QList<Preset> load(const QString &path, bool *opened = nullptr);
// Preserves raw existing lines and suppresses exact-line duplicates, as before.
KRITAUI_EXPORT void append(const QString &path, const QString &name, int label);
// Whole-file text for the preferences editor, written back unchanged in form,
// like the Python plugin's Name List tab.
KRITAUI_EXPORT QString readText(const QString &path, bool *opened = nullptr);
KRITAUI_EXPORT bool writeText(const QString &path, const QString &text);
} // namespace KisLazyToolsRenamePresets

#endif
