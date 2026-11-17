/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKADJUSTDOCK_H
#define QUICKADJUSTDOCK_H

#include <KoCanvasObserverBase.h>
#include <KoColor.h>
#include <KoDockFactoryBase.h>
#include <kis_types.h>

#include <QDockWidget>
#include <QHash>
#include <QList>
#include <QPointer>

class KisAngleSelector;
class KisCanvas2;
class KisCanvasResourceProvider;
class KisDoubleSliderSpinBox;
class KisSliderSpinBox;
class QComboBox;
class QuickAdjustToolOptionsPad;
class QTimer;
class QToolButton;

class QuickAdjustDock : public QDockWidget, public KoCanvasObserverBase
{
    Q_OBJECT
public:
    explicit QuickAdjustDock(QWidget *parent = nullptr, bool compactPopup = false);
    ~QuickAdjustDock() override;

    QString observerName() override;
    void setCanvas(KoCanvasBase *canvas) override;
    void unsetCanvas() override;

private Q_SLOTS:
    void syncFromCanvas();
    void updateStatusButtons();
    void slotBrushSizeChanged(qreal size);
    void slotBrushOpacityChanged(int value);
    void slotBrushFlowChanged(int value);
    void slotBrushRotationChanged(qreal angle);
    void slotBrushBlendChanged(int index);
    void slotLayerOpacityChanged(int value);
    void slotLayerBlendChanged(int index);
    void slotResetBrush();
    void slotForegroundColorChanged(const KoColor &color);
    void slotBrushPresetChanged(const KisPaintOpPresetSP preset);

private:
    KisSliderSpinBox *createPercentSlider(const QString &prefix);
    void populateBlendModes(QComboBox *combo);
    QWidget *createColorHistoryWidget();
    QWidget *createBrushHistoryWidget();
    QWidget *createBrushToggleWidget();
    void loadColorHistory();
    void updateColorHistoryButtons();
    void selectHistoryColor(int index);
    void updateBrushHistoryButtons();
    void selectHistoryBrush(int index);
    QToolButton *createStatusButton(const QString &toolTip, const QString &iconName);
    void triggerAction(const QString &name);
    void setControlsEnabled(bool enabled);

protected:
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QPointer<KisCanvas2> m_canvas;
    KisCanvasResourceProvider *m_resourceProvider{nullptr};
    QTimer *m_syncTimer{nullptr};
    KisDoubleSliderSpinBox *m_brushSize{nullptr};
    KisSliderSpinBox *m_brushOpacity{nullptr};
    KisSliderSpinBox *m_brushFlow{nullptr};
    KisAngleSelector *m_brushRotation{nullptr};
    QWidget *m_brushRotationRow{nullptr};
    KisSliderSpinBox *m_layerOpacity{nullptr};
    QComboBox *m_brushBlend{nullptr};
    QComboBox *m_layerBlend{nullptr};
    QWidget *m_colorHistoryGroup{nullptr};
    QWidget *m_brushHistoryGroup{nullptr};
    QList<QToolButton *> m_colorButtons;
    QList<KoColor> m_colorHistory;
    QList<QToolButton *> m_brushButtons;
    QList<KisPaintOpPresetSP> m_brushHistory;
    QHash<QString, QToolButton *> m_brushToggleButtons;
    QToolButton *m_toolOptionsToggle{nullptr};
    QToolButton *m_eraseToggle{nullptr};
    QToolButton *m_alphaToggle{nullptr};
    QToolButton *m_selectionToggle{nullptr};
    QToolButton *m_gestureToggle{nullptr};
    QuickAdjustToolOptionsPad *m_toolOptions{nullptr};
    bool m_compactPopup{false};
    bool m_syncing{false};
};

class QuickAdjustDockFactory : public KoDockFactoryBase
{
public:
    QString id() const override;
    QDockWidget *createDockWidget() override;
    DockPosition defaultDockPosition() const override;
};

#endif
