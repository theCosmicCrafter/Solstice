/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RestNoteDock.h"

#include <KoResourcePaths.h>
#include <kis_config.h>
#include <kis_config_notifier.h>
#include <klocalizedstring.h>

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWindow>

#include <algorithm>
#include <functional>

namespace
{
int jsonInt(const QJsonObject &object, const char *key, int fallback)
{
    return object.value(QLatin1String(key)).toInt(fallback);
}

class ProgressButton : public QPushButton
{
public:
    explicit ProgressButton(int durationSeconds, QWidget *parent = nullptr)
        : QPushButton(i18n("Resume work"), parent)
        , m_duration(std::max(1, durationSeconds) * 1000)
    {
        setEnabled(false);
        setMinimumSize(320, 64);
        QFont f = font();
        f.setPointSize(15);
        setFont(f);
        setCursor(Qt::ArrowCursor);
        auto *timer = new QTimer(this);
        timer->setInterval(200);
        connect(timer, &QTimer::timeout, this, [this, timer]() {
            m_elapsed = std::min(m_duration, m_elapsed + timer->interval());
            if (m_elapsed == m_duration) {
                timer->stop();
                setEnabled(true);
                setCursor(Qt::PointingHandCursor);
            }
            update();
        });
        timer->start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        QPainterPath path;
        path.addRoundedRect(r, 8, 8);
        painter.setClipPath(path);
        painter.fillRect(r, QColor(45, 45, 48));
        const qreal progress = qreal(m_elapsed) / m_duration;
        if (progress > 0) {
            QColor fill(170, 145, 110);
            if (!isEnabled()) {
                fill = QColor(55 + int(20 * progress), 55 + int(13 * progress), 58 + int(2 * progress));
            }
            QRectF fillRect = r;
            fillRect.setWidth(r.width() * progress);
            painter.fillRect(fillRect, fill);
        }
        painter.setClipping(false);
        painter.setFont(font());
        painter.setPen(isEnabled() ? QColor(245, 235, 220) : QColor(140, 140, 145));
        painter.drawText(r, Qt::AlignCenter, text());
        painter.setPen(QPen(isEnabled() ? QColor(170, 145, 110, 140) : QColor(255, 255, 255, 25), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(r, 8, 8);
    }

private:
    int m_duration{1000};
    int m_elapsed{0};
};

class BreakOverlay : public QWidget
{
public:
    BreakOverlay(const RestNoteConfig &config, QWidget *host, std::function<void()> finished)
        : QWidget(host)
        , m_host(host)
        , m_finished(std::move(finished))
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::StrongFocus);
        if (m_host) {
            setGeometry(m_host->rect());
            m_host->installEventFilter(this);
        }

        auto *layout = new QVBoxLayout(this);
        layout->setAlignment(Qt::AlignCenter);
        layout->setSpacing(36);
        auto *title = new QLabel(i18n("Time for a break"), this);
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet(
            QStringLiteral("color: rgba(230,220,205,200); font-size:%1px; font-weight:200; letter-spacing:8px;")
                .arg(config.overlayTitleFontSize));
        layout->addWidget(title);
        auto *message = new QLabel(
            i18n("%1-minute break.\nLook into the distance and let your eyes relax.", std::max(1, config.breakMinutes)),
            this);
        message->setAlignment(Qt::AlignCenter);
        message->setStyleSheet(
            QStringLiteral("color: rgba(210,200,185,160); font-size:%1px;").arg(config.overlayMessageFontSize));
        layout->addWidget(message);
        auto *resume = new ProgressButton(config.breakSeconds(), this);
        layout->addWidget(resume, 0, Qt::AlignCenter);
        auto *skip = new QPushButton(i18n("Skip"), this);
        skip->setCursor(Qt::PointingHandCursor);
        skip->setStyleSheet(
            QStringLiteral("color:rgba(180,170,155,140); background:transparent; border:none; font-size:%1px;")
                .arg(config.overlaySkipFontSize));
        layout->addWidget(skip, 0, Qt::AlignCenter);
        connect(resume, &QPushButton::clicked, this, &BreakOverlay::finish);
        connect(skip, &QPushButton::clicked, this, &BreakOverlay::finish);

        auto *fade = new QVariantAnimation(this);
        fade->setDuration(30000);
        fade->setStartValue(0);
        fade->setEndValue(235);
        fade->setEasingCurve(QEasingCurve::InOutSine);
        connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_alpha = value.toInt();
            update();
        });
        fade->start();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_host && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
            setGeometry(m_host->rect());
            raise();
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), QColor(15, 17, 22, m_alpha));
    }

