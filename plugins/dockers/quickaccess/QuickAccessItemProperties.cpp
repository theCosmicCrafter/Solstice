/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessItemProperties.h"
#include "QuickAccessItemPresentation.h"
#include "QuickAccessLayoutEngine.h"
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <klocalizedstring.h>
using namespace QuickAccessItemPresentation;
namespace
{
QPushButton *createColorButton(const QColor &initial, QWidget *parent)
{
    auto *button = new QPushButton(parent);
    const auto setColor = [button](const QColor &color) {
        button->setProperty("quickAccessColor", color.name(QColor::HexArgb));
        button->setText(color.name(QColor::HexArgb));
        button->setStyleSheet(
            QStringLiteral("background-color: %1; border: 1px solid #888;").arg(color.name(QColor::HexArgb)));
    };
    setColor(initial.isValid() ? initial : QColor(Qt::white));
    QObject::connect(button, &QPushButton::clicked, button, [button, setColor]() {
        const QColor current(button->property("quickAccessColor").toString());
        const QColor chosen = QColorDialog::getColor(current, button->window(), i18nc("@title:window", "Select Color"));
        if (chosen.isValid())
            setColor(chosen);
    });
    return button;
}

QColor buttonColor(const QPushButton *button)
{
    return QColor(button->property("quickAccessColor").toString());
}
} // namespace

