/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessGesturePresentation.h"

#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisResourceModel.h>
#include <KisResourceServerProvider.h>
#include <KisViewManager.h>
#include <KoResource.h>
#include <kis_action.h>
#include <kis_action_manager.h>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFileInfo>
#include <QPixmap>

namespace QuickAccessGesturePresentation
{
const QStringList Directions{QStringLiteral("left_up"),
                             QStringLiteral("up"),
                             QStringLiteral("right_up"),
                             QStringLiteral("left"),
                             QStringLiteral("center"),
                             QStringLiteral("right"),
                             QStringLiteral("left_down"),
                             QStringLiteral("down"),
                             QStringLiteral("right_down")};

QString configuredValue(const QJsonObject &config)
{
    const QJsonObject parameters = config.value(QStringLiteral("parameters")).toObject();
    const QString type = config.value(QStringLiteral("gesture_type")).toString();
    if (type == QStringLiteral("action"))
        return parameters.value(QStringLiteral("action_id")).toString();
    if (type == QStringLiteral("brush"))
        return parameters.value(QStringLiteral("brush_name")).toString();
    if (type == QStringLiteral("docker_toggle"))
        return parameters.value(QStringLiteral("docker_name")).toString();
    return QString();
}

QJsonObject aliasFor(const QuickAccess::Document &document, const QString &category, const QString &id)
{
    return document.aliases.value(category).toObject().value(id).toObject();
}

QString resolveIcon(const QString &iconName)
{
    if (QFileInfo(iconName).isAbsolute() && QFileInfo::exists(iconName))
        return iconName;
    const QString fileName = QFileInfo(iconName).fileName();
    if (!fileName.isEmpty()) {
        const QString bundled = QStringLiteral(":/quickaccess/default_icons/%1").arg(fileName);
        if (QFileInfo::exists(bundled))
            return bundled;
    }
    return QString();
}

QIcon gestureIcon(const QuickAccess::Document &document, const QJsonObject &config)
{
    const QJsonObject parameters = config.value(QStringLiteral("parameters")).toObject();
    const QString type = config.value(QStringLiteral("gesture_type")).toString();
    if (type == QStringLiteral("brush")) {
        const QString name = parameters.value(QStringLiteral("brush_name")).toString();
        const auto resources =
            KisResourceServerProvider::instance()->paintOpPresetServer()->resourceModel()->resourcesForName(name);
        if (!resources.isEmpty())
            return QPixmap::fromImage(resources.constFirst()->image());
    }

    QString category;
    QString id;
    if (type == QStringLiteral("action")) {
        category = QStringLiteral("actions");
        id = parameters.value(QStringLiteral("action_id")).toString();
    } else if (type == QStringLiteral("docker_toggle")) {
        category = QStringLiteral("dockers");
        id = parameters.value(QStringLiteral("docker_name")).toString();
    }
    if (id.isEmpty())
        return {};

    const QString iconPath =
        resolveIcon(aliasFor(document, category, id).value(QStringLiteral("icon_name")).toString());
    if (!iconPath.isEmpty())
        return QIcon(iconPath);

    if (type == QStringLiteral("action")) {
        if (KisMainWindow *window = KisPart::instance()->currentMainwindow()) {
            if (KisViewManager *viewManager = window->viewManager()) {
                if (KisAction *action = viewManager->actionManager()->actionByName(id))
                    return action->icon();
            }
        }
    } else {
        for (QWidget *widget : QApplication::allWidgets()) {
            if (auto *docker = qobject_cast<QDockWidget *>(widget); docker && docker->objectName() == id)
                return docker->windowIcon();
        }
    }
    return {};
}
} // namespace QuickAccessGesturePresentation