private:
    void finish()
    {
        const auto callback = std::move(m_finished);
        close();
        deleteLater();
        if (callback)
            callback();
    }

    int m_alpha{0};
    QPointer<QWidget> m_host;
    std::function<void()> m_finished;
};

class MicroToast : public QWidget
{
public:
    MicroToast(const RestNoteConfig &config, QScreen *screen, std::function<void()> finished)
        : m_duration(std::max(1, config.microDurationSeconds) * 1000)
        , m_finished(std::move(finished))
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::WindowTransparentForInput);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        resize(config.toastWidth, config.toastHeight);
        const QRect available = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
        move(available.right() - width() - config.toastMargin, available.bottom() - height() - config.toastMargin);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(20, 16, 20, 16);
        layout->setSpacing(6);
        auto *title = new QLabel(i18n("Eye break"), this);
        title->setStyleSheet(QStringLiteral("color:rgba(230,220,205,220); font-size:%1px; letter-spacing:3px;")
                                 .arg(config.toastTitleFontSize));
        layout->addWidget(title);
        auto *message = new QLabel(i18n("Look about 6 m (20 ft) away for 20 seconds."), this);
        message->setStyleSheet(
            QStringLiteral("color:rgba(245,235,220,200); font-size:%1px;").arg(config.toastMessageFontSize));
        layout->addWidget(message);
        layout->addStretch();
        setWindowOpacity(0);
        auto *fadeIn = new QVariantAnimation(this);
        fadeIn->setDuration(800);
        fadeIn->setStartValue(0.0);
        fadeIn->setEndValue(1.0);
        fadeIn->setEasingCurve(QEasingCurve::OutCubic);
        connect(fadeIn, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            setWindowOpacity(value.toReal());
        });
        fadeIn->start();
        m_timer = new QTimer(this);
        m_timer->setInterval(100);
        connect(m_timer, &QTimer::timeout, this, [this]() {
            m_elapsed = std::min(m_duration, m_elapsed + m_timer->interval());
            update();
            if (m_elapsed == m_duration)
                fadeOut();
        });
        m_timer->start();
    }

    void cancel()
    {
        m_finished = {};
        fadeOut();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
        QPainterPath path;
        path.addRoundedRect(r, 10, 10);
        painter.setClipPath(path);
        painter.fillRect(r, QColor(28, 30, 36, 235));
        painter.setClipping(false);
        painter.setPen(QPen(QColor(170, 145, 110, 80), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(r, 10, 10);
        const qreal remaining = 1.0 - qreal(m_elapsed) / m_duration;
        painter.fillRect(QRectF(r.left() + 2, r.bottom() - 5, r.width() - 4, 3), QColor(255, 255, 255, 20));
        painter.fillRect(QRectF(r.left() + 2, r.bottom() - 5, (r.width() - 4) * remaining, 3),
                         QColor(170, 145, 110, 200));
    }

private:
    void fadeOut()
    {
        if (m_fading)
            return;
        m_fading = true;
        m_timer->stop();
        auto *fade = new QVariantAnimation(this);
        fade->setDuration(1200);
        fade->setStartValue(windowOpacity());
        fade->setEndValue(0.0);
        fade->setEasingCurve(QEasingCurve::InCubic);
        connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            setWindowOpacity(value.toReal());
        });
        connect(fade, &QVariantAnimation::finished, this, [this]() {
            const auto callback = std::move(m_finished);
            close();
            deleteLater();
            if (callback)
                callback();
        });
        fade->start();
    }

    int m_duration{20000};
    int m_elapsed{0};
    bool m_fading{false};
    QTimer *m_timer{nullptr};
    std::function<void()> m_finished;
};

QSpinBox *spinBox(int minimum, int maximum, int value, const QString &suffix, QWidget *parent)
{
    auto *spin = new QSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setValue(value);
    spin->setSuffix(suffix);
    return spin;
}
} // namespace

int RestNoteConfig::workSeconds() const
{
    return workMinutes * 60;
}

int RestNoteConfig::breakSeconds() const
{
    return breakMinutes * 60;
}

int RestNoteConfig::microIntervalSeconds() const
{
    return microIntervalMinutes * 60;
}

