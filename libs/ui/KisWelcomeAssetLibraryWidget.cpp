/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "KisWelcomeAssetLibraryWidget.h"

#include "KisMainWindow.h"
#include "KisViewManager.h"

#include <KoResourcePaths.h>
#include <KoStore.h>
#include <kconfiggroup.h>
#include <klocalizedstring.h>
#include <ksharedconfig.h>

#include <kis_file_layer.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_import_catcher.h>
#include <kis_node_commands_adapter.h>
#include <kis_node_manager.h>

#include <QAbstractItemView>
#include <QCache>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDateTime>
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
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace
{
constexpr auto defaultExtensions = "kra,jpg,jpeg,png,svg";

struct PathEntry {
    QString alias;
    QString path;
    bool includeSubfolders{false};
    QString extensions{QString::fromLatin1(defaultExtensions)};
};

struct Settings {
    QVector<PathEntry> paths;
    bool autoColumns{true};
    int columns{3};
    int thumbnailSize{140};
    int uiFontSize{10};
    int headerFontSize{10};
    int assetNameFontSize{10};
    QJsonObject raw;

    static QString filePath()
    {
        return QDir(KoResourcePaths::saveLocation("data", "krita_asset_library/", true))
            .filePath(QStringLiteral("config.json"));
    }

    void load()
    {
        QByteArray bytes;
        QFile file(filePath());
        if (file.open(QIODevice::ReadOnly)) {
            bytes = file.readAll();
        } else {
            bytes = KSharedConfig::openConfig()
                        ->group(QStringLiteral("asset_library"))
                        .readEntry(QStringLiteral("settings_json"), QString())
                        .toUtf8();
        }
        raw = QJsonDocument::fromJson(bytes).object();
        paths.clear();
        for (const QJsonValue &value : raw.value(QStringLiteral("paths")).toArray()) {
            const QJsonObject object = value.toObject();
            PathEntry entry;
            entry.alias = object.value(QStringLiteral("alias")).toString();
            entry.path = object.value(QStringLiteral("path")).toString();
            entry.includeSubfolders = object.value(QStringLiteral("include_subfolders"))
                                          .toBool(object.value(QStringLiteral("nested")).toBool(false));
            entry.extensions =
                object.value(QStringLiteral("extensions")).toString(QString::fromLatin1(defaultExtensions));
            if (!entry.path.isEmpty())
                paths.append(entry);
        }
        autoColumns = raw.value(QStringLiteral("auto_columns")).toBool(true);
        columns = std::clamp(raw.value(QStringLiteral("columns")).toInt(3), 1, 12);
        thumbnailSize = std::clamp(raw.value(QStringLiteral("thumbnail_size")).toInt(140), 48, 512);
        const int legacyFont = raw.value(QStringLiteral("font_size")).toInt(10);
        uiFontSize = std::clamp(raw.value(QStringLiteral("ui_font_size")).toInt(legacyFont), 7, 32);
        headerFontSize = std::clamp(raw.value(QStringLiteral("header_font_size")).toInt(uiFontSize), 7, 32);
        assetNameFontSize = std::clamp(raw.value(QStringLiteral("asset_name_font_size")).toInt(legacyFont), 7, 32);
    }

    bool save()
    {
        QJsonArray pathArray;
        for (const PathEntry &entry : std::as_const(paths)) {
            pathArray.append(QJsonObject{{QStringLiteral("alias"), entry.alias},
                                         {QStringLiteral("path"), entry.path},
                                         {QStringLiteral("include_subfolders"), entry.includeSubfolders},
                                         {QStringLiteral("extensions"), entry.extensions}});
        }
        raw.insert(QStringLiteral("paths"), pathArray);
        raw.insert(QStringLiteral("auto_columns"), autoColumns);
        raw.insert(QStringLiteral("columns"), columns);
        raw.insert(QStringLiteral("thumbnail_size"), thumbnailSize);
        raw.insert(QStringLiteral("ui_font_size"), uiFontSize);
        raw.insert(QStringLiteral("header_font_size"), headerFontSize);
        raw.insert(QStringLiteral("asset_name_font_size"), assetNameFontSize);
        raw.remove(QStringLiteral("font_size"));
        QFile file(filePath());
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(QJsonDocument(raw).toJson()) >= 0;
    }
};

QTableWidgetItem *checkableItem(bool checked)
{
    auto *item = new QTableWidgetItem;
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}

class SettingsDialog : public QDialog
{
public:
    SettingsDialog(const Settings &settings, QWidget *parent)
        : QDialog(parent)
        , m_settings(settings)
    {
        setWindowTitle(i18n("Asset Library Settings"));
        resize(760, 480);
        auto *outer = new QVBoxLayout(this);
        auto *tabs = new QTabWidget(this);

        auto *pathsPage = new QWidget(tabs);
        auto *pathsLayout = new QVBoxLayout(pathsPage);
        auto *buttonsLayout = new QHBoxLayout;
        buttonsLayout->addStretch();
        auto *add = new QPushButton(i18n("Add"), pathsPage);
        auto *remove = new QPushButton(i18n("Remove"), pathsPage);
        auto *up = new QPushButton(i18n("Up"), pathsPage);
        auto *down = new QPushButton(i18n("Down"), pathsPage);
        buttonsLayout->addWidget(add);
        buttonsLayout->addWidget(remove);
        buttonsLayout->addWidget(up);
        buttonsLayout->addWidget(down);
        pathsLayout->addLayout(buttonsLayout);
        m_table = new QTableWidget(0, 4, pathsPage);
        m_table->setHorizontalHeaderLabels(
            {i18n("Alias"), i18n("Path"), i18n("Include Subfolders"), i18n("Extensions")});
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        pathsLayout->addWidget(m_table, 1);
        for (const PathEntry &entry : std::as_const(m_settings.paths))
            appendRow(entry);
        connect(add, &QPushButton::clicked, this, [this]() {
            const QString path = QFileDialog::getExistingDirectory(this, i18n("Select Asset Folder"));
            if (path.isEmpty())
                return;
            bool ok = false;
            const QString alias = QInputDialog::getText(this,
                                                        i18n("Folder Alias"),
                                                        i18n("Alias (optional)"),
                                                        QLineEdit::Normal,
                                                        QString(),
                                                        &ok);
            if (ok)
                appendRow({alias, path, false, QString::fromLatin1(defaultExtensions)});
        });
        connect(remove, &QPushButton::clicked, this, [this]() {
            if (m_table->currentRow() >= 0)
                m_table->removeRow(m_table->currentRow());
        });
        connect(up, &QPushButton::clicked, this, [this]() {
            moveRow(-1);
        });
        connect(down, &QPushButton::clicked, this, [this]() {
            moveRow(1);
        });
        tabs->addTab(pathsPage, i18n("Asset Paths"));

        auto *displayPage = new QWidget(tabs);
        auto *displayLayout = new QVBoxLayout(displayPage);
        auto *form = new QFormLayout;
        m_autoColumns = new QCheckBox(i18n("Auto columns"), displayPage);
        m_autoColumns->setChecked(m_settings.autoColumns);
        m_columns = spinBox(1, 12, m_settings.columns, displayPage);
        m_thumbnailSize = spinBox(48, 512, m_settings.thumbnailSize, displayPage);
        m_uiFontSize = spinBox(7, 32, m_settings.uiFontSize, displayPage);
        m_headerFontSize = spinBox(7, 32, m_settings.headerFontSize, displayPage);
        m_assetNameFontSize = spinBox(7, 32, m_settings.assetNameFontSize, displayPage);
        m_columns->setDisabled(m_autoColumns->isChecked());
        connect(m_autoColumns, &QCheckBox::toggled, m_columns, &QWidget::setDisabled);
        form->addRow(i18n("Thumbnail columns"), m_autoColumns);
        form->addRow(i18n("Fixed thumbnail columns"), m_columns);
        form->addRow(i18n("Thumbnail size"), m_thumbnailSize);
        form->addRow(i18n("Folder button font size"), m_uiFontSize);
        form->addRow(i18n("Header font size"), m_headerFontSize);
        form->addRow(i18n("Asset filename font size"), m_assetNameFontSize);
        displayLayout->addLayout(form);
        displayLayout->addStretch();
        tabs->addTab(displayPage, i18n("Display"));
        outer->addWidget(tabs, 1);
        auto *dialogButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(dialogButtons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(dialogButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        outer->addWidget(dialogButtons);
    }

    Settings values() const
    {
        Settings result = m_settings;
        result.paths.clear();
        for (int row = 0; row < m_table->rowCount(); ++row) {
            PathEntry entry;
            entry.alias = itemText(row, 0).trimmed();
            entry.path = itemText(row, 1).trimmed();
            entry.includeSubfolders = m_table->item(row, 2) && m_table->item(row, 2)->checkState() == Qt::Checked;
            entry.extensions = itemText(row, 3).trimmed();
            if (entry.extensions.isEmpty())
                entry.extensions = QString::fromLatin1(defaultExtensions);
            if (!entry.path.isEmpty())
                result.paths.append(entry);
        }
        result.autoColumns = m_autoColumns->isChecked();
        result.columns = m_columns->value();
        result.thumbnailSize = m_thumbnailSize->value();
        result.uiFontSize = m_uiFontSize->value();
        result.headerFontSize = m_headerFontSize->value();
        result.assetNameFontSize = m_assetNameFontSize->value();
        return result;
    }

private:
    static QSpinBox *spinBox(int minimum, int maximum, int value, QWidget *parent)
    {
        auto *spin = new QSpinBox(parent);
        spin->setRange(minimum, maximum);
        spin->setValue(value);
        return spin;
    }

    QString itemText(int row, int column) const
    {
        return m_table->item(row, column) ? m_table->item(row, column)->text() : QString();
    }

    void appendRow(const PathEntry &entry)
    {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(entry.alias));
        m_table->setItem(row, 1, new QTableWidgetItem(entry.path));
        m_table->setItem(row, 2, checkableItem(entry.includeSubfolders));
        m_table->setItem(row, 3, new QTableWidgetItem(entry.extensions));
    }

    void moveRow(int offset)
    {
        const int row = m_table->currentRow();
        const int target = row + offset;
        if (row < 0 || target < 0 || target >= m_table->rowCount())
            return;
        PathEntry entry{itemText(row, 0),
                        itemText(row, 1),
                        m_table->item(row, 2) && m_table->item(row, 2)->checkState() == Qt::Checked,
                        itemText(row, 3)};
        m_table->removeRow(row);
        m_table->insertRow(target);
        m_table->setItem(target, 0, new QTableWidgetItem(entry.alias));
        m_table->setItem(target, 1, new QTableWidgetItem(entry.path));
        m_table->setItem(target, 2, checkableItem(entry.includeSubfolders));
        m_table->setItem(target, 3, new QTableWidgetItem(entry.extensions));
        m_table->selectRow(target);
    }

    Settings m_settings;
    QTableWidget *m_table{nullptr};
    QCheckBox *m_autoColumns{nullptr};
    QSpinBox *m_columns{nullptr};
    QSpinBox *m_thumbnailSize{nullptr};
    QSpinBox *m_uiFontSize{nullptr};
    QSpinBox *m_headerFontSize{nullptr};
    QSpinBox *m_assetNameFontSize{nullptr};
};

class AssetTile : public QFrame
{
public:
    AssetTile(const QString &path,
              const QPixmap &thumbnail,
              int thumbnailSize,
              int fontSize,
              std::function<void()> open,
              std::function<void(const QPoint &)> menu,
              QWidget *parent)
        : QFrame(parent)
        , m_open(std::move(open))
        , m_menu(std::move(menu))
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
        QFont font = name->font();
        font.setPointSize(fontSize);
        name->setFont(font);
        layout->addWidget(name);
    }

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_open)
            m_open();
        QFrame::mouseDoubleClickEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (m_menu)
            m_menu(event->globalPos());
    }

