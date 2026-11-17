/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_view_res_vector_compositor_test.h"

#include <testui.h>

#include <QImage>

#include <KoColorBackground.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <KoPathShape.h>

#include <KisDocument.h>
#include <KisPart.h>

#include <kis_group_layer.h>
#include <kis_paint_layer.h>
#include <kis_types.h>

#include "canvas/kis_view_res_vector_compositor.h"
#include "kis_shape_layer.h"

#include <testutil.h>

namespace
{

QImage plainConvert(KisPaintDeviceSP device, const QRect &rc)
{
    return device->convertToQImage(0,
                                   rc,
                                   KoColorConversionTransformation::internalRenderingIntent(),
                                   KoColorConversionTransformation::internalConversionFlags());
}

KisShapeLayer *makeRectShapeLayer(KisDocument *doc,
                                  KisImageWSP image,
                                  const QString &name,
                                  quint8 opacity,
                                  const QRectF &rect,
                                  const QColor &color)
{
    KisShapeLayer *shapeLayer = new KisShapeLayer(doc->shapeController(), image, name, opacity);

    KoPathShape *path = new KoPathShape();
    path->setShapeId(KoPathShapeId);
    path->moveTo(rect.topLeft());
    path->lineTo(rect.bottomLeft());
    path->lineTo(rect.bottomRight());
    path->lineTo(rect.topRight());
    path->close();
    path->normalize();
    path->setBackground(toQShared(new KoColorBackground(color)));
    path->setZIndex(0);
    shapeLayer->addShape(path);

    return shapeLayer;
}

void fillDeviceRect(KisPaintDeviceSP device, const QRect &rc, const QColor &color)
{
    device->fill(rc, KoColor(color, device->colorSpace()));
}

} // namespace

void KisViewResVectorCompositorTest::testUnblendSourceOver()
{
    // D = S + B*(1-a_s)  =>  B = (D - S)/(1 - a_s)
    QImage backdrop(4, 4, QImage::Format_ARGB32_Premultiplied);
    QImage source(4, 4, QImage::Format_ARGB32_Premultiplied);
    QImage display(4, 4, QImage::Format_ARGB32_Premultiplied);

    backdrop.fill(qRgba(64, 128, 200, 255));
    source.fill(qRgba(200, 40, 10, 128));

    // composite S over B in premultiplied space
    for (int y = 0; y < 4; y++) {
        QRgb *d = reinterpret_cast<QRgb *>(display.scanLine(y));
        const QRgb *b = reinterpret_cast<const QRgb *>(backdrop.constScanLine(y));
        const QRgb *s = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < 4; x++) {
            const qreal sa = qAlpha(s[x]) / 255.0;
            d[x] = qRgba(qBound(0, qRound(qRed(s[x]) + qRed(b[x]) * (1 - sa)), 255),
                         qBound(0, qRound(qGreen(s[x]) + qGreen(b[x]) * (1 - sa)), 255),
                         qBound(0, qRound(qBlue(s[x]) + qBlue(b[x]) * (1 - sa)), 255),
                         qBound(0, qRound(qAlpha(s[x]) + qAlpha(b[x]) * (1 - sa)), 255));
        }
    }

    KisViewResVectorCompositor::unblendSourceOver(display, source);

    for (int y = 0; y < 4; y++) {
        const QRgb *d = reinterpret_cast<const QRgb *>(display.constScanLine(y));
        const QRgb *b = reinterpret_cast<const QRgb *>(backdrop.constScanLine(y));
        for (int x = 0; x < 4; x++) {
            QVERIFY2(qAbs(qRed(d[x]) - qRed(b[x])) <= 2, "red channel drift");
            QVERIFY2(qAbs(qGreen(d[x]) - qGreen(b[x])) <= 2, "green channel drift");
            QVERIFY2(qAbs(qBlue(d[x]) - qBlue(b[x])) <= 2, "blue channel drift");
            QVERIFY2(qAbs(qAlpha(d[x]) - qAlpha(b[x])) <= 2, "alpha channel drift");
        }
    }

    // opaque source: backdrop is fully replaced
    source.fill(qRgba(1, 2, 3, 255));
    QImage covered(4, 4, QImage::Format_ARGB32_Premultiplied);
    covered.fill(qRgba(90, 90, 90, 255));
    KisViewResVectorCompositor::unblendSourceOver(covered, source);
    QCOMPARE(covered.pixel(0, 0), QRgb(0));

    // transparent source: backdrop untouched
    source.fill(0);
    QImage untouched(4, 4, QImage::Format_ARGB32_Premultiplied);
    untouched.fill(qRgba(11, 22, 33, 44));
    KisViewResVectorCompositor::unblendSourceOver(untouched, source);
    QCOMPARE(untouched.pixel(2, 2), qRgba(11, 22, 33, 44));
}

