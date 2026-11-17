/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2019 Wolthera van Hövell tot Westerflier <griffinvalley@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "KisResourceItemListView.h"

#include <QEvent>
#include <QScroller>
#include <QScrollBar>
#include <QHelpEvent>

#include <QHash>
#include <QPaintEvent>
#include <QPainter>

#include <algorithm>

#include "KisIconToolTip.h"


struct  Q_DECL_HIDDEN KisResourceItemListView::Private
{
    Private(KisResourceItemListView *_q) : tip(_q) {}

    ListViewMode viewMode = ListViewMode::IconGrid;
    bool strictSelectionMode {false};
    KisIconToolTip tip;

    QScroller* scroller {0};
    QString prev_scrollbar_style;

    QSize requestedItemSize = QSize(56, 56);

    // Solstice grouping: header rects in contents coordinates.
    GroupFunction groupOf;
    QVector<QPair<QRect, QString>> groupHeaders;

    // Solstice: scroll to the current item set by the program
    bool followCurrentItem{true};
};

KisResourceItemListView::KisResourceItemListView(QWidget *parent)
    : QListView(parent)
    , m_d(new Private(this))
{
    setSelectionMode(QAbstractItemView::SingleSelection);
    setContextMenuPolicy(Qt::DefaultContextMenu);   
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(true);

    // Default configuration
    setViewMode(QListView::IconMode);
    setGridSize(QSize(56, 56));
    setIconSize(QSize(56, 56));
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(true);

    m_d->scroller = KisKineticScroller::createPreconfiguredScroller(this);
    if (m_d->scroller) {
        connect(m_d->scroller, SIGNAL(stateChanged(QScroller::State)), this, SLOT(slotScrollerStateChange(QScroller::State)));
    }

    connect(this, SIGNAL(clicked(QModelIndex)), SIGNAL(currentResourceClicked(const QModelIndex &)));

    m_d->prev_scrollbar_style = horizontalScrollBar()->styleSheet();
}

KisResourceItemListView::~KisResourceItemListView()
{
}

void KisResourceItemListView::setListViewMode(ListViewMode viewMode)
{
    m_d->viewMode = viewMode;

    auto restoreScrollbar = [&, this] () {
        horizontalScrollBar()->setStyleSheet(m_d->prev_scrollbar_style);
        setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAsNeeded);
    };

    switch (viewMode) {
    case ListViewMode::IconGrid: {
        setViewMode(ViewMode::IconMode);
        setFlow(Flow::LeftToRight);
        setWrapping(true);
        restoreScrollbar();

        setItemSize(m_d->requestedItemSize);
        break;
    }
    case ListViewMode::IconStripHorizontal: {
        setViewMode(ViewMode::IconMode);
        setFlow(Flow::LeftToRight);
        setWrapping(false);

        // this is the only way to hide it and not have it occupy space
        horizontalScrollBar()->setStyleSheet("QScrollBar::horizontal {height: 0px;}");
        setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOff);

        setItemSize(m_d->requestedItemSize);
        break;
    }
    case ListViewMode::Detail: {
        setViewMode(ViewMode::ListMode);
        setFlow(Flow::TopToBottom);
        setWrapping(false);
        restoreScrollbar();
        setItemSize(m_d->requestedItemSize);
        // horizontalScrollBar()->setStyleSheet(m_d->prev_scrollbar_style);
        setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAsNeeded);
        setHorizontalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAsNeeded);
        break;
    }
    }
}

void KisResourceItemListView::setItemSize(QSize size)
{
    m_d->requestedItemSize = size;

    switch (m_d->viewMode) {
    case ListViewMode::IconGrid: {
        setGridSize(size);
        setIconSize(size);
        break;
    }
    case ListViewMode::IconStripHorizontal: {
        // you can not set the item size in strip mode
        // it is configured automatically based on size
        break;
    }
    case ListViewMode::Detail: {
        const int w = width() - horizontalScrollBar()->width();
        setGridSize(QSize(w, size.height()));
        setIconSize(QSize(size));
        break;
    }
    }
}

void KisResourceItemListView::setStrictSelectionMode(bool enable)
{
    m_d->strictSelectionMode = enable;
}

void KisResourceItemListView::setFixedToolTipThumbnailSize(const QSize &size)
{
    m_d->tip.setFixedToolTipThumbnailSize(size);
}

void KisResourceItemListView::setToolTipShouldRenderCheckers(bool value)
{
    m_d->tip.setToolTipShouldRenderCheckers(value);
}

void KisResourceItemListView::rowsAboutToBeRemoved(const QModelIndex &parent, int start, int end)
{
    // QAbstractItemView moves the current index if the row it points to is removed,
    // which we don't want for strict selections
    QModelIndex current = currentIndex();
    if (selectionMode() == SingleSelection
            && m_d->strictSelectionMode
            && current.isValid()
            && current.row() >= start
            && current.row() <= end) {

        selectionModel()->clear();
    }
    QListView::rowsAboutToBeRemoved(parent, start, end);
}

void KisResourceItemListView::selectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    // base class takes care of viewport updates
    QListView::selectionChanged(selected, deselected);

    if (selected.isEmpty()) {
        Q_EMIT currentResourceChanged(QModelIndex());
    }
    else {
        Q_EMIT currentResourceChanged(selected.indexes().first());
    }
}

QItemSelectionModel::SelectionFlags KisResourceItemListView::selectionCommand(const QModelIndex &index, const QEvent *event) const
{
    QItemSelectionModel::SelectionFlags cmd = QListView::selectionCommand(index, event);

    // avoid deselecting the current item by Ctrl-clicking in single selection mode
    if (selectionMode() == SingleSelection
            && m_d->strictSelectionMode
            && cmd.testFlag(QItemSelectionModel::Deselect)) {

        cmd = QItemSelectionModel::NoUpdate;
    }
    return cmd;
}

