/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AssetLibraryDock.h"

#include <KisMainWindow.h>
#include <KisViewManager.h>
#include <KoResourcePaths.h>
#include <KoStore.h>
#include <kconfiggroup.h>
#include <kis_file_layer.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_import_catcher.h>
#include <kis_node_commands_adapter.h>
#include <kis_node_manager.h>
#include <klocalizedstring.h>
#include <ksharedconfig.h>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace
{
constexpr auto defaultExtensions = "kra,jpg,jpeg,png,svg";

int boundedInt(const QJsonObject &object, const char *key, int fallback, int minimum, int maximum)
{
    const int value = object.value(QLatin1String(key)).toInt(fallback);
    return value >= minimum && value <= maximum ? value : fallback;
}

class AssetTile : public QFrame
{
public:
    AssetTile(const QString &path,
              const QPixmap &thumbnail,
              int thumbnailSize,
              int fontSize,
              std::function<void(const QString &)> open,
              std::function<void(const QString &)> insertLayer,
              std::function<void(const QString &)> insertFileLayer,
              std::function<void(const QString &)> duplicate,
              std::function<void(const QString &)> rename,
              std::function<void(const QString &)> remove,
              QWidget *parent)
        : QFrame(parent)
        , m_path(path)
        , m_open(std::move(open))
        , m_insertLayer(std::move(insertLayer))
        , m_insertFileLayer(std::move(insertFileLayer))
        , m_duplicate(std::move(duplicate))
        , m_rename(std::move(rename))
        , m_remove(std::move(remove))
    {
        setFrameShape(QFrame::StyledPanel);
        setCursor(Qt::PointingHandCursor);
        setToolTip(path);
        setFixedSize(thumbnailSize + 24, thumbnailSize + 54);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);
        auto *image = new QLabel(this);
        image->setAlignment(Qt::AlignCenter);
        image->setFixedSize(thumbnailSize, thumbnailSize);
        image->setPixmap(thumbnail);
        layout->addWidget(image);
        auto *name = new QLabel(QFileInfo(path).fileName(), this);
        name->setAlignment(Qt::AlignCenter);
        name->setWordWrap(true);
        QFont f = name->font();
        f.setPointSize(fontSize);
        name->setFont(f);
        layout->addWidget(name);
    }

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_open)
            m_open(m_path);
        QFrame::mouseDoubleClickEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        QMenu menu(this);
        QAction *open = menu.addAction(i18n("Open"));
        QAction *insertLayer = menu.addAction(i18n("Insert as New Layer"));
        QAction *insertFileLayer = menu.addAction(i18n("Insert as New File Layer"));
        QAction *duplicate = menu.addAction(i18n("Duplicate"));
        QAction *rename = menu.addAction(i18n("Rename"));
        QAction *remove = menu.addAction(i18n("Delete"));
        QAction *selected = menu.exec(event->globalPos());
        if (selected == open && m_open)
            m_open(m_path);
        else if (selected == insertLayer && m_insertLayer)
            m_insertLayer(m_path);
        else if (selected == insertFileLayer && m_insertFileLayer)
            m_insertFileLayer(m_path);
        else if (selected == duplicate && m_duplicate)
            m_duplicate(m_path);
        else if (selected == rename && m_rename)
            m_rename(m_path);
        else if (selected == remove && m_remove)
            m_remove(m_path);
    }

private:
    QString m_path;
    std::function<void(const QString &)> m_open;
    std::function<void(const QString &)> m_insertLayer;
    std::function<void(const QString &)> m_insertFileLayer;
    std::function<void(const QString &)> m_duplicate;
    std::function<void(const QString &)> m_rename;
    std::function<void(const QString &)> m_remove;
};

void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        else if (QLayout *child = item->layout())
            clearLayout(child);
        delete item;
    }
}

QTableWidgetItem *checkedItem(bool checked)
{
    auto *item = new QTableWidgetItem;
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}
} // namespace

QString AssetLibrarySettings::filePath()
{
    return QDir(KoResourcePaths::saveLocation("data", "krita_asset_library/", true))
        .filePath(QStringLiteral("config.json"));
}

