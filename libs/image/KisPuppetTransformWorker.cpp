/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisPuppetTransformWorker.h"

#include <QDebug>
#include <QPainter>
#include <QStringList>
#include <QtMath>

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

#include <KoColorSpace.h>
#include <KoCompositeOpRegistry.h>

#include "kis_algebra_2d.h"
#include "kis_grid_interpolation_tools.h"
#include "kis_paint_device.h"
#include "kis_painter.h"
#include "kis_sequential_iterator.h"
#include "kis_warptransform_worker.h"

#include "gpu/KisGpuGridWarpWorker.h"

namespace
{
constexpr qreal TargetCellSize = 24.0;
constexpr int MinimumCells = 4;
constexpr int MaximumCells = 64;
/// Relative stiffness of triangles without artwork: they keep the mapping
/// continuous but barely couple separate limbs.
constexpr double EmptyStiffness = 1e-3;
/// Pin constraints: effectively hard compared to edge weights of about 0.5.
constexpr double PinWeight = 1e4;
constexpr int Iterations = 40;
/// Geodesic cost factor of an edge with no solid triangle (empty space).
constexpr double EmptyPathCost = 50.0;

struct Corner {
    int index;
    double weight;
};

using Triangle = std::array<Corner, 3>;

/// Grid coordinates (u, v): vertex (c, r) sits at integer (c, r).
QPointF toGrid(const KisPuppetTransformWorker::Mesh &mesh, const QPointF &point)
{
    const QPointF d = point - mesh.origin;
    const qreal a = mesh.columnStep.x();
    const qreal b = mesh.rowStep.x();
    const qreal c = mesh.columnStep.y();
    const qreal e = mesh.rowStep.y();
    const qreal det = a * e - b * c;
    if (qFuzzyIsNull(det))
        return QPointF();
    return QPointF((e * d.x() - b * d.y()) / det, (-c * d.x() + a * d.y()) / det);
}

/// Which half of cell (c, r) contains local (s, t); diagonals alternate.
int triangleHalf(int column, int row, qreal s, qreal t)
{
    if ((column + row) % 2 == 0)
        return s >= t ? 0 : 1; // diagonal top-left to bottom-right
    return s + t <= 1.0 ? 0 : 1; // diagonal top-right to bottom-left
}

int vertexIndex(const KisPuppetTransformWorker::Mesh &mesh, int column, int row)
{
    return row * (mesh.columns + 1) + column;
}

/// The triangle of @p point with its barycentric weights (extrapolated outside the grid).
Triangle locate(const KisPuppetTransformWorker::Mesh &mesh,
                const QPointF &point,
                int *cellColumn = nullptr,
                int *cellRow = nullptr,
                int *cellHalf = nullptr)
{
    const QPointF grid = toGrid(mesh, point);
    const int column = qBound(0, int(std::floor(grid.x())), mesh.columns - 1);
    const int row = qBound(0, int(std::floor(grid.y())), mesh.rows - 1);
    const qreal s = grid.x() - column;
    const qreal t = grid.y() - row;
    const int half = triangleHalf(column, row, s, t);
    const int tl = vertexIndex(mesh, column, row);
    const int tr = vertexIndex(mesh, column + 1, row);
    const int bl = vertexIndex(mesh, column, row + 1);
    const int br = vertexIndex(mesh, column + 1, row + 1);
    if (cellColumn)
        *cellColumn = column;
    if (cellRow)
        *cellRow = row;
    if (cellHalf)
        *cellHalf = half;
    if ((column + row) % 2 == 0) {
        if (half == 0)
            return {{{tl, 1.0 - s}, {tr, s - t}, {br, t}}};
        return {{{tl, 1.0 - t}, {br, s}, {bl, t - s}}};
    }
    if (half == 0)
        return {{{tl, 1.0 - s - t}, {tr, s}, {bl, t}}};
    return {{{tr, 1.0 - t}, {br, s + t - 1.0}, {bl, 1.0 - s}}};
}

/// Vertex indices of triangle @p half of cell (c, r), counter-clockwise order not required.
std::array<int, 3> triangleVertices(const KisPuppetTransformWorker::Mesh &mesh, int column, int row, int half)
{
    const int tl = vertexIndex(mesh, column, row);
    const int tr = vertexIndex(mesh, column + 1, row);
    const int bl = vertexIndex(mesh, column, row + 1);
    const int br = vertexIndex(mesh, column + 1, row + 1);
    if ((column + row) % 2 == 0)
        return half == 0 ? std::array<int, 3>{tl, tr, br} : std::array<int, 3>{tl, br, bl};
    return half == 0 ? std::array<int, 3>{tl, tr, bl} : std::array<int, 3>{tr, br, bl};
}

double cotangent(const QPointF &apex, const QPointF &a, const QPointF &b)
{
    const QPointF u = a - apex;
    const QPointF v = b - apex;
    const double cross = std::abs(u.x() * v.y() - u.y() * v.x());
    if (cross < 1e-12)
        return 0.0;
    return (u.x() * v.x() + u.y() * v.y()) / cross;
}

bool isFinite(const QPointF &point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

QPointF rotated(const QPointF &offset, qreal angle)
{
    const qreal c = std::cos(angle);
    const qreal s = std::sin(angle);
    return QPointF(offset.x() * c - offset.y() * s, offset.x() * s + offset.y() * c);
}

/**
 * Which blocks of the source hold a non-transparent pixel. Rendering cells
 * over fully transparent source are skipped: they would only write
 * transparent pixels, and the grid polygon ops overwrite, so squeezed empty
 * space would cut holes into folded artwork. Unlike a test against the
 * mesh's solid triangles (built from a 2000 px thumbnail and an alpha
 * threshold), this never drops a visible pixel: faint pixels, thin lines,
 * specks between triangles and content painted after the mesh was built.
 */
class SourceCoverage
{
public:
    /// Block edge in source pixels.
    static constexpr int BlockSize = 8;
    /// The sampling footprint around a cell: bilinear sampling near a cell's
    /// edge reads the pixels next to it.
    static constexpr qreal Margin = 2.0;

    /// @p origin: the position of source pixel (0, 0).
    SourceCoverage(const QPointF &origin, const QSize &size)
        : m_origin(origin)
        , m_columns((size.width() + BlockSize - 1) / BlockSize)
        , m_rows((size.height() + BlockSize - 1) / BlockSize)
        , m_bits(m_columns * m_rows)
    {
    }

    static SourceCoverage fromDevice(KisPaintDeviceSP device, const QRect &rect)
    {
        SourceCoverage coverage(rect.topLeft(), rect.size());
        if (rect.isEmpty())
            return coverage;
        const KoColorSpace *cs = device->colorSpace();
        std::vector<quint8> alpha;
        KisSequentialConstIterator it(device, rect);
        int count = it.nConseqPixels();
        while (it.nextPixels(count)) {
            count = it.nConseqPixels();
            alpha.resize(size_t(count));
            cs->copyOpacityU8(const_cast<quint8 *>(it.rawDataConst()), alpha.data(), count);
            const int x = it.x() - rect.left();
            const int y = it.y() - rect.top();
            for (int i = 0; i < count; ++i) {
                if (alpha[size_t(i)]) {
                    coverage.mark(x + i, y);
                    // The rest of this block is already marked.
                    i = ((x + i) / BlockSize + 1) * BlockSize - x - 1;
                }
            }
        }
        return coverage;
    }

    static SourceCoverage fromImage(const QImage &image, const QPointF &origin)
    {
        SourceCoverage coverage(origin, image.size());
        for (int y = 0; y < image.height(); ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(line[x])) {
                    coverage.mark(x, y);
                    x = (x / BlockSize + 1) * BlockSize - 1;
                }
            }
        }
        return coverage;
    }