private:
    std::function<void()> m_open;
    std::function<void(const QPoint &)> m_menu;
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
} // namespace

struct KisWelcomeAssetLibraryWidget::Private {
    struct Section {
        QString title;
        QStringList files;
    };

    explicit Private(KisWelcomeAssetLibraryWidget *q)
        : q(q)
        , thumbnailCache(64 * 1024)
    {
    }

    void buildUi()
    {
        auto *rootLayout = new QHBoxLayout(q);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(6);
        folderPanel = new QWidget(q);
        auto *folderLayout = new QVBoxLayout(folderPanel);
        folderLayout->setContentsMargins(0, 0, 0, 0);
        folderList = new QListWidget(folderPanel);
        folderList->setMinimumWidth(120);
        folderLayout->addWidget(folderList, 1);
        auto *refresh = new QPushButton(i18n("Refresh"), folderPanel);
        auto *settingsButton = new QPushButton(i18n("Settings"), folderPanel);
        folderLayout->addWidget(refresh);
        folderLayout->addWidget(settingsButton);
        rootLayout->addWidget(folderPanel, 0);

        auto *assetPanel = new QWidget(q);
        auto *assetPanelLayout = new QVBoxLayout(assetPanel);
        assetPanelLayout->setContentsMargins(6, 0, 0, 0);
        statusLabel = new QLabel(assetPanel);
        assetPanelLayout->addWidget(statusLabel);
        scroll = new QScrollArea(assetPanel);
        scroll->setWidgetResizable(true);
        scroll->viewport()->installEventFilter(q);
        assetHost = new QWidget(scroll);
        assetLayout = new QVBoxLayout(assetHost);
        assetLayout->setContentsMargins(4, 4, 4, 4);
        assetLayout->setSpacing(14);
        scroll->setWidget(assetHost);
        assetPanelLayout->addWidget(scroll, 1);
        rootLayout->addWidget(assetPanel, 1);

        layoutTimer = new QTimer(q);
        layoutTimer->setSingleShot(true);
        layoutTimer->setInterval(100);
        QObject::connect(layoutTimer, &QTimer::timeout, q, [this]() {
            populate();
        });
        QObject::connect(folderList, &QListWidget::currentItemChanged, q, [this](QListWidgetItem *current) {
            folderChanged(current);
        });
        QObject::connect(refresh, &QPushButton::clicked, q, [this]() {
            refreshAssets();
        });
        QObject::connect(settingsButton, &QPushButton::clicked, q, [this]() {
            SettingsDialog dialog(settings, q);
            if (dialog.exec() != QDialog::Accepted)
                return;
            settings = dialog.values();
            settings.save();
            thumbnailCache.clear();
            applyFont();
            loadFolders();
        });
    }

