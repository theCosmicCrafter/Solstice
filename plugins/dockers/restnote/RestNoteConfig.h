/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RESTNOTECONFIG_H
#define RESTNOTECONFIG_H

#include <QString>

struct RestNoteConfig {
    int workMinutes{50};
    int breakMinutes{10};
    bool microEnabled{true};
    int microIntervalMinutes{20};
    int microDurationSeconds{20};
    int microSkipThresholdSeconds{180};
    bool idleEnabled{true};
    int idleThresholdSeconds{45};
    int toastMargin{128};
    int toastWidth{480};
    int toastHeight{220};
    int toastTitleFontSize{28};
    int toastMessageFontSize{24};
    int overlayTitleFontSize{50};
    int overlayMessageFontSize{32};
    int overlaySkipFontSize{18};

    int workSeconds() const;
    int breakSeconds() const;
    int microIntervalSeconds() const;
    static QString filePath();
    void load(const QString &path = filePath());
    bool save(const QString &path = filePath()) const;
};

#endif
