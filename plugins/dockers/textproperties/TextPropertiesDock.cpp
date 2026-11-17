/*
 *  SPDX-FileCopyrightText: 2024 Wolthera van Hövell tot Westerflier <griffinvalley@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "TextPropertiesDock.h"

#include <QQuickWidget>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QStringListModel>
#include <QQuickStyle>
#include <QColorDialog>

#include <KisQQuickWidget.h>
#include <KisScopedPerformanceLogger.h>

#include <KisViewManager.h>
#include <kis_canvas_resource_provider.h>

#include <KLocalizedContext>

#include "TextPropertiesCanvasObserver.h"

struct TextPropertiesDock::Private
{
    Private(QObject *parent = nullptr)
        : canvasObserver(new TextPropertiesCanvasObserver(parent)) {

    }
    TextPropertiesCanvasObserver *canvasObserver{nullptr};
};

TextPropertiesDock::TextPropertiesDock()
    : QDockWidget(i18n("Text Properties"))
    , d(new Private)
{
    m_quickWidget = new KisQQuickWidget(this);

    setWidget(m_quickWidget);
    setEnabled(true);

    m_quickWidget->setMinimumHeight(100);

    // Solstice: the QML is loaded when the docker's contents are first
    // shown, see loadContents().
    m_quickWidget->installEventFilter(this);

    m_quickWidget->setPalette(this->palette());
}

TextPropertiesDock::~TextPropertiesDock()
{
    delete m_quickWidget;
}

bool TextPropertiesDock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_quickWidget && event->type() == QEvent::Show) {
        loadContents();
    }
    return QDockWidget::eventFilter(watched, event);
}

void TextPropertiesDock::loadContents()
{
    /**
     * Solstice: every main window creates all dockers at start, hidden ones
     * too. Loading this docker's QML (an engine and about 44 files) took
     * about one second of each start, so it waits until the contents are
     * shown for the first time. The canvas observer does not depend on the
     * QML: it follows the canvas from the start, and the QML reads its
     * current state when it is created.
     */
    if (m_contentsLoaded)
        return;
    m_contentsLoaded = true;

    KisScopedPerformanceLogger logger(QStringLiteral("TextPropertiesDock::loadContents"));

    m_quickWidget->setSource(QUrl("qrc:/TextProperties.qml"));

    if (!m_quickWidget->errors().empty()) {
        qWarning() << "Errors in " << windowTitle() << ":" << m_quickWidget->errors();
    } else {
        m_quickWidget->rootObject()->setProperty("canvasObserver", QVariant::fromValue(d->canvasObserver));
    }
}

QString TextPropertiesDock::observerName()
{
    return d->canvasObserver->observerName();
}

void TextPropertiesDock::setViewManager(KisViewManager *kisview)
{
    d->canvasObserver->setViewManager(kisview);
}

void TextPropertiesDock::setCanvas(KoCanvasBase *canvas)
{
    d->canvasObserver->setCanvas(canvas);
}

void TextPropertiesDock::unsetCanvas()
{
    d->canvasObserver->unsetCanvas();
}

QColor TextPropertiesDock::modalColorDialog(QColor oldColor)
{
    QColor c = QColorDialog::getColor(oldColor);
    return c.isValid()? c: oldColor;
}
