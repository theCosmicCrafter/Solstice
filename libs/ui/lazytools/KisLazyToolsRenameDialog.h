/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_LAZY_TOOLS_RENAME_DIALOG_H
#define KIS_LAZY_TOOLS_RENAME_DIALOG_H

#include "KisLazyToolsRenamePresets.h"
#include <functional>

class QDialog;
class QWidget;

namespace KisLazyToolsRenameDialog
{
using Apply = std::function<bool(const QString &name, int label)>;
// The caller owns the modal dialog. It closes only after Apply succeeds.
KRITAUI_EXPORT QDialog *
create(QWidget *parent, const Apply &apply, const QString &presetPath = KisLazyToolsRenamePresets::filePath());
} // namespace KisLazyToolsRenameDialog

#endif
