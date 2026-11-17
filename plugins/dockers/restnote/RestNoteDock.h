/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RESTNOTEDOCK_H
#define RESTNOTEDOCK_H

#include "RestNoteConfig.h"
#include "RestNoteTimer.h"

#include <KoDockFactoryBase.h>

#include <QDockWidget>
#include <QPointer>

class QLabel;
class QPushButton;
class QTimer;
class QWidget;

class RestNoteDock : public QDockWidget
{
    Q_OBJECT
public:
    explicit RestNoteDock(QWidget *parent = nullptr);
    ~RestNoteDock() override;

    /// Solstice/RestNoteEnabled (Configure Solstice > General > Custom), off by
    /// default. The plugin registers the docker only when it is on.
    static bool isEnabled();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    using State = RestNoteTimer::State;

    void buildUi();
    void updateSizing();
    void tick();
    void refreshDisplay();
    void startBigBreak();
    void endBigBreak();
    void showBigBreak();
    void showMicroBreak();
    void endMicroBreak();
    void cancelTransientWindows();
    void showSettings();
    void applyEnabled();
    QWidget *mainWindowHost() const;
    QScreen *currentScreen() const;

    RestNoteConfig m_config;
    RestNoteTimer m_timer;
    QTimer *m_tickTimer{nullptr};
    bool m_active{false};
    QWidget *m_root{nullptr};
    QLabel *m_statusLabel{nullptr};
    QLabel *m_timeLabel{nullptr};
    QLabel *m_subLabel{nullptr};
    QPushButton *m_pauseButton{nullptr};
    QPushButton *m_resetButton{nullptr};
    QPushButton *m_restButton{nullptr};
    QPushButton *m_settingsButton{nullptr};
    QPointer<QWidget> m_overlay;
    QPointer<QWidget> m_microToast;
};

class RestNoteDockFactory : public KoDockFactoryBase
{
public:
    QString id() const override;
    QDockWidget *createDockWidget() override;
    DockPosition defaultDockPosition() const override;
};

#endif