QString RestNoteConfig::filePath()
{
    return QDir(KoResourcePaths::saveLocation("data", "rest_note/config/", true)).filePath(QStringLiteral("main.json"));
}

void RestNoteConfig::load()
{
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly)) {
        save();
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
    workMinutes = jsonInt(o, "work_minutes", workMinutes);
    breakMinutes = jsonInt(o, "break_minutes", breakMinutes);
    microEnabled = o.value(QStringLiteral("micro_break_enabled")).toBool(microEnabled);
    microIntervalMinutes = jsonInt(o, "micro_break_interval_minutes", microIntervalMinutes);
    microDurationSeconds = jsonInt(o, "micro_break_duration_seconds", microDurationSeconds);
    microSkipThresholdSeconds = jsonInt(o, "micro_skip_threshold_seconds", microSkipThresholdSeconds);
    idleEnabled = o.value(QStringLiteral("idle_enabled")).toBool(idleEnabled);
    idleThresholdSeconds = jsonInt(o, "idle_threshold_seconds", idleThresholdSeconds);
    toastMargin = jsonInt(o, "micro_toast_margin", toastMargin);
    toastWidth = jsonInt(o, "micro_toast_width", toastWidth);
    toastHeight = jsonInt(o, "micro_toast_height", toastHeight);
    toastTitleFontSize = jsonInt(o, "micro_toast_title_font_size", toastTitleFontSize);
    toastMessageFontSize = jsonInt(o, "micro_toast_message_font_size", toastMessageFontSize);
    overlayTitleFontSize = jsonInt(o, "overlay_title_font_size", overlayTitleFontSize);
    overlayMessageFontSize = jsonInt(o, "overlay_message_font_size", overlayMessageFontSize);
    overlaySkipFontSize = jsonInt(o, "overlay_skip_font_size", overlaySkipFontSize);
}

bool RestNoteConfig::save() const
{
    QJsonObject o{{QStringLiteral("work_minutes"), workMinutes},
                  {QStringLiteral("break_minutes"), breakMinutes},
                  {QStringLiteral("micro_break_enabled"), microEnabled},
                  {QStringLiteral("micro_break_interval_minutes"), microIntervalMinutes},
                  {QStringLiteral("micro_break_duration_seconds"), microDurationSeconds},
                  {QStringLiteral("micro_skip_threshold_seconds"), microSkipThresholdSeconds},
                  {QStringLiteral("idle_enabled"), idleEnabled},
                  {QStringLiteral("idle_threshold_seconds"), idleThresholdSeconds},
                  {QStringLiteral("micro_toast_margin"), toastMargin},
                  {QStringLiteral("micro_toast_width"), toastWidth},
                  {QStringLiteral("micro_toast_height"), toastHeight},
                  {QStringLiteral("micro_toast_title_font_size"), toastTitleFontSize},
                  {QStringLiteral("micro_toast_message_font_size"), toastMessageFontSize},
                  {QStringLiteral("overlay_title_font_size"), overlayTitleFontSize},
                  {QStringLiteral("overlay_message_font_size"), overlayMessageFontSize},
                  {QStringLiteral("overlay_skip_font_size"), overlaySkipFontSize}};
    QFile file(filePath());
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(QJsonDocument(o).toJson()) >= 0;
}

RestNoteDock::RestNoteDock(QWidget *parent)
    : QDockWidget(parent)
{
    setWindowTitle(i18n("Rest Note"));
    m_config.load();
    m_remaining = m_config.workSeconds();
    m_microRemaining = m_config.microIntervalSeconds();
    m_lastActivity = QDateTime::currentDateTime();
    buildUi();
    m_tickTimer = new QTimer(this);
    connect(m_tickTimer, &QTimer::timeout, this, &RestNoteDock::tick);
    connect(KisConfigNotifier::instance(), &KisConfigNotifier::configChanged, this, &RestNoteDock::applyEnabled);
    applyEnabled();
    refreshDisplay();
}

RestNoteDock::~RestNoteDock()
{
    if (m_active)
        qApp->removeEventFilter(this);
    cancelTransientWindows();
}

bool RestNoteDock::isEnabled()
{
    return KisConfig(true).readEntry<bool>("Solstice/RestNoteEnabled", false);
}

