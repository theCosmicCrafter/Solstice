/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COMPOSITION_DOCUMENT_STATE_H
#define COMPOSITION_DOCUMENT_STATE_H
#include <QObject>
#include <QTimer>
#include <kis_types.h>
class KisDocument;
// One state per document, shared by all its dockers/views; never serialized.
class CompositionDocumentState : public QObject
{
    Q_OBJECT
public:
    /// The state of @p document, created on first use and bound to @p image;
    /// null without a document (a view manager between views has none).
    static CompositionDocumentState *get(KisDocument *document, KisImageSP image);
    KisLayerCompositionSP snapshot() const
    {
        return m_snapshot;
    }
    KisLayerCompositionSP active() const
    {
        return m_active;
    }
    void apply(KisLayerCompositionSP comp);
    void setExporting(bool exporting);
    void scheduleRefresh();
Q_SIGNALS:
    void changed();

private:
    explicit CompositionDocumentState(KisDocument *document);
    void bindImage(KisImageSP image);
    void refresh();
    void synchronizeRoot();
    KisLayerCompositionSP matchingComp();
    KisImageWSP m_image;
    KisNodeWSP m_root;
    bool m_hasRoot = false;
    KisLayerCompositionSP m_snapshot;
    KisLayerCompositionSP m_active;
    KisLayerCompositionSP m_lastApplied;
    QTimer m_timer;
    int m_exports = 0;
};
#endif
