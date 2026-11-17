/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessDock.h"
#include "QuickAccessItemDragController.h"
#include "QuickAccessItemPresentation.h"
#include "QuickAccessStrokePreviews.h"
#include <KisResourceServerProvider.h>
#include <KisViewManager.h>
#include <KoResource.h>
#include <kconfiggroup.h>
#include <ksharedconfig.h>
#include <kis_action.h>
#include <kis_action_manager.h>
#include <kis_canvas2.h>
#include <klocalizedstring.h>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTabWidget>
#include <QVBoxLayout>

using namespace QuickAccessItemPresentation;
namespace
{
constexpr int DefaultIconSize = 42;
constexpr int SeparatorEdgeMargin = 5;
class QuickAccessSeparator : public QWidget
{
public:
    QuickAccessSeparator(bool vertical, int thickness, const QColor &color, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_vertical(vertical)
        , m_thickness(qMax(1, thickness))
        , m_color(color.isValid() ? color : QColor(QStringLiteral("#5a5a5a")))
    {
        setAttribute(Qt::WA_TranslucentBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_color);
        QRectF bar;
        if (m_vertical) {
            bar = QRectF((width() - m_thickness) / 2.0,
                         SeparatorEdgeMargin,
                         m_thickness,
                         qMax(0, height() - SeparatorEdgeMargin * 2));
        } else {
            bar = QRectF(SeparatorEdgeMargin,
                         (height() - m_thickness) / 2.0,
                         qMax(0, width() - SeparatorEdgeMargin * 2),
                         m_thickness);
        }
        const qreal radius = qMax(1.0, m_thickness / 2.0);
        painter.drawRoundedRect(bar, radius, radius);
    }

private:
    bool m_vertical;
    int m_thickness;
    QColor m_color;
};

class ElidingPushButton : public QPushButton
{
public:
    explicit ElidingPushButton(const QString &text, QWidget *parent = nullptr)
        : QPushButton(parent)
        , m_fullText(text)
    {
        setAccessibleName(text);
        setToolTip(text);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    void setFullText(const QString &text)
    {
        m_fullText = text;
        setAccessibleName(text);
        setToolTip(text);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionButton option;
        initStyleOption(&option);

        const int iconWidth = option.icon.isNull() ? 0 : option.iconSize.width() + 6;
        const int horizontalPadding = style()->pixelMetric(QStyle::PM_ButtonMargin, &option, this) * 2 + iconWidth;
        option.text = fontMetrics().elidedText(m_fullText, Qt::ElideRight, qMax(0, width() - horizontalPadding));
        painter.drawControl(QStyle::CE_PushButton, option);
    }

private:
    QString m_fullText;
};

QString stripMnemonic(QString text)
{
    const QString escapedAmpersand = QStringLiteral("\x1f");
    text.replace(QStringLiteral("&&"), escapedAmpersand);
    text.remove(QLatin1Char('&'));
    text.replace(escapedAmpersand, QStringLiteral("&"));
    return text;
}

} // namespace

void QuickAccessDock::updateStrokePreviews()
{
    for (StrokePreviewButton &entry : m_strokePreviewButtons) {
        if (entry.shown || !entry.button || !m_strokePreviews->ready(entry.name))
            continue;
        entry.button->setIcon(
            QIcon(m_strokePreviews->pixmap(entry.name, entry.size, entry.button->devicePixelRatioF())));
        entry.shown = true;
    }
}

QWidget *QuickAccessDock::createTabPage(const QuickAccess::Tab &tab)
{
    auto *container = new QWidget(m_tabs);
    auto *containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(4, 4, 4, 4);
    containerLayout->setSpacing(4);

    for (const QuickAccess::Grid &grid : tab.grids) {
        auto *gridWidget = new QWidget(container);
        auto *layout = new QGridLayout(gridWidget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        const KConfigGroup appearance = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
        const int cellSize = m_popupMode ? appearance.readEntry("PopupIconSize", DefaultIconSize)
                                         : appearance.readEntry("DockerIconSize", DefaultIconSize);
        int rowCount = 0;
        for (const QuickAccess::Item &item : grid.items) {
            layout->addWidget(createItemWidget(item), item.row, item.column, item.rowSpan, item.columnSpan);
            rowCount = qMax(rowCount, item.bottom());
        }
        for (int column = 0; column < grid.columns; ++column) {
            layout->setColumnMinimumWidth(column, cellSize);
            layout->setColumnStretch(column, 0);
        }
        for (int row = 0; row < rowCount; ++row)
            layout->setRowMinimumHeight(row, cellSize);
        gridWidget->setFixedWidth(grid.columns * cellSize + qMax(0, grid.columns - 1) * layout->horizontalSpacing());
        gridWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        containerLayout->addWidget(gridWidget, 0, Qt::AlignLeft);
    }
    if (m_popupMode) {
        containerLayout->setSizeConstraint(QLayout::SetFixedSize);
        return container;
    }

    containerLayout->addStretch();

    auto *scrollArea = new QScrollArea(m_tabs);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(container);
    return scrollArea;
}

QWidget *QuickAccessDock::createItemWidget(const QuickAccess::Item &item)
{
    if (item.type == QuickAccess::ItemType::Label) {
        auto *label = new QLabel(item.payload.value(QStringLiteral("text")).toString(), m_tabs);
        label->setAlignment(Qt::AlignCenter);
        const QColor background(compatibleString(item.payload,
                                                 QStringLiteral("background_color"),
                                                 QStringLiteral("backgroundColor"),
                                                 QStringLiteral("#263746")));
        const QColor foreground(compatibleString(item.payload,
                                                 QStringLiteral("font_color"),
                                                 QStringLiteral("fontColor"),
                                                 QStringLiteral("#4fc3f7")));
        const int fontSize =
            qBound(6, compatibleInt(item.payload, QStringLiteral("font_size"), QStringLiteral("fontSize"), 18), 96);
        label->setStyleSheet(QStringLiteral("background: %1; color: %2; font-size: %3px; padding: 2px 6px;")
                                 .arg(background.name(), foreground.name())
                                 .arg(fontSize));
        if (!m_popupMode)
            attachItemMenu(label, item);
        m_itemDrag->attach(label, item);
        return label;
    }
    if (item.type == QuickAccess::ItemType::Separator) {
        const bool vertical =
            item.payload.value(QStringLiteral("orientation")).toString() == QStringLiteral("vertical");
        bool thicknessOk = false;
        const int configuredThickness = item.payload.value(QStringLiteral("thickness")).toVariant().toInt(&thicknessOk);
        const int thickness = thicknessOk ? qMax(1, configuredThickness) : 2;
        const QColor color(item.payload.value(QStringLiteral("color")).toString(QStringLiteral("#5a5a5a")));
        auto *separator = new QuickAccessSeparator(vertical, thickness, color, m_tabs);
        if (!m_popupMode)
            attachItemMenu(separator, item);
        m_itemDrag->attach(separator, item);
        return separator;
    }

    auto *button = new ElidingPushButton(itemText(item), m_tabs);
    const KConfigGroup appearance = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
    const int iconSize = m_popupMode ? appearance.readEntry("PopupIconSize", DefaultIconSize)
                                     : appearance.readEntry("DockerIconSize", DefaultIconSize);
    button->setMinimumSize(iconSize, iconSize);

    if (showsStrokePreview(item)) {
        // The stroke preview fills the item's cells (an empty preview area
        // until it is available); the name is only in the tooltip.
        const QString name = item.payload.value(QStringLiteral("brush_name")).toString();
        const int spacing = 2;
        const QSize cells(item.columnSpan * iconSize + (item.columnSpan - 1) * spacing,
                          item.rowSpan * iconSize + (item.rowSpan - 1) * spacing);
        const QSize previewSize = cells - QSize(8, 8);
        button->setFullText(QString());
        button->setToolTip(name);
        button->setIcon(QIcon(m_strokePreviews->pixmap(QString(), previewSize, devicePixelRatioF())));
        button->setIconSize(previewSize);
        m_strokePreviewButtons.append({name, button, previewSize, false});
    } else if (item.type == QuickAccess::ItemType::Brush && m_canvas) {
        const QString name = item.payload.value(QStringLiteral("brush_name")).toString();
        const auto resources =
            KisResourceServerProvider::instance()->paintOpPresetServer()->resourceModel()->resourcesForName(name);
        if (!resources.isEmpty()) {
            button->setIcon(QPixmap::fromImage(resources.constFirst()->image()));
            button->setIconSize(QSize(iconSize - 4, iconSize - 4));
            button->setFullText(QString());
            button->setToolTip(name);
        }
    } else if (item.type == QuickAccess::ItemType::Color) {
        const QColor color(item.payload.value(QStringLiteral("color")).toString());
        if (color.isValid()) {
            button->setFullText(QString());
            button->setStyleSheet(
                QStringLiteral("background-color: %1; border: 2px solid #808080;").arg(color.name(QColor::HexArgb)));
            button->setToolTip(color.name(QColor::HexArgb));
        }
    }

    QJsonObject configuredAlias;
    QString configuredIcon;
    if (item.type == QuickAccess::ItemType::Action) {
        const QString id = item.payload.value(QStringLiteral("action_id")).toString();
        configuredAlias = aliasFor(m_document, QStringLiteral("actions"), id);
        configuredIcon = configuredAlias.value(QStringLiteral("icon_name")).toString();
    } else if (item.type == QuickAccess::ItemType::DockerToggle) {
        const QString id = item.payload.value(QStringLiteral("docker_id")).toString();
        configuredAlias = aliasFor(m_document, QStringLiteral("dockers"), id);
        configuredIcon = configuredAlias.value(QStringLiteral("icon_name")).toString();
    } else if (item.type == QuickAccess::ItemType::Script) {
        configuredIcon = item.payload.value(QStringLiteral("icon_name")).toString();
    }
    const QString configuredIconPath = resolveDefaultIcon(configuredIcon);
    if (!configuredIconPath.isEmpty()) {
        const QString toolTip = itemText(item);
        button->setIcon(QIcon(configuredIconPath));
        button->setIconSize(QSize(iconSize - 4, iconSize - 4));
        button->setFullText(QString());
        button->setToolTip(toolTip);
    }

    if (item.type == QuickAccess::ItemType::Action || item.type == QuickAccess::ItemType::DockerToggle) {
        const bool docker = item.type == QuickAccess::ItemType::DockerToggle;
        QColor background(
            compatibleString(configuredAlias, QStringLiteral("background_color"), QStringLiteral("backgroundColor")));
        QColor foreground(compatibleString(configuredAlias, QStringLiteral("font_color"), QStringLiteral("fontColor")));
        if (!background.isValid())
            background = QColor(docker ? QStringLiteral("#263a2f") : QStringLiteral("#3a263f"));
        if (!foreground.isValid())
            foreground = Qt::white;
        int fontSize = compatibleInt(configuredAlias, QStringLiteral("font_size"), QStringLiteral("fontSize"), 18);
        fontSize = qBound(6, fontSize, 96);
        button->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; color: %2; font-size: %3px; border: 1px solid %4; "
                           "border-radius: 4px; padding: %5; }")
                .arg(background.name(),
                     foreground.name(),
                     QString::number(fontSize),
                     docker ? QStringLiteral("#1c2212") : QStringLiteral("#6b4a73"),
                     configuredIconPath.isEmpty() ? QStringLiteral("2px 6px") : QStringLiteral("0px")));
    }