void AssetLibrarySettings::load()
{
    QByteArray bytes;
    QFile file(filePath());
    if (file.open(QIODevice::ReadOnly)) {
        bytes = file.readAll();
    } else {
        const QString legacy = KSharedConfig::openConfig()
                                   ->group(QStringLiteral("asset_library"))
                                   .readEntry(QStringLiteral("settings_json"), QString());
        bytes = legacy.toUtf8();
    }
    const QJsonObject object = QJsonDocument::fromJson(bytes).object();
    paths.clear();
    for (const QJsonValue &value : object.value(QStringLiteral("paths")).toArray()) {
        const QJsonObject item = value.toObject();
        AssetPathEntry entry;
        entry.alias = item.value(QStringLiteral("alias")).toString();
        entry.path = item.value(QStringLiteral("path")).toString();
        entry.includeSubfolders =
            item.value(QStringLiteral("include_subfolders")).toBool(item.value(QStringLiteral("nested")).toBool(false));
        entry.extensions = item.value(QStringLiteral("extensions")).toString(QString::fromLatin1(defaultExtensions));
        if (!entry.path.isEmpty())
            paths.append(entry);
    }
    windowWidth = std::max(80, object.value(QStringLiteral("window_width")).toInt(windowWidth));
    windowHeight = std::max(80, object.value(QStringLiteral("window_height")).toInt(windowHeight));
    const QJsonArray sizes = object.value(QStringLiteral("splitter_sizes")).toArray();
    if (sizes.size() == 2)
        splitterSizes = {std::max(80, sizes.at(0).toInt(150)), std::max(0, sizes.at(1).toInt(570))};
    rightPanelHidden = object.value(QStringLiteral("right_panel_hidden")).toBool(rightPanelHidden);
    expandedWindowWidth = std::max(80, object.value(QStringLiteral("expanded_window_width")).toInt(windowWidth));
    collapsedWindowWidth =
        std::max(80, object.value(QStringLiteral("collapsed_window_width")).toInt(collapsedWindowWidth));
    autoColumns = object.value(QStringLiteral("auto_columns")).toBool(autoColumns);
    columns = boundedInt(object, "columns", columns, 1, 12);
    thumbnailSize = std::clamp(object.value(QStringLiteral("thumbnail_size")).toInt(thumbnailSize), 48, 512);
    const int legacyFont = object.value(QStringLiteral("font_size")).toInt(10);
    uiFontSize = boundedInt(object, "ui_font_size", legacyFont, 7, 32);
    headerFontSize = boundedInt(object, "header_font_size", uiFontSize, 7, 32);
    assetNameFontSize = boundedInt(object, "asset_name_font_size", legacyFont, 7, 32);
}

bool AssetLibrarySettings::save() const
{
    QJsonArray pathArray;
    for (const AssetPathEntry &entry : paths) {
        pathArray.append(QJsonObject{{QStringLiteral("alias"), entry.alias},
                                     {QStringLiteral("path"), entry.path},
                                     {QStringLiteral("include_subfolders"), entry.includeSubfolders},
                                     {QStringLiteral("extensions"), entry.extensions}});
    }
    QJsonArray sizes;
    for (int value : splitterSizes)
        sizes.append(value);
    const QJsonObject object{{QStringLiteral("paths"), pathArray},
                             {QStringLiteral("window_width"), windowWidth},
                             {QStringLiteral("window_height"), windowHeight},
                             {QStringLiteral("splitter_sizes"), sizes},
                             {QStringLiteral("right_panel_hidden"), rightPanelHidden},
                             {QStringLiteral("expanded_window_width"), expandedWindowWidth},
                             {QStringLiteral("collapsed_window_width"), collapsedWindowWidth},
                             {QStringLiteral("auto_columns"), autoColumns},
                             {QStringLiteral("columns"), columns},
                             {QStringLiteral("thumbnail_size"), thumbnailSize},
                             {QStringLiteral("ui_font_size"), uiFontSize},
                             {QStringLiteral("header_font_size"), headerFontSize},
                             {QStringLiteral("asset_name_font_size"), assetNameFontSize}};
    QFile file(filePath());
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(QJsonDocument(object).toJson()) >= 0;
}

