/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "KisReferenceImageFilters.h"
#include "KisDocument.h"
#include "KisReferenceImage.h"
#include "KisReferenceFilterTemplates.h"
#include "KisViewManager.h"
#include "flake/KisReferenceImagesLayer.h"
#include "widgets/kis_filter_selector_widget.h"
#include <KisGlobalResourcesInterface.h>
#include <KoCanvasBase.h>
#include <KoColorSpaceRegistry.h>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <filter/kis_filter.h>
#include <filter/kis_filter_registry.h>
#include <kis_paint_device.h>

namespace
{
using Effects = QVector<KisReferenceImage::FilterEffect>;
class SetFiltersCommand : public KUndo2Command
{
public:
    SetFiltersCommand(KisReferenceImage *image, Effects effects, bool enabled)
        : KUndo2Command(kundo2_i18n("Change reference image filters"))
        , m_image(image)
        , m_before(image->filterEffects())
        , m_after(effects)
        , m_beforeEnabled(image->filtersEnabled())
        , m_afterEnabled(enabled)
    {
    }
    void undo() override
    {
        m_image->setFilterEffects(m_before, m_beforeEnabled);
    }
    void redo() override
    {
        m_image->setFilterEffects(m_after, m_afterEnabled);
    }

private:
    KisReferenceImage *m_image;
    Effects m_before, m_after;
    bool m_beforeEnabled, m_afterEnabled;
};
QString effectName(const KisReferenceImage::FilterEffect &effect)
{
    auto filter = KisFilterRegistry::instance()->value(effect.id);
    return filter ? filter->name() : i18n("Unavailable filter: %1", effect.id);
}
void saveTemplate(QWidget *parent, const Effects &effects, bool enabled)
{
    bool accepted = false;
    const QString name = QInputDialog::getText(parent,
                                               i18n("Save Filter Template"),
                                               i18n("Template name:"),
                                               QLineEdit::Normal,
                                               QString(),
                                               &accepted)
                             .trimmed();
    if (!accepted)
        return;
    KisReferenceFilterTemplates store;
    const bool overwrite = store.contains(name);
    if (overwrite
        && QMessageBox::question(parent,
                                 i18n("Save Filter Template"),
                                 i18n("Replace the filter template “%1”?", name),
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No)
            != QMessageBox::Yes)
        return;
    QString error;
    if (!store.save({name, effects, enabled}, overwrite, &error))
        QMessageBox::warning(parent, i18n("Save Filter Template"), error);
}
bool loadTemplate(QWidget *parent, const QString &name, KisReferenceFilterTemplates::Template *value)
{
    QString error;
    if (KisReferenceFilterTemplates().load(name, value, &error))
        return true;
    QMessageBox::warning(parent, i18n("Apply Filter Template"), error);
    return false;
}
void editFilters(KisReferenceImage *image, KoCanvasBase *canvas, KisViewManager *view, int selected, bool adding)
{
    auto layer = view->document()->referenceImagesLayer();
    const Effects before = image->filterEffects();
    const bool beforeEnabled = image->filtersEnabled();
    Effects effects = before;
    QDialog dialog(view->mainWindowAsQWidget());
    dialog.setWindowTitle(i18n("Reference Image Filters"));
    dialog.resize(850, 580);
    auto *layout = new QVBoxLayout(&dialog);
    auto *enabled = new QCheckBox(i18n("Enable Filters"), &dialog);
    enabled->setChecked(beforeEnabled);
    layout->addWidget(enabled);
    auto *body = new QHBoxLayout();
    layout->addLayout(body);
    auto *left = new QVBoxLayout();
    body->addLayout(left);
    auto *list = new QListWidget(&dialog);
    left->addWidget(list);
    auto *selector = new KisFilterSelectorWidget(&dialog);
    selector->setView(view);
    KisPaintDeviceSP device = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    device->convertFromQImage(image->getImage(), KoColorSpaceRegistry::instance()->p709SRGBProfile());
    selector->setPaintDevice(false, device);
    body->addWidget(selector, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QTimer preview;
    preview.setSingleShot(true);
    preview.setInterval(150);
    QObject::connect(&preview, &QTimer::timeout, &dialog, [&]() {
        image->previewFilterEffects(effects, enabled->isChecked());
    });
    bool loading = false, addMode = false;
    auto refresh = [&](int row) {
        QSignalBlocker blocker(list);
        list->clear();
        for (const auto &effect : effects) {
            auto *item = new QListWidgetItem(effectName(effect), list);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(effect.enabled ? Qt::Checked : Qt::Unchecked);
        }
        list->setCurrentRow(row);
    };
    auto load = [&](int row) {
        loading = true;
        addMode = false;
        selector->showFilterGallery(false);
        if (row >= 0 && row < effects.size()) {
            auto filter = KisFilterRegistry::instance()->value(effects[row].id);
            selector->setEnabled(bool(filter));
            if (filter) {
                auto config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
                config->fromXML(effects[row].configuration);
                selector->setFilter(filter, config);
            }
        } else
            selector->setEnabled(false);
        loading = false;
    };
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, load);
    QObject::connect(list, &QListWidget::itemChanged, &dialog, [&](QListWidgetItem *item) {
        effects[list->row(item)].enabled = item->checkState() == Qt::Checked;
        preview.start();
    });
    QObject::connect(enabled, &QCheckBox::toggled, &dialog, [&]() {
        preview.start();
    });
    QObject::connect(selector, &KisFilterSelectorWidget::configurationChanged, &dialog, [&]() {
        if (loading || addMode)
            return;
        const int row = list->currentRow();
        auto config = selector->configuration();
        auto filter = selector->currentFilter();
        if (row < 0 || !config || !filter || !filter->configurationAllowedForMask(config))
            return;
        effects[row].configuration = config->toXML();
        preview.start();
    });
    auto button = [&](const QString &text, auto callback) {
        auto *b = new QPushButton(text, &dialog);
        left->addWidget(b);
        QObject::connect(b, &QPushButton::clicked, &dialog, callback);
    };
    auto beginAdd = [&]() {
        loading = true;
        addMode = true;
        selector->setEnabled(true);
        selector->showFilterGallery(true);
        loading = false;
    };
    button(i18n("Add Filter…"), beginAdd);
    button(i18n("Add Selected Filter"), [&]() {
        if (!addMode)
            return;
        auto filter = selector->currentFilter();
        if (!filter)
            return;
        auto config = selector->configuration();
        if (!filter || !config || !filter->configurationAllowedForMask(config))
            return;
        effects.append({filter->id(), config->toXML(), true});
        refresh(effects.size() - 1);
        load(effects.size() - 1);
        preview.start();
    });
    button(i18n("Remove"), [&]() {
        const int row = list->currentRow();
        if (row < 0)
            return;
        effects.removeAt(row);
        const int next = qMin(row, int(effects.size()) - 1);
        refresh(next);
        load(next);
        preview.start();
    });
    auto move = [&](int direction) {
        const int row = list->currentRow(), next = row + direction;
        if (row < 0 || next < 0 || next >= effects.size())
            return;
        effects.swapItemsAt(row, next);
        refresh(next);
        load(next);
        preview.start();
    };
    button(i18n("Move Up"), [&]() {
        move(-1);
    });
    button(i18n("Move Down"), [&]() {
        move(1);
    });
    button(i18n("Save as Template…"), [&]() {
        saveTemplate(&dialog, effects, enabled->isChecked());
    });
    button(i18n("Apply Template…"), [&]() {
        const auto names = KisReferenceFilterTemplates().names();
        if (names.isEmpty()) {
            QMessageBox::information(&dialog, i18n("Apply Filter Template"), i18n("No saved filter templates."));
            return;
        }
        bool accepted = false;
        const QString name = QInputDialog::getItem(&dialog,
                                                   i18n("Apply Filter Template"),
                                                   i18n("Replace the current filter stack with:"),
                                                   names,
                                                   0,
                                                   false,
                                                   &accepted);
        KisReferenceFilterTemplates::Template value;
        if (!accepted || !loadTemplate(&dialog, name, &value))
            return;
        effects = value.effects;
        enabled->setChecked(value.enabled);
        const int row = effects.isEmpty() ? -1 : 0;
        refresh(row);
        load(row);
        preview.start();
    });
    refresh(selected);
    load(selected);
    if (adding)
        beginAdd();
    const bool accepted = dialog.exec() == QDialog::Accepted;
    preview.stop();
    image->setFilterEffects(before, beforeEnabled);
    if (accepted)
        canvas->addCommand(new SetFiltersCommand(image, effects, enabled->isChecked()));
}
} // namespace

