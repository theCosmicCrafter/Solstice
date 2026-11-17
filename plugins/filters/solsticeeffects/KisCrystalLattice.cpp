/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisCrystalLattice.h"
#include <cmath>
#include <filter/kis_filter_configuration.h>
#include <limits>

namespace
{
qint64 modulo(qint64 value, qint64 period)
{
    const qint64 remainder = value % period;
    return remainder < 0 ? remainder + period : remainder;
}

double bounded(const KisFilterConfigurationSP config, const char *key, double fallback, double low, double high)
{
    const double value = config->getDouble(key, fallback);
    return qBound(low, std::isfinite(value) ? value : fallback, high);
}

int coordinate(qint64 value)
{
    return int(qBound(qint64(std::numeric_limits<int>::min()), value, qint64(std::numeric_limits<int>::max())));
}
} // namespace

KisCrystalLattice::KisCrystalLattice(const KisFilterConfigurationSP config)
    : m_size(bounded(config, "cellSize", 20.0, 1.0, 512.0))
    , m_jitter(bounded(config, "irregularity", 100.0, 0.0, 100.0) / 100.0)
    , m_periodic(config->getBool("tiling", false))
    , m_tile(qBound(-1000000000, config->getInt("tileX", 0), 1000000000),
             qBound(-1000000000, config->getInt("tileY", 0), 1000000000),
             qBound(1, config->getInt("tileWidth", 1024), 1000000),
             qBound(1, config->getInt("tileHeight", 1024), 1000000))
    , m_stepX(m_size)
    , m_stepY(m_size)
    , m_randomX(quint32(qMax(0, config->getInt("seed", 0))))
    , m_randomY(quint32(qMax(0, config->getInt("seed", 0))) ^ 0x9e3779b97f4a7c15ULL)
{
    if (m_periodic) {
        m_columns = qMax(1, qRound(m_tile.width() / m_size));
        m_rows = qMax(1, qRound(m_tile.height() / m_size));
        m_stepX = double(m_tile.width()) / m_columns;
        m_stepY = double(m_tile.height()) / m_rows;
    }
    // Include any grid cell that can contain a site within the own-cell
    // diagonal. This also covers anisotropic periodic grids (e.g. 1 x 100).
    // nextafter includes distance ties on an exactly integral search bound.
    const double bound = std::nextafter(std::hypot(m_stepX, m_stepY), std::numeric_limits<double>::infinity());
    m_searchX = int(std::ceil(bound / m_stepX));
    m_searchY = int(std::ceil(bound / m_stepY));
}

bool KisCrystalLattice::identity() const
{
    return m_stepX == 1.0 && m_stepY == 1.0 && m_jitter == 0.0;
}

KisCrystalLattice::Site KisCrystalLattice::site(qint64 column, qint64 row)
{
    if (m_periodic) {
        column = modulo(column, m_columns);
        row = modulo(row, m_rows);
    }
    const double dx = m_jitter > 0.0 ? m_jitter * (m_randomX.doubleRandomAt(column, row) - 0.5) : 0.0;
    const double dy = m_jitter > 0.0 ? m_jitter * (m_randomY.doubleRandomAt(column, row) - 0.5) : 0.0;
    return {column, row, QPointF((column + 0.5 + dx) * m_stepX, (row + 0.5 + dy) * m_stepY)};
}

void KisCrystalLattice::prepareCandidates(qint64 column, qint64 row)
{
    if (m_cached && column == m_cachedColumn && row == m_cachedRow)
        return;
    m_cached = true;
    m_cachedColumn = column;
    m_cachedRow = row;
    m_candidates.clear();
    // In periodic mode enumerate each canonical cell at most once. In
    // particular, a one-column tile must not scan hundreds of duplicate sites.
    const int width = m_periodic ? qMin(m_columns, 2 * m_searchX + 1) : 2 * m_searchX + 1;
    const int height = m_periodic ? qMin(m_rows, 2 * m_searchY + 1) : 2 * m_searchY + 1;
    m_candidates.reserve(width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            m_candidates.push_back(site(column - m_searchX + x, row - m_searchY + y));
        }
    }
}

KisCrystalLattice::Site KisCrystalLattice::nearestSite(int x, int y)
{
    // Use pixel centers in grid space. Integer modulo makes all repeated
    // periods evaluate exactly the same floating-point query coordinates.
    const double px = (m_periodic ? double(modulo(qint64(x) - m_tile.x(), m_tile.width())) : double(x)) + 0.5;
    const double py = (m_periodic ? double(modulo(qint64(y) - m_tile.y(), m_tile.height())) : double(y)) + 0.5;
    const qint64 column = qint64(std::floor(px / m_stepX));
    const qint64 row = qint64(std::floor(py / m_stepY));
    prepareCandidates(column, row);
    double bestDistance = std::numeric_limits<double>::infinity();
    Site best = m_candidates.front();
    for (const Site &candidate : m_candidates) {
        double dx = std::abs(px - candidate.position.x());
        double dy = std::abs(py - candidate.position.y());
        if (m_periodic) {
            dx = qMin(dx, m_tile.width() - dx);
            dy = qMin(dy, m_tile.height() - dy);
        }
        const double distance = dx * dx + dy * dy;
        if (distance < bestDistance
            || (distance == bestDistance
                && (candidate.row < best.row || (candidate.row == best.row && candidate.column < best.column)))) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

QPoint KisCrystalLattice::sampleAt(int x, int y)
{
    const Site nearest = nearestSite(x, y);
    qint64 sx = qint64(std::floor(nearest.position.x()));
    qint64 sy = qint64(std::floor(nearest.position.y()));
    if (m_periodic) {
        // Sample within the source period containing the output pixel. For a
        // canvas-sized period this wraps to the opposite canvas edge. Repeated
        // source tiles produce repeated output without unbounded dependencies.
        sx = qint64(x) - modulo(qint64(x) - m_tile.x(), m_tile.width()) + modulo(sx, m_tile.width());
        sy = qint64(y) - modulo(qint64(y) - m_tile.y(), m_tile.height()) + modulo(sy, m_tile.height());
    }
    return QPoint(coordinate(sx), coordinate(sy));
}

QRect KisCrystalLattice::dependencyRect(const QRect &rect) const
{
    if (rect.isEmpty() || identity())
        return rect;
    if (!m_periodic) {
        const int halo = int(std::ceil(std::hypot(m_stepX, m_stepY))) + 1;
        return rect.adjusted(-halo, -halo, halo, halo);
    }
    // All dependencies lie inside each source period; reverse dirty bounds
    // are the same union. Never use an unbounded rect for adjustment layers.
    const qint64 left = qint64(rect.left()) - modulo(qint64(rect.left()) - m_tile.x(), m_tile.width());
    const qint64 top = qint64(rect.top()) - modulo(qint64(rect.top()) - m_tile.y(), m_tile.height());
    const qint64 right =
        qint64(rect.right()) - modulo(qint64(rect.right()) - m_tile.x(), m_tile.width()) + m_tile.width() - 1;
    const qint64 bottom =
        qint64(rect.bottom()) - modulo(qint64(rect.bottom()) - m_tile.y(), m_tile.height()) + m_tile.height() - 1;
    return QRect(QPoint(coordinate(left), coordinate(top)), QPoint(coordinate(right), coordinate(bottom)));
}
