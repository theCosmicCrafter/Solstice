/*
 *  tool_transform_args.h - part of Krita
 *
 *  SPDX-FileCopyrightText: 2010 Marc Pegon <pe.marc@free.fr>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "tool_transform_args.h"

#include <QDomElement>
#include <QLineF>
#include <QtMath>

#include <limits>

#include <ksharedconfig.h>
#include <kconfig.h>
#include <kconfiggroup.h>

#include "kis_liquify_transform_worker.h"
#include "kis_dom_utils.h"
#include <QMatrix4x4>


ToolTransformArgs::ToolTransformArgs()
    : m_liquifyProperties(new KisLiquifyProperties())
{
    KConfigGroup configGroup =  KSharedConfig::openConfig()->group("KisToolTransform");
    QString savedFilterId = configGroup.readEntry("filterId", "Bicubic");
    setFilterId(savedFilterId);
    m_transformAroundRotationCenter = configGroup.readEntry("transformAroundRotationCenter", "0").toInt();
    m_meshShowHandles = configGroup.readEntry("meshShowHandles", true);
    m_meshSymmetricalHandles = configGroup.readEntry("meshSymmetricalHandles", true);
    m_meshScaleHandles = configGroup.readEntry("meshScaleHandles", false);
    m_puppetShowMesh = configGroup.readEntry("puppetShowMesh", true);
    m_puppetExpansion = configGroup.readEntry("puppetExpansion", 2);
    m_puppetClickAction =
        configGroup.readEntry("puppetClickAction", 0) == int(PuppetDeletePin) ? PuppetDeletePin : PuppetSelectPin;
}

void ToolTransformArgs::setPuppetClickAction(PuppetClickAction action)
{
    m_puppetClickAction = action;
    KSharedConfig::openConfig()->group("KisToolTransform").writeEntry("puppetClickAction", int(action));
}

int ToolTransformArgs::puppetOrder(int index) const
{
    return index >= 0 && index < m_puppetOrders.size() ? m_puppetOrders[index] : 0;
}

void ToolTransformArgs::setPuppetOrder(int index, int value)
{
    if (index < 0 || index >= m_origPoints.size()) {
        return;
    }
    m_puppetOrders.resize(m_origPoints.size());
    m_puppetOrders[index] = value;
}

void ToolTransformArgs::changePuppetOrder(const QVector<int> &indexes, PuppetOrderChange change)
{
    m_puppetOrders.resize(m_origPoints.size());
    int lowest = 0;
    int highest = 0;
    bool first = true;
    for (int i = 0; i < m_puppetOrders.size(); ++i) {
        if (indexes.contains(i)) {
            continue;
        }
        lowest = first ? m_puppetOrders[i] : qMin(lowest, m_puppetOrders[i]);
        highest = first ? m_puppetOrders[i] : qMax(highest, m_puppetOrders[i]);
        first = false;
    }
    for (int index : indexes) {
        if (index < 0 || index >= m_puppetOrders.size()) {
            continue;
        }
        int &order = m_puppetOrders[index];
        switch (change) {
        case PuppetOrderToFront:
            order = highest + 1;
            break;
        case PuppetOrderForward:
            order += 1;
            break;
        case PuppetOrderBackward:
            order -= 1;
            break;
        case PuppetOrderToBack:
            order = lowest - 1;
            break;
        }
    }
}

void ToolTransformArgs::setPuppetShowMesh(bool value)
{
    m_puppetShowMesh = value;
    KSharedConfig::openConfig()->group("KisToolTransform").writeEntry("puppetShowMesh", value);
}

void ToolTransformArgs::setPuppetExpansion(int value)
{
    m_puppetExpansion = qBound(0, value, 64);
    KSharedConfig::openConfig()->group("KisToolTransform").writeEntry("puppetExpansion", m_puppetExpansion);
}

void ToolTransformArgs::setPoints(QVector<QPointF> origPoints, QVector<QPointF> transfPoints)
{
    m_origPoints = origPoints;
    m_transfPoints = transfPoints;
    // New pins: rotations and orders of the old ones do not apply to them.
    const int count = qMin(m_origPoints.size(), m_transfPoints.size());
    m_puppetRotations = QVector<qreal>(count, 0.0);
    m_puppetOrders = QVector<int>(count, 0);
}

qreal ToolTransformArgs::puppetRotation(int index) const
{
    return index >= 0 && index < m_puppetRotations.size() ? m_puppetRotations[index] : 0.0;
}

void ToolTransformArgs::setPuppetRotation(int index, qreal value)
{
    if (index < 0 || index >= m_origPoints.size()) {
        return;
    }
    m_puppetRotations.resize(m_origPoints.size());
    m_puppetRotations[index] = value;
}

void ToolTransformArgs::removePuppetPoint(int index)
{
    if (index < 0 || index >= m_origPoints.size() || index >= m_transfPoints.size()) {
        return;
    }
    m_origPoints.removeAt(index);
    m_transfPoints.removeAt(index);
    if (index < m_puppetRotations.size()) {
        m_puppetRotations.removeAt(index);
    }
    if (index < m_puppetOrders.size()) {
        m_puppetOrders.removeAt(index);
    }
}

KisPuppetTransformWorker ToolTransformArgs::createPuppetWorker() const
{
    return KisPuppetTransformWorker(m_puppetMesh, m_origPoints, m_transfPoints, m_puppetRotations, m_puppetOrders);
}

void ToolTransformArgs::puppetControlPoints(const QVector<QPointF> &originalPoints,
                                            const QVector<QPointF> &transformedPoints,
                                            QVector<QPointF> *expandedOriginalPoints,
                                            QVector<QPointF> *expandedTransformedPoints) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(originalPoints.size() == transformedPoints.size());
    expandedOriginalPoints->clear();
    expandedTransformedPoints->clear();
    expandedOriginalPoints->reserve(originalPoints.size() * 8);
    expandedTransformedPoints->reserve(transformedPoints.size() * 8);

    QVector<QVector<int>> pinGraph(originalPoints.size());
    if (originalPoints.size() > 1) {
        QVector<bool> connected(originalPoints.size(), false);
        QVector<qreal> closestDistance(originalPoints.size(), std::numeric_limits<qreal>::max());
        QVector<int> closestPin(originalPoints.size(), -1);
        closestDistance[0] = 0.0;

        for (int edge = 0; edge < originalPoints.size(); ++edge) {
            int nextPin = -1;
            for (int i = 0; i < originalPoints.size(); ++i) {
                if (!connected[i] && (nextPin < 0 || closestDistance[i] < closestDistance[nextPin])) {
                    nextPin = i;
                }
            }
            if (nextPin < 0) {
                break;
            }
            connected[nextPin] = true;
            if (closestPin[nextPin] >= 0) {
                pinGraph[nextPin].append(closestPin[nextPin]);
                pinGraph[closestPin[nextPin]].append(nextPin);
            }
            for (int i = 0; i < originalPoints.size(); ++i) {
                if (!connected[i]) {
                    const qreal distance = QLineF(originalPoints[nextPin], originalPoints[i]).length();
                    if (distance < closestDistance[i]) {
                        closestDistance[i] = distance;
                        closestPin[i] = nextPin;
                    }
                }
            }
        }
    }

    for (int i = 0; i < originalPoints.size(); ++i) {
        qreal nearestDistance = std::numeric_limits<qreal>::max();
        for (int j = 0; j < originalPoints.size(); ++j) {
            if (i != j) {
                nearestDistance = qMin(nearestDistance, QLineF(originalPoints[i], originalPoints[j]).length());
            }
        }
        const qreal radius = nearestDistance == std::numeric_limits<qreal>::max()
            ? 32.0
            : qBound<qreal>(8.0, nearestDistance * 0.28, 64.0);
        const qreal angle = puppetRotation(i);
        const qreal cosine = qCos(angle);
        const qreal sine = qSin(angle);
        const QPointF offsets[] = {QPointF(),
                                   QPointF(radius, 0.0),
                                   QPointF(-radius, 0.0),
                                   QPointF(0.0, radius),
                                   QPointF(0.0, -radius)};

        for (const QPointF &offset : offsets) {
            expandedOriginalPoints->append(originalPoints[i] + offset);
            const QPointF rotatedOffset(offset.x() * cosine - offset.y() * sine,
                                        offset.x() * sine + offset.y() * cosine);
            expandedTransformedPoints->append(transformedPoints[i] + rotatedOffset);
        }
    }

    // A terminal pin owns the open-ended part of the artwork beyond it. Adding
    // progressively distant guide points along that branch makes a pin rotation
    // propagate through the unpinned limb/body instead of remaining a tiny local
    // twist. A newly placed pin in that branch splits the graph and stops the
    // propagation naturally.
    for (int i = 0; i < originalPoints.size(); ++i) {
        if (pinGraph[i].size() != 1) {
            continue;
        }
        const QPointF neighbourDirection = originalPoints[i] - originalPoints[pinGraph[i][0]];
        const qreal neighbourDistance = QLineF(QPointF(), neighbourDirection).length();
        if (qFuzzyIsNull(neighbourDistance)) {
            continue;
        }
        const QPointF direction = neighbourDirection / neighbourDistance;
        const qreal angle = puppetRotation(i);
        const qreal cosine = qCos(angle);
        const qreal sine = qSin(angle);
        const qreal guideDistances[] = {0.75 * neighbourDistance, 1.5 * neighbourDistance, 3.0 * neighbourDistance};
        for (qreal guideDistance : guideDistances) {
            const QPointF offset = direction * guideDistance;
            const QPointF rotatedOffset(offset.x() * cosine - offset.y() * sine,
                                        offset.x() * sine + offset.y() * cosine);
            expandedOriginalPoints->append(originalPoints[i] + offset);
            expandedTransformedPoints->append(transformedPoints[i] + rotatedOffset);
        }
    }
}

void ToolTransformArgs::setFilterId(const QString &id) {
    m_filter = KisFilterStrategyRegistry::instance()->value(id);

    if (m_filter) {
        KConfigGroup configGroup =  KSharedConfig::openConfig()->group("KisToolTransform");
        configGroup.writeEntry("filterId", id);
    }
}

void ToolTransformArgs::setTransformAroundRotationCenter(bool value)
{
    m_transformAroundRotationCenter = value;

    KConfigGroup configGroup =  KSharedConfig::openConfig()->group("KisToolTransform");
    configGroup.writeEntry("transformAroundRotationCenter", int(value));
}

void ToolTransformArgs::init(const ToolTransformArgs& args)
{
    m_mode = args.mode();
    m_transformedCenter = args.transformedCenter();
    m_originalCenter = args.originalCenter();
    m_rotationCenterOffset = args.rotationCenterOffset();
    m_transformAroundRotationCenter = args.transformAroundRotationCenter();
    m_cameraPos = args.m_cameraPos;
    m_aX = args.aX();
    m_aY = args.aY();
    m_aZ = args.aZ();
    m_scaleX = args.scaleX();
    m_scaleY = args.scaleY();
    m_shearX = args.shearX();
    m_shearY = args.shearY();
    m_boundsRotation = args.boundsRotation();
    m_origPoints = args.origPoints(); //it's a copy
    m_transfPoints = args.transfPoints();
    m_puppetRotations = args.m_puppetRotations;
    m_warpType = args.warpType();
    m_alpha = args.alpha();
    m_puppetShowMesh = args.m_puppetShowMesh;
    m_puppetExpansion = args.m_puppetExpansion;
    m_puppetMesh = args.m_puppetMesh;
    m_puppetOrders = args.m_puppetOrders;
    m_puppetClickAction = args.m_puppetClickAction;
    m_defaultPoints = args.defaultPoints();
    m_keepAspectRatio = args.keepAspectRatio();
    m_filter = args.m_filter;
    m_flattenedPerspectiveTransform = args.m_flattenedPerspectiveTransform;
    m_editTransformPoints = args.m_editTransformPoints;
    m_pixelPrecision = args.pixelPrecision();
    m_previewPixelPrecision = args.previewPixelPrecision();
    m_externalSource = args.externalSource();

    if (args.m_liquifyWorker) {
        m_liquifyWorker.reset(new KisLiquifyTransformWorker(*args.m_liquifyWorker.data()));
    }

    m_meshTransform = args.m_meshTransform;
    m_meshShowHandles = args.m_meshShowHandles;
    m_meshSymmetricalHandles = args.m_meshSymmetricalHandles;
    m_meshScaleHandles = args.m_meshScaleHandles;

    m_continuedTransformation.reset(args.m_continuedTransformation ? new ToolTransformArgs(*args.m_continuedTransformation) : 0);
}

bool ToolTransformArgs::meshScaleHandles() const
{
    return m_meshScaleHandles;
}

void ToolTransformArgs::setMeshScaleHandles(bool meshScaleHandles)
{
    m_meshScaleHandles = meshScaleHandles;

    KConfigGroup configGroup =  KSharedConfig::openConfig()->group("KisToolTransform");
    configGroup.writeEntry("meshScaleHandles", meshScaleHandles);
}

void ToolTransformArgs::clear()
{
    m_origPoints.clear();
    m_transfPoints.clear();
    m_puppetRotations.clear();
    m_puppetMesh = KisPuppetTransformWorker::Mesh();
    m_puppetOrders.clear();
    m_meshTransform = KisBezierTransformMesh();
}

ToolTransformArgs::ToolTransformArgs(const ToolTransformArgs& args)
    : m_liquifyProperties(new KisLiquifyProperties(*args.m_liquifyProperties.data()))
{
    init(args);
}

KisToolChangesTrackerData *ToolTransformArgs::clone() const
{
    return new ToolTransformArgs(*this);
}

ToolTransformArgs& ToolTransformArgs::operator=(const ToolTransformArgs& args)
{
    if (this == &args) return *this;

    // The Puppet click action is a tool preference (KConfig), not transform
    // state: undo and restoring a continued transform must not change it.
    const PuppetClickAction clickAction = m_puppetClickAction;

    clear();

    m_liquifyProperties.reset(new KisLiquifyProperties(*args.m_liquifyProperties.data()));
    init(args);
    m_puppetClickAction = clickAction;

    return *this;
}

bool ToolTransformArgs::operator==(const ToolTransformArgs& other) const
{
    return m_mode == other.m_mode && m_defaultPoints == other.m_defaultPoints && m_origPoints == other.m_origPoints
        && m_transfPoints == other.m_transfPoints && m_warpType == other.m_warpType && m_alpha == other.m_alpha
        && m_puppetRotations == other.m_puppetRotations && m_puppetShowMesh == other.m_puppetShowMesh
        && m_puppetExpansion == other.m_puppetExpansion && m_puppetMesh == other.m_puppetMesh
        && m_puppetOrders == other.m_puppetOrders && m_transformedCenter == other.m_transformedCenter
        && m_originalCenter == other.m_originalCenter && m_rotationCenterOffset == other.m_rotationCenterOffset
        && m_transformAroundRotationCenter == other.m_transformAroundRotationCenter && m_aX == other.m_aX
        && m_aY == other.m_aY && m_aZ == other.m_aZ && m_cameraPos == other.m_cameraPos && m_scaleX == other.m_scaleX
        && m_scaleY == other.m_scaleY && m_shearX == other.m_shearX && m_shearY == other.m_shearY
        && m_boundsRotation == other.m_boundsRotation && m_keepAspectRatio == other.m_keepAspectRatio
        && m_flattenedPerspectiveTransform == other.m_flattenedPerspectiveTransform
        && m_editTransformPoints == other.m_editTransformPoints
        && (m_liquifyProperties == other.m_liquifyProperties || *m_liquifyProperties == *other.m_liquifyProperties)
        && m_meshTransform == other.m_meshTransform &&

        // pointer types

        m_externalSource == other.m_externalSource &&

        ((m_filter && other.m_filter && m_filter->id() == other.m_filter->id()) || m_filter == other.m_filter) &&

        ((m_liquifyWorker && other.m_liquifyWorker && *m_liquifyWorker == *other.m_liquifyWorker)
         || m_liquifyWorker == other.m_liquifyWorker)
        && m_pixelPrecision == other.m_pixelPrecision && m_previewPixelPrecision == other.m_previewPixelPrecision;
}

bool ToolTransformArgs::isSameMode(const ToolTransformArgs& other) const
{
    if (m_mode != other.m_mode) return false;

    bool result = true;

    if (m_mode == FREE_TRANSFORM) {
        result &= m_transformedCenter == other.m_transformedCenter;
        result &= m_originalCenter == other.m_originalCenter;
        result &= m_scaleX == other.m_scaleX;
        result &= m_scaleY == other.m_scaleY;
        result &= m_shearX == other.m_shearX;
        result &= m_shearY == other.m_shearY;
        result &= m_boundsRotation == other.m_boundsRotation;
        result &= m_aX == other.m_aX;
        result &= m_aY == other.m_aY;
        result &= m_aZ == other.m_aZ;

    } else if (m_mode == PERSPECTIVE_4POINT) {
        result &= m_transformedCenter == other.m_transformedCenter;
        result &= m_originalCenter == other.m_originalCenter;
        result &= m_scaleX == other.m_scaleX;
        result &= m_scaleY == other.m_scaleY;
        result &= m_shearX == other.m_shearX;
        result &= m_shearY == other.m_shearY;
        result &= m_flattenedPerspectiveTransform == other.m_flattenedPerspectiveTransform;

    } else if (m_mode == WARP || m_mode == CAGE || m_mode == PUPPET) {
        result &= m_origPoints == other.m_origPoints;
        result &= m_transfPoints == other.m_transfPoints;
        if (m_mode == PUPPET) {
            result &= m_puppetRotations == other.m_puppetRotations;
            result &= m_puppetOrders == other.m_puppetOrders;
            // A legacy transform (no mesh) gets one when edited; that alone
            // must not count as a change, or Reset would only ever restore it.
            result &= !m_puppetMesh.isValid() || !other.m_puppetMesh.isValid() || m_puppetMesh == other.m_puppetMesh;
        }

    } else if (m_mode == LIQUIFY) {
        result &= m_liquifyProperties &&
            (m_liquifyProperties == other.m_liquifyProperties ||
             *m_liquifyProperties == *other.m_liquifyProperties);

        result &=
            (m_liquifyWorker && other.m_liquifyWorker &&
             *m_liquifyWorker == *other.m_liquifyWorker)
            || m_liquifyWorker == other.m_liquifyWorker;

    } else if (m_mode == MESH) {
        result &= m_meshTransform == other.m_meshTransform;
    } else {
        KIS_SAFE_ASSERT_RECOVER_NOOP(0 && "unknown transform mode");
    }

    return result;
}

ToolTransformArgs::ToolTransformArgs(TransformMode mode,
                                     QPointF transformedCenter,
                                     QPointF originalCenter,
                                     QPointF rotationCenterOffset,
                                     bool transformAroundRotationCenter,
                                     double aX, double aY, double aZ,
                                     double scaleX, double scaleY,
                                     double shearX, double shearY,
                                     double boundsRotation,
                                     KisWarpTransformWorker::WarpType warpType,
                                     double alpha,
                                     bool defaultPoints,
                                     const QString &filterId,
                                     int pixelPrecision, int previewPixelPrecision,
                                     KisPaintDeviceSP externalSource)
    : m_mode(mode)
    , m_defaultPoints(defaultPoints)
    , m_origPoints {QVector<QPointF>()}
    , m_transfPoints {QVector<QPointF>()}
    , m_warpType(warpType)
    , m_alpha(alpha)
    , m_transformedCenter(transformedCenter)
    , m_originalCenter(originalCenter)
    , m_rotationCenterOffset(rotationCenterOffset)
    , m_transformAroundRotationCenter(transformAroundRotationCenter)
    , m_aX(aX)
    , m_aY(aY)
    , m_aZ(aZ)
    , m_scaleX(scaleX)
    , m_scaleY(scaleY)
    , m_shearX(shearX)
    , m_shearY(shearY)
    , m_boundsRotation(boundsRotation)
    , m_liquifyProperties(new KisLiquifyProperties())
    , m_pixelPrecision(pixelPrecision)
    , m_previewPixelPrecision(previewPixelPrecision)
    , m_externalSource(externalSource)
{
    setFilterId(filterId);
}


ToolTransformArgs::~ToolTransformArgs()
{
    clear();
}

void ToolTransformArgs::translateSrcAndDst(const QPointF &offset)
{
    transformSrcAndDst(QTransform::fromTranslate(offset.x(), offset.y()));
}

void ToolTransformArgs::transformSrcAndDst(const QTransform &t)
{
    if (m_mode == FREE_TRANSFORM ) {
        m_originalCenter = t.map(m_originalCenter);
        m_transformedCenter = t.map(m_transformedCenter);

        QMatrix4x4 m(t);
        m_cameraPos = m.map(m_cameraPos);
    } else if (m_mode == PERSPECTIVE_4POINT) {
        m_originalCenter = t.map(m_originalCenter);
        m_transformedCenter = t.map(m_transformedCenter);

        m_flattenedPerspectiveTransform = t.inverted() * m_flattenedPerspectiveTransform * t;

    } else if (m_mode == WARP || m_mode == CAGE || m_mode == PUPPET) {
        for (auto &pt : m_origPoints) {
            pt = t.map(pt);
        }

        for (auto &pt : m_transfPoints) {
            pt = t.map(pt);
        }
        if (m_mode == PUPPET && m_puppetMesh.isValid()) {
            m_puppetMesh.transform(t);
        }
    } else if (m_mode == LIQUIFY) {
        KIS_ASSERT_RECOVER_RETURN(m_liquifyWorker);
        m_liquifyWorker->transformSrcAndDst(t);
    } else if (m_mode == MESH) {
        m_meshTransform.transformSrcAndDst(t);
    } else {
        KIS_ASSERT_RECOVER_NOOP(0 && "unknown transform mode");
    }
}

void ToolTransformArgs::translateDstSpace(const QPointF &offset)
{
    if (m_mode == FREE_TRANSFORM || m_mode == PERSPECTIVE_4POINT) {
        m_transformedCenter += offset;
    } else if (m_mode == WARP || m_mode == CAGE || m_mode == PUPPET) {
        for (auto &pt : m_transfPoints) {
            pt += offset;
        }
    } else if (m_mode == LIQUIFY) {
        KIS_ASSERT_RECOVER_RETURN(m_liquifyWorker);
        m_liquifyWorker->translateDstSpace(offset);
    } else if (m_mode == MESH) {
        m_meshTransform.translate(offset);
    } else {
        KIS_ASSERT_RECOVER_NOOP(0 && "unknown transform mode");
    }
}

bool ToolTransformArgs::isIdentity() const
{
    if (m_mode == FREE_TRANSFORM) {
        return (m_transformedCenter == m_originalCenter && m_scaleX == 1
                && m_scaleY == 1 && m_shearX == 0 && m_shearY == 0
                && m_aX == 0 && m_aY == 0 && m_aZ == 0 && m_boundsRotation == 0);
    } else if (m_mode == PERSPECTIVE_4POINT) {
            return (m_transformedCenter == m_originalCenter && m_scaleX == 1
                    && m_scaleY == 1 && m_shearX == 0 && m_shearY == 0
                    && m_flattenedPerspectiveTransform.isIdentity());
    } else if (m_mode == WARP || m_mode == CAGE || m_mode == PUPPET) {
        for (int i = 0; i < m_origPoints.size(); ++i)
            if (m_origPoints[i] != m_transfPoints[i])
                return false;

        if (m_mode == PUPPET) {
            for (qreal rotation : m_puppetRotations) {
                if (!qFuzzyIsNull(rotation)) {
                    return false;
                }
            }
        }

        return true;
    } else if (m_mode == LIQUIFY) {
        return !m_liquifyWorker || m_liquifyWorker->isIdentity();
    } else if (m_mode == MESH) {
        return m_meshTransform.isIdentity();
    } else {
        KIS_ASSERT_RECOVER_NOOP(0 && "unknown transform mode");
        return true;
    }
}

bool ToolTransformArgs::isUnchanging() const
{
    return !m_externalSource && isIdentity();
}

void ToolTransformArgs::initLiquifyTransformMode(const QRect &srcRect)
{
    m_liquifyWorker.reset(new KisLiquifyTransformWorker(srcRect, 0, 8));
    m_liquifyProperties->loadAndResetMode();
}

void ToolTransformArgs::saveLiquifyTransformMode() const
{
    m_liquifyProperties->saveMode();
}

void ToolTransformArgs::toXML(QDomElement *e) const
{
    e->setAttribute("mode", (int) m_mode);

    QDomDocument doc = e->ownerDocument();

    if (m_mode == FREE_TRANSFORM || m_mode == PERSPECTIVE_4POINT) {

        QDomElement freeEl = doc.createElement("free_transform");
        e->appendChild(freeEl);

        KisDomUtils::saveValue(&freeEl, "transformedCenter", m_transformedCenter);
        KisDomUtils::saveValue(&freeEl, "originalCenter", m_originalCenter);
        KisDomUtils::saveValue(&freeEl, "rotationCenterOffset", m_rotationCenterOffset);
        KisDomUtils::saveValue(&freeEl, "transformAroundRotationCenter", m_transformAroundRotationCenter);

        KisDomUtils::saveValue(&freeEl, "aX", m_aX);
        KisDomUtils::saveValue(&freeEl, "aY", m_aY);
        KisDomUtils::saveValue(&freeEl, "aZ", m_aZ);

        KisDomUtils::saveValue(&freeEl, "cameraPos", m_cameraPos);

        KisDomUtils::saveValue(&freeEl, "scaleX", m_scaleX);
        KisDomUtils::saveValue(&freeEl, "scaleY", m_scaleY);

        KisDomUtils::saveValue(&freeEl, "shearX", m_shearX);
        KisDomUtils::saveValue(&freeEl, "shearY", m_shearY);

        // bounds rotation is currently not supported by transform masks, so it should be null
        KisDomUtils::saveValue(&freeEl, "boundsRotation", m_boundsRotation);

        KisDomUtils::saveValue(&freeEl, "keepAspectRatio", m_keepAspectRatio);
        KisDomUtils::saveValue(&freeEl, "flattenedPerspectiveTransform", m_flattenedPerspectiveTransform);

        KisDomUtils::saveValue(&freeEl, "filterId", m_filter->id());

    } else if (m_mode == WARP || m_mode == CAGE || m_mode == PUPPET) {
        QDomElement warpEl = doc.createElement("warp_transform");
        e->appendChild(warpEl);

        KisDomUtils::saveValue(&warpEl, "defaultPoints", m_defaultPoints);
        KisDomUtils::saveValue(&warpEl, "originalPoints", m_origPoints);
        KisDomUtils::saveValue(&warpEl, "transformedPoints", m_transfPoints);

        KisDomUtils::saveValue(&warpEl, "warpType", (int)m_warpType); // limited!
        KisDomUtils::saveValue(&warpEl, "alpha", m_alpha);

        if (m_mode == PUPPET) {
            KisDomUtils::saveValue(&warpEl, "rotations", m_puppetRotations);
            KisDomUtils::saveValue(&warpEl, "showMesh", m_puppetShowMesh);
            KisDomUtils::saveValue(&warpEl, "expansion", m_puppetExpansion);
            if (m_puppetMesh.isValid()) {
                KisDomUtils::saveValue(&warpEl, "mesh", m_puppetMesh.toString());
            }
            KisDomUtils::saveValue(&warpEl, "orders", m_puppetOrders);
        }

        if(m_mode == CAGE){
            KisDomUtils::saveValue(&warpEl,"pixelPrecision",m_pixelPrecision);
            KisDomUtils::saveValue(&warpEl,"previewPixelPrecision",m_previewPixelPrecision);
        }

    } else if (m_mode == LIQUIFY) {
        QDomElement liqEl = doc.createElement("liquify_transform");
        e->appendChild(liqEl);

        m_liquifyProperties->toXML(&liqEl);
        m_liquifyWorker->toXML(&liqEl);
    } else if (m_mode == MESH) {
        QDomElement meshEl = doc.createElement("mesh_transform");
        e->appendChild(meshEl);

        KisDomUtils::saveValue(&meshEl, "mesh", m_meshTransform);
    } else {
        KIS_ASSERT_RECOVER_RETURN(0 && "Unknown transform mode");
    }

    // m_editTransformPoints should not be saved since it is reset explicitly
}

ToolTransformArgs ToolTransformArgs::fromXML(const QDomElement &e)
{
    ToolTransformArgs args;

    int newMode = e.attribute("mode", "0").toInt();
    if (newMode < 0 || newMode >= N_MODES) return ToolTransformArgs();

    args.m_mode = (TransformMode) newMode;

    // reset explicitly
    args.m_editTransformPoints = false;

    bool result = false;

    if (args.m_mode == FREE_TRANSFORM || args.m_mode == PERSPECTIVE_4POINT) {

        QDomElement freeEl;

        QString filterId;

        result =
            KisDomUtils::findOnlyElement(e, "free_transform", &freeEl) &&

            KisDomUtils::loadValue(freeEl, "transformedCenter", &args.m_transformedCenter) &&
            KisDomUtils::loadValue(freeEl, "originalCenter", &args.m_originalCenter) &&
            KisDomUtils::loadValue(freeEl, "rotationCenterOffset", &args.m_rotationCenterOffset) &&

            KisDomUtils::loadValue(freeEl, "aX", &args.m_aX) &&
            KisDomUtils::loadValue(freeEl, "aY", &args.m_aY) &&
            KisDomUtils::loadValue(freeEl, "aZ", &args.m_aZ) &&

            KisDomUtils::loadValue(freeEl, "cameraPos", &args.m_cameraPos) &&

            KisDomUtils::loadValue(freeEl, "scaleX", &args.m_scaleX) &&
            KisDomUtils::loadValue(freeEl, "scaleY", &args.m_scaleY) &&

            KisDomUtils::loadValue(freeEl, "shearX", &args.m_shearX) &&
            KisDomUtils::loadValue(freeEl, "shearY", &args.m_shearY) &&

            KisDomUtils::loadValue(freeEl, "keepAspectRatio", &args.m_keepAspectRatio) &&
            KisDomUtils::loadValue(freeEl, "flattenedPerspectiveTransform", &args.m_flattenedPerspectiveTransform) &&
            KisDomUtils::loadValue(freeEl, "filterId", &filterId);

        // transformAroundRotationCenter is a new parameter introduced in Krita 4.0,
        // so it might be not present in older transform masks
        if (!KisDomUtils::loadValue(freeEl, "transformAroundRotationCenter", &args.m_transformAroundRotationCenter)) {
            args.m_transformAroundRotationCenter = false;
        }

        // bounds rotation is currently not supported by transform masks, so it should be null for them
        if (!KisDomUtils::loadValue(freeEl, "boundsRotation", &args.m_boundsRotation)) {
            args.m_boundsRotation = 0.0;
        }

        if (result) {
            args.m_filter = KisFilterStrategyRegistry::instance()->value(filterId);
            result = (bool) args.m_filter;
        }

    } else if (args.m_mode == WARP || args.m_mode == CAGE || args.m_mode == PUPPET) {
        QDomElement warpEl;

        int warpType = 0;

        result =
            KisDomUtils::findOnlyElement(e, "warp_transform", &warpEl) &&

            KisDomUtils::loadValue(warpEl, "defaultPoints", &args.m_defaultPoints) &&

            KisDomUtils::loadValue(warpEl, "originalPoints", &args.m_origPoints) &&
            KisDomUtils::loadValue(warpEl, "transformedPoints", &args.m_transfPoints) &&

            KisDomUtils::loadValue(warpEl, "warpType", &warpType) &&
            KisDomUtils::loadValue(warpEl, "alpha", &args.m_alpha);

        if(args.m_mode == CAGE){
            // Pixel precision is a parameter introduced in Krita 4.2, so we should
            // expect it not being present in older files. In case it is not found,
            // just use the default value initialized by c-tor (that is, do nothing).

            (void) KisDomUtils::loadValue(warpEl, "pixelPrecision", &args.m_pixelPrecision);
            (void) KisDomUtils::loadValue(warpEl, "previewPixelPrecision", &args.m_previewPixelPrecision);
        }

        if (args.m_mode == PUPPET) {
            (void)KisDomUtils::loadValue(warpEl, "rotations", &args.m_puppetRotations);
            args.m_puppetRotations.resize(args.m_origPoints.size());
            for (qreal &rotation : args.m_puppetRotations) {
                if (!std::isfinite(rotation)) {
                    rotation = 0.0;
                }
            }
            (void)KisDomUtils::loadValue(warpEl, "showMesh", &args.m_puppetShowMesh);
            (void)KisDomUtils::loadValue(warpEl, "expansion", &args.m_puppetExpansion);
            args.m_puppetExpansion = qBound(0, args.m_puppetExpansion, 64);
            // Missing in documents saved before the mesh solver: they keep the
            // legacy MLS deformation.
            QString mesh;
            if (KisDomUtils::loadValue(warpEl, "mesh", &mesh)) {
                args.m_puppetMesh = KisPuppetTransformWorker::Mesh::fromString(mesh);
            }
            // Missing in older documents: every pin at order 0.
            (void)KisDomUtils::loadValue(warpEl, "orders", &args.m_puppetOrders);
            args.m_puppetOrders.resize(args.m_origPoints.size());
        }

        if (result && warpType >= 0 && warpType < KisWarpTransformWorker::N_MODES) {
            args.m_warpType = (KisWarpTransformWorker::WarpType_) warpType;
        } else {
            result = false;
        }

    } else if (args.m_mode == LIQUIFY) {
        QDomElement liquifyEl;

        result =
            KisDomUtils::findOnlyElement(e, "liquify_transform", &liquifyEl);

        *args.m_liquifyProperties = KisLiquifyProperties::fromXML(e);
        args.m_liquifyWorker.reset(KisLiquifyTransformWorker::fromXML(e));
    } else if (args.m_mode == MESH) {
        QDomElement meshEl;

        result =
            KisDomUtils::findOnlyElement(e, "mesh_transform", &meshEl);

        result &= KisDomUtils::loadValue(meshEl, "mesh", &args.m_meshTransform);

    } else {
        KIS_ASSERT_RECOVER_NOOP(0 && "Unknown transform mode");
    }

    KIS_SAFE_ASSERT_RECOVER(result) {
        args = ToolTransformArgs();
    }

    return args;
}

void ToolTransformArgs::saveContinuedState()
{
    m_continuedTransformation.reset();
    m_continuedTransformation.reset(new ToolTransformArgs(*this));
}

void ToolTransformArgs::restoreContinuedState()
{
    std::unique_ptr<ToolTransformArgs> tempTransformation(
        new ToolTransformArgs(*m_continuedTransformation));

    *this = *tempTransformation;
    m_continuedTransformation.swap(tempTransformation);
}

const ToolTransformArgs* ToolTransformArgs::continuedTransform() const
{
    return m_continuedTransformation.get();
}

const KisBezierTransformMesh *ToolTransformArgs::meshTransform() const
{
    return &m_meshTransform;
}

KisBezierTransformMesh *ToolTransformArgs::meshTransform()
{
    return &m_meshTransform;
}

bool ToolTransformArgs::meshShowHandles() const
{
    return m_meshShowHandles;
}

void ToolTransformArgs::setMeshShowHandles(bool value)
{
    m_meshShowHandles = value;

    KConfigGroup configGroup =  KSharedConfig::openConfig()->group("KisToolTransform");
    configGroup.writeEntry("meshShowHandles", value);
}

bool ToolTransformArgs::meshSymmetricalHandles() const
{
    return m_meshSymmetricalHandles;
}

void ToolTransformArgs::setMeshSymmetricalHandles(bool value)
{
    m_meshSymmetricalHandles = value;

    KConfigGroup configGroup =  KSharedConfig::openConfig()->group("KisToolTransform");
    configGroup.writeEntry("meshSymmetricalHandles", value);
}

void ToolTransformArgs::scale3dSrcAndDst(qreal scale)
{
    const QTransform t = QTransform::fromScale(scale, scale);

    if (m_mode == FREE_TRANSFORM ) {
        m_originalCenter = t.map(m_originalCenter);
        m_transformedCenter = t.map(m_transformedCenter);

        // we need to scale Z-coordinate of the camera pos as well,
        // so we cannot just do `QMatrix4x4 m(t)`.
        QMatrix4x4 m;
        m.scale(scale);
        m_cameraPos = m.map(m_cameraPos);
    } else {
        transformSrcAndDst(t);
    }
}
