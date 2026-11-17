/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisBrushStrokeLayer.h"

#include <QBuffer>
#include <QDataStream>
#include <QDomDocument>
#include <QHash>
#include <cmath>

#include <KisGlobalResourcesInterface.h>
#include <KisLocalStrokeResources.h>
#include <KisMimeDatabase.h>
#include <KisResourceLoaderRegistry.h>
#include <KisResourceTypes.h>
#include <KoCanvasResourceProvider.h>
#include <KoColorProfile.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoLocalStrokeCanvasResources.h>
#include <kis_abr_brush.h>
#include <kis_debug.h>
#include <kis_paintop_preset.h>
#include <kis_paintop_settings.h>
#include <resources/KoSegmentGradient.h>
#include <resources/KoStopGradient.h>
#include <strokes/freehand_stroke.h>

/**
 * The brush stroke file of a layer (docs/agent/brush-stroke-layer.md):
 * a QDataStream of a header, the brush presets as XML, the resources as
 * their files (the presets' brush tips and textures, the strokes' patterns
 * and gradients), and the strokes, which refer to the presets, patterns and
 * gradients by index.
 */
namespace
{
const quint32 magic = 0x5342534c; // "SBSL"
// 2: the start of each copy's dab spacing (KisRecordedBrushStroke::starts)
// 3: the canvas mirror (KisRecordedBrushStroke::mirrorHorizontal and others)
const quint32 version = 3;

/// more copies of a stroke than any tool paints: damaged data
const qint32 maximumStrokeInfoCount = 4096;

/// A position a stroke can be drawn at: finite and within reach of any image
/// (a damaged file would otherwise make the redraw cover a huge area)
bool validPosition(const QPointF &point)
{
    const qreal limit = 1e7;
    return std::isfinite(point.x()) && std::isfinite(point.y()) && qAbs(point.x()) < limit && qAbs(point.y()) < limit;
}

void prepare(QDataStream &stream)
{
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}

void writeColor(QDataStream &stream, const KoColor &color)
{
    const KoColorSpace *cs = color.colorSpace();
    stream << cs->colorModelId().id() << cs->colorDepthId().id() << (cs->profile() ? cs->profile()->name() : QString())
           << QByteArray(reinterpret_cast<const char *>(color.data()), int(cs->pixelSize()));
}

KoColor readColor(QDataStream &stream)
{
    QString model;
    QString depth;
    QString profile;
    QByteArray data;
    stream >> model >> depth >> profile >> data;

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace(model, depth, profile);
    if (!cs || data.size() != int(cs->pixelSize())) {
        stream.setStatus(QDataStream::ReadCorruptData);
        return KoColor();
    }
    return KoColor(reinterpret_cast<const quint8 *>(data.constData()), cs);
}

void writePaintInformation(QDataStream &stream, const KisPaintInformation &pi)
{
    stream << pi.pos() << pi.pressure() << pi.xTilt() << pi.yTilt() << pi.rotation() << pi.tangentialPressure()
           << pi.perspective() << pi.currentTime() << pi.drawingSpeed() << pi.canvasRotation()
           << pi.tiltDirectionOffset() << pi.canvasMirroredH() << pi.canvasMirroredV();
}

KisPaintInformation readPaintInformation(QDataStream &stream)
{
    QPointF pos;
    qreal pressure, xTilt, yTilt, rotation, tangentialPressure, perspective, time, speed, canvasRotation,
        tiltDirectionOffset;
    bool mirroredH, mirroredV;
    stream >> pos >> pressure >> xTilt >> yTilt >> rotation >> tangentialPressure >> perspective >> time >> speed
        >> canvasRotation >> tiltDirectionOffset >> mirroredH >> mirroredV;

    KisPaintInformation pi(pos, pressure, xTilt, yTilt, rotation, tangentialPressure, perspective, time, speed);
    pi.setCanvasRotation(canvasRotation);
    pi.setTiltDirectionOffset(tiltDirectionOffset);
    pi.setCanvasMirroredH(mirroredH);
    pi.setCanvasMirroredV(mirroredV);
    return pi;
}

/// The preset's XML as KisPaintOpPreset::toXML() writes it, without the
/// brush tips and other resources it embeds (the file stores them once in
/// its resources). toXML() looks those up, which only the GUI thread may
/// do, and a .kra is saved on another thread.
QString presetXml(KisPaintOpPresetSP preset)
{
    QDomDocument doc;
    QDomElement root = doc.createElement("Preset");
    root.setAttribute("paintopid", preset->paintOp().id());
    root.setAttribute("name", preset->name());
    preset->settings()->toXML(doc, root);
    doc.appendChild(root);
    return doc.toString(-1);
}

KisPaintOpPresetSP presetFromXml(const QString &xml)
{
    QDomDocument doc;
    if (!doc.setContent(xml)) {
        return nullptr;
    }
    KisPaintOpPresetSP preset(new KisPaintOpPreset());
    preset->fromXML(doc.documentElement(), KisGlobalResourcesInterface::instance());
    return preset->valid() ? preset : nullptr;
}

/**
 * The preset with a snapshot of the resources it uses, as a recorded preset
 * has (a clone of the stroke's own): redrawing then needs no lookup in the
 * global resources, which only the GUI thread may do, while redrawing runs
 * in the image's processing. The canvas resources it needs come from the
 * stroke.
 */
KisPaintOpPresetSP
snapshotPreset(KisPaintOpPresetSP preset, const KisRecordedBrushStroke &stroke, const QList<KoResourceSP> &saved)
{
    KoLocalStrokeCanvasResourcesSP canvasResources(new KoLocalStrokeCanvasResources());
    canvasResources->storeResource(KoCanvasResource::ForegroundColor, QVariant::fromValue(stroke.fgColor));
    canvasResources->storeResource(KoCanvasResource::BackgroundColor, QVariant::fromValue(stroke.bgColor));
    if (stroke.pattern) {
        canvasResources->storeResource(KoCanvasResource::CurrentPattern, QVariant::fromValue(stroke.pattern));
    }
    if (stroke.gradient) {
        canvasResources->storeResource(KoCanvasResource::CurrentGradient, QVariant::fromValue(stroke.gradient));
    }

    // the brush tips and textures saved in the file, which draw the stroke
    // as it was drawn even where they are not installed; the installed
    // ones for those the file lacks or could not load
    KisResourcesInterfaceSP lookup = KisGlobalResourcesInterface::instance();
    if (!saved.isEmpty()) {
        QList<KoResourceSP> resources = saved;
        const KisResourcesInterfaceSP savedResources = QSharedPointer<KisLocalStrokeResources>::create(saved);
        for (const KoResourceLoadResult &link : preset->linkedResources(savedResources)) {
            if (!link.resource()) {
                const KoResourceSignature signature = link.signature();
                if (KoResourceSP installed = KisGlobalResourcesInterface::instance()
                                                 ->source(signature.type)
                                                 .bestMatch(signature.md5sum, signature.filename, signature.name)) {
                    resources << installed;
                }
            }
        }
        lookup = QSharedPointer<KisLocalStrokeResources>::create(resources);
    }
    return preset->cloneWithResourcesSnapshot(lookup, canvasResources, nullptr);
}

/// A resource with its file, so that it loads where it is not installed.
/// An ABR brush tip has no file of its own (it lives in its .abr file): its
/// tip image is stored as a PNG.
void writeResource(QDataStream &stream, KoResourceSP resource)
{
    QBuffer buffer;
    buffer.open(QBuffer::WriteOnly);
    if (KisAbrBrushSP abrBrush = resource.dynamicCast<KisAbrBrush>()) {
        if (!abrBrush->brushTipImage().save(&buffer, "PNG")) {
            buffer.buffer().clear();
        }
    } else if (!resource->isSerializable() || !resource->saveToDevice(&buffer)) {
        buffer.buffer().clear();
    }
    stream << resource->resourceType().first << resource->resourceType().second << resource->md5Sum()
           << resource->filename() << resource->name() << buffer.buffer();
}

KoResourceSP readResource(QDataStream &stream)
{
    QString type;
    QString subType;
    QString md5;
    QString filename;
    QString name;
    QByteArray data;
    stream >> type >> subType >> md5 >> filename >> name >> data;

    if (subType == ResourceSubType::AbrBrushes && !data.isEmpty()) {
        const QImage tip = QImage::fromData(data, "PNG");
        if (!tip.isNull()) {
            KisAbrBrushSP abrBrush(new KisAbrBrush(filename, nullptr));
            abrBrush->setBrushTipImage(tip);
            abrBrush->setName(name);
            abrBrush->setMD5Sum(md5);
            abrBrush->setValid(true);
            return abrBrush;
        }
    }

    KoResourceSP resource;
    KisResourceLoaderBase *loader =
        KisResourceLoaderRegistry::instance()->loader(type, KisMimeDatabase::mimeTypeForFile(filename, false));
    if (loader) {
        resource = loader->create(filename);
    } else if (type == ResourceType::Patterns) {
        resource.reset(new KoPattern(filename));
    } else if (subType == ResourceSubType::StopGradients) {
        resource.reset(new KoStopGradient(filename));
    } else if (subType == ResourceSubType::SegmentedGradients) {
        resource.reset(new KoSegmentGradient(filename));
    }
    if (resource && !data.isEmpty()) {
        QBuffer buffer(&data);
        buffer.open(QBuffer::ReadOnly);
        if (resource->loadFromDevice(&buffer, KisGlobalResourcesInterface::instance())) {
            resource->setName(name);
            resource->setMD5Sum(md5);
            resource->setValid(true);
            return resource;
        }
    }

    // not saved with its file: the installed one, if any
    return KisGlobalResourcesInterface::instance()->source(type).bestMatch(md5, filename, name);
}
} // namespace

