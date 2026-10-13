/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessSettingsDialog.h"

#include <klocalizedstring.h>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

namespace
{
QFrame *separator(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}
} // namespace

QuickAccessSettingsDialog::QuickAccessSettingsDialog(const Settings &settings, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18nc("@title:window", "Quick Access Settings"));
    resize(settings.dialogWidth, settings.dialogHeight);
    auto *root = new QVBoxLayout(this);
    auto *page = new QWidget(this);
    auto *pageLayout = new QVBoxLayout(page);

    auto *gridGroup = new QGroupBox(i18nc("@title:group", "Grid"), page);
    auto *gridForm = new QFormLayout(gridGroup);
    m_columns = new QSpinBox(gridGroup);
    m_columns->setRange(1, 64);
    m_columns->setValue(settings.columns);
    m_dockerIconSize = new QSpinBox(gridGroup);
    m_dockerIconSize->setRange(24, 96);
    m_dockerIconSize->setSuffix(i18nc("@item:valuesuffix", " px"));
    m_dockerIconSize->setValue(settings.dockerIconSize);
    gridForm->addRow(i18nc("@label:spinbox", "Columns:"), m_columns);
    gridForm->addRow(i18nc("@label:spinbox", "Docker icon size:"), m_dockerIconSize);
    m_gestureEnabled = new QCheckBox(i18nc("@option:check", "Enable gesture recognition"), gridGroup);
    m_gestureEnabled->setChecked(settings.gestureEnabled);
    m_hueSvcEnabled = new QCheckBox(i18nc("@option:check", "Enable HueSVC docker"), gridGroup);
    m_hueSvcEnabled->setChecked(settings.hueSvcEnabled);
    m_quickAdjustEnabled = new QCheckBox(i18nc("@option:check", "Enable Quick Brush Adjustments docker"), gridGroup);
    m_quickAdjustEnabled->setChecked(settings.quickAdjustEnabled);
    gridForm->addRow(m_gestureEnabled);
    gridForm->addRow(m_hueSvcEnabled);
    gridForm->addRow(m_quickAdjustEnabled);
    pageLayout->addWidget(gridGroup);

    pageLayout->addWidget(separator(page));
    auto *headerGroup = new QGroupBox(i18nc("@title:group", "Header Buttons"), page);
    auto *headerForm = new QFormLayout(headerGroup);
    m_headerButtonColor = createColorButton(settings.headerButtonColor);
    m_headerButtonFontColor = createColorButton(settings.headerButtonFontColor);
    headerForm->addRow(i18nc("@label", "Background color:"), m_headerButtonColor);
    headerForm->addRow(i18nc("@label", "Font color:"), m_headerButtonFontColor);
    pageLayout->addWidget(headerGroup);

    auto *activeGroup = new QGroupBox(i18nc("@title:group", "Active Tab"), page);
    auto *activeForm = new QFormLayout(activeGroup);
    m_activeTabFontSize = new QSpinBox(activeGroup);
    m_activeTabFontSize->setRange(6, 24);
    m_activeTabFontSize->setSuffix(i18nc("@item:valuesuffix", " px"));
    m_activeTabFontSize->setValue(settings.activeTabFontSize);
    m_activeTabFontColor = createColorButton(settings.activeTabFontColor);
    m_activeTabBackgroundColor = createColorButton(settings.activeTabBackgroundColor);
    activeForm->addRow(i18nc("@label:spinbox", "Font size:"), m_activeTabFontSize);
    activeForm->addRow(i18nc("@label", "Font color:"), m_activeTabFontColor);
    activeForm->addRow(i18nc("@label", "Background color:"), m_activeTabBackgroundColor);
    pageLayout->addWidget(activeGroup);

    auto *inactiveGroup = new QGroupBox(i18nc("@title:group", "Other Tabs"), page);
    auto *inactiveForm = new QFormLayout(inactiveGroup);
    m_inactiveTabFontSize = new QSpinBox(inactiveGroup);
    m_inactiveTabFontSize->setRange(6, 24);
    m_inactiveTabFontSize->setSuffix(i18nc("@item:valuesuffix", " px"));
    m_inactiveTabFontSize->setValue(settings.inactiveTabFontSize);
    m_inactiveTabFontColor = createColorButton(settings.inactiveTabFontColor);
    m_inactiveTabBackgroundColor = createColorButton(settings.inactiveTabBackgroundColor);
    inactiveForm->addRow(i18nc("@label:spinbox", "Font size:"), m_inactiveTabFontSize);
    inactiveForm->addRow(i18nc("@label", "Font color:"), m_inactiveTabFontColor);
    inactiveForm->addRow(i18nc("@label", "Background color:"), m_inactiveTabBackgroundColor);
    pageLayout->addWidget(inactiveGroup);

    auto *dialogGroup = new QGroupBox(i18nc("@title:group", "Settings Dialog Size"), page);
    auto *dialogForm = new QFormLayout(dialogGroup);
    m_dialogWidth = new QSpinBox(dialogGroup);
    m_dialogWidth->setRange(280, 1200);
    m_dialogWidth->setSuffix(i18nc("@item:valuesuffix", " px"));
    m_dialogWidth->setValue(settings.dialogWidth);
    m_dialogHeight = new QSpinBox(dialogGroup);
    m_dialogHeight->setRange(200, 1200);
    m_dialogHeight->setSuffix(i18nc("@item:valuesuffix", " px"));
    m_dialogHeight->setValue(settings.dialogHeight);
    dialogForm->addRow(i18nc("@label:spinbox", "Width:"), m_dialogWidth);
    dialogForm->addRow(i18nc("@label:spinbox", "Height:"), m_dialogHeight);
    pageLayout->addWidget(dialogGroup);

    auto *popupGroup = new QGroupBox(i18nc("@title:group", "Popup and HueSVC"), page);
    auto *popupForm = new QFormLayout(popupGroup);
    m_popupIconSize = new QSpinBox(popupGroup);
    m_popupIconSize->setRange(24, 96);
    m_popupIconSize->setValue(settings.popupIconSize);
    m_rgbDisplayMode = new QComboBox(popupGroup);
    m_rgbDisplayMode->addItem(i18nc("@item:inlistbox", "Percentage"), QStringLiteral("percentage"));
    m_rgbDisplayMode->addItem(i18nc("@item:inlistbox", "0–255 values"), QStringLiteral("value"));
    m_rgbDisplayMode->setCurrentIndex(qMax(0, m_rgbDisplayMode->findData(settings.rgbDisplayMode)));
    m_huePopupWidth = new QSpinBox(popupGroup);
    m_huePopupWidth->setRange(200, 1200);
    m_huePopupWidth->setValue(settings.huePopupWidth);
    m_huePopupHeight = new QSpinBox(popupGroup);
    m_huePopupHeight->setRange(200, 1200);
    m_huePopupHeight->setValue(settings.huePopupHeight);
    m_hueControlsWidth = new QSpinBox(popupGroup);
    m_hueControlsWidth->setRange(160, 600);
    m_hueControlsWidth->setValue(settings.hueControlsWidth);
    m_hueControlsFontSize = new QSpinBox(popupGroup);
    m_hueControlsFontSize->setRange(6, 36);
    m_hueControlsFontSize->setValue(settings.hueControlsFontSize);
    popupForm->addRow(i18nc("@label:spinbox", "Palette popup icon size:"), m_popupIconSize);
    popupForm->addRow(i18nc("@label", "RGB display:"), m_rgbDisplayMode);
    popupForm->addRow(i18nc("@label:spinbox", "Selector width:"), m_huePopupWidth);
    popupForm->addRow(i18nc("@label:spinbox", "Popup height:"), m_huePopupHeight);
    popupForm->addRow(i18nc("@label:spinbox", "Controls width:"), m_hueControlsWidth);
    popupForm->addRow(i18nc("@label:spinbox", "Controls font size:"), m_hueControlsFontSize);
    pageLayout->addWidget(popupGroup);

    auto *adjustGroup = new QGroupBox(i18nc("@title:group", "Quick Brush Adjustments"), page);
    auto *adjustForm = new QFormLayout(adjustGroup);
    const auto addCheck = [adjustGroup, adjustForm](const QString &label, bool checked, QCheckBox **target) {
        *target = new QCheckBox(label, adjustGroup);
        (*target)->setChecked(checked);
        adjustForm->addRow(*target);
    };
    addCheck(i18nc("@option:check", "Show brush size slider"), settings.sizeSliderEnabled, &m_sizeSliderEnabled);
    addCheck(i18nc("@option:check", "Show brush opacity slider"),
             settings.opacitySliderEnabled,
             &m_opacitySliderEnabled);
    addCheck(i18nc("@option:check", "Show brush flow slider"), settings.flowSliderEnabled, &m_flowSliderEnabled);
    addCheck(i18nc("@option:check", "Show layer opacity slider"),
             settings.layerOpacitySliderEnabled,
             &m_layerOpacitySliderEnabled);
    addCheck(i18nc("@option:check", "Enable color history"), settings.colorHistoryEnabled, &m_colorHistoryEnabled);
    m_colorHistoryTotal = new QSpinBox(adjustGroup);
    m_colorHistoryTotal->setRange(2, 40);
    m_colorHistoryTotal->setValue(settings.colorHistoryTotal);
    m_colorHistoryIconSize = new QSpinBox(adjustGroup);
    m_colorHistoryIconSize->setRange(16, 64);
    m_colorHistoryIconSize->setValue(settings.colorHistoryIconSize);
    adjustForm->addRow(i18nc("@label:spinbox", "Color history count:"), m_colorHistoryTotal);
    adjustForm->addRow(i18nc("@label:spinbox", "Color history icon size:"), m_colorHistoryIconSize);
    addCheck(i18nc("@option:check", "Enable brush history"), settings.brushHistoryEnabled, &m_brushHistoryEnabled);
    m_brushHistoryTotal = new QSpinBox(adjustGroup);
    m_brushHistoryTotal->setRange(2, 40);
    m_brushHistoryTotal->setValue(settings.brushHistoryTotal);
    m_brushHistoryIconSize = new QSpinBox(adjustGroup);
    m_brushHistoryIconSize->setRange(16, 64);
    m_brushHistoryIconSize->setValue(settings.brushHistoryIconSize);
    adjustForm->addRow(i18nc("@label:spinbox", "Brush history count:"), m_brushHistoryTotal);
    adjustForm->addRow(i18nc("@label:spinbox", "Brush history icon size:"), m_brushHistoryIconSize);
    m_altEraseKey = new QLineEdit(settings.altEraseKey, adjustGroup);
    m_preserveAlphaKey = new QLineEdit(settings.preserveAlphaKey, adjustGroup);
    m_selectOutlineKey = new QLineEdit(settings.selectOutlineKey, adjustGroup);
    adjustForm->addRow(i18nc("@label", "Temporary eraser key:"), m_altEraseKey);
    adjustForm->addRow(i18nc("@label", "Temporary preserve-alpha key:"), m_preserveAlphaKey);
    adjustForm->addRow(i18nc("@label", "Temporary freehand-selection key:"), m_selectOutlineKey);
    addCheck(i18nc("@option:check", "Enable floating Tool Options"),
             settings.toolOptionsEnabled,
             &m_toolOptionsEnabled);
    m_toolOptionsPosition = new QComboBox(adjustGroup);
    m_toolOptionsPosition->addItem(i18nc("@item:inlistbox", "Left of docker"), QStringLiteral("left_align_top"));
    m_toolOptionsPosition->addItem(i18nc("@item:inlistbox", "Right of docker"), QStringLiteral("right_align_top"));
    m_toolOptionsPosition->addItem(i18nc("@item:inlistbox", "Below docker, left aligned"),
                                   QStringLiteral("bottom_left"));
    m_toolOptionsPosition->setCurrentIndex(qMax(0, m_toolOptionsPosition->findData(settings.toolOptionsPosition)));
    adjustForm->addRow(i18nc("@label", "Tool Options position:"), m_toolOptionsPosition);
    m_blendModes = new QTextEdit(adjustGroup);
    m_blendModes->setPlainText(settings.blendModes.join(QLatin1Char('\n')));
    m_blendModes->setPlaceholderText(i18nc("@info:placeholder", "Enter blend mode IDs, one per line"));
    m_blendModes->setMinimumHeight(100);
    m_blendModes->setMaximumHeight(150);
    adjustForm->addRow(i18nc("@label", "Blending mode list:"), m_blendModes);
    pageLayout->addWidget(adjustGroup);

    auto *brushSetGroup = new QGroupBox(i18nc("@title:group", "Temporary Brush Sets"), page);
    auto *brushSetLayout = new QVBoxLayout(brushSetGroup);
    m_tempBrushSets = new QTableWidget(brushSetGroup);
    m_tempBrushSets->setColumnCount(3);
    m_tempBrushSets->setHorizontalHeaderLabels({i18nc("@title:column", "Hold Key"),
                                                i18nc("@title:column", "Brush Preset"),
                                                i18nc("@title:column", "Size Scale")});
    m_tempBrushSets->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (const QJsonValue &value : settings.tempBrushSets) {
        const QJsonObject object = value.toObject();
        const int row = m_tempBrushSets->rowCount();
        m_tempBrushSets->insertRow(row);
        m_tempBrushSets->setItem(row, 0, new QTableWidgetItem(object.value(QStringLiteral("key")).toString()));
        m_tempBrushSets->setItem(row, 1, new QTableWidgetItem(object.value(QStringLiteral("brush")).toString()));
        m_tempBrushSets->setItem(
            row,
            2,
            new QTableWidgetItem(QString::number(object.value(QStringLiteral("size_scale")).toDouble())));
    }
    brushSetLayout->addWidget(m_tempBrushSets);
    auto *brushSetButtons = new QHBoxLayout;
    auto *addBrushSet = new QPushButton(i18nc("@action:button", "Add Row"), brushSetGroup);
    auto *removeBrushSet = new QPushButton(i18nc("@action:button", "Remove Row"), brushSetGroup);
    brushSetButtons->addWidget(addBrushSet);
    brushSetButtons->addWidget(removeBrushSet);
    brushSetButtons->addStretch();
    brushSetLayout->addLayout(brushSetButtons);
    connect(addBrushSet, &QPushButton::clicked, this, [this] {
        m_tempBrushSets->insertRow(m_tempBrushSets->rowCount());
    });
    connect(removeBrushSet, &QPushButton::clicked, this, [this] {
        if (m_tempBrushSets->currentRow() >= 0)
            m_tempBrushSets->removeRow(m_tempBrushSets->currentRow());
    });
    pageLayout->addWidget(brushSetGroup);
    pageLayout->addStretch();

    pageLayout->removeWidget(popupGroup);
    pageLayout->removeWidget(adjustGroup);
    pageLayout->removeWidget(brushSetGroup);

    auto *popupPage = new QWidget(this);
    auto *popupPageLayout = new QVBoxLayout(popupPage);
    popupGroup->setParent(popupPage);
    popupPageLayout->addWidget(popupGroup);
    popupPageLayout->addStretch();

    auto *adjustPage = new QWidget(this);
    auto *adjustPageLayout = new QVBoxLayout(adjustPage);
    adjustGroup->setParent(adjustPage);
    adjustPageLayout->addWidget(adjustGroup);
    adjustPageLayout->addStretch();

    auto *brushSetsPage = new QWidget(this);
    auto *brushSetsPageLayout = new QVBoxLayout(brushSetsPage);
    brushSetGroup->setParent(brushSetsPage);
    brushSetsPageLayout->addWidget(brushSetGroup);

    auto *tabs = new QTabWidget(this);
    const auto addScrollableTab = [tabs](QWidget *content, const QString &title) {
        auto *scrollArea = new QScrollArea(tabs);
        scrollArea->setWidgetResizable(true);
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setWidget(content);
        tabs->addTab(scrollArea, title);
    };
    addScrollableTab(page, i18nc("@title:tab", "General"));
    addScrollableTab(popupPage, i18nc("@title:tab", "Popup and HueSVC"));
    addScrollableTab(adjustPage, i18nc("@title:tab", "Quick Brush Adjustments"));
    addScrollableTab(brushSetsPage, i18nc("@title:tab", "Temporary Brushes"));
    root->addWidget(tabs);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QuickAccessSettingsDialog::Settings QuickAccessSettingsDialog::settings() const
{
    Settings result;
    result.columns = m_columns->value();
    result.dockerIconSize = m_dockerIconSize->value();
    result.headerButtonColor = buttonColor(m_headerButtonColor);
    result.headerButtonFontColor = buttonColor(m_headerButtonFontColor);
    result.activeTabFontSize = m_activeTabFontSize->value();
    result.activeTabFontColor = buttonColor(m_activeTabFontColor);
    result.activeTabBackgroundColor = buttonColor(m_activeTabBackgroundColor);
    result.inactiveTabFontSize = m_inactiveTabFontSize->value();
    result.inactiveTabFontColor = buttonColor(m_inactiveTabFontColor);
    result.inactiveTabBackgroundColor = buttonColor(m_inactiveTabBackgroundColor);
    result.dialogWidth = m_dialogWidth->value();
    result.dialogHeight = m_dialogHeight->value();
    result.gestureEnabled = m_gestureEnabled->isChecked();
    result.hueSvcEnabled = m_hueSvcEnabled->isChecked();
    result.quickAdjustEnabled = m_quickAdjustEnabled->isChecked();
    result.popupIconSize = m_popupIconSize->value();
    result.rgbDisplayMode = m_rgbDisplayMode->currentData().toString();
    result.huePopupWidth = m_huePopupWidth->value();
    result.huePopupHeight = m_huePopupHeight->value();
    result.hueControlsWidth = m_hueControlsWidth->value();
    result.hueControlsFontSize = m_hueControlsFontSize->value();
    result.sizeSliderEnabled = m_sizeSliderEnabled->isChecked();
    result.opacitySliderEnabled = m_opacitySliderEnabled->isChecked();
    result.flowSliderEnabled = m_flowSliderEnabled->isChecked();
    result.layerOpacitySliderEnabled = m_layerOpacitySliderEnabled->isChecked();
    result.colorHistoryEnabled = m_colorHistoryEnabled->isChecked();
    result.colorHistoryTotal = m_colorHistoryTotal->value();
    result.colorHistoryIconSize = m_colorHistoryIconSize->value();
    result.brushHistoryEnabled = m_brushHistoryEnabled->isChecked();
    result.brushHistoryTotal = m_brushHistoryTotal->value();
    result.brushHistoryIconSize = m_brushHistoryIconSize->value();
    result.altEraseKey = m_altEraseKey->text().trimmed();
    result.preserveAlphaKey = m_preserveAlphaKey->text().trimmed();
    result.selectOutlineKey = m_selectOutlineKey->text().trimmed();
    result.toolOptionsEnabled = m_toolOptionsEnabled->isChecked();
    result.toolOptionsPosition = m_toolOptionsPosition->currentData().toString();
    result.blendModes.clear();
    const QStringList blendModeLines = m_blendModes->toPlainText().split(QLatin1Char('\n'));
    for (const QString &line : blendModeLines) {
        const QString mode = line.trimmed();
        if (!mode.isEmpty() && !result.blendModes.contains(mode))
            result.blendModes.append(mode);
    }
    for (int row = 0; row < m_tempBrushSets->rowCount(); ++row) {
        const auto text = [this, row](int column) {
            const QTableWidgetItem *item = m_tempBrushSets->item(row, column);
            return item ? item->text().trimmed() : QString();
        };
        if (text(0).isEmpty() || text(1).isEmpty())
            continue;
        QJsonObject object;
        object.insert(QStringLiteral("key"), text(0));
        object.insert(QStringLiteral("brush"), text(1));
        object.insert(QStringLiteral("size_scale"), text(2).toDouble());
        result.tempBrushSets.append(object);
    }
    return result;
}

QPushButton *QuickAccessSettingsDialog::createColorButton(const QColor &color)
{
    auto *button = new QPushButton(this);
    const auto setColor = [button](const QColor &value) {
        const QColor validColor = value.isValid() ? value : QColor(Qt::white);
        button->setProperty("quickAccessColor", validColor.name(QColor::HexArgb));
        button->setText(validColor.name(QColor::HexArgb));
        button->setStyleSheet(
            QStringLiteral("background-color: %1; border: 1px solid #888;").arg(validColor.name(QColor::HexArgb)));
    };
    setColor(color);
    connect(button, &QPushButton::clicked, button, [button, setColor]() {
        const QColor selected =
            QColorDialog::getColor(buttonColor(button), button->window(), i18nc("@title:window", "Select Color"));
        if (selected.isValid())
            setColor(selected);
    });
    return button;
}

QColor QuickAccessSettingsDialog::buttonColor(const QPushButton *button)
{
    return QColor(button->property("quickAccessColor").toString());
}