    /// Whether @p cell (source coordinates) may sample a non-transparent pixel.
    bool covers(const QPolygonF &cell) const
    {
        const QRectF rect = cell.boundingRect().translated(-m_origin).adjusted(-Margin, -Margin, Margin, Margin);
        const int left = qMax(0, int(std::floor(rect.left())) / BlockSize);
        const int top = qMax(0, int(std::floor(rect.top())) / BlockSize);
        const int right = qMin(m_columns - 1, int(std::floor(rect.right())) / BlockSize);
        const int bottom = qMin(m_rows - 1, int(std::floor(rect.bottom())) / BlockSize);
        for (int row = top; row <= bottom; ++row) {
            for (int column = left; column <= right; ++column) {
                if (m_bits.testBit(row * m_columns + column))
                    return true;
            }
        }
        return false;
    }

private:
    void mark(int x, int y)
    {
        m_bits.setBit((y / BlockSize) * m_columns + x / BlockSize);
    }

    QPointF m_origin;
    int m_columns;
    int m_rows;
    QBitArray m_bits;
};

/// Forwards only the grid cells of one stacking order to @p Op.
template<class Op>
struct OrderFilterOp {
    Op &op;
    const KisPuppetTransformWorker &worker;
    const SourceCoverage &coverage;
    int order; ///< the owning pin of this group (stackingGroups())
    bool filterOrder; ///< false: one pass with every group
    std::function<QPointF(const QPointF &)> toImage; ///< empty: already image coordinates
    void operator()(const QPolygonF &srcPolygon, const QPolygonF &dstPolygon)
    {
        if (!coverage.covers(srcPolygon))
            return;
        if (filterOrder) {
            const QPointF center = srcPolygon.boundingRect().center();
            if (worker.ownerAt(toImage ? toImage(center) : center) != order)
                return;
        }
        op(srcPolygon, dstPolygon);
    }
};

