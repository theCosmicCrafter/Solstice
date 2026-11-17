/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisLazyToolsFastExportDialog.h"

#include "KisDocument.h"
#include "KisLazyToolsFastExportPresets.h"
#include "kis_config.h"
#include "kis_image.h"
#include "kis_layer_composition.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QUrl>
#include <QVBoxLayout>
#include <klocalizedstring.h>

namespace
{
namespace Presets = KisLazyToolsFastExportPresets;

// The layer comp combo box keeps each comp's plain name here; the text of a
// missing comp says that it is missing.
const int LayerCompNameRole = Qt::UserRole + 1;

// Accepts a folder dragged from the system file manager, so a subfolder found
// from a registered folder can become the export folder.
class FolderLineEdit : public QLineEdit
{
public:
    FolderLineEdit(const QString &text, QWidget *parent, const std::function<void()> &dropped)
        : QLineEdit(text, parent)
        , m_dropped(dropped)
    {
    }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (!droppedFolder(event->mimeData()).isEmpty()) {
            event->acceptProposedAction();
        } else {
            QLineEdit::dragEnterEvent(event);
        }
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (!droppedFolder(event->mimeData()).isEmpty()) {
            event->acceptProposedAction();
        } else {
            QLineEdit::dragMoveEvent(event);
        }
    }

    void dropEvent(QDropEvent *event) override
    {
        const QString folder = droppedFolder(event->mimeData());
        if (folder.isEmpty()) {
            QLineEdit::dropEvent(event);
            return;
        }
        setText(folder);
        event->acceptProposedAction();
        m_dropped();
    }

private:
    static QString droppedFolder(const QMimeData *mimeData)
    {
        if (!mimeData || !mimeData->hasUrls()) {
            return QString();
        }
        for (const QUrl &url : mimeData->urls()) {
            if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir()) {
                return url.toLocalFile();
            }
        }
        return QString();
    }

    std::function<void()> m_dropped;
};