AssetLibraryDock::AssetLibraryDock(QWidget *parent)
    : QDockWidget(parent)
    , m_thumbnailCache(64 * 1024)
{
    setWindowTitle(i18n("Asset Library"));
    m_settings.load();
    buildUi();
    applySettings();
    loadFolders();
}

AssetLibraryDock::~AssetLibraryDock() = default;

QString AssetLibraryDock::observerName()
{
    return QStringLiteral("AssetLibraryDocker");
}

void AssetLibraryDock::setViewManager(KisViewManager *viewManager)
{
    m_viewManager = viewManager;
}

void AssetLibraryDock::setCanvas(KoCanvasBase *)
{
}

void AssetLibraryDock::unsetCanvas()
{
}

void AssetLibraryDock::buildUi()
{
    m_root = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(m_root);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);
    m_splitter = new QSplitter(Qt::Horizontal, m_root);
    m_folderPanel = new QWidget(m_splitter);
    auto *folderLayout = new QVBoxLayout(m_folderPanel);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    folderLayout->setSpacing(6);
    m_folderList = new QListWidget(m_folderPanel);
    m_folderList->setMinimumWidth(120);
    folderLayout->addWidget(m_folderList, 1);
    auto *refresh = new QPushButton(i18n("Refresh"), m_folderPanel);
    auto *settings = new QPushButton(i18n("Settings"), m_folderPanel);
    m_hideButton = new QPushButton(i18n("Hide"), m_folderPanel);
    folderLayout->addWidget(refresh);
    folderLayout->addWidget(settings);
    folderLayout->addWidget(m_hideButton);
    m_splitter->addWidget(m_folderPanel);

    m_assetPanel = new QWidget(m_splitter);
    auto *rightLayout = new QVBoxLayout(m_assetPanel);
    rightLayout->setContentsMargins(6, 0, 0, 0);
    m_statusLabel = new QLabel(m_assetPanel);
    rightLayout->addWidget(m_statusLabel);
    m_scroll = new QScrollArea(m_assetPanel);
    m_scroll->setWidgetResizable(true);
    m_scroll->viewport()->installEventFilter(this);
    m_assetHost = new QWidget(m_scroll);
    m_assetLayout = new QVBoxLayout(m_assetHost);
    m_assetLayout->setContentsMargins(4, 4, 4, 4);
    m_assetLayout->setSpacing(14);
    m_scroll->setWidget(m_assetHost);
    rightLayout->addWidget(m_scroll, 1);
    m_splitter->addWidget(m_assetPanel);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    rootLayout->addWidget(m_splitter, 1);
    setWidget(m_root);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(500);
    connect(m_saveTimer, &QTimer::timeout, this, &AssetLibraryDock::saveRuntimeSettings);
    m_layoutTimer = new QTimer(this);
    m_layoutTimer->setSingleShot(true);
    m_layoutTimer->setInterval(150);
    connect(m_layoutTimer, &QTimer::timeout, this, &AssetLibraryDock::populateSections);
    connect(m_folderList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current) {
        folderChanged(current);
    });
    connect(refresh, &QPushButton::clicked, this, &AssetLibraryDock::refreshAssets);
    connect(settings, &QPushButton::clicked, this, &AssetLibraryDock::showSettings);
    connect(m_hideButton, &QPushButton::clicked, this, [this]() {
        setAssetPanelHidden(!m_assetPanel->isHidden(), true);
    });
    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        if (!m_restoringLayout)
            m_saveTimer->start();
        if (m_settings.autoColumns && !m_sections.isEmpty() && !m_assetPanel->isHidden())
            m_layoutTimer->start();
    });
}

void AssetLibraryDock::applySettings()
{
    resize(m_settings.rightPanelHidden ? m_settings.collapsedWindowWidth : m_settings.windowWidth,
           m_settings.windowHeight);
    QFont f = font();
    f.setPointSize(m_settings.uiFontSize);
    setFont(f);
    m_restoringLayout = true;
    m_splitter->setSizes(m_settings.splitterSizes);
    m_restoringLayout = false;
    setAssetPanelHidden(m_settings.rightPanelHidden, false);
}

