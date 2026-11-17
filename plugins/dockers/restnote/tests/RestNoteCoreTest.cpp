/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RestNoteConfig.h"
#include "RestNoteTimer.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace
{
using State = RestNoteTimer::State;
using Transition = RestNoteTimer::Transition;
const QDateTime start = QDateTime::fromString(QStringLiteral("2023-11-14T22:13:20Z"), Qt::ISODate);

bool write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

const QByteArray legacy = R"({
    "work_minutes":37,"break_minutes":7,"micro_break_enabled":false,
    "micro_break_interval_minutes":13,"micro_break_duration_seconds":25,
    "micro_skip_threshold_seconds":91,"idle_enabled":false,"idle_threshold_seconds":75,
    "micro_toast_margin":83,"micro_toast_width":430,"micro_toast_height":190,
    "micro_toast_title_font_size":24,"micro_toast_message_font_size":19,
    "overlay_title_font_size":47,"overlay_message_font_size":31,"overlay_skip_font_size":17
})";

RestNoteConfig cycleConfig()
{
    RestNoteConfig config;
    config.workMinutes = 2;
    config.microIntervalMinutes = 1;
    config.microSkipThresholdSeconds = 0;
    config.idleEnabled = false;
    return config;
}

Transition advance(RestNoteTimer &timer, const RestNoteConfig &config, int seconds)
{
    Transition result = Transition::None;
    for (int i = 0; i < seconds; ++i)
        result = timer.tick(config, start);
    return result;
}
} // namespace

class RestNoteCoreTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void defaultsAndLegacyRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("main.json");
        RestNoteConfig config;
        config.load(path);
        const auto defaults = QJsonDocument::fromJson(read(path)).object();
        QCOMPARE(defaults.size(), 16);
        QCOMPARE(config.workSeconds(), 3000);
        QCOMPARE(config.breakSeconds(), 600);
        QCOMPARE(config.microIntervalSeconds(), 1200);
        QCOMPARE(defaults.value("micro_break_duration_seconds").toInt(), 20);
        QCOMPARE(defaults.value("micro_skip_threshold_seconds").toInt(), 180);
        QCOMPARE(defaults.value("idle_threshold_seconds").toInt(), 45);
        QCOMPARE(defaults.value("micro_toast_margin").toInt(), 128);
        QCOMPARE(defaults.value("micro_toast_width").toInt(), 480);
        QCOMPARE(defaults.value("micro_toast_height").toInt(), 220);
        QCOMPARE(defaults.value("micro_toast_title_font_size").toInt(), 28);
        QCOMPARE(defaults.value("micro_toast_message_font_size").toInt(), 24);
        QCOMPARE(defaults.value("overlay_title_font_size").toInt(), 50);
        QCOMPARE(defaults.value("overlay_message_font_size").toInt(), 32);
        QCOMPARE(defaults.value("overlay_skip_font_size").toInt(), 18);
        QVERIFY(config.microEnabled);
        QVERIFY(config.idleEnabled);

        QVERIFY(write(path, legacy));
        config.load(path);
        QCOMPARE(read(path), legacy); // Loading an existing file does not rewrite it.
        QCOMPARE(config.workSeconds(), 2220);
        QCOMPARE(config.breakSeconds(), 420);
        QCOMPARE(config.microIntervalSeconds(), 780);
        QVERIFY(!config.microEnabled);
        QVERIFY(!config.idleEnabled);
        QVERIFY(config.save(path));
        QCOMPARE(QJsonDocument::fromJson(read(path)), QJsonDocument::fromJson(legacy));
    }

    void malformedAndPartialConfig()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("main.json");
        RestNoteConfig config;
        config.workMinutes = 43;
        config.toastWidth = 401;
        for (const QByteArray &data : {QByteArray("broken json"), QByteArray("[]"), QByteArray("{}")}) {
            QVERIFY(write(path, data));
            config.load(path);
            QCOMPARE(config.workMinutes, 43);
            QCOMPARE(config.toastWidth, 401);
            QCOMPARE(read(path), data);
        }
        QVERIFY(
            write(path, R"({"work_minutes":"30","break_minutes":-2,"micro_break_enabled":0,"idle_enabled":false})"));
        config.load(path);
        QCOMPARE(config.workMinutes, 43); // No coercion of legacy strings.
        QCOMPARE(config.breakSeconds(), -120); // Loading does not impose dialog ranges.
        QVERIFY(config.microEnabled);
        QVERIFY(!config.idleEnabled);
        QVERIFY(!config.save(dir.path())); // A directory is not a writable JSON file.
    }

    void workAndBigBreak()
    {
        auto config = cycleConfig();
        config.workMinutes = 1;
        RestNoteTimer timer;
        timer.reset(config, start);
        QCOMPARE(advance(timer, config, 59), Transition::None);
        QCOMPARE(timer.remaining(), 1);
        // Both timers expire together: the big break wins.
        QCOMPARE(timer.tick(config, start), Transition::BigBreak);
        QCOMPARE(timer.state(), State::Break);
        QCOMPARE(timer.remaining(), 0);
        QCOMPARE(timer.microRemaining(), 60);
        QVERIFY(!timer.togglePause(start));
        QCOMPARE(advance(timer, config, 60), Transition::None);
        QCOMPARE(timer.state(), State::Break);
        QCOMPARE(timer.remaining(), 0); // Only the overlay's completion resets work.
        timer.reset(config, start.addSecs(120));
        QCOMPARE(timer.state(), State::Running);
        QCOMPARE(timer.remaining(), 60);
        QCOMPARE(timer.microRemaining(), 60);
    }

    void eyeBreakAndPriority()
    {
        const auto config = cycleConfig();
        RestNoteTimer timer;
        timer.reset(config, start);
        QCOMPARE(advance(timer, config, 60), Transition::MicroBreak);
        QCOMPARE(timer.state(), State::MicroBreak);
        QCOMPARE(timer.remaining(), 60);
        QCOMPARE(timer.microRemaining(), 0);
        QCOMPARE(advance(timer, config, 10), Transition::None);
        QCOMPARE(timer.remaining(), 50);
        QCOMPARE(timer.microRemaining(), 0);
        timer.endMicroBreak(config);
        QCOMPARE(timer.state(), State::Running);
        QCOMPARE(timer.remaining(), 50);
        QCOMPARE(timer.microRemaining(), 60);

        timer.reset(config, start);
        advance(timer, config, 60);
        QCOMPARE(advance(timer, config, 60), Transition::BigBreak);
        QCOMPARE(timer.state(), State::Break);
        QCOMPARE(timer.remaining(), 0);
        timer.endMicroBreak(config); // A late completion must not resume a big break.
        QCOMPARE(timer.state(), State::Break);
    }

    void skipBoundary_data()
    {
        QTest::addColumn<int>("threshold");
        QTest::addColumn<bool>("skipped");
        QTest::newRow("below") << 59 << false;
        QTest::newRow("equal") << 60 << true;
        QTest::newRow("above") << 61 << true;
    }

    void skipBoundary()
    {
        QFETCH(int, threshold);
        QFETCH(bool, skipped);
        auto config = cycleConfig();
        config.microSkipThresholdSeconds = threshold;
        RestNoteTimer timer;
        timer.reset(config, start);
        QCOMPARE(advance(timer, config, 60), skipped ? Transition::None : Transition::MicroBreak);
        QCOMPARE(timer.state(), skipped ? State::Running : State::MicroBreak);
        QCOMPARE(timer.microRemaining(), skipped ? 60 : 0);
    }

    void idlePauseAndClock()
    {
        auto config = cycleConfig();
        config.idleEnabled = true;
        RestNoteTimer timer;
        timer.reset(config, start);
        timer.tick(config, start.addSecs(44));
        QCOMPARE(timer.remaining(), 119); // No wall-clock catch-up.
        timer.tick(config, start.addSecs(45));
        QCOMPARE(timer.state(), State::Idle);
        QCOMPARE(timer.remaining(), 119);
        timer.recordActivity(start.addSecs(100));
        timer.tick(config, start.addSecs(100));
        QCOMPARE(timer.state(), State::Running);
        QCOMPARE(timer.remaining(), 118);
        QVERIFY(!timer.togglePause(start.addSecs(101)));
        timer.recordActivity(start.addSecs(200));
        timer.tick(config, start.addSecs(300));
        QCOMPARE(timer.state(), State::Paused);
        QCOMPARE(timer.remaining(), 118);
        QVERIFY(!timer.togglePause(start.addSecs(300)));
        timer.tick(config, start.addSecs(301));
        QCOMPARE(timer.state(), State::Running); // Resume renews activity time.
        QCOMPARE(timer.remaining(), 117);
        timer.tick(config, start.addSecs(345));
        QCOMPARE(timer.state(), State::Idle);
        QVERIFY(!timer.togglePause(start.addSecs(345)));
        QCOMPARE(timer.state(), State::Paused);
        timer.togglePause(start.addSecs(345));
        timer.tick(config, start.addSecs(400));
        config.idleEnabled = false;
        timer.tick(config, start.addSecs(401));
        QCOMPARE(timer.state(), State::Running);
    }

    void pauseEyeBreakAndDisableReminders()
    {
        auto config = cycleConfig();
        RestNoteTimer timer;
        timer.reset(config, start);
        advance(timer, config, 60);
        config.idleEnabled = true;
        timer.tick(config, start.addSecs(100));
        QCOMPARE(timer.state(), State::MicroBreak); // Idle cannot override a break.
        QCOMPARE(timer.remaining(), 59);
        QVERIFY(timer.togglePause(start.addSecs(100)));
        QCOMPARE(timer.state(), State::Paused);
        QCOMPARE(timer.microRemaining(), 0); // Cancelling the toast does not finish it.
        timer.tick(config, start.addSecs(200));
        QCOMPARE(timer.remaining(), 59);
        timer.togglePause(start.addSecs(200));
        QCOMPARE(timer.tick(config, start.addSecs(200)), Transition::MicroBreak);
        timer.endMicroBreak(config);
        timer.togglePause(start);
        timer.endMicroBreak(config);
        QCOMPARE(timer.state(), State::Paused);

        config.microEnabled = false;
        config.idleEnabled = false;
        timer.reset(config, start);
        advance(timer, config, 70);
        QCOMPARE(timer.state(), State::Running);
        QCOMPARE(timer.microRemaining(), 60);
        config.microEnabled = true;
        timer.tick(config, start);
        QCOMPARE(timer.microRemaining(), 59);
    }

    void settingsChanges_data()
    {
        QTest::addColumn<int>("state");
        QTest::newRow("running") << int(State::Running);
        QTest::newRow("paused") << int(State::Paused);
        QTest::newRow("idle") << int(State::Idle);
        QTest::newRow("big-break") << int(State::Break);
        QTest::newRow("eye-break") << int(State::MicroBreak);
    }

    void settingsChanges()
    {
        QFETCH(int, state);
        auto config = cycleConfig();
        RestNoteTimer timer;
        timer.reset(config, start);
        advance(timer, config, 10);
        switch (State(state)) {
        case State::Running:
            break;
        case State::Paused:
            timer.togglePause(start);
            break;
        case State::Idle:
            config.idleEnabled = true;
            timer.tick(config, start.addSecs(100));
            break;
        case State::Break:
            timer.startBigBreak(config);
            break;
        case State::MicroBreak:
            advance(timer, config, 50);
            break;
        }
        QCOMPARE(timer.state(), State(state));
        config.workMinutes = 3;
        config.microIntervalMinutes = 2;
        timer.settingsChanged(config);
        QCOMPARE(timer.state(), State(state));
        QCOMPARE(timer.remaining(), State(state) == State::Running ? 110 : 180);
        QCOMPARE(timer.microRemaining(), State(state) == State::Running ? 50 : 120);
        config.workMinutes = 1;
        config.microIntervalMinutes = 0;
        timer.settingsChanged(config);
        QCOMPARE(timer.remaining(), 60);
        QCOMPARE(timer.microRemaining(), 0);
        QCOMPARE(timer.state(), State(state));
        timer.reset(config, start);
        QCOMPARE(timer.state(), State::Running);
    }
};

QTEST_GUILESS_MAIN(RestNoteCoreTest)
#include "RestNoteCoreTest.moc"