    void applyFont()
    {
        QFont font = q->font();
        font.setPointSize(settings.uiFontSize);
        q->setFont(font);
    }

    void loadFolders()
    {
        const QString preferredPath = currentPath;
        folderList->clear();
        int preferredRow = -1;
        for (int index = 0; index < settings.paths.size(); ++index) {
            const PathEntry &entry = settings.paths.at(index);
            QString label = entry.alias.trimmed();
            if (label.isEmpty())
                label = QFileInfo(entry.path).fileName();
            auto *item = new QListWidgetItem(label.isEmpty() ? entry.path : label, folderList);
            item->setToolTip(entry.path);
            item->setData(Qt::UserRole, index);
            if (entry.path == preferredPath)
                preferredRow = index;
        }
        if (folderList->count()) {
            folderList->setCurrentRow(preferredRow >= 0 ? preferredRow : 0);
        } else {
            currentPath.clear();
            sections.clear();
            clearLayout(assetLayout);
            statusLabel->setText(i18n("Add asset paths from Settings."));
        }
    }

    void folderChanged(QListWidgetItem *item)
    {
        if (!item) {
            currentPath.clear();
            return;
        }
        const int index = item->data(Qt::UserRole).toInt();
        if (index < 0 || index >= settings.paths.size())
            return;
        const PathEntry &entry = settings.paths.at(index);
        currentPath = entry.path;
        includeSubfolders = entry.includeSubfolders;
        extensionText = entry.extensions;
        refreshAssets();
    }

