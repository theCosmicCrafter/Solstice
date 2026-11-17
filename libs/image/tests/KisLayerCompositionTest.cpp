/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <KoColorSpaceRegistry.h>
#include <atomic>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_layer_composition.h>
#include <kis_paint_layer.h>
#include <kis_undo_adapter.h>
#include <kis_undo_stores.h>
#include <simpletest.h>
#include <thread>

class KisLayerCompositionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testApplyAndUndo();
    void testAddedSubtreeAndCopy();
    void testConcurrentAddition();
};

void KisLayerCompositionTest::testApplyAndUndo()
{
    auto *undo = new KisSurrogateUndoStore;
    KisImageSP image = new KisImage(undo, 32, 32, KoColorSpaceRegistry::instance()->rgb8(), "comps");
    image->barrierLock();
    KisPaintLayerSP first = new KisPaintLayer(image, "first", 255);
    image->addNode(first);
    KisLayerComposition comp(image, "saved");
    comp.store();
    KisPaintLayerSP added = new KisPaintLayer(image, "added", 255);
    image->addNode(added);
    const auto before = comp.visibilityMap();
    comp.apply();
    QVERIFY(!added->visible());
    QCOMPARE(comp.visibilityMap(), before);
    first->setVisible(false);
    added->setVisible(true);
    image->undoAdapter()->addCommand(comp.createApplyCommand());
    QVERIFY(first->visible());
    QVERIFY(!added->visible());
    undo->undo();
    QVERIFY(!first->visible());
    QVERIFY(added->visible());
    undo->redo();
    QVERIFY(first->visible());
    QVERIFY(!added->visible());
    QVERIFY(!comp.createApplyCommand());
    // A transient snapshot leaves nodes created after it unchanged.
    added->setVisible(true);
    QVERIFY(!comp.createApplyCommand(true));
    QVERIFY(added->visible());
    QCOMPARE(comp.visibilityMap(), before);
    image->unlock();
    image->waitForDone();
}

void KisLayerCompositionTest::testAddedSubtreeAndCopy()
{
    KisImageSP image = new KisImage(nullptr, 32, 32, KoColorSpaceRegistry::instance()->rgb8(), "comps");
    image->barrierLock();
    KisPaintLayerSP old = new KisPaintLayer(image, "old", 255);
    image->addNode(old);
    // Old compositions can legitimately omit existing nodes.
    auto comp = toQShared(new KisLayerComposition(image, "legacy"));
    image->addComposition(comp);
    KisGroupLayerSP group = new KisGroupLayer(image, "group", 255);
    KisPaintLayerSP child = new KisPaintLayer(image, "child", 255);
    image->addNode(child, group);
    group->setCollapsed(true);
    image->addNode(group);
    QCOMPARE(comp->visibilityMap().size(), 2);
    QVERIFY(!comp->visibilityMap().contains(old->uuid()));
    QVERIFY(comp->visibilityMap().value(child->uuid()));
    image->setNewLayersVisibleInCompositions(false);
    image->removeNode(group);
    image->addNode(group);
    QVERIFY(comp->visibilityMap().value(child->uuid()));
    KisPaintLayerSP hidden = new KisPaintLayer(image, "hidden", 255);
    image->addNode(hidden);
    QVERIFY(!comp->visibilityMap().value(hidden->uuid(), true));
    QDomDocument xml;
    auto element = xml.createElement("compositions");
    comp->save(xml, element);
    const auto values = element.firstChildElement().elementsByTagName("value");
    for (int i = 0; i < values.size(); ++i)
        QVERIFY(values.at(i).toElement().hasAttribute("collapsed"));
    QVERIFY(!comp->uuid().isNull());
    QCOMPARE(QUuid(element.firstChildElement().attribute("uuid")), comp->uuid());
    QVERIFY(toQShared(new KisLayerComposition(image, "other"))->uuid() != comp->uuid());
    KisImageSP copy = image->clone();
    QVERIFY(!copy->newLayersVisibleInCompositions());
    QCOMPARE(copy->compositions().size(), 1);
    // Copies keep the id, so a comp can be found in a copied image.
    QCOMPARE(copy->compositions().first()->uuid(), comp->uuid());
    copy->barrierLock();
    copy->compositions().first()->apply();
    QVERIFY(!copy->root()->lastChild()->visible());
    copy->unlock();
    image->unlock();
    image->waitForDone();
    copy->waitForDone();
}

void KisLayerCompositionTest::testConcurrentAddition()
{
    KisImageSP image = new KisImage(nullptr, 32, 32, KoColorSpaceRegistry::instance()->rgb8(), "comps");
    image->barrierLock();
    auto comp = toQShared(new KisLayerComposition(image, "persistent"));
    image->addComposition(comp);
    QList<KisPaintLayerSP> layers;
    for (int i = 0; i < 100; ++i)
        layers << new KisPaintLayer(image, QString::number(i), 255);
    std::atomic<bool> done{false};
    std::thread worker([&]() {
        for (const auto &layer : layers)
            image->addNode(layer);
        done = true;
    });
    do {
        auto temporary = toQShared(new KisLayerComposition(image, "temporary"));
        image->addComposition(temporary);
        const auto snapshot = image->compositions();
        for (const auto &item : snapshot)
            item->visibilityMap();
        image->removeComposition(temporary);
    } while (!done);
    worker.join();
    QCOMPARE(comp->visibilityMap().size(), 100);
    image->unlock();
    image->waitForDone();
}

SIMPLE_TEST_MAIN(KisLayerCompositionTest)
#include "KisLayerCompositionTest.moc"