class FastExportDialog : public QDialog
{
public:
    FastExportDialog(QWidget *parent,
                     const KisLazyToolsFastExportDialog::Documents &documents,
                     const KisLazyToolsFastExport::WaitForImage &wait,
                     KisDocument *presetDocument)
        : QDialog(parent)
        , m_documents(documents)
        , m_wait(wait)
        , m_presetDocument(presetDocument)
    {
        setWindowTitle(i18n("Fast Image Export"));
        setAttribute(Qt::WA_DeleteOnClose);

        const auto settings = KisLazyToolsFastExport::Settings::load();
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        m_format = new QComboBox(this);
        m_format->addItem(i18n("PNG"), QStringLiteral("png"));
        m_format->addItem(i18n("JPEG"), QStringLiteral("jpg"));
        m_scope = new QComboBox(this);
        m_scope->addItem(i18n("Active document"), false);
        m_scope->addItem(i18n("All open documents"), true);
        m_destination = new QComboBox(this);
        m_destination->addItem(i18n("Same folder as the document"), 0);
        m_destination->addItem(i18n("Configured export folder"), 1);
        m_destination->addItem(i18n("Choose a folder now"), 2);
        m_destination->setCurrentIndex(qMax(0, m_destination->findData(settings.destination)));
        m_registeredFolder = new QComboBox(this);
        m_registeredFolder->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_registeredFolder->setMinimumContentsLength(30);
        auto *browse = new QPushButton(i18n("Browse…"), this);
        const QStringList explorerFolders = KisLazyToolsFastExport::explorerFolders();
        for (const QString &folder : explorerFolders) {
            m_registeredFolder->addItem(QDir::toNativeSeparators(folder), folder);
            m_registeredFolder->setItemData(m_registeredFolder->count() - 1,
                                            QDir::toNativeSeparators(folder),
                                            Qt::ToolTipRole);
        }
        if (explorerFolders.isEmpty()) {
            m_registeredFolder->addItem(i18n("No registered folders"));
            m_registeredFolder->setEnabled(false);
        }
        m_registeredFolder->setToolTip(
            i18n("Browse… starts in this folder. Register folders in Settings > Configure Solstice > General > "
                 "Custom."));
        browse->setToolTip(i18n("Choose the export folder, starting in the registered folder."));
        auto *registeredRow = new QHBoxLayout;
        registeredRow->addWidget(m_registeredFolder, 1);
        registeredRow->addWidget(browse);
        m_folder = new FolderLineEdit(settings.folder, this, [this] {
            m_destination->setCurrentIndex(m_destination->findData(1));
        });
        m_folder->setToolTip(i18n("Drop a folder here from the file manager to export into it."));
        m_name = new QLineEdit(this);
        m_name->setPlaceholderText(i18n("Same as the document"));
        m_name->setToolTip(i18n("Optional file name without the extension. Used only for the active document."));
        auto *clearName = new QPushButton(i18n("Clear"), this);
        clearName->setEnabled(false);
        auto *nameRow = new QHBoxLayout;
        nameRow->addWidget(m_name, 1);
        nameRow->addWidget(clearName);
        m_pngCompression = new QSpinBox(this);
        m_pngCompression->setRange(0, 9);
        m_pngCompression->setValue(settings.pngCompression);
        m_pngAlpha = new QCheckBox(i18n("Save alpha channel"), this);
        m_pngAlpha->setChecked(settings.pngAlpha);
        m_jpegQuality = new QSpinBox(this);
        m_jpegQuality->setRange(0, 100);
        m_jpegQuality->setValue(settings.jpegQuality);
        // Created after the original controls, so their order among the
        // dialog's combo boxes and check boxes stays the same.
        m_area = new QComboBox(this);
        m_area->addItem(i18n("Whole image"), static_cast<int>(Presets::Area::WholeImage));
        m_area->addItem(i18n("Selection"), static_cast<int>(Presets::Area::Selection));
        m_area->setToolTip(i18n("Export the whole image, or the area selected when the export runs."));
        m_relative = new QCheckBox(i18n("Relative to the document"), this);
        m_relative->setToolTip(
            i18n("Save the export folder relative to the document's folder, so it follows the document when "
                 "both are moved together."));
        m_layerComp = new QComboBox(this);
        m_layerComp->setToolTip(
            i18n("Export the image as this layer comp shows it. The comp is applied to a copy of the image, so "
                 "the canvas does not change."));

        form->addRow(i18n("Format:"), m_format);
        form->addRow(i18n("Documents:"), m_scope);
        form->addRow(i18n("Destination:"), m_destination);
        form->addRow(i18n("Registered folder:"), registeredRow);
        form->addRow(i18n("Export folder:"), m_folder);
        form->addRow(QString(), m_relative);
        form->addRow(i18n("Name:"), nameRow);
        form->addRow(i18n("Area:"), m_area);
        form->addRow(i18n("Layer comp:"), m_layerComp);
        form->addRow(i18n("PNG compression:"), m_pngCompression);
        form->addRow(QString(), m_pngAlpha);
        form->addRow(i18n("JPEG quality:"), m_jpegQuality);
        layout->addWidget(createPresetGroup());
        layout->addLayout(form);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        m_exportButton = buttons->addButton(i18n("Export"), QDialogButtonBox::AcceptRole);
        m_regionButton = buttons->addButton(i18n("Export Region"), QDialogButtonBox::ActionRole);
        m_regionButton->setToolTip(i18n("Export the selected area of the active document."));
        m_clearName = clearName;
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(browse, &QPushButton::clicked, this, [this] {
            browseExportFolder();
        });
        connect(m_exportButton, &QPushButton::clicked, this, [this] {
            exportImages();
        });
        connect(m_regionButton, &QPushButton::clicked, this, [this] {
            exportRegion();
        });
        connect(clearName, &QPushButton::clicked, m_name, &QLineEdit::clear);
        connect(m_name, &QLineEdit::textChanged, this, [this] {
            updateState();
        });
        connect(m_scope, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
            updateState();
        });

        // While a saved export is selected, the form edits it.
        auto apply = [this] {
            applyFormToPreset();
        };
        connect(m_format, QOverload<int>::of(&QComboBox::currentIndexChanged), this, apply);
        connect(m_destination, QOverload<int>::of(&QComboBox::currentIndexChanged), this, apply);
        // The one-off destination is remembered as soon as it is chosen, not
        // only on Export, so the next dialog opens with it.
        connect(m_destination, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
            if (m_loading || editingPreset()) {
                return;
            }
            auto remembered = KisLazyToolsFastExport::Settings::load();
            remembered.destination = m_destination->currentData().toInt();
            remembered.save();
        });
        connect(m_folder, &QLineEdit::textChanged, this, apply);
        connect(m_relative, &QCheckBox::toggled, this, apply);
        connect(m_name, &QLineEdit::textChanged, this, apply);
        connect(m_area, QOverload<int>::of(&QComboBox::currentIndexChanged), this, apply);
        connect(m_layerComp, QOverload<int>::of(&QComboBox::currentIndexChanged), this, apply);
        connect(m_pngCompression, QOverload<int>::of(&QSpinBox::valueChanged), this, apply);
        connect(m_pngAlpha, &QCheckBox::toggled, this, apply);
        connect(m_jpegQuality, QOverload<int>::of(&QSpinBox::valueChanged), this, apply);

        reloadPresets();
        updateState();
    }

