/* SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "KisPresetDockerFilters.h"
#include <KisResourceItemListView.h>
#include <KisResourceModel.h>
#include <KisResourceStorage.h>
#include <KisResourceTypes.h>
#include <KisStorageModel.h>
#include <KisTagFilterResourceProxyModel.h>
#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QToolButton>
#include <algorithm>
#include <kis_config.h>
#include <kis_paintop_factory.h>
#include <kis_paintop_registry.h>
#include <klocalizedstring.h>

namespace
{
// Keep the dropdown open when a checkbox is toggled, including by keyboard.
class MultiCheckMenu : public QMenu
{
public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        auto *action = actionAt(event->pos());
        if (event->button() == Qt::LeftButton && action && action->isEnabled() && action->isCheckable()) {
            action->trigger();
            event->accept();
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        auto *action = activeAction();
        if ((event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && action
            && action->isEnabled() && action->isCheckable()) {
            action->trigger();
            event->accept();
            return;
        }
        QMenu::keyPressEvent(event);
    }
};
} // namespace

KisPresetDockerFilters::KisPresetDockerFilters(KisTagFilterResourceProxyModel *model,
                                               KisResourceItemListView *view,
                                               QWidget *parent,
                                               const QString &resourceType)
    : QWidget(parent)
    , m_presets(resourceType == ResourceType::PaintOpPresets)
    , m_groupingKey(m_presets ? QStringLiteral("Solstice/BrushPresetGrouping")
                              : QStringLiteral("Solstice/%1Grouping").arg(resourceType))
    , m_model(model)
    , m_resources(new KisResourceModel(resourceType, this))
    , m_view(view)
{
    setObjectName("PresetDockerFilters");
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto makeButton = [&](const QString &name, bool engines) {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        button->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new MultiCheckMenu(button);
        button->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, menu, engines]() {
            populate(menu, engines);
        });
        layout->addWidget(button);
        return button;
    };
    m_engines = makeButton("PresetEngineFilter", true);
    m_bundles = makeButton("PresetBundleFilter", false);
    m_engines->setToolTip(i18n("Show presets from the checked brush engines."));
    m_engines->setVisible(m_presets);
    m_bundles->setToolTip(
        m_presets ? i18n("Show presets stored in the checked bundles. This does not enable or disable bundles.")
                  : i18n("Show the items stored in the checked bundles. This does not enable or disable bundles."));
    m_grouping = new QComboBox(this);
    m_grouping->setObjectName("PresetGrouping");
    m_grouping->addItem(i18n("No Grouping"), NoGrouping);
    if (m_presets) {
        m_grouping->addItem(i18n("Group by Engine"), GroupByEngine);
    }
    m_grouping->addItem(i18n("Group by Bundle"), GroupByBundle);
    m_grouping->setToolTip(m_presets ? i18n("Show the presets in groups by brush engine or by bundle.")
                                     : i18n("Show the items in groups by bundle."));
    const int grouping = KisConfig(true).readEntry<int>(m_groupingKey, 0);
    m_grouping->setCurrentIndex(qMax(0, m_grouping->findData(grouping)));
    m_grouping->setVisible(m_view);
    layout->addWidget(m_grouping);
    connect(m_grouping, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        KisConfig(false).writeEntry<int>(m_groupingKey, m_grouping->itemData(index).toInt());
        applyGrouping();
    });
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(0);
    connect(&m_refresh, &QTimer::timeout, this, &KisPresetDockerFilters::refresh);
    for (auto *source : {static_cast<QAbstractItemModel *>(m_resources),
                         static_cast<QAbstractItemModel *>(KisStorageModel::instance())}) {
        auto schedule = [this]() {
            m_refresh.start();
        };
        connect(source, &QAbstractItemModel::modelReset, this, schedule);
        connect(source, &QAbstractItemModel::rowsInserted, this, schedule);
        connect(source, &QAbstractItemModel::rowsRemoved, this, schedule);
        connect(source,
                &QAbstractItemModel::dataChanged,
                this,
                [this](const QModelIndex &, const QModelIndex &, const QVector<int> &roles) {
                    // Dirty flags and thumbnail updates do not change the facets.
                    if (roles.isEmpty() || roles.contains(Qt::DisplayRole)
                        || roles.contains(Qt::UserRole + KisAbstractResourceModel::MetaData)
                        || roles.contains(Qt::UserRole + KisAbstractResourceModel::MD5)
                        || roles.contains(Qt::UserRole + KisAbstractResourceModel::StorageActive)
                        || roles.contains(Qt::UserRole + KisAbstractResourceModel::ResourceActive))
                        m_refresh.start();
                });
    }
    refresh();
}

void KisPresetDockerFilters::refresh()
{
    m_engineNames.clear();
    m_bundleNames.clear();
    m_storageKeys.clear();
    QSet<int> usedStorages;
    for (int row = 0; row < m_resources->rowCount(); ++row) {
        const auto index = m_resources->index(row, 0);
        if (m_presets) {
            const QString engine =
                index.data(Qt::UserRole + KisAbstractResourceModel::MetaData).toMap().value("paintopid").toString();
            auto *factory = KisPaintOpRegistry::instance()->value(engine);
            m_engineNames[engine] = factory ? factory->name() : (engine.isEmpty() ? i18n("Unknown engine") : engine);
        }
        usedStorages.unite(KisTagFilterResourceProxyModel::activeStorageIdsForIndex(index));
    }
    auto *storages = KisStorageModel::instance();
    for (int row = 0; row < storages->rowCount(); ++row) {
        const int id = storages->index(row, KisStorageModel::Id).data().toInt();
        if (!usedStorages.contains(id))
            continue;
        // Photoshop brush libraries are bundles of their tips, patterns and
        // presets
        const QString type = storages->index(row, KisStorageModel::StorageType).data().toString();
        if (type == "Bundle"
            || type
                == KisResourceStorage::storageTypeToUntranslatedString(
                    KisResourceStorage::StorageType::AdobeBrushLibrary)) {
            const QString location = storages->index(row, KisStorageModel::Location).data().toString();
            const QString filename = QFileInfo(location).fileName();
            QString label = storages->index(row, KisStorageModel::DisplayName).data().toString();
            if (label.isEmpty() || label == location)
                label = filename;
            else if (label != filename)
                label += " (" + filename + ')';
            m_storageKeys[id] = location;
            m_bundleNames[location] = label;
        }
    }
    for (int id : usedStorages) {
        if (!m_storageKeys.contains(id)) {
            m_storageKeys[id] = QString();
            m_bundleNames[QString()] = i18n("Not in a bundle");
        }
    }
    apply();
    m_groupCache.clear();
    applyGrouping();
}

KisPresetDockerFilters::~KisPresetDockerFilters()
{
    // The view may outlive this bar; its grouping function refers to it.
    if (m_view)
        m_view->setGrouping({});
}

QPair<QString, QString> KisPresetDockerFilters::groupOf(const QModelIndex &index) const
{
    const QVariant idValue = index.data(Qt::UserRole + KisAbstractResourceModel::Id);
    const int id = idValue.isValid() ? idValue.toInt() : -1;
    if (id >= 0) {
        auto it = m_groupCache.constFind(id);
        if (it != m_groupCache.constEnd())
            return it.value();
    }
    // Sort keys put named groups first ("1") and the catch-all group last ("2").
    QPair<QString, QString> group;
    if (m_grouping->currentData().toInt() == GroupByEngine) {
        const QString engine =
            index.data(Qt::UserRole + KisAbstractResourceModel::MetaData).toMap().value("paintopid").toString();
        const QString name = m_engineNames.value(engine);
        group = engine.isEmpty() || name.isEmpty() ? qMakePair(QString("2"), i18n("Unknown engine"))
                                                   : qMakePair(QString("1" + name), name);
    } else {
        // A preset in several bundles is shown once, under the bundle whose
        // name sorts first.
        QString bundle;
        for (int storage : KisTagFilterResourceProxyModel::activeStorageIdsForIndex(index)) {
            const QString location = m_storageKeys.value(storage);
            if (location.isEmpty())
                continue;
            const QString name = m_bundleNames.value(location, location);
            if (bundle.isEmpty() || QString::localeAwareCompare(name, bundle) < 0)
                bundle = name;
        }
        group = bundle.isEmpty() ? qMakePair(QString("2"), i18n("Not in a bundle")) : qMakePair(QString("1" + bundle), bundle);
    }
    if (id >= 0)
        m_groupCache.insert(id, group);
    return group;
}

void KisPresetDockerFilters::applyGrouping()
{
    if (!m_view)
        return;
    m_groupCache.clear();
    if (m_grouping->currentData().toInt() == NoGrouping) {
        m_view->setGrouping({});
    } else {
        m_view->setGrouping([this](const QModelIndex &index) {
            return groupOf(index);
        });
    }
}

void KisPresetDockerFilters::populate(QMenu *menu, bool engines)
{
    menu->clear();
    const auto &names = engines ? m_engineNames : m_bundleNames;
    auto &excluded = engines ? m_excludedEngines : m_excludedBundles;
    auto *all = menu->addAction(i18n("Select All"));
    all->setObjectName("SelectAll");
    connect(all, &QAction::triggered, this, [this, engines]() {
        (engines ? m_excludedEngines : m_excludedBundles).clear();
        apply();
    });
    auto *none = menu->addAction(i18n("Clear All"));
    none->setObjectName("ClearAll");
    connect(none, &QAction::triggered, this, [this, engines]() {
        const auto keys = (engines ? m_engineNames : m_bundleNames).keys();
        (engines ? m_excludedEngines : m_excludedBundles) = QSet<QString>(keys.begin(), keys.end());
        apply();
    });
    menu->addSeparator();
    auto keys = names.keys();
    std::sort(keys.begin(), keys.end(), [&](const QString &a, const QString &b) {
        return QString::localeAwareCompare(names[a], names[b]) < 0;
    });
    for (const auto &key : keys) {
        auto *action = menu->addAction(names[key]);
        action->setData(key);
        action->setCheckable(true);
        action->setChecked(!excluded.contains(key));
        connect(action, &QAction::toggled, this, [this, engines, key](bool checked) {
            auto &excluded = engines ? m_excludedEngines : m_excludedBundles;
            if (checked)
                excluded.remove(key);
            else
                excluded.insert(key);
            apply();
        });
    }
}

void KisPresetDockerFilters::apply()
{
    QStringList engines;
    for (const auto &key : m_engineNames.keys())
        if (!m_excludedEngines.contains(key))
            engines << key;
    QSet<int> ids;
    QSet<QString> bundles;
    for (auto it = m_storageKeys.cbegin(); it != m_storageKeys.cend(); ++it) {
        if (!m_excludedBundles.contains(it.value())) {
            ids.insert(it.key());
            bundles.insert(it.value());
        }
    }
    const bool allEngines = engines.size() == m_engineNames.size();
    const bool allBundles = bundles.size() == m_bundleNames.size();
    QMap<QString, QStringList> metadata;
    if (!allEngines)
        metadata["paintopid"] = engines;
    m_model->setAdditionalFilters(metadata, !allBundles, ids);
    m_engines->setText(allEngines ? i18n("Engines: All") : i18n("Engines: %1", engines.size()));
    m_bundles->setText(allBundles ? i18n("Bundles: All") : i18n("Bundles: %1", bundles.size()));
}
