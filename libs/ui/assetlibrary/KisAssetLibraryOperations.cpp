/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisAssetLibraryOperations.h"
#include <KisViewManager.h>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <kis_file_layer.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_import_catcher.h>
#include <kis_node_commands_adapter.h>
#include <kis_node_manager.h>
#include <klocalizedstring.h>

void KisAssetLibrary::insertAsLayer(const QString &path, KisViewManager *view, QWidget *parentWidget)
{
    if (!view || !view->image()) {
        QMessageBox::warning(parentWidget, i18n("Insert as New Layer"), i18n("No active document."));
        return;
    }
    new KisImportCatcher(path,
                         view,
                         QFileInfo(path).suffix().compare(QStringLiteral("svg"), Qt::CaseInsensitive) == 0
                             ? QStringLiteral("KisShapeLayer")
                             : QStringLiteral("KisPaintLayer"));
}

void KisAssetLibrary::insertAsFileLayer(const QString &path, KisViewManager *view, QWidget *parentWidget)
{
    if (!view || !view->image()) {
        QMessageBox::warning(parentWidget, i18n("Insert as New File Layer"), i18n("No active document."));
        return;
    }
    const QFileInfo info(path);
    KisLayerSP layer = new KisFileLayer(view->image(),
                                        info.absolutePath(),
                                        info.fileName(),
                                        KisFileLayer::None,
                                        QStringLiteral("Bicubic"),
                                        info.completeBaseName(),
                                        255);
    KisNodeSP active = view->activeNode();
    KisNodeSP parent;
    if (active && active->parent())
        parent = active->parent();
    else
        parent = view->image()->rootLayer();
    KisNodeCommandsAdapter adapter(view);
    adapter.addNode(layer, parent, active);
    view->nodeManager()->slotNonUiActivatedNode(layer);
}

QString
KisAssetLibrary::requestAssetPath(const QString &path, const QString &title, QWidget *parentWidget, bool allowCurrent)
{
    bool ok = false;
    QString name = QInputDialog::getText(parentWidget,
                                         title,
                                         i18n("New file name"),
                                         QLineEdit::Normal,
                                         QFileInfo(path).fileName(),
                                         &ok)
                       .trimmed();
    if (!ok || name.isEmpty())
        return {};
    if (QFileInfo(name).fileName() != name) {
        QMessageBox::warning(parentWidget, title, i18n("File name cannot contain a path."));
        return {};
    }
    if (QFileInfo(name).suffix().isEmpty())
        name += QLatin1Char('.') + QFileInfo(path).suffix();
    const QString target = QDir(QFileInfo(path).absolutePath()).filePath(name);
    if (allowCurrent && QFileInfo(target) == QFileInfo(path))
        return target;
    if (QFileInfo::exists(target)) {
        QMessageBox::warning(parentWidget, title, i18n("A file with that name already exists."));
        return {};
    }
    return target;
}
