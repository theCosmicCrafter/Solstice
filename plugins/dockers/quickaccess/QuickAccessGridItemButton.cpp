/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessGridItemButton.h"
#include "QuickAccessGridEditDialog.h"
#include "QuickAccessStrokePreviews.h"
#include "QuickAccessGridEditMetrics.h"

#include <KisResourceServerProvider.h>
#include <KoResource.h>
#include <brushengine/kis_paintop_preset.h>
#include <klocalizedstring.h>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>

using namespace QuickAccessGridEditMetrics;

namespace
{
QPoint globalMousePosition(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

QJsonObject aliasFor(const QuickAccess::Document &document, const QString &category, const QString &id)
{
    return document.aliases.value(category).toObject().value(id).toObject();
}

QString firstString(const QJsonObject &object, const QString &first, const QString &second = QString())
{
    QString value = object.value(first).toString();
    if (value.isEmpty() && !second.isEmpty())
        value = object.value(second).toString();
    return value;
}

QString itemText(const QuickAccess::Document &document, const QuickAccess::Item &item)
{
    switch (item.type) {
    case QuickAccess::ItemType::Brush:
        return item.payload.value(QStringLiteral("brush_name")).toString();
    case QuickAccess::ItemType::Action: {
        const QString id = item.payload.value(QStringLiteral("action_id")).toString();
        const QString alias =
            aliasFor(document, QStringLiteral("actions"), id).value(QStringLiteral("custom_name")).toString();
        return alias.isEmpty() ? id : alias;
    }
    case QuickAccess::ItemType::DockerToggle: {
        const QString id = item.payload.value(QStringLiteral("docker_id")).toString();
        const QString alias =
            aliasFor(document, QStringLiteral("dockers"), id).value(QStringLiteral("custom_name")).toString();
        return alias.isEmpty() ? id : alias;
    }
    case QuickAccess::ItemType::Color:
        return QString();
    case QuickAccess::ItemType::Script:
        return item.payload.value(QStringLiteral("custom_name")).toString(i18n("Script"));
    case QuickAccess::ItemType::BrushSize:
    case QuickAccess::ItemType::BrushBlendMode:
    case QuickAccess::ItemType::Label:
        return item.payload.value(QStringLiteral("text")).toString();
    case QuickAccess::ItemType::Separator:
        return QString();
    }
    return QString();
}

QString bundledIconPath(const QString &iconName)
{
    const QString fileName = QFileInfo(iconName).fileName();
    if (fileName.isEmpty())
        return QString();
    const QString path = QStringLiteral(":/quickaccess/default_icons/%1").arg(fileName);
    if (QFileInfo::exists(path))
        return path;
    return QFileInfo(iconName).isAbsolute() && QFileInfo::exists(iconName) ? iconName : QString();
}

QString colorValue(const QJsonObject &object, const QString &snake, const QString &camel, const QString &fallback)
{
    QColor color(firstString(object, snake, camel));
    return color.isValid() ? color.name(QColor::HexArgb) : fallback;
}
} // namespace

QuickAccessGridItemButton::QuickAccessGridItemButton(const QuickAccess::Document &document,
                                                     const QuickAccess::Item &item,
                                                     int tabIndex,
                                                     QuickAccessGridEditDialog *dialog,
                                                     QWidget *parent)
    : QPushButton(itemText(document, item), parent)
    , m_item(item)
    , m_tabIndex(tabIndex)
    , m_dialog(dialog)
{
    setToolTip(itemText(document, item).isEmpty() ? QuickAccess::itemTypeToString(item.type)
                                                  : itemText(document, item));
    setCursor(Qt::SizeAllCursor);
    setMouseTracking(isResizable());
    configureAppearance(document);
}

bool QuickAccessGridItemButton::isStrokePreview() const
{
    return m_item.type == QuickAccess::ItemType::Brush
        && m_item.payload.value(QStringLiteral("display")).toString() == QStringLiteral("stroke");
}

QSize QuickAccessGridItemButton::strokePreviewSize() const
{
    return QSize(m_item.columnSpan * CellSize + qMax(0, m_item.columnSpan - 1) * CellSpacing - 6, CellSize - 6);
}

void QuickAccessGridItemButton::updateStrokePreview()
{
    if (!isStrokePreview() || !m_dialog->m_strokePreviews)
        return;
    const QString name = m_item.payload.value(QStringLiteral("brush_name")).toString();
    setIcon(QIcon(m_dialog->m_strokePreviews->pixmap(m_dialog->m_strokePreviews->ready(name) ? name : QString(),
                                                     strokePreviewSize(),
                                                     devicePixelRatioF())));
}

QString QuickAccessGridItemButton::itemId() const
{
    return m_item.id;
}

void QuickAccessGridItemButton::updateSelectionStyle(const QuickAccess::Document &document)
{
    configureAppearance(document);
}

void QuickAccessGridItemButton::paintEvent(QPaintEvent *event)
{
    QPushButton::paintEvent(event);
    if (m_item.type == QuickAccess::ItemType::Separator) {
        bool thicknessOk = false;
        const int configuredThickness =
            m_item.payload.value(QStringLiteral("thickness")).toVariant().toInt(&thicknessOk);
        const int thickness = thicknessOk ? qMax(1, configuredThickness) : 2;
        QColor color(m_item.payload.value(QStringLiteral("color")).toString(QStringLiteral("#5a5a5a")));
        if (!color.isValid())
            color = QColor(QStringLiteral("#5a5a5a"));
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        QRectF bar;
        if (resizeVertically()) {
            bar = QRectF((width() - thickness) / 2.0,
                         SeparatorEdgeMargin,
                         thickness,
                         qMax(0, height() - SeparatorEdgeMargin * 2));
        } else {
            bar = QRectF(SeparatorEdgeMargin,
                         (height() - thickness) / 2.0,
                         qMax(0, width() - SeparatorEdgeMargin * 2),
                         thickness);
        }
        const qreal radius = qMax(1.0, thickness / 2.0);
        painter.drawRoundedRect(bar, radius, radius);
    }
    if (!isResizable())
        return;
    QPainter painter(this);
    painter.setPen(QPen(QColor(255, 255, 255, 110), 1));
    if (resizeVertically()) {
        const int y = height() - ResizeHandleWidth / 2;
        painter.drawLine(6, y, width() - 6, y);
    } else {
        const int x = width() - ResizeHandleWidth / 2;
        painter.drawLine(x, 6, x, height() - 6);
    }
}

void QuickAccessGridItemButton::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        m_dialog->showItemContextMenu(m_tabIndex, m_item.id, globalMousePosition(event));
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const bool toggle = event->modifiers().testFlag(Qt::ControlModifier);
        m_dialog->selectItem(m_tabIndex, m_item.id, toggle, !toggle);
        m_dragStart = globalMousePosition(event);
        m_dragMode = onResizeHandle(event->pos()) ? Resize : Move;
    }
    QPushButton::mousePressEvent(event);
}

