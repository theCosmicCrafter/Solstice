/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessGridEditDialog.h"
#include "QuickAccessStrokePreviews.h"

#include "QuickAccessLayoutEngine.h"

#include <KisResourceServerProvider.h>
#include <KoResource.h>
#include <brushengine/kis_paintop_preset.h>
#include <klocalizedstring.h>

#include <QApplication>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QIcon>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRubberBand>
#include <QScrollArea>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace
{
constexpr int CellSize = 42;
constexpr int CellSpacing = 4;
constexpr int CellStep = CellSize + CellSpacing;
constexpr int CanvasMargin = 4;
constexpr int VisibleRows = 10;
constexpr int ResizeHandleWidth = 8;
constexpr int SeparatorEdgeMargin = 5;
constexpr int MaximumHistory = 20;

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

bool overlaps(const QuickAccess::Item &first, const QuickAccess::Item &second)
{
    return !(first.right() <= second.column || second.right() <= first.column || first.bottom() <= second.row
             || second.bottom() <= first.row);
}

bool overlapsAny(const QuickAccess::Item &item, const QList<QuickAccess::Item> &placed)
{
    return std::any_of(placed.cbegin(), placed.cend(), [&item](const QuickAccess::Item &other) {
        return overlaps(item, other);
    });
}

QList<QuickAccess::Item> moveGroup(const QList<QuickAccess::Item> &items,
                                   const QSet<QString> &selection,
                                   int rowDelta,
                                   int columnDelta,
                                   int columns)
{
    QList<QuickAccess::Item> moving;
    QList<QuickAccess::Item> rest;
    for (const QuickAccess::Item &item : items) {
        (selection.contains(item.id) ? moving : rest).append(item);
    }
    if (moving.isEmpty())
        return items;

    int minimumRow = moving.first().row;
    int minimumColumn = moving.first().column;
    int maximumRight = moving.first().right();
    for (const QuickAccess::Item &item : std::as_const(moving)) {
        minimumRow = qMin(minimumRow, item.row);
        minimumColumn = qMin(minimumColumn, item.column);
        maximumRight = qMax(maximumRight, item.right());
    }
    rowDelta = qMax(rowDelta, -minimumRow);
    columnDelta = qBound(-minimumColumn, columnDelta, columns - maximumRight);
    for (QuickAccess::Item &item : moving) {
        item.row += rowDelta;
        item.column += columnDelta;
    }

    std::sort(rest.begin(), rest.end(), [columns](const QuickAccess::Item &a, const QuickAccess::Item &b) {
        const int aIndex = a.row * columns + a.column;
        const int bIndex = b.row * columns + b.column;
        return aIndex == bIndex ? a.id < b.id : aIndex < bIndex;
    });
    QList<QuickAccess::Item> placed = moving;
    for (QuickAccess::Item item : std::as_const(rest)) {
        int cursor = qMax(0, item.row * columns + item.column);
        while (item.column + item.columnSpan > columns || overlapsAny(item, placed)) {
            ++cursor;
            item.row = cursor / columns;
            item.column = cursor % columns;
        }
        placed.append(item);
    }
    return placed;
}

QString colorValue(const QJsonObject &object, const QString &snake, const QString &camel, const QString &fallback)
{
    QColor color(firstString(object, snake, camel));
    return color.isValid() ? color.name(QColor::HexArgb) : fallback;
}
} // namespace

