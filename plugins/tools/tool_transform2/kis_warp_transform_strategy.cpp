/*
 *  SPDX-FileCopyrightText: 2014 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_warp_transform_strategy.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>

#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QQueue>
#include <QtMath>

#include "kis_coordinates_converter.h"
#include "tool_transform_args.h"
#include "transform_transaction_properties.h"
#include "kis_painting_tweaks.h"
#include "kis_cursor.h"
#include "kis_transform_utils.h"
#include "kis_algebra_2d.h"
#include "KisHandlePainterHelper.h"
#include "kis_signal_compressor.h"



struct KisWarpTransformStrategy::Private
{
    Private(KisWarpTransformStrategy *_q,
            const KisCoordinatesConverter *_converter,
            ToolTransformArgs &_currentArgs,
            TransformTransactionProperties &_transaction)
        : q(_q),
          converter(_converter),
          currentArgs(_currentArgs),
          transaction(_transaction),
          recalculateSignalCompressor(40, KisSignalCompressor::FIRST_ACTIVE)
    {
    }

    KisWarpTransformStrategy * const q;

    /// standard members ///

    const KisCoordinatesConverter *converter {0};

    //////
    ToolTransformArgs &currentArgs;
    //////
    TransformTransactionProperties &transaction;

    QTransform paintingTransform;
    QPointF paintingOffset;

    QTransform handlesTransform;

    /// custom members ///

    QImage transformedImage;

    int pointIndexUnderCursor {0};

    enum Mode {
        OVER_POINT = 0,
        MULTIPLE_POINT_SELECTION,
        MOVE_MODE,
        ROTATE_MODE,
        SCALE_MODE,
        ROTATE_PIN,
        DELETE_POINT,
        RUBBER_BAND, // Puppet: rectangle selection of pins
        NOTHING
    };
    Mode mode {NOTHING};

    QVector<int> pointsInAction;
    int lastNumPoints {0};

    bool drawConnectionLines {false}; // useful while developing
    bool drawOrigPoints {false};
    bool drawTransfPoints {true};
    bool closeOnStartPointClick {false};
    bool clipOriginalPointsPosition {true};
    QPointF pointPosOnClick;
    bool pointWasDragged {false};

    QPointF lastMousePos;

    // Puppet pin selection
    bool shiftOnHover{false};
    bool rubberBandActive{false};
    QPointF rubberBandStart;
    QPointF rubberBandEnd;
    QVector<int> rubberBandBaseSelection;

    mutable QImage puppetMask;
    QImage puppetMaskSource; ///< setPuppetMaskSource(); used without a thumbnail
    mutable qint64 puppetMaskCacheKey{-1};
    mutable QRectF puppetMaskBounds;
    mutable int puppetMaskExpansion{-1};

    // Mesh solver (KisPuppetTransformWorker), cached between repaints.
    mutable std::unique_ptr<KisPuppetTransformWorker> puppetWorker;
    mutable KisPuppetTransformWorker::Mesh puppetWorkerMesh;
    mutable QVector<QPointF> puppetWorkerOriginal;
    mutable QVector<QPointF> puppetWorkerTransformed;
    mutable QVector<qreal> puppetWorkerRotations;
    mutable QVector<int> puppetWorkerOrders;
    // Image <-> preview thumbnail space of the last recalculation.
    std::function<QPointF(const QPointF &)> imageToThumbMap;
    std::function<QPointF(const QPointF &)> thumbToImageMap;

    // cage transform also uses this logic. This helps this class know what transform type we are using
    TransformType transformType = TransformType::WARP_TRANSFORM;
    KisSignalCompressor recalculateSignalCompressor;

    void recalculateTransformations();
    void drawPuppetMesh(QPainter &gc) const;
    void updatePuppetMask() const;
    bool puppetMaskContains(const QPointF &point) const;
    void ensurePuppetMesh();
    const KisPuppetTransformWorker *cachedPuppetWorker() const;
    inline QPointF imageToThumb(const QPointF &pt, bool useFlakeOptimization);

    bool shouldCloseTheCage() const;
    QVector<QPointF*> getSelectedPoints(QPointF *center, bool limitToSelectedOnly = false) const;
};

KisWarpTransformStrategy::KisWarpTransformStrategy(const KisCoordinatesConverter *converter,
                                                   KoSnapGuide *snapGuide,
                                                   ToolTransformArgs &currentArgs,
                                                   TransformTransactionProperties &transaction)
    : KisSimplifiedActionPolicyStrategy(converter, snapGuide),
      m_d(new Private(this, converter, currentArgs, transaction))
{
    connect(&m_d->recalculateSignalCompressor, SIGNAL(timeout()),
            SLOT(recalculateTransformations()));
}

KisWarpTransformStrategy::~KisWarpTransformStrategy()
{
}

void KisWarpTransformStrategy::setTransformFunction(const QPointF &mousePos, bool perspectiveModifierActive, bool shiftModifierActive, bool altModifierActive)
{
    m_d->shiftOnHover = shiftModifierActive;
    const double handleRadius = KisTransformUtils::effectiveHandleGrabRadius(m_d->converter);

    bool cursorOverPoint = false;
    m_d->pointIndexUnderCursor = -1;

    KisTransformUtils::HandleChooser<Private::Mode>
        handleChooser(mousePos, Private::NOTHING);

    const QVector<QPointF> &points = m_d->currentArgs.transfPoints();
    for (int i = 0; i < points.size(); ++i) {
        if (handleChooser.addFunction(points[i],
                                      handleRadius, Private::NOTHING)) {

            cursorOverPoint = true;
            m_d->pointIndexUnderCursor = i;
        }
    }

    if (!cursorOverPoint && m_d->transformType == TransformType::PUPPET_TRANSFORM
        && !m_d->currentArgs.isEditingTransformPoints()) {
        // The ring is drawn at 24 screen pixels (paint()), the handle grab
        // radius is KisTransformUtils::handleRadius (8) screen pixels.
        const qreal rotationRadius = 3.0 * handleRadius;
        const qreal ringTolerance = 0.65 * handleRadius;
        qreal closestDistance = std::numeric_limits<qreal>::max();
        for (int i = 0; i < points.size(); ++i) {
            const qreal distance = QLineF(mousePos, points[i]).length();
            const qreal distanceFromRing = qAbs(distance - rotationRadius);
            if (distanceFromRing <= ringTolerance && distanceFromRing < closestDistance) {
                closestDistance = distanceFromRing;
                m_d->pointIndexUnderCursor = i;
            }
        }
        if (m_d->pointIndexUnderCursor >= 0) {
            m_d->mode = Private::ROTATE_PIN;
            return;
        }
    }

    const bool puppet = m_d->transformType == TransformType::PUPPET_TRANSFORM;
    if (cursorOverPoint && puppet) {
        // Puppet: the layer sampler modifier (Ctrl+Alt in the default input
        // profile) or the "Delete pin" click action deletes; Shift or Ctrl
        // toggles the pin in the selection.
        const bool deletes =
            altModifierActive || m_d->currentArgs.puppetClickAction() == ToolTransformArgs::PuppetDeletePin;
        m_d->mode = deletes ? Private::DELETE_POINT
            : (shiftModifierActive || perspectiveModifierActive) && !m_d->currentArgs.isEditingTransformPoints()
            ? Private::MULTIPLE_POINT_SELECTION
            : Private::OVER_POINT;
    } else if (cursorOverPoint) {
        m_d->mode = perspectiveModifierActive && !m_d->currentArgs.isEditingTransformPoints()
            ? Private::MULTIPLE_POINT_SELECTION
            : Private::OVER_POINT;

    } else if (puppet && !m_d->currentArgs.isEditingTransformPoints()) {
        m_d->mode = Private::RUBBER_BAND;
    } else if (!m_d->currentArgs.isEditingTransformPoints() && m_d->transformType != TransformType::PUPPET_TRANSFORM) {
        QPolygonF polygon(m_d->currentArgs.transfPoints());
        bool insidePolygon = polygon.boundingRect().contains(mousePos);
        m_d->mode = insidePolygon ? Private::MOVE_MODE :
            !perspectiveModifierActive ? Private::ROTATE_MODE :
            Private::SCALE_MODE;
    } else {
        m_d->mode = Private::NOTHING;
    }
}

QCursor KisWarpTransformStrategy::getCurrentCursor() const
{
    QCursor cursor;

    switch (m_d->mode) {
    case Private::OVER_POINT:
        cursor = KisCursor::pointingHandCursor();
        break;
    case Private::MULTIPLE_POINT_SELECTION:
        cursor = KisCursor::crossCursor();
        break;
    case Private::MOVE_MODE:
        cursor = KisCursor::moveCursor();
        break;
    case Private::ROTATE_MODE:
        cursor = KisCursor::rotateCursor();
        break;
    case Private::SCALE_MODE:
        cursor = KisCursor::sizeVerCursor();
        break;
    case Private::ROTATE_PIN:
        cursor = KisCursor::rotateCursor();
        break;
    case Private::DELETE_POINT:
        cursor = Qt::ForbiddenCursor;
        break;
    case Private::RUBBER_BAND:
        cursor = KisCursor::arrowCursor();
        break;
    case Private::NOTHING:
        cursor = KisCursor::arrowCursor();
        break;
    }

    return cursor;
}

void KisWarpTransformStrategy::overrideDrawingItems(bool drawConnectionLines,
                                                    bool drawOrigPoints,
                                                    bool drawTransfPoints)
{
    m_d->drawConnectionLines = drawConnectionLines;
    m_d->drawOrigPoints = drawOrigPoints;
    m_d->drawTransfPoints = drawTransfPoints;
}

void KisWarpTransformStrategy::setCloseOnStartPointClick(bool value)
{
    m_d->closeOnStartPointClick = value;
}

void KisWarpTransformStrategy::setClipOriginalPointsPosition(bool value)
{
    m_d->clipOriginalPointsPosition = value;
}

void KisWarpTransformStrategy::setTransformType(TransformType type) {
    m_d->transformType = type;
}

void KisWarpTransformStrategy::Private::updatePuppetMask() const
{
    const QImage source = q->originalImage().isNull() ? puppetMaskSource : q->originalImage();
    const QRectF bounds = transaction.originalRect();
    if (source.isNull() || bounds.isEmpty()) {
        puppetMask = QImage();
        return;
    }

    if (puppetMaskCacheKey == source.cacheKey() && puppetMaskBounds == bounds
        && puppetMaskExpansion == currentArgs.puppetExpansion()) {
        return;
    }

    const int width = source.width();
    const int height = source.height();
    QVector<quint8> content(width * height, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            content[y * width + x] = qAlpha(source.pixel(x, y)) > 8;
        }
    }

    QVector<quint8> outside(width * height, 0);
    QQueue<int> queue;
    auto enqueueOutside = [&](int x, int y) {
        const int index = y * width + x;
        if (!content[index] && !outside[index]) {
            outside[index] = 1;
            queue.enqueue(index);
        }
    };
    for (int x = 0; x < width; ++x) {
        enqueueOutside(x, 0);
        enqueueOutside(x, height - 1);
    }
    for (int y = 0; y < height; ++y) {
        enqueueOutside(0, y);
        enqueueOutside(width - 1, y);
    }
    while (!queue.isEmpty()) {
        const int index = queue.dequeue();
        const QPoint cell(index % width, index / width);
        static const QPoint neighbours[] = {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)};
        for (const QPoint &offset : neighbours) {
            const QPoint next = cell + offset;
            if (next.x() >= 0 && next.x() < width && next.y() >= 0 && next.y() < height) {
                enqueueOutside(next.x(), next.y());
            }
        }
    }

    for (int i = 0; i < content.size(); ++i) {
        content[i] = !outside[i];
    }

    const int radiusX = qCeil(currentArgs.puppetExpansion() * width / bounds.width());
    const int radiusY = qCeil(currentArgs.puppetExpansion() * height / bounds.height());
    if (radiusX > 0) {
        QVector<quint8> horizontal(content.size(), 0);
        for (int y = 0; y < height; ++y) {
            int lastContent = -width - radiusX;
            for (int x = 0; x < width; ++x) {
                if (content[y * width + x])
                    lastContent = x;
                horizontal[y * width + x] = x - lastContent <= radiusX;
            }
            lastContent = 2 * width + radiusX;
            for (int x = width - 1; x >= 0; --x) {
                if (content[y * width + x])
                    lastContent = x;
                horizontal[y * width + x] |= lastContent - x <= radiusX;
            }
        }
        content.swap(horizontal);
    }
    if (radiusY > 0) {
        QVector<quint8> vertical(content.size(), 0);
        for (int x = 0; x < width; ++x) {
            int lastContent = -height - radiusY;
            for (int y = 0; y < height; ++y) {
                if (content[y * width + x])
                    lastContent = y;
                vertical[y * width + x] = y - lastContent <= radiusY;
            }
            lastContent = 2 * height + radiusY;
            for (int y = height - 1; y >= 0; --y) {
                if (content[y * width + x])
                    lastContent = y;
                vertical[y * width + x] |= lastContent - y <= radiusY;
            }
        }
        content.swap(vertical);
    }

    puppetMask = QImage(width, height, QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y) {
        uchar *line = puppetMask.scanLine(y);
        for (int x = 0; x < width; ++x) {
            line[x] = content[y * width + x] ? 255 : 0;
        }
    }
    puppetMaskCacheKey = source.cacheKey();
    puppetMaskBounds = bounds;
    puppetMaskExpansion = currentArgs.puppetExpansion();
}

void KisWarpTransformStrategy::setPuppetMaskSource(const QImage &image)
{
    m_d->puppetMaskSource = image;
}

void KisWarpTransformStrategy::Private::ensurePuppetMesh()
{
    // The mesh is built once from the artwork and stored in the arguments, so
    // the preview, the overlay and the final rendering share it. It is rebuilt
    // only when the expansion changes.
    if (transformType != TransformType::PUPPET_TRANSFORM) {
        return;
    }
    const KisPuppetTransformWorker::Mesh &mesh = currentArgs.puppetMesh();
    if (mesh.isValid() && mesh.expansion == currentArgs.puppetExpansion()) {
        return;
    }
    updatePuppetMask();
    if (puppetMask.isNull()) {
        return;
    }
    currentArgs.setPuppetMesh(
        KisPuppetTransformWorker::Mesh::build(puppetMask, puppetMaskBounds, currentArgs.puppetExpansion()));
}

const KisPuppetTransformWorker *KisWarpTransformStrategy::Private::cachedPuppetWorker() const
{
    if (!currentArgs.usesPuppetMesh()) {
        return nullptr;
    }
    if (!puppetWorker || puppetWorkerMesh != currentArgs.puppetMesh()
        || puppetWorkerOriginal != currentArgs.origPoints() || puppetWorkerTransformed != currentArgs.transfPoints()
        || puppetWorkerRotations != currentArgs.puppetRotations() || puppetWorkerOrders != currentArgs.puppetOrders()) {
        puppetWorker.reset(new KisPuppetTransformWorker(currentArgs.createPuppetWorker()));
        puppetWorkerMesh = currentArgs.puppetMesh();
        puppetWorkerOriginal = currentArgs.origPoints();
        puppetWorkerTransformed = currentArgs.transfPoints();
        puppetWorkerRotations = currentArgs.puppetRotations();
        puppetWorkerOrders = currentArgs.puppetOrders();
    }
    return puppetWorker.get();
}

bool KisWarpTransformStrategy::Private::puppetMaskContains(const QPointF &point) const
{
    if (puppetMask.isNull() || !puppetMaskBounds.contains(point)) {
        return false;
    }
    const int x = qBound(0,
                         qFloor((point.x() - puppetMaskBounds.left()) * puppetMask.width() / puppetMaskBounds.width()),
                         puppetMask.width() - 1);
    const int y = qBound(0,
                         qFloor((point.y() - puppetMaskBounds.top()) * puppetMask.height() / puppetMaskBounds.height()),
                         puppetMask.height() - 1);
    return puppetMask.constScanLine(y)[x] != 0;
}

void KisWarpTransformStrategy::Private::drawPuppetMesh(QPainter &gc) const
{
    if (!currentArgs.puppetShowMesh()) {
        return;
    }

    updatePuppetMask();
    const QRectF bounds = transaction.originalRect();
    if (puppetMask.isNull() || bounds.isEmpty()) {
        return;
    }

    // With the mesh solver the overlay shows its triangles (clipped to the
    // artwork); otherwise the legacy display grid.
    const KisPuppetTransformWorker *worker = cachedPuppetWorker();
    const KisPuppetTransformWorker::Mesh *mesh = worker ? &worker->mesh() : nullptr;
    const int columns = mesh ? mesh->columns : qBound(4, qCeil(bounds.width() / 32.0), 48);
    const int rows = mesh ? mesh->rows : qBound(4, qCeil(bounds.height() / 32.0), 48);
    auto gridPoint = [&](int column, int row) {
        return mesh
            ? mesh->vertex(column, row)
            : QPointF(bounds.left() + bounds.width() * column / columns, bounds.top() + bounds.height() * row / rows);
    };

    QPen meshPen(QColor(72, 72, 72, 150));
    meshPen.setCosmetic(true);
    meshPen.setWidth(qMax(1, q->decorationThickness()));
    gc.setPen(meshPen);
    gc.setBrush(Qt::NoBrush);

    QVector<QPointF> originalPoints;
    QVector<QPointF> transformedPoints;
    currentArgs.puppetControlPoints(currentArgs.origPoints(),
                                    currentArgs.transfPoints(),
                                    &originalPoints,
                                    &transformedPoints);
    auto transformMeshPoint = [&](const QPointF &point) {
        if (currentArgs.isEditingTransformPoints() || originalPoints.isEmpty()) {
            return point;
        }
        if (worker) {
            return worker->map(point);
        }
        switch (currentArgs.warpType()) {
        case KisWarpTransformWorker::AFFINE_TRANSFORM:
            return KisWarpTransformWorker::affineTransformMath(point,
                                                               originalPoints,
                                                               transformedPoints,
                                                               currentArgs.alpha());
        case KisWarpTransformWorker::SIMILITUDE_TRANSFORM:
            return KisWarpTransformWorker::similitudeTransformMath(point,
                                                                   originalPoints,
                                                                   transformedPoints,
                                                                   currentArgs.alpha());
        case KisWarpTransformWorker::RIGID_TRANSFORM:
            return KisWarpTransformWorker::rigidTransformMath(point,
                                                              originalPoints,
                                                              transformedPoints,
                                                              currentArgs.alpha());
        default:
            return point;
        }
    };

    QPainterPath meshPath;
    auto addClippedLine = [&](const QPointF &start, const QPointF &end) {
        const qreal sourceDx = qAbs(end.x() - start.x()) * puppetMask.width() / bounds.width();
        const qreal sourceDy = qAbs(end.y() - start.y()) * puppetMask.height() / bounds.height();
        const int steps = qMax(1, qCeil(qMax(sourceDx, sourceDy)));
        bool drawing = false;
        for (int step = 0; step <= steps; ++step) {
            const QPointF point = start + (end - start) * (qreal(step) / steps);
            if (puppetMaskContains(point)) {
                const QPointF transformedPoint = transformMeshPoint(point);
                if (!drawing) {
                    meshPath.moveTo(transformedPoint);
                    drawing = true;
                } else {
                    meshPath.lineTo(transformedPoint);
                }
            } else {
                drawing = false;
            }
        }
    };

    for (int row = 0; row <= rows; ++row) {
        addClippedLine(gridPoint(0, row), gridPoint(columns, row));
    }
    for (int column = 0; column <= columns; ++column) {
        addClippedLine(gridPoint(column, 0), gridPoint(column, rows));
    }
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const QPointF topLeft = gridPoint(column, row);
            const QPointF topRight = gridPoint(column + 1, row);
            const QPointF bottomLeft = gridPoint(column, row + 1);
            const QPointF bottomRight = gridPoint(column + 1, row + 1);
            addClippedLine((row + column) % 2 ? topRight : topLeft, (row + column) % 2 ? bottomLeft : bottomRight);
        }
    }
    gc.drawPath(meshPath);
}

void KisWarpTransformStrategy::drawConnectionLines(QPainter &gc,
                                                   const QVector<QPointF> &origPoints,
                                                   const QVector<QPointF> &transfPoints,
                                                   bool isEditingPoints)
{
    Q_UNUSED(isEditingPoints);

    QPen antsPen;
    QPen outlinePen;

    KisPaintingTweaks::initAntsPen(&antsPen, &outlinePen);
    antsPen.setWidth(decorationThickness());
    outlinePen.setWidth(decorationThickness());

    const int numPoints = origPoints.size();

    for (int i = 0; i < numPoints; ++i) {
        gc.setPen(outlinePen);
        gc.drawLine(transfPoints[i], origPoints[i]);
        gc.setPen(antsPen);
        gc.drawLine(transfPoints[i], origPoints[i]);
    }
}

void KisWarpTransformStrategy::paint(QPainter &gc)
{
    // Draw preview image

    gc.save();

    gc.setOpacity(m_d->transaction.basePreviewOpacity());
    gc.setTransform(m_d->paintingTransform, true);
    gc.drawImage(m_d->paintingOffset, m_d->transformedImage);

    gc.restore();


    gc.save();
    gc.setTransform(m_d->handlesTransform, true);

    if (m_d->drawConnectionLines) {
        gc.setOpacity(0.5);

        drawConnectionLines(gc,
                            m_d->currentArgs.origPoints(),
                            m_d->currentArgs.transfPoints(),
                            m_d->currentArgs.isEditingTransformPoints());
    }

    if (m_d->transformType == TransformType::PUPPET_TRANSFORM) {
        m_d->drawPuppetMesh(gc);
    }

    QPen mainPen(Qt::black);
    mainPen.setCosmetic(true);
    mainPen.setWidth(decorationThickness());
    QPen outlinePen(Qt::white);
    outlinePen.setCosmetic(true);
    outlinePen.setWidth(decorationThickness());

    // draw handles
    {
        const int numPoints = m_d->currentArgs.origPoints().size();



        qreal handlesExtraScale = KisTransformUtils::scaleFromAffineMatrix(m_d->handlesTransform);

        qreal dstIn = 8 / handlesExtraScale;
        qreal dstOut = 10 / handlesExtraScale;
        qreal srcIn = 6 / handlesExtraScale;
        qreal srcOut = 6 / handlesExtraScale;

        QRectF handleRect1(-0.5 * dstIn, -0.5 * dstIn, dstIn, dstIn);
        QRectF handleRect2(-0.5 * dstOut, -0.5 * dstOut, dstOut, dstOut);

        if (m_d->drawTransfPoints) {
            gc.setOpacity(1.0);

            for (int i = 0; i < numPoints; ++i) {
                if (m_d->transformType == TransformType::PUPPET_TRANSFORM
                    && !m_d->currentArgs.isEditingTransformPoints()) {
                    // Keep in sync with the ring hit test in setTransformFunction().
                    const qreal ringRadius = 24.0 / handlesExtraScale;
                    const QRectF ringRect(-ringRadius, -ringRadius, 2.0 * ringRadius, 2.0 * ringRadius);
                    gc.setPen(outlinePen);
                    const QPointF direction(qCos(m_d->currentArgs.puppetRotation(i)) * ringRadius,
                                            qSin(m_d->currentArgs.puppetRotation(i)) * ringRadius);
                    gc.drawEllipse(ringRect
                                       .adjusted(-2.0 / handlesExtraScale,
                                                 -2.0 / handlesExtraScale,
                                                 2.0 / handlesExtraScale,
                                                 2.0 / handlesExtraScale)
                                       .translated(m_d->currentArgs.transfPoints()[i]));
                    gc.drawLine(m_d->currentArgs.transfPoints()[i], m_d->currentArgs.transfPoints()[i] + direction);
                    gc.setPen(mainPen);
                    gc.drawEllipse(ringRect.translated(m_d->currentArgs.transfPoints()[i]));
                    gc.drawLine(m_d->currentArgs.transfPoints()[i], m_d->currentArgs.transfPoints()[i] + direction);
                }
                gc.setPen(outlinePen);
                gc.drawEllipse(handleRect2.translated(m_d->currentArgs.transfPoints()[i]));
                gc.setPen(mainPen);
                gc.drawEllipse(handleRect1.translated(m_d->currentArgs.transfPoints()[i]));
            }

            if (m_d->rubberBandActive) {
                QPen bandPen(QColor(255, 255, 255, 220));
                bandPen.setCosmetic(true);
                bandPen.setStyle(Qt::DashLine);
                gc.setPen(bandPen);
                gc.setBrush(QColor(80, 140, 255, 40));
                gc.drawRect(QRectF(m_d->rubberBandStart, m_d->rubberBandEnd).normalized());
                gc.setBrush(Qt::NoBrush);
            }

            QPointF center;
            QVector<QPointF*> selectedPoints = m_d->getSelectedPoints(&center, true);

            QBrush selectionBrush = selectedPoints.size() > 1 ? Qt::red : Qt::black;

            QBrush oldBrush = gc.brush();
            gc.setBrush(selectionBrush);
            Q_FOREACH (const QPointF *pt, selectedPoints) {
                gc.drawEllipse(handleRect1.translated(*pt));
            }
            gc.setBrush(oldBrush);

        }

        if (m_d->drawOrigPoints) {
            QPainterPath inLine;
            inLine.moveTo(-0.5 * srcIn,            0);
            inLine.lineTo( 0.5 * srcIn,            0);
            inLine.moveTo(           0, -0.5 * srcIn);
            inLine.lineTo(           0,  0.5 * srcIn);

            QPainterPath outLine;
            outLine.moveTo(-0.5 * srcOut, -0.5 * srcOut);
            outLine.lineTo( 0.5 * srcOut, -0.5 * srcOut);
            outLine.lineTo( 0.5 * srcOut,  0.5 * srcOut);
            outLine.lineTo(-0.5 * srcOut,  0.5 * srcOut);
            outLine.lineTo(-0.5 * srcOut, -0.5 * srcOut);

            gc.setOpacity(0.5);

            for (int i = 0; i < numPoints; ++i) {
                gc.setPen(outlinePen);
                gc.drawPath(outLine.translated(m_d->currentArgs.origPoints()[i]));
                gc.setPen(mainPen);
                gc.drawPath(inLine.translated(m_d->currentArgs.origPoints()[i]));
            }
        }

    }

    // draw grid lines only if we are using the GRID mode. Also only use this logic for warp, not cage transforms
    if (m_d->currentArgs.warpCalculation() == KisWarpTransformWorker::WarpCalculation::GRID &&
        m_d->transformType == TransformType::WARP_TRANSFORM ) {

    // see how many rows we have. we are only going to do lines up to 6 divisions/
    // it is almost impossible to use with 6 even.
    const int numPoints = m_d->currentArgs.origPoints().size();

    // grid is always square, so get the square root to find # of rows
    int rowsInWarp = sqrt(m_d->currentArgs.origPoints().size());


        KisHandlePainterHelper handlePainter(&gc, 0.0, decorationThickness());
        handlePainter.setHandleStyle(KisHandleStyle::primarySelection());

        // draw horizontal lines
        for (int i = 0; i < numPoints; i++) {
            if (i != 0 &&  i % rowsInWarp == rowsInWarp -1) {
                // skip line if it is the last in the row
            } else {
                handlePainter.drawConnectionLine(m_d->currentArgs.transfPoints()[i], m_d->currentArgs.transfPoints()[i+1]  );
            }
        }

        // draw vertical lines
        for (int i = 0; i < numPoints; i++) {

            if ( (numPoints - i - 1) < rowsInWarp ) {
                // last row doesn't need to draw vertical lines
            } else {
                handlePainter.drawConnectionLine(m_d->currentArgs.transfPoints()[i], m_d->currentArgs.transfPoints()[i+rowsInWarp] );
            }
        }

    } // end if statement

    gc.restore();
}

void KisWarpTransformStrategy::flushPendingRecalculation()
{
    // The compressor delivers a drag's latest arguments to the stroke up to
    // 40 ms later; Apply in that time would end the stroke with older ones.
    if (m_d->recalculateSignalCompressor.isActive()) {
        m_d->recalculateSignalCompressor.stop();
        m_d->recalculateTransformations();
    }
}

void KisWarpTransformStrategy::externalConfigChanged()
{
    if (m_d->lastNumPoints != m_d->currentArgs.transfPoints().size()) {
        m_d->pointsInAction.clear();
    }

    m_d->recalculateTransformations();
}

bool KisWarpTransformStrategy::beginPrimaryAction(const QPointF &pt)
{
    const bool pointMode = m_d->mode == Private::OVER_POINT || m_d->mode == Private::MULTIPLE_POINT_SELECTION
        || m_d->mode == Private::ROTATE_PIN || m_d->mode == Private::DELETE_POINT;
    if (pointMode
        && (m_d->pointIndexUnderCursor < 0 || m_d->pointIndexUnderCursor >= m_d->currentArgs.transfPoints().size())) {
        // The hover that set this mode belonged to other points: a new
        // transform started, and the press came without a hover move, as
        // the first mouse event after the canvas switched from the pen
        // (KoToolManager keeps one tool per input device). Decide for the
        // press position, as that hover would have.
        setTransformFunction(pt, false, m_d->shiftOnHover, false);
    }

    const bool isEditingPoints = m_d->currentArgs.isEditingTransformPoints();
    bool retval = false;

    if (m_d->mode == Private::OVER_POINT || m_d->mode == Private::MULTIPLE_POINT_SELECTION
        || m_d->mode == Private::MOVE_MODE || m_d->mode == Private::ROTATE_MODE || m_d->mode == Private::SCALE_MODE
        || m_d->mode == Private::ROTATE_PIN || m_d->mode == Private::DELETE_POINT
        || m_d->mode == Private::RUBBER_BAND) {
        retval = true;

    } else if (isEditingPoints) {
        QPointF newPos = m_d->clipOriginalPointsPosition ?
            KisTransformUtils::clipInRect(pt, m_d->transaction.originalRect()) :
            pt;

        m_d->currentArgs.refOriginalPoints().append(newPos);
        m_d->currentArgs.refTransformedPoints().append(newPos);
        m_d->currentArgs.setPuppetRotation(m_d->currentArgs.origPoints().size() - 1, 0.0);
        m_d->currentArgs.setPuppetOrder(m_d->currentArgs.origPoints().size() - 1, 0);

        m_d->mode = Private::OVER_POINT;
        m_d->pointIndexUnderCursor = m_d->currentArgs.origPoints().size() - 1;

        m_d->recalculateSignalCompressor.start();

        retval = true;
    }

    if (m_d->mode == Private::DELETE_POINT) {
        m_d->currentArgs.removePuppetPoint(m_d->pointIndexUnderCursor);
        m_d->pointsInAction.clear();
        m_d->pointIndexUnderCursor = -1;
        m_d->recalculateSignalCompressor.start();
    } else if (m_d->mode == Private::OVER_POINT) {
        m_d->pointPosOnClick =
            m_d->currentArgs.transfPoints()[m_d->pointIndexUnderCursor];
        m_d->pointWasDragged = false;

        // Puppet: dragging a pin of a multiple selection moves all of them.
        const bool keepSelection = m_d->transformType == TransformType::PUPPET_TRANSFORM && !isEditingPoints
            && m_d->pointsInAction.size() > 1 && m_d->pointsInAction.contains(m_d->pointIndexUnderCursor);
        if (!keepSelection) {
            m_d->pointsInAction.clear();
            m_d->pointsInAction << m_d->pointIndexUnderCursor;
        }
        m_d->lastNumPoints = m_d->currentArgs.transfPoints().size();
    } else if (m_d->mode == Private::RUBBER_BAND) {
        m_d->rubberBandActive = true;
        m_d->rubberBandStart = pt;
        m_d->rubberBandEnd = pt;
        m_d->rubberBandBaseSelection = m_d->shiftOnHover ? m_d->pointsInAction : QVector<int>();
        m_d->pointsInAction = m_d->rubberBandBaseSelection;
        m_d->lastNumPoints = m_d->currentArgs.transfPoints().size();
    } else if (m_d->mode == Private::MULTIPLE_POINT_SELECTION) {
        QVector<int>::iterator it =
            std::find(m_d->pointsInAction.begin(),
                      m_d->pointsInAction.end(),
                      m_d->pointIndexUnderCursor);

        if (it != m_d->pointsInAction.end()) {
            m_d->pointsInAction.erase(it);
        } else {
            m_d->pointsInAction << m_d->pointIndexUnderCursor;
        }

        m_d->lastNumPoints = m_d->currentArgs.transfPoints().size();
    }

    m_d->lastMousePos = pt;
    return retval;
}

QVector<QPointF*> KisWarpTransformStrategy::Private::getSelectedPoints(QPointF *center, bool limitToSelectedOnly) const
{
    QVector<QPointF> &points = currentArgs.refTransformedPoints();

    QRectF boundingRect;
    QVector<QPointF*> selectedPoints;
    if (limitToSelectedOnly || pointsInAction.size() > 1) {
        Q_FOREACH (int index, pointsInAction) {
            selectedPoints << &points[index];
            KisAlgebra2D::accumulateBounds(points[index], &boundingRect);
        }
    } else {
        QVector<QPointF>::iterator it = points.begin();
        QVector<QPointF>::iterator end = points.end();
        for (; it != end; ++it) {
            selectedPoints << &(*it);
            KisAlgebra2D::accumulateBounds(*it, &boundingRect);
        }
    }

    *center = boundingRect.center();
    return selectedPoints;
}

void KisWarpTransformStrategy::continuePrimaryAction(const QPointF &pt, bool shiftModifierActive, bool altModifierActive)
{
    Q_UNUSED(shiftModifierActive);
    Q_UNUSED(altModifierActive);

    // toplevel code switches to HOVER mode if nothing is selected
    KIS_ASSERT_RECOVER_RETURN(
        m_d->mode == Private::MOVE_MODE || m_d->mode == Private::ROTATE_MODE || m_d->mode == Private::SCALE_MODE
        || m_d->mode == Private::ROTATE_PIN || m_d->mode == Private::DELETE_POINT || m_d->mode == Private::RUBBER_BAND
        || (m_d->mode == Private::OVER_POINT && m_d->pointIndexUnderCursor >= 0 && !m_d->pointsInAction.isEmpty())
        || (m_d->mode == Private::MULTIPLE_POINT_SELECTION && m_d->pointIndexUnderCursor >= 0));

    if (m_d->mode == Private::DELETE_POINT) {
        return;
    } else if (m_d->mode == Private::RUBBER_BAND) {
        m_d->rubberBandEnd = pt;
        const QRectF rect = QRectF(m_d->rubberBandStart, m_d->rubberBandEnd).normalized();
        m_d->pointsInAction = m_d->rubberBandBaseSelection;
        for (int i = 0; i < m_d->currentArgs.transfPoints().size(); ++i) {
            if (rect.contains(m_d->currentArgs.transfPoints()[i]) && !m_d->pointsInAction.contains(i)) {
                m_d->pointsInAction << i;
            }
        }
        m_d->lastMousePos = pt;
        // Selection only: no need to recalculate the deformation.
        Q_EMIT requestCanvasUpdate();
        return;
    } else if (m_d->mode == Private::ROTATE_PIN) {
        const QPointF center = m_d->currentArgs.transfPoint(m_d->pointIndexUnderCursor);
        const QPointF oldDirection = m_d->lastMousePos - center;
        const QPointF newDirection = pt - center;
        if (!qFuzzyIsNull(QLineF(QPointF(), oldDirection).length())
            && !qFuzzyIsNull(QLineF(QPointF(), newDirection).length())) {
            // angleBetweenVectors() is a difference of atan2() values: crossing
            // the -pi/pi cut would add a full turn to the stored rotation.
            const qreal angle =
                std::remainder(KisAlgebra2D::angleBetweenVectors(oldDirection, newDirection), 2.0 * M_PI);
            // Puppet: rotating a pin of a multiple selection turns all of them
            // about that pin.
            const bool group =
                m_d->pointsInAction.size() > 1 && m_d->pointsInAction.contains(m_d->pointIndexUnderCursor);
            const QVector<int> pins = group ? m_d->pointsInAction : QVector<int>{m_d->pointIndexUnderCursor};
            QTransform rotation;
            rotation.rotateRadians(angle);
            for (int index : pins) {
                m_d->currentArgs.setPuppetRotation(index, m_d->currentArgs.puppetRotation(index) + angle);
                if (index != m_d->pointIndexUnderCursor) {
                    m_d->currentArgs.transfPoint(index) =
                        center + rotation.map(m_d->currentArgs.transfPoint(index) - center);
                }
            }
        }
    } else if (m_d->mode == Private::OVER_POINT) {
        if (m_d->currentArgs.isEditingTransformPoints()) {
            QPointF newPos = m_d->clipOriginalPointsPosition ?
                KisTransformUtils::clipInRect(pt, m_d->transaction.originalRect()) :
                pt;
            m_d->currentArgs.origPoint(m_d->pointIndexUnderCursor) = newPos;
            m_d->currentArgs.transfPoint(m_d->pointIndexUnderCursor) = newPos;
        } else if (m_d->transformType == TransformType::PUPPET_TRANSFORM && m_d->pointsInAction.size() > 1) {
            const QPointF delta = pt - m_d->currentArgs.transfPoint(m_d->pointIndexUnderCursor);
            for (int index : m_d->pointsInAction) {
                m_d->currentArgs.transfPoint(index) += delta;
            }
        } else {
            m_d->currentArgs.transfPoint(m_d->pointIndexUnderCursor) = pt;
        }

        const qreal handleRadiusSq = pow2(KisTransformUtils::effectiveHandleGrabRadius(m_d->converter));
        qreal dist =
            kisSquareDistance(
                m_d->currentArgs.transfPoint(m_d->pointIndexUnderCursor),
                m_d->pointPosOnClick);

        if (dist > handleRadiusSq) {
            m_d->pointWasDragged = true;
        }
    } else if (m_d->mode == Private::MOVE_MODE) {
        QPointF center;
        QVector<QPointF*> selectedPoints = m_d->getSelectedPoints(&center);

        QPointF diff = pt - m_d->lastMousePos;

        QVector<QPointF*>::iterator it = selectedPoints.begin();
        QVector<QPointF*>::iterator end = selectedPoints.end();
        for (; it != end; ++it) {
            **it += diff;
        }
    } else if (m_d->mode == Private::ROTATE_MODE) {
        QPointF center;
        QVector<QPointF*> selectedPoints = m_d->getSelectedPoints(&center);

        QPointF oldDirection = m_d->lastMousePos - center;
        QPointF newDirection = pt - center;

        qreal rotateAngle = KisAlgebra2D::angleBetweenVectors(oldDirection, newDirection);
        QTransform R;
        R.rotateRadians(rotateAngle);

        QTransform t =
            QTransform::fromTranslate(-center.x(), -center.y()) *
            R *
            QTransform::fromTranslate(center.x(), center.y());

        QVector<QPointF*>::iterator it = selectedPoints.begin();
        QVector<QPointF*>::iterator end = selectedPoints.end();
        for (; it != end; ++it) {
            **it = t.map(**it);
        }
    } else if (m_d->mode == Private::SCALE_MODE) {
        QPointF center;
        QVector<QPointF*> selectedPoints = m_d->getSelectedPoints(&center);

        QPolygonF polygon(m_d->currentArgs.origPoints());
        QSizeF maxSize = polygon.boundingRect().size();
        qreal maxDimension = qMax(maxSize.width(), maxSize.height());

        qreal scale = 1.0 - (pt - m_d->lastMousePos).y() / maxDimension;

        QTransform t =
            QTransform::fromTranslate(-center.x(), -center.y()) *
            QTransform::fromScale(scale, scale) *
            QTransform::fromTranslate(center.x(), center.y());

        QVector<QPointF*>::iterator it = selectedPoints.begin();
        QVector<QPointF*>::iterator end = selectedPoints.end();
        for (; it != end; ++it) {
            **it = t.map(**it);
        }
    }

    m_d->lastMousePos = pt;
    m_d->recalculateSignalCompressor.start();

}

bool KisWarpTransformStrategy::changePuppetOrder(int change)
{
    QVector<int> pins;
    for (int index : m_d->pointsInAction) {
        if (index >= 0 && index < m_d->currentArgs.origPoints().size()) {
            pins << index;
        }
    }
    if (pins.isEmpty()) {
        return false;
    }
    m_d->currentArgs.changePuppetOrder(pins, ToolTransformArgs::PuppetOrderChange(change));
    m_d->recalculateTransformations();
    return true;
}

bool KisWarpTransformStrategy::Private::shouldCloseTheCage() const
{
    return currentArgs.isEditingTransformPoints() &&
        closeOnStartPointClick &&
        pointIndexUnderCursor == 0 &&
        currentArgs.origPoints().size() > 2 &&
        !pointWasDragged;
}

bool KisWarpTransformStrategy::acceptsClicks() const
{
    // Puppet: a pin deleted by a click (on press) must be committed, and a
    // click on empty canvas ends the rubber band (and clears the selection).
    return m_d->shouldCloseTheCage() || m_d->currentArgs.isEditingTransformPoints()
        || m_d->mode == Private::DELETE_POINT || m_d->mode == Private::RUBBER_BAND;
}

bool KisWarpTransformStrategy::endPrimaryAction()
{
    if (m_d->mode == Private::RUBBER_BAND && m_d->rubberBandActive) {
        m_d->rubberBandActive = false;
        // A plain click on empty canvas clears the selection.
        const qreal radius = KisTransformUtils::effectiveHandleGrabRadius(m_d->converter);
        if (QLineF(m_d->rubberBandStart, m_d->rubberBandEnd).length() < radius
            && m_d->rubberBandBaseSelection.isEmpty()) {
            m_d->pointsInAction.clear();
        }
        Q_EMIT requestCanvasUpdate();
        // Selection only: nothing to commit (an undo step would be a no-op).
        return false;
    }
    if (m_d->shouldCloseTheCage()) {
        m_d->currentArgs.setEditingTransformPoints(false);
    }

    return true;
}

inline QPointF KisWarpTransformStrategy::Private::imageToThumb(const QPointF &pt, bool useFlakeOptimization)
{
    return useFlakeOptimization ? converter->imageToDocument(converter->documentToFlake((pt))) : q->thumbToImageTransform().inverted().map(pt);
}

void KisWarpTransformStrategy::Private::recalculateTransformations()
{
    ensurePuppetMesh();

    QTransform scaleTransform = KisTransformUtils::imageToFlakeTransform(converter);

    QTransform resultThumbTransform = q->thumbToImageTransform() * scaleTransform;
    qreal scale = KisTransformUtils::scaleFromAffineMatrix(resultThumbTransform);
    bool useFlakeOptimization = scale < 1.0 &&
        !KisTransformUtils::thumbnailTooSmall(resultThumbTransform, q->originalImage().rect());

    {
        // Affine maps between image and thumbnail space for the mesh preview.
        const QPointF origin = imageToThumb(QPointF(0, 0), useFlakeOptimization);
        const QPointF xAxis = imageToThumb(QPointF(1, 0), useFlakeOptimization) - origin;
        const QPointF yAxis = imageToThumb(QPointF(0, 1), useFlakeOptimization) - origin;
        const QTransform toThumb(xAxis.x(), xAxis.y(), yAxis.x(), yAxis.y(), origin.x(), origin.y());
        const QTransform toImage = toThumb.inverted();
        imageToThumbMap = [toThumb](const QPointF &point) {
            return toThumb.map(point);
        };
        thumbToImageMap = [toImage](const QPointF &point) {
            return toImage.map(point);
        };
    }

    QVector<QPointF> thumbOrigPoints(currentArgs.numPoints());
    QVector<QPointF> thumbTransfPoints(currentArgs.numPoints());

    for (int i = 0; i < currentArgs.numPoints(); ++i) {
        thumbOrigPoints[i] = imageToThumb(currentArgs.origPoints()[i], useFlakeOptimization);
        thumbTransfPoints[i] = imageToThumb(currentArgs.transfPoints()[i], useFlakeOptimization);
    }

    paintingOffset = transaction.originalTopLeft();

    if (!q->originalImage().isNull() && !currentArgs.isEditingTransformPoints()) {
        QPointF origTLInFlake = imageToThumb(transaction.originalTopLeft(), useFlakeOptimization);

        if (useFlakeOptimization) {
            transformedImage = q->originalImage().transformed(resultThumbTransform);
            paintingTransform = QTransform();
        } else {
            transformedImage = q->originalImage();
            paintingTransform = resultThumbTransform;

        }

        transformedImage = q->calculateTransformedImage(currentArgs,
                                                        transformedImage,
                                                        thumbOrigPoints,
                                                        thumbTransfPoints,
                                                        origTLInFlake,
                                                        &paintingOffset);
    } else {
        transformedImage = q->originalImage();
        paintingOffset = imageToThumb(transaction.originalTopLeft(), false);
        paintingTransform = resultThumbTransform;
    }

    handlesTransform = scaleTransform;
    Q_EMIT q->requestCanvasUpdate();
    Q_EMIT q->requestImageRecalculation();
}

QImage KisWarpTransformStrategy::calculateTransformedImage(ToolTransformArgs &currentArgs,
                                                           const QImage &srcImage,
                                                           const QVector<QPointF> &origPoints,
                                                           const QVector<QPointF> &transfPoints,
                                                           const QPointF &srcOffset,
                                                           QPointF *dstOffset)
{
    if (m_d->transformType == TransformType::PUPPET_TRANSFORM && currentArgs.usesPuppetMesh() && m_d->imageToThumbMap) {
        // The mesh lives in image space; the preview image in thumbnail space.
        const KisPuppetTransformWorker *worker = m_d->cachedPuppetWorker();
        return worker->runOnQImage(srcImage, srcOffset, m_d->imageToThumbMap, m_d->thumbToImageMap, dstOffset);
    }
    QVector<QPointF> effectiveOriginalPoints = origPoints;
    QVector<QPointF> effectiveTransformedPoints = transfPoints;
    if (m_d->transformType == TransformType::PUPPET_TRANSFORM) {
        currentArgs.puppetControlPoints(origPoints,
                                        transfPoints,
                                        &effectiveOriginalPoints,
                                        &effectiveTransformedPoints);
    }
    return KisWarpTransformWorker::transformQImage(currentArgs.warpType(),
                                                   effectiveOriginalPoints,
                                                   effectiveTransformedPoints,
                                                   currentArgs.alpha(),
                                                   srcImage,
                                                   srcOffset,
                                                   dstOffset);
}

#include "moc_kis_warp_transform_strategy.cpp"
