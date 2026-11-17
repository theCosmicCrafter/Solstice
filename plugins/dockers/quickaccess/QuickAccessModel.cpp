/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessModel.h"

#include <QHash>
#include <klocalizedstring.h>

namespace QuickAccess
{

QString itemTypeToString(ItemType type)
{
    switch (type) {
    case ItemType::Brush:
        return QStringLiteral("brush");
    case ItemType::Action:
        return QStringLiteral("action");
    case ItemType::Label:
        return QStringLiteral("label");
    case ItemType::Separator:
        return QStringLiteral("separator");
    case ItemType::DockerToggle:
        return QStringLiteral("docker_toggle");
    case ItemType::Color:
        return QStringLiteral("color");
    case ItemType::Script:
        return QStringLiteral("script");
    case ItemType::BrushSize:
        return QStringLiteral("brush_size");
    case ItemType::BrushBlendMode:
        return QStringLiteral("brush_blend_mode");
    }
    return QString();
}

bool itemTypeFromString(const QString &value, ItemType *type)
{
    static const QHash<QString, ItemType> types{
        {QStringLiteral("brush"), ItemType::Brush},
        {QStringLiteral("action"), ItemType::Action},
        {QStringLiteral("label"), ItemType::Label},
        {QStringLiteral("separator"), ItemType::Separator},
        {QStringLiteral("docker_toggle"), ItemType::DockerToggle},
        {QStringLiteral("color"), ItemType::Color},
        {QStringLiteral("script"), ItemType::Script},
        {QStringLiteral("brush_size"), ItemType::BrushSize},
        {QStringLiteral("brush_blend_mode"), ItemType::BrushBlendMode},
    };
    const auto it = types.constFind(value);
    if (it == types.constEnd() || !type) {
        return false;
    }
    *type = it.value();
    return true;
}

void Item::normalize()
{
    rowSpan = qMax(1, rowSpan);
    columnSpan = qMax(1, columnSpan);

    if (type == ItemType::Brush) {
        // A brush shown as a stroke preview takes two cells, an icon one.
        rowSpan = 1;
        columnSpan = payload.value(QStringLiteral("display")).toString() == QStringLiteral("stroke") ? 2 : 1;
    } else if (type == ItemType::Color || type == ItemType::BrushSize) {
        rowSpan = 1;
        columnSpan = 1;
    } else if (type == ItemType::BrushBlendMode) {
        rowSpan = 1;
        columnSpan = 2;
    } else if (type == ItemType::Separator) {
        if (payload.value(QStringLiteral("orientation")).toString() == QStringLiteral("vertical")) {
            columnSpan = 1;
        } else {
            rowSpan = 1;
        }
    }
}

int Item::right() const
{
    return column + columnSpan;
}

int Item::bottom() const
{
    return row + rowSpan;
}

QJsonObject Item::toJson() const
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("type"), itemTypeToString(type)},
        {QStringLiteral("row"), row},
        {QStringLiteral("col"), column},
        {QStringLiteral("row_span"), rowSpan},
        {QStringLiteral("col_span"), columnSpan},
        {QStringLiteral("payload"), payload},
    };
}

bool Item::fromJson(const QJsonObject &object, Item *item, QString *error)
{
    if (!item || object.value(QStringLiteral("id")).toString().isEmpty()) {
        if (error)
            *error = i18n("A Quick Access item has no id.");
        return false;
    }

    Item parsed;
    parsed.id = object.value(QStringLiteral("id")).toString();
    if (!itemTypeFromString(object.value(QStringLiteral("type")).toString(), &parsed.type)) {
        if (error)
            *error = i18n("Item '%1' has an unsupported type.", parsed.id);
        return false;
    }
    parsed.row = object.value(QStringLiteral("row")).toInt();
    parsed.column = object.value(QStringLiteral("col")).toInt();
    parsed.rowSpan = object.value(QStringLiteral("row_span")).toInt(1);
    parsed.columnSpan = object.value(QStringLiteral("col_span")).toInt(1);
    parsed.payload = object.value(QStringLiteral("payload")).toObject();
    parsed.normalize();
    *item = parsed;
    return true;
}

bool Item::operator==(const Item &other) const
{
    return id == other.id && type == other.type && row == other.row && column == other.column
        && rowSpan == other.rowSpan && columnSpan == other.columnSpan && payload == other.payload;
}

void Grid::normalize()
{
    columns = qMax(1, columns);
    for (Item &item : items) {
        item.normalize();
        if (item.type == ItemType::Brush)
            item.columnSpan = qMin(item.columnSpan, columns);
    }
}