class QuickAccessGridItemButton : public QPushButton
{
public:
    QuickAccessGridItemButton(const QuickAccess::Document &document,
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

    bool isStrokePreview() const
    {
        return m_item.type == QuickAccess::ItemType::Brush
            && m_item.payload.value(QStringLiteral("display")).toString() == QStringLiteral("stroke");
    }

    QSize strokePreviewSize() const
    {
        return QSize(m_item.columnSpan * CellSize + qMax(0, m_item.columnSpan - 1) * CellSpacing - 6, CellSize - 6);
    }

    /// Shows the stroke preview (an empty preview area until it is available).
    void updateStrokePreview()
    {
        if (!isStrokePreview() || !m_dialog->m_strokePreviews)
            return;
        const QString name = m_item.payload.value(QStringLiteral("brush_name")).toString();
        setIcon(QIcon(m_dialog->m_strokePreviews->pixmap(m_dialog->m_strokePreviews->ready(name) ? name : QString(),
                                                         strokePreviewSize(),
                                                         devicePixelRatioF())));
    }

    QString itemId() const
    {
        return m_item.id;
    }

    void updateSelectionStyle(const QuickAccess::Document &document)
    {
        configureAppearance(document);
    }

protected:
    void paintEvent(QPaintEvent *event) override
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

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton) {
            m_dialog->showItemContextMenu(m_tabIndex, m_item.id, globalMousePosition(event));
            return;
        }
        if (event->button() == Qt::LeftButton) {
            const bool toggle = event->modifiers().testFlag(Qt::ControlModifier);
            m_dialog->selectItem(m_tabIndex, m_item.id, toggle, !toggle);
            m_dragStart = globalMousePosition(event);
            m_dragMode = onResizeHandle(event->position().toPoint()) ? Resize : Move;
        }
        QPushButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragMode == None) {
            if (isResizable())
                setCursor(onResizeHandle(event->position().toPoint())
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

    void mouseReleaseEvent(QMouseEvent *event) override
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

private:
    enum DragMode {
        None,
        Move,
        Resize
    };

    bool isResizable() const
    {
        if (m_item.type == QuickAccess::ItemType::Label || m_item.type == QuickAccess::ItemType::Separator)
            return true;
        if (m_item.type != QuickAccess::ItemType::Action && m_item.type != QuickAccess::ItemType::DockerToggle
            && m_item.type != QuickAccess::ItemType::Script)
            return false;
        return iconPath().isEmpty();
    }

    bool resizeVertically() const
    {
        return m_item.type == QuickAccess::ItemType::Separator
            && m_item.payload.value(QStringLiteral("orientation")).toString() == QStringLiteral("vertical");
    }

    bool onResizeHandle(const QPoint &position) const
    {
        if (!isResizable())
            return false;
        return resizeVertically() ? position.y() >= height() - ResizeHandleWidth
                                  : position.x() >= width() - ResizeHandleWidth;
    }

    QString iconPath() const
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

    void configureAppearance(const QuickAccess::Document &document)
    {
        QColor background(QStringLiteral("#2f2f2f"));
        QColor foreground(Qt::white);
        QColor border(QStringLiteral("#555555"));
        int fontSize = 12;
        QString icon;

        QJsonObject styleObject = m_item.payload;
        if (m_item.type == QuickAccess::ItemType::Action) {
            styleObject = aliasFor(document,
                                   QStringLiteral("actions"),
                                   m_item.payload.value(QStringLiteral("action_id")).toString());
            background = QColor(
                colorValue(styleObject, QStringLiteral("background_color"), QString(), QStringLiteral("#3a263f")));
            foreground =
                QColor(colorValue(styleObject, QStringLiteral("font_color"), QString(), QStringLiteral("#ffffff")));
            border = QColor(QStringLiteral("#6b4a73"));
            icon = bundledIconPath(styleObject.value(QStringLiteral("icon_name")).toString());
        } else if (m_item.type == QuickAccess::ItemType::DockerToggle) {
            styleObject = aliasFor(document,
                                   QStringLiteral("dockers"),
                                   m_item.payload.value(QStringLiteral("docker_id")).toString());
            background = QColor(
                colorValue(styleObject, QStringLiteral("background_color"), QString(), QStringLiteral("#263a2f")));
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

    QuickAccess::Item m_item;
    int m_tabIndex;
    QuickAccessGridEditDialog *m_dialog;
    QPoint m_dragStart;
    DragMode m_dragMode{None};
};

class QuickAccessGridCanvas : public QWidget
{
public:
    QuickAccessGridCanvas(QuickAccess::Document *document,
                          QuickAccess::Grid *grid,
                          int tabIndex,
                          QuickAccessGridEditDialog *dialog,
                          QWidget *parent = nullptr)
        : QWidget(parent)
        , m_document(document)
        , m_grid(grid)
        , m_tabIndex(tabIndex)
        , m_dialog(dialog)
    {
        setAutoFillBackground(true);
        rebuild();
    }

    void rebuild()
    {
        const QObjectList directChildren = children();
        for (QObject *child : directChildren) {
            if (auto *button = dynamic_cast<QuickAccessGridItemButton *>(child))
                delete button;
        }
        int rows = VisibleRows;
        for (const QuickAccess::Item &item : std::as_const(m_grid->items))
            rows = qMax(rows, item.bottom() + 2);
        setMinimumSize(CanvasMargin * 2 + m_grid->columns * CellStep, CanvasMargin * 2 + rows * CellStep);
        resize(minimumSize());
        for (const QuickAccess::Item &item : std::as_const(m_grid->items)) {
            auto *button = new QuickAccessGridItemButton(*m_document, item, m_tabIndex, m_dialog, this);
            button->setGeometry(CanvasMargin + item.column * CellStep,
                                CanvasMargin + item.row * CellStep,
                                item.columnSpan * CellSize + qMax(0, item.columnSpan - 1) * CellSpacing,
                                item.rowSpan * CellSize + qMax(0, item.rowSpan - 1) * CellSpacing);
            button->show();
        }
        if (m_dropHighlight)
            m_dropHighlight->raise();
        update();
    }

    void showDropHighlight(const QuickAccess::Item &item)
    {
        if (!m_dropHighlight) {
            m_dropHighlight = new QFrame(this);
            m_dropHighlight->setAttribute(Qt::WA_TransparentForMouseEvents);
            m_dropHighlight->setStyleSheet(
                QStringLiteral("QFrame { border: 2px solid #4fc3f7; background-color: rgba(79, 195, 247, 60); "
                               "border-radius: 3px; }"));
        }
        m_dropHighlight->setGeometry(CanvasMargin + item.column * CellStep,
                                     CanvasMargin + item.row * CellStep,
                                     item.columnSpan * CellSize + qMax(0, item.columnSpan - 1) * CellSpacing,
                                     item.rowSpan * CellSize + qMax(0, item.rowSpan - 1) * CellSpacing);
        m_dropHighlight->raise();
        m_dropHighlight->show();
    }

    void clearDropHighlight()
    {
        if (m_dropHighlight)
            m_dropHighlight->hide();
    }

    void updateSelectionStyles()
    {
        const QObjectList directChildren = children();
        for (QObject *child : directChildren) {
            if (auto *button = dynamic_cast<QuickAccessGridItemButton *>(child))
                button->updateSelectionStyle(*m_document);
        }
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);
        QPainter painter(this);
        painter.setPen(QPen(QColor(QStringLiteral("#3f3f3f")), 1));
        painter.setBrush(Qt::NoBrush);
        const int rows = qMax(VisibleRows, (height() - CanvasMargin * 2) / CellStep);
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < m_grid->columns; ++column) {
                painter.drawRect(CanvasMargin + column * CellStep,
                                 CanvasMargin + row * CellStep,
                                 CellSize - 1,
                                 CellSize - 1);
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        m_origin = event->position().toPoint();
        if (!m_rubberBand)
            m_rubberBand = new QRubberBand(QRubberBand::Rectangle, this);
        m_rubberBand->setGeometry(QRect(m_origin, QSize()));
        m_rubberBand->show();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_rubberBand && !m_origin.isNull()) {
            m_rubberBand->setGeometry(QRect(m_origin, event->position().toPoint()).normalized());
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_rubberBand) {
            const QRect selection = m_rubberBand->geometry();
            m_rubberBand->hide();
            m_origin = QPoint();
            m_dialog->selectItemsInRect(m_tabIndex, selection, event->modifiers().testFlag(Qt::ControlModifier));
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QuickAccess::Document *m_document;
    QuickAccess::Grid *m_grid;
    int m_tabIndex;
    QuickAccessGridEditDialog *m_dialog;
    QRubberBand *m_rubberBand{nullptr};
    QFrame *m_dropHighlight{nullptr};
    QPoint m_origin;
};

QuickAccessGridEditDialog::QuickAccessGridEditDialog(const QuickAccess::Document &document, QWidget *parent)
    : QDialog(parent)
    , m_document(document)
    , m_strokePreviews(new QuickAccessStrokePreviews(this))
{
    QStringList strokeBrushes;
    for (const QuickAccess::Tab &tab : std::as_const(m_document.tabs)) {
        for (const QuickAccess::Grid &grid : tab.grids) {
            for (const QuickAccess::Item &item : grid.items) {
                if (item.type == QuickAccess::ItemType::Brush
                    && item.payload.value(QStringLiteral("display")).toString() == QStringLiteral("stroke"))
                    strokeBrushes << item.payload.value(QStringLiteral("brush_name")).toString();
            }
        }
    }
    m_strokePreviews->setNames(strokeBrushes);
    connect(m_strokePreviews, &QuickAccessStrokePreviews::changed, this, [this]() {
        for (QuickAccessGridCanvas *canvas : std::as_const(m_canvases)) {
            for (QObject *child : canvas->children()) {
                if (auto *button = dynamic_cast<QuickAccessGridItemButton *>(child))
                    button->updateStrokePreview();
            }
        }
    });
    setWindowTitle(i18nc("@title:window", "Edit Grid"));
    resize(850, 620);
    auto *layout = new QVBoxLayout(this);

    m_undoButton = new QToolButton(this);
    m_undoButton->setIcon(QIcon(QStringLiteral(":/quickaccess/system_icons/undo.png")));
    if (m_undoButton->icon().isNull())
        m_undoButton->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    m_undoButton->setToolTip(i18nc("@action", "Undo"));
    m_undoButton->setFixedSize(32, 32);
    layout->addWidget(m_undoButton, 0, Qt::AlignLeft);

    m_tabs = new QTabWidget(this);
    m_selection.resize(m_document.tabs.size());
    m_history.resize(m_document.tabs.size());
    for (int tabIndex = 0; tabIndex < m_document.tabs.size(); ++tabIndex) {
        QuickAccess::Grid *grid = gridForTab(tabIndex);
        if (!grid)
            continue;
        auto *scrollArea = new QScrollArea(m_tabs);
        scrollArea->setWidgetResizable(false);
        scrollArea->setFrameShape(QFrame::NoFrame);
        auto *canvas = new QuickAccessGridCanvas(&m_document, grid, tabIndex, this, scrollArea);
        scrollArea->setWidget(canvas);
        m_canvases.append(canvas);
        m_tabs->addTab(scrollArea, m_document.tabs.at(tabIndex).name);
        if (m_document.tabs.at(tabIndex).id == m_document.activeTabId)
            m_tabs->setCurrentIndex(tabIndex);
    }
    layout->addWidget(m_tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(m_undoButton, &QToolButton::clicked, this, &QuickAccessGridEditDialog::undoCurrentTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, &QuickAccessGridEditDialog::currentTabChanged);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateUndoButton();
}

QuickAccess::Document QuickAccessGridEditDialog::document() const
{
    return m_document;
}

QuickAccess::Grid *QuickAccessGridEditDialog::gridForTab(int tabIndex)
{
    if (tabIndex < 0 || tabIndex >= m_document.tabs.size() || m_document.tabs[tabIndex].grids.isEmpty())
        return nullptr;
    return &m_document.tabs[tabIndex].grids.first();
}

const QuickAccess::Item *QuickAccessGridEditDialog::itemForId(int tabIndex, const QString &itemId) const
{
    if (tabIndex < 0 || tabIndex >= m_document.tabs.size() || m_document.tabs.at(tabIndex).grids.isEmpty())
        return nullptr;
    const auto &items = m_document.tabs.at(tabIndex).grids.first().items;
    for (const QuickAccess::Item &item : items) {
        if (item.id == itemId)
            return &item;
    }
    return nullptr;
}

void QuickAccessGridEditDialog::currentTabChanged(int index)
{
    if (index >= 0 && index < m_document.tabs.size())
        m_document.activeTabId = m_document.tabs.at(index).id;
    updateUndoButton();
}

void QuickAccessGridEditDialog::selectItem(int tabIndex, const QString &itemId, bool toggle, bool preserveGroup)
{
    if (tabIndex < 0 || tabIndex >= m_selection.size())
        return;
    QSet<QString> &selection = m_selection[tabIndex];
    if (toggle) {
        if (selection.contains(itemId))
            selection.remove(itemId);
        else
            selection.insert(itemId);
    } else if (!preserveGroup || !selection.contains(itemId)) {
        selection = {itemId};
    }
    if (tabIndex < m_canvases.size())
        m_canvases.at(tabIndex)->updateSelectionStyles();
}

void QuickAccessGridEditDialog::selectItemsInRect(int tabIndex, const QRect &rect, bool additive)
{
    if (tabIndex < 0 || tabIndex >= m_selection.size() || tabIndex >= m_canvases.size())
        return;
    QSet<QString> selected = additive ? m_selection.at(tabIndex) : QSet<QString>();
    const QObjectList directChildren = m_canvases.at(tabIndex)->children();
    for (QObject *child : directChildren) {
        if (auto *button = dynamic_cast<QuickAccessGridItemButton *>(child)) {
            if (rect.intersects(button->geometry()))
                selected.insert(button->itemId());
        }
    }
    m_selection[tabIndex] = selected;
    if (tabIndex < m_canvases.size())
        m_canvases.at(tabIndex)->updateSelectionStyles();
}

bool QuickAccessGridEditDialog::isSelected(int tabIndex, const QString &itemId) const
{
    return tabIndex >= 0 && tabIndex < m_selection.size() && m_selection.at(tabIndex).contains(itemId);
}

void QuickAccessGridEditDialog::showMovePreview(int tabIndex, const QString &itemId, int rowDelta, int columnDelta)
{
    const QuickAccess::Item *source = itemForId(tabIndex, itemId);
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!source || !grid || tabIndex < 0 || tabIndex >= m_canvases.size())
        return;
    QuickAccess::Item preview = *source;
    preview.row = qMax(0, source->row + rowDelta);
    preview.column = qBound(0, source->column + columnDelta, grid->columns - source->columnSpan);
    m_canvases.at(tabIndex)->showDropHighlight(preview);
}

void QuickAccessGridEditDialog::showResizePreview(int tabIndex, const QString &itemId, int rowDelta, int columnDelta)
{
    const QuickAccess::Item *source = itemForId(tabIndex, itemId);
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!source || !grid || tabIndex < 0 || tabIndex >= m_canvases.size())
        return;
    if (!rowDelta && !columnDelta) {
        clearPreview(tabIndex);
        return;
    }
    QuickAccess::Item preview = *source;
    preview.rowSpan = qMax(1, source->rowSpan + rowDelta);
    preview.columnSpan = qBound(1, source->columnSpan + columnDelta, grid->columns - source->column);
    m_canvases.at(tabIndex)->showDropHighlight(preview);
}

void QuickAccessGridEditDialog::clearPreview(int tabIndex)
{
    if (tabIndex >= 0 && tabIndex < m_canvases.size())
        m_canvases.at(tabIndex)->clearDropHighlight();
}

void QuickAccessGridEditDialog::moveSelected(int tabIndex, int rowDelta, int columnDelta)
{
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!grid || m_selection.value(tabIndex).isEmpty() || (!rowDelta && !columnDelta))
        return;
    pushUndo(tabIndex);
    grid->items = moveGroup(grid->items, m_selection.at(tabIndex), rowDelta, columnDelta, grid->columns);
    refreshCanvas(tabIndex);
}

void QuickAccessGridEditDialog::resizeItem(int tabIndex, const QString &itemId, int rowDelta, int columnDelta)
{
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    const QuickAccess::Item *item = itemForId(tabIndex, itemId);
    if (!grid || !item || (!rowDelta && !columnDelta))
        return;
    pushUndo(tabIndex);
    const QuickAccess::LayoutResult result = QuickAccess::LayoutEngine(grid->columns)
                                                 .resizeItem(grid->items,
                                                             itemId,
                                                             qMax(1, item->rowSpan + rowDelta),
                                                             qMax(1, item->columnSpan + columnDelta));
    if (result.isValid()) {
        grid->items = result.items;
    } else {
        m_history[tabIndex].removeLast();
    }
    refreshCanvas(tabIndex);
    updateUndoButton();
}

void QuickAccessGridEditDialog::showItemContextMenu(int tabIndex, const QString &itemId, const QPoint &globalPosition)
{
    if (!isSelected(tabIndex, itemId))
        selectItem(tabIndex, itemId, false, false);
    QMenu menu(this);
    QAction *remove = menu.addAction(QIcon(QStringLiteral(":/quickaccess/system_icons/circle-xmark.png")),
                                     i18nc("@action", "Remove"));
    QMenu *copyMenu = menu.addMenu(i18nc("@title:menu", "Copy to Tab"));
    QMenu *moveMenu = menu.addMenu(i18nc("@title:menu", "Move to Tab"));
    for (int target = 0; target < m_document.tabs.size(); ++target) {
        if (target == tabIndex)
            continue;
        QAction *copy = copyMenu->addAction(m_document.tabs.at(target).name);
        connect(copy, &QAction::triggered, this, [this, tabIndex, target]() {
            copyOrMoveSelected(tabIndex, target, false);
        });
        QAction *move = moveMenu->addAction(m_document.tabs.at(target).name);
        connect(move, &QAction::triggered, this, [this, tabIndex, target]() {
            copyOrMoveSelected(tabIndex, target, true);
        });
    }
    copyMenu->setEnabled(!copyMenu->actions().isEmpty());
    moveMenu->setEnabled(!moveMenu->actions().isEmpty());
    QAction *chosen = menu.exec(globalPosition);
    if (chosen == remove)
        removeSelected(tabIndex);
}

void QuickAccessGridEditDialog::removeSelected(int tabIndex)
{
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!grid || m_selection.value(tabIndex).isEmpty())
        return;
    pushUndo(tabIndex);
    const QSet<QString> selected = m_selection.at(tabIndex);
    grid->items.erase(std::remove_if(grid->items.begin(),
                                     grid->items.end(),
                                     [&selected](const QuickAccess::Item &item) {
                                         return selected.contains(item.id);
                                     }),
                      grid->items.end());
    m_selection[tabIndex].clear();
    refreshCanvas(tabIndex);
}

