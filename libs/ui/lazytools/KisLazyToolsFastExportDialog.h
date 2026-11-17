/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_LAZY_TOOLS_FAST_EXPORT_DIALOG_H
#define KIS_LAZY_TOOLS_FAST_EXPORT_DIALOG_H

#include "KisLazyToolsFastExport.h"

class QDialog;
class QWidget;

namespace KisLazyToolsFastExportDialog
{
using Documents = std::function<QList<QPointer<KisDocument>>(bool allDocuments)>;
// Resolves documents when Export is pressed, not when the modeless dialog opens.
// As before, the dialog is parented to the main window and deletes on close.
// The saved exports listed and edited are those of presetDocument, normally
// the document active when the dialog opens; without it the list is disabled.
KRITAUI_EXPORT QDialog *create(QWidget *parent,
                               const Documents &documents,
                               const KisLazyToolsFastExport::WaitForImage &wait,
                               KisDocument *presetDocument = nullptr);
} // namespace KisLazyToolsFastExportDialog

#endif