bool KisBrushStrokeLayer::saveStrokes(const QVector<KisRecordedBrushStrokeSP> &strokes, QIODevice *device)
{
    QStringList presets;
    QHash<const KisPaintOpPreset *, int> presetIndex;
    QVector<KoResourceSP> resources;
    QHash<QString, int> resourceIndex;

    auto addResource = [&](KoResourceSP resource) {
        if (!resource) {
            return -1;
        }
        const QString key = resource->resourceType().first + resource->md5Sum() + resource->filename();
        if (!resourceIndex.contains(key)) {
            resourceIndex.insert(key, resources.size());
            resources << resource;
        }
        return resourceIndex.value(key);
    };

    struct Indices {
        int preset;
        int pattern;
        int gradient;
    };
    QVector<Indices> indices;
    for (const KisRecordedBrushStrokeSP &stroke : strokes) {
        Indices index;
        // strokes usually share few presets: store each one once
        const KisPaintOpPreset *key = stroke->preset.data();
        if (!presetIndex.contains(key)) {
            const QString xml = presetXml(stroke->preset);
            int existing = presets.indexOf(xml);
            if (existing < 0) {
                existing = presets.size();
                presets << xml;
            }
            presetIndex.insert(key, existing);
        }
        index.preset = presetIndex.value(key);
        // the preset's brush tips and textures; a recorded preset has them
        // in its resources snapshot, which needs no lookup in the global
        // resources (another thread saves the .kra)
        if (stroke->preset->resourcesInterface().dynamicCast<KisLocalStrokeResources>()) {
            for (const KoResourceLoadResult &link :
                 stroke->preset->linkedResources(stroke->preset->resourcesInterface())) {
                addResource(link.resource());
            }
        }
        index.pattern = addResource(stroke->pattern);
        index.gradient = addResource(stroke->gradient);
        indices << index;
    }

    QDataStream stream(device);
    prepare(stream);
    stream << magic << version;

    stream << quint32(presets.size());
    for (const QString &xml : presets) {
        stream << xml;
    }
    stream << quint32(resources.size());
    for (KoResourceSP resource : resources) {
        writeResource(stream, resource);
    }

    stream << quint32(strokes.size());
    for (int i = 0; i < strokes.size(); i++) {
        const KisRecordedBrushStroke &stroke = *strokes[i];
        stream << qint32(indices[i].preset) << qint32(indices[i].pattern) << qint32(indices[i].gradient);
        writeColor(stream, stroke.fgColor);
        writeColor(stream, stroke.bgColor);
        stream << stroke.compositeOpId << stroke.opacity << stroke.effectiveZoom << qint32(stroke.seed)
               << qint32(stroke.strokeInfoCount) << stroke.deviceOffset;
        stream << stroke.mirrorHorizontal << stroke.mirrorVertical << stroke.mirrorAxesCenter;
        stream << quint32(stroke.starts.size());
        for (const KisRecordedBrushStroke::Start &start : stroke.starts) {
            stream << start.hasLastDab << start.lastPosition << start.lastAngle << start.spacingUpdateInterval
                   << start.timingUpdateInterval << qint32(start.dabSeqNo);
        }

        stream << quint32(stroke.jobs.size());
        for (const KisRecordedBrushStroke::Job &job : stroke.jobs) {
            stream << qint32(job.type) << qint32(job.strokeInfoId);
            writePaintInformation(stream, job.pi1);
            writePaintInformation(stream, job.pi2);
            stream << job.control1 << job.control2;
        }
    }
    return stream.status() == QDataStream::Ok;
}