    QStringList extensions() const
    {
        QStringList result;
        for (QString extension : extensionText.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
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

    void refreshAssets()
    {
        layoutTimer->stop();
        sections.clear();
        if (currentPath.isEmpty()) {
            clearLayout(assetLayout);
            statusLabel->setText(i18n("No asset path is selected."));
            return;
        }
        if (!QDir(currentPath).exists()) {
            clearLayout(assetLayout);
            statusLabel->setText(i18n("Folder not found: %1", currentPath));
            return;
        }
        const QStringList allowed = extensions();
        QMap<QString, QStringList> byFolder;
        QDirIterator iterator(currentPath,
                              QDir::Files | QDir::NoDotAndDotDot,
                              includeSubfolders ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            if (allowed.contains(QFileInfo(path).suffix().toLower()))
                byFolder[QFileInfo(path).absolutePath()].append(path);
        }
        for (auto it = byFolder.begin(); it != byFolder.end(); ++it) {
            QStringList files = it.value();
            std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) {
                return QString::localeAwareCompare(QFileInfo(a).fileName(), QFileInfo(b).fileName()) < 0;
            });
            const QString folderName = QFileInfo(it.key()).fileName();
            sections.append({folderName.isEmpty() ? it.key() : folderName, files});
        }
        populate();
        layoutTimer->start();
    }

