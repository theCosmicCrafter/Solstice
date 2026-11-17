/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_DUST_DETECTOR_H
#define KIS_DUST_DETECTOR_H

#include <QPoint>
#include <QRect>
#include <QVector>

#include <KoColor.h>

#include "kis_types.h"
#include "kritaimage_export.h"

class KoUpdater;

/**
 * Finds small isolated specks ("dust") and small holes in a paint device.
 *
 * A dust component is an 8-connected group of candidate pixels whose bounding
 * box is at most dustSize pixels wide and high. Which pixels are candidates
 * depends on the mode; every pixel next to a component is, by construction,
 * a non-candidate (the surrounding).
 *
 * The result for a pixel depends only on the pixels within dustSize + 1 of
 * it, so splitting an area into tiles and analysing each tile through
 * neededRect() gives the same result as analysing the whole area at once.
 */
class KRITAIMAGE_EXPORT KisDustDetector
{
public:
    enum Mode {
        /// Opaque or semi-opaque specks surrounded by transparency
        DustOnTransparency = 0,
        /// Non-white specks surrounded by white or transparency
        DustOnWhite,
        /// Transparent or semi-transparent holes inside painted areas
        TransparentGaps
    };

    enum Containment {
        /// Every pixel of a component must be inside the enclosure
        FullyInside = 0,
        /// At least one pixel of a component must be inside the enclosure
        Touching
    };

    enum Replacement {
        /// Replace dust pixels with transparency
        Erase = 0,
        /// Replace dust pixels with opaque white
        PaintWhite,
        /// Replace each component with its main surrounding color
        SurroundingColor,
        /// Replace dust pixels with a given color
        FixedColor
    };

    struct Options {
        Mode mode{DustOnTransparency};
        /// Maximum width and height of a dust component, in pixels
        int dustSize{4};
        /**
         * In the range 0..1. DustOnTransparency: opacity at or below this is
         * transparent. DustOnWhite: color difference from white at or below
         * this counts as white. TransparentGaps: opacity below 1 - tolerance
         * is a gap.
         */
        qreal tolerance{0.0};
    };

    struct Component {
        QRect bounds;
        QVector<QPoint> pixels;
        /// The main color around the component, in the reference color space.
        /// Only computed for TransparentGaps.
        KoColor surroundingColor;
    };

    static constexpr int minimumDustSize = 1;
    static constexpr int maximumDustSize = 1000;

    /// The tolerance the filter and the tool start with, in percent. It
    /// ignores nearly invisible pixels such as eraser leftovers, which would
    /// otherwise join specks to nearby lines.
    static constexpr qreal defaultTolerancePercent = 2.0;

    /// The area that must be read to analyse \p rect
    static QRect neededRect(const QRect &rect, int dustSize);

    /// The replacement that matches the mode when no color is chosen
    static Replacement defaultReplacement(Mode mode);

    /**
     * Find the dust components of \p reference that have at least one pixel
     * in \p rect.
     *
     * Returns an empty list when \p progress is interrupted.
     */
    static QVector<Component>
    detect(KisPaintDeviceSP reference, const QRect &rect, const Options &options, KoUpdater *progress = nullptr);

    /**
     * Like detect(), but only keeps the components that satisfy
     * \p containment against \p enclosure (pixels selected at 50% or more).
     */
    static QVector<Component> detect(KisPaintDeviceSP reference,
                                     const QRect &rect,
                                     const Options &options,
                                     KisPixelSelectionSP enclosure,
                                     Containment containment,
                                     KoUpdater *progress = nullptr);

    /**
     * Overwrite the pixels of \p components in \p target. Pixels outside
     * \p clipRect are left untouched unless \p clipRect is empty.
     * \p fixedColor is only used by FixedColor.
     */
    static void apply(KisPaintDeviceSP target,
                      const QVector<Component> &components,
                      Replacement replacement,
                      const KoColor &fixedColor = KoColor(),
                      const QRect &clipRect = QRect());

    /// Select the pixels of \p components in \p selection
    static void select(KisPixelSelectionSP selection, const QVector<Component> &components);
};

#endif // KIS_DUST_DETECTOR_H
