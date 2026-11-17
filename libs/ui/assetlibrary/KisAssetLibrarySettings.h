/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_ASSET_LIBRARY_SETTINGS_H
#define KIS_ASSET_LIBRARY_SETTINGS_H

#include <QJsonObject>
#include <QObject>
#include <QVector>
#include <kritaui_export.h>

struct KisAssetLibraryPathEntry {
    QString alias;
    QString path;
    bool includeSubfolders{false};
    QString extensions{QStringLiteral("kra,jpg,jpeg,png,svg")};

    bool operator==(const KisAssetLibraryPathEntry &other) const;
    bool operator!=(const KisAssetLibraryPathEntry &other) const
    {
        return !(*this == other);
    }
};

/**
 * Announces every successful settings save within this process, so that the
 * docker in each window and the welcome page can adopt folders and display
 * settings changed by another view.
 */
class KRITAUI_EXPORT KisAssetLibrarySettingsNotifier : public QObject
{
    Q_OBJECT
public:
    static KisAssetLibrarySettingsNotifier *instance();

Q_SIGNALS:
    void sigSaved();
};

class KRITAUI_EXPORT KisAssetLibrarySettings
{
public:
    // Preserve the two existing consumers' validation and save contracts.
    enum class Usage {
        Docker,
        Welcome
    };
    explicit KisAssetLibrarySettings(Usage usage = Usage::Docker);

    QVector<KisAssetLibraryPathEntry> paths;
    int windowWidth{720};
    int windowHeight{520};
    QList<int> splitterSizes{150, 570};
    bool rightPanelHidden{false};
    int expandedWindowWidth{720};
    int collapsedWindowWidth{170};
    bool autoColumns{true};
    int columns{3};
    int thumbnailSize{140};
    int uiFontSize{10};
    int headerFontSize{10};
    int assetNameFontSize{10};

    static QString filePath();
    void load();
    void load(const QString &path, const QByteArray &legacySettings);
    // Docker: writes the complete known schema. Welcome: updates the shared
    // path/display fields of the current file, keeping every other field.
    bool save() const;
    bool save(const QString &path) const;
    // Updates only the docker layout fields of the current file, so that a
    // docker resize never writes back stale folders or display settings.
    bool saveLayout() const;
    bool saveLayout(const QString &path) const;
    void fromJson(const QJsonObject &object);
    QJsonObject toJson() const;

    // Folders and display settings, which both views edit.
    bool hasSameSharedValues(const KisAssetLibrarySettings &other) const;
    void copySharedValues(const KisAssetLibrarySettings &other);

private:
    void writeSharedValues(QJsonObject &object) const;
    void writeLayoutValues(QJsonObject &object) const;
    bool write(const QString &path, const QJsonObject &object) const;

    Usage m_usage;
    QJsonObject m_raw;
};
#endif