void RestNoteDock::applyEnabled()
{
    // Turning the setting off stops the timers, breaks and idle tracking at
    // once and hides the docker; turning it on again (when the docker was
    // loaded at startup) starts a fresh work period.
    const bool enabled = isEnabled();
    if (enabled == m_active)
        return;
    m_active = enabled;
    if (enabled) {
        qApp->installEventFilter(this);
        m_state = State::Running;
        m_remaining = m_config.workSeconds();
        m_microRemaining = m_config.microIntervalSeconds();
        m_lastActivity = QDateTime::currentDateTime();
        m_tickTimer->start(1000);
        toggleViewAction()->setEnabled(true);
        refreshDisplay();
    } else {
        qApp->removeEventFilter(this);
        m_tickTimer->stop();
        cancelTransientWindows();
        hide();
        toggleViewAction()->setEnabled(false);
    }
}

void RestNoteDock::buildUi()
{
    m_root = new QWidget(this);
    auto *layout = new QVBoxLayout(m_root);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    m_statusLabel = new QLabel(i18n("WORKING"), m_root);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet(QStringLiteral("color:#b8b8b8; letter-spacing:2px;"));
    layout->addWidget(m_statusLabel);
    m_timeLabel = new QLabel(QStringLiteral("00:00"), m_root);
    m_timeLabel->setAlignment(Qt::AlignCenter);
    m_timeLabel->setStyleSheet(QStringLiteral("color:#e6dcc8;"));
    layout->addWidget(m_timeLabel);
    m_subLabel = new QLabel(m_root);
    m_subLabel->setAlignment(Qt::AlignCenter);
    m_subLabel->setStyleSheet(QStringLiteral("color:rgba(184,184,184,180);"));
    layout->addWidget(m_subLabel);
    auto *separator = new QFrame(m_root);
    separator->setFrameShape(QFrame::HLine);
    separator->setStyleSheet(QStringLiteral("color:rgba(255,255,255,30);"));
    layout->addWidget(separator);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    const auto makeButton = [this, buttons](const QString &iconPath, const QString &tip) {
        auto *button = new QPushButton(m_root);
        button->setIcon(QIcon(iconPath));
        button->setToolTip(tip);
        button->setStyleSheet(QStringLiteral("background-color:rgb(70,70,70);"));
        buttons->addWidget(button);
        return button;
    };
    m_pauseButton = makeButton(QStringLiteral(":/restnote/icons/pause.png"), i18n("Pause / Resume"));
    m_resetButton = makeButton(QStringLiteral(":/restnote/icons/refresh.png"), i18n("Reset"));
    m_restButton = makeButton(QStringLiteral(":/restnote/icons/rest.png"), i18n("Take a break now"));
    m_settingsButton = makeButton(QStringLiteral(":/restnote/icons/setting.png"), i18n("Settings…"));
    buttons->addStretch();
    layout->addLayout(buttons);
    setWidget(m_root);

    connect(m_pauseButton, &QPushButton::clicked, this, [this]() {
        if (m_state == State::Running || m_state == State::Idle) {
            m_state = State::Paused;
        } else if (m_state == State::Paused) {
            m_state = State::Running;
            m_lastActivity = QDateTime::currentDateTime();
        } else if (m_state == State::MicroBreak) {
            if (auto *toast = dynamic_cast<MicroToast *>(m_microToast.data()))
                toast->cancel();
            m_microToast = nullptr;
            m_state = State::Paused;
        }
        refreshDisplay();
    });
    connect(m_resetButton, &QPushButton::clicked, this, [this]() {
        cancelTransientWindows();
        m_state = State::Running;
        m_remaining = m_config.workSeconds();
        m_microRemaining = m_config.microIntervalSeconds();
        m_lastActivity = QDateTime::currentDateTime();
        refreshDisplay();
    });
    connect(m_restButton, &QPushButton::clicked, this, [this]() {
        if (m_state != State::Break)
            startBigBreak();
    });
    connect(m_settingsButton, &QPushButton::clicked, this, &RestNoteDock::showSettings);
    updateSizing();
}

bool RestNoteDock::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::Wheel:
    case QEvent::TabletMove:
    case QEvent::TabletPress:
        m_lastActivity = QDateTime::currentDateTime();
        break;
    default:
        break;
    }
    return false;
}

void RestNoteDock::resizeEvent(QResizeEvent *event)
{
    QDockWidget::resizeEvent(event);
    updateSizing();
}

