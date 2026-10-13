/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisSolsticeLazyTools.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

#include <kactioncollection.h>
#include <klocalizedstring.h>
#include <kundo2magicstring.h>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoResourcePaths.h>

#include "KisDocument.h"
#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisViewManager.h"
#include "commands/kis_set_global_selection_command.h"
#include "kis_action.h"
#include "kis_action_manager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_config_notifier.h"
#include "kis_group_layer.h"
#include "kis_image.h"
#include "kis_node.h"
#include "kis_node_commands_adapter.h"
#include "kis_node_manager.h"
#include "kis_node_view_color_scheme.h"
#include "kis_properties_configuration.h"
#include "kis_selection.h"
#include "kis_selection_mask.h"
#include "kis_undo_adapter.h"
#include "utils/KisMenuMnemonicFilter.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{

constexpr int HotkeyId = 0x534F4C43;

KisNodeSP findNodeByName(const KisNodeSP &parent, const QString &name)
{
    if (!parent) {
        return {};
    }
    for (KisNodeSP child = parent->firstChild(); child; child = child->nextSibling()) {
        if (child->name() == name) {
            return child;
        }
        if (KisNodeSP result = findNodeByName(child, name)) {
            return result;
        }
    }
    return {};
}

class ScreenColorHotkeyFilter : public QAbstractNativeEventFilter
{
public:
    static ScreenColorHotkeyFilter *instance()
    {
        static ScreenColorHotkeyFilter filter;
        return &filter;
    }

