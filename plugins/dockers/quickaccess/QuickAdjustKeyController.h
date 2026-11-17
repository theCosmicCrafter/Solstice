/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKADJUSTKEYCONTROLLER_H
#define QUICKADJUSTKEYCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QString>
#include <brushengine/kis_paintop_preset.h>

class KisCanvasResourceProvider;
class KisMainWindow;
class KisViewManager;

class QuickAdjustKeyController : public QObject
{
    Q_OBJECT
public:
    explicit QuickAdjustKeyController(QObject *parent = nullptr);
    ~QuickAdjustKeyController() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Mode {
        None,
        Eraser,
        PreserveAlpha,
        FreehandSelection,
        BrushPreset
    };

    KisCanvasResourceProvider *resourceProvider() const;
    bool selectPreset(const KisPaintOpPresetSP &preset) const;
    KisMainWindow *ownerWindow() const;
    bool activateForKey(class QKeyEvent *event);
    void deactivate();
    static bool matches(class QKeyEvent *event, const QString &sequence);
    static bool textEditorHasFocus();

    Mode m_mode{Mode::None};
    QPointer<KisMainWindow> m_activeWindow;
    QPointer<KisViewManager> m_activeViewManager;
    int m_activeKey{0};
    bool m_originalToggleState{false};
    KisPaintOpPresetSP m_originalPreset;
    qreal m_originalSize{0.0};
    qreal m_sizeScale{0.0};
};

#endif
