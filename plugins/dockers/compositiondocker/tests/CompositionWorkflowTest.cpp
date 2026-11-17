/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "CompositionDocumentState.h"
#include "compositionmodel.h"
#include <KisDocument.h>
#include <KisPart.h>
#include <KoColorSpaceRegistry.h>
#include <QAbstractItemModelTester>
#include <QMimeData>
#include <QTemporaryDir>
#include <QVersionNumber>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_kra_loader.h>
#include <kis_kra_saver.h>
#include <kis_layer_composition.h>
#include <kis_paint_layer.h>
#include <kundo2stack.h>
#include <simpletest.h>
#include <testui.h>

class CompositionWorkflowTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testModel();
    void testDocumentState();
    void testPersistence();
};

void CompositionWorkflowTest::testModel()
{
    KisImageSP image = new KisImage(nullptr, 32, 32, KoColorSpaceRegistry::instance()->rgb8(), "model");
    QList<KisLayerCompositionSP> comps;
    for (int i = 0; i < 5; ++i) {
        comps << toQShared(new KisLayerComposition(image, QString::number(i)));
        image->addComposition(comps.last());
    }
    CompositionModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    auto snapshot = toQShared(new KisLayerComposition(image, "snapshot"));
    model.setState(image, snapshot, comps[0]);
    QCOMPARE(model.rowCount(), 6);
    QVERIFY(!model.setData(model.index(0, 1), Qt::Checked, Qt::CheckStateRole));
    QVERIFY(model.setData(model.index(1, 1), Qt::Unchecked, Qt::CheckStateRole));
    QVERIFY(!comps[0]->isExportEnabled());
    // Names are edited in place; the snapshot row and other columns are not.
    QVERIFY(model.flags(model.index(1, 2)) & Qt::ItemIsEditable);
    QVERIFY(!(model.flags(model.index(0, 2)) & Qt::ItemIsEditable));
    QVERIFY(!(model.flags(model.index(1, 0)) & Qt::ItemIsEditable));
    QCOMPARE(model.data(model.index(1, 2), Qt::EditRole).toString(), QString("0"));
    QVERIFY(model.setData(model.index(1, 2), QString("  first "), Qt::EditRole));
    QCOMPARE(comps[0]->name(), QString("first"));
    QCOMPARE(model.data(model.index(1, 2)).toString(), QString("first"));
    QVERIFY(!model.setData(model.index(1, 2), QString("  "), Qt::EditRole));
    QVERIFY(!model.setData(model.index(1, 2), QString("first"), Qt::EditRole));
    QVERIFY(!model.setData(model.index(0, 2), QString("snapshot name"), Qt::EditRole));
    QCOMPARE(comps[0]->name(), QString("first"));
    QVERIFY(!model.moveRows({}, 0, 1, {}, 3));
    QVERIFY(model.moveRows({}, 1, 2, {}, 6));
    QCOMPARE(image->compositions(), (QList<KisLayerCompositionSP>{comps[2], comps[3], comps[4], comps[0], comps[1]}));
    // Move non-contiguous rows as a stable block, retaining export metadata.
    QScopedPointer<QMimeData> mime(model.mimeData({model.index(1, 2), model.index(3, 2)}));
    QVERIFY(model.dropMimeData(mime.data(), Qt::MoveAction, 6, 0, {}));
    QCOMPARE(image->compositions(), (QList<KisLayerCompositionSP>{comps[3], comps[0], comps[1], comps[2], comps[4]}));
    QVERIFY(!comps[0]->isExportEnabled());
    QCOMPARE(model.compositionFromIndex(model.index(0, 0)), snapshot);
    QVERIFY(!image->reorderCompositions({comps[0], comps[0], comps[0], comps[0], comps[0]}));
}

