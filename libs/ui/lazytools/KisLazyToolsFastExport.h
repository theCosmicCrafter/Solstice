/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_LAZY_TOOLS_FAST_EXPORT_H
#define KIS_LAZY_TOOLS_FAST_EXPORT_H

#include <QList>
#include <QPointer>
#include <QStringList>
#include <functional>
#include <ksharedconfig.h>
#include <kis_types.h>
#include <kritaui_export.h>

class KisDocument;

namespace KisLazyToolsFastExport
{
struct KRITAUI_EXPORT Settings {
    QString folder;
    int pngCompression{6};
    bool pngAlpha{true};
    int jpegQuality{90};
    // The one-off export's Destination: 0 the document's folder, 1 the
    // configured export folder (default), 2 a folder chosen on export.
    int destination{1};

    static Settings load(KSharedConfigPtr config = KSharedConfig::openConfig());
    void save(KSharedConfigPtr config = KSharedConfig::openConfig()) const;
};

// Folders the export dialog can open in the system file manager. They are
// never used as export destinations, and they are stored apart from Settings so
// a modeless export dialog cannot overwrite a list edited in Preferences.
KRITAUI_EXPORT QStringList explorerFolders(KSharedConfigPtr config = KSharedConfig::openConfig());
KRITAUI_EXPORT void setExplorerFolders(const QStringList &folders,
                                       KSharedConfigPtr config = KSharedConfig::openConfig());
// Trims, drops empty entries and removes duplicates while keeping the order.
KRITAUI_EXPORT QStringList normalizedExplorerFolders(const QStringList &folders);

struct Result {
    int exported{0};
    QStringList skipped;
};

using WaitForImage = std::function<bool(KisImageSP)>;
// The format options for an export: the options last used for this format
// (as File > Export remembers them), with this dialog's PNG compression and
// alpha, or JPEG quality, applied on top.
KRITAUI_EXPORT KisPropertiesConfigurationSP exportOptions(const QString &extension, const Settings &settings);
// Trims a user-entered name and drops a trailing image extension (.png, .jpg,
// .jpeg or the export extension), since the name excludes the extension.
KRITAUI_EXPORT QString normalizedBaseName(const QString &name, const QString &extension);
// Rejects path separators, characters Windows does not allow in file names,
// "." and "..", and a trailing dot or space.
KRITAUI_EXPORT bool isValidBaseName(const QString &name);
// An empty output folder selects the source directory, and an empty base name
// the source's completeBaseName(). Unnamed documents are skipped before path
// generation; exportDocumentSync keeps document identity.
KRITAUI_EXPORT QString outputPath(const QString &source,
                                  const QString &extension,
                                  const QString &outputFolder,
                                  const QString &baseName = QString());
KRITAUI_EXPORT Result run(const QList<QPointer<KisDocument>> &documents,
                          const QString &extension,
                          const QString &outputFolder,
                          const Settings &settings,
                          const WaitForImage &wait,
                          const QString &baseName = QString());

enum class RegionResult {
    Exported,
    Unnamed,
    NoSelection,
    Failed
};
// Exports the image projection inside the document's global selection, masked
// by it and cropped to its bounds, like File > Export Region. Paths and
// options follow run(); the source document is left unchanged.
KRITAUI_EXPORT RegionResult exportRegion(KisDocument *document,
                                         const QString &extension,
                                         const QString &outputFolder,
                                         const Settings &settings,
                                         const WaitForImage &wait,
                                         const QString &baseName = QString());
// Builds the temporary document exportRegion() writes: the projection inside
// the global selection, masked and cropped. The caller has waited for the
// image. Returns null when there is no selection or it is empty.
KRITAUI_EXPORT KisDocument *createRegionDocument(KisDocument *document, bool batchMode);
// The same, with the pixels of projectionImage, a copy of the document's image
// of the same size (a layer comp applied to it may hide the selection mask).
KRITAUI_EXPORT KisDocument *createRegionDocument(KisDocument *document, bool batchMode, KisImageSP projectionImage);
} // namespace KisLazyToolsFastExport

#endif