    void updateRegistration()
    {
#ifdef Q_OS_WIN
        const bool shouldRegister = KisConfig(true).readEntry<bool>("Solstice/ColorPickFromAnywhere", true);
        if (shouldRegister == m_registered) {
            return;
        }
        if (m_registered) {
            UnregisterHotKey(nullptr, HotkeyId);
            m_registered = false;
        }
        if (shouldRegister) {
            m_registered = RegisterHotKey(nullptr, HotkeyId, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'C');
        }
#endif
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    bool nativeEventFilter(const QByteArray &eventType, void *message, long *result) override
#else
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override
#endif
    {
        Q_UNUSED(eventType);
        Q_UNUSED(result);
#ifdef Q_OS_WIN
        MSG *msg = static_cast<MSG *>(message);
        if (msg && msg->message == WM_HOTKEY && msg->wParam == HotkeyId) {
            if (KisMainWindow *window = KisPart::instance()->currentMainwindow()) {
                if (QAction *action = window->actionCollection()->action(QStringLiteral("screen_color_picker"))) {
                    action->trigger();
                }
            }
            return true;
        }
#else
        Q_UNUSED(message);
#endif
        return false;
    }

private:
    ScreenColorHotkeyFilter()
    {
        qApp->installNativeEventFilter(this);
        updateRegistration();
    }

    ~ScreenColorHotkeyFilter() override
    {
#ifdef Q_OS_WIN
        if (m_registered) {
            UnregisterHotKey(nullptr, HotkeyId);
        }
#endif
    }

    bool m_registered{false};
};

QString foregroundColorConfigKey(int slot)
{
    return QStringLiteral("Solstice/ForegroundColor%1").arg(slot);
}

QColor defaultForegroundColor(int slot)
{
    static const QColor colors[] = {QColor(Qt::black),
                                    QColor(Qt::white),
                                    QColor(238, 50, 51),
                                    QColor(255, 170, 63),
                                    QColor(247, 229, 61),
                                    QColor(151, 202, 63),
                                    QColor(91, 173, 220),
                                    QColor(191, 106, 209),
                                    QColor(118, 119, 114)};
    return colors[qBound(1, slot, 9) - 1];
}

int colorLabelIndex(const QString &name)
{
    static const QHash<QString, int> labels = {
        {QStringLiteral("blue"), 1},
        {QStringLiteral("green"), 2},
        {QStringLiteral("yellow"), 3},
        {QStringLiteral("orange"), 4},
        {QStringLiteral("brown"), 5},
        {QStringLiteral("red"), 6},
        {QStringLiteral("purple"), 7},
        {QStringLiteral("grey"), 8},
        {QStringLiteral("gray"), 8},
    };
    return labels.value(name.trimmed().toLower(), 0);
}

QString colorLabelName(int index)
{
    static const QStringList names = {QString(),
                                      QStringLiteral("Blue"),
                                      QStringLiteral("Green"),
                                      QStringLiteral("Yellow"),
                                      QStringLiteral("Orange"),
                                      QStringLiteral("Brown"),
                                      QStringLiteral("Red"),
                                      QStringLiteral("Purple"),
                                      QStringLiteral("Grey")};
    return names.value(index);
}

QString renamePresetPath()
{
    return QDir(KoResourcePaths::getAppDataLocation())
        .filePath(QStringLiteral("lazy_tools/config/name_color_list.txt"));
}

class RenameLayerDialog : public QDialog
{
public:
    explicit RenameLayerDialog(KisViewManager *viewManager)
        : QDialog(viewManager->mainWindowAsQWidget())
        , m_viewManager(viewManager)
    {
        setWindowTitle(i18n("Rename Layer"));
        setMinimumSize(240, 200);

        KisConfig cfg(true);
        const int columns = qMax(1, cfg.readEntry<int>("Solstice/RenameGridColumns", 3));
        resize(qMax(240, cfg.readEntry<int>("Solstice/RenameDialogWidth", 500)),
               qMax(200, cfg.readEntry<int>("Solstice/RenameDialogHeight", 800)));

        auto *layout = new QVBoxLayout(this);
        auto *scrollArea = new QScrollArea(this);
        scrollArea->setWidgetResizable(true);
        auto *presetsWidget = new QWidget(scrollArea);
        auto *grid = new QGridLayout(presetsWidget);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(4);
        grid->setAlignment(Qt::AlignTop | Qt::AlignLeft);

        QFile presetFile(renamePresetPath());
        if (presetFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            int buttonIndex = 0;
            QTextStream stream(&presetFile);
            while (!stream.atEnd()) {
                const QString line = stream.readLine().trimmed();
                if (line.isEmpty()) {
                    continue;
                }
                const qsizetype comma = line.indexOf(QLatin1Char(','));
                const QString name = (comma < 0 ? line : line.left(comma)).trimmed();
                const int label = comma < 0 ? 0 : colorLabelIndex(line.mid(comma + 1));
                if (name.isEmpty()) {
                    continue;
                }
                auto *button = new QPushButton(name, presetsWidget);
                button->setMinimumHeight(28);
                if (label > 0) {
                    QPixmap pixmap(14, 14);
                    pixmap.fill(KisNodeViewColorScheme::instance()->colorFromLabelIndex(label));
                    button->setIcon(QIcon(pixmap));
                }
                connect(button, &QPushButton::clicked, this, [this, name, label] {
                    apply(name, label);
                });
                grid->addWidget(button, buttonIndex / columns, buttonIndex % columns);
                ++buttonIndex;
            }
            for (int column = 0; column < columns; ++column) {
                grid->setColumnStretch(column, 1);
            }
        }
        scrollArea->setWidget(presetsWidget);
        layout->addWidget(scrollArea);

        auto *manualLayout = new QHBoxLayout;
        m_colorCombo = new QComboBox(this);
        const QVector<QColor> colors = KisNodeViewColorScheme::instance()->allColorLabels();
        for (int index = 0; index <= 8; ++index) {
            QPixmap pixmap(14, 14);
            pixmap.fill(index == 0 ? Qt::white : colors.value(index));
            m_colorCombo->addItem(QIcon(pixmap), QString(), index);
            m_colorCombo->setItemData(index, index == 0 ? i18n("None") : colorLabelName(index), Qt::ToolTipRole);
        }
        m_nameInput = new QLineEdit(this);
        m_nameInput->setPlaceholderText(i18n("Layer name"));
        m_savePreset = new QCheckBox(i18n("Save"), this);
        manualLayout->addWidget(m_colorCombo);
        manualLayout->addWidget(m_nameInput);
        manualLayout->addWidget(m_savePreset);
        layout->addLayout(manualLayout);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            const QString name = m_nameInput->text().trimmed();
            if (name.isEmpty()) {
                return;
            }
            const int label = m_colorCombo->currentData().toInt();
            if (m_savePreset->isChecked()) {
                savePreset(name, label);
            }
            apply(name, label);
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_nameInput, &QLineEdit::returnPressed, buttons->button(QDialogButtonBox::Ok), &QPushButton::click);
        m_nameInput->setFocus();
    }

