/*
 *  SPDX-FileCopyrightText: 2010 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *  SPDX-FileCopyrightText: 2007 Eric Lamarque <eric.lamarque@free.fr>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <QtEndian>

#include "kis_abr_brush_collection.h"
#include "kis_abr_brush.h"

#include <QDomElement>
#include <QFile>
#include <QImage>
#include <QPoint>
#include <QColor>
#include <QByteArray>
#include <kis_debug.h>
#include <QString>
#include <QBuffer>
#include <QFileInfo>
#include <KoMD5Generator.h>
#include <klocalizedstring.h>

#include <KoColor.h>
#include <QDomDocument>
#include <QHash>
#include <QTextStream>
#include "KisAbrParser.h"
#include <asl/kis_asl_reader.h>
#include "KisAbrPresetConverter.h"

namespace
{
/// the name of a tip that has none of its own: test.abr -> test_12
QString abr_v1_brush_name(const QString filename, qint32 id)
{
    QString result = filename;
    int pos = filename.lastIndexOf('.');
    result.remove(pos, 4);
    QTextStream(&result) << "_" << id;
    return result;
}

/**
 * Solstice: the names of the presets that use each sampled tip, by the
 * tip's identifier (`sampledData`), from the preset descriptors of version
 * 6 and later (docs/agent/abr-import.md)
 */
QDomDocument readDescriptors(const QByteArray &descriptors)
{
    if (descriptors.isEmpty()) {
        return QDomDocument();
    }
    QByteArray data = descriptors;
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);
    return KisAslReader::readFillLayer(buffer);
}

QHash<QString, QString> presetNamesBySample(const QDomDocument &doc)
{
    QHash<QString, QString> names;

    const QDomNodeList nodes = doc.elementsByTagName(QStringLiteral("node"));
    for (int i = 0; i < nodes.size(); i++) {
        const QDomElement preset = nodes.at(i).toElement();
        if (preset.attribute(QStringLiteral("type")) != QStringLiteral("Descriptor")) {
            continue;
        }
        QString name;
        QString sample;
        for (QDomElement child = preset.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
            const QString key = child.attribute(QStringLiteral("key"));
            if (key == QStringLiteral("Nm  ")) {
                name = child.attribute(QStringLiteral("value")).trimmed();
            } else if (key == QStringLiteral("Brsh")) {
                for (QDomElement tip = child.firstChildElement(); !tip.isNull(); tip = tip.nextSiblingElement()) {
                    if (tip.attribute(QStringLiteral("key")) == QStringLiteral("sampledData")) {
                        sample = tip.attribute(QStringLiteral("value"));
                    }
                }
            }
        }
        if (!name.isEmpty() && !sample.isEmpty() && !names.contains(sample)) {
            names.insert(sample, name);
        }
    }
    return names;
}

/**
 * Solstice: the patterns of the `patt` section, read one by one with the
 * layer style reader so that a pattern it cannot read (an unsupported
 * color mode) does not hide the ones after it
 */