protected:
    void changeEvent(QEvent *event) override
    {
        QDialog::changeEvent(event);
        // Another dialog may have changed the document's saved exports.
        if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
            reloadPresets();
        }
    }

private:
    QGroupBox *createPresetGroup()
    {
        m_presetGroup = new QGroupBox(this);
        m_presetList = new QListWidget(m_presetGroup);
        m_presetList->setSelectionMode(QAbstractItemView::SingleSelection);
        m_presetList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
        m_presetList->setToolTip(
            i18n("Export settings saved with this document. Select one to edit it below; check the ones Export "
                 "Checked runs."));
        m_addPreset = new QPushButton(i18n("Add"), m_presetGroup);
        m_addPreset->setToolTip(i18n("Save the settings below as a new export of this document."));
        m_duplicatePreset = new QPushButton(i18n("Duplicate"), m_presetGroup);
        m_renamePreset = new QPushButton(i18n("Rename"), m_presetGroup);
        m_removePreset = new QPushButton(i18n("Remove"), m_presetGroup);
        m_moveUp = new QPushButton(i18n("Move Up"), m_presetGroup);
        m_moveDown = new QPushButton(i18n("Move Down"), m_presetGroup);
        m_exportChecked = new QPushButton(i18n("Export Checked"), m_presetGroup);
        m_exportChecked->setToolTip(i18n("Run the checked saved exports."));
        m_exportAll = new QPushButton(i18n("Export All"), m_presetGroup);
        m_exportAll->setToolTip(i18n("Run all saved exports."));
        m_presetNote = new QLabel(m_presetGroup);
        m_presetNote->setWordWrap(true);

        auto *side = new QVBoxLayout;
        for (QPushButton *button :
             {m_addPreset, m_duplicatePreset, m_renamePreset, m_removePreset, m_moveUp, m_moveDown}) {
            side->addWidget(button);
        }
        side->addStretch(1);
        auto *listRow = new QHBoxLayout;
        listRow->addWidget(m_presetList, 1);
        listRow->addLayout(side);
        auto *runRow = new QHBoxLayout;
        runRow->addStretch(1);
        runRow->addWidget(m_exportChecked);
        runRow->addWidget(m_exportAll);
        auto *groupLayout = new QVBoxLayout(m_presetGroup);
        groupLayout->addLayout(listRow);
        groupLayout->addWidget(m_presetNote);
        groupLayout->addLayout(runRow);

        connect(m_presetList, &QListWidget::currentRowChanged, this, [this] {
            presetSelectionChanged();
        });
        connect(m_presetList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
            presetItemChanged(item);
        });
        connect(m_addPreset, &QPushButton::clicked, this, [this] {
            addPreset();
        });
        connect(m_duplicatePreset, &QPushButton::clicked, this, [this] {
            duplicatePreset();
        });
        connect(m_renamePreset, &QPushButton::clicked, this, [this] {
            if (QListWidgetItem *item = m_presetList->currentItem()) {
                m_presetList->editItem(item);
            }
        });
        connect(m_removePreset, &QPushButton::clicked, this, [this] {
            removePreset();
        });
        connect(m_moveUp, &QPushButton::clicked, this, [this] {
            movePreset(-1);
        });
        connect(m_moveDown, &QPushButton::clicked, this, [this] {
            movePreset(1);
        });
        connect(m_exportChecked, &QPushButton::clicked, this, [this] {
            runPresets(Presets::Which::Checked);
        });
        connect(m_exportAll, &QPushButton::clicked, this, [this] {
            runPresets(Presets::Which::All);
        });
        return m_presetGroup;
    }

    QString documentPath() const
    {
        return m_presetDocument ? m_presetDocument->localFilePath() : QString();
    }

    // The summary with the comp's current name, which a rename in the Layer
    // Comps docker changes.
    QString presetSummary(Presets::Preset preset) const
    {
        if (m_presetDocument) {
            if (const KisLayerCompositionSP comp = Presets::findLayerComp(m_presetDocument->image(), preset)) {
                preset.layerCompName = comp->name();
            }
        }
        return Presets::summary(preset, documentPath());
    }

    QString currentPresetId() const
    {
        const QListWidgetItem *item = m_presetList->currentItem();
        return item ? item->data(Qt::UserRole).toString() : QString();
    }

    bool editingPreset() const
    {
        return !m_editingId.isEmpty();
    }

    // Rebuilds the list from the document, keeping the selected row.
    void reloadPresets()
    {
        const QString selectedId = m_presetList->currentRow() > 0 ? currentPresetId() : QString();
        const QList<Presets::Preset> presets = Presets::presets(m_presetDocument);
        {
            QSignalBlocker blocker(m_presetList);
            m_presetList->clear();
            auto *oneOff = new QListWidgetItem(i18n("One-off export (not saved)"), m_presetList);
            oneOff->setToolTip(i18n("Export once with the settings below, as before."));
            int selectedRow = 0;
            for (const Presets::Preset &preset : presets) {
                auto *item = new QListWidgetItem(preset.name, m_presetList);
                item->setData(Qt::UserRole, preset.id);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
                item->setCheckState(preset.enabled ? Qt::Checked : Qt::Unchecked);
                item->setToolTip(presetSummary(preset));
                if (preset.id == selectedId) {
                    selectedRow = m_presetList->count() - 1;
                }
            }
            m_presetList->setCurrentRow(selectedRow);
        }

        if (m_presetDocument) {
            m_presetGroup->setTitle(i18n("Saved exports of %1", m_presetDocument->caption()));
        } else {
            m_presetGroup->setTitle(i18n("Saved exports"));
        }
        if (!m_presetDocument) {
            m_presetNote->setText(i18n("Open a document to save exports with it."));
        } else if (!Presets::canStore(m_presetDocument)) {
            m_presetNote->setText(
                i18n("This file format cannot keep saved exports; they last until the document is closed. Save "
                     "the document as .kra to keep them."));
        } else {
            m_presetNote->clear();
        }
        m_presetNote->setVisible(!m_presetNote->text().isEmpty());
        presetSelectionChanged();
    }

    void presetSelectionChanged()
    {
        const QString id = m_presetList->currentRow() > 0 ? currentPresetId() : QString();
        if (!editingPreset() && !id.isEmpty()) {
            m_oneOff = formState();
        }
        m_editingId = id;
        if (editingPreset()) {
            for (const Presets::Preset &preset : Presets::presets(m_presetDocument)) {
                if (preset.id == id) {
                    loadPreset(preset);
                }
            }
        } else {
            fillLayerComps(nullptr);
            if (m_oneOff.valid) {
                restoreFormState(m_oneOff);
                m_oneOff.valid = false;
            }
        }
        updateState();
    }

    // Lists the document's layer comps and selects the one the preset uses. A
    // comp it uses that the document no longer has stays listed as missing, so
    // editing other settings does not change the export's comp.
    void fillLayerComps(const Presets::Preset *preset)
    {
        QScopedValueRollback<bool> loading(m_loading, true);
        m_layerComp->clear();
        m_layerComp->addItem(i18nc("Saved export layer comp", "None (the layers as they are)"), QString());
        for (const Presets::LayerComp &comp : Presets::layerComps(m_presetDocument)) {
            m_layerComp->addItem(comp.name, comp.id);
            m_layerComp->setItemData(m_layerComp->count() - 1, comp.name, LayerCompNameRole);
        }
        int index = 0;
        if (preset && !preset->layerComp.isEmpty()) {
            const KisLayerCompositionSP comp =
                m_presetDocument ? Presets::findLayerComp(m_presetDocument->image(), *preset) : KisLayerCompositionSP();
            index = comp ? m_layerComp->findData(comp->uuid().toString(QUuid::WithoutBraces)) : -1;
            if (index < 0) {
                m_layerComp->addItem(i18nc("A layer comp that a saved export uses but the document no longer has",
                                           "%1 (missing)",
                                           preset->layerCompName),
                                     preset->layerComp);
                index = m_layerComp->count() - 1;
                m_layerComp->setItemData(index, preset->layerCompName, LayerCompNameRole);
            }
        }
        m_layerComp->setCurrentIndex(index);
    }

    struct FormState {
        bool valid{false};
        int format{0};
        int destination{0};
        QString folder;
        QString name;
        int pngCompression{6};
        bool pngAlpha{true};
        int jpegQuality{90};
    };

    FormState formState() const
    {
        return {true,
                m_format->currentIndex(),
                m_destination->currentIndex(),
                m_folder->text(),
                m_name->text(),
                m_pngCompression->value(),
                m_pngAlpha->isChecked(),
                m_jpegQuality->value()};
    }

    void restoreFormState(const FormState &state)
    {
        QScopedValueRollback<bool> loading(m_loading, true);
        m_format->setCurrentIndex(state.format);
        m_destination->setCurrentIndex(state.destination);
        m_folder->setText(state.folder);
        m_name->setText(state.name);
        m_pngCompression->setValue(state.pngCompression);
        m_pngAlpha->setChecked(state.pngAlpha);
        m_jpegQuality->setValue(state.jpegQuality);
        m_relative->setChecked(false);
        m_area->setCurrentIndex(0);
    }

    void loadPreset(const Presets::Preset &preset)
    {
        QScopedValueRollback<bool> loading(m_loading, true);
        m_format->setCurrentIndex(qMax(0, m_format->findData(preset.format)));
        m_destination->setCurrentIndex(
            m_destination->findData(preset.destination == Presets::Destination::Folder ? 1 : 0));
        const QString resolved = Presets::resolvedFolder(preset, documentPath());
        m_folder->setText(preset.destination == Presets::Destination::Folder && !resolved.isEmpty() ? resolved
                                                                                                    : preset.folder);
        m_relative->setChecked(preset.folderRelative);
        m_name->setText(preset.nameRule == Presets::NameRule::Fixed ? preset.fixedName : QString());
        m_area->setCurrentIndex(m_area->findData(static_cast<int>(preset.area)));
        fillLayerComps(&preset);
        m_pngCompression->setValue(preset.pngCompression);
        m_pngAlpha->setChecked(preset.pngAlpha);
        m_jpegQuality->setValue(preset.jpegQuality);
    }

    // Fills a preset from the form. Returns false when the name is invalid.
    bool readForm(Presets::Preset *preset) const
    {
        const QString format = m_format->currentData().toString();
        const QString name = KisLazyToolsFastExport::normalizedBaseName(m_name->text(), format);
        if (!name.isEmpty() && !KisLazyToolsFastExport::isValidBaseName(name)) {
            return false;
        }
        if (preset->format != format || preset->formatOptions.isEmpty()) {
            // Keep the options File > Export remembers for this format, so
            // later changes there do not change this export.
            preset->formatOptions = KisConfig(true).exportConfigurationXML(
                format == QLatin1String("png") ? QStringLiteral("image/png") : QStringLiteral("image/jpeg"));
        }
        preset->format = format;
        preset->pngCompression = m_pngCompression->value();
        preset->pngAlpha = m_pngAlpha->isChecked();
        preset->jpegQuality = m_jpegQuality->value();
        const int destination = m_destination->currentData().toInt();
        const QString folder = m_folder->text().trimmed();
        preset->destination = destination == 0 || (destination == 2 && folder.isEmpty())
            ? Presets::Destination::SameFolder
            : Presets::Destination::Folder;
        preset->folderRelative = m_relative->isChecked() && !documentPath().isEmpty();
        preset->folder = Presets::storedFolder(folder, preset->folderRelative, documentPath());
        preset->nameRule = name.isEmpty() ? Presets::NameRule::DocumentName : Presets::NameRule::Fixed;
        preset->fixedName = name;
        preset->area =
            editingPreset() ? static_cast<Presets::Area>(m_area->currentData().toInt()) : Presets::Area::WholeImage;
        // Like Area, chosen after the export is saved.
        preset->layerComp = editingPreset() ? m_layerComp->currentData().toString() : QString();
        preset->layerCompName =
            preset->layerComp.isEmpty() ? QString() : m_layerComp->currentData(LayerCompNameRole).toString();
        return true;
    }

    // Reads the document's current list, changes it and stores it, so two
    // open dialogs do not overwrite each other's changes.
    void mutate(const std::function<void(QList<Presets::Preset> &)> &change)
    {
        if (!m_presetDocument) {
            return;
        }
        QList<Presets::Preset> presets = Presets::presets(m_presetDocument);
        change(presets);
        Presets::setPresets(m_presetDocument, presets);
    }

    int indexOf(const QList<Presets::Preset> &presets, const QString &id) const
    {
        for (int i = 0; i < presets.size(); ++i) {
            if (presets[i].id == id) {
                return i;
            }
        }
        return -1;
    }

    void selectPreset(const QString &id)
    {
        for (int row = 1; row < m_presetList->count(); ++row) {
            if (m_presetList->item(row)->data(Qt::UserRole).toString() == id) {
                m_presetList->setCurrentRow(row);
                return;
            }
        }
        m_presetList->setCurrentRow(0);
    }

    void applyFormToPreset()
    {
        updateState();
        if (m_loading || !editingPreset()) {
            return;
        }
        const QString id = m_editingId;
        mutate([&](QList<Presets::Preset> &presets) {
            const int index = indexOf(presets, id);
            if (index >= 0) {
                readForm(&presets[index]);
            }
        });
        for (const Presets::Preset &preset : Presets::presets(m_presetDocument)) {
            if (preset.id == id && m_presetList->currentItem()) {
                m_presetList->currentItem()->setToolTip(presetSummary(preset));
            }
        }
    }

    void presetItemChanged(QListWidgetItem *item)
    {
        const QString id = item->data(Qt::UserRole).toString();
        if (id.isEmpty()) {
            return;
        }
        const QString name = item->text().trimmed();
        const bool enabled = item->checkState() == Qt::Checked;
        mutate([&](QList<Presets::Preset> &presets) {
            const int index = indexOf(presets, id);
            if (index < 0) {
                return;
            }
            if (!name.isEmpty()) {
                presets[index].name = name;
            }
            presets[index].enabled = enabled;
        });
        if (name.isEmpty() || name != item->text()) {
            reloadPresets();
        }
    }

    QString uniqueName(const QList<Presets::Preset> &presets) const
    {
        QStringList names;
        for (const Presets::Preset &preset : presets) {
            names << preset.name;
        }
        for (int number = presets.size() + 1;; ++number) {
            const QString name = i18nc("Default name of a saved export", "Export %1", number);
            if (!names.contains(name)) {
                return name;
            }
        }
    }

    void addPreset()
    {
        QString id;
        bool valid = true;
        mutate([&](QList<Presets::Preset> &presets) {
            Presets::Preset preset = Presets::newPreset(uniqueName(presets));
            valid = readForm(&preset);
            if (valid) {
                id = preset.id;
                presets << preset;
            }
        });
        if (!valid) {
            warnInvalidName();
            return;
        }
        reloadPresets();
        selectPreset(id);
        if (QListWidgetItem *item = m_presetList->currentItem()) {
            m_presetList->editItem(item);
        }
    }

    void duplicatePreset()
    {
        const QString sourceId = m_editingId;
        QString id;
        mutate([&](QList<Presets::Preset> &presets) {
            const int index = indexOf(presets, sourceId);
            if (index < 0) {
                return;
            }
            Presets::Preset copy = presets[index];
            copy.id = Presets::newPreset(QString()).id;
            copy.name = i18nc("Name of a duplicated saved export", "%1 copy", copy.name);
            id = copy.id;
            presets.insert(index + 1, copy);
        });
        reloadPresets();
        selectPreset(id);
    }

    void removePreset()
    {
        const QString id = m_editingId;
        QString next;
        mutate([&](QList<Presets::Preset> &presets) {
            const int index = indexOf(presets, id);
            if (index < 0) {
                return;
            }
            presets.removeAt(index);
            if (!presets.isEmpty()) {
                next = presets[qMin(index, presets.size() - 1)].id;
            }
        });
        reloadPresets();
        selectPreset(next);
    }

    void movePreset(int offset)
    {
        const QString id = m_editingId;
        mutate([&](QList<Presets::Preset> &presets) {
            const int index = indexOf(presets, id);
            if (index >= 0 && index + offset >= 0 && index + offset < presets.size()) {
                presets.move(index, index + offset);
            }
        });
        reloadPresets();
        selectPreset(id);
    }

    void updateState()
    {
        const bool single = !m_scope->currentData().toBool();
        const bool editing = editingPreset();
        // One name or selection cannot serve several documents, so both apply
        // only to the active document. A saved export keeps its own name.
        m_name->setEnabled(single || editing);
        m_clearName->setEnabled((single || editing) && !m_name->text().isEmpty());
        m_exportButton->setEnabled(!editing);
        m_regionButton->setEnabled(single && !editing);
        m_area->setEnabled(editing);
        m_layerComp->setEnabled(editing);
        const bool usesLayerComp = editing && !m_layerComp->currentData().toString().isEmpty();
        m_name->setPlaceholderText(usesLayerComp ? i18n("Document name_layer comp name")
                                                 : i18n("Same as the document"));
        m_relative->setEnabled(editing && !documentPath().isEmpty() && m_destination->currentData().toInt() != 0);
        // A saved export cannot ask for a folder each time it runs.
        if (auto *model = qobject_cast<QStandardItemModel *>(m_destination->model())) {
            if (QStandardItem *item = model->item(m_destination->findData(2))) {
                item->setEnabled(!editing);
            }
        }

        const int row = m_presetList->currentRow();
        const bool hasDocument = !m_presetDocument.isNull();
        m_presetList->setEnabled(hasDocument);
        m_addPreset->setEnabled(hasDocument);
        m_duplicatePreset->setEnabled(editing);
        m_renamePreset->setEnabled(editing);
        m_removePreset->setEnabled(editing);
        m_moveUp->setEnabled(editing && row > 1);
        m_moveDown->setEnabled(editing && row + 1 < m_presetList->count());
        const bool hasPresets = m_presetList->count() > 1;
        m_exportChecked->setEnabled(hasDocument && hasPresets);
        m_exportAll->setEnabled(hasDocument && hasPresets);
    }

    void warnInvalidName()
    {
        QMessageBox::warning(this,
                             i18n("Fast Image Export"),
                             i18n("The name cannot contain \\ / : * ? \" < > | or end with a dot or a space."));
    }

    void runPresets(Presets::Which which)
    {
        if (!m_presetDocument) {
            return;
        }
        // The list shown belongs to this dialog's document; all open documents
        // run their own saved exports.
        const QList<QPointer<KisDocument>> documents =
            m_scope->currentData().toBool() ? m_documents(true) : QList<QPointer<KisDocument>>({m_presetDocument});
        const auto result = Presets::run(documents, which, m_wait);
        if (!result.conflicts.isEmpty()) {
            QMessageBox::warning(this,
                                 i18n("Fast Image Export"),
                                 i18n("Nothing was exported. More than one saved export would write:\n%1",
                                      result.conflicts.join(QLatin1Char('\n'))));
        } else if (!result.skipped.isEmpty()) {
            QMessageBox::warning(this,
                                 i18n("Fast Image Export"),
                                 i18np("Exported %1 file. Skipped:\n%2",
                                       "Exported %1 files. Skipped:\n%2",
                                       result.exported,
                                       result.skipped.join(QLatin1Char('\n'))));
        } else {
            accept();
        }
    }

    void browseExportFolder()
    {
        // The registered folder is only where the chooser starts; the folder
        // picked in it becomes the export folder.
        QString start = m_registeredFolder->currentData().toString();
        if (start.isEmpty() || !QFileInfo(start).isDir()) {
            start = m_folder->text().trimmed();
        }
        const QString folder = QFileDialog::getExistingDirectory(this, i18n("Select Export Folder"), start);
        if (!folder.isEmpty()) {
            m_folder->setText(folder);
            m_destination->setCurrentIndex(m_destination->findData(1));
        }
    }

    struct Request {
        QString outputFolder;
        QString name;
        KisLazyToolsFastExport::Settings settings;
    };

    // Resolves the destination and name shared by both export buttons. Returns
    // false, before saving preferences, when they are invalid or cancelled.
    bool prepare(Request *request)
    {
        const int destination = m_destination->currentData().toInt();
        if (destination == 1) {
            request->outputFolder = m_folder->text().trimmed();
        } else if (destination == 2) {
            request->outputFolder =
                QFileDialog::getExistingDirectory(this, i18n("Select Export Folder"), m_folder->text());
            if (request->outputFolder.isEmpty()) {
                return false;
            }
        }
        if (destination != 0 && request->outputFolder.isEmpty()) {
            QMessageBox::warning(this, i18n("Fast Image Export"), i18n("Select an export folder first."));
            return false;
        }

        request->name = m_scope->currentData().toBool()
            ? QString()
            : KisLazyToolsFastExport::normalizedBaseName(m_name->text(), m_format->currentData().toString());
        if (!request->name.isEmpty() && !KisLazyToolsFastExport::isValidBaseName(request->name)) {
            QMessageBox::warning(this,
                                 i18n("Fast Image Export"),
                                 i18n("The name cannot contain \\ / : * ? \" < > | or end with a dot or a space."));
            return false;
        }

        request->settings.folder = m_folder->text().trimmed();
        request->settings.pngCompression = m_pngCompression->value();
        request->settings.pngAlpha = m_pngAlpha->isChecked();
        request->settings.jpegQuality = m_jpegQuality->value();
        request->settings.destination = destination;
        request->settings.save();
        return true;
    }

    void exportRegion()
    {
        Request request;
        if (!prepare(&request)) {
            return;
        }
        const QList<QPointer<KisDocument>> documents = m_documents(false);
        const auto result =
            KisLazyToolsFastExport::exportRegion(documents.isEmpty() ? nullptr : documents.first().data(),
                                                 m_format->currentData().toString(),
                                                 request.outputFolder,
                                                 request.settings,
                                                 m_wait,
                                                 request.name);
        switch (result) {
        case KisLazyToolsFastExport::RegionResult::Exported:
            accept();
            return;
        case KisLazyToolsFastExport::RegionResult::Unnamed:
            QMessageBox::warning(this,
                                 i18n("Fast Image Export"),
                                 i18n("Save the active document before exporting a region."));
            return;
        case KisLazyToolsFastExport::RegionResult::NoSelection:
            QMessageBox::warning(this, i18n("Fast Image Export"), i18n("Select an area to export first."));
            return;
        case KisLazyToolsFastExport::RegionResult::Failed:
            QMessageBox::warning(this, i18n("Fast Image Export"), i18n("Could not export the selected area."));
            return;
        }
    }

    void exportImages()
    {
        Request request;
        if (!prepare(&request)) {
            return;
        }
        const auto result = KisLazyToolsFastExport::run(m_documents(m_scope->currentData().toBool()),
                                                        m_format->currentData().toString(),
                                                        request.outputFolder,
                                                        request.settings,
                                                        m_wait,
                                                        request.name);
        const int exported = result.exported;
        const QStringList &skipped = result.skipped;

        if (!skipped.isEmpty()) {
            QMessageBox::warning(this,
                                 i18n("Fast Image Export"),
                                 i18np("Exported %1 document. Skipped: %2",
                                       "Exported %1 documents. Skipped: %2",
                                       exported,
                                       skipped.join(QStringLiteral(", "))));
        } else {
            accept();
        }
    }

    KisLazyToolsFastExportDialog::Documents m_documents;
    KisLazyToolsFastExport::WaitForImage m_wait;
    QComboBox *m_format{nullptr};
    QComboBox *m_scope{nullptr};
    QComboBox *m_destination{nullptr};
    QComboBox *m_registeredFolder{nullptr};
    QLineEdit *m_folder{nullptr};
    QLineEdit *m_name{nullptr};
    QSpinBox *m_pngCompression{nullptr};
    QCheckBox *m_pngAlpha{nullptr};
    QSpinBox *m_jpegQuality{nullptr};
    QComboBox *m_area{nullptr};
    QCheckBox *m_relative{nullptr};
    QComboBox *m_layerComp{nullptr};
    QPushButton *m_clearName{nullptr};
    QPushButton *m_exportButton{nullptr};
    QPushButton *m_regionButton{nullptr};

    QPointer<KisDocument> m_presetDocument;
    QGroupBox *m_presetGroup{nullptr};
    QListWidget *m_presetList{nullptr};
    QLabel *m_presetNote{nullptr};
    QPushButton *m_addPreset{nullptr};
    QPushButton *m_duplicatePreset{nullptr};
    QPushButton *m_renamePreset{nullptr};
    QPushButton *m_removePreset{nullptr};
    QPushButton *m_moveUp{nullptr};
    QPushButton *m_moveDown{nullptr};
    QPushButton *m_exportChecked{nullptr};
    QPushButton *m_exportAll{nullptr};
    // The saved export the form edits; empty for a one-off export.
    QString m_editingId;
    // The one-off settings, kept while a saved export is edited.
    FormState m_oneOff;
    bool m_loading{false};
};

} // namespace

QDialog *KisLazyToolsFastExportDialog::create(QWidget *parent,
                                              const Documents &documents,
                                              const KisLazyToolsFastExport::WaitForImage &wait,
                                              KisDocument *presetDocument)
{
    return new FastExportDialog(parent, documents, wait, presetDocument);
}
