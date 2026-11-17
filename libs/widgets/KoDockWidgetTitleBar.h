/* This file is part of the KDE project
   SPDX-FileCopyrightText: 2007 Marijn Kruisselbrink <mkruisselbrink@kde.org>

   SPDX-License-Identifier: LGPL-2.0-or-later
*/
#ifndef KODOCKWIDGETTITLEBAR_H_
#define KODOCKWIDGETTITLEBAR_H_

#include "kritawidgets_export.h"
#include <QDockWidget>

/**
 * @short A custom title bar for dock widgets.
 *
 * Allow customization such as hidden text.
 * 
 * @see KoDockWidgetTitleBarButton
 */
class KRITAWIDGETS_EXPORT KoDockWidgetTitleBar : public QWidget
{
    Q_OBJECT
public:
    explicit KoDockWidgetTitleBar(QDockWidget *dockWidget);
    ~KoDockWidgetTitleBar() override;

    void updateIcons();

    /**
     * Solstice interface (docs/agent/ui-modernization.md): a title band with
     * a separator line and more padding. The static flag starts off; the main
     * window supplies "Solstice/ModernInterface" (default true) before and
     * after creating dockers, and title bars created later follow it.
     */
    static void setSolsticeLookEnabled(bool enabled);
    static bool solsticeLookEnabled();
    /// Applies the current Solstice look setting to this title bar.
    void updateSolsticeLook();

public Q_SLOTS:
    void setLocked(bool locked);

protected:
    void paintEvent(QPaintEvent* event) override; ///< reimplemented from QWidget
    void resizeEvent(QResizeEvent* event) override; ///< reimplemented from QWidget
private:
    Q_PRIVATE_SLOT(d, void toggleFloating())
    Q_PRIVATE_SLOT(d, void topLevelChanged(bool topLevel))
    Q_PRIVATE_SLOT(d, void featuresChanged(QDockWidget::DockWidgetFeatures))
    Q_PRIVATE_SLOT(d, void dockWidgetTitleChanged(const QString &title))

    class Private;
    Private * const d;
};

#endif // KODOCKWIDGETTITLEBAR_H_
