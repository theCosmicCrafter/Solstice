/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RESTNOTETIMER_H
#define RESTNOTETIMER_H

#include <QDateTime>

struct RestNoteConfig;

// One tick is one delivered timer event, as in the original docker. The clock
// is supplied only for idle detection; elapsed wall time does not catch up ticks.
class RestNoteTimer
{
public:
    enum class State {
        Running,
        Paused,
        Break,
        MicroBreak,
        Idle
    };
    enum class Transition {
        None,
        BigBreak,
        MicroBreak
    };

    State state() const
    {
        return m_state;
    }
    int remaining() const
    {
        return m_remaining;
    }
    int microRemaining() const
    {
        return m_microRemaining;
    }

    void reset(const RestNoteConfig &config, const QDateTime &now);
    void recordActivity(const QDateTime &now);
    Transition tick(const RestNoteConfig &config, const QDateTime &now);
    // True means the caller must cancel the active eye-break notification.
    bool togglePause(const QDateTime &now);
    void startBigBreak(const RestNoteConfig &config);
    void endMicroBreak(const RestNoteConfig &config);
    void settingsChanged(const RestNoteConfig &config);

private:
    State m_state{State::Running};
    int m_remaining{0};
    int m_microRemaining{0};
    QDateTime m_lastActivity;
};

#endif