void AssetLibraryDock::loadFolders()
{
    m_folderList->clear();
    for (int index = 0; index < m_settings.paths.size(); ++index) {
        const AssetPathEntry &entry = m_settings.paths.at(index);
        const QString label =
            entry.alias.trimmed().isEmpty() ? QFileInfo(entry.path).fileName() : entry.alias.trimmed();
        auto *item = new QListWidgetItem(label.isEmpty() ? entry.path : label, m_folderList);
        item->setToolTip(entry.path);
        item->setData(Qt::UserRole, index);
    }
    if (m_folderList->count())
        m_folderList->setCurrentRow(0);
    else
        clearAssets(i18n("Add asset paths from Settings."));

    if (m_settings.autoColumns && !m_sections.isEmpty())
        m_layoutTimer->start();
}

void AssetLibraryDock::folderChanged(QListWidgetItem *current)
{
    if (!current) {
        m_currentPath.clear();
        clearAssets(i18n("No asset path is selected."));
        return;
    }
    const int index = current->data(Qt::UserRole).toInt();
    if (index < 0 || index >= m_settings.paths.size())
        return;
    const AssetPathEntry &entry = m_settings.paths.at(index);
    m_currentPath = entry.path;
    m_includeSubfolders = entry.includeSubfolders;
    m_extensionText = entry.extensions;
    refreshAssets();
}

QStringList AssetLibraryDock::extensions() const
{
    QStringList result;
    for (QString extension : m_extensionText.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        extension = extension.trimmed().toLower();
        if (extension.startsWith(QLatin1Char('.')))
            extension.remove(0, 1);
        if (!extension.isEmpty())
            result.append(extension);
    }
    if (result.isEmpty())
        result = {QStringLiteral("kra"), QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg")};
    return result;
}

QVector<AssetLibraryDock::Section> AssetLibraryDock::collectSections() const
{
    QVector<Section> sections;
    QMap<QString, QStringList> byFolder;
    const QStringList suffixes = extensions();
    const auto matches = [&suffixes](const QString &path) {
        return suffixes.contains(QFileInfo(path).suffix().toLower());
    };
    QDirIterator iterator(m_currentPath,
                          QDir::Files | QDir::NoDotAndDotDot,
                          m_includeSubfolders ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (matches(path))
            byFolder[QFileInfo(path).absolutePath()].append(path);
    }
    for (auto it = byFolder.begin(); it != byFolder.end(); ++it) {
        QStringList files = it.value();
        std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) {
            return QString::localeAwareCompare(QFileInfo(a).fileName(), QFileInfo(b).fileName()) < 0;
        });
        sections.append({QFileInfo(it.key()).fileName().isEmpty() ? it.key() : QFileInfo(it.key()).fileName(), files});
    }
    return sections;
}

void AssetLibraryDock::refreshAssets()
{
    m_layoutTimer->stop();
    if (m_currentPath.isEmpty()) {
        clearAssets(i18n("No asset path is selected."));
        return;
    }
    if (!QDir(m_currentPath).exists()) {
        clearAssets(i18n("Folder not found: %1", m_currentPath));
        return;
    }
    m_sections = collectSections();
    populateSections();
}

void AssetLibraryDock::clearAssets(const QString &message)
{
    m_sections.clear();
    clearLayout(m_assetLayout);
    m_statusLabel->setText(message);
}

int AssetLibraryDock::thumbnailColumns() const
{
    if (!m_settings.autoColumns)
        return std::max(1, m_settings.columns);
    const int tileWidth = m_settings.thumbnailSize + 24;
    return std::max(1, (std::max(1, m_scroll->viewport()->width() - 8) + 10) / (tileWidth + 10));
}