    ~RenameLayerDialog() override
    {
        KisConfig cfg(false);
        cfg.writeEntry("Solstice/RenameDialogWidth", width());
        cfg.writeEntry("Solstice/RenameDialogHeight", height());
    }

private:
    void apply(const QString &name, int label)
    {
        KisNodeSP node = m_viewManager->activeNode();
        if (!node || name.isEmpty()) {
            return;
        }
        m_viewManager->nodeManager()->setNodeName(node, name);
        node->setColorLabelIndex(label);
        accept();
    }

    void savePreset(const QString &name, int label)
    {
        const QString path = renamePresetPath();
        QDir().mkpath(QFileInfo(path).absolutePath());

        QStringList lines;
        QFile input(path);
        if (input.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream stream(&input);
            while (!stream.atEnd()) {
                lines << stream.readLine();
            }
        }
        const QString newLine = label > 0 ? QStringLiteral("%1, %2").arg(name, colorLabelName(label)) : name;
        if (!lines.contains(newLine)) {
            lines << newLine;
            QFile output(path);
            if (output.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                QTextStream stream(&output);
                stream << lines.join(QLatin1Char('\n'));
            }
        }
    }

    KisViewManager *m_viewManager{nullptr};
    QComboBox *m_colorCombo{nullptr};
    QLineEdit *m_nameInput{nullptr};
    QCheckBox *m_savePreset{nullptr};
};

class SelectionMaskPopup : public QDialog
{
public:
    SelectionMaskPopup(KisViewManager *viewManager, const std::function<void()> &createMask, QWidget *parent)
        : QDialog(parent)
        , m_viewManager(viewManager)
        , m_createMask(createMask)
    {
        setWindowTitle(i18n("Selection Masks"));
        setAttribute(Qt::WA_DeleteOnClose);

        auto *layout = new QVBoxLayout(this);
        auto *buttonLayout = new QHBoxLayout;
        auto *createButton = new QPushButton(i18n("Create Mask"), this);
        auto *refreshButton = new QPushButton(i18n("Refresh"), this);
        buttonLayout->addWidget(createButton);
        buttonLayout->addStretch();
        buttonLayout->addWidget(refreshButton);
        layout->addLayout(buttonLayout);

        m_scrollArea = new QScrollArea(this);
        m_scrollArea->setWidgetResizable(true);
        layout->addWidget(m_scrollArea);

        connect(createButton, &QPushButton::clicked, this, [this] {
            m_createMask();
            rebuild();
        });
        connect(refreshButton, &QPushButton::clicked, this, [this] {
            rebuild();
        });
        rebuild();
    }

private:
    void rebuild()
    {
        QWidget *container = new QWidget;
        auto *grid = new QGridLayout(container);
        grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        grid->setSpacing(10);

        KisImageSP image = m_viewManager->image();
        KisNodeSP group = image ? findNodeByName(image->root(), QStringLiteral("Selection_Mask_Group")) : KisNodeSP();
        int index = 0;
        if (group) {
            for (KisNodeSP node = group->firstChild(); node; node = node->nextSibling()) {
                KisSelectionMaskSP mask = dynamic_cast<KisSelectionMask *>(node.data());
                if (!mask) {
                    continue;
                }
                auto *button = new QToolButton(container);
                button->setFixedSize(64, 64);
                button->setIcon(QPixmap::fromImage(
                    mask->createThumbnail(64, 64, Qt::KeepAspectRatio, KisThumbnailBoundsMode::Precise)));
                button->setIconSize(QSize(60, 60));
                button->setToolTip(mask->name());
                connect(button, &QToolButton::clicked, this, [this, mask] {
                    if (KisImageSP image = m_viewManager->image()) {
                        m_viewManager->undoAdapter()->addCommand(
                            new KisSetGlobalSelectionCommand(image, mask->selection()));
                    }
                });
                grid->addWidget(button, index / 6, index % 6);
                ++index;
            }
        }

        if (!image || !group || index == 0) {
            auto *label =
                new QLabel(!image ? i18n("No active document")
                                  : (!group ? i18n("No saved selections found") : i18n("No selection masks found")),
                           container);
            label->setAlignment(Qt::AlignCenter);
            grid->addWidget(label, 0, 0, 1, 6);
        }

        QWidget *old = m_scrollArea->takeWidget();
        m_scrollArea->setWidget(container);
        delete old;
        resize(qMin(600, qMax(400, qMin(index, 6) * 74 + 36)), qMin(700, qMax(200, ((index + 5) / 6) * 74 + 100)));
    }

