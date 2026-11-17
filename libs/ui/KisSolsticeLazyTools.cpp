/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisSolsticeLazyTools.h"
#include "lazytools/KisLazyToolsRenameDialog.h"
#include "lazytools/KisLazyToolsFastExportDialog.h"
#include "lazytools/KisLazyToolsFastExportPresets.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QDialog>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScopedPointer>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

#include <kactioncollection.h>
#include <klocalizedstring.h>
#include <kundo2magicstring.h>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>

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
#include "kis_icon_utils.h"
#include "kis_image.h"
#include "kis_node.h"
#include "kis_node_commands_adapter.h"
#include "kis_node_manager.h"
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
    action = m_viewManager->actionManager()->createAction("fast_image_export_run_all");
    connect(action, &QAction::triggered, this, [this] {
        runSavedExports(false);
    });
    action = m_viewManager->actionManager()->createAction("fast_image_export_run_checked");
    connect(action, &QAction::triggered, this, [this] {
        runSavedExports(true);
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
    KisLazyToolsFastExportDialog::create(
        m_viewManager->mainWindowAsQWidget(),
        [viewManager = m_viewManager](bool allDocuments) {
            if (allDocuments)
                return KisPart::instance()->documents();
            QList<QPointer<KisDocument>> documents;
            if (KisDocument *document = viewManager->document())
                documents << document;
            return documents;
        },
        [viewManager = m_viewManager](KisImageSP image) {
            return viewManager->blockUntilOperationsFinished(image);
        },
        m_viewManager->document())
        ->show();
}

void KisSolsticeLazyTools::runSavedExports(bool checkedOnly)
{
    KisDocument *document = m_viewManager->document();
    if (!document) {
        return;
    }
    const auto result = KisLazyToolsFastExportPresets::run({document},
                                                           checkedOnly ? KisLazyToolsFastExportPresets::Which::Checked
                                                                       : KisLazyToolsFastExportPresets::Which::All,
                                                           [viewManager = m_viewManager](KisImageSP image) {
                                                               return viewManager->blockUntilOperationsFinished(image);
                                                           });
    QWidget *parent = m_viewManager->mainWindowAsQWidget();
    if (!result.conflicts.isEmpty()) {
        QMessageBox::warning(parent,
                             i18n("Fast Image Export"),
                             i18n("Nothing was exported. More than one saved export would write:\n%1",
                                  result.conflicts.join(QLatin1Char('\n'))));
    } else if (!result.skipped.isEmpty()) {
        QMessageBox::warning(parent,
                             i18n("Fast Image Export"),
                             i18np("Exported %1 file. Skipped:\n%2",
                                   "Exported %1 files. Skipped:\n%2",
                                   result.exported,
                                   result.skipped.join(QLatin1Char('\n'))));
    } else {
        m_viewManager->showFloatingMessage(i18np("Exported %1 file", "Exported %1 files", result.exported),
                                           KisIconUtils::loadIcon(QStringLiteral("document-export")));
    }
}

void KisSolsticeLazyTools::showRenameDialog()
{
    QScopedPointer<QDialog> dialog(
        KisLazyToolsRenameDialog::create(m_viewManager->mainWindowAsQWidget(),
                                         [viewManager = m_viewManager](const QString &name, int label) {
                                             KisNodeSP node = viewManager->activeNode();
                                             if (!node)
                                                 return false;
                                             viewManager->nodeManager()->setNodeName(node, name);
                                             node->setColorLabelIndex(label);
                                             return true;
                                         }));
    dialog->move(QCursor::pos());
    dialog->exec();
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
