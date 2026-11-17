/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_CRYSTAL_LATTICE_H
#define KIS_CRYSTAL_LATTICE_H

#include <KisRandomGenerator2D.h>
#include <QPointF>
#include <QRect>
#include <kis_types.h>
#include <vector>

// Plugin-local geometry, independent of pixel format. Sites stay inside their
// grid cells; an own-cell site bounds the search distance by the cell diagonal.
class KisCrystalLattice
{
public:
    struct Site {
        qint64 column;
        qint64 row;
        QPointF position;
    };
    explicit KisCrystalLattice(const KisFilterConfigurationSP config);
    Site site(qint64 column, qint64 row);
    Site nearestSite(int x, int y);
    QPoint sampleAt(int x, int y);
    QRect dependencyRect(const QRect &rect) const;
    bool identity() const;
    bool periodic() const
    {
        return m_periodic;
    }
    QRect tileRect() const
    {
        return m_tile;
    }
    double stepX() const
    {
        return m_stepX;
    }
    double stepY() const
    {
        return m_stepY;
    }
    int columns() const
    {
        return m_columns;
    }
    int rows() const
    {
        return m_rows;
    }

private:
    void prepareCandidates(qint64 column, qint64 row);
    double m_size;
    double m_jitter;
    bool m_periodic;
    QRect m_tile;
    double m_stepX, m_stepY;
    int m_columns = 0, m_rows = 0;
    int m_searchX, m_searchY;
    KisRandomGenerator2D m_randomX, m_randomY;
    bool m_cached = false;
    qint64 m_cachedColumn = 0, m_cachedRow = 0;
    std::vector<Site> m_candidates;
};

#endif
