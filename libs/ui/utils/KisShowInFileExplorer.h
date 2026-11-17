/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_SHOW_IN_FILE_EXPLORER_H
#define KIS_SHOW_IN_FILE_EXPLORER_H

#include <QString>

#include <kritaui_export.h>

class QWidget;

namespace KisShowInFileExplorer
{
/// Localized menu label for the action ("Show in File Explorer").
KRITAUI_EXPORT QString actionText();

/**
 * Opens the folder that contains @p filePath in the system file manager.
 * On Windows the file is selected in Explorer. When the file no longer
 * exists, its folder is opened if that still exists; otherwise a warning
 * is shown on @p parentWidget.
 */
KRITAUI_EXPORT void showFile(const QString &filePath, QWidget *parentWidget);
} // namespace KisShowInFileExplorer

#endif