void KisViewResVectorCompositorTest::testCollectUnits()
{
    using namespace TestUtil;

    QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());
    const QRect refRect(0, 0, 64, 64);
    MaskParent p(refRect);
    p.image->setResolution(1.0, 1.0);
    doc->setCurrentImage(p.image);

    KisShapeLayerSP shapeLayer = makeRectShapeLayer(doc.data(), p.image, "shape", 255, QRectF(5, 5, 30, 30), Qt::red);

    p.image->addNode(shapeLayer);
    shapeLayer->setDirty();
    p.waitForImageAndShapeLayers();

    const QVector<KisViewResVectorCompositor::Unit> units = KisViewResVectorCompositor::collectUnits(p.image);

    // bottom->top: paint1, shape
    QCOMPARE(units.size(), 2);
    QVERIFY(!units[0].isVector);
    QVERIFY(units[0].recompositable);
    QVERIFY(units[1].isVector);
    QVERIFY(units[1].recompositable);
    QCOMPARE(units[1].opacity, quint8(255));
    QCOMPARE(units[1].shapeLayers.size(), 1);
    QCOMPARE(units[1].shapeLayers[0].first, shapeLayer.data());
    QVERIFY(!units[1].extent.isEmpty());
    QVERIFY(units[1].extent.intersects(QRect(5, 5, 30, 30)));

    // hiding the vector layer removes it from the merge sequence
    shapeLayer->setVisible(false);
    const auto hiddenUnits = KisViewResVectorCompositor::collectUnits(p.image);
    QCOMPARE(hiddenUnits.size(), 1);
    shapeLayer->setVisible(true);
}

void KisViewResVectorCompositorTest::testComputeJobs()
{
    using namespace TestUtil;

    QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());
    const QRect refRect(0, 0, 64, 64);
    MaskParent p(refRect);
    p.image->setResolution(1.0, 1.0);
    doc->setCurrentImage(p.image);

    // raster layer above the vector layer, overlapping it in doc space:
    // it must be pulled into the same job so stacking survives.
    KisPaintLayerSP raster = new KisPaintLayer(p.image, "raster", 255);
    fillDeviceRect(raster->paintDevice(), QRect(20, 20, 20, 20), Qt::blue);
    raster->setDirty();

    KisShapeLayerSP shapeLayer = makeRectShapeLayer(doc.data(), p.image, "shape", 255, QRectF(5, 5, 30, 30), Qt::red);

    p.image->addNode(shapeLayer); // on top of raster
    shapeLayer->setDirty();
    p.image->addNode(raster);
    raster->setDirty();
    p.waitForImageAndShapeLayers();

    const auto units = KisViewResVectorCompositor::collectUnits(p.image);
    QCOMPARE(units.size(), 3);

    const auto jobs = KisViewResVectorCompositor::computeJobs(units);
    QCOMPARE(jobs.size(), 1);
    QCOMPARE(jobs[0].units.size(), 2); // raster+vector; empty paint1 stays out
    QVERIFY(jobs[0].docRect.contains(QRect(5, 5, 30, 30)));
    QVERIFY(jobs[0].docRect.contains(QRect(20, 20, 20, 20)));

    // a non-over unit overlapping the vector disqualifies the whole job
    raster->setCompositeOpId(COMPOSITE_MULT);
    const auto units2 = KisViewResVectorCompositor::collectUnits(p.image);
    const auto jobs2 = KisViewResVectorCompositor::computeJobs(units2);
    QVERIFY(jobs2.isEmpty());
    raster->setCompositeOpId(COMPOSITE_OVER);
}