void populateReferenceImageFilterMenu(QMenu *menu, KisReferenceImage *image, KoCanvasBase *canvas, KisViewManager *view)
{
    menu->setEnabled(image != nullptr);
    if (!image)
        return;
    auto *enabled = menu->addAction(i18n("Enable Filters"));
    enabled->setCheckable(true);
    enabled->setChecked(image->filtersEnabled());
    QObject::connect(enabled, &QAction::triggered, menu, [=](bool checked) {
        canvas->addCommand(new SetFiltersCommand(image, image->filterEffects(), checked));
    });
    menu->addSeparator();
    const auto effects = image->filterEffects();
    for (int i = 0; i < effects.size(); ++i) {
        auto *entry = menu->addMenu(QString::number(i + 1) + ". " + effectName(effects[i]));
        auto *active = entry->addAction(i18n("Enabled"));
        active->setCheckable(true);
        active->setChecked(effects[i].enabled);
        QObject::connect(active, &QAction::triggered, menu, [=](bool checked) {
            auto stack = image->filterEffects();
            stack[i].enabled = checked;
            canvas->addCommand(new SetFiltersCommand(image, stack, image->filtersEnabled()));
        });
        entry->addAction(i18n("Edit…"), menu, [=]() {
            editFilters(image, canvas, view, i, false);
        });
        auto *up = entry->addAction(i18n("Move Up"), menu, [=]() {
            auto stack = image->filterEffects();
            stack.swapItemsAt(i, i - 1);
            canvas->addCommand(new SetFiltersCommand(image, stack, image->filtersEnabled()));
        });
        up->setEnabled(i > 0);
        auto *down = entry->addAction(i18n("Move Down"), menu, [=]() {
            auto stack = image->filterEffects();
            stack.swapItemsAt(i, i + 1);
            canvas->addCommand(new SetFiltersCommand(image, stack, image->filtersEnabled()));
        });
        down->setEnabled(i + 1 < effects.size());
        entry->addAction(i18n("Remove"), menu, [=]() {
            auto stack = image->filterEffects();
            stack.removeAt(i);
            canvas->addCommand(new SetFiltersCommand(image, stack, image->filtersEnabled()));
        });
    }
    menu->addSeparator();
    menu->addAction(i18n("Add Filter…"), menu, [=]() {
        editFilters(image, canvas, view, -1, true);
    });
    menu->addAction(i18n("Manage Filters…"), menu, [=]() {
        editFilters(image, canvas, view, effects.isEmpty() ? -1 : 0, false);
    });
    menu->addAction(i18n("Clear Filters"), menu, [=]() {
        canvas->addCommand(new SetFiltersCommand(image, {}, image->filtersEnabled()));
    });
    menu->addSeparator();
    menu->addAction(i18n("Save as Template…"), menu, [=]() {
        saveTemplate(view->mainWindowAsQWidget(), image->filterEffects(), image->filtersEnabled());
    });
    auto *templates = menu->addMenu(i18n("Apply Template"));
    auto *remove = menu->addMenu(i18n("Delete Template"));
    const auto names = KisReferenceFilterTemplates().names();
    templates->setEnabled(!names.isEmpty());
    remove->setEnabled(!names.isEmpty());
    for (const auto &name : names) {
        const QString label = QString(name).replace("&", "&&");
        templates->addAction(label, menu, [=]() {
            KisReferenceFilterTemplates::Template value;
            if (loadTemplate(view->mainWindowAsQWidget(), name, &value))
                canvas->addCommand(new SetFiltersCommand(image, value.effects, value.enabled));
        });
        remove->addAction(label, menu, [=]() {
            auto *parent = view->mainWindowAsQWidget();
            if (QMessageBox::question(
                    parent,
                    i18n("Delete Template"),
                    i18n("Delete the filter template “%1”? Images already using it will not change.", name),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No)
                != QMessageBox::Yes)
                return;
            QString error;
            if (!KisReferenceFilterTemplates().remove(name, &error))
                QMessageBox::warning(parent, i18n("Delete Template"), error);
        });
    }
}