    KisViewManager *m_viewManager{nullptr};
    std::function<void()> m_createMask;
    QScrollArea *m_scrollArea{nullptr};
};

class FastExportDialog : public QDialog
{
public:
    explicit FastExportDialog(KisViewManager *viewManager)
        : QDialog(viewManager->mainWindowAsQWidget())
        , m_viewManager(viewManager)
    {
        setWindowTitle(i18n("Fast Image Export"));
        setAttribute(Qt::WA_DeleteOnClose);

        KisConfig cfg(true);
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
        m_folder = new QLineEdit(cfg.readEntry<QString>("Solstice/FastExportFolder", QString()), this);
        auto *browse = new QPushButton(i18n("Browse…"), this);
        auto *folderRow = new QHBoxLayout;
        folderRow->addWidget(m_folder);
        folderRow->addWidget(browse);
        m_pngCompression = new QSpinBox(this);
        m_pngCompression->setRange(0, 9);
        m_pngCompression->setValue(cfg.readEntry<int>("Solstice/FastExportPngCompression", 6));
        m_pngAlpha = new QCheckBox(i18n("Save alpha channel"), this);
        m_pngAlpha->setChecked(cfg.readEntry<bool>("Solstice/FastExportPngAlpha", true));
        m_jpegQuality = new QSpinBox(this);
        m_jpegQuality->setRange(0, 100);
        m_jpegQuality->setValue(cfg.readEntry<int>("Solstice/FastExportJpegQuality", 90));
        form->addRow(i18n("Format:"), m_format);
        form->addRow(i18n("Documents:"), m_scope);
        form->addRow(i18n("Destination:"), m_destination);
        form->addRow(i18n("Export folder:"), folderRow);
        form->addRow(i18n("PNG compression:"), m_pngCompression);
        form->addRow(QString(), m_pngAlpha);
        form->addRow(i18n("JPEG quality:"), m_jpegQuality);
        layout->addLayout(form);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        QPushButton *exportButton = buttons->addButton(i18n("Export"), QDialogButtonBox::AcceptRole);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString folder =
                QFileDialog::getExistingDirectory(this, i18n("Select Export Folder"), m_folder->text());
            if (!folder.isEmpty()) {
                m_folder->setText(folder);
            }
        });
        connect(exportButton, &QPushButton::clicked, this, [this] {
            exportImages();
        });
    }