    int columns() const
    {
        if (!settings.autoColumns)
            return std::max(1, settings.columns);
        const int tileWidth = settings.thumbnailSize + 24;
        return std::max(1, (std::max(1, scroll->viewport()->width() - 8) + 10) / (tileWidth + 10));
    }

    QString thumbnailKey(const QString &path) const
    {
        const QFileInfo info(path);
        const qreal ratio = q->devicePixelRatioF();
        return QStringLiteral("%1|%2|%3|%4|%5")
            .arg(info.absoluteFilePath())
            .arg(info.size())
            .arg(info.lastModified().toMSecsSinceEpoch())
            .arg(settings.thumbnailSize)
            .arg(ratio);
    }

    QPixmap thumbnail(const QString &path)
    {
        const QString key = thumbnailKey(path);
        if (const QPixmap *cached = thumbnailCache.object(key))
            return *cached;
        const QFileInfo info(path);
        const qreal ratio = q->devicePixelRatioF();
        const int pixelSize = std::max(1, qRound(settings.thumbnailSize * ratio));
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
            if (reader.size().isValid())
                reader.setScaledSize(reader.size().scaled(pixelSize, pixelSize, Qt::KeepAspectRatio));
            source = reader.read();
        }
        QPixmap result;
        if (!source.isNull()) {
            result =
                QPixmap::fromImage(source.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            result.setDevicePixelRatio(ratio);
        } else {
            result = QPixmap(pixelSize, pixelSize);
            result.setDevicePixelRatio(ratio);
            result.fill(QColor(245, 245, 245));
            QPainter painter(&result);
            painter.setPen(QColor(75, 75, 75));
            painter.drawRect(result.rect().adjusted(0, 0, -1, -1));
            painter.drawText(result.rect(), Qt::AlignCenter, info.suffix().toUpper());
        }
        const int cost = std::max(1, result.width() * result.height() * 4 / 1024);
        thumbnailCache.insert(key, new QPixmap(result), cost);
        return result;
    }