void QuickAccessGridEditDialog::copyOrMoveSelected(int sourceTab, int targetTab, bool move)
{
    QuickAccess::Grid *source = gridForTab(sourceTab);
    QuickAccess::Grid *target = gridForTab(targetTab);
    if (!source || !target || m_selection.value(sourceTab).isEmpty())
        return;

    QList<QuickAccess::Item> copied;
    for (const QuickAccess::Item &item : std::as_const(source->items)) {
        if (m_selection.at(sourceTab).contains(item.id))
            copied.append(item);
    }
    if (copied.isEmpty())
        return;
    pushUndo(targetTab);
    if (move)
        pushUndo(sourceTab);

    int minimumRow = copied.first().row;
    int minimumColumn = copied.first().column;
    int targetRow = 0;
    for (const QuickAccess::Item &item : std::as_const(copied)) {
        minimumRow = qMin(minimumRow, item.row);
        minimumColumn = qMin(minimumColumn, item.column);
    }
    for (const QuickAccess::Item &item : std::as_const(target->items))
        targetRow = qMax(targetRow, item.bottom());

    QSet<QString> newSelection;
    for (QuickAccess::Item &item : copied) {
        item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        item.row = targetRow + item.row - minimumRow;
        item.column = item.column - minimumColumn;
        const QuickAccess::LayoutResult result =
            QuickAccess::LayoutEngine(target->columns).addItem(target->items, item);
        if (result.isValid()) {
            target->items = result.items;
            newSelection.insert(item.id);
        }
    }
    if (move) {
        const QSet<QString> oldSelection = m_selection.at(sourceTab);
        source->items.erase(std::remove_if(source->items.begin(),
                                           source->items.end(),
                                           [&oldSelection](const QuickAccess::Item &item) {
                                               return oldSelection.contains(item.id);
                                           }),
                            source->items.end());
        m_selection[sourceTab].clear();
        refreshCanvas(sourceTab);
    }
    m_selection[targetTab] = newSelection;
    refreshCanvas(targetTab);
    updateUndoButton();
}

