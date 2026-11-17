/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_BRUSH_STROKE_PREVIEW_CACHE_H
#define KIS_BRUSH_STROKE_PREVIEW_CACHE_H

#include <QImage>
#include <QModelIndex>
#include <QObject>
#include <QScopedPointer>
#include <kis_types.h>
#include <kritaui_export.h>

class KisResourceStorage;
class KisDocument;
/** GUI-thread facade. Resource loading uses independent saved copies; disk I/O
 * and PNG decoding use a worker. Requests belong to visible chooser instances. */
class KRITAUI_EXPORT KisBrushStrokePreviewCache : public QObject
{
    Q_OBJECT
public:
    struct KRITAUI_EXPORT Request {
        QString location, filename, md5;
        QString id() const;
        static Request fromIndex(const QModelIndex &index);
    };
    static KisBrushStrokePreviewCache *instance();
    explicit KisBrushStrokePreviewCache(const QString &directory, QObject *parent = nullptr);
    ~KisBrushStrokePreviewCache() override;
    void setRequests(QObject *consumer, const QList<Request> &requests);
    void removeConsumer(QObject *consumer);
    QImage preview(const Request &request) const;
    bool ready(const Request &request) const;
    bool isBusy() const;
    void invalidate();
    static QString cacheKey(KisPaintOpPresetSP preset, const QString &savedMd5);
    static void prune(const QString &directory);
Q_SIGNALS:
    void previewReady();
    void renderStarted();

private:
    void process();
    void observeDocument(KisDocument *document);
    void observeImage(KisImageSP image);
    void finish(const QImage &image, bool cancelled);
    bool wanted(const QString &id) const;
    /// A change of one preset's row in the resource model (not of its links)
    void presetChanged(const Request &request);
    struct Private;
    QScopedPointer<Private> m_d;
};
#endif
