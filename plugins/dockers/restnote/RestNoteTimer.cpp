/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RestNoteTimer.h"
#include "RestNoteConfig.h"

#include <algorithm>

void RestNoteTimer::reset(const RestNoteConfig &config, const QDateTime &now)
{
    m_state = State::Running;
    m_remaining = config.workSeconds();
    m_microRemaining = config.microIntervalSeconds();
    m_lastActivity = now;
}

void RestNoteTimer::recordActivity(const QDateTime &now)
{
    m_lastActivity = now;
}

RestNoteTimer::Transition RestNoteTimer::tick(const RestNoteConfig &config, const QDateTime &now)
{
    if (config.idleEnabled) {
        const qint64 idle = m_lastActivity.secsTo(now);
        if (m_state == State::Running && idle >= config.idleThresholdSeconds)
            m_state = State::Idle;
        else if (m_state == State::Idle && idle < config.idleThresholdSeconds)
            m_state = State::Running;
    } else if (m_state == State::Idle) {
        m_state = State::Running;
    }

    if (m_state == State::Running) {
        if (--m_remaining <= 0) {
            startBigBreak(config);
            return Transition::BigBreak;
        }
        if (config.microEnabled && --m_microRemaining <= 0) {
            if (m_remaining <= config.microSkipThresholdSeconds) {
                m_microRemaining = config.microIntervalSeconds();
            } else {
                m_state = State::MicroBreak;
                return Transition::MicroBreak;
            }
        }
    } else if (m_state == State::MicroBreak && --m_remaining <= 0) {
        startBigBreak(config);
        return Transition::BigBreak;
    }
    return Transition::None;
}

bool RestNoteTimer::togglePause(const QDateTime &now)
{
    if (m_state == State::Running || m_state == State::Idle) {
        m_state = State::Paused;
    } else if (m_state == State::Paused) {
        m_state = State::Running;
        m_lastActivity = now;
    } else if (m_state == State::MicroBreak) {
        m_state = State::Paused;
        return true;
    }
    return false;
}

void RestNoteTimer::startBigBreak(const RestNoteConfig &config)
{
    m_microRemaining = config.microIntervalSeconds();
    m_remaining = 0;
    m_state = State::Break;
}

void RestNoteTimer::endMicroBreak(const RestNoteConfig &config)
{
    m_microRemaining = config.microIntervalSeconds();
    if (m_state == State::MicroBreak)
        m_state = State::Running;
}

void RestNoteTimer::settingsChanged(const RestNoteConfig &config)
{
    if (m_state == State::Running) {
        m_remaining = std::min(m_remaining, config.workSeconds());
        m_microRemaining = std::min(m_microRemaining, config.microIntervalSeconds());
    } else {
        m_remaining = config.workSeconds();
        m_microRemaining = config.microIntervalSeconds();
    }
}