void QuickAccessGridItemButton::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragMode == None) {
        if (isResizable())
            setCursor(onResizeHandle(event->pos())
                          ? (resizeVertically() ? Qt::SizeVerCursor : Qt::SizeHorCursor)
                          : Qt::SizeAllCursor);
        QPushButton::mouseMoveEvent(event);
        return;
    }
    if (event->buttons().testFlag(Qt::LeftButton)) {
        const QPoint delta = globalMousePosition(event) - m_dragStart;
        const int columnDelta = qRound(delta.x() / double(CellStep));
        const int rowDelta = qRound(delta.y() / double(CellStep));
        if (m_dragMode == Resize) {
            m_dialog->showResizePreview(m_tabIndex,
                                        m_item.id,
                                        resizeVertically() ? rowDelta : 0,
                                        resizeVertically() ? 0 : columnDelta);
        } else if (rowDelta || columnDelta) {
            m_dialog->showMovePreview(m_tabIndex, m_item.id, rowDelta, columnDelta);
        } else {
            m_dialog->clearPreview(m_tabIndex);
        }
    }
    QPushButton::mouseMoveEvent(event);
}

void QuickAccessGridItemButton::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragMode != None) {
        m_dialog->clearPreview(m_tabIndex);
        const QPoint delta = globalMousePosition(event) - m_dragStart;
        const int columnDelta = qRound(delta.x() / double(CellStep));
        const int rowDelta = qRound(delta.y() / double(CellStep));
        if (m_dragMode == Resize) {
            m_dialog->resizeItem(m_tabIndex,
                                 m_item.id,
                                 resizeVertically() ? rowDelta : 0,
                                 resizeVertically() ? 0 : columnDelta);
        } else if (rowDelta || columnDelta) {
            m_dialog->moveSelected(m_tabIndex, rowDelta, columnDelta);
        }
        m_dragMode = None;
    }
    QPushButton::mouseReleaseEvent(event);
}

