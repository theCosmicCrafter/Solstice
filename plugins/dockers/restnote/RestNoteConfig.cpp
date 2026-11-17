/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RestNoteConfig.h"

#include <KoResourcePaths.h>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace
{
int jsonInt(const QJsonObject &object, const char *key, int fallback)
{
    return object.value(QLatin1String(key)).toInt(fallback);
}

} // namespace

int RestNoteConfig::workSeconds() const
{
    return workMinutes * 60;
}

int RestNoteConfig::breakSeconds() const
{
    return breakMinutes * 60;
}

int RestNoteConfig::microIntervalSeconds() const
{
    return microIntervalMinutes * 60;
}

QString RestNoteConfig::filePath()
{
    return QDir(KoResourcePaths::saveLocation("data", "rest_note/config/", true)).filePath(QStringLiteral("main.json"));
}

void RestNoteConfig::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        save(path);
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
    workMinutes = jsonInt(o, "work_minutes", workMinutes);
    breakMinutes = jsonInt(o, "break_minutes", breakMinutes);
    microEnabled = o.value(QStringLiteral("micro_break_enabled")).toBool(microEnabled);
    microIntervalMinutes = jsonInt(o, "micro_break_interval_minutes", microIntervalMinutes);
    microDurationSeconds = jsonInt(o, "micro_break_duration_seconds", microDurationSeconds);
    microSkipThresholdSeconds = jsonInt(o, "micro_skip_threshold_seconds", microSkipThresholdSeconds);
    idleEnabled = o.value(QStringLiteral("idle_enabled")).toBool(idleEnabled);
    idleThresholdSeconds = jsonInt(o, "idle_threshold_seconds", idleThresholdSeconds);
    toastMargin = jsonInt(o, "micro_toast_margin", toastMargin);
    toastWidth = jsonInt(o, "micro_toast_width", toastWidth);
    toastHeight = jsonInt(o, "micro_toast_height", toastHeight);
    toastTitleFontSize = jsonInt(o, "micro_toast_title_font_size", toastTitleFontSize);
    toastMessageFontSize = jsonInt(o, "micro_toast_message_font_size", toastMessageFontSize);
    overlayTitleFontSize = jsonInt(o, "overlay_title_font_size", overlayTitleFontSize);
    overlayMessageFontSize = jsonInt(o, "overlay_message_font_size", overlayMessageFontSize);
    overlaySkipFontSize = jsonInt(o, "overlay_skip_font_size", overlaySkipFontSize);
}

bool RestNoteConfig::save(const QString &path) const
{
    QJsonObject o{{QStringLiteral("work_minutes"), workMinutes},
                  {QStringLiteral("break_minutes"), breakMinutes},
                  {QStringLiteral("micro_break_enabled"), microEnabled},
                  {QStringLiteral("micro_break_interval_minutes"), microIntervalMinutes},
                  {QStringLiteral("micro_break_duration_seconds"), microDurationSeconds},
                  {QStringLiteral("micro_skip_threshold_seconds"), microSkipThresholdSeconds},
                  {QStringLiteral("idle_enabled"), idleEnabled},
                  {QStringLiteral("idle_threshold_seconds"), idleThresholdSeconds},
                  {QStringLiteral("micro_toast_margin"), toastMargin},
                  {QStringLiteral("micro_toast_width"), toastWidth},
                  {QStringLiteral("micro_toast_height"), toastHeight},
                  {QStringLiteral("micro_toast_title_font_size"), toastTitleFontSize},
                  {QStringLiteral("micro_toast_message_font_size"), toastMessageFontSize},
                  {QStringLiteral("overlay_title_font_size"), overlayTitleFontSize},
                  {QStringLiteral("overlay_message_font_size"), overlayMessageFontSize},
                  {QStringLiteral("overlay_skip_font_size"), overlaySkipFontSize}};
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(QJsonDocument(o).toJson()) >= 0;
}
