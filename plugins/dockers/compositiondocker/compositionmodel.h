/*
 *  SPDX-FileCopyrightText: 2012 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef COMPOSITIONMODEL_H
#define COMPOSITIONMODEL_H

#include <QModelIndex>

#include <kis_types.h>
#include <kis_layer_composition.h>

class CompositionModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    CompositionModel(QObject* parent = 0);
    ~CompositionModel() override;
    
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData ( const QModelIndex& index, const QVariant& value, int role = Qt::EditRole ) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool
    dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) override;
    Qt::DropActions supportedDropActions() const override;
    bool moveRows(const QModelIndex &sourceParent,
                  int sourceRow,
                  int count,
                  const QModelIndex &destinationParent,
                  int destinationChild) override;

    KisLayerCompositionSP compositionFromIndex(const QModelIndex& index);
    void setCompositions(QList<KisLayerCompositionSP> compositions);
    void setState(KisImageSP image, KisLayerCompositionSP snapshot, KisLayerCompositionSP active);
    bool isSnapshot(const QModelIndex &index) const;
    QModelIndex indexForComposition(KisLayerCompositionSP comp) const;

    // public Q_SLOTS:
    //     void clear();
private:
    QList<KisLayerCompositionSP> m_compositions;
    KisImageWSP m_image;
    KisLayerCompositionSP m_snapshot;
    KisLayerCompositionSP m_active;
};

#endif // TASKSETMODEL_H
