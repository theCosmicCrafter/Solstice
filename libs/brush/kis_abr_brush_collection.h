/*
 *  SPDX-FileCopyrightText: 2010 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *  SPDX-FileCopyrightText: 2007 Eric Lamarque <eric.lamarque@free.fr>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_ABR_BRUSH_COLLECTION_H
#define KIS_ABR_BRUSH_COLLECTION_H

#include <QImage>
#include <QDataStream>
#include <QMap>
#include <QString>
#include <QStringList>
#include <kis_debug.h>

#include <kis_scaling_size_brush.h>
#include <kis_types.h>
#include <kis_shared.h>
#include <brushengine/kis_paint_information.h>
#include <kis_abr_brush.h>
#include <resources/KoPattern.h>


class QString;
class QIODevice;


/**
 * load a collection of brushes from an abr file
 */
class BRUSH_EXPORT KisAbrBrushCollection
{

protected:

public:

    /// Construct brush to load filename later as brush
    KisAbrBrushCollection(const QString& filename);

    ~KisAbrBrushCollection() {}

    bool load();

    bool loadFromDevice(QIODevice *dev);

    bool save();

    bool saveToDevice(QIODevice* dev) const;

    bool isLoaded() const;

    /**
     * @return a preview of the brush
     */
    QImage image() const;

    /**
     * @return default file extension for saving the brush
     */
    QString defaultFileExtension() const;

    QList<KisAbrBrushSP> brushes() const {
        return m_abrBrushes->values();
    }

    QSharedPointer<QMap<QString, KisAbrBrushSP>> brushesMap() const {
        return m_abrBrushes;
    }

    KisAbrBrushSP brushByName(QString name) const {
        if (m_abrBrushes->contains(name)) {
            return m_abrBrushes.data()->operator[](name);
        }
        return KisAbrBrushSP();
    }

    QDateTime lastModified() const {
        return m_lastModified;
    }

    /// Solstice: the patterns stored in the file, by file name
    /// (`<identifier>.pat`); docs/agent/abr-import.md
    QSharedPointer<QMap<QString, KoPatternSP>> patternsMap() const
    {
        return m_patterns;
    }

    KoPatternSP patternByName(const QString &name) const
    {
        return m_patterns->value(name);
    }

    /// Solstice: the file's brush presets as Pixel Brush presets, by file
    /// name (`<file>_preset_<n>.kpp`)
    QSharedPointer<QMap<QString, KisPaintOpPresetSP>> presetsMap() const
    {
        return m_presets;
    }

    KisPaintOpPresetSP presetByName(const QString &name) const
    {
        return m_presets->value(name);
    }

    /// Solstice: the folders of the file's presets in Photoshop's Brushes
    /// panel, by preset file name; presets at the top level are left out
    QMap<QString, QStringList> presetFolders() const
    {
        return m_presetFolders;
    }

    QString filename() const {
        return m_filename;
    }

protected:
    KisAbrBrushCollection(const KisAbrBrushCollection& rhs);

    void toXML(QDomDocument& d, QDomElement& e) const;

private:

    bool m_isLoaded;
    QDateTime m_lastModified;
    QString m_filename;
    QSharedPointer<QMap<QString, KisAbrBrushSP>> m_abrBrushes;
    QSharedPointer<QMap<QString, KoPatternSP>> m_patterns;
    QSharedPointer<QMap<QString, KisPaintOpPresetSP>> m_presets;
    QMap<QString, QStringList> m_presetFolders;
};

typedef QSharedPointer<KisAbrBrushCollection> KisAbrBrushCollectionSP;

#endif

