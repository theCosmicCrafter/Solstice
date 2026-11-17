/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISABRPRESETCONVERTER_H
#define KISABRPRESETCONVERTER_H

#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <kis_abr_brush.h>
#include <kis_types.h>
#include <resources/KoPattern.h>

#include "kritabrush_export.h"

class QDomElement;

/**
 * Makes Pixel Brush presets from the brush presets of a Photoshop brush
 * file: the `brushPreset` descriptors of its `desc` section (read into XML
 * by KisAslReader). The tips are the file's own ABR tips, the textures its
 * patterns. Recognized unsupported or approximate settings are counted in
 * `unsupported`; unknown keys are not exhaustively reported. See
 * docs/agent/abr-import.md.
 */
class BRUSH_EXPORT KisAbrPresetConverter
{
public:
    struct Sources {
        /// the file's tips, by the identifier presets use (`sampledData`)
        QHash<QString, KisAbrBrushSP> tipsBySample;
        /// the file's patterns, by their identifier (`Idnt`)
        QHash<QString, KoPatternSP> patternsById;
        /// the base of the presets' file names
        QString baseName;
    };

    struct Result {
        /// by file name (`<baseName>_preset_<n>.kpp`)
        QMap<QString, KisPaintOpPresetSP> presets;
        /// Photoshop settings that were left out, with how many presets
        /// used them
        QMap<QString, int> unsupported;
        int skipped = 0;
    };

    /// @p root is the descriptor document's root element
    static Result convert(const QDomElement &root, const Sources &sources);

    /// The folders of the presets, from the `phry` section's `hierarchy`
    /// list (@p root is its descriptor document's root element): one entry
    /// per preset, in the order of the `desc` section, with the names of the
    /// folders it is in (empty at the top level). See
    /// docs/agent/abr-import.md.
    static QVector<QStringList> presetFolders(const QDomElement &root);
};

#endif // KISABRPRESETCONVERTER_H
