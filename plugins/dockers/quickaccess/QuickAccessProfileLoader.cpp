/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessProfileLoader.h"
#include "QuickAccessItemPresentation.h"
#include "QuickAccessLayoutEngine.h"
#include "QuickAccessLegacyImporter.h"
#include "QuickAccessProfileRepository.h"
#include "QuickAccessSettingsStore.h"
#include <QDir>
#include <QFileInfo>
#include <klocalizedstring.h>
#include <utility>
using namespace QuickAccessItemPresentation;
namespace
{
bool repairBrushSpans(QuickAccess::Document *document)
{
    bool changed = false;
    for (QuickAccess::Tab &tab : document->tabs) {
        for (QuickAccess::Grid &grid : tab.grids) {
            const QuickAccess::LayoutEngine engine(grid.columns);
            if (engine.validate(grid.items).isValid())
                continue;
            QStringList strokeBrushes;
            for (const QuickAccess::Item &item : std::as_const(grid.items)) {
                if (showsStrokePreview(item))
                    strokeBrushes << item.id;
            }
            for (const QString &id : std::as_const(strokeBrushes)) {
                const QuickAccess::LayoutResult result = engine.resizeItem(grid.items, id, 1, qMin(2, grid.columns));
                if (result.isValid()) {
                    grid.items = result.items;
                    changed = true;
                    break;
                }
            }
        }
    }
    return changed;
}
} // namespace

void QuickAccessProfileLoader::load(QuickAccess::Document *document,
                                    const QString &profilePath,
                                    const QString &legacyDirectory,
                                    QuickAccessSettingsStore &settings,
                                    const WarningHandler &warning)
{
    QuickAccess::ProfileRepository repository(profilePath);
    QString error;
    if (repository.exists()) {
        if (repository.load(document, &error)) {
            // Icon items saved wider than one cell are shrunk here too.
            const bool repairedBrushes = repairBrushSpans(document);
            if (normalizeIconItemSpans(document) || repairedBrushes)
                repository.save(*document, &error);
            if (settings.migrationVersion() < 2) {
                QuickAccess::LegacyImportResult imported;
                QString migrationError;
                if (QuickAccess::LegacyImporter::importDirectory(legacyDirectory, &imported, &migrationError))
                    settings.importLegacy(imported.settings);
            }
            return;
        }
        warning(i18nc("@title:window", "Quick Access Profile"),
                i18n("The Quick Access profile could not be loaded:\n%1", error));
        *document = QuickAccess::Document::createDefault();
        return;
    }

    const QString legacyPath = legacyDirectory;
    if (QFileInfo::exists(QDir(legacyPath).filePath(QStringLiteral("quick_access_palette.json")))) {
        QuickAccess::LegacyImportResult imported;
        if (QuickAccess::LegacyImporter::importDirectory(legacyPath, &imported, &error)) {
            *document = imported.document;
            if (!repository.save(*document, &error)) {
                warning(i18nc("@title:window", "Quick Access Migration"),
                        i18n("The imported Quick Access profile could not be saved:\n%1", error));
            } else {
                settings.importLegacy(imported.settings);
            }
            return;
        }
        warning(i18nc("@title:window", "Quick Access Migration"),
                i18n("The legacy Quick Access configuration could not be imported:\n%1", error));
    }

    *document = QuickAccess::Document::createDefault();
    if (!repository.save(*document, &error)) {
        warning(i18nc("@title:window", "Quick Access Profile"),
                i18n("The default Quick Access profile could not be saved:\n%1", error));
    }
}
