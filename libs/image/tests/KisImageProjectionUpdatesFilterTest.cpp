/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>

#include <atomic>
#include <thread>

#include "kis_group_layer.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_undo_stores.h"

#include <simpletest.h>

/**
 * Regression tests for the projection updates filter stack of KisImage.
 *
 * Stroke workers add and remove filters (for example UpdateCommand of
 * KisProcessingApplicator calls disableDirtyRequests()) while other threads,
 * including the GUI thread, read the stack in refreshGraphAsync() and
 * requestProjectionUpdate(). A non-const iteration of a Qt 6 container that
 * was never allocated detaches it, which writes the container. That write
 * could drop a filter added at the same moment by a worker, and the
 * applicator's enableDirtyRequests() then crashed on an empty stack
 * (libkis Node::save() on a new image: cropImage() then
 * initialRefreshGraph()).
 */
class KisImageProjectionUpdatesFilterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testConcurrentAddAndRefresh();
    void testCropThenInitialRefresh();
};

namespace
{
KisImageSP createImage(int size)
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), size, size, cs, "filter test");
    KisPaintLayerSP layer = new KisPaintLayer(image, "layer", OPACITY_OPAQUE_U8);
    layer->paintDevice()->fill(QRect(0, 0, size, size), KoColor(Qt::red, cs));
    image->addNode(layer, image->rootLayer());
    return image;
}
} // namespace

void KisImageProjectionUpdatesFilterTest::testConcurrentAddAndRefresh()
{
    // A worker installs the first filter of a new image while the main
    // thread reads the (still unallocated) stack. The filter must survive.
    const int iterations = 300;
    int lostFilters = 0;

    for (int i = 0; i < iterations; i++) {
        KisImageSP image = createImage(64);
        image->waitForDone();

        std::atomic<int> ready{0};
        std::thread worker([&]() {
            ready.fetch_add(1);
            while (ready.load() < 2) { }
            image->disableDirtyRequests();
        });

        ready.fetch_add(1);
        while (ready.load() < 2) { }
        for (int j = 0; j < 20; j++) {
            image->refreshGraphAsync();
        }
        worker.join();

        if (!image->currentProjectionUpdatesFilter()) {
            lostFilters++;
        } else {
            image->enableDirtyRequests();
        }
        QVERIFY(!image->currentProjectionUpdatesFilter());
        image->waitForDone();
    }

    QCOMPARE(lostFilters, 0);
}

void KisImageProjectionUpdatesFilterTest::testCropThenInitialRefresh()
{
    // The sequence of libkis Node::save(): an asynchronous crop stroke on a
    // new image, immediately followed by initialRefreshGraph().
    for (int i = 0; i < 300; i++) {
        KisImageSP image = createImage(128);
        image->cropImage(QRect(16, 16, 64, 64));
        image->initialRefreshGraph();
        image->waitForDone();

        QCOMPARE(image->bounds(), QRect(0, 0, 64, 64));
        QVERIFY(!image->currentProjectionUpdatesFilter());
    }
}

SIMPLE_TEST_MAIN(KisImageProjectionUpdatesFilterTest)

#include "KisImageProjectionUpdatesFilterTest.moc"