QMap<QString, KoPatternSP> readPatterns(const QByteArray &section, const QString &fileName)
{
    QMap<QString, KoPatternSP> patterns;
    qint64 pos = 0;
    int unreadable = 0;
    while (pos + 4 <= section.size()) {
        const quint32 length = qFromBigEndian<quint32>(section.constData() + pos);
        const qint64 blockSize = 4 + ((qint64(length) + 3) & ~qint64(3));
        if (length == 0 || pos + 4 + length > section.size()) {
            break;
        }
        QByteArray block = section.mid(int(pos), int(qMin<qint64>(blockSize, section.size() - pos)));
        pos += blockSize;

        QBuffer buffer(&block);
        buffer.open(QIODevice::ReadOnly);
        const QDomDocument doc = KisAslReader::readPsdSectionPattern(buffer, block.size());

        bool read = false;
        const QDomNodeList nodes = doc.elementsByTagName(QStringLiteral("node"));
        for (int i = 0; i < nodes.size(); i++) {
            const QDomElement node = nodes.at(i).toElement();
            if (node.attribute(QStringLiteral("classId")) != QStringLiteral("KisPattern")) {
                continue;
            }
            QString name;
            QString identifier;
            QByteArray data;
            for (QDomElement child = node.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
                const QString key = child.attribute(QStringLiteral("key"));
                if (key == QStringLiteral("Nm  ")) {
                    name = child.attribute(QStringLiteral("value"));
                } else if (key == QStringLiteral("Idnt")) {
                    identifier = child.attribute(QStringLiteral("value"));
                } else if (key == QStringLiteral("Data")) {
                    data = qUncompress(QByteArray::fromBase64(child.text().toLatin1()));
                }
            }
            if (identifier.isEmpty() || data.isEmpty()) {
                continue;
            }
            const QString patternFileName = identifier + QStringLiteral(".pat");
            KoPatternSP pattern(new KoPattern(patternFileName));
            QBuffer patternBuffer(&data);
            patternBuffer.open(QIODevice::ReadOnly);
            if (!pattern->loadFromDevice(&patternBuffer, nullptr)) {
                continue;
            }
            pattern->setName(name.isEmpty() ? identifier : name);
            pattern->setMD5Sum(KoMD5Generator::generateHash(data));
            pattern->setValid(true);
            patterns.insert(patternFileName, pattern);
            read = true;
        }
        unreadable += !read;
    }
    if (unreadable) {
        warnKrita << "ABR" << fileName << ":" << unreadable << "patterns could not be read";
    }
    return patterns;
}
} // namespace

KisAbrBrushCollection::KisAbrBrushCollection(const QString &filename)
    : m_isLoaded(false)
    , m_lastModified()
    , m_filename(filename)
    , m_abrBrushes(new QMap<QString, KisAbrBrushSP>())
    , m_patterns(new QMap<QString, KoPatternSP>())
    , m_presets(new QMap<QString, KisPaintOpPresetSP>())
{
}

KisAbrBrushCollection::KisAbrBrushCollection(const KisAbrBrushCollection& rhs)
    : m_isLoaded(rhs.m_isLoaded)
    , m_lastModified(rhs.m_lastModified)
{
    m_abrBrushes.reset(new QMap<QString, KisAbrBrushSP>());
    m_patterns.reset(new QMap<QString, KoPatternSP>(*rhs.m_patterns));
    m_presets.reset(new QMap<QString, KisPaintOpPresetSP>(*rhs.m_presets));
    m_presetFolders = rhs.m_presetFolders;
    for (auto it = rhs.m_abrBrushes->begin();
         it != rhs.m_abrBrushes->end();
         ++it) {

        m_abrBrushes->insert(it.key(), KisAbrBrushSP(new KisAbrBrush(*it.value(), this)));
    }
}

bool KisAbrBrushCollection::load()
{
    m_isLoaded = true;
    QFile file(filename());
    QFileInfo info(file);
    m_lastModified = info.lastModified();
    // check if the file is open correctly
    if (!file.open(QIODevice::ReadOnly)) {
        warnKrita << "Can't open file " << filename();
        return false;
    }

    bool res = loadFromDevice(&file);
    file.close();

    return res;

}

