/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_BRUSH_STROKE_LAYER_PRIVATE_H
#define KIS_BRUSH_STROKE_LAYER_PRIVATE_H

#include <QtGlobal>

class QString;
class QTransform;

// Internal helpers shared by the layer's implementation units in kritaui.
// Keep the public layer and recorded-stroke layout in KisBrushStrokeLayer.h.
namespace KisBrushStrokeLayerPrivate
{
/// The square root of the transform's area scale; shared by replay and blending.
qreal lengthScale(const QTransform &transform);

/// Queue a canvas message using plain values only. The image address is compared
/// with open views on the GUI thread, never dereferenced or owned by the event.
void postImageMessage(const void *image, const QString &message);
} // namespace KisBrushStrokeLayerPrivate

#endif