void KisResourceItemListView::contextMenuEvent(QContextMenuEvent *event)
{
    QListView::contextMenuEvent(event);
    Q_EMIT contextMenuRequested(event->globalPos());
}

bool KisResourceItemListView::viewportEvent(QEvent *event)
{
    if (!model()) return true;

    if (event->type() == QEvent::ToolTip) {
        QHelpEvent *he = static_cast<QHelpEvent *>(event);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QStyleOptionViewItem option = viewOptions();
#else
        QStyleOptionViewItem option;
        initViewItemOption(&option);
#endif
        QModelIndex index = model()->buddy(indexAt(he->pos()));
        if (index.isValid()) {
            option.rect = visualRect(index);
            m_d->tip.showTip(he->pos(), option, index);
            return true;
        }
        m_d->tip.hide();
    }

    return QListView::viewportEvent(event);
}

void KisResourceItemListView::setGrouping(const GroupFunction &groupOf)
{
    m_d->groupOf = groupOf;
    m_d->groupHeaders.clear();
    scheduleDelayedItemsLayout();
    viewport()->update();
}

void KisResourceItemListView::doItemsLayout()
{
    // The plain layout below sets a scroll range without the group headers,
    // which clips the scroll position; the grouped layout restores it
    const int scrollPosition = verticalScrollBar()->value();
    QListView::doItemsLayout();
    m_d->groupHeaders.clear();
    if (!m_d->groupOf || m_d->viewMode != ListViewMode::IconGrid || viewMode() != IconMode || !model()
        || !gridSize().isValid()) {
        return;
    }

    struct Group {
        QString sortKey;
        QString label;
        QVector<int> rows;
    };
    QVector<Group> groups;
    QHash<QString, int> groupIndex;
    const int rowCount = model()->rowCount(rootIndex());
    for (int row = 0; row < rowCount; ++row) {
        if (isRowHidden(row)) {
            continue;
        }
        const QPair<QString, QString> group = m_d->groupOf(model()->index(row, modelColumn(), rootIndex()));
        const QString id = group.first + QLatin1Char('\n') + group.second;
        auto it = groupIndex.find(id);
        if (it == groupIndex.end()) {
            it = groupIndex.insert(id, int(groups.size()));
            groups.append({group.first, group.second, {}});
        }
        groups[it.value()].rows.append(row);
    }
    if (groups.isEmpty()) {
        return;
    }
    // The ungrouped layout's first cell gives the item offset inside a cell;
    // the groups then stack under their headers with the same columns.
    const QPoint origin =
        rectForIndex(model()->index(groups.first().rows.first(), modelColumn(), rootIndex())).topLeft();
    std::stable_sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
        return QString::localeAwareCompare(a.sortKey, b.sortKey) < 0;
    });

    const QSize cell = gridSize();
    const int width = viewport()->width();
    const int columns = qMax(1, width / cell.width());
    const int headerHeight = fontMetrics().height() + 10;
    int y = 0;
    for (const Group &group : groups) {
        m_d->groupHeaders.append({QRect(0, y, width, headerHeight), group.label});
        y += headerHeight;
        for (int i = 0; i < group.rows.size(); ++i) {
            const QPoint position(origin.x() + (i % columns) * cell.width(),
                                  origin.y() + y + (i / columns) * cell.height());
            setPositionForIndex(position, model()->index(group.rows[i], modelColumn(), rootIndex()));
        }
        y += ((group.rows.size() + columns - 1) / columns) * cell.height() + 4;
    }
    updateGeometries();
    verticalScrollBar()->setValue(scrollPosition);
    viewport()->update();
}

void KisResourceItemListView::paintEvent(QPaintEvent *event)
{
    QListView::paintEvent(event);
    if (m_d->groupHeaders.isEmpty()) {
        return;
    }
    QPainter painter(viewport());
    QFont font = painter.font();
    font.setBold(true);
    painter.setFont(font);
    const QColor text = palette().color(QPalette::Text);
    QColor line = text;
    line.setAlphaF(0.25);
    for (const auto &header : m_d->groupHeaders) {
        const QRect rect = header.first.translated(-horizontalOffset(), -verticalOffset());
        if (!rect.intersects(event->rect())) {
            continue;
        }
        const QRect textRect = rect.adjusted(6, 0, -6, -3);
        painter.setPen(text);
        painter.drawText(textRect,
                         Qt::AlignLeft | Qt::AlignBottom,
                         painter.fontMetrics().elidedText(header.second, Qt::ElideRight, textRect.width()));
        painter.fillRect(QRect(rect.left() + 6, rect.bottom(), rect.width() - 12, 1), line);
    }
}

void KisResourceItemListView::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);

    switch (m_d->viewMode) {
    case ListViewMode::IconStripHorizontal: {
        const int height = event->size().height();
        setGridSize(QSize(height, height));
        setIconSize(QSize(height, height));
        break;
    }
    case ListViewMode::Detail: {
        setItemSize(m_d->requestedItemSize);
        break;
    }
    case ListViewMode::IconGrid: {
        break;
    }
    }
    if (m_d->followCurrentItem) {
        scrollTo(currentIndex(), QAbstractItemView::PositionAtCenter);
    }
}

void KisResourceItemListView::setFollowCurrentItem(bool follow)
{
    m_d->followCurrentItem = follow;
}

bool KisResourceItemListView::followCurrentItem() const
{
    return m_d->followCurrentItem;
}
