/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RestNoteDock.h"
#include "RestNoteNotifications.h"
#include "RestNoteSettingsDialog.h"

#include <kis_config.h>
#include <kis_config_notifier.h>
#include <klocalizedstring.h>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>

RestNoteDock::RestNoteDock(QWidget *parent)
    : QDockWidget(parent)
{
    setWindowTitle(i18n("Rest Note"));
    m_config.load();
    m_timer.reset(m_config, QDateTime::currentDateTime());
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
        m_timer.reset(m_config, QDateTime::currentDateTime());
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
        if (m_timer.togglePause(QDateTime::currentDateTime())) {
            RestNoteNotifications::cancelEyeBreak(m_microToast.data());
            m_microToast = nullptr;
        }
        refreshDisplay();
    });
    connect(m_resetButton, &QPushButton::clicked, this, [this]() {
        cancelTransientWindows();
        m_timer.reset(m_config, QDateTime::currentDateTime());
        refreshDisplay();
    });
    connect(m_restButton, &QPushButton::clicked, this, [this]() {
        if (m_timer.state() != State::Break)
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
        m_timer.recordActivity(QDateTime::currentDateTime());
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
    switch (m_timer.tick(m_config, QDateTime::currentDateTime())) {
    case RestNoteTimer::Transition::BigBreak:
        showBigBreak();
        return;
    case RestNoteTimer::Transition::MicroBreak:
        showMicroBreak();
        break;
    case RestNoteTimer::Transition::None:
        break;
    }
    refreshDisplay();
}

void RestNoteDock::refreshDisplay()
{
    const int value = std::max(0, m_timer.remaining());
    m_timeLabel->setText(
        QStringLiteral("%1:%2").arg(value / 60, 2, 10, QLatin1Char('0')).arg(value % 60, 2, 10, QLatin1Char('0')));
    m_pauseButton->setEnabled(m_timer.state() != State::Break);
    if (m_timer.state() == State::Paused)
        m_pauseButton->setIcon(QIcon(QStringLiteral(":/restnote/icons/play.png")));
    else
        m_pauseButton->setIcon(QIcon(QStringLiteral(":/restnote/icons/pause.png")));

    switch (m_timer.state()) {
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
    if (m_timer.state() == State::Idle) {
        m_subLabel->setText(i18n("Away — timer paused"));
    } else if (!m_config.microEnabled) {
        m_subLabel->setText(i18n("Eye breaks: disabled"));
    } else if (m_timer.state() == State::Break) {
        m_subLabel->setText(QStringLiteral("—"));
    } else if (m_timer.state() == State::MicroBreak) {
        m_subLabel->setText(i18n("Look about 6 m (20 ft) away"));
    } else {
        const int micro = std::max(0, m_timer.microRemaining());
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
    m_timer.startBigBreak(m_config);
    showBigBreak();
}

void RestNoteDock::showBigBreak()
{
    RestNoteNotifications::cancelEyeBreak(m_microToast.data());
    m_microToast = nullptr;
    if (m_overlay)
        m_overlay->close();
    m_overlay = RestNoteNotifications::createBreakOverlay(m_config, mainWindowHost(), [this]() {
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
    m_timer.reset(m_config, QDateTime::currentDateTime());
    refreshDisplay();
}

void RestNoteDock::showMicroBreak()
{
    m_microToast = RestNoteNotifications::createEyeBreak(m_config, currentScreen(), [this]() {
        endMicroBreak();
    });
    m_microToast->show();
}

void RestNoteDock::endMicroBreak()
{
    m_microToast = nullptr;
    m_timer.endMicroBreak(m_config);
    refreshDisplay();
}

void RestNoteDock::cancelTransientWindows()
{
    if (m_overlay) {
        m_overlay->close();
        m_overlay->deleteLater();
        m_overlay = nullptr;
    }
    RestNoteNotifications::cancelEyeBreak(m_microToast.data());
    m_microToast = nullptr;
}

void RestNoteDock::showSettings()
{
    if (!RestNoteSettingsDialog::edit(m_config, widget()))
        return;
    m_config.save();
    m_timer.settingsChanged(m_config);
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