/// Forwards each grid cell to the recorder of its stacking group, with the
/// cell filter of OrderFilterOp: one grid pass records every group.
struct GroupRecordersOp {
    const KisPuppetTransformWorker &worker;
    const SourceCoverage &coverage;
    const QVector<int> &groups; ///< stackingGroups()
    std::vector<std::unique_ptr<KisGpuGridWarpWorker::Recorder>> &recorders;
    void operator()(const QPolygonF &srcPolygon, const QPolygonF &dstPolygon)
    {
        if (!coverage.covers(srcPolygon))
            return;
        const int group = groups.indexOf(worker.ownerAt(srcPolygon.boundingRect().center()));
        if (group >= 0)
            (*recorders[size_t(group)])(srcPolygon, dstPolygon);
    }
};
} // namespace

bool KisPuppetTransformWorker::Mesh::isValid() const
{
    return columns > 0 && rows > 0 && columns <= MaximumCells && rows <= MaximumCells
        && solid.size() == 2 * columns * rows && isFinite(origin) && isFinite(columnStep) && isFinite(rowStep)
        && !qFuzzyIsNull(columnStep.x() * rowStep.y() - columnStep.y() * rowStep.x());
}

bool KisPuppetTransformWorker::Mesh::operator==(const Mesh &other) const
{
    return origin == other.origin && columnStep == other.columnStep && rowStep == other.rowStep
        && columns == other.columns && rows == other.rows && solid == other.solid && expansion == other.expansion;
}

void KisPuppetTransformWorker::Mesh::transform(const QTransform &t)
{
    const QPointF newOrigin = t.map(origin);
    columnStep = t.map(origin + columnStep) - newOrigin;
    rowStep = t.map(origin + rowStep) - newOrigin;
    origin = newOrigin;
}

QPointF KisPuppetTransformWorker::Mesh::vertex(int column, int row) const
{
    return origin + column * columnStep + row * rowStep;
}

qreal KisPuppetTransformWorker::Mesh::cellSize() const
{
    return qMin(KisAlgebra2D::norm(columnStep), KisAlgebra2D::norm(rowStep));
}

bool KisPuppetTransformWorker::Mesh::triangleSolid(int column, int row, int half) const
{
    return solid.testBit(2 * (row * columns + column) + half);
}

QString KisPuppetTransformWorker::Mesh::toString() const
{
    if (!isValid())
        return QString();
    QString bits;
    bits.reserve((solid.size() + 3) / 4);
    for (int i = 0; i < solid.size(); i += 4) {
        int nibble = 0;
        for (int j = 0; j < 4 && i + j < solid.size(); ++j)
            nibble |= solid.testBit(i + j) ? 1 << j : 0;
        bits += QString::number(nibble, 16);
    }
    auto number = [](qreal value) {
        return QString::number(value, 'g', 17);
    };
    return QStringList{number(origin.x()),
                       number(origin.y()),
                       number(columnStep.x()),
                       number(columnStep.y()),
                       number(rowStep.x()),
                       number(rowStep.y()),
                       QString::number(columns),
                       QString::number(rows),
                       QString::number(expansion),
                       bits}
        .join(QLatin1Char(';'));
}

KisPuppetTransformWorker::Mesh KisPuppetTransformWorker::Mesh::fromString(const QString &text)
{
    Mesh mesh;
    const QStringList parts = text.split(QLatin1Char(';'));
    if (parts.size() != 10)
        return Mesh();
    bool numbersOk = true;
    auto value = [&](int index) {
        bool good = false;
        const qreal v = parts[index].toDouble(&good);
        numbersOk = numbersOk && good && std::isfinite(v);
        return v;
    };
    mesh.origin = QPointF(value(0), value(1));
    mesh.columnStep = QPointF(value(2), value(3));
    mesh.rowStep = QPointF(value(4), value(5));
    bool columnsOk = false;
    bool rowsOk = false;
    bool expansionOk = false;
    mesh.columns = parts[6].toInt(&columnsOk);
    mesh.rows = parts[7].toInt(&rowsOk);
    mesh.expansion = parts[8].toInt(&expansionOk);
    // Mesh::build() never makes more than MaximumCells per side; a larger
    // count only comes from a damaged file and would allocate a huge solve.
    if (!numbersOk || !columnsOk || !rowsOk || !expansionOk || mesh.columns <= 0 || mesh.rows <= 0
        || mesh.columns > MaximumCells || mesh.rows > MaximumCells)
        return Mesh();
    const int bitCount = 2 * mesh.columns * mesh.rows;
    const QString &bits = parts[9];
    if (bits.size() != (bitCount + 3) / 4)
        return Mesh();
    mesh.solid = QBitArray(bitCount);
    for (int i = 0; i < bits.size(); ++i) {
        bool ok = false;
        const int nibble = QString(bits[i]).toInt(&ok, 16);
        if (!ok)
            return Mesh();
        for (int j = 0; j < 4 && 4 * i + j < bitCount; ++j)
            mesh.solid.setBit(4 * i + j, nibble & (1 << j));
    }
    return mesh.isValid() ? mesh : Mesh();
}