private:
    void exportImages()
    {
        QString outputFolder;
        const int destination = m_destination->currentData().toInt();
        if (destination == 1) {
            outputFolder = m_folder->text().trimmed();
        } else if (destination == 2) {
            outputFolder = QFileDialog::getExistingDirectory(this, i18n("Select Export Folder"), m_folder->text());
            if (outputFolder.isEmpty()) {
                return;
            }
        }
        if (destination != 0 && outputFolder.isEmpty()) {
            QMessageBox::warning(this, i18n("Fast Image Export"), i18n("Select an export folder first."));
            return;
        }

        KisConfig cfg(false);
        cfg.writeEntry("Solstice/FastExportFolder", m_folder->text().trimmed());
        cfg.writeEntry("Solstice/FastExportPngCompression", m_pngCompression->value());
        cfg.writeEntry("Solstice/FastExportPngAlpha", m_pngAlpha->isChecked());
        cfg.writeEntry("Solstice/FastExportJpegQuality", m_jpegQuality->value());

        QList<QPointer<KisDocument>> documents;
        if (m_scope->currentData().toBool()) {
            documents = KisPart::instance()->documents();
        } else if (KisDocument *document = m_viewManager->document()) {
            documents << document;
        }

        const QString extension = m_format->currentData().toString();
        const QByteArray mimeType =
            extension == QLatin1String("png") ? QByteArray("image/png") : QByteArray("image/jpeg");
        int exported = 0;
        QStringList skipped;
        for (const QPointer<KisDocument> &document : documents) {
            if (!document || document->localFilePath().isEmpty()) {
                skipped << (document ? document->caption() : i18n("Unknown document"));
                continue;
            }
            const QFileInfo source(document->localFilePath());
            const QString folder = destination == 0 ? source.absolutePath() : outputFolder;
            const QString path = QDir(folder).filePath(source.completeBaseName() + QLatin1Char('.') + extension);
            KisPropertiesConfigurationSP options(new KisPropertiesConfiguration);
            if (extension == QLatin1String("png")) {
                options->setProperty("compression", m_pngCompression->value());
                options->setProperty("alpha", m_pngAlpha->isChecked());
            } else {
                options->setProperty("quality", m_jpegQuality->value());
            }
            if (m_viewManager->blockUntilOperationsFinished(document->image())
                && document->exportDocumentSync(path, mimeType, options)) {
                ++exported;
            } else {
                skipped << document->caption();
            }
        }

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

    KisViewManager *m_viewManager{nullptr};
    QComboBox *m_format{nullptr};
    QComboBox *m_scope{nullptr};
    QComboBox *m_destination{nullptr};
    QLineEdit *m_folder{nullptr};
    QSpinBox *m_pngCompression{nullptr};
    QCheckBox *m_pngAlpha{nullptr};
    QSpinBox *m_jpegQuality{nullptr};
};

} // namespace

KisSolsticeLazyTools::KisSolsticeLazyTools(KisViewManager *viewManager)
    : QObject(viewManager)
    , m_viewManager(viewManager)
{
    ScreenColorHotkeyFilter::instance();
    connect(KisConfigNotifier::instance(), &KisConfigNotifier::configChanged, this, [this] {
        applyCustomSettings();
    });
    QTimer::singleShot(0, this, [this] {
        applyCustomSettings();
    });
}

KisSolsticeLazyTools::~KisSolsticeLazyTools() = default;

void KisSolsticeLazyTools::createActions()
{
    if (!m_menuMnemonicFilter) {
        m_menuMnemonicFilter = new KisMenuMnemonicFilter(m_viewManager->qtMainWindow()->menuBar());
    }
    applyCustomSettings(); // Before XMLGUI builds menus, even if startup processes events.

    KisAction *action = m_viewManager->actionManager()->createAction("create_selection_mask_alternative");
    connect(action, &QAction::triggered, this, [this] {
        createSelectionMask();
    });
    action = m_viewManager->actionManager()->createAction("create_selection_mask_popup");
    connect(action, &QAction::triggered, this, [this] {
        showSelectionMaskPopup();
    });
    action = m_viewManager->actionManager()->createAction("fast_image_export");
    connect(action, &QAction::triggered, this, [this] {
        showFastExportDialog();
    });
    action = m_viewManager->actionManager()->createAction("rename_alternative");
    connect(action, &QAction::triggered, this, [this] {
        showRenameDialog();
    });
    action = m_viewManager->actionManager()->createAction("screen_color_picker");
    connect(action, &QAction::triggered, this, [this] {
        pickColorFromScreen();
    });
    for (int slot = 1; slot <= 9; ++slot) {
        action = m_viewManager->actionManager()->createAction(QStringLiteral("set_foreground_color%1").arg(slot));
        connect(action, &QAction::triggered, this, [this, slot] {
            setForegroundColor(slot);
        });
    }
}