bool KisAbrBrushCollection::loadFromDevice(QIODevice *dev)
{
    // Solstice: read with KisAbrParser (docs/agent/abr-import.md)
    KisAbrParser::Contents contents;
    const bool parsed = KisAbrParser::parse(dev->readAll(), &contents);
    for (const QString &warning : contents.warnings) {
        warnKrita << "ABR" << filename() << ":" << warning;
    }
    if (!parsed) {
        warnKrita << "ERROR: unable to read the ABR file" << filename() << "version" << contents.version << "subversion"
                  << contents.subversion;
        return false;
    }
    *m_patterns = readPatterns(contents.patterns, filename());
    if (contents.samples.isEmpty()) {
        warnKrita << "ERROR: no sample brush found in" << filename();
        return !m_patterns->isEmpty();
    }

    const QString fileName = QFileInfo(filename()).fileName();
    const QDomDocument descriptors = readDescriptors(contents.descriptors);
    const QHash<QString, QString> presetNames = presetNamesBySample(descriptors);
    KisAbrPresetConverter::Sources sources;
    sources.baseName = QFileInfo(filename()).completeBaseName();

    for (const KisAbrParser::Sample &sample : contents.samples) {
        // the resource's file name identifies it, as it always has; the
        // name shown is the tip's own or its preset's when there is one
        const QString key = !sample.name.isEmpty() ? sample.name : abr_v1_brush_name(fileName, sample.index);
        const QString shownName = !sample.name.isEmpty() ? sample.name : presetNames.value(sample.id, key);

        KisAbrBrushSP abrBrush;
        if (m_abrBrushes->contains(key)) {
            abrBrush = m_abrBrushes.data()->operator[](key);
        } else {
            abrBrush = KisAbrBrushSP(new KisAbrBrush(key, this));
            QBuffer buf;
            buf.open(QFile::ReadWrite);
            sample.image.save(&buf, "PNG");
            abrBrush->setMD5Sum(KoMD5Generator::generateHash(buf.data()));
        }

        abrBrush->setBrushTipImage(sample.image);
        abrBrush->setValid(true);
        abrBrush->setName(shownName);
        m_abrBrushes.data()->operator[](key) = abrBrush;
        if (!sample.id.isEmpty() || contents.samples.size() == 1) {
            sources.tipsBySample.insert(sample.id, abrBrush);
        }
    }

    // Solstice: the file's brush presets as Pixel Brush presets
    // (docs/agent/abr-import.md)
    for (auto it = m_patterns->constBegin(); it != m_patterns->constEnd(); ++it) {
        sources.patternsById.insert(QFileInfo(it.key()).completeBaseName(), it.value());
    }
    if (!descriptors.isNull()) {
        const KisAbrPresetConverter::Result converted =
            KisAbrPresetConverter::convert(descriptors.documentElement(), sources);
        *m_presets = converted.presets;
        if (converted.skipped) {
            warnKrita << "ABR" << filename() << ":" << converted.skipped
                      << "presets whose tips are missing are skipped";
        }
        for (auto it = converted.unsupported.constBegin(); it != converted.unsupported.constEnd(); ++it) {
            warnKrita << "ABR" << filename() << ":" << it.value() << "presets use" << it.key()
                      << ", which Solstice does not have";
        }

        // the folders of the Brushes panel: the hierarchy lists
        // the presets in the order of the descriptors; presets it does not
        // list stay at the top level
        m_presetFolders.clear();
        const QDomDocument hierarchy = readDescriptors(contents.hierarchy);
        if (!hierarchy.isNull()) {
            const QVector<QStringList> folders = KisAbrPresetConverter::presetFolders(hierarchy.documentElement());
            for (int i = 0; i < folders.size(); i++) {
                const QString fileName = QStringLiteral("%1_preset_%2.kpp").arg(sources.baseName).arg(i + 1);
                if (!folders[i].isEmpty() && m_presets->contains(fileName)) {
                    m_presetFolders.insert(fileName, folders[i]);
                }
            }
        }
    }

    return true;
}

bool KisAbrBrushCollection::save()
{
    return false;
}

bool KisAbrBrushCollection::saveToDevice(QIODevice */*dev*/) const
{
    return false;
}

bool KisAbrBrushCollection::isLoaded() const
{
    return m_isLoaded;
}

QImage KisAbrBrushCollection::image() const
{
    if (m_abrBrushes->size() > 0) {
        return m_abrBrushes->values().first()->image();
    }
    return QImage();
}

void KisAbrBrushCollection::toXML(QDomDocument& d, QDomElement& e) const
{
    Q_UNUSED(d);
    Q_UNUSED(e);
    // Do nothing...
}

QString KisAbrBrushCollection::defaultFileExtension() const
{
    return QString(".abr");
}