QPixmap AssetLibraryDock::thumbnailForPath(const QString &path)
{
    const QFileInfo info(path);
    const qreal pixelRatio = devicePixelRatioF();
    const int pixelSize = std::max(1, qRound(m_settings.thumbnailSize * pixelRatio));
    const QString cacheKey = QStringLiteral("%1|%2|%3|%4|%5")
                                 .arg(info.absoluteFilePath())
                                 .arg(info.size())
                                 .arg(info.lastModified().toMSecsSinceEpoch())
                                 .arg(pixelSize)
                                 .arg(pixelRatio);
    if (const QPixmap *cached = m_thumbnailCache.object(cacheKey))
        return *cached;

    QImage source;
    if (info.suffix().compare(QStringLiteral("kra"), Qt::CaseInsensitive) == 0) {
        QScopedPointer<KoStore> store(KoStore::createStore(path, KoStore::Read));
        if (store) {
            const QStringList candidates{QStringLiteral("preview.png"),
                                         QStringLiteral("Thumbnails/thumbnail.png"),
                                         QStringLiteral("mergedimage.png")};
            for (const QString &candidate : candidates) {
                if (!store->hasFile(candidate) || !store->open(candidate))
                    continue;
                const QByteArray bytes = store->read(store->size());
                store->close();
                if (source.loadFromData(bytes))
                    break;
            }
        }
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize originalSize = reader.size();
        if (originalSize.isValid())
            reader.setScaledSize(originalSize.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio));
        source = reader.read();
    }

    QPixmap thumbnail;
    if (!source.isNull()) {
        thumbnail =
            QPixmap::fromImage(source.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        thumbnail.setDevicePixelRatio(pixelRatio);
    } else {
        thumbnail = QPixmap(pixelSize, pixelSize);
        thumbnail.setDevicePixelRatio(pixelRatio);
        thumbnail.fill(QColor(245, 245, 245));
        QPainter painter(&thumbnail);
        painter.setPen(QColor(75, 75, 75));
        painter.drawRect(thumbnail.rect().adjusted(0, 0, -1, -1));
        QFont font;
        font.setPointSize(std::max(10, m_settings.thumbnailSize / 7));
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(thumbnail.rect(), Qt::AlignCenter, info.suffix().toUpper());
    }

    const int costKiB = std::max(1, thumbnail.width() * thumbnail.height() * 4 / 1024);
    m_thumbnailCache.insert(cacheKey, new QPixmap(thumbnail), costKiB);
    return thumbnail;
}

void AssetLibraryDock::populateSections()
{
    clearLayout(m_assetLayout);
    m_scroll->verticalScrollBar()->setValue(0);
    int count = 0;
    for (const Section &section : std::as_const(m_sections))
        count += section.files.size();
    m_statusLabel->setText(i18np("1 asset", "%1 assets", count));
    const int columns = thumbnailColumns();
    m_currentColumns = columns;
    for (const Section &section : std::as_const(m_sections)) {
        if (section.files.isEmpty())
            continue;
        auto *title = new QLabel(section.title, m_assetHost);
        title->setToolTip(section.title);
        title->setWordWrap(true);
        QFont f = font();
        f.setBold(true);
        f.setPointSize(m_settings.headerFontSize);
        title->setFont(f);
        m_assetLayout->addWidget(title);
        auto *gridHost = new QWidget(m_assetHost);
        auto *grid = new QGridLayout(gridHost);
        grid->setContentsMargins(0, 0, 0, 10);
        grid->setSpacing(10);
        for (int index = 0; index < section.files.size(); ++index) {
            const QString path = section.files.at(index);
            auto *tile = new AssetTile(
                path,
                thumbnailForPath(path),
                m_settings.thumbnailSize,
                m_settings.assetNameFontSize,
                [this](const QString &p) {
                    openAsset(p);
                },
                [this](const QString &p) {
                    insertAsLayer(p);
                },
                [this](const QString &p) {
                    insertAsFileLayer(p);
                },
                [this](const QString &p) {
                    duplicateAsset(p);
                },
                [this](const QString &p) {
                    renameAsset(p);
                },
                [this](const QString &p) {
                    deleteAsset(p);
                },
                gridHost);
            grid->addWidget(tile, index / columns, index % columns);
        }
        grid->setColumnStretch(columns, 1);
        m_assetLayout->addWidget(gridHost);
    }
    m_assetLayout->addStretch();
}

void AssetLibraryDock::openAsset(const QString &path)
{
    if (m_viewManager && m_viewManager->mainWindow())
        m_viewManager->mainWindow()->openDocument(path, KisMainWindow::None);
}