void RestNoteDock::updateSizing()
{
    if (!m_root)
        return;
    const int timePointSize = std::clamp(int(std::min(m_root->width() / 5.5, m_root->height() / 5.5)), 14, 96);
    QFont timeFont = m_timeLabel->font();
    timeFont.setPointSize(timePointSize);
    timeFont.setWeight(QFont::Light);
    m_timeLabel->setFont(timeFont);
    m_timeLabel->setMinimumHeight(QFontMetrics(timeFont).height() + 6);
    QFont statusFont = m_statusLabel->font();
    statusFont.setPointSize(std::clamp(int(timePointSize * .28), 8, 24));
    m_statusLabel->setFont(statusFont);
    QFont subFont = m_subLabel->font();
    subFont.setPointSize(std::clamp(int(timePointSize * .22), 8, 18));
    m_subLabel->setFont(subFont);
    const int buttonSize = std::clamp(int(timePointSize * .5), 16, 64);
    for (auto *button : {m_pauseButton, m_resetButton, m_restButton, m_settingsButton}) {
        button->setFixedSize(buttonSize, buttonSize);
        button->setIconSize(QSize(buttonSize, buttonSize));
    }
}

void RestNoteDock::tick()
{
    if (m_config.idleEnabled) {
        const qint64 idle = m_lastActivity.secsTo(QDateTime::currentDateTime());
        if (m_state == State::Running && idle >= m_config.idleThresholdSeconds)
            m_state = State::Idle;
        else if (m_state == State::Idle && idle < m_config.idleThresholdSeconds)
            m_state = State::Running;
    } else if (m_state == State::Idle) {
        m_state = State::Running;
    }

    if (m_state == State::Running) {
        if (--m_remaining <= 0) {
            startBigBreak();
            return;
        }
        if (m_config.microEnabled && --m_microRemaining <= 0)
            maybeStartMicroBreak();
    } else if (m_state == State::MicroBreak && --m_remaining <= 0) {
        if (auto *toast = dynamic_cast<MicroToast *>(m_microToast.data()))
            toast->cancel();
        m_microToast = nullptr;
        startBigBreak();
        return;
    }
    refreshDisplay();
}

void RestNoteDock::refreshDisplay()
{
    const int value = std::max(0, m_remaining);
    m_timeLabel->setText(
        QStringLiteral("%1:%2").arg(value / 60, 2, 10, QLatin1Char('0')).arg(value % 60, 2, 10, QLatin1Char('0')));
    m_pauseButton->setEnabled(m_state != State::Break);
    if (m_state == State::Paused)
        m_pauseButton->setIcon(QIcon(QStringLiteral(":/restnote/icons/play.png")));
    else
        m_pauseButton->setIcon(QIcon(QStringLiteral(":/restnote/icons/pause.png")));

    switch (m_state) {
    case State::Running:
        m_statusLabel->setText(i18n("WORKING"));
        break;
    case State::Paused:
        m_statusLabel->setText(i18n("PAUSED"));
        break;
    case State::Break:
        m_statusLabel->setText(i18n("ON BREAK"));
        break;
    case State::MicroBreak:
        m_statusLabel->setText(i18n("EYE BREAK"));
        break;
    case State::Idle:
        m_statusLabel->setText(i18n("IDLE"));
        break;
    }
    if (m_state == State::Idle) {
        m_subLabel->setText(i18n("Away — timer paused"));
    } else if (!m_config.microEnabled) {
        m_subLabel->setText(i18n("Eye breaks: disabled"));
    } else if (m_state == State::Break) {
        m_subLabel->setText(QStringLiteral("—"));
    } else if (m_state == State::MicroBreak) {
        m_subLabel->setText(i18n("Look about 6 m (20 ft) away"));
    } else {
        const int micro = std::max(0, m_microRemaining);
        m_subLabel->setText(i18n("Next eye break in %1:%2",
                                 QString::number(micro / 60).rightJustified(2, QLatin1Char('0')),
                                 QString::number(micro % 60).rightJustified(2, QLatin1Char('0'))));
    }
}

QScreen *RestNoteDock::currentScreen() const
{
    if (window() && window()->windowHandle() && window()->windowHandle()->screen())
        return window()->windowHandle()->screen();
    return QGuiApplication::primaryScreen();
}