QSize KisPuppetTransformWorker::Mesh::gridSize(const QRectF &bounds)
{
    return QSize(qBound(MinimumCells, qCeil(bounds.width() / TargetCellSize), MaximumCells),
                 qBound(MinimumCells, qCeil(bounds.height() / TargetCellSize), MaximumCells));
}

KisPuppetTransformWorker::Mesh
KisPuppetTransformWorker::Mesh::build(const QImage &mask, const QRectF &bounds, int expansion)
{
    Mesh mesh;
    if (bounds.isEmpty())
        return mesh;
    const QSize grid = gridSize(bounds);
    mesh.origin = bounds.topLeft();
    mesh.columns = grid.width();
    mesh.rows = grid.height();
    mesh.columnStep = QPointF(bounds.width() / mesh.columns, 0.0);
    mesh.rowStep = QPointF(0.0, bounds.height() / mesh.rows);
    mesh.solid = QBitArray(2 * mesh.columns * mesh.rows);
    mesh.expansion = expansion;
    if (mask.isNull())
        return mesh;

    const QImage gray =
        mask.format() == QImage::Format_Grayscale8 ? mask : mask.convertToFormat(QImage::Format_Grayscale8);
    const qreal scaleX = bounds.width() / gray.width();
    const qreal scaleY = bounds.height() / gray.height();
    for (int y = 0; y < gray.height(); ++y) {
        const uchar *line = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            if (!line[x])
                continue;
            const QPointF center(bounds.left() + (x + 0.5) * scaleX, bounds.top() + (y + 0.5) * scaleY);
            int column = 0;
            int row = 0;
            int half = 0;
            (void)locate(mesh, center, &column, &row, &half);
            mesh.solid.setBit(2 * (row * mesh.columns + column) + half);
        }
    }
    return mesh;
}

qreal KisPuppetTransformWorker::pinRadius(const Mesh &mesh)
{
    return 1.0 * mesh.cellSize();
}

KisPuppetTransformWorker::KisPuppetTransformWorker(const Mesh &mesh,
                                                   const QVector<QPointF> &originalPins,
                                                   const QVector<QPointF> &transformedPins,
                                                   const QVector<qreal> &pinRotations,
                                                   const QVector<int> &pinOrders)
    : m_mesh(mesh)
    , m_orders(pinOrders)
{
    if (!m_mesh.isValid())
        return;
    m_deformed.resize((m_mesh.columns + 1) * (m_mesh.rows + 1));
    for (int row = 0; row <= m_mesh.rows; ++row)
        for (int column = 0; column <= m_mesh.columns; ++column)
            m_deformed[vertexIndex(m_mesh, column, row)] = m_mesh.vertex(column, row);
    solve(originalPins, transformedPins, pinRotations);
}

bool KisPuppetTransformWorker::isValid() const
{
    return m_mesh.isValid();
}

bool KisPuppetTransformWorker::isIdentity() const
{
    return m_identity;
}

const QVector<QPointF> &KisPuppetTransformWorker::deformedVertices() const
{
    return m_deformed;
}

const KisPuppetTransformWorker::Mesh &KisPuppetTransformWorker::mesh() const
{
    return m_mesh;
}