void AssetLibraryDock::insertAsLayer(const QString &path)
{
    if (!m_viewManager || !m_viewManager->image()) {
        QMessageBox::warning(this, i18n("Insert as New Layer"), i18n("No active document."));
        return;
    }
    new KisImportCatcher(path,
                         m_viewManager,
                         QFileInfo(path).suffix().compare(QStringLiteral("svg"), Qt::CaseInsensitive) == 0
                             ? QStringLiteral("KisShapeLayer")
                             : QStringLiteral("KisPaintLayer"));
}

void AssetLibraryDock::insertAsFileLayer(const QString &path)
{
    if (!m_viewManager || !m_viewManager->image()) {
        QMessageBox::warning(this, i18n("Insert as New File Layer"), i18n("No active document."));
        return;
    }
    const QFileInfo info(path);
    KisLayerSP layer = new KisFileLayer(m_viewManager->image(),
                                        info.absolutePath(),
                                        info.fileName(),
                                        KisFileLayer::None,
                                        QStringLiteral("Bicubic"),
                                        info.completeBaseName(),
                                        255);
    KisNodeSP active = m_viewManager->activeNode();
    KisNodeSP parent;
    if (active && active->parent())
        parent = active->parent();
    else
        parent = m_viewManager->image()->rootLayer();
    KisNodeCommandsAdapter adapter(m_viewManager);
    adapter.addNode(layer, parent, active);
    m_viewManager->nodeManager()->slotNonUiActivatedNode(layer);
}

QString AssetLibraryDock::requestAssetPath(const QString &path, const QString &title, bool allowCurrent)
{
    bool ok = false;
    QString name =
        QInputDialog::getText(this, title, i18n("New file name"), QLineEdit::Normal, QFileInfo(path).fileName(), &ok)
            .trimmed();
    if (!ok || name.isEmpty())
        return {};
    if (QFileInfo(name).fileName() != name) {
        QMessageBox::warning(this, title, i18n("File name cannot contain a path."));
        return {};
    }
    if (QFileInfo(name).suffix().isEmpty())
        name += QLatin1Char('.') + QFileInfo(path).suffix();
    const QString target = QDir(QFileInfo(path).absolutePath()).filePath(name);
    if (allowCurrent && QFileInfo(target) == QFileInfo(path))
        return target;
    if (QFileInfo::exists(target)) {
        QMessageBox::warning(this, title, i18n("A file with that name already exists."));
        return {};
    }
    return target;
}

void AssetLibraryDock::duplicateAsset(const QString &path)
{
    const QString target = requestAssetPath(path, i18n("Duplicate Asset"));
    if (target.isEmpty())
        return;
    if (!QFile::copy(path, target))
        QMessageBox::warning(this, i18n("Duplicate Asset"), i18n("Could not copy the file."));
    refreshAssets();
}

void AssetLibraryDock::renameAsset(const QString &path)
{
    const QString target = requestAssetPath(path, i18n("Rename Asset"), true);
    if (target.isEmpty() || target == path)
        return;
    if (!QFile::rename(path, target))
        QMessageBox::warning(this, i18n("Rename Asset"), i18n("Could not rename the file."));
    refreshAssets();
}