    void populate()
    {
        clearLayout(assetLayout);
        scroll->verticalScrollBar()->setValue(0);
        int count = 0;
        for (const Section &section : std::as_const(sections))
            count += section.files.size();
        statusLabel->setText(i18np("1 asset", "%1 assets", count));
        currentColumns = columns();
        for (const Section &section : std::as_const(sections)) {
            if (section.files.isEmpty())
                continue;
            auto *title = new QLabel(section.title, assetHost);
            QFont titleFont = q->font();
            titleFont.setBold(true);
            titleFont.setPointSize(settings.headerFontSize);
            title->setFont(titleFont);
            assetLayout->addWidget(title);
            auto *gridHost = new QWidget(assetHost);
            auto *grid = new QGridLayout(gridHost);
            grid->setContentsMargins(0, 0, 0, 10);
            grid->setSpacing(10);
            for (int index = 0; index < section.files.size(); ++index) {
                const QString path = section.files.at(index);
                auto *tile = new AssetTile(
                    path,
                    thumbnail(path),
                    settings.thumbnailSize,
                    settings.assetNameFontSize,
                    [this, path]() {
                        openAsset(path);
                    },
                    [this, path](const QPoint &position) {
                        showAssetMenu(path, position);
                    },
                    gridHost);
                grid->addWidget(tile, index / currentColumns, index % currentColumns);
            }
            grid->setColumnStretch(currentColumns, 1);
            assetLayout->addWidget(gridHost);
        }
        assetLayout->addStretch();
    }

    void openAsset(const QString &path)
    {
        if (mainWindow)
            mainWindow->openDocument(path, KisMainWindow::None);
    }

    bool requireDocument(const QString &title) const
    {
        if (mainWindow && mainWindow->viewManager() && mainWindow->viewManager()->image())
            return true;
        QMessageBox::warning(q, title, i18n("No active document."));
        return false;
    }

    void insertAsLayer(const QString &path)
    {
        if (!requireDocument(i18n("Insert as New Layer")))
            return;
        new KisImportCatcher(path,
                             mainWindow->viewManager(),
                             QFileInfo(path).suffix().compare(QStringLiteral("svg"), Qt::CaseInsensitive) == 0
                                 ? QStringLiteral("KisShapeLayer")
                                 : QStringLiteral("KisPaintLayer"));
    }

    void insertAsFileLayer(const QString &path)
    {
        if (!requireDocument(i18n("Insert as New File Layer")))
            return;
        KisViewManager *view = mainWindow->viewManager();
        const QFileInfo info(path);
        KisLayerSP layer = new KisFileLayer(view->image(),
                                            info.absolutePath(),
                                            info.fileName(),
                                            KisFileLayer::None,
                                            QStringLiteral("Bicubic"),
                                            info.completeBaseName(),
                                            255);
        KisNodeSP active = view->activeNode();
        KisNodeSP parent = active && active->parent() ? active->parent() : KisNodeSP(view->image()->rootLayer());
        KisNodeCommandsAdapter adapter(view);
        adapter.addNode(layer, parent, active);
        view->nodeManager()->slotNonUiActivatedNode(layer);
    }

    QString requestPath(const QString &path, const QString &title, bool allowCurrent = false)
    {
        bool ok = false;
        QString name =
            QInputDialog::getText(q, title, i18n("New file name"), QLineEdit::Normal, QFileInfo(path).fileName(), &ok)
                .trimmed();
        if (!ok || name.isEmpty())
            return {};
        if (QFileInfo(name).fileName() != name) {
            QMessageBox::warning(q, title, i18n("File name cannot contain a path."));
            return {};
        }
        if (QFileInfo(name).suffix().isEmpty())
            name += QLatin1Char('.') + QFileInfo(path).suffix();
        const QString target = QDir(QFileInfo(path).absolutePath()).filePath(name);
        if (allowCurrent && QFileInfo(target) == QFileInfo(path))
            return target;
        if (QFileInfo::exists(target)) {
            QMessageBox::warning(q, title, i18n("A file with that name already exists."));
            return {};
        }
        return target;
    }

