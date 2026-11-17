/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "KisReferenceFilterTemplates.h"
#include <KisSolsticePaths.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <klocalizedstring.h>

namespace
{
QString key(const QString &name)
{
    return name.trimmed().normalized(QString::NormalizationForm_C).toCaseFolded();
}
bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}
bool decode(const QByteArray &bytes, KisReferenceFilterTemplates::Template *result)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const auto root = document.object();
    if (root.value("version").toInt(-1) != 1 || !root.value("name").isString()
        || key(root.value("name").toString()).isEmpty() || !root.value("enabled").isBool()
        || !root.value("effects").isArray())
        return false;
    KisReferenceFilterTemplates::Template value;
    value.name = root.value("name").toString();
    value.enabled = root.value("enabled").toBool();
    for (const auto &item : root.value("effects").toArray()) {
        if (!item.isObject())
            return false;
        const auto effect = item.toObject();
        if (!effect.value("id").isString() || effect.value("id").toString().isEmpty()
            || !effect.value("configuration").isString() || !effect.value("enabled").isBool())
            return false;
        value.effects.append({effect.value("id").toString(),
                              effect.value("configuration").toString(),
                              effect.value("enabled").toBool()});
    }
    *result = value;
    return true;
}
} // namespace

KisReferenceFilterTemplates::KisReferenceFilterTemplates(const QString &directory)
    : m_directory(directory.isEmpty() ? QDir(KisSolsticePaths::configDir()).filePath("reference-image-filter-templates")
                                      : directory)
{
}

QString KisReferenceFilterTemplates::filePath(const QString &name) const
{
    // Names are labels, never paths (including Unicode, slashes and Windows device names).
    const auto digest = QCryptographicHash::hash(key(name).toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(m_directory).filePath(QString::fromLatin1(digest) + ".json");
}
QStringList KisReferenceFilterTemplates::names() const
{
    QStringList result;
    const QDir dir(m_directory);
    for (const auto &entry : dir.entryList({"*.json"}, QDir::Files)) {
        QFile file(dir.filePath(entry));
        Template value;
        if (file.open(QIODevice::ReadOnly) && decode(file.readAll(), &value) && filePath(value.name) == file.fileName())
            result.append(value.name);
    }
    result.sort(Qt::CaseInsensitive);
    return result;
}
bool KisReferenceFilterTemplates::contains(const QString &name) const
{
    return QFileInfo::exists(filePath(name));
}
bool KisReferenceFilterTemplates::load(const QString &name, Template *result, QString *error) const
{
    QFile file(filePath(name));
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, file.errorString());
    Template value;
    if (!decode(file.readAll(), &value) || key(value.name) != key(name))
        return fail(error, i18n("Invalid or unsupported filter template."));
    *result = value;
    return true;
}
bool KisReferenceFilterTemplates::save(const Template &value, bool overwrite, QString *error) const
{
    if (key(value.name).isEmpty())
        return fail(error, i18n("Enter a template name."));
    if (!QDir().mkpath(m_directory))
        return fail(error, i18n("Could not create the filter template folder."));
    QLockFile lock(filePath(value.name) + ".lock");
    if (!lock.tryLock(0))
        return fail(error, i18n("This filter template is being modified. Try again."));
    if (!overwrite && contains(value.name))
        return fail(error, i18n("A filter template with this name already exists."));
    QJsonArray effects;
    for (const auto &effect : value.effects) {
        if (effect.id.isEmpty())
            return fail(error, i18n("Invalid or unsupported filter template."));
        effects.append(
            QJsonObject{{"id", effect.id}, {"configuration", effect.configuration}, {"enabled", effect.enabled}});
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{"version", 1},
                                                       {"name", value.name.trimmed()},
                                                       {"enabled", value.enabled},
                                                       {"effects", effects}})
                                 .toJson();
    QSaveFile file(filePath(value.name));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, file.errorString());
    return true;
}
bool KisReferenceFilterTemplates::remove(const QString &name, QString *error) const
{
    QLockFile lock(filePath(name) + ".lock");
    if (!lock.tryLock(0))
        return fail(error, i18n("This filter template is being modified. Try again."));
    QFile file(filePath(name));
    return file.remove() || fail(error, file.errorString());
}