    if (item.type == QuickAccess::ItemType::BrushSize || item.type == QuickAccess::ItemType::BrushBlendMode) {
        const bool size = item.type == QuickAccess::ItemType::BrushSize;
        QColor background(compatibleString(item.payload,
                                           QStringLiteral("background_color"),
                                           QStringLiteral("backgroundColor"),
                                           size ? QStringLiteral("#3a263f") : QStringLiteral("#263a3a")));
        QColor foreground(compatibleString(item.payload,
                                           QStringLiteral("font_color"),
                                           QStringLiteral("fontColor"),
                                           QStringLiteral("#ffffff")));
        const int fontSize =
            qBound(6, compatibleInt(item.payload, QStringLiteral("font_size"), QStringLiteral("fontSize"), 18), 96);
        button->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; color: %2; font-size: %3px; border: 1px solid %4; "
                           "border-radius: 4px; padding: 2px 6px; }")
                .arg(background.name(), foreground.name())
                .arg(fontSize)
                .arg(size ? QStringLiteral("#6b4a73") : QStringLiteral("#4a8b8b")));
    }

    connect(button, &QPushButton::clicked, this, [this, item]() {
        activateItem(item);
    });
    if (!m_popupMode)
        attachItemMenu(button, item);
    m_itemDrag->attach(button, item);
    return button;
}