    void showAssetMenu(const QString &path, const QPoint &position)
    {
        QMenu menu(q);
        QAction *open = menu.addAction(i18n("Open"));
        QAction *insertLayer = menu.addAction(i18n("Insert as New Layer"));
        QAction *insertFileLayer = menu.addAction(i18n("Insert as New File Layer"));
        QAction *duplicate = menu.addAction(i18n("Duplicate"));
        QAction *rename = menu.addAction(i18n("Rename"));
        QAction *remove = menu.addAction(i18n("Delete"));
        const bool hasDocument = mainWindow && mainWindow->viewManager() && mainWindow->viewManager()->image();
        insertLayer->setEnabled(hasDocument);
        insertFileLayer->setEnabled(hasDocument);
        QAction *selected = menu.exec(position);
        if (selected == open) {
            openAsset(path);
        } else if (selected == insertLayer) {
            insertAsLayer(path);
        } else if (selected == insertFileLayer) {
            insertAsFileLayer(path);
        } else if (selected == duplicate) {
            const QString target = requestPath(path, i18n("Duplicate Asset"));
            if (!target.isEmpty() && !QFile::copy(path, target))
                QMessageBox::warning(q, i18n("Duplicate Asset"), i18n("Could not copy the file."));
            refreshAssets();
        } else if (selected == rename) {
            const QString target = requestPath(path, i18n("Rename Asset"), true);
            if (!target.isEmpty() && target != path && !QFile::rename(path, target))
                QMessageBox::warning(q, i18n("Rename Asset"), i18n("Could not rename the file."));
            refreshAssets();
        } else if (selected == remove
                   && QMessageBox::question(q,
                                            i18n("Delete Asset"),
                                            i18n("Delete this file?\n%1", path),
                                            QMessageBox::Yes | QMessageBox::No,
                                            QMessageBox::No)
                       == QMessageBox::Yes) {
            if (!QFile::remove(path))
                QMessageBox::warning(q, i18n("Delete Asset"), i18n("Could not delete the file."));
            refreshAssets();
        }
    }

    KisWelcomeAssetLibraryWidget *q;
    KisMainWindow *mainWindow{nullptr};
    Settings settings;
    QCache<QString, QPixmap> thumbnailCache;
    QVector<Section> sections;
    QString currentPath;
    QString extensionText{QString::fromLatin1(defaultExtensions)};
    bool includeSubfolders{false};
    QWidget *folderPanel{nullptr};
    QListWidget *folderList{nullptr};
    QLabel *statusLabel{nullptr};
    QScrollArea *scroll{nullptr};
    QWidget *assetHost{nullptr};
    QVBoxLayout *assetLayout{nullptr};
    QTimer *layoutTimer{nullptr};
    int currentColumns{0};
    QDateTime settingsModified;
    bool loaded{false};
};

KisWelcomeAssetLibraryWidget::KisWelcomeAssetLibraryWidget(QWidget *parent)
    : QWidget(parent)
    , d(new Private(this))
{
    d->buildUi();
}

KisWelcomeAssetLibraryWidget::~KisWelcomeAssetLibraryWidget() = default;

void KisWelcomeAssetLibraryWidget::setMainWindow(KisMainWindow *mainWindow)
{
    d->mainWindow = mainWindow;
}

void KisWelcomeAssetLibraryWidget::reloadSettings()
{
    const QDateTime modified = QFileInfo(Settings::filePath()).lastModified();
    if (d->loaded && modified == d->settingsModified)
        return;
    d->settings.load();
    d->settingsModified = modified;
    d->loaded = true;
    d->applyFont();
    d->loadFolders();
}

bool KisWelcomeAssetLibraryWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == d->scroll->viewport() && event->type() == QEvent::Resize && d->layoutTimer && d->settings.autoColumns
        && !d->sections.isEmpty() && d->columns() != d->currentColumns) {
        d->layoutTimer->start();
    }
    return QWidget::eventFilter(watched, event);
}
