/*
 *  SPDX-FileCopyrightText: 2016 Jouni Pentikäinen <joupent@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_animated_transform_parameters.h"

#include "kis_transform_mask.h"
#include <testutil.h>
#include "kistest.h"
#include "tool_transform_args.h"
#include "commands_new/KisSimpleModifyTransformMaskCommand.h"
#include "commands_new/KisLazyCreateTransformMaskKeyframesCommand.h"
#include "kis_image_animation_interface.h"
#include "kis_transform_mask_params_interface.h"
#include "KisAnimatedTransformMaskParamsHolder.h"
#include "kis_keyframe_channel.h"

#include <QDomDocument>

#include <limits>

#include <KoToolRegistry.h>

void KisAnimatedTransformParametersTest::initTestCase()
{
    KoToolRegistry::instance();
}

QSharedPointer<KisTransformMaskAdapter> adapterFromParams(KisTransformMaskParamsInterfaceSP params)
{
    return params.dynamicCast<KisTransformMaskAdapter>();
}

ToolTransformArgs argsFromParams(KisTransformMaskParamsInterfaceSP params) {
    return *adapterFromParams(params)->transformArgs();
}

void KisAnimatedTransformParametersTest::testTransformKeyframing()
{
    TestUtil::MaskParent p;
    KisTransformMaskSP mask = new KisTransformMask(p.image, "mask");
    p.image->addNode(mask, p.layer);

    // Make mask animated
    QList<KoID> ids = {
        KisKeyframeChannel::PositionX,
        KisKeyframeChannel::PositionY,
        KisKeyframeChannel::ScaleX,
        KisKeyframeChannel::ScaleY,
        KisKeyframeChannel::ShearX,
        KisKeyframeChannel::ShearY,
        KisKeyframeChannel::RotationX,
        KisKeyframeChannel::RotationY,
        KisKeyframeChannel::RotationZ
    };

    Q_FOREACH( const KoID& koid, ids ) {
        mask->getKeyframeChannel(koid.id(), true);
        QVERIFY(mask->getKeyframeChannel(koid.id(), false));
    }

    QVERIFY(!adapterFromParams(mask->transformParams())->isInitialized());

    ToolTransformArgs args;

    KUndo2Command firstFrameCommand;
    KUndo2Command secondFrameCommand;

    {
        p.image->animationInterface()->switchCurrentTimeAsync(0);
        p.image->waitForDone();

        args.setMode(ToolTransformArgs::FREE_TRANSFORM);
        args.setScaleX(0.75);

        new KisLazyCreateTransformMaskKeyframesCommand(mask, &firstFrameCommand);
        new KisSimpleModifyTransformMaskCommand(mask, toQShared(new KisTransformMaskAdapter(args)), {}, &firstFrameCommand);
        firstFrameCommand.redo();

        QVERIFY(adapterFromParams(mask->transformParams())->isInitialized());
        QCOMPARE(argsFromParams(mask->transformParams()).scaleX(), 0.75);
    }

    {
        p.image->animationInterface()->switchCurrentTimeAsync(10);
        p.image->waitForDone();

        args.setScaleX(0.5);

        new KisLazyCreateTransformMaskKeyframesCommand(mask, &secondFrameCommand);
        new KisSimpleModifyTransformMaskCommand(mask, toQShared(new KisTransformMaskAdapter(args)), {}, &secondFrameCommand);
        secondFrameCommand.redo();

        QVERIFY(adapterFromParams(mask->transformParams())->isInitialized());
        QCOMPARE(argsFromParams(mask->transformParams()).scaleX(), 0.5);
    }

    p.image->animationInterface()->switchCurrentTimeAsync(0);
    p.image->waitForDone();
    QVERIFY(p.image->animationInterface()->currentTime() == 0);
    QCOMPARE(argsFromParams(mask->transformParams()).scaleX(), 0.75);

    p.image->animationInterface()->switchCurrentTimeAsync(10);
    p.image->waitForDone();
    QVERIFY(p.image->animationInterface()->currentTime() == 10);
    QCOMPARE(argsFromParams(mask->transformParams()).scaleX(), 0.5);

    secondFrameCommand.undo();
    QVERIFY(p.image->animationInterface()->currentTime() == 10);
    QCOMPARE(argsFromParams(mask->transformParams()).scaleX(), 0.75);

    firstFrameCommand.undo();
    QVERIFY(p.image->animationInterface()->currentTime() == 10);
    QCOMPARE(argsFromParams(mask->transformParams()).scaleX(), 1.0);

    mask->forceUpdateTimedNode();
    p.image->waitForDone();
}

void KisAnimatedTransformParametersTest::testPuppetTransformSerialization()
{
    ToolTransformArgs args;
    args.setMode(ToolTransformArgs::PUPPET);
    args.setDefaultPoints(false);
    args.setWarpType(KisWarpTransformWorker::RIGID_TRANSFORM);
    args.setAlpha(1.5);
    args.setPoints({QPointF(10.0, 20.0), QPointF(30.0, 40.0)}, {QPointF(12.0, 23.0), QPointF(30.0, 40.0)});
    args.setPuppetRotation(0, 0.7853981633974483);
    args.setPuppetShowMesh(false);
    args.setPuppetExpansion(7);

    QDomDocument document;
    QDomElement element = document.createElement(QStringLiteral("transform"));
    document.appendChild(element);
    args.toXML(&element);

    const ToolTransformArgs restored = ToolTransformArgs::fromXML(element);
    QCOMPARE(restored.mode(), ToolTransformArgs::PUPPET);
    QCOMPARE(restored.defaultPoints(), false);
    QCOMPARE(restored.warpType(), KisWarpTransformWorker::RIGID_TRANSFORM);
    QCOMPARE(restored.alpha(), 1.5);
    QCOMPARE(restored.origPoints(), args.origPoints());
    QCOMPARE(restored.transfPoints(), args.transfPoints());
    QCOMPARE(restored.puppetRotation(0), 0.7853981633974483);
    QCOMPARE(restored.puppetRotation(1), 0.0);
    QCOMPARE(restored.puppetShowMesh(), false);
    QCOMPARE(restored.puppetExpansion(), 7);
    QVERIFY(!restored.isIdentity());
    // Documents without a mesh keep the legacy MLS deformation.
    QVERIFY(!restored.usesPuppetMesh());

    // The mesh solver state round-trips and moves with the points.
    QImage mask(40, 30, QImage::Format_Grayscale8);
    mask.fill(0);
    mask.setPixel(5, 5, 255);
    mask.setPixel(20, 25, 255);
    ToolTransformArgs meshArgs(args);
    meshArgs.setPuppetMesh(KisPuppetTransformWorker::Mesh::build(mask, QRectF(0, 0, 160, 120), 7));
    QVERIFY(meshArgs.usesPuppetMesh());
    QVERIFY(!(meshArgs == args));
    QDomDocument meshDocument;
    QDomElement meshElement = meshDocument.createElement(QStringLiteral("transform"));
    meshDocument.appendChild(meshElement);
    meshArgs.toXML(&meshElement);
    const ToolTransformArgs meshRestored = ToolTransformArgs::fromXML(meshElement);
    QVERIFY(meshRestored.usesPuppetMesh());
    QVERIFY(meshRestored.puppetMesh() == meshArgs.puppetMesh());
    ToolTransformArgs scaled(meshArgs);
    scaled.scale3dSrcAndDst(0.5);
    QCOMPARE(scaled.puppetMesh().origin, QPointF(0, 0));
    QCOMPARE(scaled.puppetMesh().columnStep, meshArgs.puppetMesh().columnStep * 0.5);
    QCOMPARE(scaled.origPoints()[0], meshArgs.origPoints()[0] * 0.5);

    // Pin orders: index-aligned, changed for selected pins, serialized.
    ToolTransformArgs ordered(meshArgs);
    QCOMPARE(ordered.puppetOrders().size(), 2);
    ordered.changePuppetOrder({1}, ToolTransformArgs::PuppetOrderToFront);
    QCOMPARE(ordered.puppetOrder(1), 1);
    // "To back" places the pin just below every other pin.
    ordered.changePuppetOrder({0}, ToolTransformArgs::PuppetOrderToBack);
    QCOMPARE(ordered.puppetOrder(0), 0);
    QVERIFY(ordered.puppetOrder(0) < ordered.puppetOrder(1));
    ordered.changePuppetOrder({0}, ToolTransformArgs::PuppetOrderBackward);
    QCOMPARE(ordered.puppetOrder(0), -1);
    QVERIFY(!(ordered == meshArgs));
    QDomDocument orderDocument;
    QDomElement orderElement = orderDocument.createElement(QStringLiteral("transform"));
    orderDocument.appendChild(orderElement);
    ordered.toXML(&orderElement);
    const ToolTransformArgs orderRestored = ToolTransformArgs::fromXML(orderElement);
    QCOMPARE(orderRestored.puppetOrders(), ordered.puppetOrders());
    ordered.removePuppetPoint(0);
    QCOMPARE(ordered.puppetOrders(), QVector<int>{1});

    QVector<QPointF> expandedOriginalPoints;
    QVector<QPointF> expandedTransformedPoints;
    restored.puppetControlPoints(restored.origPoints(),
                                 restored.transfPoints(),
                                 &expandedOriginalPoints,
                                 &expandedTransformedPoints);
    QCOMPARE(expandedOriginalPoints.size(), 16);
    QCOMPARE(expandedTransformedPoints.size(), 16);
    QVERIFY(expandedTransformedPoints[1] - restored.transfPoints()[0]
            != expandedOriginalPoints[1] - restored.origPoints()[0]);
    for (int i = 5; i < 10; ++i) {
        QCOMPARE(expandedTransformedPoints[i], expandedOriginalPoints[i]);
    }
    QVERIFY(expandedTransformedPoints[10] - restored.transfPoints()[0]
            != expandedOriginalPoints[10] - restored.origPoints()[0]);

    ToolTransformArgs rotationOnly;
    rotationOnly.setMode(ToolTransformArgs::PUPPET);
    rotationOnly.setPoints({QPointF(10.0, 20.0)}, {QPointF(10.0, 20.0)});
    rotationOnly.setPuppetRotation(0, 0.25);
    QVERIFY(!rotationOnly.isIdentity());
}

void KisAnimatedTransformParametersTest::testPuppetArgsState()
{
    ToolTransformArgs args;
    args.setMode(ToolTransformArgs::PUPPET);
    args.setPoints({QPointF(10.0, 20.0), QPointF(30.0, 40.0)}, {QPointF(10.0, 20.0), QPointF(30.0, 40.0)});
    args.setPuppetRotation(1, 0.5);
    args.setPuppetOrder(1, 2);

    // New pins do not inherit the rotations and orders of the old ones.
    ToolTransformArgs replaced(args);
    replaced.setPoints({QPointF(1.0, 2.0), QPointF(3.0, 4.0)}, {QPointF(1.0, 2.0), QPointF(3.0, 4.0)});
    QCOMPARE(replaced.puppetRotations(), QVector<qreal>({0.0, 0.0}));
    QCOMPARE(replaced.puppetOrders(), QVector<int>({0, 0}));
    QVERIFY(replaced.isIdentity());

    // Reset compares with isSameMode(): an order change is a change...
    ToolTransformArgs reordered(args);
    reordered.changePuppetOrder({0}, ToolTransformArgs::PuppetOrderToFront);
    QVERIFY(!args.isSameMode(reordered));
    // ...and so is a different mesh, but not a legacy transform getting one.
    QImage mask(40, 30, QImage::Format_Grayscale8);
    mask.fill(255);
    ToolTransformArgs meshed(args);
    meshed.setPuppetMesh(KisPuppetTransformWorker::Mesh::build(mask, QRectF(0, 0, 160, 120), 2));
    QVERIFY(args.isSameMode(meshed));
    ToolTransformArgs remeshed(meshed);
    remeshed.setPuppetMesh(KisPuppetTransformWorker::Mesh::build(mask, QRectF(0, 0, 160, 120), 5));
    QVERIFY(!meshed.isSameMode(remeshed));

    // The click action is a tool preference: assigning (undo, restoring a
    // continued transform) keeps the current one.
    const ToolTransformArgs::PuppetClickAction savedAction = args.puppetClickAction();
    ToolTransformArgs current(args);
    current.setPuppetClickAction(ToolTransformArgs::PuppetDeletePin);
    ToolTransformArgs older(args);
    older.setPuppetClickAction(ToolTransformArgs::PuppetSelectPin);
    current.setPuppetClickAction(ToolTransformArgs::PuppetDeletePin);
    current = older;
    QCOMPARE(current.puppetClickAction(), ToolTransformArgs::PuppetDeletePin);
    current.setPuppetClickAction(savedAction);

    // Non-finite rotations in a document load as zero.
    ToolTransformArgs damaged(args);
    damaged.setPuppetRotation(0, std::numeric_limits<qreal>::quiet_NaN());
    damaged.setPuppetRotation(1, std::numeric_limits<qreal>::infinity());
    QDomDocument document;
    QDomElement element = document.createElement(QStringLiteral("transform"));
    document.appendChild(element);
    damaged.toXML(&element);
    const ToolTransformArgs restored = ToolTransformArgs::fromXML(element);
    QCOMPARE(restored.puppetRotation(0), 0.0);
    QCOMPARE(restored.puppetRotation(1), 0.0);
}

KISTEST_MAIN(KisAnimatedTransformParametersTest)
