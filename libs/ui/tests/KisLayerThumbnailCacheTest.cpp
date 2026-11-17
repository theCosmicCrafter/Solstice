/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <KisIdleTasksManager.h>
#include <KisLayerThumbnailCache.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <QElapsedTimer>
#include <QPointer>
#include <QSignalSpy>
#include <kis_image.h>
#include <kis_node_juggler_compressed.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_undo_stores.h>
#include <random>
#include <simpletest.h>

// Solstice: the layer docker showed 1x1 placeholders instead of thumbnails
// after removed layers were destroyed (dead weak pointer map keys).
class KisLayerThumbnailCacheTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testDestroyedNodesKeepOtherThumbnails()
    {
        const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
        KisImageSP image = new KisImage(new KisSurrogateUndoStore, 512, 512, cs, "thumbnail cache");
        QVector<KisNodeSP> nodes;
        for (int i = 0; i < 40; ++i) {
            KisPaintLayerSP layer = new KisPaintLayer(image, QString("layer%1").arg(i), OPACITY_OPAQUE_U8, cs);
            layer->paintDevice()->fill(QRect(10 * i, 10, 50, 50), KoColor(Qt::red, cs));
            image->addNode(layer, image->root());
            nodes << layer;
        }
        image->initialRefreshGraph();
        image->waitForDone();

        KisIdleTasksManager manager;
        manager.setImage(image);
        KisLayerThumbnailCache cache;
        cache.setMaxSize(32);
        cache.setImage(image, &manager);

        auto placeholders = [&]() {
            int count = 0;
            for (KisNodeSP node : nodes) {
                if (cache.thumbnail(node).size() == QSize(1, 1)) {
                    count++;
                }
            }
            return count;
        };
        QTRY_COMPARE_WITH_TIMEOUT(placeholders(), 0, 5000);

        // The layer model notifies the removal while the node is still alive
        // (in the application the undo command holds it); it is destroyed later.
        QVector<KisNodeSP> alive;
        for (int i = 0; i < nodes.size(); ++i) {
            if (i % 3 == 1) {
                image->removeNode(nodes[i]);
                image->waitForDone();
                cache.notifyNodeRemoved(nodes[i]);
            } else {
                alive << nodes[i];
            }
        }
        nodes = alive; // the removed nodes are destroyed here

        QCOMPARE(placeholders(), 0);
    }

    /**
     * Solstice: Duplicate Layer runs in a node juggler stroke that stays open
     * for seconds, and the idle task cannot run meanwhile. The duplicate must
     * get a provisional thumbnail while that stroke is still open, and the
     * idle task must still regenerate it afterwards.
     */
    void testDuplicateGetsThumbnailWhileJugglerStrokeIsOpen()
    {
        const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
        KisImageSP image = new KisImage(new KisSurrogateUndoStore, 512, 512, cs, "duplicate thumbnail");
        KisPaintLayerSP layer = new KisPaintLayer(image, "layer", OPACITY_OPAQUE_U8, cs);
        layer->paintDevice()->fill(QRect(40, 60, 300, 200), KoColor(Qt::red, cs));
        image->addNode(layer, image->root());
        image->initialRefreshGraph();
        image->waitForDone();

        KisIdleTasksManager manager;
        manager.setImage(image);
        KisLayerThumbnailCache cache;
        cache.setMaxSize(32);
        cache.setImage(image, &manager);
        QTRY_VERIFY_WITH_TIMEOUT(cache.thumbnail(layer).size() != QSize(1, 1), 5000);

        QSignalSpy updates(&cache, SIGNAL(sigLayerThumbnailUpdated(KisNodeSP)));
        KisNodeJugglerCompressed *juggler =
            new KisNodeJugglerCompressed(kundo2_noi18n("Duplicate Nodes"), image, nullptr, 750);
        juggler->setAutoDelete(true);
        juggler->duplicateNode({layer});
        QPointer<KisNodeJugglerCompressed> jugglerGuard(juggler); // deletes itself when its stroke ends

        KisNodeSP copy;
        QVERIFY(QTest::qWaitFor(
            [&]() {
                copy = image->root()->lastChild();
                return copy && copy.data() != layer.data();
            },
            5000));
        cache.notifyNodeAdded(copy); // what the layer model does on insertion

        QTRY_VERIFY_WITH_TIMEOUT(cache.thumbnail(copy).size() != QSize(1, 1), 1000);
        QVERIFY(jugglerGuard && !jugglerGuard->isEnded()); // the juggler stroke is still open
        QCOMPARE(cache.thumbnail(copy), cache.thumbnail(layer));

        // The idle task replaces the provisional thumbnail once the stroke ends.
        auto copyUpdates = [&]() {
            int count = 0;
            for (const QList<QVariant> &args : updates) {
                if (args.first().value<KisNodeSP>() == copy)
                    count++;
            }
            return count;
        };
        QCOMPARE(copyUpdates(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(copyUpdates() >= 2, 10000);
        QCOMPARE(cache.thumbnail(copy), cache.thumbnail(layer));
    }

    /**
     * Opt-in measurement (KRITA_THUMBNAIL_BENCHMARK=1): how long a duplicated
     * layer waits for its Layers docker thumbnail, with the application's
     * duplication path (KisNodeJugglerCompressed, 750 ms) and a direct addNode().
     */
    void benchmarkDuplicateThumbnail()
    {
        if (!qEnvironmentVariableIsSet("KRITA_THUMBNAIL_BENCHMARK")) {
            QSKIP("set KRITA_THUMBNAIL_BENCHMARK=1");
        }
        for (bool f32 : {false, true}) {
            const KoColorSpace *cs = f32 ? KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", QString())
                                         : KoColorSpaceRegistry::instance()->rgb8();
            KisImageSP image = new KisImage(new KisSurrogateUndoStore, 2480, 3508, cs, "duplicate thumbnail");
            KisPaintLayerSP layer = new KisPaintLayer(image, "layer", OPACITY_OPAQUE_U8, cs);
            const QRect content(200, 300, 1800, 2600);
            std::mt19937 random(1);
            for (int y = content.top(); y < content.bottom(); y += 97) {
                for (int x = content.left(); x < content.right(); x += 89) {
                    layer->paintDevice()->fill(QRect(x, y, 60, 70), KoColor(QColor::fromRgb(random()), cs));
                }
            }
            image->addNode(layer, image->root());
            image->initialRefreshGraph();
            image->waitForDone();

            KisIdleTasksManager manager;
            manager.setImage(image);
            KisLayerThumbnailCache cache;
            cache.setMaxSize(64);
            cache.setImage(image, &manager);
            QTRY_VERIFY_WITH_TIMEOUT(cache.thumbnail(layer).size() != QSize(1, 1), 10000);

            auto newNode = [&](const QSet<KisNode *> &before) -> KisNodeSP {
                for (KisNodeSP node = image->root()->firstChild(); node; node = node->nextSibling()) {
                    if (!before.contains(node.data()))
                        return node;
                }
                return KisNodeSP();
            };
            auto existing = [&]() {
                QSet<KisNode *> nodes;
                for (KisNodeSP node = image->root()->firstChild(); node; node = node->nextSibling())
                    nodes << node.data();
                return nodes;
            };
            auto waitForThumbnail = [&](const QSet<KisNode *> &before, QElapsedTimer &timer, qint64 *added) {
                KisNodeSP node;
                if (!QTest::qWaitFor(
                        [&]() {
                            return bool(node = newNode(before));
                        },
                        10000))
                    return -1.0;
                *added = timer.elapsed();
                cache.notifyNodeAdded(node); // what the layer model does on insertion
                if (!QTest::qWaitFor(
                        [&]() {
                            return cache.thumbnail(node).size() != QSize(1, 1);
                        },
                        10000))
                    return -1.0;
                return double(timer.elapsed());
            };

            {
                const QSet<KisNode *> before = existing();
                QElapsedTimer timer;
                timer.start();
                KisNodeJugglerCompressed *juggler =
                    new KisNodeJugglerCompressed(kundo2_noi18n("Duplicate Nodes"), image, nullptr, 750);
                juggler->setAutoDelete(true);
                juggler->duplicateNode({layer});
                qint64 added = 0;
                const double shown = waitForThumbnail(before, timer, &added);
                qInfo().noquote() << QString("%1 juggler duplicate: node added after %2 ms, thumbnail after %3 ms")
                                         .arg(f32 ? "F32" : "RGBA8")
                                         .arg(added)
                                         .arg(shown);
                image->waitForDone();
            }
            {
                const QSet<KisNode *> before = existing();
                QElapsedTimer timer;
                timer.start();
                image->addNode(layer->clone(), image->root());
                qint64 added = 0;
                const double shown = waitForThumbnail(before, timer, &added);
                qInfo().noquote() << QString("%1 direct addNode: node added after %2 ms, thumbnail after %3 ms")
                                         .arg(f32 ? "F32" : "RGBA8")
                                         .arg(added)
                                         .arg(shown);
                image->waitForDone();
            }
            for (KisThumbnailBoundsMode mode : {KisThumbnailBoundsMode::Precise, KisThumbnailBoundsMode::Coarse}) {
                KisNodeSP copy = layer->clone();
                QElapsedTimer timer;
                timer.start();
                copy->createThumbnail(64, 64, Qt::KeepAspectRatio, mode);
                qInfo().noquote() << QString("%1 thumbnail of a fresh copy (%2): %3 ms")
                                         .arg(f32 ? "F32" : "RGBA8")
                                         .arg(mode == KisThumbnailBoundsMode::Precise ? "precise" : "coarse")
                                         .arg(timer.nsecsElapsed() / 1.0e6);
            }
        }
    }
};

SIMPLE_TEST_MAIN(KisLayerThumbnailCacheTest)
#include "KisLayerThumbnailCacheTest.moc"