QWidget *RestNoteDock::mainWindowHost() const
{
    QWidget *candidate = const_cast<RestNoteDock *>(this);
    while (candidate) {
        if (qobject_cast<QMainWindow *>(candidate))
            return candidate;
        candidate = candidate->parentWidget();
    }

    candidate = QApplication::activeWindow();
    return qobject_cast<QMainWindow *>(candidate) ? candidate : window();
}

void RestNoteDock::startBigBreak()
{
    if (auto *toast = dynamic_cast<MicroToast *>(m_microToast.data()))
        toast->cancel();
    m_microToast = nullptr;
    if (m_overlay)
        m_overlay->close();
    m_microRemaining = m_config.microIntervalSeconds();
    m_remaining = 0;
    m_state = State::Break;
    m_overlay = new BreakOverlay(m_config, mainWindowHost(), [this]() {
        endBigBreak();
    });
    m_overlay->show();
    m_overlay->raise();
    m_overlay->setFocus(Qt::OtherFocusReason);
    refreshDisplay();
}

void RestNoteDock::endBigBreak()
{
    m_overlay = nullptr;
    m_state = State::Running;
    m_remaining = m_config.workSeconds();
    m_microRemaining = m_config.microIntervalSeconds();
    m_lastActivity = QDateTime::currentDateTime();
    refreshDisplay();
}

void RestNoteDock::maybeStartMicroBreak()
{
    if (m_remaining <= m_config.microSkipThresholdSeconds) {
        m_microRemaining = m_config.microIntervalSeconds();
        return;
    }
    m_state = State::MicroBreak;
    m_microToast = new MicroToast(m_config, currentScreen(), [this]() {
        endMicroBreak();
    });
    m_microToast->show();
}

void RestNoteDock::endMicroBreak()
{
    m_microToast = nullptr;
    m_microRemaining = m_config.microIntervalSeconds();
    if (m_state == State::MicroBreak)
        m_state = State::Running;
    refreshDisplay();
}

void RestNoteDock::cancelTransientWindows()
{
    if (m_overlay) {
        m_overlay->close();
        m_overlay->deleteLater();
        m_overlay = nullptr;
    }
    if (auto *toast = dynamic_cast<MicroToast *>(m_microToast.data()))
        toast->cancel();
    m_microToast = nullptr;
}