void KisViewResVectorCompositorTest::testBackdropAndRecomposite()
{
    using namespace TestUtil;

    QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());
    const QRect refRect(0, 0, 64, 64);
    MaskParent p(refRect);
    p.image->setResolution(1.0, 1.0);
    doc->setCurrentImage(p.image);

    // paint backdrop content on the base layer
    fillDeviceRect(p.layer->paintDevice(), QRect(0, 0, 64, 64), Qt::darkGray);

    KisShapeLayerSP shapeLayer = makeRectShapeLayer(doc.data(), p.image, "shape", 255, QRectF(5, 5, 30, 30), Qt::red);

    p.image->addNode(shapeLayer);
    shapeLayer->setDirty();
    p.waitForImageAndShapeLayers();

    const auto units = KisViewResVectorCompositor::collectUnits(p.image);
    const auto jobs = KisViewResVectorCompositor::computeJobs(units);
    QCOMPARE(jobs.size(), 1);

    const KisViewResVectorCompositor::Job &job = jobs[0];
    QVERIFY(job.docRect.contains(QRect(5, 5, 30, 30)));

    const QImage backdrop = KisViewResVectorCompositor::buildBackdrop(p.image, job, units, &plainConvert);
    QVERIFY(!backdrop.isNull());
    QCOMPARE(backdrop.size(), job.docRect.size());

    // where the vector was fully opaque, the backdrop is peeled away
    const QPoint inside = QPoint(20, 20) - job.docRect.topLeft();
    QCOMPARE(qAlpha(backdrop.pixel(inside)), 0);

    /**
     * The base paint layer overlaps the vector, so it is part of the job
     * and peeled as well: outside the vector coverage the backdrop is the
     * content *below the whole job* (nothing here -> fully transparent).
     * Its pixels are restored by its own unit patch during recomposite.
     */
    const QPoint outside = QPoint(0, 0) - job.docRect.topLeft();
    QCOMPARE(qAlpha(backdrop.pixel(outside)), 0);

    /**
     * Re-compositing the job (backdrop + every unit's source patch in
     * order) must reproduce the displayed projection. This is the key
     * invariant: the recomposite is a no-op when the vector unit keeps its
     * document-resolution pixels, and swapping in a crisp repaint only
     * changes the vector's own pixels.
     */
    QImage recomposite = backdrop;
    {
        QPainter gc(&recomposite);
        for (int i = 0; i < job.units.size(); i++) {
            const QImage patch =
                KisViewResVectorCompositor::buildUnitPatch(units[job.units[i]], job.docRect, &plainConvert);
            if (patch.isNull())
                continue;
            gc.drawImage(0, 0, patch);
        }
    }

    const QImage displayed =
        plainConvert(p.image->projection(), job.docRect).convertToFormat(QImage::Format_ARGB32_Premultiplied);

    int mismatch = 0;
    for (int y = 0; y < recomposite.height(); y++) {
        for (int x = 0; x < recomposite.width(); x++) {
            const QRgb a = recomposite.pixel(x, y);
            const QRgb b = displayed.pixel(x, y);
            if (qAbs(qAlpha(a) - qAlpha(b)) > 2 || qAbs(qRed(a) - qRed(b)) > 2 || qAbs(qGreen(a) - qGreen(b)) > 2
                || qAbs(qBlue(a) - qBlue(b)) > 2) {
                mismatch++;
            }
        }
    }
    QCOMPARE(mismatch, 0);
}

KISTEST_MAIN(KisViewResVectorCompositorTest)