bool QuickAccessGridItemButton::isResizable() const
{
    if (m_item.type == QuickAccess::ItemType::Label || m_item.type == QuickAccess::ItemType::Separator)
        return true;
    if (m_item.type != QuickAccess::ItemType::Action && m_item.type != QuickAccess::ItemType::DockerToggle
        && m_item.type != QuickAccess::ItemType::Script)
        return false;
    return iconPath().isEmpty();
}

bool QuickAccessGridItemButton::resizeVertically() const
{
    return m_item.type == QuickAccess::ItemType::Separator
        && m_item.payload.value(QStringLiteral("orientation")).toString() == QStringLiteral("vertical");
}

bool QuickAccessGridItemButton::onResizeHandle(const QPoint &position) const
{
    if (!isResizable())
        return false;
    return resizeVertically() ? position.y() >= height() - ResizeHandleWidth
                              : position.x() >= width() - ResizeHandleWidth;
}

QString QuickAccessGridItemButton::iconPath() const
{
    QJsonObject alias;
    if (m_item.type == QuickAccess::ItemType::Action) {
        alias = aliasFor(m_dialog->m_document,
                         QStringLiteral("actions"),
                         m_item.payload.value(QStringLiteral("action_id")).toString());
    } else if (m_item.type == QuickAccess::ItemType::DockerToggle) {
        alias = aliasFor(m_dialog->m_document,
                         QStringLiteral("dockers"),
                         m_item.payload.value(QStringLiteral("docker_id")).toString());
    }
    return bundledIconPath(alias.value(QStringLiteral("icon_name")).toString());
}

