/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisLazyToolsFastExportPresets.h"

#include "KisDocument.h"
#include "kis_config.h"
#include "kis_debug.h"
#include "kis_image.h"
#include "kis_layer_composition.h"
#include "kis_properties_configuration.h"
#include <QDir>
#include <QDomDocument>
#include <QFileInfo>
#include <QHash>
#include <QScopedPointer>
#include <QUuid>
#include <algorithm>
#include <klocalizedstring.h>
#include <memory>

namespace
{
const int FormatVersion = 1;
const QString RootTag = QStringLiteral("fastExport");
const QString PresetTag = QStringLiteral("preset");
const QString OptionsTag = QStringLiteral("formatOptions");

QByteArray exportMimeType(const QString &format)
{
    return format == QLatin1String("png") ? QByteArray("image/png") : QByteArray("image/jpeg");
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool readBool(const QDomElement &element, const QString &name, bool defaultValue)
{
    return element.hasAttribute(name) ? element.attribute(name) == QLatin1String("1") : defaultValue;
}

int readInt(const QDomElement &element, const QString &name, int defaultValue, int minimum, int maximum)
{
    bool ok = false;
    const int value = element.attribute(name).toInt(&ok);
    return ok ? qBound(minimum, value, maximum) : defaultValue;
}

QString pathKey(const QString &path)
{
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    return clean.toLower();
#else
    return clean;
#endif
}

// A copy of the document's image in batch mode, with the layer comp applied
// when there is one. Null when the copy fails or the comp is gone.
KisDocument *createSnapshot(KisDocument *document, const QUuid &layerComp)
{
    std::unique_ptr<KisDocument> snapshot(document->lockAndCreateSnapshot());
    if (!snapshot || !snapshot->image()) {
        return nullptr;
    }
    snapshot->setFileBatchMode(true);
    if (layerComp.isNull()) {
        return snapshot.release();
    }
    // Image copies keep their compositions' ids.
    KisImageSP image = snapshot->image();
    KisLayerCompositionSP composition;
    for (const KisLayerCompositionSP &candidate : image->compositions()) {
        if (candidate->uuid() == layerComp) {
            composition = candidate;
        }
    }
    if (!composition) {
        return nullptr;
    }
    image->barrierLock();
    composition->apply();
    image->unlock();
    image->refreshGraphAsync();
    image->waitForDone();
    return snapshot.release();
}

QString reason(const QString &document, const QString &text)
{
    return i18nc("Fast Image Export skipped item: document, reason", "%1: %2", document, text);
}

QString reason(const QString &document, const QString &preset, const QString &text)
{
    return i18nc("Fast Image Export skipped item: document, saved export, reason",
                 "%1 / %2: %3",
                 document,
                 preset,
                 text);
}
} // namespace

bool KisLazyToolsFastExportPresets::Preset::operator==(const Preset &other) const
{
    return id == other.id && name == other.name && enabled == other.enabled && format == other.format
        && pngCompression == other.pngCompression && pngAlpha == other.pngAlpha && jpegQuality == other.jpegQuality
        && formatOptions == other.formatOptions && destination == other.destination && folder == other.folder
        && folderRelative == other.folderRelative && nameRule == other.nameRule && fixedName == other.fixedName
        && area == other.area && layerComp == other.layerComp && layerCompName == other.layerCompName;
}

KisLazyToolsFastExportPresets::Preset KisLazyToolsFastExportPresets::newPreset(const QString &name)
{
    Preset preset;
    preset.id = newId();
    preset.name = name;
    return preset;
}

QByteArray KisLazyToolsFastExportPresets::toXml(const QList<Preset> &presets)
{
    if (presets.isEmpty()) {
        return QByteArray();
    }
    QDomDocument document;
    QDomElement root = document.createElement(RootTag);
    root.setAttribute(QStringLiteral("version"), FormatVersion);
    document.appendChild(root);
    for (const Preset &preset : presets) {
        QDomElement element = document.createElement(PresetTag);
        element.setAttribute(QStringLiteral("id"), preset.id);
        element.setAttribute(QStringLiteral("name"), preset.name);
        element.setAttribute(QStringLiteral("enabled"), preset.enabled ? 1 : 0);
        element.setAttribute(QStringLiteral("format"), preset.format);
        element.setAttribute(QStringLiteral("pngCompression"), preset.pngCompression);
        element.setAttribute(QStringLiteral("pngAlpha"), preset.pngAlpha ? 1 : 0);
        element.setAttribute(QStringLiteral("jpegQuality"), preset.jpegQuality);
        element.setAttribute(QStringLiteral("destination"),
                             preset.destination == Destination::Folder ? QStringLiteral("folder")
                                                                       : QStringLiteral("same"));
        element.setAttribute(QStringLiteral("folder"), preset.folder);
        element.setAttribute(QStringLiteral("folderRelative"), preset.folderRelative ? 1 : 0);
        element.setAttribute(QStringLiteral("nameRule"),
                             preset.nameRule == NameRule::Fixed ? QStringLiteral("fixed") : QStringLiteral("document"));
        element.setAttribute(QStringLiteral("fixedName"), preset.fixedName);
        element.setAttribute(QStringLiteral("area"),
                             preset.area == Area::Selection ? QStringLiteral("selection") : QStringLiteral("image"));
        // Only exports of a layer comp have them, so older data is unchanged.
        if (!preset.layerComp.isEmpty()) {
            element.setAttribute(QStringLiteral("layerComp"), preset.layerComp);
            element.setAttribute(QStringLiteral("layerCompName"), preset.layerCompName);
        }
        if (!preset.formatOptions.isEmpty()) {
            QDomElement options = document.createElement(OptionsTag);
            options.appendChild(document.createTextNode(preset.formatOptions));
            element.appendChild(options);
        }
        root.appendChild(element);
    }
    return document.toByteArray();
}

QList<KisLazyToolsFastExportPresets::Preset> KisLazyToolsFastExportPresets::fromXml(const QByteArray &data)
{
    QList<Preset> presets;
    if (data.isEmpty()) {
        return presets;
    }
    QDomDocument document;
    if (!document.setContent(data) || document.documentElement().tagName() != RootTag) {
        warnUI << "Fast Image Export: ignoring unreadable saved export settings";
        return presets;
    }
    QStringList ids;
    for (QDomElement element = document.documentElement().firstChildElement(PresetTag); !element.isNull();
         element = element.nextSiblingElement(PresetTag)) {
        Preset preset;
        preset.format = element.attribute(QStringLiteral("format"));
        if (preset.format != QLatin1String("png") && preset.format != QLatin1String("jpg")) {
            continue;
        }
        preset.id = element.attribute(QStringLiteral("id"));
        if (preset.id.isEmpty() || ids.contains(preset.id)) {
            preset.id = newId();
        }
        ids << preset.id;
        preset.name = element.attribute(QStringLiteral("name"));
        preset.enabled = readBool(element, QStringLiteral("enabled"), true);
        preset.pngCompression = readInt(element, QStringLiteral("pngCompression"), 6, 0, 9);
        preset.pngAlpha = readBool(element, QStringLiteral("pngAlpha"), true);
        preset.jpegQuality = readInt(element, QStringLiteral("jpegQuality"), 90, 0, 100);
        preset.destination = element.attribute(QStringLiteral("destination")) == QLatin1String("folder")
            ? Destination::Folder
            : Destination::SameFolder;
        preset.folder = element.attribute(QStringLiteral("folder"));
        preset.folderRelative = readBool(element, QStringLiteral("folderRelative"), false);
        preset.nameRule = element.attribute(QStringLiteral("nameRule")) == QLatin1String("fixed")
            ? NameRule::Fixed
            : NameRule::DocumentName;
        preset.fixedName = element.attribute(QStringLiteral("fixedName"));
        preset.area = element.attribute(QStringLiteral("area")) == QLatin1String("selection") ? Area::Selection
                                                                                              : Area::WholeImage;
        preset.layerComp = element.attribute(QStringLiteral("layerComp"));
        preset.layerCompName =
            preset.layerComp.isEmpty() ? QString() : element.attribute(QStringLiteral("layerCompName"));
        preset.formatOptions = element.firstChildElement(OptionsTag).text();
        presets << preset;
    }
    return presets;
}

bool KisLazyToolsFastExportPresets::canStore(const KisDocument *document)
{
    if (!document) {
        return false;
    }
    const QString path = document->localFilePath();
    if (path.isEmpty()) {
        return true;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("kra") || suffix == QLatin1String("krz");
}

QList<KisLazyToolsFastExportPresets::Preset> KisLazyToolsFastExportPresets::presets(const KisDocument *document)
{
    return document ? fromXml(document->solsticeFastExportData()) : QList<Preset>();
}

void KisLazyToolsFastExportPresets::setPresets(KisDocument *document, const QList<Preset> &presets)
{
    if (!document) {
        return;
    }
    const QByteArray data = toXml(presets);
    if (data == document->solsticeFastExportData()) {
        return;
    }
    document->setSolsticeFastExportData(data);
    if (!canStore(document)) {
        return;
    }
    // Like Layer Comps: the change is saved with the document, but not undoable.
    if (KisImageSP image = document->image()) {
        image->setModifiedWithoutUndo();
    } else {
        document->setModified(true);
    }
}

QString KisLazyToolsFastExportPresets::resolvedFolder(const Preset &preset, const QString &documentPath)
{
    const QString documentFolder = documentPath.isEmpty() ? QString() : QFileInfo(documentPath).absolutePath();
    if (preset.destination == Destination::SameFolder) {
        return documentFolder;
    }
    if (preset.folder.trimmed().isEmpty()) {
        return QString();
    }
    const QString folder = QDir::fromNativeSeparators(preset.folder.trimmed());
    if (!preset.folderRelative) {
        return QDir::cleanPath(folder);
    }
    return documentFolder.isEmpty() ? QString() : QDir::cleanPath(QDir(documentFolder).absoluteFilePath(folder));
}

QString
KisLazyToolsFastExportPresets::storedFolder(const QString &absoluteFolder, bool relative, const QString &documentPath)
{
    const QString folder = QDir::fromNativeSeparators(absoluteFolder.trimmed());
    if (folder.isEmpty() || !relative || documentPath.isEmpty()) {
        return folder;
    }
    // On another drive this stays absolute, which resolvedFolder() accepts too.
    return QDir::fromNativeSeparators(QDir(QFileInfo(documentPath).absolutePath()).relativeFilePath(folder));
}

QString KisLazyToolsFastExportPresets::outputPath(const Preset &preset,
                                                  const QString &documentPath,
                                                  const QString &layerCompName)
{
    const QString folder = resolvedFolder(preset, documentPath);
    if (folder.isEmpty()) {
        return QString();
    }
    QString name = preset.nameRule == NameRule::Fixed
        ? KisLazyToolsFastExport::normalizedBaseName(preset.fixedName, preset.format)
        : QFileInfo(documentPath).completeBaseName();
    if (preset.nameRule == NameRule::DocumentName && !layerCompName.isEmpty()) {
        name += QLatin1Char('_') + layerCompName;
    }
    if (!KisLazyToolsFastExport::isValidBaseName(name)) {
        return QString();
    }
    return QDir(folder).filePath(name + QLatin1Char('.') + preset.format);
}

QList<KisLazyToolsFastExportPresets::LayerComp> KisLazyToolsFastExportPresets::layerComps(KisDocument *document)
{
    QList<LayerComp> comps;
    KisImageSP image;
    if (document) {
        image = document->image();
    }
    if (!image) {
        return comps;
    }
    for (const KisLayerCompositionSP &composition : image->compositions()) {
        comps << LayerComp{composition->uuid().toString(QUuid::WithoutBraces), composition->name()};
    }
    return comps;
}

KisLayerCompositionSP KisLazyToolsFastExportPresets::findLayerComp(KisImageSP image, const Preset &preset)
{
    if (!image || preset.layerComp.isEmpty()) {
        return KisLayerCompositionSP();
    }
    const QUuid id(preset.layerComp);
    const QList<KisLayerCompositionSP> compositions = image->compositions();
    for (const KisLayerCompositionSP &composition : compositions) {
        if (!id.isNull() && composition->uuid() == id) {
            return composition;
        }
    }
    KisLayerCompositionSP named;
    for (const KisLayerCompositionSP &composition : compositions) {
        if (!preset.layerCompName.isEmpty() && composition->name() == preset.layerCompName) {
            if (named) {
                // Two comps with that name: neither is certainly the one.
                return KisLayerCompositionSP();
            }
            named = composition;
        }
    }
    return named;
}

KisPropertiesConfigurationSP KisLazyToolsFastExportPresets::exportOptions(const Preset &preset)
{
    KisPropertiesConfigurationSP options(new KisPropertiesConfiguration);
    const QString saved = preset.formatOptions.isEmpty()
        ? KisConfig(true).exportConfigurationXML(QString::fromLatin1(exportMimeType(preset.format)))
        : preset.formatOptions;
    if (!saved.isEmpty()) {
        options->fromXML(saved);
    }
    if (preset.format == QLatin1String("png")) {
        options->setProperty("compression", preset.pngCompression);
        options->setProperty("alpha", preset.pngAlpha);
    } else {
        options->setProperty("quality", preset.jpegQuality);
    }
    return options;
}

QString KisLazyToolsFastExportPresets::summary(const Preset &preset, const QString &documentPath)
{
    QStringList parts;
    parts << (preset.format == QLatin1String("png") ? i18n("PNG") : i18n("JPEG"));
    if (preset.destination == Destination::SameFolder) {
        parts << i18n("Same folder");
    } else if (preset.folder.trimmed().isEmpty()) {
        parts << i18n("No folder");
    } else {
        const QString resolved = resolvedFolder(preset, documentPath);
        parts << QDir::toNativeSeparators(resolved.isEmpty() ? preset.folder : resolved);
    }
    if (preset.nameRule == NameRule::Fixed && !preset.fixedName.isEmpty()) {
        parts << preset.fixedName;
    }
    if (preset.area == Area::Selection) {
        parts << i18n("Selection");
    }
    if (!preset.layerComp.isEmpty()) {
        parts << i18nc("Saved export summary: the layer comp it exports", "Layer comp %1", preset.layerCompName);
    }
    return parts.join(QStringLiteral(" · "));
}

KisLazyToolsFastExportPresets::Result
KisLazyToolsFastExportPresets::run(const QList<QPointer<KisDocument>> &documents,
                                   Which which,
                                   const KisLazyToolsFastExport::WaitForImage &wait)
{
    struct Item {
        Preset preset;
        QString path;
        // The layer comp to apply first; null for the layers as they are.
        QUuid layerComp;
    };
    struct Job {
        QPointer<KisDocument> document;
        QString caption;
        QList<Item> items;
    };

    Result result;
    QList<Job> jobs;
    for (const QPointer<KisDocument> &document : documents) {
        if (!document) {
            result.skipped << reason(i18n("Unknown document"), i18n("not available"));
            continue;
        }
        const QString caption = document->caption();
        const QString documentPath = document->localFilePath();
        if (documentPath.isEmpty()) {
            result.skipped << reason(caption, i18n("not saved yet"));
            continue;
        }
        QList<Preset> selected = presets(document);
        if (selected.isEmpty()) {
            result.skipped << reason(caption, i18n("no saved exports"));
            continue;
        }
        if (which == Which::Checked) {
            selected.erase(std::remove_if(selected.begin(),
                                          selected.end(),
                                          [](const Preset &preset) {
                                              return !preset.enabled;
                                          }),
                           selected.end());
            if (selected.isEmpty()) {
                result.skipped << reason(caption, i18n("no checked exports"));
                continue;
            }
        }
        Job job{document, caption, {}};
        for (const Preset &preset : selected) {
            QUuid layerComp;
            QString layerCompName;
            if (!preset.layerComp.isEmpty()) {
                // A deleted comp is reported rather than exporting the layers
                // as they happen to be.
                const KisLayerCompositionSP composition = findLayerComp(document->image(), preset);
                if (!composition) {
                    result.skipped << reason(caption, preset.name, i18n("layer comp not found"));
                    continue;
                }
                layerComp = composition->uuid();
                layerCompName = composition->name();
            }
            const QString path = outputPath(preset, documentPath, layerCompName);
            if (path.isEmpty()) {
                result.skipped << reason(caption,
                                         preset.name,
                                         resolvedFolder(preset, documentPath).isEmpty() ? i18n("no export folder")
                                                                                        : i18n("invalid file name"));
                continue;
            }
            job.items << Item{preset, path, layerComp};
        }
        jobs << job;
    }

    // Two presets writing the same file would silently replace one result
    // with the other, so such a run writes nothing.
    QHash<QString, int> uses;
    for (const Job &job : jobs) {
        for (const Item &item : job.items) {
            ++uses[pathKey(item.path)];
        }
    }
    for (const Job &job : jobs) {
        for (const Item &item : job.items) {
            const QString native = QDir::toNativeSeparators(item.path);
            if (uses.value(pathKey(item.path)) > 1 && !result.conflicts.contains(native)) {
                result.conflicts << native;
            }
        }
    }
    if (!result.conflicts.isEmpty()) {
        return result;
    }

    for (const Job &job : jobs) {
        if (job.items.isEmpty()) {
            continue;
        }
        if (!job.document || !wait(job.document->image())) {
            for (const Item &item : job.items) {
                result.skipped << reason(job.caption, item.preset.name, i18n("could not export"));
            }
            continue;
        }
        // The layers as they are, and each layer comp, get one snapshot and one
        // region, shared by all presets that use them. A comp is applied to its
        // snapshot only, so the document, its canvas and undo stack stay as they
        // are. Batch mode skips the format options dialog; the preset has the options.
        QList<QUuid> layerComps;
        for (const Item &item : job.items) {
            if (!layerComps.contains(item.layerComp)) {
                layerComps << item.layerComp;
            }
        }
        for (const QUuid &layerComp : layerComps) {
            QScopedPointer<KisDocument> snapshot;
            QScopedPointer<KisDocument> region;
            bool snapshotCreated = false;
            bool regionCreated = false;
            for (const Item &item : job.items) {
                if (item.layerComp != layerComp) {
                    continue;
                }
                if (!snapshotCreated && job.document && (!layerComp.isNull() || item.preset.area == Area::WholeImage)) {
                    snapshot.reset(createSnapshot(job.document, layerComp));
                    snapshotCreated = true;
                }
                KisDocument *source = nullptr;
                if (item.preset.area == Area::Selection) {
                    if (!regionCreated && job.document && (layerComp.isNull() || snapshot)) {
                        // The selection is the document's: applying a comp can
                        // hide its selection mask in the snapshot.
                        region.reset(
                            layerComp.isNull()
                                ? KisLazyToolsFastExport::createRegionDocument(job.document, true)
                                : KisLazyToolsFastExport::createRegionDocument(job.document, true, snapshot->image()));
                        regionCreated = true;
                    }
                    if (!region) {
                        result.skipped << reason(job.caption,
                                                 item.preset.name,
                                                 regionCreated ? i18n("no selection") : i18n("could not export"));
                        continue;
                    }
                    source = region.data();
                } else {
                    source = snapshot.data();
                }
                if (source
                    && source->exportDocumentSync(item.path,
                                                  exportMimeType(item.preset.format),
                                                  exportOptions(item.preset))) {
                    ++result.exported;
                } else {
                    result.skipped << reason(job.caption, item.preset.name, i18n("could not export"));
                }
            }
        }
    }
    return result;
}
