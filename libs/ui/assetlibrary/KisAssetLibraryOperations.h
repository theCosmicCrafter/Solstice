/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_ASSET_LIBRARY_OPERATIONS_H
#define KIS_ASSET_LIBRARY_OPERATIONS_H
#include <QString>
#include <kritaui_export.h>
class KisViewManager;
class QWidget;
namespace KisAssetLibrary
{
KRITAUI_EXPORT void insertAsLayer(const QString &path, KisViewManager *view, QWidget *parentWidget);
KRITAUI_EXPORT void insertAsFileLayer(const QString &path, KisViewManager *view, QWidget *parentWidget);
KRITAUI_EXPORT QString requestAssetPath(const QString &path,
                                        const QString &title,
                                        QWidget *parentWidget,
                                        bool allowCurrent = false);
} // namespace KisAssetLibrary
#endif
