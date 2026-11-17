/*
 *  SPDX-FileCopyrightText: 2010 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_BRUSH_BASED_PAINTOP_OPTIONS_WIDGET_H
#define KIS_BRUSH_BASED_PAINTOP_OPTIONS_WIDGET_H

#include "kis_paintop_settings_widget.h"
#include "kis_types.h"
#include "kis_brush.h"
#include <kritapaintop_export.h>
#include "KisBrushOptionWidgetFlags.h"
#include "KisBrushTipOptionData.h"
#include <lager/cursor.hpp>

class KisBrushOptionWidget;

class PAINTOP_EXPORT KisBrushBasedPaintopOptionWidget : public KisPaintOpSettingsWidget
{
public:
    KisBrushBasedPaintopOptionWidget(KisBrushOptionWidgetFlags flags, QWidget* parent = 0);
    /// Solstice: the brush tip's state lives in @p brushTipData, e.g. a
    /// shared options model (docs/agent/brush-option-shared-model.md).
    KisBrushBasedPaintopOptionWidget(KisBrushOptionWidgetFlags flags,
                                     lager::cursor<KisBrushTipOptionData> brushTipData,
                                     QWidget *parent = 0);
    ~KisBrushBasedPaintopOptionWidget() override;

    KisBrushSP brush();

    lager::reader<qreal> effectiveBrushSize() const override;

protected:
    KisBrushOptionWidget *brushOptionWidget() const;

private:
    KisBrushOptionWidget *m_brushOption;
};

#endif // KIS_BRUSH_BASED_PAINTOP_OPTIONS_WIDGET_H
