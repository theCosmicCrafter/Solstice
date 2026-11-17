/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_LAZY_TOOLS_FAST_EXPORT_PRESETS_H
#define KIS_LAZY_TOOLS_FAST_EXPORT_PRESETS_H

#include "KisLazyToolsFastExport.h"

#include <QByteArray>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <kritaui_export.h>

class KisDocument;

// Export settings saved with a document. Each one is run on its own: format,
// options, folder, name and area. The .kra keeps them; other formats cannot.
namespace KisLazyToolsFastExportPresets
{
enum class Destination {
    SameFolder,
    Folder
};

enum class NameRule {
    DocumentName,
    Fixed
};

enum class Area {
    WholeImage,
    Selection
};

struct KRITAUI_EXPORT Preset {
    QString id;
    QString name;
    bool enabled{true};
    QString format{QStringLiteral("png")}; // "png" or "jpg"
    int pngCompression{6};
    bool pngAlpha{true};
    int jpegQuality{90};
    // The format's remaining options (forceSRGB, progressive...) as
    // KisPropertiesConfiguration XML. Empty uses those File > Export remembers.
    QString formatOptions;
    Destination destination{Destination::SameFolder};
    // "/" separators. Relative to the document's folder when folderRelative.
    QString folder;
    bool folderRelative{false};
    // DocumentName adds "_<layer comp name>" when the export uses a layer comp.
    NameRule nameRule{NameRule::DocumentName};
    QString fixedName;
    Area area{Area::WholeImage};
    // The KisLayerComposition::uuid() the export applies to a copy of the
    // image first, without braces; empty exports the layers as they are.
    QString layerComp;
    // The comp's name when the export was last edited. Finds the comp when
    // its id is gone (a file saved by Krita drops it).
    QString layerCompName;

    bool operator==(const Preset &other) const;
    bool operator!=(const Preset &other) const
    {
        return !(*this == other);
    }
};

// A preset with a new id and the given name; other values are the defaults.
KRITAUI_EXPORT Preset newPreset(const QString &name);

// An empty list gives empty data, so a document without presets saves nothing.
KRITAUI_EXPORT QByteArray toXml(const QList<Preset> &presets);
// Unknown elements and attributes are ignored, presets with an unknown format
// are dropped and missing ids are generated. Unreadable data gives an empty list.
KRITAUI_EXPORT QList<Preset> fromXml(const QByteArray &data);

// Whether saving the document keeps its presets: unsaved documents (saved as
// .kra by default) and .kra/.krz files.
KRITAUI_EXPORT bool canStore(const KisDocument *document);
KRITAUI_EXPORT QList<Preset> presets(const KisDocument *document);
// Stores the presets in the document. When it can keep them, the document is
// marked modified, without an undo step.
KRITAUI_EXPORT void setPresets(KisDocument *document, const QList<Preset> &presets);

// The export folder for a document at documentPath, or empty when there is
// none (no folder set, or a relative folder for an unsaved document).
KRITAUI_EXPORT QString resolvedFolder(const Preset &preset, const QString &documentPath);
// The folder as the preset stores it: relative to the document's folder when
// relative is set and the document has a path, absolute otherwise.
KRITAUI_EXPORT QString storedFolder(const QString &absoluteFolder, bool relative, const QString &documentPath);
// The full output path, or empty when the folder or name is invalid. A
// document-named export of a layer comp appends "_" and layerCompName.
KRITAUI_EXPORT QString outputPath(const Preset &preset,
                                  const QString &documentPath,
                                  const QString &layerCompName = QString());

struct LayerComp {
    QString id;
    QString name;
};
// The document's layer comps in the Layer Comps docker's order.
KRITAUI_EXPORT QList<LayerComp> layerComps(KisDocument *document);
// The comp the preset uses: the one with its id, otherwise the only comp
// with its stored name. Null when it uses none or none matches.
KRITAUI_EXPORT KisLayerCompositionSP findLayerComp(KisImageSP image, const Preset &preset);
// The preset's format options, with its compression and alpha, or quality, on top.
KRITAUI_EXPORT KisPropertiesConfigurationSP exportOptions(const Preset &preset);
// One line for the list: format and destination.
KRITAUI_EXPORT QString summary(const Preset &preset, const QString &documentPath);

enum class Which {
    All,
    Checked
};

struct Result {
    int exported{0};
    // "Document: reason" or "Document / Preset: reason".
    QStringList skipped;
    // Output paths that two presets of this run share. Nothing is written then.
    QStringList conflicts;
};

// Runs the documents' presets without the format options dialog: whole-image
// presets export a snapshot of the document in batch mode, selection presets
// a region document in batch mode. The documents' paths, modified flags and
// batch modes are unchanged.
KRITAUI_EXPORT Result run(const QList<QPointer<KisDocument>> &documents,
                          Which which,
                          const KisLazyToolsFastExport::WaitForImage &wait);
} // namespace KisLazyToolsFastExportPresets

#endif