void CompositionWorkflowTest::testDocumentState()
{
    QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());
    KisImageSP image = new KisImage(doc->createUndoStore(), 32, 32, KoColorSpaceRegistry::instance()->rgb8(), "state");
    doc->setCurrentImage(image);
    image->barrierLock();
    KisPaintLayerSP a = new KisPaintLayer(image, "a", 255);
    KisPaintLayerSP b = new KisPaintLayer(image, "b", 255);
    image->addNode(a);
    image->addNode(b);
    auto compA = toQShared(new KisLayerComposition(image, "A"));
    b->setVisible(false);
    compA->store();
    image->addComposition(compA);
    auto compB = toQShared(new KisLayerComposition(image, "B"));
    a->setVisible(false);
    b->setVisible(true);
    compB->store();
    image->addComposition(compB);
    a->setVisible(true);
    image->unlock();
    QCOMPARE(CompositionDocumentState::get(nullptr, image), static_cast<CompositionDocumentState *>(nullptr));
    auto *state = CompositionDocumentState::get(doc.data(), image);
    QCOMPARE(state, CompositionDocumentState::get(doc.data(), image));
    state->apply(compA);
    QVERIFY(state->snapshot());
    auto snapshot = state->snapshot();
    state->apply(compB);
    QCOMPARE(state->snapshot(), snapshot);
    image->waitForDone();
    doc->undoStack()->undo();
    image->waitForDone();
    QTRY_COMPARE(state->active(), compA);
    state->apply(snapshot);
    QVERIFY(a->visible());
    QVERIFY(b->visible());
    state->apply(compB);
    QCOMPARE(state->snapshot(), snapshot);
    state->setExporting(true);
    state->apply(compA);
    QVERIFY(!a->visible());
    state->setExporting(false);
    image->barrierLock();
    KisPaintLayerSP added = new KisPaintLayer(image, "later", 255);
    image->addNode(added);
    image->unlock();
    state->apply(snapshot);
    QVERIFY(added->visible());
    // Replacing the document image invalidates transient references.
    KisImageSP replacement = image->clone();
    doc->setCurrentImage(replacement);
    QCOMPARE(state, CompositionDocumentState::get(doc.data(), replacement));
    QVERIFY(!state->snapshot());
}

void CompositionWorkflowTest::testPersistence()
{
    QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());
    doc->setFileBatchMode(true);
    KisImageSP image =
        new KisImage(doc->createUndoStore(), 32, 32, KoColorSpaceRegistry::instance()->rgb8(), "persistence");
    doc->setCurrentImage(image);
    KisPaintLayerSP layer = new KisPaintLayer(image, "paint", 255);
    image->addNode(layer);
    image->waitForDone();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    // Zero comps must still persist the document's policy.
    for (bool flag : {false, true}) {
        image->setNewLayersVisibleInCompositions(flag);
        const QString file = temp.filePath(flag ? "on.kra" : "off.kra");
        QVERIFY(doc->exportDocumentSync(file, KisDocument::nativeFormatMimeType()));
        QScopedPointer<KisDocument> loaded(KisPart::instance()->createDocument());
        loaded->setFileBatchMode(true);
        QVERIFY(loaded->loadNativeFormat(file));
        QCOMPARE(loaded->image()->newLayersVisibleInCompositions(), flag);
        QVERIFY(loaded->image()->compositions().isEmpty());
    }
    auto comp = toQShared(new KisLayerComposition(image, "saved"));
    image->barrierLock(true);
    comp->store();
    image->addComposition(comp);
    image->unlock();
    comp->setExportEnabled(false);
    const QString file = temp.filePath("comps.kra");
    QVERIFY(doc->exportDocumentSync(file, KisDocument::nativeFormatMimeType()));
    QScopedPointer<KisDocument> loaded(KisPart::instance()->createDocument());
    loaded->setFileBatchMode(true);
    QVERIFY(loaded->loadNativeFormat(file));
    QCOMPARE(loaded->image()->compositions().size(), 1);
    QCOMPARE(loaded->image()->compositions().first()->visibilityMap(), comp->visibilityMap());
    QVERIFY(!loaded->image()->compositions().first()->isExportEnabled());
    QCOMPARE(loaded->image()->compositions().first()->uuid(), comp->uuid());
    // Exercise old XML both with and without a compositions element.
    KisKraSaver saver(doc.data(), file);
    QDomDocument xml;
    auto element = saver.saveXML(xml, image);
    auto compositions = element.firstChildElement("compositions");
    compositions.removeAttribute("newLayersVisible");
    // A repeated id (a hand-edited file) and a missing one (Krita, older
    // Solstice) both get new ids.
    const auto saved = compositions.firstChildElement("composition");
    compositions.appendChild(saved.cloneNode());
    auto withoutId = saved.cloneNode().toElement();
    withoutId.removeAttribute("uuid");
    compositions.appendChild(withoutId);
    for (bool removeElement : {false, true}) {
        if (removeElement)
            element.removeChild(element.firstChildElement("compositions"));
        KisKraLoader loader(doc.data(), 2, QVersionNumber(6, 0, 0));
        KisImageSP legacy = loader.loadXML(element);
        QVERIFY(legacy);
        QVERIFY(!legacy->newLayersVisibleInCompositions());
        if (!removeElement) {
            const auto comps = legacy->compositions();
            QCOMPARE(comps.size(), 3);
            QCOMPARE(comps[0]->uuid(), comp->uuid());
            QVERIFY(!comps[1]->uuid().isNull() && comps[1]->uuid() != comp->uuid());
            QVERIFY(!comps[2]->uuid().isNull() && comps[2]->uuid() != comp->uuid());
            QVERIFY(comps[1]->uuid() != comps[2]->uuid());
        }
    }
}

KISTEST_MAIN(CompositionWorkflowTest)
#include "CompositionWorkflowTest.moc"
