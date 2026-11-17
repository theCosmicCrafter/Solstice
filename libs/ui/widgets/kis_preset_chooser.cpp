/*
 *  SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *  SPDX-FileCopyrightText: 2009 Sven Langkamp <sven.langkamp@gmail.com>
 *  SPDX-FileCopyrightText: 2011 Silvio Heinrich <plassy@web.de>
 *  SPDX-FileCopyrightText: 2011 Srikanth Tiyyagura <srikanth.tulasiram@gmail.com>
 *  SPDX-FileCopyrightText: 2011 José Luis Vergara <pentalis@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_preset_chooser.h"

#include "KisBrushStrokePreviewCache.h"
#include "KisPresetDockerFilters.h"
#include <KisResourceModel.h>
#include <QAbstractItemDelegate>
#include <QApplication>
#include <QEvent>
#include <QHelpEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QStyleOptionViewItem>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <kis_config.h>
#include <klocalizedstring.h>
#include <KisKineticScroller.h>

#include <KoIcon.h>
#include <KisResourceItemChooser.h>
#include <KisResourceItemChooserSync.h>
#include <KisResourceItemListView.h>
#include <KisResourceLocator.h>
#include <KisResourceTypes.h>

#include <brushengine/kis_paintop_settings.h>
#include <brushengine/kis_paintop_preset.h>
#include "kis_config_notifier.h"
#include <kis_icon.h>
#include <KisResourceModelProvider.h>
#include <KisTagFilterResourceProxyModel.h>
#include <KisResourceThumbnailCache.h>
#include <KisResourceMetaDataModel.h>


/// The resource item delegate for rendering the resource preview
class KisPresetDelegate : public QAbstractItemDelegate
{
public:
    KisPresetDelegate(QObject * parent = 0)
        : QAbstractItemDelegate(parent)
        , m_showText(false)
        , m_useDirtyPresets(false) {}

    ~KisPresetDelegate() override {}

    /// reimplemented
    void paint(QPainter *, const QStyleOptionViewItem &, const QModelIndex &) const override;

    /// reimplemented
    QSize sizeHint(const QStyleOptionViewItem & option, const QModelIndex &) const override {
        return option.decorationSize;
    }

    void setShowText(bool showText) {
        m_showText = showText;
    }

    void setUseDirtyPresets(bool value) {
        m_useDirtyPresets = value;
    }
    void setStrokePreview(bool enabled)
    {
        m_strokePreview = enabled;
    }

private:
    bool m_showText;
    bool m_useDirtyPresets;
    bool m_strokePreview = false;
};

void KisPresetDelegate::paint(QPainter * painter, const QStyleOptionViewItem & option, const QModelIndex & index) const
{
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (!(option.state & QStyle::State_Enabled)) {
        painter->setOpacity(0.2);
    }

    if (!index.isValid()) {
        painter->restore();
        return;
    }

    bool dirty = index.data(Qt::UserRole + KisAbstractResourceModel::Dirty).toBool();

    if (m_strokePreview) {
        const QRect cell = option.rect.adjusted(2, 2, -2, -2);
        const QRect imageRect(cell.topLeft(), QSize(cell.width(), cell.width() / 3));
        painter->fillRect(imageRect, QColor("#303030"));
        const auto request = KisBrushStrokePreviewCache::Request::fromIndex(index);
        const auto image = KisBrushStrokePreviewCache::instance()->preview(request);
        if (!image.isNull())
            painter->drawImage(imageRect, image);
        QString name = index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString().replace('_', ' ');
        if (m_useDirtyPresets && dirty) {
            name += '*';
            KisIconUtils::loadIcon("dirty-preset")
                .paint(painter, QRect(imageRect.topLeft() + QPoint(3, 3), QSize(16, 16)));
        }
        if (index.data(Qt::UserRole + KisAbstractResourceModel::BrokenStatus).toBool())
            KisIconUtils::loadIcon("broken-preset")
                .paint(painter, QRect(imageRect.bottomRight() - QPoint(20, 20), QSize(20, 20)));
        painter->setPen(option.palette.color(QPalette::Text));
        painter->drawText(QRect(cell.left(), imageRect.bottom() + 3, cell.width(), option.fontMetrics.height() + 2),
                          Qt::AlignCenter,
                          option.fontMetrics.elidedText(name, Qt::ElideMiddle, cell.width()));
        if (option.state & (QStyle::State_Selected | QStyle::State_MouseOver)) {
            painter->setPen(QPen(option.palette.highlight(), option.state & QStyle::State_Selected ? 3 : 1));
            painter->drawRect(cell);
        }
        painter->restore();
        return;
    }

    QImage preview = KisResourceThumbnailCache::instance()->getImage(index);

    if (preview.isNull()) {
        preview = QImage(512, 512, QImage::Format_RGB32);
        preview.fill(Qt::red);
    }

    qreal devicePixelRatioF = painter->device()->devicePixelRatioF();

    QRect paintRect = option.rect.adjusted(1, 1, -1, -1);
    if (!m_showText) {
        QImage previewHighDpi =
            KisResourceThumbnailCache::instance()->getImage(index,
                                                             paintRect.size() * devicePixelRatioF,
                                                             Qt::IgnoreAspectRatio,
                                                             Qt::SmoothTransformation);
        previewHighDpi.setDevicePixelRatio(devicePixelRatioF);
        painter->drawImage(paintRect.x(), paintRect.y(), previewHighDpi);
    }
    else {
        QSize pixSize(paintRect.height(), paintRect.height());
        QImage previewHighDpi = KisResourceThumbnailCache::instance()->getImage(index,
                                                                                 pixSize * devicePixelRatioF,
                                                                                 Qt::KeepAspectRatio,
                                                                                 Qt::SmoothTransformation);
        previewHighDpi.setDevicePixelRatio(devicePixelRatioF);
        painter->drawImage(paintRect.x(), paintRect.y(), previewHighDpi);

        // Put an asterisk after the preset if it is dirty. This will help in case the pixmap icon is too small

        QString dirtyPresetIndicator = QString("");
        if (m_useDirtyPresets && dirty) {
            dirtyPresetIndicator = QString("*");
        }

//        qreal brushSize = metaData["paintopSize"].toReal();
//        qDebug() << "brushsize" << brushSize;
//        QString brushSizeText;
//        // Disable displayed decimal precision beyond a certain brush size
//        if (brushSize < 100) {
//            brushSizeText = QString::number(brushSize, 'g', 3);
//        }
//        else {
//            brushSizeText = QString::number(brushSize, 'f', 0);
//        }

//        painter->drawText(pixSize.width() + 10, option.rect.y() + option.rect.height() - 10, brushSizeText); // brush size

        QString presetDisplayName = index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString().replace("_", " "); // don't need underscores that might be part of the file name
        painter->drawText(pixSize.width() + 10, option.rect.y() + option.rect.height() - 10, presetDisplayName.append(dirtyPresetIndicator));

    }

    if (m_useDirtyPresets && dirty) {
        const QIcon icon = KisIconUtils::loadIcon("dirty-preset");
        QPixmap pixmap = icon.pixmap(QSize(16,16));
        painter->drawPixmap(paintRect.x() + 3, paintRect.y() + 3, pixmap);
    }

    if (index.data(Qt::UserRole + KisAbstractResourceModel::BrokenStatus).toBool()) {
        const QIcon icon = KisIconUtils::loadIcon("broken-preset");
        icon.paint(painter, QRect(paintRect.x() + paintRect.height() - 25, paintRect.y() + paintRect.height() - 25, 25, 25));
    }

    if (option.state & QStyle::State_Selected) {
        painter->setCompositionMode(QPainter::CompositionMode_HardLight);
        painter->setOpacity(1.0);
        painter->fillRect(option.rect, option.palette.highlight());

        // highlight is not strong enough to pick out preset. draw border around it.
        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter->setPen(QPen(option.palette.highlight(), 4, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        QRect selectedBorder = option.rect.adjusted(2 , 2, -2, -2); // constrict the rectangle so it doesn't bleed into other presets
        painter->drawRect(selectedBorder);
    }

    painter->restore();

}

KisPresetChooser::KisPresetChooser(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("KisPresetChooser");

    QVBoxLayout * layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_chooser = new KisResourceItemChooser(ResourceType::PaintOpPresets, false, this);
    m_chooser->setRowHeight(50);
    m_delegate = new KisPresetDelegate(this);
    m_chooser->setItemDelegate(m_delegate);
    m_chooser->setSynced(true);
    m_chooser->showImportExportBtns(false);
    layout->addWidget(m_chooser);

    connect(m_chooser, SIGNAL(resourceSelected(KoResourceSP )),
            this, SLOT(slotResourceWasSelected(KoResourceSP )));

    connect(m_chooser, SIGNAL(resourceSelected(KoResourceSP )),
            this, SIGNAL(resourceSelected(KoResourceSP )));
    connect(m_chooser, SIGNAL(resourceClicked(KoResourceSP )),
            this, SIGNAL(resourceClicked(KoResourceSP )));

    connect(m_chooser, &KisResourceItemChooser::listViewModeChanged, this, &KisPresetChooser::showHideBrushNames);

    m_mode = ViewMode::THUMBNAIL;

    connect(KisConfigNotifier::instance(), SIGNAL(configChanged()),
            SLOT(notifyConfigChanged()));


    notifyConfigChanged();
    m_previewRequestsTimer.setSingleShot(true);
    m_previewRequestsTimer.setInterval(30);
    connect(&m_previewRequestsTimer, &QTimer::timeout, this, &KisPresetChooser::updatePreviewRequests);
    auto *view = m_chooser->itemView();
    view->viewport()->installEventFilter(this);
    connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        m_previewRequestsTimer.start();
    });
    connect(view->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        m_previewRequestsTimer.start();
    });
    connect(view->model(), &QAbstractItemModel::modelReset, this, [this]() {
        m_previewRequestsTimer.start();
    });
    connect(view->model(), &QAbstractItemModel::dataChanged, this, [this]() {
        m_previewRequestsTimer.start();
    });
    connect(view->model(), &QAbstractItemModel::rowsInserted, this, [this]() {
        m_previewRequestsTimer.start();
    });
    connect(view->model(), &QAbstractItemModel::rowsRemoved, this, [this]() {
        m_previewRequestsTimer.start();
    });
    connect(view->model(), &QAbstractItemModel::layoutChanged, this, [this]() {
        m_previewRequestsTimer.start();
    });
}

KisPresetChooser::~KisPresetChooser()
{
    if (m_strokePreview)
        KisBrushStrokePreviewCache::instance()->removeConsumer(this);
}

void KisPresetChooser::setStrokePreviewMode(bool enabled)
{
    if (m_strokePreview == enabled)
        return;
    m_strokePreview = enabled;
    m_delegate->setStrokePreview(enabled);
    m_chooser->setSynced(!enabled);
    m_chooser->setResponsiveness(!enabled && !m_dockerFilters);
    m_chooser->setBottomBarLayout(enabled || m_dockerFilters);
    if (enabled) {
        m_previewWidth = qBound(90, KisConfig(true).readEntry<int>("Solstice/BrushStrokePreviewWidth", 180), 240);
        connect(KisBrushStrokePreviewCache::instance(),
                &KisBrushStrokePreviewCache::previewReady,
                m_chooser->itemView()->viewport(),
                QOverload<>::of(&QWidget::update),
                Qt::UniqueConnection);
    } else {
        KisBrushStrokePreviewCache::instance()->removeConsumer(this);
    }
    updateViewSettings();
}

void KisPresetChooser::enableDockerFilters()
{
    if (m_dockerFilters)
        return;
    m_dockerFilters = true;
    m_chooser->setResponsiveness(false);
    m_chooser->setBottomBarWidget(new KisPresetDockerFilters(m_chooser->tagFilterModel(), m_chooser->itemView()));
    updateViewSettings();
}

bool KisPresetChooser::eventFilter(QObject *object, QEvent *event)
{
    if (m_strokePreview && object == m_chooser->itemView()->viewport()) {
        if (event->type() == QEvent::ToolTip) {
            const auto *help = static_cast<QHelpEvent *>(event);
            const auto index = m_chooser->itemView()->indexAt(help->pos());
            const QString name = index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
            if (name.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), name, m_chooser->itemView());
            return true; // The shared resource tooltip contains the stored icon.
        }
        if (event->type() == QEvent::Hide)
            KisBrushStrokePreviewCache::instance()->removeConsumer(this);
        if (event->type() == QEvent::Show || event->type() == QEvent::Resize)
            m_previewRequestsTimer.start();
        if (event->type() == QEvent::Wheel) {
            auto *wheel = static_cast<QWheelEvent *>(event);
            if (wheel->modifiers() & Qt::ControlModifier) {
                setIconSize(iconSize() + wheel->angleDelta().y() / 120 * 10);
                saveIconSize();
                return true;
            }
        }
    }
    return QWidget::eventFilter(object, event);
}

void KisPresetChooser::updatePreviewRequests()
{
    if (!m_strokePreview)
        return;
    auto *view = m_chooser->itemView();
    if (!view->isVisible()) {
        KisBrushStrokePreviewCache::instance()->removeConsumer(this);
        return;
    }
    QList<KisBrushStrokePreviewCache::Request> requests;
    const QRect viewport = view->viewport()->rect();
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        const auto index = view->model()->index(row, 0);
        if (view->visualRect(index).intersects(viewport))
            requests << KisBrushStrokePreviewCache::Request::fromIndex(index);
    }
    KisBrushStrokePreviewCache::instance()->setRequests(this, requests);
}

void KisPresetChooser::setViewMode(KisPresetChooser::ViewMode mode)
{
    m_mode = mode;
    updateViewSettings();
}

void KisPresetChooser::setViewModeToThumbnail()
{
    setViewMode(KisPresetChooser::ViewMode::THUMBNAIL);
}

void KisPresetChooser::setViewModeToDetail()
{
    setViewMode(KisPresetChooser::ViewMode::DETAIL);
}

void KisPresetChooser::notifyConfigChanged()
{
    KisConfig cfg(true);
    m_delegate->setUseDirtyPresets(cfg.useDirtyPresets());
    if (!m_strokePreview)
        setIconSize(cfg.presetIconSize());
}

void KisPresetChooser::slotResourceWasSelected(KoResourceSP resource)
{
    m_currentPresetConnections.clear();
    if (!resource) return;

    KisPaintOpPresetSP preset = resource.dynamicCast<KisPaintOpPreset>();
    KIS_SAFE_ASSERT_RECOVER_RETURN(preset);

    m_currentPresetConnections.addUniqueConnection(
        preset->updateProxy(), SIGNAL(sigSettingsChanged()),
        this, SLOT(slotCurrentPresetChanged()));
}

void KisPresetChooser::slotCurrentPresetChanged()
{
    // Solstice: this only repaints the current item (its dirty mark and
    // thumbnail). A hidden view paints all its items when it is shown, so
    // it need not search its rows for the item on every setting change.
    if (!m_chooser->itemView()->isVisible())
        return;

    KoResourceSP currentResource = m_chooser->currentResource();
    if (!currentResource) return;

    QModelIndex index = m_chooser->tagFilterModel()->indexForResource(currentResource);
    Q_EMIT m_chooser->tagFilterModel()->dataChanged(index,
                                               index,
                                               {Qt::UserRole + KisAbstractResourceModel::Thumbnail});
}

void KisPresetChooser::updateViewSettings()
{
    if (m_strokePreview) {
        m_chooser->setListViewMode(ListViewMode::IconGrid);
        m_chooser->setColumnWidth(m_previewWidth);
        m_chooser->setRowHeight(m_previewWidth / 3 + fontMetrics().height() + 10);
        m_chooser->itemView()->viewport()->update();
        m_previewRequestsTimer.start();
        return;
    }
    switch (m_mode) {
    case ViewMode::THUMBNAIL: {
        m_chooser->setListViewMode(ListViewMode::IconGrid);
        m_delegate->setShowText(false);
        break;
    }
    case ViewMode::DETAIL: {
        m_chooser->setListViewMode(ListViewMode::Detail);
        m_delegate->setShowText(true);
        break;
    }
    }
}

void KisPresetChooser::setCurrentResource(KoResourceSP resource)
{
    m_chooser->setCurrentResource(resource);
}

KoResourceSP KisPresetChooser::currentResource() const
{
    return m_chooser->currentResource();
}

void KisPresetChooser::showTaggingBar(bool show)
{
    m_chooser->showTaggingBar(show);
}

KisResourceItemChooser *KisPresetChooser::itemChooser()
{
    return m_chooser;
}

void KisPresetChooser::setPresetFilter(const QString& paintOpId)
{
    QMap<QString, QVariant> metaDataFilter;
    if (!paintOpId.isEmpty()) { // empty means "all"
        metaDataFilter["paintopid"] = paintOpId;
    }
    m_chooser->tagFilterModel()->setMetaDataFilter(metaDataFilter);
    updateViewSettings();
}

void KisPresetChooser::setIconSize(int newSize)
{
    if (m_strokePreview) {
        m_previewWidth = 3 * qBound(30, newSize, 80);
        updateViewSettings();
        return;
    }
    KisResourceItemChooserSync* chooserSync = KisResourceItemChooserSync::instance();
    chooserSync->setBaseLength(newSize);
}

int KisPresetChooser::iconSize()
{
    if (m_strokePreview)
        return m_previewWidth / 3;
    KisResourceItemChooserSync* chooserSync = KisResourceItemChooserSync::instance();
    return chooserSync->baseLength();
}

void KisPresetChooser::saveIconSize()
{
    if (m_strokePreview) {
        KisConfig(false).writeEntry("Solstice/BrushStrokePreviewWidth", m_previewWidth);
        return;
    }
    // save icon size
    if (KisConfig(true).presetIconSize() != iconSize()) {
        KisConfig(false).setPresetIconSize(iconSize());
    }
}

void KisPresetChooser::showHideBrushNames(ListViewMode newViewMode)
{
    switch (newViewMode) {
    case ListViewMode::Detail: {
        m_delegate->setShowText(true);
        break;
    }
    default: {
        m_delegate->setShowText(false);
    }
    }
}
