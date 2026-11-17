/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisColorReferenceConfigWidget.h"
#include "KisColorReferenceFilter.h"
#include <KisGlobalResourcesInterface.h>
#include <KisViewManager.h>
#include <KoColorModelStandardIds.h>
#include <KoResourcePaths.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <kis_canvas_resource_provider.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
#include <widgets/gradient/KisGenericGradientEditor.h>
#include <widgets/gradient/KisGradientChooser.h>

namespace
{
QStringList droppedImageFiles(const QMimeData *mime)
{
    static const QStringList extensions =
        {"png", "jpg", "jpeg", "tif", "tiff", "bmp", "tga", "psd", "psb", "webp", "kra", "krz"};
    QStringList files;
    for (const auto &url : mime->urls()) {
        if (!url.isLocalFile())
            continue;
        const QFileInfo file(url.toLocalFile());
        if (file.isFile() && extensions.contains(file.suffix().toLower()) && !files.contains(file.absoluteFilePath()))
            files.append(file.absoluteFilePath());
    }
    return files;
}
} // namespace

KisColorReferenceConfigWidget::KisColorReferenceConfigWidget(QWidget *parent,
                                                             KisPaintDeviceSP device,
                                                             const QString &libraryDirectory)
    : KisConfigWidget(parent)
    , m_source(new QComboBox(this))
    , m_pages(new QStackedWidget(this))
    , m_images(new QListWidget(this))
    , m_delete(new QPushButton(i18n("Delete"), this))
    , m_notice(new QLabel(this))
    , m_gradient(new KisGradientChooser(this))
    , m_gradientPreview(new QLabel(this))
    , m_amount(new KisDoubleSliderSpinBox(this))
    , m_keep(new QCheckBox(i18n("Keep brightness"), this))
    , m_library(libraryDirectory.isEmpty() ? QDir(KoResourcePaths::getAppDataLocation()).filePath("colorreference")
                                           : libraryDirectory)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    m_source->setObjectName("colorReferenceSource");
    m_source->addItems({i18n("Image"), i18n("Gradient")});
    form->addRow(i18n("Reference:"), m_source);
    layout->addLayout(form);
    auto *imagesPage = new QWidget(this);
    auto *imagesLayout = new QVBoxLayout(imagesPage);
    imagesLayout->setContentsMargins(0, 0, 0, 0);
    m_images->setObjectName("colorReferenceImages");
    m_images->setViewMode(QListView::IconMode);
    m_images->setResizeMode(QListView::Adjust);
    m_images->setMovement(QListView::Static);
    m_images->setIconSize(QSize(96, 64));
    m_images->setGridSize(QSize(116, 90));
    m_images->setMinimumSize(360, 210);
    m_images->setSelectionMode(QAbstractItemView::SingleSelection);
    m_images->setDragDropMode(QAbstractItemView::DropOnly);
    m_images->setDefaultDropAction(Qt::CopyAction);
    m_images->setDropIndicatorShown(false);
    m_images->viewport()->setAcceptDrops(true);
    m_images->viewport()->installEventFilter(this);
    m_images->setToolTip(i18n("Drop image files here to add references. Multiple files are supported."));
    imagesLayout->addWidget(m_images, 1);
    auto *buttons = new QHBoxLayout();
    auto *load = new QPushButton(i18n("Load Image..."), this);
    buttons->addWidget(load);
    buttons->addWidget(m_delete);
    buttons->addStretch();
    imagesLayout->addLayout(buttons);
    m_notice->setWordWrap(true);
    m_notice->setTextFormat(Qt::PlainText);
    imagesLayout->addWidget(m_notice);
    m_pages->addWidget(imagesPage);
    auto *gradientPage = new QWidget(this);
    auto *gradientLayout = new QVBoxLayout(gradientPage);
    gradientLayout->setContentsMargins(0, 0, 0, 0);
    m_gradient->setObjectName("colorReferenceGradients");
    m_gradient->setEditOptionsVisible(false);
    m_gradient->setNameLabelVisible(false);
    m_gradientPreview->setMinimumHeight(28);
    m_gradientPreview->setMaximumHeight(28);
    m_gradientPreview->setScaledContents(true);
    m_gradientPreview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto *edit = new QPushButton(i18n("Edit Gradient..."), this);
    edit->setObjectName("colorReferenceEditGradient");
    gradientLayout->addWidget(m_gradientPreview);
    gradientLayout->addWidget(m_gradient, 1);
    gradientLayout->addWidget(edit, 0, Qt::AlignLeft);
    m_pages->addWidget(gradientPage);
    layout->addWidget(m_pages, 1);
    auto *adjustments = new QFormLayout();
    m_amount->setObjectName("colorReferenceAmount");
    m_amount->setRange(0, 100, 1);
    m_amount->setSuffix(i18n(" %"));
    m_keep->setObjectName("colorReferenceKeepBrightness");
    adjustments->addRow(i18n("Amount:"), m_amount);
    adjustments->addRow(m_keep);
    layout->addLayout(adjustments);
    auto *hint = new QLabel(
        i18n("Maps dark to light tones to the reference colors. Lower Amount to retain more of the original colors."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    if (device && device->colorSpace()->colorModelId() != RGBAColorModelID) {
        auto *notice =
            new QLabel(i18n("Colors are processed through floating-point RGB. Grayscale documents remain grayscale."),
                       this);
        notice->setWordWrap(true);
        layout->addWidget(notice);
    }
    refreshLibrary();
    setConfiguration(KisColorReferenceFilter().defaultConfiguration(KisGlobalResourcesInterface::instance()));
    connect(m_images, &QListWidget::itemSelectionChanged, this, &KisColorReferenceConfigWidget::selectImage);
    connect(m_source, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_pages->setCurrentIndex(index);
        m_stored->setProperty("source", index);
        m_stored->setProperty("lut", m_stored->getString(index == 0 ? "imageLut" : "gradientLut"));
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_gradient, &KisGradientChooser::resourceClicked, this, [this](KoResourceSP resource) {
        updateGradient(resource.dynamicCast<KoAbstractGradient>());
    });
    connect(m_gradient, &KisGradientChooser::resourceSelected, this, [this](KoResourceSP resource) {
        updateGradient(resource.dynamicCast<KoAbstractGradient>());
    });
    connect(edit, &QPushButton::clicked, this, &KisColorReferenceConfigWidget::editGradient);
    connect(m_amount, &KisDoubleSliderSpinBox::valueChanged, this, [this] {
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(m_keep, &QCheckBox::toggled, this, [this] {
        Q_EMIT sigConfigurationItemChanged();
    });
    connect(load, &QPushButton::clicked, this, &KisColorReferenceConfigWidget::loadImage);
    connect(m_delete, &QPushButton::clicked, this, &KisColorReferenceConfigWidget::deleteImage);
}
void KisColorReferenceConfigWidget::refreshLibrary()
{
    const QSignalBlocker blocker(m_images);
    m_images->clear();
    const auto names = ColorReference::presetNames();
    for (int i = 0; i < names.size(); ++i) {
        auto *item = new QListWidgetItem(QIcon(QPixmap::fromImage(ColorReference::presetImage(i))), names[i], m_images);
        item->setData(Qt::UserRole, QString("preset:%1").arg(i));
    }
    for (const QString &id : m_library.entries()) {
        auto *item = new QListWidgetItem(QIcon(QPixmap::fromImage(m_library.image(id))),
                                         QFileInfo(id).completeBaseName(),
                                         m_images);
        item->setData(Qt::UserRole, QString("user:" + id));
        item->setToolTip(id);
    }
}
void KisColorReferenceConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    const QSignalBlocker a(this), b(m_images), c(m_source), d(m_amount), e(m_keep), f(m_gradient);
    const auto *filterConfig = dynamic_cast<const KisFilterConfiguration *>(config.data());
    m_stored = filterConfig ? filterConfig->clone()
                            : KisColorReferenceFilter().defaultConfiguration(KisGlobalResourcesInterface::instance());
    if (!filterConfig)
        m_stored->fromXML(config->toXML());
    m_source->setCurrentIndex(qBound(0, config->getInt("source", 0), 1));
    m_pages->setCurrentIndex(m_source->currentIndex());
    m_amount->setValue(config->getDouble("amount", 100));
    m_keep->setChecked(config->getBool("keepBrightness", true));
    const auto gradient = ColorReference::gradientFromXml(config->getString("gradientXML"));
    m_currentGradient = gradient ? gradient : ColorReference::defaultGradient();
    m_gradient->setCurrentResource(m_currentGradient);
    updateGradientPreview();
    m_images->clearSelection();
    m_images->setCurrentItem(nullptr);
    const QString id = config->getString("referenceId");
    bool found = false;
    for (int i = 0; i < m_images->count(); ++i)
        if (m_images->item(i)->data(Qt::UserRole).toString() == id) {
            m_images->setCurrentRow(i);
            found = true;
            break;
        }
    m_delete->setEnabled(found && id.startsWith("user:"));
    m_notice->setText(found ? QString()
                            : i18n("The reference image is unavailable. The saved colors are still in use."));
}
KisPropertiesConfigurationSP KisColorReferenceConfigWidget::configuration() const
{
    auto config = m_stored->clone();
    config->setProperty("amount", m_amount->value());
    config->setProperty("keepBrightness", m_keep->isChecked());
    return config;
}
void KisColorReferenceConfigWidget::selectImage()
{
    const auto *item = m_images->currentItem();
    if (!item)
        return;
    const QString id = item->data(Qt::UserRole).toString();
    m_delete->setEnabled(id.startsWith("user:"));
    const QImage image =
        id.startsWith("preset:") ? ColorReference::presetImage(id.mid(7).toInt()) : m_library.image(id.mid(5));
    const QString lut = ColorReference::encode(ColorReference::fromImage(image));
    if (lut.isEmpty()) {
        m_notice->setText(i18n("Cannot read this reference. The previous colors are still in use."));
        return;
    }
    m_stored->setProperty("referenceId", id);
    m_stored->setProperty("imageLut", lut);
    if (m_source->currentIndex() == 0)
        m_stored->setProperty("lut", lut);
    m_notice->clear();
    Q_EMIT sigConfigurationItemChanged();
}
void KisColorReferenceConfigWidget::updateGradient(KoAbstractGradientSP gradient)
{
    if (!gradient)
        return;
    gradient = gradient->clone().dynamicCast<KoAbstractGradient>();
    if (canvasResourcesInterface())
        gradient->bakeVariableColors(canvasResourcesInterface());
    const QString lut = ColorReference::encode(ColorReference::fromGradient(gradient));
    if (lut.isEmpty())
        return;
    const QString xml = ColorReference::gradientXml(gradient);
    // Mouse selection can emit both selected and clicked. Keyboard navigation
    // emits only selected; clicking an already-selected preset must still work.
    if (m_stored->getString("gradientLut") == lut && m_stored->getString("gradientXML") == xml)
        return;
    m_currentGradient = gradient;
    m_stored->setProperty("gradientXML", xml);
    m_stored->setProperty("gradientLut", lut);
    if (m_source->currentIndex() == 1)
        m_stored->setProperty("lut", lut);
    updateGradientPreview();
    Q_EMIT sigConfigurationItemChanged();
}
void KisColorReferenceConfigWidget::updateGradientPreview()
{
    const auto table = ColorReference::decode(m_stored->getString("gradientLut"));
    if (table.isEmpty()) {
        m_gradientPreview->clear();
        return;
    }
    QImage preview(256, 1, QImage::Format_RGB32);
    for (int x = 0; x < preview.width(); ++x) {
        const auto c = ColorReference::lookup(table, double(x) / (preview.width() - 1));
        preview.setPixel(x,
                         0,
                         qRgb(qRound(qBound(0.0, c[0], 1.0) * 255),
                              qRound(qBound(0.0, c[1], 1.0) * 255),
                              qRound(qBound(0.0, c[2], 1.0) * 255)));
    }
    m_gradientPreview->setPixmap(QPixmap::fromImage(preview));
}
void KisColorReferenceConfigWidget::editGradient()
{
    QDialog dialog(this);
    dialog.setWindowTitle(i18n("Edit Reference Gradient"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    auto *editor = new KisGenericGradientEditor(scroll);
    editor->setGradientPresetChooserVisible(false);
    editor->setGradientPresetChooserOptionsButtonVisible(false);
    editor->setUpdateGradientButtonVisible(false);
    editor->setAddGradientButtonVisible(false);
    editor->setCanvasResourcesInterface(canvasResourcesInterface());
    editor->setGradient(m_currentGradient);
    scroll->setWidget(editor);
    layout->addWidget(scroll, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(700, 440);
    if (dialog.exec() == QDialog::Accepted)
        updateGradient(editor->gradient());
}
void KisColorReferenceConfigWidget::loadImage()
{
    if (m_importing)
        return;
    if (m_library.entries().size() >= 50) {
        QMessageBox::information(this, i18n("Color Reference"), i18n("The library already contains 50 images."));
        return;
    }
    const QString filename = QFileDialog::getOpenFileName(
        this,
        i18n("Load Reference Image"),
        QString(),
        i18n("Images (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.tga *.psd *.psb *.webp *.kra *.krz)"));
    if (filename.isEmpty())
        return;
    importImages({filename});
}
bool KisColorReferenceConfigWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_images->viewport()
        && (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop)) {
        auto *drop = static_cast<QDropEvent *>(event);
        const QStringList files = droppedImageFiles(drop->mimeData());
        if (m_importing || m_source->currentIndex() != 0 || files.isEmpty()
            || !drop->possibleActions().testFlag(Qt::CopyAction)) {
            drop->ignore();
            return true;
        }
        // Always copy: never allow a file manager to move/delete the originals.
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        if (event->type() == QEvent::Drop) {
            // Finish the native drag before invoking importers. The widget context
            // cancels this callback if the filter dialog closes in the meantime.
            QTimer::singleShot(0, this, [this, files] {
                importImages(files);
            });
        }
        return true;
    }
    return KisConfigWidget::eventFilter(watched, event);
}
void KisColorReferenceConfigWidget::importImages(const QStringList &filenames)
{
    if (m_importing || filenames.isEmpty())
        return;
    const QScopedValueRollback<bool> importing(m_importing, true);
    QStringList errors;
    QString lastId;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const QString &filename : filenames) {
        if (m_library.entries().size() >= 50) {
            errors.append(i18n("The library contains 50 images. Remaining files were not added."));
            break;
        }
        QString error;
        const auto image = ColorReference::Library::importImage(filename, &error);
        const QString id = image.isNull() ? QString() : m_library.add(image, QFileInfo(filename).fileName(), &error);
        if (id.isEmpty())
            errors.append(i18n("Cannot load %1. %2", QFileInfo(filename).fileName(), error));
        else
            lastId = id;
    }
    QApplication::restoreOverrideCursor();
    if (!lastId.isEmpty()) {
        refreshLibrary();
        for (int i = 0; i < m_images->count(); ++i)
            if (m_images->item(i)->data(Qt::UserRole).toString() == "user:" + lastId) {
                m_images->setCurrentRow(i);
                m_images->scrollToItem(m_images->item(i));
                break;
            }
    }
    if (!errors.isEmpty())
        m_notice->setText(errors.join('\n'));
}
void KisColorReferenceConfigWidget::deleteImage()
{
    if (m_importing)
        return;
    const auto *item = m_images->currentItem();
    if (!item)
        return;
    const QString id = item->data(Qt::UserRole).toString();
    if (!id.startsWith("user:"))
        return;
    if (QMessageBox::question(
            this,
            i18n("Delete Reference Image"),
            i18n("Delete %1 from the reference library? Existing filter layers will keep their saved colors.",
                 item->text()))
        != QMessageBox::Yes)
        return;
    if (!m_library.remove(id.mid(5))) {
        QMessageBox::warning(this, i18n("Color Reference"), i18n("Cannot delete the reference image."));
        return;
    }
    const auto config = configuration();
    refreshLibrary();
    setConfiguration(config); // Deletion never changes the applied table.
}
void KisColorReferenceConfigWidget::setView(KisViewManager *view)
{
    setCanvasResourcesInterface(view ? view->canvasResourceProvider()->resourceManager()->canvasResourcesInterface()
                                     : nullptr);
}
void KisColorReferenceConfigWidget::setCanvasResourcesInterface(KoCanvasResourcesInterfaceSP resources)
{
    const QSignalBlocker blocker(m_gradient);
    m_gradient->setCanvasResourcesInterface(resources);
    KisConfigWidget::setCanvasResourcesInterface(resources);
}
