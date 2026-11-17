/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAdjustToolOptionsPad.h"

#include <QAction>
#include <QDockWidget>
#include <QGuiApplication>
#include <QLabel>
#include <QMainWindow>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <kconfiggroup.h>
#include <klocalizedstring.h>
#include <utility>

QuickAdjustToolOptionsPad::QuickAdjustToolOptionsPad(QDockWidget *owner,
                                                     QToolButton *toggle,
                                                     bool compactPopup,
                                                     ContextProvider context,
                                                     KSharedConfigPtr config)
    : QObject(owner)
    , m_owner(owner)
    , m_toolOptionsToggle(toggle)
    , m_compactPopup(compactPopup)
    , m_context(std::move(context))
    , m_config(std::move(config))
{
}

QuickAdjustToolOptionsPad::~QuickAdjustToolOptionsPad()
{
    restore();
}

void QuickAdjustToolOptionsPad::hide()
{
    if (m_toolOptionsPad)
        m_toolOptionsPad->hide();
}

void QuickAdjustToolOptionsPad::ensure()
{
    if (m_compactPopup || m_toolOptionsPad || !m_toolOptionsToggle->isVisible())
        return;
    const Context context = m_context();
    if (!context.canvasAvailable)
        return;
    if (!context.toolOptionsInDocker) {
        m_toolOptionsToggle->hide();
        return;
    }

    QWidget *mainWindow = context.mainWindow;
    QDockWidget *docker =
        mainWindow ? mainWindow->findChild<QDockWidget *>(QStringLiteral("sharedtooldocker")) : nullptr;
    if (!docker || !docker->widget()) {
        m_toolOptionsToggle->hide();
        return;
    }

    m_toolOptionsDocker = docker;
    m_toolOptionsDockerWasVisible = docker->isVisible();
    m_borrowedToolOptions = docker->widget();
    m_toolOptionsPlaceholder = new QLabel(i18nc("@info", "Tool Options are open beside Quick Brush Adjustments."));
    m_toolOptionsPlaceholder->hide();

    m_toolOptionsPad = new QWidget(mainWindow, Qt::Tool | Qt::FramelessWindowHint);
    m_toolOptionsPad->setObjectName(QStringLiteral("quickAccessToolOptionsPad"));
    m_toolOptionsPad->setAutoFillBackground(true);
    auto *padLayout = new QVBoxLayout(m_toolOptionsPad);
    padLayout->setContentsMargins(4, 4, 4, 4);
    padLayout->addWidget(m_borrowedToolOptions);
    docker->setWidget(m_toolOptionsPlaceholder);
    docker->hide();
    docker->toggleViewAction()->setEnabled(false);

    padLayout->invalidate();
    padLayout->activate();
    resizeToContent();
    setVisible(m_toolOptionsToggle->isChecked());

    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (!m_toolOptionsPad)
            return;
        if (state != Qt::ApplicationActive) {
            m_toolOptionsPad->hide();
        } else if (m_toolOptionsToggle && m_toolOptionsToggle->isChecked() && m_owner->isVisible()) {
            resizeToContent();
            m_toolOptionsPad->show();
            position();
        }
    });
}

void QuickAdjustToolOptionsPad::setVisible(bool visible)
{
    if (!m_compactPopup && visible && !m_toolOptionsPad)
        ensure();
    if (m_toolOptionsPad) {
        resizeToContent();
        m_toolOptionsPad->setVisible(visible && m_owner->isVisible()
                                     && QGuiApplication::applicationState() == Qt::ApplicationActive);
        if (visible) {
            position();
            QTimer::singleShot(0, this, [this] {
                resizeToContent();
                position();
            });
        }
    }
    if (m_toolOptionsToggle) {
        m_toolOptionsToggle->setIcon(QIcon(QStringLiteral(":/quickaccess/quick_adjust/tool_options_%1.png")
                                               .arg(visible ? QStringLiteral("on") : QStringLiteral("off"))));
    }
    KConfigGroup config = m_config->group(QStringLiteral("QuickAccessAdjust"));
    config.writeEntry("ToolOptionsStartVisible", visible);
    config.sync();
}