void QuickAccessGridEditDialog::pushUndo(int tabIndex)
{
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!grid || tabIndex < 0 || tabIndex >= m_history.size())
        return;
    m_history[tabIndex].append(grid->items);
    while (m_history[tabIndex].size() > MaximumHistory)
        m_history[tabIndex].removeFirst();
    updateUndoButton();
}

void QuickAccessGridEditDialog::undoCurrentTab()
{
    const int tabIndex = m_tabs->currentIndex();
    QuickAccess::Grid *grid = gridForTab(tabIndex);
    if (!grid || tabIndex < 0 || tabIndex >= m_history.size() || m_history[tabIndex].isEmpty())
        return;
    grid->items = m_history[tabIndex].takeLast();
    m_selection[tabIndex].clear();
    refreshCanvas(tabIndex);
    updateUndoButton();
}

void QuickAccessGridEditDialog::refreshCanvas(int tabIndex)
{
    if (tabIndex >= 0 && tabIndex < m_canvases.size()) {
        QuickAccessGridCanvas *canvas = m_canvases.at(tabIndex);
        QTimer::singleShot(0, canvas, [canvas]() {
            canvas->rebuild();
        });
    }
}

void QuickAccessGridEditDialog::updateUndoButton()
{
    const int tabIndex = m_tabs ? m_tabs->currentIndex() : -1;
    m_undoButton->setEnabled(tabIndex >= 0 && tabIndex < m_history.size() && !m_history.at(tabIndex).isEmpty());
}