void KisPuppetTransformWorker::solve(const QVector<QPointF> &originalPins,
                                     const QVector<QPointF> &transformedPins,
                                     const QVector<qreal> &pinRotations)
{
    const int pinCount = qMin(originalPins.size(), transformedPins.size());
    m_identity = true;
    for (int i = 0; i < pinCount; ++i) {
        const qreal rotation = i < pinRotations.size() ? pinRotations[i] : 0.0;
        if (!isFinite(originalPins[i]) || !isFinite(transformedPins[i]) || !std::isfinite(rotation)) {
            // Damaged arguments: render the source unchanged instead of
            // spreading NaN through the solve and the rendering grid.
            qWarning() << "KisPuppetTransformWorker: non-finite pin" << i << "; the transform is ignored";
            m_identity = true;
            return;
        }
        if (originalPins[i] != transformedPins[i] || !qFuzzyIsNull(rotation))
            m_identity = false;
    }
    if (m_identity || pinCount == 0)
        return;

    const int n = m_deformed.size();
    const QVector<QPointF> rest = m_deformed;

    // Ownership: every vertex belongs to the pin nearest to it along the
    // artwork (geodesic over the mesh; empty space is expensive).
    std::vector<std::vector<std::pair<int, double>>> neighbours(n);
    for (int row = 0; row < m_mesh.rows; ++row) {
        for (int column = 0; column < m_mesh.columns; ++column) {
            for (int half = 0; half < 2; ++half) {
                const std::array<int, 3> v = triangleVertices(m_mesh, column, row, half);
                const double factor = m_mesh.triangleSolid(column, row, half) ? 1.0 : EmptyPathCost;
                for (int k = 0; k < 3; ++k) {
                    const int a = v[k];
                    const int b = v[(k + 1) % 3];
                    const double cost = factor * KisAlgebra2D::norm(rest[a] - rest[b]);
                    neighbours[a].push_back({b, cost});
                    neighbours[b].push_back({a, cost});
                }
            }
        }
    }
    std::vector<double> distance(n, std::numeric_limits<double>::infinity());
    std::vector<int> owner(n, -1);
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    for (int i = 0; i < pinCount; ++i) {
        const Triangle corners = locate(m_mesh, originalPins[i]);
        for (const Corner &corner : corners) {
            const double d = KisAlgebra2D::norm(rest[corner.index] - originalPins[i]);
            if (d < distance[corner.index]) {
                distance[corner.index] = d;
                owner[corner.index] = i;
                queue.push({d, corner.index});
            }
        }
    }
    while (!queue.empty()) {
        const Item item = queue.top();
        queue.pop();
        if (item.first > distance[item.second])
            continue;
        for (const auto &edge : neighbours[item.second]) {
            const double d = item.first + edge.second;
            if (d < distance[edge.first]) {
                distance[edge.first] = d;
                owner[edge.first] = owner[item.second];
                queue.push({d, edge.first});
            }
        }
    }
    m_owner = QVector<int>(owner.begin(), owner.end());

    // Hinges: a pin's rotation turns only the side of the pin away from its
    // neighbouring pins (pins whose parts border its part), like a joint
    // turning the limb beyond it. Points toward a neighbour only follow the
    // pin's position; rotating them too twisted the neighbour's side of the
    // joint and pushed the artwork aside, leaving a gap.
    QVector<QVector<QPointF>> neighbourDirections(pinCount);
    for (int row = 0; row < m_mesh.rows; ++row) {
        for (int column = 0; column < m_mesh.columns; ++column) {
            for (int half = 0; half < 2; ++half) {
                if (!m_mesh.triangleSolid(column, row, half))
                    continue;
                const std::array<int, 3> v = triangleVertices(m_mesh, column, row, half);
                for (int k = 0; k < 3; ++k) {
                    const int a = owner[v[k]];
                    const int b = owner[v[(k + 1) % 3]];
                    if (a < 0 || b < 0 || a == b)
                        continue;
                    const QPointF ab = originalPins[b] - originalPins[a];
                    const qreal length = KisAlgebra2D::norm(ab);
                    if (length <= 0)
                        continue;
                    if (!neighbourDirections[a].contains(ab / length))
                        neighbourDirections[a] << ab / length;
                    if (!neighbourDirections[b].contains(-ab / length))
                        neighbourDirections[b] << -ab / length;
                }
            }
        }
    }
    // Whether @p offset from a rotated pin points toward one of its neighbours.
    auto towardNeighbour = [&](int pin, const QPointF &offset) {
        const qreal rotation = pin < pinRotations.size() ? pinRotations[pin] : 0.0;
        const qreal length = KisAlgebra2D::norm(offset);
        if (qFuzzyIsNull(rotation) || length <= 0)
            return false;
        for (const QPointF &direction : neighbourDirections[pin]) {
            if ((offset.x() * direction.x() + offset.y() * direction.y()) / length > 0.5)
                return true;
        }
        return false;
    };
    auto pinOffset = [&](int pin, const QPointF &offset) {
        const qreal rotation = pin < pinRotations.size() ? pinRotations[pin] : 0.0;
        return towardNeighbour(pin, offset) ? offset : rotated(offset, rotation);
    };

    // Pin constraints: center and four rigidly attached neighbours.
    const qreal radius = pinRadius(m_mesh);
    const QPointF offsets[] = {QPointF(),
                               QPointF(radius, 0),
                               QPointF(-radius, 0),
                               QPointF(0, radius),
                               QPointF(0, -radius)};
    QVector<Triangle> constraintCorners;
    QVector<QPointF> constraintTargets;
    QVector<QPointF> controlOriginal;
    QVector<QPointF> controlTransformed;
    for (int i = 0; i < pinCount; ++i) {
        for (const QPointF &offset : offsets) {
            // The neighbour's side of a rotated joint stays unconstrained, so
            // it can fold under the turned part instead of being dragged.
            if (towardNeighbour(i, offset))
                continue;
            const QPointF original = originalPins[i] + offset;
            const QPointF target = transformedPins[i] + pinOffset(i, offset);
            constraintCorners << locate(m_mesh, original);
            constraintTargets << target;
            controlOriginal << original;
            controlTransformed << target;
        }
    }

    // Triangles with cotangent edge weights, scaled by their stiffness.
    struct Edge {
        int a;
        int b;
        double weight;
    };
    QVector<std::array<Edge, 3>> triangleEdges;
    triangleEdges.reserve(2 * m_mesh.columns * m_mesh.rows);
    QVector<Eigen::Triplet<double>> triplets;
    triplets.reserve(2 * m_mesh.columns * m_mesh.rows * 12 + constraintCorners.size() * 9);
    for (int row = 0; row < m_mesh.rows; ++row) {
        for (int column = 0; column < m_mesh.columns; ++column) {
            for (int half = 0; half < 2; ++half) {
                const std::array<int, 3> v = triangleVertices(m_mesh, column, row, half);
                const double stiffness = m_mesh.triangleSolid(column, row, half) ? 1.0 : EmptyStiffness;
                std::array<Edge, 3> edges;
                for (int k = 0; k < 3; ++k) {
                    const int a = v[(k + 1) % 3];
                    const int b = v[(k + 2) % 3];
                    const double weight = stiffness * 0.5 * qMax(0.0, cotangent(rest[v[k]], rest[a], rest[b]));
                    edges[k] = {a, b, weight};
                    triplets.append({a, a, weight});
                    triplets.append({b, b, weight});
                    triplets.append({a, b, -weight});
                    triplets.append({b, a, -weight});
                }
                triangleEdges.append(edges);
            }
        }
    }
    Eigen::VectorXd constantX = Eigen::VectorXd::Zero(n);
    Eigen::VectorXd constantY = Eigen::VectorXd::Zero(n);
    for (int c = 0; c < constraintCorners.size(); ++c) {
        const Triangle &corners = constraintCorners[c];
        for (const Corner &i : corners) {
            for (const Corner &j : corners)
                triplets.append({i.index, j.index, PinWeight * i.weight * j.weight});
            constantX[i.index] += PinWeight * i.weight * constraintTargets[c].x();
            constantY[i.index] += PinWeight * i.weight * constraintTargets[c].y();
        }
    }
    Eigen::SparseMatrix<double> system(n, n);
    system.setFromTriplets(triplets.begin(), triplets.end());
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver(system);
    if (solver.info() != Eigen::Success) {
        // The rest mesh stays (the artwork renders unchanged).
        qWarning() << "KisPuppetTransformWorker: the ARAP system could not be factorized" << solver.info();
        return;
    }

    // Start every vertex with the rigid motion of the pin nearest to it along
    // the artwork, with the same hinge rule as the constraints, so a rotated
    // pin already turns the free part beyond it and the iterations only relax
    // the joints. Starting from a smooth field instead needs hundreds of
    // iterations to carry a large rotation through a narrow joint.
    QVector<QPointF> current(n);
    for (int v = 0; v < n; ++v) {
        const int pin = owner[v];
        if (pin < 0) {
            current[v] = rest[v];
            continue;
        }
        current[v] = transformedPins[pin] + pinOffset(pin, rest[v] - originalPins[pin]);
    }

    for (int iteration = 0; iteration < Iterations; ++iteration) {
        Eigen::VectorXd rhsX = constantX;
        Eigen::VectorXd rhsY = constantY;
        for (const std::array<Edge, 3> &edges : triangleEdges) {
            // Local step: the rotation that best maps the rest edges.
            double s00 = 0;
            double s01 = 0;
            double s10 = 0;
            double s11 = 0;
            for (const Edge &e : edges) {
                const QPointF u = current[e.a] - current[e.b];
                const QPointF x = rest[e.a] - rest[e.b];
                s00 += e.weight * u.x() * x.x();
                s01 += e.weight * u.x() * x.y();
                s10 += e.weight * u.y() * x.x();
                s11 += e.weight * u.y() * x.y();
            }
            const double angle = std::atan2(s10 - s01, s00 + s11);
            const double c = std::cos(angle);
            const double s = std::sin(angle);
            for (const Edge &e : edges) {
                const QPointF x = rest[e.a] - rest[e.b];
                const double rx = e.weight * (c * x.x() - s * x.y());
                const double ry = e.weight * (s * x.x() + c * x.y());
                rhsX[e.a] += rx;
                rhsY[e.a] += ry;
                rhsX[e.b] -= rx;
                rhsY[e.b] -= ry;
            }
        }
        // Global step.
        const Eigen::VectorXd x = solver.solve(rhsX);
        const Eigen::VectorXd y = solver.solve(rhsY);
        for (int i = 0; i < n; ++i)
            current[i] = QPointF(x[i], y[i]);
    }
    m_deformed = current;
}