void QuickAdjustToolOptionsPad::resizeToContent()
{
    if (!m_toolOptionsPad || !m_borrowedToolOptions)
        return;

    QSize contentSize = m_borrowedToolOptions->sizeHint();
    QScrollArea *scrollArea = qobject_cast<QScrollArea *>(m_borrowedToolOptions.data());
    if (scrollArea && scrollArea->widget()) {
        QWidget *contents = scrollArea->widget();
        contents->ensurePolished();
        if (contents->layout()) {
            contents->layout()->invalidate();
            contents->layout()->activate();
        }
        contentSize = contents->sizeHint();
        const int frame = scrollArea->frameWidth() * 2;
        contentSize += QSize(frame, frame);
    }
    if (!contentSize.isValid() || contentSize.isEmpty())
        contentSize = m_borrowedToolOptions->minimumSizeHint();
    contentSize = contentSize.expandedTo(QSize(1, 1));

    QSize maximumSize;
    const Context context = m_context();
    if (context.canvasAvailable && context.mainWindow)
        maximumSize = context.mainWindow->centralWidget()->size();
    if ((!maximumSize.isValid() || maximumSize.isEmpty()) && m_toolOptionsPad->screen())
        maximumSize = m_toolOptionsPad->screen()->availableGeometry().size();

    const QMargins margins = m_toolOptionsPad->layout()->contentsMargins();
    const QSize marginSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    if (maximumSize.isValid() && !maximumSize.isEmpty()) {
        const QSize maximumContent = (maximumSize - marginSize - QSize(8, 8)).expandedTo(QSize(1, 1));
        if (scrollArea) {
            const int scrollBarExtent = m_owner->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
            if (contentSize.height() > maximumContent.height())
                contentSize.rwidth() += scrollBarExtent;
            if (contentSize.width() > maximumContent.width())
                contentSize.rheight() += scrollBarExtent;
        }
        contentSize = contentSize.boundedTo(maximumContent);
    }

    const QSize wanted = contentSize + marginSize;
    if (wanted != m_toolOptionsPad->size()) {
        m_toolOptionsPad->resize(wanted);
        position();
    }
}

void QuickAdjustToolOptionsPad::position()
{
    if (!m_toolOptionsPad)
        return;
    const KConfigGroup config = m_config->group(QStringLiteral("QuickAccessAdjust"));
    const QString position = config.readEntry("ToolOptionsPosition", QStringLiteral("left_align_top"));
    const QPoint dockTopLeft = m_owner->mapToGlobal(QPoint(0, 0));
    QPoint target;
    if (position == QStringLiteral("right_align_top")) {
        target = QPoint(dockTopLeft.x() + m_owner->width() + 5, dockTopLeft.y());
    } else if (position == QStringLiteral("bottom_left")) {
        target = QPoint(dockTopLeft.x(), dockTopLeft.y() + m_owner->height() + 5);
    } else {
        target = QPoint(dockTopLeft.x() - m_toolOptionsPad->width() - 5, dockTopLeft.y());
    }

    if (QScreen *screen = QGuiApplication::screenAt(dockTopLeft)) {
        const QRect bounds = screen->availableGeometry();
        target.setX(qBound(bounds.left(), target.x(), bounds.right() - m_toolOptionsPad->width() + 1));
        target.setY(qBound(bounds.top(), target.y(), bounds.bottom() - m_toolOptionsPad->height() + 1));
    }
    m_toolOptionsPad->move(target);
}

void QuickAdjustToolOptionsPad::restore()
{
    if (m_toolOptionsDocker && m_borrowedToolOptions) {
        if (m_toolOptionsPad && m_toolOptionsPad->layout())
            m_toolOptionsPad->layout()->removeWidget(m_borrowedToolOptions);
        m_toolOptionsDocker->setWidget(m_borrowedToolOptions);
        m_borrowedToolOptions->setParent(m_toolOptionsDocker);
        m_toolOptionsDocker->toggleViewAction()->setEnabled(true);
        m_toolOptionsDocker->setVisible(m_toolOptionsDockerWasVisible);
    }
    if (m_toolOptionsPlaceholder)
        m_toolOptionsPlaceholder->deleteLater();
    if (m_toolOptionsPad)
        m_toolOptionsPad->deleteLater();
    m_toolOptionsDocker.clear();
    m_borrowedToolOptions.clear();
    m_toolOptionsPlaceholder.clear();
    m_toolOptionsPad.clear();
}
