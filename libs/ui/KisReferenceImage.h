/*
 * SPDX-FileCopyrightText: 2017 Boudewijn Rempt <boud@valdyas.org>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef KISREFERENCEIMAGE_H
#define KISREFERENCEIMAGE_H

#include <QSharedDataPointer>

#include <KoColor.h>
#include <KoShape.h>
#include <kis_types.h>
#include <kritaui_export.h>
#include <kundo2command.h>

class QImage;
class QPointF;
class QPainter;
class QRectF;
class KoStore;
class KisCoordinatesConverter;
template<typename T>
class QFutureWatcher;
class KisCanvas2;

/**
 * @brief The KisReferenceImage class represents a single reference image
 */
class KRITAUI_EXPORT KisReferenceImage : public KoShape
{
public:
    struct KRITAUI_EXPORT SetSaturationCommand : public KUndo2Command {
        QVector<KisReferenceImage*> images;
        QVector<qreal> oldSaturations;
        qreal newSaturation;

        explicit SetSaturationCommand(const QList<KoShape *> &images, qreal newSaturation, KUndo2Command *parent = 0);
        void undo() override;
        void redo() override;
    };

    KisReferenceImage();
    KisReferenceImage(const KisReferenceImage &rhs);
    ~KisReferenceImage();

    KoShape *cloneShape() const override;

    /**
     * Load a reference image from specified file.
     * If parent is provided and the image cannot be loaded, a warning message will be displayed to user.
     * @return reference image or null if one could not be loaded
     */
    static KisReferenceImage * fromFile(const QString &filename, const KisCoordinatesConverter &converter, QWidget *parent /*= nullptr*/);
    static KisReferenceImage * fromClipboard(const KisCoordinatesConverter &converter);
    static KisReferenceImage * fromQImage(const KisCoordinatesConverter &converter, const QImage &img);

    /**
     * Load a reference image from specified paint device.
     * @return reference image or null if one could not be loaded
     */
    static KisReferenceImage *
    fromPaintDevice(KisPaintDeviceSP src, const KisCoordinatesConverter &converter, QWidget *parent);

    void setSaturation(qreal saturation);
    qreal saturation() const;

    void setEmbed(bool embed);
    bool embed();
    bool hasLocalFile();

    void setFilename(const QString &filename);
    QString filename() const;
    QString internalFile() const;

    void paint(QPainter &gc) const override;

    QColor getPixel(QPointF position);

    void saveXml(QDomDocument &document, QDomElement &parentElement, int id);
    bool saveImage(KoStore *store) const;

    static KisReferenceImage * fromXml(const QDomElement &elem);
    bool loadImage(KoStore *store);

    QImage getImage();

    // Ordered native filter configurations; the original pixels stay untouched.
    struct FilterEffect {
        QString id;
        QString configuration;
        bool enabled = true;
    };
    QVector<FilterEffect> filterEffects() const;
    bool filtersEnabled() const;
    void setFilterEffects(const QVector<FilterEffect> &effects, bool enabled);
    void previewFilterEffects(const QVector<FilterEffect> &effects, bool enabled);
    static QImage renderFilterEffects(const QImage &source, const QVector<FilterEffect> &effects);

private:
    void ensureFilterCache();
    QFutureWatcher<QImage> *m_filterWatcher = nullptr;
    quint64 m_filterRevision = 0;
    bool m_previewActive = false;
    bool m_previewEnabled = true;
    QVector<FilterEffect> m_previewEffects;
    struct Private;
    QSharedDataPointer<Private> d;
};

#endif // KISREFERENCEIMAGE_H
