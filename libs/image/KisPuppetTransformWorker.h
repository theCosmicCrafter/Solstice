/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISPUPPETTRANSFORMWORKER_H
#define KISPUPPETTRANSFORMWORKER_H

#include <QBitArray>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QTransform>
#include <QVector>

#include <functional>

#include "kis_types.h"
#include "kritaimage_export.h"

/**
 * Puppet Warp deformation on a triangle mesh of the artwork (Solstice,
 * docs/agent/puppet-warp.md).
 *
 * The mesh is a regular grid of cells, each split into two triangles along
 * alternating diagonals. Triangles that cover artwork are "solid"; the others
 * stay in the system with a tiny stiffness, so limbs separated by empty space
 * deform independently while every point keeps a continuous mapping.
 *
 * Pins constrain small rigid neighbourhoods (a center and four points around
 * it, rotated with the pin). The free mesh follows as rigidly as possible
 * (ARAP: per-triangle rotation fitting alternating with a sparse linear solve),
 * so rotating a pin turns the unpinned part beyond it as one piece.
 *
 * The result depends only on the mesh and the pins, so the preview, the mesh
 * overlay and the final rendering agree when they share both.
 */
class KRITAIMAGE_EXPORT KisPuppetTransformWorker
{
public:
    struct KRITAIMAGE_EXPORT Mesh {
        QPointF origin; ///< image coordinates of grid vertex (0, 0)
        QPointF columnStep; ///< from vertex (c, r) to (c + 1, r)
        QPointF rowStep; ///< from vertex (c, r) to (c, r + 1)
        int columns = 0;
        int rows = 0;
        QBitArray solid; ///< two triangles per cell, row-major
        int expansion = -1; ///< mask expansion the mesh was built with

        bool isValid() const;
        bool operator==(const Mesh &other) const;
        bool operator!=(const Mesh &other) const
        {
            return !(*this == other);
        }
        /// Affine maps only (translation, scale, rotation, shear).
        void transform(const QTransform &t);
        QPointF vertex(int column, int row) const;
        qreal cellSize() const;
        bool triangleSolid(int column, int row, int half) const;

        QString toString() const;
        static Mesh fromString(const QString &text);

        /// Grid size used for @p bounds (about 24 image pixels per cell).
        static QSize gridSize(const QRectF &bounds);
        /**
         * Builds the mesh over @p bounds. @p mask covers @p bounds; a nonzero
         * pixel is artwork.
         */
        static Mesh build(const QImage &mask, const QRectF &bounds, int expansion);
    };

    /**
     * @param pinOrders stacking order per pin (higher on top) for overlapping
     * parts: each part belongs to the pin nearest to it along the artwork.
     * Orders only sort the rendering groups (stackingGroups()); with equal
     * orders (or none), later pins are on top.
     */
    KisPuppetTransformWorker(const Mesh &mesh,
                             const QVector<QPointF> &originalPins,
                             const QVector<QPointF> &transformedPins,
                             const QVector<qreal> &pinRotations,
                             const QVector<int> &pinOrders = QVector<int>());

    bool isValid() const;
    bool isIdentity() const;

    /// The deformed position of a point given in original image coordinates.
    QPointF map(const QPointF &point) const;
    /// Deformed grid vertices, row-major over (columns + 1) x (rows + 1).
    const QVector<QPointF> &deformedVertices() const;
    const Mesh &mesh() const;

    /// Rigid neighbourhood of a pin: its center and four points at this radius.
    static qreal pinRadius(const Mesh &mesh);

    /// Stacking order of the part at @p point (original coordinates).
    int orderAt(const QPointF &point) const;
    /// The pin owning the part at @p point (original coordinates), -1 if none.
    int ownerAt(const QPointF &point) const;
    /**
     * Rendering groups bottom to top: the owning pins (and -1 for unowned
     * parts), sorted by (order, pin index), so equal orders put later pins
     * on top. Each group renders into its own layer and the layers composite
     * with "over": the grid polygon ops overwrite pixels, so parts of
     * different pins rendered into one buffer would cut holes into each other
     * with their transparent edge pixels. One group renders in a single pass.
     */
    QVector<int> stackingGroups() const;

    /**
     * Final rendering. With the GPU engine, RGBA float devices are painted on
     * the GPU (phase 4.96, KisGpuGridWarpWorker), bit-identical to the CPU.
     *
     * Only rendering cells whose source (grown by the sampling footprint)
     * holds a non-transparent pixel are painted: the grid polygon ops
     * overwrite pixels, so transparent cells of squeezed empty space would
     * cut holes into folded artwork, and skipping a transparent cell loses
     * nothing. runOnQImage() applies the same rule to the thumbnail.
     */
    void run(KisPaintDeviceSP srcDevice, KisPaintDeviceSP dstDevice) const;

    /// False when KRITA_GPU_PUPPET=0 keeps run() on the CPU (development switch).
    static bool isGpuEnabled();

    /**
     * Preview in thumbnail space: @p srcImage (ARGB32) at @p srcImageOffset;
     * the mesh lives in image space, mapped by @p imageToThumb / @p thumbToImage.
     */
    QImage runOnQImage(const QImage &srcImage,
                       const QPointF &srcImageOffset,
                       const std::function<QPointF(const QPointF &)> &imageToThumb,
                       const std::function<QPointF(const QPointF &)> &thumbToImage,
                       QPointF *newOffset) const;

    QRect approxChangeRect(const QRect &rect) const;

private:
    void solve(const QVector<QPointF> &originalPins,
               const QVector<QPointF> &transformedPins,
               const QVector<qreal> &pinRotations);

    Mesh m_mesh;
    QVector<QPointF> m_deformed;
    QVector<int> m_owner; ///< nearest pin per vertex along the artwork, -1 if none
    QVector<int> m_orders;
    bool m_identity = true;
};

#endif // KISPUPPETTRANSFORMWORKER_H
