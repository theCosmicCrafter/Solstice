/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RESTNOTENOTIFICATIONS_H
#define RESTNOTENOTIFICATIONS_H

#include <functional>

struct RestNoteConfig;
class QWidget;
class QScreen;

namespace RestNoteNotifications
{
QWidget *createBreakOverlay(const RestNoteConfig &config, QWidget *host, std::function<void()> finished);
QWidget *createEyeBreak(const RestNoteConfig &config, QScreen *screen, std::function<void()> finished);
// Cancels completion before the existing fade-out; the toast deletes itself.
void cancelEyeBreak(QWidget *toast);
} // namespace RestNoteNotifications

#endif
