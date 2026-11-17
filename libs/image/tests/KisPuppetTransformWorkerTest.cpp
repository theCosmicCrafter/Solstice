/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <simpletest.h>

#include <QElapsedTimer>
#include <QPainter>
#include <QScopeGuard>

#include <cmath>
#include <limits>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>

#include "KisPuppetTransformWorker.h"
#include "kis_algebra_2d.h"
#include "kis_paint_device.h"

namespace
{
/**
 * A torso with an arm: the upper arm goes right from the shoulder to the
 * elbow, the forearm hangs down from the elbow. Pins sit on the torso, the
 * shoulder and the elbow; the forearm is free (like the user's report, where
 * rotating the elbow twisted the forearm instead of turning it).
 */
const QRectF Bounds(0, 0, 400, 320);
const QPointF TorsoPin(50, 150);
const QPointF ShoulderPin(110, 115);
const QPointF ElbowPin(225, 115);

QImage armMask()
{
    QImage mask(Bounds.size().toSize(), QImage::Format_Grayscale8);
    mask.fill(0);
    QPainter gc(&mask);
    gc.setPen(Qt::NoPen);
    gc.setBrush(Qt::white);
    gc.drawRect(QRect(0, 50, 100, 200)); // torso
    gc.drawRect(QRect(100, 100, 140, 30)); // upper arm
    gc.drawRect(QRect(210, 130, 30, 160)); // forearm
    gc.end();
    return mask;
}

QPointF rotated(const QPointF &offset, qreal angle)
{
    return QPointF(offset.x() * std::cos(angle) - offset.y() * std::sin(angle),
                   offset.x() * std::sin(angle) + offset.y() * std::cos(angle));
}
} // namespace

class KisPuppetTransformWorkerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testMeshMarksArtwork()
    {
        const auto mesh = KisPuppetTransformWorker::Mesh::build(armMask(), Bounds, 0);
        QVERIFY(mesh.isValid());
        QCOMPARE(mesh.columns, 17);
        QCOMPARE(mesh.rows, 14);
        int solid = 0;
        for (int i = 0; i < mesh.solid.size(); ++i)
            solid += mesh.solid.testBit(i);
        QVERIFY(solid > 40);
        QVERIFY(solid < mesh.solid.size());
        // Far right column is empty.
        QVERIFY(!mesh.triangleSolid(mesh.columns - 1, 0, 0));
    }

    void testSerializationRoundTrip()
    {
        const auto mesh = KisPuppetTransformWorker::Mesh::build(armMask(), Bounds, 3);
        const auto copy = KisPuppetTransformWorker::Mesh::fromString(mesh.toString());
        QVERIFY(copy.isValid());
        QVERIFY(copy == mesh);
        QVERIFY(!KisPuppetTransformWorker::Mesh::fromString(QStringLiteral("1;2;3")).isValid());
    }

    void testDamagedMeshStringsAreRejected()
    {
        // A valid 4x4 mesh string, then single damaged fields.
        const QString bits(8, QLatin1Char('0'));
        auto meshString = [&](const QString &x, const QString &columns) {
            return QStringList{x, "0", "24", "0", "0", "24", columns, "4", "2", bits}.join(QLatin1Char(';'));
        };
        QVERIFY(KisPuppetTransformWorker::Mesh::fromString(meshString("0", "4")).isValid());
        // A bad number must not be hidden by the integers parsed after it.
        QVERIFY(!KisPuppetTransformWorker::Mesh::fromString(meshString("x", "4")).isValid());
        QVERIFY(!KisPuppetTransformWorker::Mesh::fromString(meshString("nan", "4")).isValid());
        QVERIFY(!KisPuppetTransformWorker::Mesh::fromString(meshString("inf", "4")).isValid());
        // Mesh::build() never makes more than 64 cells per side.
        const QString wideBits((2 * 65 * 4 + 3) / 4, QLatin1Char('0'));
        QVERIFY(!KisPuppetTransformWorker::Mesh::fromString(
                     QStringList{"0", "0", "24", "0", "0", "24", "65", "4", "2", wideBits}.join(QLatin1Char(';')))
                     .isValid());
    }

    void testNonFinitePinsRenderUnchanged()
    {
        const auto mesh = KisPuppetTransformWorker::Mesh::build(armMask(), Bounds, 0);
        KisPuppetTransformWorker worker(mesh,
                                        {TorsoPin, ElbowPin},
                                        {TorsoPin, ElbowPin + QPointF(10, 0)},
                                        {0.0, std::numeric_limits<qreal>::quiet_NaN()});
        QVERIFY(worker.isIdentity());
        QCOMPARE(worker.map(ElbowPin), ElbowPin);
    }

    void testIdentity()
    {
        const auto mesh = KisPuppetTransformWorker::Mesh::build(armMask(), Bounds, 0);
        KisPuppetTransformWorker worker(mesh, {TorsoPin, ElbowPin}, {TorsoPin, ElbowPin}, {0.0, 0.0});
        QVERIFY(worker.isIdentity());
        QCOMPARE(worker.map(QPointF(123.4, 56.7)), QPointF(123.4, 56.7));
    }

    void testRotatedElbowTurnsForearmRigidly()
    {
        const auto mesh = KisPuppetTransformWorker::Mesh::build(armMask(), Bounds, 0);
        const qreal angle = M_PI / 2; // the forearm swings to point left
        QElapsedTimer timer;
        timer.start();
        KisPuppetTransformWorker worker(mesh,
                                        {TorsoPin, ShoulderPin, ElbowPin},
                                        {TorsoPin, ShoulderPin, ElbowPin},
                                        {0.0, 0.0, angle});
        qInfo() << "solve ms" << timer.nsecsElapsed() / 1e6 << "grid" << mesh.columns << mesh.rows;
        QVERIFY(!worker.isIdentity());

        // Points along the forearm turn about the elbow as one piece.
        qreal worst = 0;
        for (qreal y = 150; y <= 280; y += 10) {
            const QPointF point(225, y);
            const QPointF expected = ElbowPin + rotated(point - ElbowPin, angle);
            worst = qMax(worst, KisAlgebra2D::norm(worker.map(point) - expected));
        }
        qInfo() << "forearm worst deviation px" << worst;
        // The joint may shift a little (about 5% of the 165 px forearm)...
        QVERIFY2(worst < 9.0, qPrintable(QString::number(worst)));
        // ...but the forearm turns by the pin's angle as one piece.
        const QPointF axis = worker.map(QPointF(225, 280)) - worker.map(QPointF(225, 160));
        const qreal turned = std::remainder(std::atan2(axis.y(), axis.x()) - std::atan2(120.0, 0.0), 2 * M_PI);
        qInfo() << "forearm turned degrees" << qRadiansToDegrees(turned);
        QVERIFY(qAbs(qRadiansToDegrees(std::remainder(turned - angle, 2 * M_PI))) < 4.0);

        // The forearm keeps its length and width.
        const QPointF a = worker.map(QPointF(212, 270));
        const QPointF b = worker.map(QPointF(238, 270));
        const QPointF c = worker.map(QPointF(225, 160));
        const QPointF d = worker.map(QPointF(225, 280));
        QVERIFY(qAbs(KisAlgebra2D::norm(b - a) - 26.0) < 2.0);
        QVERIFY(qAbs(KisAlgebra2D::norm(d - c) - 120.0) < 4.0);

        // Pinned parts stay.
        QVERIFY(KisAlgebra2D::norm(worker.map(TorsoPin) - TorsoPin) < 0.5);
        QVERIFY(KisAlgebra2D::norm(worker.map(QPointF(30, 200)) - QPointF(30, 200)) < 1.0);
        QVERIFY(KisAlgebra2D::norm(worker.map(ShoulderPin) - ShoulderPin) < 0.5);
    }

    void testMovedPinDragsFreeEnd()
    {
        // A pin moved away translates the free part beyond it.
        const auto mesh = KisPuppetTransformWorker::Mesh::build(armMask(), Bounds, 0);
        const QPointF offset(-40, 30);
        KisPuppetTransformWorker worker(mesh,
                                        {TorsoPin, ShoulderPin, ElbowPin},
                                        {TorsoPin, ShoulderPin, ElbowPin + offset},
                                        {0.0, 0.0, 0.0});
        const QPointF tip(225, 280);
        const QPointF moved = worker.map(tip);
        // Mostly carried along (the joint may bend), clearly not left behind.
        QVERIFY(KisAlgebra2D::norm(moved - (tip + offset)) < 25.0);
        QVERIFY(KisAlgebra2D::norm(worker.map(TorsoPin) - TorsoPin) < 0.5);
    }

    void testScaledMeshScalesResult()
    {
        // The in-stack preview renders a scaled copy (level of detail): the
        // deformation must scale with the mesh and the pins.
        const auto mesh = KisPuppetTransformWorker::Mesh::build(armMask(), Bounds, 0);
        const QVector<QPointF> original{TorsoPin, ShoulderPin, ElbowPin};
        const QVector<QPointF> transformed{TorsoPin, ShoulderPin + QPointF(5, -8), ElbowPin + QPointF(-30, 20)};
        const QVector<qreal> rotations{0.0, 0.2, 1.1};
        KisPuppetTransformWorker full(mesh, original, transformed, rotations);

        const qreal scale = 0.25;
        const QTransform t = QTransform::fromScale(scale, scale);
        auto scaledMesh = mesh;
        scaledMesh.transform(t);
        QVector<QPointF> scaledOriginal;
        QVector<QPointF> scaledTransformed;
        for (const QPointF &p : original)
            scaledOriginal << t.map(p);
        for (const QPointF &p : transformed)
            scaledTransformed << t.map(p);
        KisPuppetTransformWorker reduced(scaledMesh, scaledOriginal, scaledTransformed, rotations);
        qreal worst = 0;
        for (qreal y = 0; y < Bounds.height(); y += 13)
            for (qreal x = 0; x < Bounds.width(); x += 17)
                worst =
                    qMax(worst, KisAlgebra2D::norm(reduced.map(t.map(QPointF(x, y))) - t.map(full.map(QPointF(x, y)))));
        QVERIFY2(worst < 1e-3, qPrintable(QString::number(worst)));
    }

    void testRendersDevice()
    {
        // The forearm's pixels move to where the rotated mesh puts them.
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        KisPaintDeviceSP src = new KisPaintDevice(cs);
        const KoColor black(Qt::black, cs);
        src->fill(QRect(0, 50, 100, 200), black);
        src->fill(QRect(100, 100, 140, 30), black);
        src->fill(QRect(210, 130, 30, 160), black);
        const auto mesh = KisPuppetTransformWorker::Mesh::build(armMask(), Bounds, 0);
        KisPuppetTransformWorker worker(mesh,
                                        {TorsoPin, ShoulderPin, ElbowPin},
                                        {TorsoPin, ShoulderPin, ElbowPin},
                                        {0.0, 0.0, M_PI / 2});
        KisPaintDeviceSP dst = new KisPaintDevice(cs);
        worker.run(src, dst);
        auto alphaAt = [&](KisPaintDeviceSP device, const QPointF &point) {
            KoColor color(cs);
            device->pixel(point.x(), point.y(), &color);
            return color.opacityU8();
        };
        const QPointF tip(225, 270);
        QCOMPARE(alphaAt(src, tip), quint8(255));
        QCOMPARE(alphaAt(dst, tip), quint8(0)); // the forearm left
        QCOMPARE(alphaAt(dst, worker.map(tip)), quint8(255)); // and arrived
        QCOMPARE(alphaAt(dst, QPointF(50, 150)), quint8(255)); // torso stays
    }

    void testRenderKeepsPixelsOutsideSolidTriangles()
    {
        // The mesh comes from a thumbnail mask with an alpha threshold, so
        // faint pixels, thin lines and content painted after the mesh was
        // built can lie in empty triangles. Applying must not delete them.
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        KisPaintDeviceSP src = new KisPaintDevice(cs);
        const KoColor black(Qt::black, cs);
        src->fill(QRect(0, 50, 100, 200), black);
        src->fill(QRect(100, 100, 140, 30), black);
        src->fill(QRect(210, 130, 30, 160), black);
        const QPoint speck(350, 30); // far from the arm mask
        src->fill(QRect(speck, QSize(3, 3)), black);
        KoColor faint(Qt::black, cs);
        faint.setOpacity(quint8(5)); // below the mask threshold
        const QPoint faintSpot(360, 280);
        src->fill(QRect(faintSpot, QSize(4, 4)), faint);

        const auto mesh = KisPuppetTransformWorker::Mesh::build(armMask(), Bounds, 0);
        int column = 0;
        int row = 0;
        // Both spots lie in empty triangles of the mesh.
        for (const QPointF &spot : {QPointF(speck), QPointF(faintSpot)}) {
            column = int((spot.x() - mesh.origin.x()) / mesh.columnStep.x());
            row = int((spot.y() - mesh.origin.y()) / mesh.rowStep.y());
            QVERIFY(!mesh.triangleSolid(column, row, 0) && !mesh.triangleSolid(column, row, 1));
        }
        KisPuppetTransformWorker worker(mesh,
                                        {TorsoPin, ShoulderPin, ElbowPin},
                                        {TorsoPin, ShoulderPin, ElbowPin + QPointF(0, 10)},
                                        {0.0, 0.0, 0.3});
        KisPaintDeviceSP dst = new KisPaintDevice(cs);
        worker.run(src, dst);
        auto maxAlphaAround = [&](const QPointF &point) {
            quint8 result = 0;
            for (int y = -2; y <= 2; ++y) {
                for (int x = -2; x <= 2; ++x) {
                    KoColor color(cs);
                    dst->pixel(qRound(point.x()) + x, qRound(point.y()) + y, &color);
                    result = qMax(result, color.opacityU8());
                }
            }
            return result;
        };
        QVERIFY(maxAlphaAround(worker.map(QPointF(speck) + QPointF(1.5, 1.5))) > 200);
        QVERIFY(maxAlphaAround(worker.map(QPointF(faintSpot) + QPointF(2, 2))) > 0);
        // The arm is still there.
        QCOMPARE(maxAlphaAround(QPointF(50, 150)), quint8(255));
    }

    void testPinOrderStacksOverlappingParts_data()
    {
        QTest::addColumn<int>("elbowOrder");
        QTest::addColumn<bool>("armOnTop");
        QTest::newRow("elbow-front") << 1 << true;
        QTest::newRow("elbow-back") << -1 << false;
        QTest::newRow("equal") << 0 << true; // one pass: later cells overwrite
    }

    void testPinOrderStacksOverlappingParts()
    {
        // Turning the elbow swings the forearm over the torso. The pin order
        // of the part's nearest pin decides which one is on top.
        QFETCH(int, elbowOrder);
        QFETCH(bool, armOnTop);
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        KisPaintDeviceSP src = new KisPaintDevice(cs);
        src->fill(QRect(0, 50, 100, 200), KoColor(Qt::red, cs));
        src->fill(QRect(100, 100, 140, 30), KoColor(Qt::blue, cs));
        src->fill(QRect(210, 130, 30, 160), KoColor(Qt::blue, cs));
        const auto mesh = KisPuppetTransformWorker::Mesh::build(armMask(), Bounds, 0);
        KisPuppetTransformWorker worker(mesh,
                                        {TorsoPin, ShoulderPin, ElbowPin},
                                        {TorsoPin, ShoulderPin, ElbowPin},
                                        {0.0, 0.0, M_PI / 2},
                                        {0, 0, elbowOrder});
        // Torso, shoulder and elbow parts render as separate groups; equal
        // orders put the later pin (the elbow) on top.
        QVERIFY(worker.stackingGroups().size() >= 2);
        QCOMPARE(worker.stackingGroups().last() == 2, elbowOrder >= 0);
        KisPaintDeviceSP dst = new KisPaintDevice(cs);
        worker.run(src, dst);
        // A point of the swung forearm that lands on the torso.
        const QPointF forearm(225, 250);
        const QPointF landed = worker.map(forearm);
        QVERIFY(QRectF(0, 50, 100, 200).contains(landed));
        KoColor color(cs);
        dst->pixel(landed.x(), landed.y(), &color);
        const QColor actual = color.toQColor();
        if (elbowOrder != 0)
            QCOMPARE(actual.blue() > actual.red(), armOnTop);
        // No holes: empty space swung along with the forearm must not erase
        // the torso (the grid polygon op overwrites pixels).
        int holes = 0;
        for (int y = 60; y < 240; y += 2) {
            for (int x = 10; x < 90; x += 2) {
                KoColor pixel(cs);
                dst->pixel(x, y, &pixel);
                holes += pixel.opacityU8() == 0;
            }
        }
        QCOMPARE(holes, 0);
    }

    void testFoldedElbowGap()
    {
        // The user's report: folding the forearm back at the elbow drags the
        // shared joint vertices and leaves a gap in the upper arm. A connected
        // mesh cannot overlap the parts; this measures the current gap so a
        // hinge (mesh cut at the joint) can be compared against it.
        const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
        KisPaintDeviceSP src = new KisPaintDevice(cs);
        const KoColor black(Qt::black, cs);
        src->fill(QRect(0, 50, 100, 200), black);
        src->fill(QRect(100, 100, 140, 30), black);
        src->fill(QRect(210, 130, 30, 160), black);
        const auto mesh = KisPuppetTransformWorker::Mesh::build(armMask(), Bounds, 0);
        const qreal angle = 2.4; // about 137 degrees: the forearm folds back up
        KisPuppetTransformWorker worker(mesh,
                                        {TorsoPin, ShoulderPin, ElbowPin},
                                        {TorsoPin, ShoulderPin, ElbowPin},
                                        {0.0, 0.0, angle});
        KisPaintDeviceSP dst = new KisPaintDevice(cs);
        worker.run(src, dst);
        // The upper arm between the pins should stay covered.
        int gap = 0;
        for (int y = 103; y < 128; ++y) {
            for (int x = 104; x < 224; ++x) {
                KoColor pixel(cs);
                dst->pixel(x, y, &pixel);
                gap += pixel.opacityU8() < 128;
            }
        }
        // The forearm still turns with the pin.
        const QPointF tip = worker.map(QPointF(225, 270));
        const QPointF expected = ElbowPin + rotated(QPointF(0, 155), angle);
        const qreal tipError = KisAlgebra2D::norm(tip - expected);
        qInfo() << "gap pixels" << gap << "tip error px" << tipError;
        if (qEnvironmentVariableIsSet("PUPPET_DUMP")) {
            dst->convertToQImage(nullptr, 0, 0, 400, 320)
                .save(QString::fromLocal8Bit(qgetenv("PUPPET_DUMP")) + QStringLiteral("/fold.png"));
        }
        // Current behaviour (hinge side unconstrained): 556 gap pixels (558
        // before the source-alpha cell filter of 2026-10-15).
        QVERIFY2(gap <= 600, qPrintable(QString::number(gap)));
    }

    void testLargeGridSolveTime()
    {
        const QRectF bounds(0, 0, 2480, 3508);
        QImage mask(620, 877, QImage::Format_Grayscale8);
        mask.fill(255);
        const auto mesh = KisPuppetTransformWorker::Mesh::build(mask, bounds, 0);
        QCOMPARE(mesh.columns, 64);
        QCOMPARE(mesh.rows, 64);
        QElapsedTimer timer;
        timer.start();
        KisPuppetTransformWorker worker(mesh,
                                        {QPointF(1200, 600), QPointF(1200, 1800), QPointF(1200, 3000)},
                                        {QPointF(1200, 600), QPointF(1300, 1800), QPointF(1200, 3000)},
                                        {0.0, 0.3, 0.0});
        qInfo() << "64x64 solve ms" << timer.nsecsElapsed() / 1e6;
        QVERIFY(!worker.isIdentity());
    }
};

SIMPLE_TEST_MAIN(KisPuppetTransformWorkerTest)

#include "KisPuppetTransformWorkerTest.moc"