bool KisBrushStrokeLayer::loadStrokes(QIODevice *device, QVector<KisRecordedBrushStrokeSP> *strokes)
{
    QDataStream stream(device);
    prepare(stream);

    quint32 fileMagic = 0;
    quint32 fileVersion = 0;
    stream >> fileMagic >> fileVersion;
    if (fileMagic != magic || fileVersion > version) {
        warnKrita << "KisBrushStrokeLayer: unknown brush stroke data" << Qt::hex << fileMagic << Qt::dec << fileVersion;
        return false;
    }

    quint32 count = 0;
    stream >> count;
    QVector<KisPaintOpPresetSP> presets;
    for (quint32 i = 0; i < count && stream.status() == QDataStream::Ok; i++) {
        QString xml;
        stream >> xml;
        presets << presetFromXml(xml);
    }

    stream >> count;
    QVector<KoResourceSP> resources;
    for (quint32 i = 0; i < count && stream.status() == QDataStream::Ok; i++) {
        resources << readResource(stream);
    }

    QList<KoResourceSP> saved;
    for (KoResourceSP resource : resources) {
        if (resource) {
            saved << resource;
        }
    }
    // presets that need no canvas resources share one snapshot
    QHash<int, KisPaintOpPresetSP> sharedSnapshots;

    stream >> count;
    QVector<KisRecordedBrushStrokeSP> result;
    for (quint32 i = 0; i < count && stream.status() == QDataStream::Ok; i++) {
        QSharedPointer<KisRecordedBrushStroke> stroke(new KisRecordedBrushStroke());
        qint32 preset, pattern, gradient, seed, strokeInfoCount;
        stream >> preset >> pattern >> gradient;
        stroke->fgColor = readColor(stream);
        stroke->bgColor = readColor(stream);
        stream >> stroke->compositeOpId >> stroke->opacity >> stroke->effectiveZoom >> seed >> strokeInfoCount
            >> stroke->deviceOffset;
        stroke->seed = seed;
        stroke->strokeInfoCount = strokeInfoCount;
        // the Multibrush tool paints up to 4 copies per brush in Snowflake
        // mode (200) and any number in Copy Translate; the limit only
        // guards against damaged data
        if (stream.status() != QDataStream::Ok || strokeInfoCount < 1 || strokeInfoCount > maximumStrokeInfoCount) {
            return false;
        }
        if (fileVersion >= 3) {
            stream >> stroke->mirrorHorizontal >> stroke->mirrorVertical >> stroke->mirrorAxesCenter;
        }
        if (fileVersion >= 2) {
            quint32 startCount = 0;
            stream >> startCount;
            // one start per copy; none for strokes read from version 1
            if (startCount > quint32(strokeInfoCount)) {
                return false;
            }
            for (quint32 j = 0; j < startCount && stream.status() == QDataStream::Ok; j++) {
                KisRecordedBrushStroke::Start start;
                qint32 dabSeqNo = 0;
                stream >> start.hasLastDab >> start.lastPosition >> start.lastAngle >> start.spacingUpdateInterval
                    >> start.timingUpdateInterval >> dabSeqNo;
                start.dabSeqNo = dabSeqNo;
                if (start.hasLastDab && !validPosition(start.lastPosition)) {
                    return false;
                }
                stroke->starts << start;
            }
        }
        if (!validPosition(QPointF(stroke->deviceOffset)) || !validPosition(stroke->mirrorAxesCenter)
            || !std::isfinite(stroke->opacity) || !std::isfinite(stroke->effectiveZoom)) {
            return false;
        }

        quint32 jobCount = 0;
        stream >> jobCount;
        for (quint32 j = 0; j < jobCount && stream.status() == QDataStream::Ok; j++) {
            KisRecordedBrushStroke::Job job;
            qint32 type, strokeInfoId;
            stream >> type >> strokeInfoId;
            if (stream.status() != QDataStream::Ok || strokeInfoId < 0 || strokeInfoId >= strokeInfoCount
                || (type != FreehandStrokeStrategy::Data::POINT && type != FreehandStrokeStrategy::Data::LINE
                    && type != FreehandStrokeStrategy::Data::CURVE)) {
                return false;
            }
            job.type = type;
            job.strokeInfoId = strokeInfoId;
            job.pi1 = readPaintInformation(stream);
            job.pi2 = readPaintInformation(stream);
            stream >> job.control1 >> job.control2;
            if (!validPosition(job.pi1.pos()) || !validPosition(job.pi2.pos()) || !validPosition(job.control1)
                || !validPosition(job.control2)) {
                return false;
            }
            stroke->jobs << job;
        }

        if (preset < 0 || preset >= presets.size() || pattern >= resources.size() || gradient >= resources.size()) {
            stream.setStatus(QDataStream::ReadCorruptData);
            break;
        }
        stroke->pattern = pattern >= 0 ? resources[pattern].dynamicCast<KoPattern>() : nullptr;
        stroke->gradient = gradient >= 0 ? resources[gradient].dynamicCast<KoAbstractGradient>() : nullptr;
        if (!presets[preset]) {
            // the brush engine is missing: the layer keeps its pixels,
            // and its record no longer matches them
            warnKrita << "KisBrushStrokeLayer: a recorded brush preset could not be loaded";
            continue;
        }
        if (!presets[preset]->requiredCanvasResources().isEmpty()) {
            stroke->preset = snapshotPreset(presets[preset], *stroke, saved);
        } else {
            if (!sharedSnapshots.contains(preset)) {
                sharedSnapshots.insert(preset, snapshotPreset(presets[preset], *stroke, saved));
            }
            stroke->preset = sharedSnapshots.value(preset);
        }
        result << stroke;
    }

    if (stream.status() != QDataStream::Ok) {
        warnKrita << "KisBrushStrokeLayer: damaged brush stroke data";
        return false;
    }
    *strokes = result;
    return true;
}