QJsonObject Grid::toJson() const
{
    QJsonArray serializedItems;
    for (const Item &item : items)
        serializedItems.append(item.toJson());
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("columns"), columns},
        {QStringLiteral("items"), serializedItems},
    };
}

bool Grid::fromJson(const QJsonObject &object, Grid *grid, QString *error)
{
    if (!grid || object.value(QStringLiteral("id")).toString().isEmpty()) {
        if (error)
            *error = i18n("A Quick Access grid has no id.");
        return false;
    }
    Grid parsed;
    parsed.id = object.value(QStringLiteral("id")).toString();
    parsed.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Grid"));
    parsed.columns = object.value(QStringLiteral("columns")).toInt(8);
    for (const QJsonValue &value : object.value(QStringLiteral("items")).toArray()) {
        Item item;
        if (!value.isObject() || !Item::fromJson(value.toObject(), &item, error))
            return false;
        parsed.items.append(item);
    }
    parsed.normalize();
    *grid = parsed;
    return true;
}

bool Grid::operator==(const Grid &other) const
{
    return id == other.id && name == other.name && columns == other.columns && items == other.items;
}

QJsonObject Tab::toJson() const
{
    QJsonArray serializedGrids;
    for (const Grid &grid : grids)
        serializedGrids.append(grid.toJson());
    return {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("grids"), serializedGrids}};
}

bool Tab::fromJson(const QJsonObject &object, Tab *tab, QString *error)
{
    if (!tab || object.value(QStringLiteral("id")).toString().isEmpty()) {
        if (error)
            *error = i18n("A Quick Access tab has no id.");
        return false;
    }
    Tab parsed;
    parsed.id = object.value(QStringLiteral("id")).toString();
    parsed.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Tab"));
    for (const QJsonValue &value : object.value(QStringLiteral("grids")).toArray()) {
        Grid grid;
        if (!value.isObject() || !Grid::fromJson(value.toObject(), &grid, error))
            return false;
        parsed.grids.append(grid);
    }
    *tab = parsed;
    return true;
}

bool Tab::operator==(const Tab &other) const
{
    return id == other.id && name == other.name && grids == other.grids;
}

Document Document::createDefault()
{
    Grid grid;
    grid.id = QStringLiteral("main-grid");
    grid.name = QStringLiteral("Main");
    Tab tab;
    tab.id = QStringLiteral("main-tab");
    tab.name = QStringLiteral("Main");
    tab.grids.append(grid);
    Document document;
    document.activeTabId = tab.id;
    document.tabs.append(tab);
    return document;
}

QJsonObject Document::toJson() const
{
    QJsonArray serializedTabs;
    for (const Tab &tab : tabs)
        serializedTabs.append(tab.toJson());
    return {
        {QStringLiteral("formatVersion"), formatVersion},
        {QStringLiteral("activeTabId"), activeTabId},
        {QStringLiteral("tabs"), serializedTabs},
        {QStringLiteral("aliases"), aliases},
        {QStringLiteral("gesturePages"), gesturePages},
    };
}

bool Document::fromJson(const QJsonObject &object, Document *document, QString *error)
{
    if (!document)
        return false;
    const int version = object.contains(QStringLiteral("formatVersion"))
        ? object.value(QStringLiteral("formatVersion")).toInt()
        : object.value(QStringLiteral("version")).toInt(1);
    if (version < 1 || version > CurrentFormatVersion) {
        if (error)
            *error = i18n("Unsupported Quick Access format version %1.", version);
        return false;
    }

    Document parsed;
    parsed.formatVersion = version;
    parsed.activeTabId = object.contains(QStringLiteral("activeTabId"))
        ? object.value(QStringLiteral("activeTabId")).toString()
        : object.value(QStringLiteral("active_tab_id")).toString();
    parsed.aliases = object.value(QStringLiteral("aliases")).toObject();
    parsed.gesturePages = object.value(QStringLiteral("gesturePages")).toArray();
    for (const QJsonValue &value : object.value(QStringLiteral("tabs")).toArray()) {
        Tab tab;
        if (!value.isObject() || !Tab::fromJson(value.toObject(), &tab, error))
            return false;
        parsed.tabs.append(tab);
    }
    if (parsed.tabs.isEmpty()) {
        if (error)
            *error = i18n("A Quick Access profile must contain at least one tab.");
        return false;
    }
    if (parsed.activeTabId.isEmpty())
        parsed.activeTabId = parsed.tabs.constFirst().id;
    *document = parsed;
    return true;
}

bool Document::operator==(const Document &other) const
{
    return formatVersion == other.formatVersion && activeTabId == other.activeTabId && tabs == other.tabs
        && aliases == other.aliases && gesturePages == other.gesturePages;
}

} // namespace QuickAccess
