/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RESTNOTESETTINGSDIALOG_H
#define RESTNOTESETTINGSDIALOG_H

struct RestNoteConfig;
class QWidget;

namespace RestNoteSettingsDialog
{
// Edits the value only on acceptance; persistence and timer updates belong to the dock.
bool edit(RestNoteConfig &config, QWidget *parent);
} // namespace RestNoteSettingsDialog

#endif
