/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisLazyToolsFastExport.h"

#include "KisDocument.h"
#include "KisImageBarrierLock.h"
#include "KisPart.h"
#include "kis_config.h"
#include "kis_group_layer.h"
#include "kis_image.h"
#include "kis_paint_layer.h"
#include "kis_pixel_selection.h"
#include "kis_properties_configuration.h"
#include "kis_selection.h"
#include "kis_sequential_iterator.h"
#include <KoColor.h>
#include <KoDocumentInfo.h>
#include <QDir>
#include <QFileInfo>
#include <kconfiggroup.h>
#include <klocalizedstring.h>

namespace
{
QByteArray exportMimeType(const QString &extension)
{
    return extension == QLatin1String("png") ? QByteArray("image/png") : QByteArray("image/jpeg");
}
} // namespace

KisPropertiesConfigurationSP KisLazyToolsFastExport::exportOptions(const QString &extension, const Settings &settings)
{
    // Start from the format options remembered by File > Export (and by the
    // previous Fast Image Export), so options this dialog does not show keep
    // their last values instead of falling back to the defaults.
    KisPropertiesConfigurationSP options(new KisPropertiesConfiguration);
    const QString saved = KisConfig(true).exportConfigurationXML(QString::fromLatin1(exportMimeType(extension)));
    if (!saved.isEmpty()) {
        options->fromXML(saved);
    }
    if (extension == QLatin1String("png")) {
        options->setProperty("compression", settings.pngCompression);
        options->setProperty("alpha", settings.pngAlpha);
    } else {
        options->setProperty("quality", settings.jpegQuality);
    }
    return options;
}

KisLazyToolsFastExport::Settings KisLazyToolsFastExport::Settings::load(KSharedConfigPtr config)
{
    const auto group = config->group(QString());
    Settings settings;
    settings.folder = group.readEntry("Solstice/FastExportFolder", QString());
    settings.pngCompression = group.readEntry("Solstice/FastExportPngCompression", 6);
    settings.pngAlpha = group.readEntry("Solstice/FastExportPngAlpha", true);
    settings.jpegQuality = group.readEntry("Solstice/FastExportJpegQuality", 90);
    settings.destination = qBound(0, group.readEntry("Solstice/FastExportDestination", 1), 2);
    return settings;
}

void KisLazyToolsFastExport::Settings::save(KSharedConfigPtr config) const
{
    auto group = config->group(QString());
    group.writeEntry("Solstice/FastExportFolder", folder);
    group.writeEntry("Solstice/FastExportPngCompression", pngCompression);
    group.writeEntry("Solstice/FastExportPngAlpha", pngAlpha);
    group.writeEntry("Solstice/FastExportJpegQuality", jpegQuality);
    group.writeEntry("Solstice/FastExportDestination", destination);
    group.sync();
}

QStringList KisLazyToolsFastExport::explorerFolders(KSharedConfigPtr config)
{
    return normalizedExplorerFolders(
        config->group(QString()).readEntry("Solstice/FastExportExplorerFolders", QStringList()));
}

void KisLazyToolsFastExport::setExplorerFolders(const QStringList &folders, KSharedConfigPtr config)
{
    auto group = config->group(QString());
    group.writeEntry("Solstice/FastExportExplorerFolders", normalizedExplorerFolders(folders));
    group.sync();
}

QStringList KisLazyToolsFastExport::normalizedExplorerFolders(const QStringList &folders)
{
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    QStringList result;
    for (const QString &folder : folders) {
        const QString trimmed = QDir::fromNativeSeparators(folder.trimmed());
        if (!trimmed.isEmpty() && !result.contains(trimmed, sensitivity)) {
            result << trimmed;
        }
    }
    return result;
}

QString KisLazyToolsFastExport::normalizedBaseName(const QString &name, const QString &extension)
{
    QString result = name.trimmed();
    const QStringList extensions = {extension, QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg")};
    for (const QString &candidate : extensions) {
        const QString suffix = QLatin1Char('.') + candidate;
        if (!candidate.isEmpty() && result.size() > suffix.size() && result.endsWith(suffix, Qt::CaseInsensitive)) {
            result.chop(suffix.size());
            break;
        }
    }
    return result;
}

bool KisLazyToolsFastExport::isValidBaseName(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..") || name.endsWith(QLatin1Char('.'))
        || name.endsWith(QLatin1Char(' '))) {
        return false;
    }
    for (const QChar character : name) {
        if (character.unicode() < 0x20 || QStringLiteral("\\/:*?\"<>|").contains(character)) {
            return false;
        }
    }
    return true;
}

QString KisLazyToolsFastExport::outputPath(const QString &sourcePath,
                                           const QString &extension,
                                           const QString &outputFolder,
                                           const QString &baseName)
{
    const QFileInfo source(sourcePath);
    const QString folder = outputFolder.isEmpty() ? source.absolutePath() : outputFolder;
    const QString name = baseName.isEmpty() ? source.completeBaseName() : baseName;
    return QDir(folder).filePath(name + QLatin1Char('.') + extension);
}

