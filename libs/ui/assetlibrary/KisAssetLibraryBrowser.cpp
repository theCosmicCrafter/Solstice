/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisAssetLibraryBrowser.h"
#include <KoStore.h>
#include <QDateTime>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QMap>
#include <QPainter>
#include <algorithm>

QStringList KisAssetLibrary::extensions(const QString &text)
{
    QStringList result;
    for (QString extension : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        extension = extension.trimmed().toLower();
        if (extension.startsWith(QLatin1Char('.')))
            extension.remove(0, 1);
        if (!extension.isEmpty())
            result.append(extension);
    }
    if (result.isEmpty())
        result = QStringList{QStringLiteral("kra"), QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg")};
    return result;
}

QVector<KisAssetLibrarySection>
KisAssetLibrary::collectSections(const QString &path, bool recursive, const QString &extensionText)
{
    QVector<KisAssetLibrarySection> sections;
    QMap<QString, QStringList> byFolder;
    const QStringList suffixes = extensions(extensionText);
    const auto matches = [&suffixes](const QString &path) {
        return suffixes.contains(QFileInfo(path).suffix().toLower());
    };
    QDirIterator iterator(path,
                          QDir::Files | QDir::NoDotAndDotDot,
                          recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (matches(path))
            byFolder[QFileInfo(path).absolutePath()].append(path);
    }
    for (auto it = byFolder.begin(); it != byFolder.end(); ++it) {
        QStringList files = it.value();
        std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) {
            return QString::localeAwareCompare(QFileInfo(a).fileName(), QFileInfo(b).fileName()) < 0;
        });
        sections.append({QFileInfo(it.key()).fileName().isEmpty() ? it.key() : QFileInfo(it.key()).fileName(), files});
    }
    return sections;
}

int KisAssetLibrary::thumbnailColumns(bool automatic, int fixedColumns, int thumbnailSize, int viewportWidth)
{
    if (!automatic)
        return std::max(1, fixedColumns);
    const int tileWidth = thumbnailSize + 24;
    return std::max(1, (std::max(1, viewportWidth - 8) + 10) / (tileWidth + 10));
}

KisAssetLibraryThumbnailCache::KisAssetLibraryThumbnailCache()
    : m_cache(64 * 1024)
{
}

void KisAssetLibraryThumbnailCache::clear()
{
    m_cache.clear();
}

QPixmap KisAssetLibraryThumbnailCache::thumbnail(const QString &path,
                                                 int logicalSize,
                                                 qreal pixelRatio,
                                                 Placeholder placeholder)
{
    const QFileInfo info(path);
    const int pixelSize = std::max(1, qRound(logicalSize * pixelRatio));
    const QString cacheKey = QStringLiteral("%1|%2|%3|%4|%5|%6")
                                 .arg(info.absoluteFilePath())
                                 .arg(info.size())
                                 .arg(info.lastModified().toMSecsSinceEpoch())
                                 .arg(logicalSize)
                                 .arg(pixelRatio)
                                 .arg(static_cast<int>(placeholder));
    if (const QPixmap *cached = m_cache.object(cacheKey))
        return *cached;

    QImage source;
    if (info.suffix().compare(QStringLiteral("kra"), Qt::CaseInsensitive) == 0) {
        QScopedPointer<KoStore> store(KoStore::createStore(path, KoStore::Read));
        if (store) {
            const QStringList candidates{QStringLiteral("preview.png"),
                                         QStringLiteral("Thumbnails/thumbnail.png"),
                                         QStringLiteral("mergedimage.png")};
            for (const QString &candidate : candidates) {
                if (!store->hasFile(candidate) || !store->open(candidate))
                    continue;
                const QByteArray bytes = store->read(store->size());
                store->close();
                if (source.loadFromData(bytes))
                    break;
            }
        }
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize originalSize = reader.size();
        if (originalSize.isValid())
            reader.setScaledSize(originalSize.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio));
        source = reader.read();
    }

    QPixmap thumbnail;
    if (!source.isNull()) {
        thumbnail =
            QPixmap::fromImage(source.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        thumbnail.setDevicePixelRatio(pixelRatio);
    } else {
        thumbnail = QPixmap(pixelSize, pixelSize);
        thumbnail.setDevicePixelRatio(pixelRatio);
        thumbnail.fill(QColor(245, 245, 245));
        QPainter painter(&thumbnail);
        painter.setPen(QColor(75, 75, 75));
        painter.drawRect(thumbnail.rect().adjusted(0, 0, -1, -1));
        if (placeholder == Placeholder::Docker) {
            QFont font;
            font.setPointSize(std::max(10, logicalSize / 7));
            font.setBold(true);
            painter.setFont(font);
        }
        painter.drawText(thumbnail.rect(), Qt::AlignCenter, info.suffix().toUpper());
    }

    const int costKiB = std::max(1, thumbnail.width() * thumbnail.height() * 4 / 1024);
    m_cache.insert(cacheKey, new QPixmap(thumbnail), costKiB);
    return thumbnail;
}
