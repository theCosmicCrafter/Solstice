/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_THROUGH_LAYER_ERASER_TARGETS_H
#define KIS_THROUGH_LAYER_ERASER_TARGETS_H

#include "kis_types.h"
#include "kritaui_export.h"

namespace KisThroughLayerEraserTargets
{
/**
 * The paint layers a through-layer erase applies to, from the top of the stack
 * down (docs/agent/through-layer-eraser.md). Hidden or locked layers and
 * groups are left out with everything inside them.
 *
 * @param root the group to look in: the image's root, or the root of its
 *        isolated mode
 * @param selectedNodes with @p selectedOnly, the layers to erase; a selected
 *        group stands for the layers inside it
 * @param includeAlphaLocked whether alpha locked layers are erased too; the
 *        stroke then erases them in spite of the lock
 */
KRITAUI_EXPORT KisNodeList collect(KisNodeSP root,
                                   const KisNodeList &selectedNodes,
                                   bool selectedOnly,
                                   bool includeAlphaLocked);
} // namespace KisThroughLayerEraserTargets

#endif // KIS_THROUGH_LAYER_ERASER_TARGETS_H