KisLazyToolsFastExport::Result KisLazyToolsFastExport::run(const QList<QPointer<KisDocument>> &documents,
                                                           const QString &extension,
                                                           const QString &outputFolder,
                                                           const Settings &settings,
                                                           const WaitForImage &wait,
                                                           const QString &baseName)
{
    const QByteArray mimeType = exportMimeType(extension);
    Result result;
    for (const QPointer<KisDocument> &document : documents) {
        if (!document || document->localFilePath().isEmpty()) {
            result.skipped << (document ? document->caption() : i18n("Unknown document"));
            continue;
        }
        const QString path = outputPath(document->localFilePath(), extension, outputFolder, baseName);
        if (wait(document->image())
            && document->exportDocumentSync(path, mimeType, exportOptions(extension, settings))) {
            ++result.exported;
        } else {
            result.skipped << document->caption();
        }
    }
    return result;
}

KisLazyToolsFastExport::RegionResult KisLazyToolsFastExport::exportRegion(KisDocument *document,
                                                                          const QString &extension,
                                                                          const QString &outputFolder,
                                                                          const Settings &settings,
                                                                          const WaitForImage &wait,
                                                                          const QString &baseName)
{
    if (!document || document->localFilePath().isEmpty()) {
        return RegionResult::Unnamed;
    }
    KisImageSP sourceImage = document->image();
    if (!sourceImage || !wait(sourceImage)) {
        return RegionResult::Failed;
    }
    // Behave like run(): format options and warnings appear unless the source
    // document is in batch mode.
    QScopedPointer<KisDocument> regionDocument(createRegionDocument(document, document->fileBatchMode()));
    if (!regionDocument) {
        return RegionResult::NoSelection;
    }
    const QString path = outputPath(document->localFilePath(), extension, outputFolder, baseName);
    return regionDocument->exportDocumentSync(path, exportMimeType(extension), exportOptions(extension, settings))
        ? RegionResult::Exported
        : RegionResult::Failed;
}

KisDocument *KisLazyToolsFastExport::createRegionDocument(KisDocument *document, bool batchMode)
{
    KisImageSP image;
    if (document) {
        image = document->image();
    }
    return createRegionDocument(document, batchMode, image);
}

KisDocument *
KisLazyToolsFastExport::createRegionDocument(KisDocument *document, bool batchMode, KisImageSP projectionImage)
{
    KisImageSP sourceImage;
    if (document) {
        sourceImage = document->image();
    }
    if (!sourceImage || !projectionImage || projectionImage->bounds() != sourceImage->bounds()) {
        return nullptr;
    }

    // Same masking and cropping as KisMainWindow::slotExportRegion(), without
    // its fallback to the active layer: Fast Image Export needs a selection.
    QRect bounds;
    KisPaintDeviceSP device;
    {
        KisImageBarrierLock lock(sourceImage);
        KisSelectionSP selection = sourceImage->globalSelection();
        if (!selection) {
            return nullptr;
        }
        KisPixelSelectionSP mask = selection->projection();
        bounds = mask->selectedExactRect() & sourceImage->bounds();
        if (bounds.isEmpty()) {
            return nullptr;
        }

        if (projectionImage == sourceImage) {
            device = new KisPaintDevice(*sourceImage->projection());
        } else {
            KisImageBarrierLock projectionLock(projectionImage);
            device = new KisPaintDevice(*projectionImage->projection());
        }
        const KoColorSpace *colorSpace = device->colorSpace();
        KisSequentialIterator deviceIt(device, bounds);
        KisSequentialConstIterator maskIt(mask, bounds);
        while (deviceIt.nextPixel() && maskIt.nextPixel()) {
            colorSpace->applyAlphaU8Mask(deviceIt.rawData(), maskIt.oldRawData(), 1);
        }
        if (device->defaultPixel() != KoColor::createTransparent(colorSpace) && bounds != sourceImage->bounds()) {
            device->setDefaultPixel(KoColor::createTransparent(colorSpace));
            device->purgeDefaultPixels();
        }
        device->crop(bounds);
        device->moveTo(device->offset() - bounds.topLeft());
    }

    KisDocument *regionDocument = KisPart::instance()->createDocument();
    regionDocument->setFileBatchMode(batchMode);
    KisImageSP regionImage = new KisImage(regionDocument->createUndoStore(),
                                          bounds.width(),
                                          bounds.height(),
                                          sourceImage->colorSpace(),
                                          i18n("Export Region"));
    regionImage->setResolution(sourceImage->xRes(), sourceImage->yRes());
    regionImage->addNode(new KisPaintLayer(regionImage, i18n("Exported Region"), OPACITY_OPAQUE_U8, device),
                         regionImage->rootLayer());
    regionDocument->setCurrentImage(regionImage);
    regionDocument->documentInfo()->setAboutInfo("title", document->caption());
    regionImage->initialRefreshGraph();
    regionImage->waitForDone();
    return regionDocument;
}
