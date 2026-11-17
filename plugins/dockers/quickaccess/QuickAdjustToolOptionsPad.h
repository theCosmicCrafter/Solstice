/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKADJUSTTOOLOPTIONSPAD_H
#define QUICKADJUSTTOOLOPTIONSPAD_H

#include <QObject>
#include <QPointer>
#include <ksharedconfig.h>
#include <functional>

class QDockWidget;
class QMainWindow;
class QToolButton;
class QWidget;

/// Owns the borrowed Tool Options widget's temporary host. The dock supplies
/// its current canvas context; no view manager is retained across callbacks.
class QuickAdjustToolOptionsPad : public QObject
{
public:
    struct Context {
        bool canvasAvailable = false;
        QMainWindow *mainWindow = nullptr;
        bool toolOptionsInDocker = true;
    };
    using ContextProvider = std::function<Context()>;
    QuickAdjustToolOptionsPad(QDockWidget *owner,
                              QToolButton *toggle,
                              bool compactPopup,
                              ContextProvider context,
                              KSharedConfigPtr config = KSharedConfig::openConfig());
    ~QuickAdjustToolOptionsPad() override;
    void ensure();
    void setVisible(bool visible);
    void resizeToContent();
    void position();
    void hide();

private:
    void restore();
    QDockWidget *m_owner;
    QToolButton *m_toolOptionsToggle;
    bool m_compactPopup;
    ContextProvider m_context;
    KSharedConfigPtr m_config;
    QPointer<QDockWidget> m_toolOptionsDocker;
    QPointer<QWidget> m_toolOptionsPad;
    QPointer<QWidget> m_borrowedToolOptions;
    QPointer<QWidget> m_toolOptionsPlaceholder;
    bool m_toolOptionsDockerWasVisible{false};
};

#endif
