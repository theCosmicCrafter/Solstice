/*
 * SPDX-FileCopyrightText: 2018 Boudewijn Rempt <boud@valdyas.org>
 * SPDX-FileCopyrightText: 2019 Agata Cacko <cacko.azh@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "KisAbrStorage.h"
#include "KisResourceStorage.h"

#include <QFileInfo>
#include <KisStaticInitializer.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>

KIS_DECLARE_STATIC_INITIALIZER {
    KisStoragePluginRegistry::instance()->addStoragePluginFactory(KisResourceStorage::StorageType::AdobeBrushLibrary, new KisStoragePluginFactory<KisAbrStorage>());
}

class AbrTagIterator : public KisResourceStorage::TagIterator
{
public:
    AbrTagIterator(KisAbrBrushCollectionSP brushCollection, const QString &location, const QString &resourceType)
        : m_brushCollection(brushCollection)
        , m_location(location)
        , m_resourceType(resourceType)
    {}

    bool hasNext() const override {
        if (m_resourceType != ResourceType::Brushes && m_resourceType != ResourceType::Patterns
            && m_resourceType != ResourceType::PaintOpPresets)
            return false;
        if (!m_taggingDone) {
            return true;
        }
        return m_folder + 1 < folderTags().size();
    }

    void next() override
    {
        if (!m_taggingDone) {
            m_taggingDone = true;
        } else {
            m_folder++;
        }
    }

    KisTagSP tag() const override
    {
        const QString fileName = QFileInfo(m_location).fileName();
        KisTagSP abrTag(new KisTag());
        abrTag->setResourceType(m_resourceType);
        abrTag->setValid(true);
        if (m_folder >= 0) {
            // Solstice: a folder of Photoshop's Brushes panel, with the
            // presets in it and in its subfolders
            // (docs/agent/abr-import.md)
            const auto folder = folderTags().at(m_folder);
            const QString name = QFileInfo(m_location).completeBaseName() + QStringLiteral(" / ")
                + folder.first.join(QStringLiteral(" / "));
            abrTag->setUrl(fileName + QLatin1Char('/') + folder.first.join(QLatin1Char('/')));
            abrTag->setName(name);
            abrTag->setComment(name);
            abrTag->setFilename(fileName);
            abrTag->setDefaultResources(folder.second);
            return abrTag;
        }
        abrTag->setUrl(fileName);
        abrTag->setName(fileName);
        abrTag->setComment(fileName);
        abrTag->setFilename(fileName);
        QStringList brushes;
        if (m_resourceType == ResourceType::Patterns) {
            // Solstice: the file's patterns (docs/agent/abr-import.md)
            brushes = m_brushCollection->patternsMap()->keys();
        } else if (m_resourceType == ResourceType::PaintOpPresets) {
            brushes = m_brushCollection->presetsMap()->keys();
        } else {
            Q_FOREACH (const KisAbrBrushSP brush, m_brushCollection->brushes()) {
                brushes << brush->filename();
            }
        }
        abrTag->setDefaultResources(brushes);

        return abrTag;
    }

private:
    /// the folders of the presets, each with the presets in it or below
    QVector<QPair<QStringList, QStringList>> folderTags() const
    {
        if (m_resourceType != ResourceType::PaintOpPresets) {
            return {};
        }
        if (!m_folderTagsDone) {
            QMap<QStringList, QStringList> folders;
            const QMap<QString, QStringList> presetFolders = m_brushCollection->presetFolders();
            for (auto it = presetFolders.constBegin(); it != presetFolders.constEnd(); ++it) {
                for (int depth = 1; depth <= it.value().size(); depth++) {
                    folders[it.value().mid(0, depth)] << it.key();
                }
            }
            for (auto it = folders.constBegin(); it != folders.constEnd(); ++it) {
                m_folderTags << qMakePair(it.key(), it.value());
            }
            m_folderTagsDone = true;
        }
        return m_folderTags;
    }

    bool m_taggingDone {false};
    /// the folder tag, after the file's own tag
    int m_folder {-1};
    mutable bool m_folderTagsDone {false};
    mutable QVector<QPair<QStringList, QStringList>> m_folderTags;
    KisAbrBrushCollectionSP m_brushCollection;
    QString m_location;
    QString m_resourceType;
};

class AbrIterator : public KisResourceStorage::ResourceIterator
{
public:
    KisAbrBrushCollectionSP m_brushCollection;
    /// Solstice: brush tips, patterns or presets (docs/agent/abr-import.md)
    QVector<QPair<QString, KoResourceSP>> m_items;
    int m_nextItem{0};
    KoResourceSP m_currentResource;
    bool isLoaded;
    QString m_currentUrl;
    QString m_resourceType;


    AbrIterator(KisAbrBrushCollectionSP brushCollection, const QString& resourceType)
        : m_brushCollection(brushCollection)
        , isLoaded(false)
        , m_resourceType(resourceType)
    {
    }

    bool hasNext() const override
    {
        if (m_resourceType != ResourceType::Brushes && m_resourceType != ResourceType::Patterns
            && m_resourceType != ResourceType::PaintOpPresets) {
            return false;
        }

        if (!isLoaded) {
            AbrIterator *self = const_cast<AbrIterator *>(this);
            if (!m_brushCollection->isLoaded()) {
                bool success = m_brushCollection->load();
                Q_UNUSED(success); // brush collection will be empty
            }
            if (m_resourceType == ResourceType::Brushes) {
                const auto brushes = m_brushCollection->brushesMap();
                for (auto it = brushes->constBegin(); it != brushes->constEnd(); ++it) {
                    self->m_items.append({it.key(), it.value()});
                }
            } else if (m_resourceType == ResourceType::Patterns) {
                const auto patterns = m_brushCollection->patternsMap();
                for (auto it = patterns->constBegin(); it != patterns->constEnd(); ++it) {
                    self->m_items.append({it.key(), it.value()});
                }
            } else {
                const auto presets = m_brushCollection->presetsMap();
                for (auto it = presets->constBegin(); it != presets->constEnd(); ++it) {
                    self->m_items.append({it.key(), it.value()});
                }
            }
            self->isLoaded = true;
        }

        return m_nextItem < m_items.size();
    }

    void next() override
    {
        KIS_SAFE_ASSERT_RECOVER_RETURN(m_nextItem < m_items.size());
        m_currentUrl = m_items[m_nextItem].first;
        m_currentResource = m_items[m_nextItem].second;
        m_nextItem++;
    }

    QString url() const override { return m_currentUrl; }
    QString type() const override
    {
        return m_resourceType;
    }
    QDateTime lastModified() const override { return m_brushCollection->lastModified(); }

    KoResourceSP resourceImpl() const override
    {
        return m_currentResource;
    }
};

KisAbrStorage::KisAbrStorage(const QString &location)
    : KisStoragePlugin(location)
    , m_brushCollection(new KisAbrBrushCollection(location))
{
}

KisAbrStorage::~KisAbrStorage()
{

}

KisResourceStorage::ResourceItem KisAbrStorage::resourceItem(const QString &url)
{
    KisResourceStorage::ResourceItem item;
    item.url = url;
    // last "_" with index is the suffix added by abr_collection
    int indexOfUnderscore = url.lastIndexOf("_");
    QString filenameUrl = url;
    // filenameUrl contains the name of the collection (filename without .abr, brush name without index)
    filenameUrl.remove(indexOfUnderscore, url.length() - indexOfUnderscore);
    item.folder = filenameUrl;
    item.resourceType = ResourceType::Brushes;
    // Solstice: the file's patterns are <identifier>.pat
    if (url.endsWith(QStringLiteral(".pat"))) {
        item.folder = QFileInfo(m_brushCollection->filename()).completeBaseName();
        item.resourceType = ResourceType::Patterns;
    } else if (url.endsWith(QStringLiteral(".kpp"))) {
        item.folder = QFileInfo(m_brushCollection->filename()).completeBaseName();
        item.resourceType = ResourceType::PaintOpPresets;
    }
    item.lastModified = QFileInfo(m_brushCollection->filename()).lastModified();
    return item;
}


KoResourceSP KisAbrStorage::resource(const QString &url)
{
    if (!m_brushCollection->isLoaded()) {
        m_brushCollection->load();
    }
    const QString name = QFileInfo(url).fileName();
    if (name.endsWith(QStringLiteral(".pat"))) {
        return m_brushCollection->patternByName(name);
    }
    if (name.endsWith(QStringLiteral(".kpp"))) {
        // a copy: editing the preset must not change the file's version,
        // to which it is reloaded
        const KisPaintOpPresetSP preset = m_brushCollection->presetByName(name);
        return preset ? preset->clone() : KoResourceSP();
    }
    return m_brushCollection->brushByName(name);
}

bool KisAbrStorage::loadVersionedResource(KoResourceSP resource)
{
    // Solstice: a preset made from the file's brush presets goes back to
    // how the file made it (reloading a preset);
    // docs/agent/abr-import.md
    KisPaintOpPresetSP preset = resource.dynamicCast<KisPaintOpPreset>();
    if (!preset) {
        return false;
    }
    if (!m_brushCollection->isLoaded()) {
        m_brushCollection->load();
    }
    const KisPaintOpPresetSP original = m_brushCollection->presetByName(QFileInfo(preset->filename()).fileName());
    if (!original) {
        return false;
    }
    preset->setSettings(original->settings()->clone());
    preset->setName(original->name());
    preset->setImage(original->image());
    return true;
}

bool KisAbrStorage::supportsVersioning() const
{
    return false;
}

QSharedPointer<KisResourceStorage::ResourceIterator> KisAbrStorage::resources(const QString &resourceType)
{
    return QSharedPointer<KisResourceStorage::ResourceIterator>(new AbrIterator(m_brushCollection, resourceType));
}

QSharedPointer<KisResourceStorage::TagIterator> KisAbrStorage::tags(const QString &resourceType)
{
    // Solstice: the tags come from the file's contents (the presets and
    // their folders), so it has to be read
    if (!m_brushCollection->isLoaded()) {
        m_brushCollection->load();
    }
    return QSharedPointer<KisResourceStorage::TagIterator>(new AbrTagIterator(m_brushCollection, location(), resourceType));
}

QImage KisAbrStorage::thumbnail() const
{
    return m_brushCollection->image();
}
