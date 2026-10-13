/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <KisIdleTasksManager.h>
#include <KisLayerThumbnailCache.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_undo_stores.h>
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
};

SIMPLE_TEST_MAIN(KisLayerThumbnailCacheTest)
#include "KisLayerThumbnailCacheTest.moc"