void QuickAccessGridItemButton::configureAppearance(const QuickAccess::Document &document)
{
    QColor background(QStringLiteral("#2f2f2f"));
    QColor foreground(Qt::white);
    QColor border(QStringLiteral("#555555"));
    int fontSize = 12;
    QString icon;

    QJsonObject styleObject = m_item.payload;
    if (m_item.type == QuickAccess::ItemType::Action) {
        styleObject =
            aliasFor(document, QStringLiteral("actions"), m_item.payload.value(QStringLiteral("action_id")).toString());
        background =
            QColor(colorValue(styleObject, QStringLiteral("background_color"), QString(), QStringLiteral("#3a263f")));
        foreground =
            QColor(colorValue(styleObject, QStringLiteral("font_color"), QString(), QStringLiteral("#ffffff")));
        border = QColor(QStringLiteral("#6b4a73"));
        icon = bundledIconPath(styleObject.value(QStringLiteral("icon_name")).toString());
    } else if (m_item.type == QuickAccess::ItemType::DockerToggle) {
        styleObject =
            aliasFor(document, QStringLiteral("dockers"), m_item.payload.value(QStringLiteral("docker_id")).toString());
        background =
            QColor(colorValue(styleObject, QStringLiteral("background_color"), QString(), QStringLiteral("#263a2f")));
        foreground =
            QColor(colorValue(styleObject, QStringLiteral("font_color"), QString(), QStringLiteral("#ffffff")));
        border = QColor(QStringLiteral("#4a8b6b"));
        icon = bundledIconPath(styleObject.value(QStringLiteral("icon_name")).toString());
    } else if (m_item.type == QuickAccess::ItemType::Label) {
        background = QColor(colorValue(styleObject,
                                       QStringLiteral("background_color"),
                                       QStringLiteral("backgroundColor"),
                                       QStringLiteral("#303030")));
        foreground = QColor(colorValue(styleObject,
                                       QStringLiteral("font_color"),
                                       QStringLiteral("fontColor"),
                                       QStringLiteral("#ffffff")));
        border = QColor(QStringLiteral("#4fc3f7"));
    } else if (m_item.type == QuickAccess::ItemType::Color) {
        QColor value(m_item.payload.value(QStringLiteral("color")).toString());
        background = value.isValid() ? value : QColor(QStringLiteral("#303030"));
        border = QColor(QStringLiteral("#808080"));
    } else if (m_item.type == QuickAccess::ItemType::Script) {
        background = QColor(QStringLiteral("#2f2a1f"));
        border = QColor(QStringLiteral("#8b7a4a"));
        icon = bundledIconPath(m_item.payload.value(QStringLiteral("icon_name")).toString());
    } else if (m_item.type == QuickAccess::ItemType::BrushSize
               || m_item.type == QuickAccess::ItemType::BrushBlendMode) {
        background = QColor(colorValue(styleObject,
                                       QStringLiteral("background_color"),
                                       QStringLiteral("backgroundColor"),
                                       QStringLiteral("#444444")));
    }

    const QString configuredFontSize =
        firstString(styleObject, QStringLiteral("font_size"), QStringLiteral("fontSize"));
    if (!configuredFontSize.isEmpty())
        fontSize = qBound(6, configuredFontSize.toInt(), 96);

    if (isStrokePreview()) {
        // As in the palette: the stroke preview over the item's cells.
        setText(QString());
        setIconSize(strokePreviewSize());
        updateStrokePreview();
    } else if (m_item.type == QuickAccess::ItemType::Brush) {
        const QString name = m_item.payload.value(QStringLiteral("brush_name")).toString();
        const auto resources =
            KisResourceServerProvider::instance()->paintOpPresetServer()->resourceModel()->resourcesForName(name);
        if (!resources.isEmpty()) {
            setIcon(QPixmap::fromImage(resources.constFirst()->image()));
            setText(QString());
        }
    } else if (!icon.isEmpty()) {
        setIcon(QIcon(icon));
        setText(QString());
    }
    if (!isStrokePreview())
        setIconSize(QSize(CellSize - 6, CellSize - 6));

    const bool selected = m_dialog->isSelected(m_tabIndex, m_item.id);
    const QColor effectiveBorder = selected ? QColor(QStringLiteral("#4fc3f7")) : border;
    const int borderWidth = selected ? 2 : 1;
    if (m_item.type == QuickAccess::ItemType::Separator) {
        setStyleSheet(QStringLiteral("QPushButton { background: #303030; color: #777777; border: %1px solid %2; }")
                          .arg(borderWidth)
                          .arg(effectiveBorder.name()));
    } else {
        setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; font-size: %3px; border: %4px solid "
                                     "%5; border-radius: 3px; padding: 0px; }")
                          .arg(background.name(QColor::HexArgb), foreground.name(QColor::HexArgb))
                          .arg(fontSize)
                          .arg(borderWidth)
                          .arg(effectiveBorder.name()));
    }
}
