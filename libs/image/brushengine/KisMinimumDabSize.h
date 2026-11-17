/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_MINIMUM_DAB_SIZE_H
#define KIS_MINIMUM_DAB_SIZE_H

#include <QString>
#include <QtGlobal>

/**
 * Solstice "At least 1 pixel" brush option (docs/agent/brush-minimum-one-pixel.md).
 *
 * The Freehand Brush tool stores the choice in kritarc (configKey()). Each
 * stroke's KisResourcesSnapshot copies it into its private clone of the
 * preset (settingsKey()), and brush-based paint ops read it from there, so
 * the option is fixed for the whole stroke and follows the preset into LOD
 * previews and Brush Stroke Layer records.
 */
namespace KisMinimumDabSize
{

/// kritarc key of the tool option (default: true)
inline QString configKey()
{
    return QStringLiteral("Solstice/BrushMinimumOnePixel");
}

/// paint op settings property written into the stroke's preset clone
inline QString settingsKey()
{
    return QStringLiteral("Solstice/MinimumOnePixelDab");
}

/// The default of configKey() for new and existing profiles.
inline constexpr bool defaultEnabled()
{
    return true;
}

/**
 * Returns @p scale raised so that a tip whose size at scale 1.0 is
 * @p tipSize pixels is drawn at least 1 px large.
 *
 * Pass the level-of-detail 0 scale (before multiplying it by
 * KisLodTransform::lodToScale()), so that an instant preview at a lower
 * level of detail draws the same dab, scaled down, as the final stroke.
 */
inline qreal apply(qreal scale, qreal tipSize)
{
    if (!(tipSize > 0.0)) {
        return scale;
    }
    return qMax(scale, 1.0 / tipSize);
}

} // namespace KisMinimumDabSize

#endif // KIS_MINIMUM_DAB_SIZE_H