int QuickAccessDock::cellSize() const
{
    const KConfigGroup appearance = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccess"));
    return m_popupMode ? appearance.readEntry("PopupIconSize", DefaultIconSize)
                       : appearance.readEntry("DockerIconSize", DefaultIconSize);
}

QString QuickAccessDock::itemText(const QuickAccess::Item &item) const
{
    switch (item.type) {
    case QuickAccess::ItemType::Brush:
        return item.payload.value(QStringLiteral("brush_name")).toString();
    case QuickAccess::ItemType::Action: {
        const QString id = item.payload.value(QStringLiteral("action_id")).toString();
        const QJsonObject alias = aliasFor(m_document, QStringLiteral("actions"), id);
        const QString customName = compatibleString(alias, QStringLiteral("custom_name"), QStringLiteral("customName"));
        if (!customName.isEmpty())
            return customName;
        if (m_canvas && m_canvas->viewManager()) {
            if (QAction *action = m_canvas->viewManager()->actionManager()->actionByName(id))
                return stripMnemonic(action->text());
        }
        return id;
    }
    case QuickAccess::ItemType::DockerToggle: {
        const QString id = item.payload.value(QStringLiteral("docker_id")).toString();
        const QJsonObject alias = aliasFor(m_document, QStringLiteral("dockers"), id);
        return compatibleString(alias, QStringLiteral("custom_name"), QStringLiteral("customName"), id);
    }
    case QuickAccess::ItemType::BrushSize:
    case QuickAccess::ItemType::BrushBlendMode:
    case QuickAccess::ItemType::Label:
        return item.payload.value(QStringLiteral("text")).toString();
    case QuickAccess::ItemType::Color:
        return item.payload.value(QStringLiteral("color")).toString();
    case QuickAccess::ItemType::Script:
        return compatibleString(item.payload,
                                QStringLiteral("custom_name"),
                                QStringLiteral("customName"),
                                i18n("Script"));
    case QuickAccess::ItemType::Separator:
        return QString();
    }
    return QString();
}