void AssetLibraryDock::deleteAsset(const QString &path)
{
    if (QMessageBox::question(this,
                              i18n("Delete Asset"),
                              i18n("Delete this file?\n%1", path),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }
    if (!QFile::remove(path))
        QMessageBox::warning(this, i18n("Delete Asset"), i18n("Could not delete the file."));
    refreshAssets();
}

void AssetLibraryDock::showSettings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(i18n("Asset Library Settings"));
    auto *outer = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    auto *pathsTab = new QWidget(tabs);
    auto *pathsLayout = new QVBoxLayout(pathsTab);
    auto *pathButtons = new QHBoxLayout;
    pathButtons->addStretch();
    auto *add = new QPushButton(i18n("Add"), pathsTab);
    auto *remove = new QPushButton(i18n("Remove"), pathsTab);
    auto *up = new QPushButton(i18n("Up"), pathsTab);
    auto *down = new QPushButton(i18n("Down"), pathsTab);
    pathButtons->addWidget(add);
    pathButtons->addWidget(remove);
    pathButtons->addWidget(up);
    pathButtons->addWidget(down);
    pathsLayout->addLayout(pathButtons);
    auto *table = new QTableWidget(0, 4, pathsTab);
    table->setHorizontalHeaderLabels({i18n("Alias"), i18n("Path"), i18n("Include Subfolders"), i18n("Extensions")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    pathsLayout->addWidget(table, 1);
    const auto appendRow = [table](const AssetPathEntry &entry) {
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(entry.alias));
        table->setItem(row, 1, new QTableWidgetItem(entry.path));
        table->setItem(row, 2, checkedItem(entry.includeSubfolders));
        table->setItem(row, 3, new QTableWidgetItem(entry.extensions));
    };
    for (const AssetPathEntry &entry : std::as_const(m_settings.paths))
        appendRow(entry);
    connect(add, &QPushButton::clicked, &dialog, [&, appendRow]() {
        const QString path = QFileDialog::getExistingDirectory(&dialog, i18n("Select Asset Folder"));
        if (path.isEmpty())
            return;
        bool ok = false;
        const QString alias =
            QInputDialog::getText(&dialog, i18n("Folder Alias"), i18n("Alias (optional)"), QLineEdit::Normal, {}, &ok);
        if (ok)
            appendRow({alias, path, false, QString::fromLatin1(defaultExtensions)});
    });
    connect(remove, &QPushButton::clicked, &dialog, [table]() {
        if (table->currentRow() >= 0)
            table->removeRow(table->currentRow());
    });
    const auto moveRow = [table](int direction) {
        const int row = table->currentRow();
        const int target = row + direction;
        if (row < 0 || target < 0 || target >= table->rowCount())
            return;
        QStringList text;
        for (int column = 0; column < 4; ++column)
            text.append(table->item(row, column) ? table->item(row, column)->text() : QString());
        const bool checked = table->item(row, 2) && table->item(row, 2)->checkState() == Qt::Checked;
        table->removeRow(row);
        table->insertRow(target);
        table->setItem(target, 0, new QTableWidgetItem(text.at(0)));
        table->setItem(target, 1, new QTableWidgetItem(text.at(1)));
        table->setItem(target, 2, checkedItem(checked));
        table->setItem(target, 3, new QTableWidgetItem(text.at(3)));
        table->selectRow(target);
    };
    connect(up, &QPushButton::clicked, &dialog, [moveRow]() {
        moveRow(-1);
    });
    connect(down, &QPushButton::clicked, &dialog, [moveRow]() {
        moveRow(1);
    });
    tabs->addTab(pathsTab, i18n("Asset Paths"));

    auto *displayTab = new QWidget(tabs);
    auto *displayLayout = new QVBoxLayout(displayTab);
    auto *form = new QFormLayout;
    auto *autoColumns = new QCheckBox(i18n("Auto columns"), displayTab);
    autoColumns->setChecked(m_settings.autoColumns);
    auto *columns = new QSpinBox(displayTab);
    columns->setRange(1, 12);
    columns->setValue(m_settings.columns);
    columns->setDisabled(autoColumns->isChecked());
    auto *thumbnail = new QSpinBox(displayTab);
    thumbnail->setRange(48, 512);
    thumbnail->setValue(m_settings.thumbnailSize);
    auto *uiFont = new QSpinBox(displayTab);
    auto *headerFont = new QSpinBox(displayTab);
    auto *nameFont = new QSpinBox(displayTab);
    for (auto *spin : {uiFont, headerFont, nameFont})
        spin->setRange(7, 32);
    uiFont->setValue(m_settings.uiFontSize);
    headerFont->setValue(m_settings.headerFontSize);
    nameFont->setValue(m_settings.assetNameFontSize);
    form->addRow(i18n("Thumbnail columns"), autoColumns);
    form->addRow(i18n("Fixed thumbnail columns"), columns);
    form->addRow(i18n("Thumbnail size"), thumbnail);
    form->addRow(i18n("Folder button font size"), uiFont);
    form->addRow(i18n("Header font size"), headerFont);
    form->addRow(i18n("Asset filename font size"), nameFont);
    displayLayout->addLayout(form);
    displayLayout->addStretch();
    connect(autoColumns, &QCheckBox::toggled, columns, &QWidget::setDisabled);
    tabs->addTab(displayTab, i18n("Display"));
    outer->addWidget(tabs, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_settings.paths.clear();
    for (int row = 0; row < table->rowCount(); ++row) {
        AssetPathEntry entry;
        entry.alias = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
        entry.path = table->item(row, 1) ? table->item(row, 1)->text().trimmed() : QString();
        entry.includeSubfolders = table->item(row, 2) && table->item(row, 2)->checkState() == Qt::Checked;
        entry.extensions =
            table->item(row, 3) ? table->item(row, 3)->text().trimmed() : QString::fromLatin1(defaultExtensions);
        if (!entry.path.isEmpty())
            m_settings.paths.append(entry);
    }
    m_settings.autoColumns = autoColumns->isChecked();
    m_settings.columns = columns->value();
    m_settings.thumbnailSize = thumbnail->value();
    m_settings.uiFontSize = uiFont->value();
    m_settings.headerFontSize = headerFont->value();
    m_settings.assetNameFontSize = nameFont->value();
    m_settings.save();
    applySettings();
    loadFolders();
}

void AssetLibraryDock::setAssetPanelHidden(bool hidden, bool save)
{
    const int height = this->height();
    if (hidden) {
        const QList<int> sizes = m_splitter->sizes();
        if (sizes.size() == 2 && sizes.at(1) > 0)
            m_settings.splitterSizes = sizes;
        m_settings.expandedWindowWidth = std::max(width(), 720);
        const int leftWidth = sizes.isEmpty() ? m_folderPanel->width() : sizes.first();
        m_settings.collapsedWindowWidth = std::max(120, leftWidth + std::max(24, width() - m_splitter->width() + 12));
        m_assetPanel->hide();
        m_hideButton->setText(i18n("Show"));
        resize(m_settings.collapsedWindowWidth, height);
    } else {
        m_assetPanel->show();
        m_restoringLayout = true;
        m_splitter->setSizes(m_settings.splitterSizes);
        m_restoringLayout = false;
        m_hideButton->setText(i18n("Hide"));
        resize(m_settings.expandedWindowWidth, height);
    }
    m_settings.rightPanelHidden = hidden;
    if (save)
        saveRuntimeSettings();
}

void AssetLibraryDock::saveRuntimeSettings()
{
    m_settings.windowHeight = height();
    if (m_assetPanel->isHidden()) {
        m_settings.collapsedWindowWidth = width();
    } else {
        m_settings.windowWidth = width();
        m_settings.expandedWindowWidth = width();
        const QList<int> sizes = m_splitter->sizes();
        if (sizes.size() == 2 && sizes.at(1) > 0)
            m_settings.splitterSizes = sizes;
    }
    m_settings.rightPanelHidden = m_assetPanel->isHidden();
    m_settings.save();
}

bool AssetLibraryDock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_scroll->viewport() && event->type() == QEvent::Resize && m_layoutTimer && m_settings.autoColumns
        && !m_sections.isEmpty() && !m_assetPanel->isHidden() && thumbnailColumns() != m_currentColumns) {
        m_layoutTimer->start();
    }
    return QDockWidget::eventFilter(watched, event);
}

void AssetLibraryDock::resizeEvent(QResizeEvent *event)
{
    QDockWidget::resizeEvent(event);
    if (!m_restoringLayout && m_saveTimer)
        m_saveTimer->start();
    if (m_layoutTimer && m_settings.autoColumns && !m_sections.isEmpty() && !m_assetPanel->isHidden()
        && thumbnailColumns() != m_currentColumns) {
        m_layoutTimer->start();
    }
}

void AssetLibraryDock::closeEvent(QCloseEvent *event)
{
    saveRuntimeSettings();
    QDockWidget::closeEvent(event);
}

QString AssetLibraryDockFactory::id() const
{
    return QStringLiteral("AssetLibraryDocker");
}

QDockWidget *AssetLibraryDockFactory::createDockWidget()
{
    auto *dock = new AssetLibraryDock;
    dock->setObjectName(id());
    return dock;
}

KoDockFactoryBase::DockPosition AssetLibraryDockFactory::defaultDockPosition() const
{
    return DockMinimized;
}
