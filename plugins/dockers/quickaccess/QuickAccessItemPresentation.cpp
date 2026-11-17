/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessItemPresentation.h"
#include <QDir>
#include <QFileInfo>
namespace QuickAccessItemPresentation
{
bool showsStrokePreview(const QuickAccess::Item &item)
{
    return item.type == QuickAccess::ItemType::Brush
        && item.payload.value(BrushDisplayKey).toString() == BrushDisplayStroke;
}

QJsonObject aliasFor(const QuickAccess::Document &document, const QString &category, const QString &id)
{
    return document.aliases.value(category).toObject().value(id).toObject();
}

QString compatibleString(const QJsonObject &object,
                         const QString &nativeName,
                         const QString &legacyName,
                         const QString &fallback)
{
    QString value = object.value(nativeName).toString();
    if (value.isEmpty())
        value = object.value(legacyName).toString();
    return value.isEmpty() ? fallback : value;
}

int compatibleInt(const QJsonObject &object, const QString &nativeName, const QString &legacyName, int fallback)
{
    bool ok = false;
    int value = object.value(nativeName).toVariant().toInt(&ok);
    if (!ok)
        value = object.value(legacyName).toVariant().toInt(&ok);
    return ok ? value : fallback;
}

QString defaultIconsDirectory()
{
    return QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
        .filePath(QStringLiteral("resources/default_icons"));
}

QString bundledIconPath(const QString &category, const QString &iconName)
{
    const QString fileName = QFileInfo(iconName).fileName();
    if (fileName.isEmpty())
        return QString();
    const QString path = QStringLiteral(":/quickaccess/%1/%2").arg(category, fileName);
    return QFileInfo::exists(path) ? path : QString();
}

QString resolveDefaultIcon(const QString &iconName)
{
    if (QFileInfo(iconName).isAbsolute() && QFileInfo::exists(iconName))
        return iconName;
    const QString bundled = bundledIconPath(QStringLiteral("default_icons"), iconName);
    if (!bundled.isEmpty())
        return bundled;
    return QString();
}

bool showsAliasIcon(const QuickAccess::Document &document, const QuickAccess::Item &item)
{
    QJsonObject alias;
    if (item.type == QuickAccess::ItemType::Action) {
        alias =
            aliasFor(document, QStringLiteral("actions"), item.payload.value(QStringLiteral("action_id")).toString());
    } else if (item.type == QuickAccess::ItemType::DockerToggle) {
        alias =
            aliasFor(document, QStringLiteral("dockers"), item.payload.value(QStringLiteral("docker_id")).toString());
    } else {
        return false;
    }
    return !resolveDefaultIcon(alias.value(QStringLiteral("icon_name")).toString()).isEmpty();
}

bool normalizeIconItemSpans(QuickAccess::Document *document)
{
    // Aliases are shared by every tab, so one icon change can affect several
    // items. Shrinking in place cannot create overlaps.
    bool changed = false;
    for (QuickAccess::Tab &tab : document->tabs) {
        for (QuickAccess::Grid &grid : tab.grids) {
            for (QuickAccess::Item &item : grid.items) {
                if ((item.rowSpan != 1 || item.columnSpan != 1) && showsAliasIcon(*document, item)) {
                    item.rowSpan = 1;
                    item.columnSpan = 1;
                    changed = true;
                }
            }
        }
    }
    return changed;
}
} // namespace QuickAccessItemPresentation
