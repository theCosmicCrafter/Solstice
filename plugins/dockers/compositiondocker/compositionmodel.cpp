/*
 *  SPDX-FileCopyrightText: 2012 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "compositionmodel.h"
#include <kis_icon.h>
#include <QMimeData>
#include <QDataStream>
#include <QIODevice>
#include <QFont>
#include <klocalizedstring.h>
#include <algorithm>

namespace
{
const QString compMime = QStringLiteral("application/x-solstice-layer-comps");
}
CompositionModel::CompositionModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}
CompositionModel::~CompositionModel() = default;
QVariant CompositionModel::data(const QModelIndex &index, int role) const
{
    auto comp = const_cast<CompositionModel *>(this)->compositionFromIndex(index);
    if (!comp)
        return {};
    if (role == Qt::DisplayRole && index.column() == 2)
        return isSnapshot(index) ? i18n("Last Layer Display State") : comp->name();
    if (role == Qt::EditRole && index.column() == 2 && !isSnapshot(index))
        return comp->name();
    if (role == Qt::DecorationRole && index.column() == 0)
        return KisIconUtils::loadIcon(comp == m_active ? "visible" : "novisible");
    if (role == Qt::CheckStateRole && index.column() == 1 && !isSnapshot(index))
        return comp->isExportEnabled() ? Qt::Checked : Qt::Unchecked;
    if (role == Qt::FontRole && isSnapshot(index)) {
        QFont font;
        font.setItalic(true);
        return font;
    }
    if (role == Qt::ToolTipRole) {
        if (index.column() == 0)
            return i18n("Apply this layer comp");
        if (index.column() == 1 && !isSnapshot(index))
            return i18n("Include in export");
        return isSnapshot(index)
            ? i18n("Restore the last custom visibility state. Newly added layers are left unchanged.")
            : i18n("Double-click to rename. Ctrl/Shift-click to select several layer comps.");
    }
    return {};
}
bool CompositionModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    auto comp = compositionFromIndex(index);
    if (!comp || isSnapshot(index) || !m_image)
        return false;
    // Renaming in place, like the Rename Composition action: not undoable,
    // and an empty name keeps the old one.
    if (index.column() == 2 && role == Qt::EditRole) {
        const QString name = value.toString().trimmed();
        if (name.isEmpty() || name == comp->name())
            return false;
        comp->setName(name);
        m_image->setModifiedWithoutUndo();
        Q_EMIT dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
        Q_EMIT m_image->sigCompositionsChanged();
        return true;
    }
    if (index.column() != 1 || role != Qt::CheckStateRole)
        return false;
    const bool enabled = value.toInt() == Qt::Checked;
    if (comp->isExportEnabled() == enabled)
        return false;
    comp->setExportEnabled(enabled);
    m_image->setModifiedWithoutUndo();
    Q_EMIT dataChanged(index, index, {Qt::CheckStateRole});
    Q_EMIT m_image->sigCompositionsChanged();
    return true;
}
QVariant CompositionModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    return section == 0 ? i18n("Apply") : section == 1 ? i18n("Export") : i18n("Layer Comp");
}
int CompositionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_compositions.size() + (m_snapshot ? 1 : 0);
}
int CompositionModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : 3;
}
Qt::ItemFlags CompositionModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;
    auto flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    if (!isSnapshot(index)) {
        flags |= Qt::ItemIsDragEnabled;
        if (index.column() == 1)
            flags |= Qt::ItemIsUserCheckable;
        if (index.column() == 2)
            flags |= Qt::ItemIsEditable;
    }
    return flags;
}
KisLayerCompositionSP CompositionModel::compositionFromIndex(const QModelIndex &index)
{
    if (!index.isValid() || index.row() >= rowCount() || index.row() < 0)
        return {};
    if (isSnapshot(index))
        return m_snapshot;
    return m_compositions.at(index.row() - (m_snapshot ? 1 : 0));
}
bool CompositionModel::isSnapshot(const QModelIndex &index) const
{
    return m_snapshot && index.isValid() && index.row() == 0;
}
QModelIndex CompositionModel::indexForComposition(KisLayerCompositionSP comp) const
{
    if (!comp)
        return {};
    if (comp == m_snapshot)
        return index(0, 2);
    const int row = m_compositions.indexOf(comp);
    return row < 0 ? QModelIndex() : index(row + (m_snapshot ? 1 : 0), 2);
}
void CompositionModel::setCompositions(QList<KisLayerCompositionSP> compositions)
{
    if (m_compositions == compositions) {
        if (rowCount())
            Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, 2));
        return;
    }
    beginResetModel();
    m_compositions = compositions;
    endResetModel();
}
void CompositionModel::setState(KisImageSP image, KisLayerCompositionSP snapshot, KisLayerCompositionSP active)
{
    if (m_image.data() != image.data() || m_snapshot != snapshot) {
        beginResetModel();
        m_image = image;
        m_snapshot = snapshot;
        m_active = active;
        m_compositions = image ? image->compositions() : QList<KisLayerCompositionSP>();
        endResetModel();
    } else {
        m_active = active;
        setCompositions(image ? image->compositions() : QList<KisLayerCompositionSP>());
    }
}
QStringList CompositionModel::mimeTypes() const
{
    return {compMime};
}
Qt::DropActions CompositionModel::supportedDropActions() const
{
    return Qt::MoveAction;
}
QMimeData *CompositionModel::mimeData(const QModelIndexList &indexes) const
{
    QList<int> rows;
    for (const auto &index : indexes) {
        if (index.isValid() && !isSnapshot(index) && !rows.contains(index.row()))
            rows << index.row();
    }
    std::sort(rows.begin(), rows.end());
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << quint64(reinterpret_cast<quintptr>(this)) << rows;
    auto *data = new QMimeData();
    data->setData(compMime, bytes);
    return data;
}
bool CompositionModel::dropMimeData(const QMimeData *data,
                                    Qt::DropAction action,
                                    int row,
                                    int,
                                    const QModelIndex &parent)
{
    if (action == Qt::IgnoreAction)
        return true;
    if (action != Qt::MoveAction || !m_image || parent.isValid() || !data->hasFormat(compMime))
        return false;
    QByteArray bytes = data->data(compMime);
    QDataStream stream(&bytes, QIODevice::ReadOnly);
    quint64 owner;
    QList<int> rows;
    stream >> owner >> rows;
    if (stream.status() != QDataStream::Ok || owner != quint64(reinterpret_cast<quintptr>(this)) || rows.isEmpty())
        return false;
    const int offset = m_snapshot ? 1 : 0;
    if (row == -1)
        row = rowCount();
    if (row < offset || row > rowCount())
        return false;
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    QList<KisLayerCompositionSP> moved;
    auto order = m_compositions;
    int target = row - offset;
    for (int source : rows) {
        if (source < offset || source >= rowCount())
            return false;
        moved << m_compositions.at(source - offset);
        if (source < row)
            --target;
    }
    for (const auto &comp : moved)
        order.removeOne(comp);
    for (const auto &comp : moved)
        order.insert(target++, comp);
    if (!m_image->reorderCompositions(order))
        return false;
    m_image->setModifiedWithoutUndo();
    setCompositions(order);
    return true;
}
bool CompositionModel::moveRows(const QModelIndex &sourceParent,
                                int sourceRow,
                                int count,
                                const QModelIndex &destinationParent,
                                int destinationChild)
{
    const int offset = m_snapshot ? 1 : 0;
    if (!m_image || sourceParent.isValid() || destinationParent.isValid() || count <= 0 || sourceRow < offset
        || sourceRow + count > rowCount() || destinationChild < offset || destinationChild > rowCount()
        || (destinationChild >= sourceRow && destinationChild <= sourceRow + count))
        return false;
    auto order = m_compositions;
    const auto moved = order.mid(sourceRow - offset, count);
    order.erase(order.begin() + sourceRow - offset, order.begin() + sourceRow - offset + count);
    int target = destinationChild - offset - (destinationChild > sourceRow ? count : 0);
    for (const auto &comp : moved)
        order.insert(target++, comp);
    if (!beginMoveRows({}, sourceRow, sourceRow + count - 1, {}, destinationChild))
        return false;
    m_compositions = order;
    endMoveRows();
    m_image->reorderCompositions(order);
    m_image->setModifiedWithoutUndo();
    return true;
}
