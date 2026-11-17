/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RestNoteNotifications.h"
#include "RestNoteConfig.h"

#include <QEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <klocalizedstring.h>
#include <algorithm>
#include <utility>

namespace
{
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

} // namespace

QWidget *
RestNoteNotifications::createBreakOverlay(const RestNoteConfig &config, QWidget *host, std::function<void()> finished)
{
    return new BreakOverlay(config, host, std::move(finished));
}

QWidget *
RestNoteNotifications::createEyeBreak(const RestNoteConfig &config, QScreen *screen, std::function<void()> finished)
{
    return new MicroToast(config, screen, std::move(finished));
}

void RestNoteNotifications::cancelEyeBreak(QWidget *widget)
{
    if (auto *toast = dynamic_cast<MicroToast *>(widget))
        toast->cancel();
}
