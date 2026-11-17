/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISBRUSHTIPOPTIONDATA_H
#define KISBRUSHTIPOPTIONDATA_H

#include <boost/operators.hpp>

#include <KisBrushModel.h>
#include <KisBrushOptionWidgetFlags.h>
#include <kis_types.h>
#include <kritapaintop_export.h>

#include "KisMaskingBrushOptionProperties.h"
#include "kis_precision_option.h"

class KisResourcesInterface;
using KisResourcesInterfaceSP = QSharedPointer<KisResourcesInterface>;

/**
 * The state of the brush tip option (KisBrushOptionWidget), as one value
 * (docs/agent/brush-option-shared-model.md).
 *
 * commonBrushSize is the size shared by the auto, predefined and text tips:
 * switching the tip type keeps it. It is not stored by itself; writing bakes
 * it into the tip's diameter or scale.
 */
struct PAINTOP_EXPORT KisBrushTipOptionData : public boost::equality_comparable<KisBrushTipOptionData> {
    KisBrushModel::BrushData brush;
    KisBrushModel::PrecisionData precision;
    qreal commonBrushSize = 778.0;

    // Out of line: the comparisons of the members are not exported.
    bool operator==(const KisBrushTipOptionData &rhs) const;

    /**
     * Reads `brush_definition` (and the precision with SupportsPrecision).
     * Returns false and keeps the data when the brush cannot be loaded.
     */
    bool read(const KisPropertiesConfiguration *settings,
              KisResourcesInterfaceSP resourcesInterface,
              KisBrushOptionWidgetFlags flags);
    /// Writes the baked brush (and the precision with SupportsPrecision).
    void write(KisPropertiesConfiguration *settings, KisBrushOptionWidgetFlags flags) const;

    /// The brush as written: the common size baked into the tips.
    KisBrushModel::BrushData bakedBrushData(KisBrushOptionWidgetFlags flags) const;
    /// True for a predefined tip whose effective application is the lightness map.
    bool lightnessModeEnabled(KisBrushOptionWidgetFlags flags) const;
};

/**
 * The state of the masking brush option (KisMaskingBrushOption), as one value.
 *
 * Preserve mode: right after reading, the stored size coefficient is written
 * back unchanged, as long as the masking brush, the master (brush tip) size
 * and the masking brush's common size are those that were read. A change to
 * any of them ends the mode until the next read.
 */
struct PAINTOP_EXPORT KisMaskingBrushOptionData : public boost::equality_comparable<KisMaskingBrushOptionData> {
    KisBrushModel::MaskingBrushData masking;
    qreal commonBrushSize = 777.0;
    bool preserveMode = false;
    KisBrushModel::BrushData originalBrush;
    qreal originalMasterSize = 0.0;
    qreal originalCommonSize = 0.0;

    bool operator==(const KisMaskingBrushOptionData &rhs) const;

    /// Reads the masking brush for the brush tip size @p masterBrushSize and
    /// starts the preserve mode.
    void
    read(const KisPropertiesConfiguration *settings, qreal masterBrushSize, KisResourcesInterfaceSP resourcesInterface);
    /// Writes the baked masking brush for the brush tip size @p masterBrushSize.
    void write(KisPropertiesConfiguration *settings, qreal masterBrushSize) const;

    /// Records the current masking brush and sizes and starts the preserve
    /// mode. The editor calls it again once its controls have settled, since
    /// they may round the sizes that were read.
    void startPreserveMode(qreal masterBrushSize);
    /// True while the preserve mode holds for @p masterBrushSize.
    bool preserveModeHolds(qreal masterBrushSize) const;
    /// The masking brush as written: the common size baked into the tips and,
    /// with the master size outside the preserve mode, the size coefficient
    /// recomputed.
    KisBrushModel::MaskingBrushData bakedMaskingData(qreal masterBrushSize) const;
};

#endif // KISBRUSHTIPOPTIONDATA_H
