/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessLegacyImporter.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <klocalizedstring.h>

namespace QuickAccess
{

bool LegacyImporter::importDirectory(const QString &remasterDirectory, LegacyImportResult *result, QString *error)
{
    if (!result)
        return false;
    const QDir directory(remasterDirectory);
    QJsonObject palette;
    if (!readObject(directory.filePath(QStringLiteral("quick_access_palette.json")), &palette, error))
        return false;

    LegacyImportResult imported;
    if (!Document::fromJson(palette, &imported.document, error))
        return false;

    QJsonObject settings;
    const QString settingsPath = directory.filePath(QStringLiteral("settings.json"));
    if (QFile::exists(settingsPath)) {
        if (!readObject(settingsPath, &settings, error))
            return false;
    } else {
        imported.warnings.append(QStringLiteral("Legacy settings.json was not found."));
    }

    QJsonObject aliases;
    const QString aliasesPath = directory.filePath(QStringLiteral("alias_config.json"));
    if (QFile::exists(aliasesPath)) {
        if (!readObject(aliasesPath, &aliases, error))
            return false;
        imported.document.aliases = aliases;
    } else {
        imported.warnings.append(QStringLiteral("Legacy alias_config.json was not found."));
    }

    const QDir gestureDirectory(directory.filePath(QStringLiteral("gesture")));
    const QString gestureSettingsPath = gestureDirectory.filePath(QStringLiteral("gesture.json"));
    if (QFile::exists(gestureSettingsPath)) {
        QJsonObject gestureSettings;
        if (!readObject(gestureSettingsPath, &gestureSettings, error))
            return false;
        settings.insert(QStringLiteral("gesture"), gestureSettings);
    }

    const QDir pagesDirectory(gestureDirectory.filePath(QStringLiteral("config")));
    const QStringList pages = pagesDirectory.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString &pageName : pages) {
        QJsonObject page;
        if (!readObject(pagesDirectory.filePath(pageName), &page, error))
            return false;
        page.insert(QStringLiteral("legacyFileName"), pageName);
        imported.document.gesturePages.append(page);
    }

    imported.settings = settings;
    *result = imported;
    return true;
}

bool LegacyImporter::readObject(const QString &path, QJsonObject *object, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = i18n("Could not read '%1': %2", path, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = i18n("Could not parse '%1': %2", path, parseError.errorString());
        return false;
    }
    *object = document.object();
    return true;
}

} // namespace QuickAccess
