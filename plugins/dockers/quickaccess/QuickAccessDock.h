/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSDOCK_H
#define QUICKACCESSDOCK_H

#include "QuickAccessModel.h"

#include <KoDockFactoryBase.h>
#include <kis_mainwindow_observer.h>

#include <QDockWidget>
#include <QPointer>

class KisCanvas2;
class KisViewManager;
class QAction;
class QTabWidget;
class QToolButton;
class QWidget;
class QuickAccessGestureController;
class QuickAccessStrokePreviews;
class QPushButton;
class QuickAccessItemDragController;

namespace QuickAccess
{
class ItemExecutor;
}

class QuickAccessDock : public QDockWidget, public KisMainwindowObserver
{
    Q_OBJECT
public:
    explicit QuickAccessDock(QWidget *parent = nullptr, bool enableGestures = true);
    ~QuickAccessDock() override;

    QString observerName() override;
    void setViewManager(KisViewManager *viewManager) override;
    void setCanvas(KoCanvasBase *canvas) override;
    void unsetCanvas() override;
    KisCanvas2 *canvas() const;
    void reloadGestureSettings();

private Q_SLOTS:
    void slotCurrentTabChanged(int index);

private:
    void buildHeader(QWidget *root);
    void buildPopupHeader(QWidget *root);
    void buildMenu(QToolButton *button);
    void loadProfile();
    bool saveProfile();
    void rebuildUi();
    QWidget *createTabPage(const QuickAccess::Tab &tab);
    QWidget *createItemWidget(const QuickAccess::Item &item);
    void attachItemMenu(QWidget *widget, const QuickAccess::Item &item);
    void moveItem(const QString &itemId, const QPoint &target);
    int cellSize() const;
    QString itemText(const QuickAccess::Item &item) const;
    void activateItem(const QuickAccess::Item &item);
    QuickAccess::Tab *activeTab();
    QuickAccess::Grid *activeGrid();
    void addItem(QuickAccess::Item item);
    void addCurrentBrush();
    void addLabel();
    void addSeparator(bool vertical);
    void addColor();
    void addBrushSize();
    void addBrushBlendMode();
    void addScript();
    void addTab();
    void showGridEditDialog();
    void showGestureDialog();
    void showResourcesDialog();
    void showSettingsDialog();
    void applyAppearanceSettings();
    void editItemProperties(const QString &itemId);
    /// Brush items: choose the stroke preview or the preset icon.
    void editBrushProperties(const QString &itemId);
    /// Shows stroke previews that became available on brush buttons.
    void updateStrokePreviews();
    void removeItem(const QString &itemId);
    void showPalettePopup();
    void showColorPopup();
    void toggleGestures();
    void movePaletteDockerToCursor();
    void moveAdjustDockerToCursor();
    void setPopupPinned(bool pinned);
    QString profilePath() const;
    QString legacyConfigPath() const;

    QPointer<KisCanvas2> m_canvas;
    QTabWidget *m_tabs{nullptr};
    QuickAccess::Document m_document;
    QString m_profilePath;
    QuickAccess::ItemExecutor *m_executor{nullptr};
    QuickAccessGestureController *m_gestureController{nullptr};
    QPointer<QAction> m_palettePopupAction;
    QPointer<QuickAccessDock> m_palettePopup;
    QPointer<QWidget> m_colorPopup;
    QPointer<QToolButton> m_popupPinButton;
    // Brush buttons showing stroke previews: preset name, button, preview size.
    struct StrokePreviewButton {
        QString name;
        QPointer<QPushButton> button;
        QSize size;
        bool shown{false};
    };
    QuickAccessStrokePreviews *m_strokePreviews{nullptr};
    QuickAccessItemDragController *m_itemDrag{nullptr};
    // While the Resources dialog is open, added items fill the grid's last
    // empty row from the left and wrap (as the original plugin does).
    bool m_sequentialPlacement{false};
    QString m_sequentialGridId;
    int m_sequentialRow{0};
    int m_sequentialColumn{0};
    QList<StrokePreviewButton> m_strokePreviewButtons;
    bool m_rebuilding{false};
    bool m_popupMode{false};
    bool m_popupPinned{false};
};

class QuickAccessDockFactory : public KoDockFactoryBase
{
public:
    QString id() const override;
    QDockWidget *createDockWidget() override;
    DockPosition defaultDockPosition() const override;
};

#endif
