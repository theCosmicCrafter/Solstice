/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISSOLSTICESTYLE_H
#define KISSOLSTICESTYLE_H

#include <QProxyStyle>

#include "kritawidgetutils_export.h"

/**
 * The "Solstice" widget style (docs/agent/ui-modernization.md):
 * Fusion with flat, rounded common controls in the palette's colors.
 *
 * It changes only drawing, never sizes, so layouts stay as with Fusion:
 * push and tool buttons, line edit frames, spin boxes, check boxes and
 * radio buttons, menu item highlights, scroll bars and tab shapes.
 * Everything else is Fusion's.
 *
 * The style reports the object name "fusion", because spin boxes adjust
 * their geometry for Fusion. Widget-local proxy styles clone the application
 * style through createStyle(), which keeps them in the Solstice style.
 */
class KRITAWIDGETUTILS_EXPORT KisSolsticeStyle : public QProxyStyle
{
    Q_OBJECT
public:
    KisSolsticeStyle();

    /// The name used in the Styles menu and the widgetStyle setting.
    static QString styleKey();
    /// Whether @p key (case-insensitive) selects this style.
    static bool isStyleKey(const QString &key);
    /**
     * A new style for a widget-local proxy style to wrap, in place of
     * QStyleFactory::create(@p name): a Solstice style while the
     * application uses it and @p name is its reported name, otherwise the
     * factory's style. The caller owns the result.
     */
    static QStyle *createStyle(const QString &name);

    void drawPrimitive(PrimitiveElement element,
                       const QStyleOption *option,
                       QPainter *painter,
                       const QWidget *widget = nullptr) const override;
    void drawControl(ControlElement element,
                     const QStyleOption *option,
                     QPainter *painter,
                     const QWidget *widget = nullptr) const override;
    void drawComplexControl(ComplexControl control,
                            const QStyleOptionComplex *option,
                            QPainter *painter,
                            const QWidget *widget = nullptr) const override;
};

#endif // KISSOLSTICESTYLE_H
