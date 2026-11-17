/*
 *  SPDX-FileCopyrightText: 2012 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "compositiondocker_dock.h"

#include <QGridLayout>
#include <QListView>
#include <QHeaderView>
#include <QPainter>
#include <QInputDialog>
#include <QThread>
#include <QAction>
#include <QStandardPaths>
#include <QMenu>
#include <QAction>
#include <QProgressDialog>

#include <klocalizedstring.h>
#include <kactioncollection.h>

#include <kis_icon.h>
#include <KoCanvasBase.h>
#include <KoFileDialog.h>

#include <KisPart.h>
#include <KisViewManager.h>
#include <kis_canvas2.h>
#include <KisKineticScroller.h>

#include <KisDocument.h>
#include <kis_group_layer.h>
#include <kis_painter.h>
#include <kis_paint_layer.h>
#include <kis_action.h>
#include <kis_action_manager.h>
#include <kis_action_registry.h>

#include <dialogs/KisAsyncAnimationFramesSaveDialog.h>
#include <animation/KisAnimationRenderingOptions.h>
#include <animation/KisAnimationRender.h>
#include <kis_image_animation_interface.h>
#include <kis_time_span.h>
#include <KisMimeDatabase.h>

#include "compositionmodel.h"
#include "CompositionDocumentState.h"
#include <QCheckBox>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QScopeGuard>
#include <QHBoxLayout>

CompositionDockerDock::CompositionDockerDock()
    : QDockWidget(i18n("Layer Comps"))
    , m_canvas(0)
{
    QWidget* widget = new QWidget(this);
    setupUi(widget);
    m_model = new CompositionModel(this);
    compositionView->setModel(m_model);
    compositionView->setRootIsDecorated(false);
    compositionView->setItemsExpandable(false);
    compositionView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    compositionView->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Only names are editable: double-click or F2 renames a comp in place.
    compositionView->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    compositionView->setDragDropMode(QAbstractItemView::InternalMove);
    compositionView->setDefaultDropAction(Qt::MoveAction);
    compositionView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    compositionView->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    compositionView->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(compositionView, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        if (index.column() == 0 && !(QApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier)))
            activated(index);
    });
    auto *overwrite = new QToolButton(widget);
    overwrite->setIcon(KisIconUtils::loadIcon("document-save"));
    overwrite->setToolTip(i18n("Overwrite Selected Layer Comps"));
    overwrite->setAutoRaise(true);
    // Keep controls together; only the list consumes extra vertical space.
    auto *buttonLayout = new QHBoxLayout;
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(2);
    for (QToolButton *button :
         {saveButton, overwrite, moveUpButton, moveDownButton, deleteButton, exportCompositions}) {
        gridLayout->removeWidget(button);
        buttonLayout->addWidget(button);
    }
    buttonLayout->addStretch(1);
    gridLayout->addLayout(buttonLayout, 2, 0, 1, 6);
    gridLayout->setRowStretch(0, 1);
    connect(overwrite, &QToolButton::clicked, this, &CompositionDockerDock::updateComposition);
    m_newLayersVisible = new QCheckBox(i18n("Show added layers in all comps"), widget);
    gridLayout->addWidget(m_newLayersVisible, 3, 0, 1, 6);
    connect(m_newLayersVisible, &QCheckBox::toggled, this, [this](bool visible) {
        if (!m_canvas)
            return;
        m_canvas->image()->setNewLayersVisibleInCompositions(visible);
        m_canvas->image()->setModifiedWithoutUndo();
    });
    compositionView->installEventFilter(this);
    deleteButton->setIcon(KisIconUtils::loadIcon("edit-delete"));
    saveButton->setIcon(KisIconUtils::loadIcon("list-add"));
    moveUpButton->setIcon(KisIconUtils::loadIcon("arrow-up"));
    moveDownButton->setIcon(KisIconUtils::loadIcon("arrow-down"));

    deleteButton->setToolTip(i18n("Delete Composition"));
    saveButton->setToolTip(i18n("New Composition"));
    exportCompositions->setToolTip(i18n("Export Composition"));
    moveUpButton->setToolTip(i18n("Apply Previous Layer Comp"));
    moveDownButton->setToolTip(i18n("Apply Next Layer Comp"));

    setWidget(widget);

    // A double-click on a name renames it; elsewhere it still applies, and so
    // does a double-click on Last Layer Display State, which has no name to edit.
    connect(compositionView, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (!(m_model->flags(index) & Qt::ItemIsEditable))
            activated(index);
    });

    compositionView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect( compositionView, SIGNAL(customContextMenuRequested(QPoint)),
             this, SLOT(customContextMenuRequested(QPoint)));

    connect( deleteButton, SIGNAL(clicked(bool)), this, SLOT(deleteClicked()));
    connect( saveButton, SIGNAL(clicked(bool)), this, SLOT(saveClicked()));
    connect( moveUpButton, SIGNAL(clicked(bool)), this, SLOT(moveCompositionUp()));
    connect( moveDownButton, SIGNAL(clicked(bool)), this, SLOT(moveCompositionDown()));

    QAction* imageAction = new QAction(KisIconUtils::loadIcon("document-export-16"), i18n("Export Images"), this);
    connect(imageAction, SIGNAL(triggered(bool)), this, SLOT(exportImageClicked()));

    QAction* animationAction = new QAction(KisIconUtils::loadIcon("addblankframe-16"), i18n("Export Animations"), this);
    connect(animationAction, SIGNAL(triggered(bool)), this, SLOT(exportAnimationClicked()));

    exportCompositions->setDefaultAction(imageAction);

    QMenu* exportMenu = new QMenu(this);
    exportMenu->addAction(imageAction);
    exportMenu->addAction(animationAction);

    exportCompositions->setMenu(exportMenu);

    connect(exportMenu, &QMenu::triggered, [this](QAction* triggered){
        exportCompositions->setDefaultAction(triggered);
    });

    saveNameEdit->setPlaceholderText(i18n("Insert Name"));

    QScroller *scroller = KisKineticScroller::createPreconfiguredScroller(compositionView);
    if (scroller) {
        connect(scroller, SIGNAL(stateChanged(QScroller::State)), this, SLOT(slotScrollerStateChanged(QScroller::State)));
    }

}

CompositionDockerDock::~CompositionDockerDock()
{

}

void CompositionDockerDock::setCanvas(KoCanvasBase * canvas)
{
    unsetCanvas();
    setEnabled(canvas != 0);

    m_canvas = dynamic_cast<KisCanvas2*>(canvas);
    if (m_canvas && m_canvas->viewManager()) {
        if (m_actions.isEmpty()) {
            KisAction *updateAction = m_canvas->viewManager()->actionManager()->createAction("update_composition");
            connect(updateAction, SIGNAL(triggered()), this, SLOT(updateComposition()));
            m_actions.append(updateAction);

            KisAction *renameAction = m_canvas->viewManager()->actionManager()->createAction("rename_composition");
            connect(renameAction, SIGNAL(triggered()), this, SLOT(renameComposition()));
            m_actions.append(renameAction);

        } else {
            Q_FOREACH (KisAction *action, m_actions) {
                m_canvas->viewManager()->actionManager()->addAction(action->objectName(), action);
            }
        }
        m_state = CompositionDocumentState::get(m_canvas->viewManager()->document(), m_canvas->image());
        if (m_state) {
            connect(m_state, &CompositionDocumentState::changed, this, &CompositionDockerDock::updateModel);
        }
        updateModel();
    }
}

void CompositionDockerDock::unsetCanvas()
{
    if (m_canvas && m_canvas->viewManager()) {
        for (KisAction *action : m_actions) {
            m_canvas->viewManager()->actionManager()->takeAction(action);
        }
    }
    if (m_state)
        disconnect(m_state, nullptr, this, nullptr);
    m_state.clear();
    setEnabled(false);
    m_canvas = 0;
    m_model->setState({}, {}, {});
}

void CompositionDockerDock::activated(const QModelIndex& index)
{
    KisLayerCompositionSP composition = m_model->compositionFromIndex(index);
    if (m_state && composition && index.column() != 1) {
        m_state->apply(composition);
        compositionView->setCurrentIndex(m_model->indexForComposition(composition));
    }
}

void CompositionDockerDock::deleteClicked()
{
    const auto selected = selectedCompositions();
    if (!m_canvas || selected.isEmpty())
        return;
    if (selected.size() > 1
        && QMessageBox::question(
               this,
               i18n("Delete Layer Comps"),
               i18np("Delete the selected layer comp?", "Delete the %1 selected layer comps?", selected.size()))
            != QMessageBox::Yes)
        return;
    KisImageSP image = m_canvas->image();
    for (const auto &comp : selected)
        image->removeComposition(comp);
    image->setModifiedWithoutUndo();
    updateModel();
}

void CompositionDockerDock::saveClicked()
{
    if (!m_canvas)
        return;
    KisImageWSP image = m_canvas->viewManager()->image();
    if (!image) return;

    // format as 001, 002 ...
    QString name = saveNameEdit->text();
    if (name.isEmpty()) {
        bool found = false;
        int i = 1;
        do {
            name = QString("%1").arg(i, 3, 10, QChar('0'));
            found = false;
            Q_FOREACH (KisLayerCompositionSP composition, m_canvas->viewManager()->image()->compositions()) {
                if (composition->name() == name) {
                    found = true;
                    break;
                }
            }
            i++;
        } while(found && i < 1000);
    }
    KisLayerCompositionSP composition(new KisLayerComposition(image, name));
    image->barrierLock(true);
    composition->store();
    image->addComposition(composition);
    image->unlock();
    saveNameEdit->clear();
    updateModel();
    compositionView->setCurrentIndex(m_model->indexForComposition(composition));
    image->setModifiedWithoutUndo();
}

void CompositionDockerDock::moveCompositionUp()
{
    applyAdjacent(-1);
}

void CompositionDockerDock::moveCompositionDown()
{
    applyAdjacent(1);
}

void CompositionDockerDock::updateModel()
{
    if (!m_canvas || !m_state)
        return;
    // While views switch or close, the view manager can have no document
    // although this docker still has the old canvas (a delayed refresh can
    // arrive then); the next setCanvas() follows.
    KisDocument *document = m_canvas->viewManager() ? m_canvas->viewManager()->document() : nullptr;
    if (!document)
        return;
    CompositionDocumentState::get(document, m_canvas->image());
    const auto selected = selectedCompositions();
    auto current = m_model->compositionFromIndex(compositionView->currentIndex());
    m_model->setState(m_canvas->image(), m_state->snapshot(), m_state->active());
    // Restore selection by identity when inserting the transient row or reordering.
    for (const auto &comp : selected) {
        const auto index = m_model->indexForComposition(comp);
        if (index.isValid())
            compositionView->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    const auto currentIndex = m_model->indexForComposition(current);
    if (currentIndex.isValid())
        compositionView->selectionModel()->setCurrentIndex(currentIndex, QItemSelectionModel::NoUpdate);
    const QSignalBlocker blocker(m_newLayersVisible);
    m_newLayersVisible->setChecked(m_canvas->image()->newLayersVisibleInCompositions());
}

void CompositionDockerDock::exportImageClicked()
{
    if (m_canvas && m_canvas->viewManager() && m_canvas->viewManager()->image()) {
        QString path;

        KoFileDialog dialog(0, KoFileDialog::OpenDirectory, "compositiondockerdock");
        dialog.setCaption(i18n("Select a Directory"));
        dialog.setDefaultDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
        path = dialog.filename();

        if (path.isNull()) return;

        if (!path.endsWith('/')) {
            path.append('/');
        }

        KisImageSP image = m_canvas->viewManager()->image();
        QString filename = m_canvas->viewManager()->document()->localFilePath();
        if (!filename.isEmpty()) {
            QFileInfo info(filename);
            path += info.completeBaseName() + '_';
        }

        KisLayerCompositionSP currentComposition = toQShared(new KisLayerComposition(image, "temp"));
        const QPointer<CompositionDocumentState> exportState = m_state;
        if (exportState)
            exportState->setExporting(true);
        const auto exportGuard = qScopeGuard([exportState]() {
            if (exportState)
                exportState->setExporting(false);
        });
        image->barrierLock(true);
        currentComposition->store();
        image->unlock();

        const QString exportWindowTitle = i18n("Exporting Compositions...");
        QSharedPointer<QProgressDialog> m_progressBar(new QProgressDialog(exportWindowTitle, i18n("Cancel"), 0, 0, this));
        const int compositionCount = image->compositions().size();
        m_progressBar->setCancelButton(0); //  TODO: This task should be threaded in the future so that canceling is possible,
                                           // Additionally, we should also be copying the image and working off that copy to
                                           // improve runtime performance and allow for the current krita instance to work
                                           // without locking up. **For now, we will simply make this operation uncancelable.**
        m_progressBar->setMaximum(compositionCount);
        m_progressBar->setMinimum(0);
        m_progressBar->setValue(0);
        int compositionsExported = 0;

        Q_FOREACH (KisLayerCompositionSP composition, image->compositions()) {
            if (m_progressBar->wasCanceled()) {
                break;
            }

            if (!composition->isExportEnabled()) {
                compositionsExported++;
                continue;
            }

            if (m_progressBar->isHidden()) {
                m_progressBar->show();
            }

            m_progressBar->setLabelText(i18n("Exporting composition: %1...", composition->name()));

            // TODO: Extension of above, but this algorithm is highly inefficient.
            // We should simply a) Deep Copy Image, b) Apply Composition, c) RefreshImageGraph,
            // d) Export Image.
            // This task can then be threaded to work in parallel.
            image->barrierLock();
            composition->apply();
            image->unlock();
            image->refreshGraphAsync();
            image->waitForDone();

            QRect r = image->bounds();

            KisDocument *d = KisPart::instance()->createDocument();

            KisImageSP dst = new KisImage(d->createUndoStore(), r.width(), r.height(), image->colorSpace(), composition->name());
            dst->setResolution(image->xRes(), image->yRes());
            d->setCurrentImage(dst);
            KisPaintLayer* paintLayer = new KisPaintLayer(dst, "projection", OPACITY_OPAQUE_U8);
            KisPainter gc(paintLayer->paintDevice());
            gc.bitBlt(QPoint(0, 0), image->rootLayer()->projection(), r);
            dst->addNode(paintLayer, dst->rootLayer(), KisLayerSP(0));

            dst->refreshGraphAsync();
            dst->waitForDone();

            d->setFileBatchMode(true);

            d->exportDocumentSync(path + composition->name() + ".png", "image/png");
            compositionsExported++;
            m_progressBar->setValue(compositionsExported);
            d->deleteLater();
        }

        image->barrierLock();
        currentComposition->apply();
        image->unlock();

        image->refreshGraphAsync();
        image->waitForDone();
    }

}

void CompositionDockerDock::exportAnimationClicked()
{
    KisConfig cfg(true);
    KisPropertiesConfigurationSP settings = cfg.exportConfiguration("ANIMATION_EXPORT");
    KisAnimationRenderingOptions exportOptions;
    exportOptions.fromProperties(settings);


    if (m_canvas &&
        m_canvas->viewManager() &&
        m_canvas->viewManager()->image() &&
        m_canvas->viewManager()->image()->animationInterface() &&
        m_canvas->viewManager()->document()) {

        QString path;

        KoFileDialog dialog(0, KoFileDialog::OpenDirectory, "compositiondockerdock");
        dialog.setCaption(i18n("Select a Directory"));
        dialog.setDefaultDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
        path = dialog.filename();

        if (path.isNull()) return;

        if (!path.endsWith('/')) {
            path.append('/');
        }

        KisImageSP image = m_canvas->viewManager()->image();
        QString filename = m_canvas->viewManager()->document()->localFilePath();
        if (!filename.isEmpty()) {
            QFileInfo info(filename);
            path += info.completeBaseName();
        }

        KisLayerCompositionSP currentComposition = toQShared(new KisLayerComposition(image, "temp"));
        const QPointer<CompositionDocumentState> exportState = m_state;
        if (exportState)
            exportState->setExporting(true);
        const auto exportGuard = qScopeGuard([exportState]() {
            if (exportState)
                exportState->setExporting(false);
        });
        image->barrierLock(true);
        currentComposition->store();
        image->unlock();

        const QString videoExtension = KisMimeDatabase::suffixesForMimeType(exportOptions.videoMimeType).first();

        Q_FOREACH (KisLayerCompositionSP composition, image->compositions()) {
            if(!composition->isExportEnabled())
                continue;

            image->barrierLock();
            composition->apply();
            image->unlock();
            image->refreshGraphAsync();
            image->waitForDone();

            KisTimeSpan range = image->animationInterface()->documentPlaybackRange();

            exportOptions.firstFrame = range.start();
            exportOptions.lastFrame = range.end();
            exportOptions.width = image->width();
            exportOptions.height = image->height();
            exportOptions.basename = QString("frame");
            exportOptions.videoFileName = QString("%1/%2/video.%3").arg(path, composition->name(), videoExtension);
            exportOptions.directory = QString("%1/%2").arg(path, composition->name());
            exportOptions.wantsOnlyUniqueFrameSequence = true;

            bool success = KisAnimationRender::render(m_canvas->viewManager()->document(), m_canvas->viewManager(), exportOptions);

            if (!success) break;
        }

        image->barrierLock();
        currentComposition->apply();
        image->unlock();
        image->refreshGraphAsync();
        image->waitForDone();
    }
}

bool CompositionDockerDock::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::KeyPress ) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (obj == compositionView && keyEvent->modifiers() == Qt::NoModifier
            && (keyEvent->key() == Qt::Key_Up || keyEvent->key() == Qt::Key_Down)) {
            // new index will be set after the method is called
            QTimer::singleShot(0, this, SLOT(activateCurrentIndex()));
        }
        return false;
    } else {
        return QObject::eventFilter(obj, event);
    }
}

void CompositionDockerDock::activateCurrentIndex()
{
    QModelIndex index = compositionView->currentIndex();
    if (index.isValid()) {
        activated(index.siblingAtColumn(2));
    }
}

void CompositionDockerDock::customContextMenuRequested(QPoint pos)
{
    if (m_actions.isEmpty()) return;

    QMenu menu;
    Q_FOREACH (KisAction *action, m_actions) {
        menu.addAction(action);

    }
    menu.exec(compositionView->mapToGlobal(pos));
}

void CompositionDockerDock::updateComposition()
{
    const auto selected = selectedCompositions();
    if (!m_canvas || selected.isEmpty())
        return;
    if (selected.size() > 1
        && QMessageBox::question(this,
                                 i18n("Overwrite Layer Comps"),
                                 i18np("Overwrite the selected layer comp?",
                                       "Overwrite the %1 selected layer comps with the current visibility?",
                                       selected.size()))
            != QMessageBox::Yes)
        return;
    KisImageSP image = m_canvas->image();
    image->barrierLock(true);
    for (const auto &comp : selected)
        comp->store();
    image->unlock();
    image->setModifiedWithoutUndo();
    Q_EMIT image->sigCompositionsChanged();
    updateModel();
}

void CompositionDockerDock::renameComposition()
{
    dbgKrita << "rename";
    QModelIndex index = compositionView->currentIndex();
    if (m_canvas && m_canvas->viewManager() && m_canvas->viewManager()->image() && index.isValid()
        && !m_model->isSnapshot(index)) {
        KisLayerCompositionSP composition = m_model->compositionFromIndex(index);
        bool ok;
        QString name = QInputDialog::getText(this, i18n("Rename Composition"),
                                             i18n("New Name:"), QLineEdit::Normal,
                                             composition->name(), &ok);
        if (ok && !name.isEmpty()) {
            composition->setName(name);
            m_canvas->image()->setModifiedWithoutUndo();
            Q_EMIT m_canvas->image()->sigCompositionsChanged();
            updateModel();
        }
    }
}

QList<KisLayerCompositionSP> CompositionDockerDock::selectedCompositions() const
{
    QList<KisLayerCompositionSP> result;
    for (const auto &index : compositionView->selectionModel()->selectedRows(2)) {
        if (!m_model->isSnapshot(index)) {
            auto comp = m_model->compositionFromIndex(index);
            if (comp)
                result << comp;
        }
    }
    return result;
}

void CompositionDockerDock::applyAdjacent(int direction)
{
    if (!m_canvas)
        return;
    const auto comps = m_canvas->image()->compositions();
    if (comps.isEmpty())
        return;
    const auto current = m_model->compositionFromIndex(compositionView->currentIndex());
    const int currentRow = comps.indexOf(current);
    const int row = currentRow < 0 ? (direction > 0 ? 0 : comps.size() - 1) : currentRow + direction;
    if (row >= 0 && row < comps.size())
        activated(m_model->indexForComposition(comps.at(row)));
}