void KisSolsticeLazyTools::createSelectionMask()
{
    KisImageSP image = m_viewManager->image();
    KisSelectionSP selection = m_viewManager->selection();
    if (!image || !selection || selection->selectedExactRect().isEmpty()) {
        return;
    }

    KisNodeCommandsAdapter commands(m_viewManager);
    commands.beginMacro(kundo2_i18n("Create Selection Mask Alternative"));
    KisNodeSP group = findNodeByName(image->root(), QStringLiteral("Selection_Mask_Group"));
    if (!group) {
        KisGroupLayerSP newGroup(new KisGroupLayer(image, QStringLiteral("Selection_Mask_Group"), OPACITY_OPAQUE_U8));
        newGroup->setVisible(false);
        commands.addNode(newGroup, image->root(), image->root()->firstChild());
        group = newGroup;
    }

    KisSelectionMaskSP mask(new KisSelectionMask(image));
    mask->setName(
        QStringLiteral("selection_mask_%1").arg(QDateTime::currentDateTime().toString(QStringLiteral("hhmmss"))));
    mask->setSelection(selection);
    commands.addNode(mask, group, KisNodeSP());
    commands.endMacro();

    if (QAction *deselect = m_viewManager->actionCollection()->action(QStringLiteral("deselect"))) {
        deselect->trigger();
    }
}

void KisSolsticeLazyTools::showSelectionMaskPopup()
{
    if (m_selectionMaskPopup) {
        m_selectionMaskPopup->raise();
        m_selectionMaskPopup->activateWindow();
        return;
    }
    m_selectionMaskPopup = new SelectionMaskPopup(
        m_viewManager,
        [this] {
            createSelectionMask();
        },
        m_viewManager->mainWindowAsQWidget());
    m_selectionMaskPopup->show();
}

void KisSolsticeLazyTools::setForegroundColor(int slot)
{
    KisConfig cfg(true);
    const QColor color = cfg.readEntry<QColor>(foregroundColorConfigKey(slot), defaultForegroundColor(slot));
    const KoColorSpace *colorSpace = m_viewManager->canvasResourceProvider()->fgColor().colorSpace();
    m_viewManager->canvasResourceProvider()->setFGColor(KoColor(color, colorSpace));
}

void KisSolsticeLazyTools::showFastExportDialog()
{
    (new FastExportDialog(m_viewManager))->show();
}

void KisSolsticeLazyTools::showRenameDialog()
{
    RenameLayerDialog dialog(m_viewManager);
    dialog.move(QCursor::pos());
    dialog.exec();
}

void KisSolsticeLazyTools::pickColorFromScreen()
{
    const QPoint globalPosition = QCursor::pos();
    QScreen *screen = QGuiApplication::screenAt(globalPosition);
    if (!screen) {
        return;
    }
    const QPoint localPosition = globalPosition - screen->geometry().topLeft();
    const QImage image = screen->grabWindow(0, localPosition.x(), localPosition.y(), 1, 1).toImage();
    if (image.isNull()) {
        return;
    }
    const QColor color = image.pixelColor(0, 0);
    const KoColorSpace *colorSpace = m_viewManager->canvasResourceProvider()->fgColor().colorSpace();
    m_viewManager->canvasResourceProvider()->setFGColor(KoColor(color, colorSpace));
}

void KisSolsticeLazyTools::applyCustomSettings()
{
    ScreenColorHotkeyFilter::instance()->updateRegistration();
    if (m_menuMnemonicFilter) {
        m_menuMnemonicFilter->setSuppressed(KisConfig(true).readEntry<bool>("Solstice/DisableTopMenuShortcuts", true));
    }
}
