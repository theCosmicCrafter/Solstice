/*
 * This file is part of Krita
 *
 * SPDX-FileCopyrightText: 2020 L. E. Segovia <amy@amyspark.me>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __KIS_IMAGE_INTERFACE_H
#define __KIS_IMAGE_INTERFACE_H

#include "kritaqmicinterface_export.h"

#include <QDebug>
#include <QMutex>
#include <QObject>
#include <QScopedPointer>
#include <QSize>
#include <QVector>
#include <limits>
#include <stdexcept>

class KisQmicApplicator;
class KisViewManager;

#define KRITA_QMIC_INTERFACE_VERSION 0x05010001

struct KRITAQMICINTERFACE_EXPORT KisQMicImage {
    QMutex m_mutex;
    QString m_layerName;
    int m_width;
    int m_height;
    int m_spectrum;
    float* m_data;

    KisQMicImage(QString layerName, int width, int height, int spectrum = 4)
        : m_mutex()
        , m_layerName(layerName)
        , m_width(width)
        , m_height(height)
        , m_spectrum(spectrum)
        , m_data(new float[checkedElementCount(width, height, spectrum)])
    {
    }

    ~KisQMicImage() {
        delete[] m_data;
    }

    const float* constData() const
    {
        return m_data;
    }

    size_t size() const
    {
        return checkedElementCount(m_width, m_height, m_spectrum) * sizeof(float);
    }

    // The native bridge and G'MIC copy lengths must also fit signed int.
    static size_t checkedElementCount(int width, int height, int spectrum)
    {
        const size_t limit = size_t(std::numeric_limits<int>::max()) / sizeof(float);
        if (width <= 0 || height <= 0 || spectrum < 1 || spectrum > 4
            || size_t(width) > limit / size_t(height) / size_t(spectrum)) {
            throw std::length_error("Unsupported G'MIC image dimensions or channel count");
        }
        return size_t(width) * size_t(height) * size_t(spectrum);
    }
};

QDebug operator<<(QDebug d, const KisQMicImage &model);

using KisQMicImageSP = QSharedPointer<KisQMicImage>;

class KRITAQMICINTERFACE_EXPORT KisImageInterface : public QObject
{
    Q_OBJECT

public:
    KisImageInterface(KisViewManager *parent = nullptr);
    ~KisImageInterface() override;

    QSize gmic_qt_get_image_size(int mode);
    QVector<KisQMicImageSP> gmic_qt_get_cropped_images(int mode, QRectF &cropRect);
    void gmic_qt_output_images(int mode, QVector<KisQMicImageSP> layers);
    void gmic_qt_detach();

Q_SIGNALS:
    void outputApplied();
    void errorOccurred(const QString &message);

private:
    struct Private;
    const QScopedPointer<Private> p;
};

#endif
