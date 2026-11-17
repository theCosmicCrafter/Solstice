/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AssetLibraryDock.h"
#include <assetlibrary/KisAssetLibraryOperations.h>
#include <utils/KisShowInFileExplorer.h>

#include <KisMainWindow.h>
#include <KisViewManager.h>
#include <klocalizedstring.h>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QInputDialog>
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
        QAction *showInExplorer = menu.addAction(KisShowInFileExplorer::actionText());
        QAction *insertLayer = menu.addAction(i18n("Insert as New Layer"));
        QAction *insertFileLayer = menu.addAction(i18n("Insert as New File Layer"));
        QAction *duplicate = menu.addAction(i18n("Duplicate"));
        QAction *rename = menu.addAction(i18n("Rename"));
        QAction *remove = menu.addAction(i18n("Delete"));
        QAction *selected = menu.exec(event->globalPos());
        if (selected == open && m_open)
            m_open(m_path);
        else if (selected == showInExplorer)
            KisShowInFileExplorer::showFile(m_path, this);
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

AssetLibraryDock::AssetLibraryDock(QWidget *parent)
    : QDockWidget(parent)
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
    // Queued, so that a save never reloads views inside the saving view's handler.
    connect(KisAssetLibrarySettingsNotifier::instance(),
            &KisAssetLibrarySettingsNotifier::sigSaved,
            this,
            &AssetLibraryDock::reloadSharedSettings,
            Qt::QueuedConnection);
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
    const QString preferredPath = m_currentPath;
    int preferredRow = 0;
    m_folderList->clear();
    for (int index = 0; index < m_settings.paths.size(); ++index) {
        const AssetPathEntry &entry = m_settings.paths.at(index);
        const QString label =
            entry.alias.trimmed().isEmpty() ? QFileInfo(entry.path).fileName() : entry.alias.trimmed();
        auto *item = new QListWidgetItem(label.isEmpty() ? entry.path : label, m_folderList);
        item->setToolTip(entry.path);
        item->setData(Qt::UserRole, index);
        if (!preferredPath.isEmpty() && entry.path == preferredPath)
            preferredRow = index;
    }
    if (m_folderList->count())
        m_folderList->setCurrentRow(preferredRow);
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
    m_sections = KisAssetLibrary::collectSections(m_currentPath, m_includeSubfolders, m_extensionText);
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
    return KisAssetLibrary::thumbnailColumns(m_settings.autoColumns,
                                             m_settings.columns,
                                             m_settings.thumbnailSize,
                                             m_scroll->viewport()->width());
}

QPixmap AssetLibraryDock::thumbnailForPath(const QString &path)
{
    return m_thumbnailCache.thumbnail(path, m_settings.thumbnailSize, devicePixelRatioF());
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
    KisAssetLibrary::insertAsLayer(path, m_viewManager, this);
}

void AssetLibraryDock::insertAsFileLayer(const QString &path)
{
    KisAssetLibrary::insertAsFileLayer(path, m_viewManager, this);
}

QString AssetLibraryDock::requestAssetPath(const QString &path, const QString &title, bool allowCurrent)
{
    return KisAssetLibrary::requestAssetPath(path, title, this, allowCurrent);
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
    m_settings.saveLayout();
}

void AssetLibraryDock::reloadSharedSettings()
{
    // The welcome page or a docker in another window saved: adopt its folders
    // and display settings, but keep this docker's own layout.
    AssetLibrarySettings latest;
    latest.load();
    if (latest.hasSameSharedValues(m_settings))
        return;
    m_settings.copySharedValues(latest);
    QFont f = font();
    f.setPointSize(m_settings.uiFontSize);
    setFont(f);
    loadFolders();
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
