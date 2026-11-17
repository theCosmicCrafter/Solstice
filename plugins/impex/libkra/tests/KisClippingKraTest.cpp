/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <simpletest.h>

#include <QObject>
#include <QTemporaryDir>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>

#include <KisDocument.h>
#include <KisPart.h>
#include <testui.h>

#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"

class KisClippingKraTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testRoundTrip();
};

namespace
{
KisLayerSP findLayer(KisImageSP image, const QString &name)
{
    for (KisNodeSP node = image->root()->firstChild(); node; node = node->nextSibling()) {
        if (node->name() == name) {
            return qobject_cast<KisLayer *>(node.data());
        }
    }
    return KisLayerSP();
}
} // namespace

void KisClippingKraTest::testRoundTrip()
{
    // Solstice: Clipping Mask is saved as the layer attribute clipped="1"
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("clipping.kra"));
    const KoColorSpace *rgb8 = KoColorSpaceRegistry::instance()->rgb8();
    QByteArray savedPixels;

    {
        QScopedPointer<KisDocument> document(KisPart::instance()->createDocument());
        // no dialogs: a failed save or load would otherwise block on a message box
        document->setFileBatchMode(true);
        KisImageSP image = new KisImage(document->createUndoStore(), 64, 64, rgb8, "clipping kra");

        auto addLayer = [&](const QString &name, const QRect &rect, const QColor &color) {
            KisPaintLayerSP layer = new KisPaintLayer(image, name, OPACITY_OPAQUE_U8, rgb8);
            layer->paintDevice()->fill(rect, KoColor(color, rgb8));
            image->addNode(layer, image->root(), image->root()->lastChild());
            return layer;
        };
        addLayer("base", QRect(10, 10, 30, 30), Qt::red);
        addLayer("clip", QRect(0, 0, 64, 20), Qt::blue)->setClipped(true);
        addLayer("plain", QRect(40, 40, 20, 20), Qt::green);

        document->setCurrentImage(image);
        image->initialRefreshGraph();
        image->waitForDone();
        savedPixels.resize(64 * 64 * 4);
        image->projection()->readBytes(reinterpret_cast<quint8 *>(savedPixels.data()), image->bounds());

        QVERIFY(document->exportDocumentSync(path, KisDocument::nativeFormatMimeType()));
    }

    QScopedPointer<KisDocument> document(KisPart::instance()->createDocument());
    document->setFileBatchMode(true);
    QVERIFY(document->loadNativeFormat(path));
    KisImageSP image = document->image();
    QVERIFY(image);

    KisLayerSP base = findLayer(image, "base");
    KisLayerSP clip = findLayer(image, "clip");
    KisLayerSP plain = findLayer(image, "plain");
    QVERIFY(base && clip && plain);
    QVERIFY(clip->isClipped());
    QVERIFY(!base->isClipped());
    QVERIFY(!plain->isClipped());

    image->initialRefreshGraph();
    image->waitForDone();
    QByteArray loadedPixels(64 * 64 * 4, '\0');
    image->projection()->readBytes(reinterpret_cast<quint8 *>(loadedPixels.data()), image->bounds());
    QCOMPARE(loadedPixels, savedPixels);
}

KISTEST_MAIN(KisClippingKraTest)

#include "KisClippingKraTest.moc"
