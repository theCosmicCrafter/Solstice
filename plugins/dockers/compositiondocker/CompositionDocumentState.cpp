/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "CompositionDocumentState.h"
#include <KisDocument.h>
#include <kis_image.h>
#include <kis_layer_composition.h>
#include <kis_node.h>
#include <kis_undo_adapter.h>

CompositionDocumentState::CompositionDocumentState(KisDocument *document)
    : QObject(document)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &CompositionDocumentState::refresh);
}
CompositionDocumentState *CompositionDocumentState::get(KisDocument *document, KisImageSP image)
{
    if (!document)
        return nullptr;
    auto *state = document->findChild<CompositionDocumentState *>(QString(), Qt::FindDirectChildrenOnly);
    if (!state)
        state = new CompositionDocumentState(document);
    if (state->m_image.data() != image.data())
        state->bindImage(image);
    return state;
}
void CompositionDocumentState::bindImage(KisImageSP image)
{
    if (m_image)
        disconnect(m_image.data(), nullptr, this, nullptr);
    m_image = image;
    m_root = nullptr;
    m_hasRoot = false;
    m_snapshot.clear();
    m_active.clear();
    m_lastApplied.clear();
    m_exports = 0;
    if (image) {
        connect(image.data(),
                &KisImage::sigNodeChanged,
                this,
                &CompositionDocumentState::scheduleRefresh,
                Qt::QueuedConnection);
        connect(image.data(),
                &KisImage::sigNodeAddedAsync,
                this,
                &CompositionDocumentState::scheduleRefresh,
                Qt::QueuedConnection);
        connect(image.data(),
                &KisImage::sigRemoveNodeAsync,
                this,
                &CompositionDocumentState::scheduleRefresh,
                Qt::QueuedConnection);
        connect(image.data(),
                &KisImage::sigLayersChangedAsync,
                this,
                &CompositionDocumentState::scheduleRefresh,
                Qt::QueuedConnection);
        connect(image.data(),
                &KisImage::sigCompositionsChanged,
                this,
                &CompositionDocumentState::scheduleRefresh,
                Qt::QueuedConnection);
    }
    scheduleRefresh();
}
void CompositionDocumentState::scheduleRefresh()
{
    if (!m_timer.isActive())
        m_timer.start();
}
KisLayerCompositionSP CompositionDocumentState::matchingComp()
{
    const auto comps = m_image->compositions();
    if (m_lastApplied && (comps.contains(m_lastApplied) || m_lastApplied == m_snapshot)
        && m_lastApplied->matchesCurrentVisibility(m_lastApplied == m_snapshot))
        return m_lastApplied;
    for (const auto &comp : comps) {
        if (comp->matchesCurrentVisibility())
            return comp;
    }
    if (m_snapshot && m_snapshot->matchesCurrentVisibility(true))
        return m_snapshot;
    return {};
}
void CompositionDocumentState::refresh()
{
    KisImageSP image = m_image;
    if (!image || m_exports)
        return;
    // A queued signal alone does not make graph traversal safe.
    if (!image->tryBarrierLock(true)) {
        scheduleRefresh();
        return;
    }
    synchronizeRoot();
    m_active = matchingComp();
    image->unlock();
    Q_EMIT changed();
}
void CompositionDocumentState::apply(KisLayerCompositionSP comp)
{
    KisImageSP image = m_image;
    if (!image || !comp || m_exports)
        return;
    image->barrierLock();
    synchronizeRoot();
    const bool restoring = comp == m_snapshot;
    if (!restoring && !image->compositions().contains(comp)) {
        image->unlock();
        return;
    }
    if (!restoring && !matchingComp()) {
        m_snapshot = toQShared(new KisLayerComposition(image, QString()));
        m_snapshot->store();
    }
    // The outer lock also covers construction of the old property values.
    image->undoAdapter()->addCommand(comp->createApplyCommand(restoring));
    m_lastApplied = comp;
    image->unlock();
    refresh();
}
void CompositionDocumentState::setExporting(bool exporting)
{
    m_exports += exporting ? 1 : -1;
    if (!m_exports)
        scheduleRefresh();
}

void CompositionDocumentState::synchronizeRoot()
{
    if (m_hasRoot && m_root.data() != m_image->root().data()) {
        m_snapshot.clear();
        m_active.clear();
        m_lastApplied.clear();
    }
    m_root = m_image->root();
    m_hasRoot = true;
}
