/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASSETLIBRARYDOCK_H
#define ASSETLIBRARYDOCK_H

#include <KoDockFactoryBase.h>
#include <kis_mainwindow_observer.h>

#include <assetlibrary/KisAssetLibraryBrowser.h>
#include <assetlibrary/KisAssetLibrarySettings.h>
#include <QDockWidget>
#include <QPixmap>
#include <QPointer>
#include <QStringList>
#include <QVector>

class KisViewManager;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QPushButton;
class QScrollArea;
class QSplitter;
class QTimer;
class QVBoxLayout;
class QWidget;

using AssetPathEntry = KisAssetLibraryPathEntry;
using AssetLibrarySettings = KisAssetLibrarySettings;

class AssetLibraryDock : public QDockWidget, public KisMainwindowObserver
{
    Q_OBJECT
public:
    explicit AssetLibraryDock(QWidget *parent = nullptr);
    ~AssetLibraryDock() override;

    QString observerName() override;
    void setViewManager(KisViewManager *viewManager) override;
    void setCanvas(KoCanvasBase *canvas) override;
    void unsetCanvas() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    using Section = KisAssetLibrarySection;

    void buildUi();
    void applySettings();
    void loadFolders();
    void folderChanged(QListWidgetItem *current);
    void refreshAssets();
    void clearAssets(const QString &message = {});
    void populateSections();
    int thumbnailColumns() const;
    QPixmap thumbnailForPath(const QString &path);
    void openAsset(const QString &path);
    void insertAsLayer(const QString &path);
    void insertAsFileLayer(const QString &path);
    void duplicateAsset(const QString &path);
    void renameAsset(const QString &path);
    void deleteAsset(const QString &path);
    QString requestAssetPath(const QString &path, const QString &title, bool allowCurrent = false);
    void showSettings();
    void setAssetPanelHidden(bool hidden, bool save);
    void saveRuntimeSettings();
    void reloadSharedSettings();

    AssetLibrarySettings m_settings;
    QPointer<KisViewManager> m_viewManager;
    QString m_currentPath;
    bool m_includeSubfolders{false};
    QString m_extensionText{QStringLiteral("kra,jpg,jpeg,png,svg")};
    QVector<Section> m_sections;
    bool m_restoringLayout{false};
    QWidget *m_root{nullptr};
    QSplitter *m_splitter{nullptr};
    QWidget *m_folderPanel{nullptr};
    QListWidget *m_folderList{nullptr};
    QPushButton *m_hideButton{nullptr};
    QWidget *m_assetPanel{nullptr};
    QLabel *m_statusLabel{nullptr};
    QScrollArea *m_scroll{nullptr};
    QWidget *m_assetHost{nullptr};
    QVBoxLayout *m_assetLayout{nullptr};
    QTimer *m_saveTimer{nullptr};
    QTimer *m_layoutTimer{nullptr};
    KisAssetLibraryThumbnailCache m_thumbnailCache;
    int m_currentColumns{0};
};

class AssetLibraryDockFactory : public KoDockFactoryBase
{
public:
    QString id() const override;
    QDockWidget *createDockWidget() override;
    DockPosition defaultDockPosition() const override;
};

#endif