int KisPuppetTransformWorker::ownerAt(const QPointF &point) const
{
    if (m_owner.isEmpty())
        return -1;
    const Triangle corners = locate(m_mesh, point);
    const Corner *nearest = &corners[0];
    for (const Corner &corner : corners) {
        if (corner.weight > nearest->weight)
            nearest = &corner;
    }
    return m_owner[nearest->index];
}

int KisPuppetTransformWorker::orderAt(const QPointF &point) const
{
    const int pin = ownerAt(point);
    return pin >= 0 && pin < m_orders.size() ? m_orders[pin] : 0;
}

QVector<int> KisPuppetTransformWorker::stackingGroups() const
{
    QVector<int> groups;
    for (int pin : m_owner) {
        if (!groups.contains(pin))
            groups << pin;
    }
    if (groups.size() <= 1)
        return QVector<int>{groups.isEmpty() ? -1 : groups.first()};
    auto order = [this](int pin) {
        return pin >= 0 && pin < m_orders.size() ? m_orders[pin] : 0;
    };
    std::sort(groups.begin(), groups.end(), [&](int a, int b) {
        // Unowned parts at the bottom, then by order, then by pin index.
        if ((a < 0) != (b < 0))
            return a < 0;
        if (order(a) != order(b))
            return order(a) < order(b);
        return a < b;
    });
    return groups;
}

