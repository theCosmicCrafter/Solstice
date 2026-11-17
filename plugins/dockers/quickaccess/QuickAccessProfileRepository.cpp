/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessProfileRepository.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <klocalizedstring.h>

namespace QuickAccess
{

ProfileRepository::ProfileRepository(QString path)
    : m_path(std::move(path))
{
}

QString ProfileRepository::path() const
{
    return m_path;
}
bool ProfileRepository::exists() const
{
    return QFileInfo::exists(m_path);
}

bool ProfileRepository::load(Document *document, QString *error) const
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        if (error)
            *error = parseError.errorString();
        return false;
    }
    return Document::fromJson(json.object(), document, error);
}

bool ProfileRepository::save(const Document &document, QString *error) const
{
    const QFileInfo info(m_path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error)
            *error = i18n("Could not create the profile folder '%1'.", info.absolutePath());
        return false;
    }
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray data = QJsonDocument(document.toJson()).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace QuickAccess
