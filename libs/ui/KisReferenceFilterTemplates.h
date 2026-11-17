/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "KisReferenceImage.h"
#include <QStringList>

/** Profile-wide snapshots, independent of any image or document. */
class KRITAUI_EXPORT KisReferenceFilterTemplates
{
public:
    struct Template {
        QString name;
        QVector<KisReferenceImage::FilterEffect> effects;
        bool enabled = true;
    };
    explicit KisReferenceFilterTemplates(const QString &directory = QString());
    QStringList names() const;
    bool contains(const QString &name) const;
    bool load(const QString &name, Template *result, QString *error) const;
    bool save(const Template &value, bool overwrite, QString *error) const;
    bool remove(const QString &name, QString *error) const;

private:
    QString filePath(const QString &name) const;
    QString m_directory;
};