QPointF KisPuppetTransformWorker::map(const QPointF &point) const
{
    if (m_identity || !m_mesh.isValid())
        return point;
    const Triangle corners = locate(m_mesh, point);
    QPointF result;
    for (const Corner &corner : corners)
        result += corner.weight * m_deformed[corner.index];
    return result;
}

void KisPuppetTransformWorker::run(KisPaintDeviceSP srcDevice, KisPaintDeviceSP dstDevice) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(*srcDevice->colorSpace() == *dstDevice->colorSpace());
    if (!m_mesh.isValid())
        return;
    if (m_identity) {
        dstDevice->makeCloneFromRough(srcDevice, srcDevice->extent());
        return;
    }
    const QRect srcBounds = srcDevice->region().boundingRect();
    const SourceCoverage coverage = SourceCoverage::fromDevice(srcDevice, srcBounds);
    dstDevice->clear();
    auto mapOp = [this](const QPointF &point) {
        return map(point);
    };
    // Solstice GPU engine (phase 4.96): record the polygons the CPU would
    // paint (the same cell filter) and paint them on the GPU, bit-identical;
    // on failure the target is cleared and painted on the CPU.
    const QVector<int> levels = stackingGroups();
    if (levels.size() == 1) {
        if (isGpuEnabled() && KisGpuGridWarpWorker::canRun(srcDevice, dstDevice)) {
            KisGpuGridWarpWorker::Recorder recorder(dstDevice->colorSpace() == srcDevice->colorSpace());
            recorder.setCanMergeRects(false);
            OrderFilterOp<KisGpuGridWarpWorker::Recorder> filterOp{recorder, *this, coverage, 0, false, {}};
            GridIterationTools::processGrid(filterOp, mapOp, srcBounds, 8);
            recorder.finalize();
            if (KisGpuGridWarpWorker::run(srcDevice, dstDevice, recorder)) {
                return;
            }
            dstDevice->clear();
        }
        GridIterationTools::PaintDevicePolygonOp polygonOp(srcDevice, dstDevice);
        polygonOp.setCanMergeRects(false);
        OrderFilterOp<GridIterationTools::PaintDevicePolygonOp> filterOp{polygonOp, *this, coverage, 0, false, {}};
        GridIterationTools::processGrid(filterOp, mapOp, srcBounds, 8);
        polygonOp.finalize();
        return;
    }
    // Render each pin's part separately and composite them bottom to top
    // (stackingGroups()), so parts never erase each other. The layers share
    // the destination's offset, so the GPU can paint them as well.
    auto makeLayer = [&]() {
        KisPaintDeviceSP layer = new KisPaintDevice(dstDevice->colorSpace());
        layer->setDefaultBounds(dstDevice->defaultBounds());
        layer->moveTo(dstDevice->x(), dstDevice->y());
        return layer;
    };
    // On the GPU path, one grid pass records every group (the CPU path walks
    // the grid once per group).
    std::vector<std::unique_ptr<KisGpuGridWarpWorker::Recorder>> recorders;
    if (isGpuEnabled() && KisGpuGridWarpWorker::canRun(srcDevice, makeLayer())) {
        for (int i = 0; i < levels.size(); ++i) {
            recorders.emplace_back(
                new KisGpuGridWarpWorker::Recorder(dstDevice->colorSpace() == srcDevice->colorSpace()));
            recorders.back()->setCanMergeRects(false);
        }
        GroupRecordersOp groupOp{*this, coverage, levels, recorders};
        GridIterationTools::processGrid(groupOp, mapOp, srcBounds, 8);
        for (auto &recorder : recorders)
            recorder->finalize();
    }
    for (int i = 0; i < levels.size(); ++i) {
        KisPaintDeviceSP layer = makeLayer();
        if (recorders.empty() || !KisGpuGridWarpWorker::run(srcDevice, layer, *recorders[size_t(i)])) {
            layer->clear();
            GridIterationTools::PaintDevicePolygonOp polygonOp(srcDevice, layer);
            polygonOp.setCanMergeRects(false);
            OrderFilterOp<GridIterationTools::PaintDevicePolygonOp> filterOp{polygonOp,
                                                                             *this,
                                                                             coverage,
                                                                             levels[i],
                                                                             true,
                                                                             {}};
            GridIterationTools::processGrid(filterOp, mapOp, srcBounds, 8);
            polygonOp.finalize();
        }
        const QRect rect = layer->extent();
        if (rect.isEmpty())
            continue;
        KisPainter gc(dstDevice);
        gc.setCompositeOpId(COMPOSITE_OVER);
        gc.bitBlt(rect.topLeft(), layer, rect);
    }
}