void RestNoteDock::showSettings()
{
    QDialog dialog(widget());
    dialog.setWindowTitle(i18n("Rest Note Settings"));
    dialog.setMinimumWidth(360);
    auto *outer = new QVBoxLayout(&dialog);
    auto *cycleGroup = new QGroupBox(i18n("Work / Break cycle"), &dialog);
    auto *cycleForm = new QFormLayout(cycleGroup);
    auto *work = spinBox(1, 180, m_config.workMinutes, i18n(" min"), cycleGroup);
    auto *rest = spinBox(1, 60, m_config.breakMinutes, i18n(" min"), cycleGroup);
    cycleForm->addRow(i18n("Work duration:"), work);
    cycleForm->addRow(i18n("Break duration:"), rest);
    outer->addWidget(cycleGroup);

    auto *microGroup = new QGroupBox(i18n("Eye break reminder (20-20-20)"), &dialog);
    auto *microLayout = new QVBoxLayout(microGroup);
    auto *microEnabled = new QCheckBox(i18n("Enable eye break reminders"), microGroup);
    microEnabled->setChecked(m_config.microEnabled);
    microLayout->addWidget(microEnabled);
    auto *microForm = new QFormLayout;
    auto *interval = spinBox(1, 60, m_config.microIntervalMinutes, i18n(" min"), microGroup);
    auto *duration = spinBox(5, 120, m_config.microDurationSeconds, i18n(" sec"), microGroup);
    auto *skip = spinBox(0, 600, m_config.microSkipThresholdSeconds, i18n(" sec"), microGroup);
    microForm->addRow(i18n("Interval:"), interval);
    microForm->addRow(i18n("Duration:"), duration);
    microForm->addRow(i18n("Skip if a long break is due within:"), skip);
    microLayout->addLayout(microForm);
    outer->addWidget(microGroup);
    for (auto *control : {interval, duration, skip}) {
        control->setEnabled(microEnabled->isChecked());
        connect(microEnabled, &QCheckBox::toggled, control, &QWidget::setEnabled);
    }

    auto *idleGroup = new QGroupBox(i18n("Idle detection"), &dialog);
    auto *idleLayout = new QVBoxLayout(idleGroup);
    auto *idleEnabled = new QCheckBox(i18n("Enable idle detection"), idleGroup);
    idleEnabled->setChecked(m_config.idleEnabled);
    idleLayout->addWidget(idleEnabled);
    auto *idleForm = new QFormLayout;
    auto *idleThreshold = spinBox(5, 3600, m_config.idleThresholdSeconds, i18n(" sec"), idleGroup);
    idleForm->addRow(i18n("Idle threshold:"), idleThreshold);
    idleLayout->addLayout(idleForm);
    idleThreshold->setEnabled(idleEnabled->isChecked());
    connect(idleEnabled, &QCheckBox::toggled, idleThreshold, &QWidget::setEnabled);
    outer->addWidget(idleGroup);

    auto *overlayGroup = new QGroupBox(i18n("Overlay appearance"), &dialog);
    auto *overlayForm = new QFormLayout(overlayGroup);
    auto *overlayTitle = spinBox(6, 200, m_config.overlayTitleFontSize, i18n(" px"), overlayGroup);
    auto *overlayMessage = spinBox(6, 200, m_config.overlayMessageFontSize, i18n(" px"), overlayGroup);
    auto *overlaySkip = spinBox(6, 200, m_config.overlaySkipFontSize, i18n(" px"), overlayGroup);
    overlayForm->addRow(i18n("Title font size:"), overlayTitle);
    overlayForm->addRow(i18n("Message font size:"), overlayMessage);
    overlayForm->addRow(i18n("Skip font size:"), overlaySkip);
    outer->addWidget(overlayGroup);

    auto *toastGroup = new QGroupBox(i18n("Toast appearance"), &dialog);
    auto *toastForm = new QFormLayout(toastGroup);
    auto *margin = spinBox(0, 200, m_config.toastMargin, i18n(" px"), toastGroup);
    auto *toastWidth = spinBox(100, 800, m_config.toastWidth, i18n(" px"), toastGroup);
    auto *toastHeight = spinBox(50, 400, m_config.toastHeight, i18n(" px"), toastGroup);
    auto *toastTitle = spinBox(6, 48, m_config.toastTitleFontSize, i18n(" px"), toastGroup);
    auto *toastMessage = spinBox(6, 48, m_config.toastMessageFontSize, i18n(" px"), toastGroup);
    toastForm->addRow(i18n("Margin:"), margin);
    toastForm->addRow(i18n("Width:"), toastWidth);
    toastForm->addRow(i18n("Height:"), toastHeight);
    toastForm->addRow(i18n("Title font size:"), toastTitle);
    toastForm->addRow(i18n("Message font size:"), toastMessage);
    outer->addWidget(toastGroup);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_config.workMinutes = work->value();
    m_config.breakMinutes = rest->value();
    m_config.microEnabled = microEnabled->isChecked();
    m_config.microIntervalMinutes = interval->value();
    m_config.microDurationSeconds = duration->value();
    m_config.microSkipThresholdSeconds = skip->value();
    m_config.idleEnabled = idleEnabled->isChecked();
    m_config.idleThresholdSeconds = idleThreshold->value();
    m_config.overlayTitleFontSize = overlayTitle->value();
    m_config.overlayMessageFontSize = overlayMessage->value();
    m_config.overlaySkipFontSize = overlaySkip->value();
    m_config.toastMargin = margin->value();
    m_config.toastWidth = toastWidth->value();
    m_config.toastHeight = toastHeight->value();
    m_config.toastTitleFontSize = toastTitle->value();
    m_config.toastMessageFontSize = toastMessage->value();
    m_config.save();
    if (m_state == State::Running) {
        m_remaining = std::min(m_remaining, m_config.workSeconds());
        m_microRemaining = std::min(m_microRemaining, m_config.microIntervalSeconds());
    } else {
        m_remaining = m_config.workSeconds();
        m_microRemaining = m_config.microIntervalSeconds();
    }
    refreshDisplay();
}

QString RestNoteDockFactory::id() const
{
    return QStringLiteral("RestNoteDocker");
}

QDockWidget *RestNoteDockFactory::createDockWidget()
{
    auto *dock = new RestNoteDock;
    dock->setObjectName(id());
    return dock;
}

KoDockFactoryBase::DockPosition RestNoteDockFactory::defaultDockPosition() const
{
    return DockMinimized;
}
