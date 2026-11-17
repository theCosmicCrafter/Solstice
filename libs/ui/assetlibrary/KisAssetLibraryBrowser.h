/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_ASSET_LIBRARY_BROWSER_H
#define KIS_ASSET_LIBRARY_BROWSER_H

#include <QCache>
#include <QPixmap>
#include <QStringList>
#include <QVector>
#include <kritaui_export.h>

struct KisAssetLibrarySection {
    QString title;
    QStringList files;
};

namespace KisAssetLibrary
{
KRITAUI_EXPORT QStringList extensions(const QString &text);
KRITAUI_EXPORT QVector<KisAssetLibrarySection>
collectSections(const QString &path, bool recursive, const QString &extensions);
KRITAUI_EXPORT int thumbnailColumns(bool automatic, int fixedColumns, int thumbnailSize, int viewportWidth);
} // namespace KisAssetLibrary

// One bounded cache per view; QPixmap access remains on the GUI thread.
class KRITAUI_EXPORT KisAssetLibraryThumbnailCache
{
public:
    enum class Placeholder {
        Docker,
        Welcome
    };
    KisAssetLibraryThumbnailCache();
    QPixmap thumbnail(const QString &path,
                      int logicalSize,
                      qreal devicePixelRatio,
                      Placeholder placeholder = Placeholder::Docker);
    void clear();

private:
    QCache<QString, QPixmap> m_cache;
};
#endif