bool QuickAccessItemProperties::edit(QuickAccess::Document &document,
                                     QuickAccess::Grid &activeGrid,
                                     const QString &itemId,
                                     const QString &displayText,
                                     QWidget *parent)
{
    QuickAccess::Grid *grid = &activeGrid;
    auto itemIt = std::find_if(grid->items.begin(), grid->items.end(), [&itemId](const QuickAccess::Item &item) {
        return item.id == itemId;
    });
    if (itemIt == grid->items.end() || itemIt->type == QuickAccess::ItemType::Brush)
        return false;

    QuickAccess::Item &item = *itemIt;
    QDialog dialog(parent);
    dialog.setWindowTitle(i18nc("@title:window", "Quick Access Item Properties"));
    dialog.setMinimumWidth(320);
    auto *root = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    root->addLayout(form);

    QLineEdit *textEdit = nullptr;
    QSpinBox *valueSpin = nullptr;
    QSpinBox *fontSize = nullptr;
    QPushButton *background = nullptr;
    QPushButton *foreground = nullptr;
    QPushButton *swatch = nullptr;
    QLineEdit *iconEdit = nullptr;
    QLineEdit *scriptEdit = nullptr;
    QString aliasCategory;
    QString aliasId;
    QJsonObject values = item.payload;

    if (item.type == QuickAccess::ItemType::Action) {
        aliasCategory = QStringLiteral("actions");
        aliasId = item.payload.value(QStringLiteral("action_id")).toString();
        values = aliasFor(document, aliasCategory, aliasId);
    } else if (item.type == QuickAccess::ItemType::DockerToggle) {
        aliasCategory = QStringLiteral("dockers");
        aliasId = item.payload.value(QStringLiteral("docker_id")).toString();
        values = aliasFor(document, aliasCategory, aliasId);
    }

    const auto addText = [&](const QString &label, const QString &value) {
        textEdit = new QLineEdit(value, &dialog);
        form->addRow(label, textEdit);
    };
    const auto addAppearance = [&](const QString &defaultBackground, const QString &defaultForeground) {
        fontSize = new QSpinBox(&dialog);
        fontSize->setRange(6, 96);
        fontSize->setValue(compatibleInt(values, QStringLiteral("font_size"), QStringLiteral("fontSize"), 18));
        form->addRow(i18nc("@label:spinbox", "Font size:"), fontSize);
        background = createColorButton(QColor(compatibleString(values,
                                                               QStringLiteral("background_color"),
                                                               QStringLiteral("backgroundColor"),
                                                               defaultBackground)),
                                       &dialog);
        foreground = createColorButton(
            QColor(
                compatibleString(values, QStringLiteral("font_color"), QStringLiteral("fontColor"), defaultForeground)),
            &dialog);
        form->addRow(i18nc("@label", "Background color:"), background);
        form->addRow(i18nc("@label", "Text color:"), foreground);
    };
    const auto addPath = [&](const QString &label,
                             const QString &buttonText,
                             QLineEdit **field,
                             const QString &value,
                             const QString &filter,
                             bool browseBundledIcons = false) {
        auto *container = new QWidget(&dialog);
        auto *layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        *field = new QLineEdit(value, container);
        auto *browse = new QPushButton(buttonText, container);
        layout->addWidget(*field);
        layout->addWidget(browse);
        form->addRow(label, container);
        QObject::connect(browse, &QPushButton::clicked, &dialog, [field, filter, browseBundledIcons, &dialog]() {
            QString initialPath = (*field)->text();
            if (browseBundledIcons && !QFileInfo(initialPath).isAbsolute()) {
                const QString bundledPath = QDir(defaultIconsDirectory()).filePath(QFileInfo(initialPath).fileName());
                initialPath = QFileInfo::exists(bundledPath) ? bundledPath : defaultIconsDirectory();
            }
            const QString selected =
                QFileDialog::getOpenFileName(&dialog, i18nc("@title:window", "Select File"), initialPath, filter);
            if (selected.isEmpty())
                return;
            if (browseBundledIcons
                && QDir::cleanPath(QFileInfo(selected).absolutePath())
                    == QDir::cleanPath(QFileInfo(defaultIconsDirectory()).absoluteFilePath())) {
                (*field)->setText(QFileInfo(selected).fileName());
            } else {
                (*field)->setText(selected);
            }
        });
    };

    switch (item.type) {
    case QuickAccess::ItemType::Action:
    case QuickAccess::ItemType::DockerToggle:
        form->addRow(i18nc("@label", "Identifier:"), new QLabel(aliasId, &dialog));
        addText(i18nc("@label:textbox", "Button name:"),
                compatibleString(values, QStringLiteral("custom_name"), QStringLiteral("customName"), displayText));
        addAppearance(item.type == QuickAccess::ItemType::Action ? QStringLiteral("#3a263f")
                                                                 : QStringLiteral("#263a2f"),
                      QStringLiteral("#ffffff"));
        addPath(i18nc("@label:textbox", "Icon:"),
                i18nc("@action:button", "Browse…"),
                &iconEdit,
                values.value(QStringLiteral("icon_name")).toString(),
                i18nc("@item:inlistbox", "Images (*.png *.jpg *.jpeg *.bmp *.gif);;All Files (*)"),
                true);
        break;
    case QuickAccess::ItemType::Label:
        addText(i18nc("@label:textbox", "Label name:"),
                item.payload.value(QStringLiteral("text")).toString(QStringLiteral("Label")));
        addAppearance(QStringLiteral("#263746"), QStringLiteral("#4fc3f7"));
        break;
    case QuickAccess::ItemType::Separator:
        valueSpin = new QSpinBox(&dialog);
        valueSpin->setRange(1, 50);
        valueSpin->setValue(qMax(1, item.payload.value(QStringLiteral("thickness")).toVariant().toInt()));
        if (!item.payload.contains(QStringLiteral("thickness")))
            valueSpin->setValue(2);
        form->addRow(i18nc("@label:spinbox", "Thickness:"), valueSpin);
        swatch =
            createColorButton(QColor(item.payload.value(QStringLiteral("color")).toString(QStringLiteral("#5a5a5a"))),
                              &dialog);
        form->addRow(i18nc("@label", "Color:"), swatch);
        break;
    case QuickAccess::ItemType::Color:
        swatch =
            createColorButton(QColor(item.payload.value(QStringLiteral("color")).toString(QStringLiteral("#ffffff"))),
                              &dialog);
        form->addRow(i18nc("@label", "Color:"), swatch);
        break;
    case QuickAccess::ItemType::BrushSize:
        valueSpin = new QSpinBox(&dialog);
        valueSpin->setRange(1, 10000);
        valueSpin->setValue(qMax(1, item.payload.value(QStringLiteral("text")).toString().toInt()));
        form->addRow(i18nc("@label:spinbox", "Size:"), valueSpin);
        addAppearance(QStringLiteral("#3a263f"), QStringLiteral("#ffffff"));
        break;
    case QuickAccess::ItemType::BrushBlendMode:
        addText(i18nc("@label:textbox", "Blending mode:"), item.payload.value(QStringLiteral("text")).toString());
        addAppearance(QStringLiteral("#263a3a"), QStringLiteral("#ffffff"));
        break;
    case QuickAccess::ItemType::Script:
        addText(i18nc("@label:textbox", "Button name:"),
                compatibleString(item.payload,
                                 QStringLiteral("custom_name"),
                                 QStringLiteral("customName"),
                                 i18n("Script")));
        addPath(i18nc("@label:textbox", "Script:"),
                i18nc("@action:button", "Browse…"),
                &scriptEdit,
                item.payload.value(QStringLiteral("script_path")).toString(),
                i18nc("@item:inlistbox", "Python scripts (*.py)"));
        addPath(i18nc("@label:textbox", "Icon:"),
                i18nc("@action:button", "Browse…"),
                &iconEdit,
                item.payload.value(QStringLiteral("icon_name")).toString(),
                i18nc("@item:inlistbox", "Images (*.png *.jpg *.jpeg *.bmp *.gif);;All Files (*)"),
                true);
        break;
    case QuickAccess::ItemType::Brush:
        return false;
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    if (!aliasCategory.isEmpty()) {
        QJsonObject alias = values;
        alias.insert(QStringLiteral("custom_name"), textEdit->text().trimmed());
        alias.insert(QStringLiteral("font_size"), QString::number(fontSize->value()));
        alias.insert(QStringLiteral("background_color"), buttonColor(background).name(QColor::HexArgb));
        alias.insert(QStringLiteral("font_color"), buttonColor(foreground).name(QColor::HexArgb));
        alias.insert(QStringLiteral("icon_name"), iconEdit->text().trimmed());
        QJsonObject aliases = document.aliases.value(aliasCategory).toObject();
        aliases.insert(aliasId, alias);
        document.aliases.insert(aliasCategory, aliases);
        // An item drawn as an icon takes one cell, including when an icon is
        // added to an item that was wider.
        normalizeIconItemSpans(&document);
    } else {
        if (textEdit)
            item.payload.insert(item.type == QuickAccess::ItemType::Script ? QStringLiteral("custom_name")
                                                                           : QStringLiteral("text"),
                                textEdit->text().trimmed());
        if (fontSize) {
            item.payload.insert(QStringLiteral("fontSize"), QString::number(fontSize->value()));
            item.payload.insert(QStringLiteral("backgroundColor"), buttonColor(background).name(QColor::HexArgb));
            item.payload.insert(QStringLiteral("fontColor"), buttonColor(foreground).name(QColor::HexArgb));
        }
        if (item.type == QuickAccess::ItemType::Separator) {
            item.payload.insert(QStringLiteral("thickness"), QString::number(valueSpin->value()));
            item.payload.insert(QStringLiteral("color"), buttonColor(swatch).name(QColor::HexArgb));
        } else if (item.type == QuickAccess::ItemType::Color) {
            item.payload.insert(QStringLiteral("color"), buttonColor(swatch).name(QColor::HexArgb));
        } else if (item.type == QuickAccess::ItemType::BrushSize) {
            item.payload.insert(QStringLiteral("text"), QString::number(valueSpin->value()));
        } else if (item.type == QuickAccess::ItemType::Script) {
            item.payload.insert(QStringLiteral("script_path"), scriptEdit->text().trimmed());
            item.payload.insert(QStringLiteral("icon_name"), iconEdit->text().trimmed());
        }
    }
    return true;
}

bool QuickAccessItemProperties::editBrush(QuickAccess::Grid &activeGrid, const QString &itemId, QWidget *parent)
{
    QuickAccess::Grid *grid = &activeGrid;
    auto itemIt = std::find_if(grid->items.begin(), grid->items.end(), [&itemId](const QuickAccess::Item &item) {
        return item.id == itemId;
    });
    if (itemIt == grid->items.end() || itemIt->type != QuickAccess::ItemType::Brush)
        return false;

    QDialog dialog(parent);
    dialog.setWindowTitle(i18nc("@title:window", "Quick Access Item Properties"));
    dialog.setMinimumWidth(320);
    auto *root = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    root->addLayout(form);
    auto *nameLabel = new QLabel(itemIt->payload.value(QStringLiteral("brush_name")).toString(), &dialog);
    form->addRow(i18nc("@label", "Brush:"), nameLabel);
    auto *display = new QComboBox(&dialog);
    display->addItem(i18nc("@item:inlistbox", "Stroke Preview"), BrushDisplayStroke);
    display->addItem(i18nc("@item:inlistbox", "Icon"), BrushDisplayIcon);
    const bool stroke = showsStrokePreview(*itemIt);
    display->setCurrentIndex(stroke ? 0 : 1);
    display->setToolTip(i18n("A stroke preview uses two cells; an icon uses one."));
    form->addRow(i18nc("@label:listbox", "Display:"), display);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    const bool newStroke = display->currentData().toString() == BrushDisplayStroke;
    if (newStroke == stroke)
        return false;
    // A stroke preview takes two cells, an icon one; neighbours are pushed
    // aside as when an item is resized in the grid editor.
    const int columnSpan = newStroke ? qMin(2, grid->columns) : 1;
    // The display decides the span when items are normalized, so it is set
    // before resizing.
    QList<QuickAccess::Item> items = grid->items;
    for (QuickAccess::Item &item : items) {
        if (item.id == itemId)
            item.payload.insert(BrushDisplayKey, newStroke ? BrushDisplayStroke : BrushDisplayIcon);
    }
    const QuickAccess::LayoutResult result =
        QuickAccess::LayoutEngine(grid->columns).resizeItem(items, itemId, 1, columnSpan);
    if (!result.isValid()) {
        QMessageBox::warning(parent,
                             i18nc("@title:window", "Quick Access Layout"),
                             i18n("The item could not be resized in the active grid."));
        return false;
    }
    grid->items = result.items;
    return true;
}