bool KisPuppetTransformWorker::isGpuEnabled()
{
    return qgetenv("KRITA_GPU_PUPPET") != "0";
}

QImage KisPuppetTransformWorker::runOnQImage(const QImage &srcImage,
                                             const QPointF &srcImageOffset,
                                             const std::function<QPointF(const QPointF &)> &imageToThumb,
                                             const std::function<QPointF(const QPointF &)> &thumbToImage,
                                             QPointF *newOffset) const
{
    KIS_ASSERT_RECOVER(srcImage.format() == QImage::Format_ARGB32)
    {
        return QImage();
    }
    if (!m_mesh.isValid() || m_identity) {
        *newOffset = srcImageOffset;
        return srcImage;
    }
    auto mapOp = [&](const QPointF &point) {
        return imageToThumb(map(thumbToImage(point)));
    };

    const QRect srcBounds = QRectF(srcImageOffset, srcImage.size()).toAlignedRect();
    QRectF dstBounds;
    const int step = 8;
    for (int y = srcBounds.top(); y <= srcBounds.bottom() + step; y += step)
        for (int x = srcBounds.left(); x <= srcBounds.right() + step; x += step)
            KisAlgebra2D::accumulateBounds(
                mapOp(QPointF(qMin(x, srcBounds.right() + 1), qMin(y, srcBounds.bottom() + 1))),
                &dstBounds);
    *newOffset = dstBounds.topLeft();
    QImage dstImage(dstBounds.toAlignedRect().size(), srcImage.format());
    dstImage.fill(0);

    const QVector<int> levels = stackingGroups();
    const SourceCoverage coverage = SourceCoverage::fromImage(srcImage, srcImageOffset);
    // The preview grid is in thumbnail space; owners are looked up in image space.
    if (levels.size() == 1) {
        GridIterationTools::QImagePolygonOp polygonOp(srcImage, dstImage, srcImageOffset, *newOffset);
        polygonOp.setCanMergeRects(false);
        OrderFilterOp<GridIterationTools::QImagePolygonOp> filterOp{polygonOp, *this, coverage, 0, false, thumbToImage};
        GridIterationTools::processGrid(filterOp, mapOp, srcBounds, 16);
        polygonOp.finalize();
        return dstImage;
    }
    QPainter gc(&dstImage);
    for (int level : levels) {
        QImage layer(dstImage.size(), dstImage.format());
        layer.fill(0);
        GridIterationTools::QImagePolygonOp polygonOp(srcImage, layer, srcImageOffset, *newOffset);
        polygonOp.setCanMergeRects(false);
        OrderFilterOp<GridIterationTools::QImagePolygonOp> filterOp{polygonOp,
                                                                    *this,
                                                                    coverage,
                                                                    level,
                                                                    true,
                                                                    thumbToImage};
        GridIterationTools::processGrid(filterOp, mapOp, srcBounds, 16);
        polygonOp.finalize();
        gc.drawImage(QPoint(), layer);
    }
    gc.end();
    return dstImage;
}

QRect KisPuppetTransformWorker::approxChangeRect(const QRect &rect) const
{
    if (m_identity || !m_mesh.isValid() || rect.isEmpty())
        return rect;
    const qreal step = qMax<qreal>(2.0, m_mesh.cellSize() / 2.0);
    QRectF bounds;
    for (qreal y = rect.top(); y < rect.bottom() + step; y += step)
        for (qreal x = rect.left(); x < rect.right() + step; x += step)
            KisAlgebra2D::accumulateBounds(
                map(QPointF(qMin<qreal>(x, rect.right() + 1), qMin<qreal>(y, rect.bottom() + 1))),
                &bounds);
    return bounds.toAlignedRect().adjusted(-2, -2, 2, 2);
}
