/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSSETTINGSDIALOG_H
#define QUICKACCESSSETTINGSDIALOG_H

#include "QuickAccessSettings.h"
#include <QDialog>

class QPushButton;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QTableWidget;
class QTextEdit;

class QuickAccessSettingsDialog : public QDialog
{
public:
    using Settings = QuickAccessSettings;

    explicit QuickAccessSettingsDialog(const Settings &settings, QWidget *parent = nullptr);

    Settings settings() const;

private:
    QPushButton *createColorButton(const QColor &color);
    static QColor buttonColor(const QPushButton *button);

    QSpinBox *m_columns{nullptr};
    QSpinBox *m_dockerIconSize{nullptr};
    QPushButton *m_headerButtonColor{nullptr};
    QPushButton *m_headerButtonFontColor{nullptr};
    QSpinBox *m_activeTabFontSize{nullptr};
    QPushButton *m_activeTabFontColor{nullptr};
    QPushButton *m_activeTabBackgroundColor{nullptr};
    QSpinBox *m_inactiveTabFontSize{nullptr};
    QPushButton *m_inactiveTabFontColor{nullptr};
    QPushButton *m_inactiveTabBackgroundColor{nullptr};
    QSpinBox *m_dialogWidth{nullptr};
    QSpinBox *m_dialogHeight{nullptr};
    QCheckBox *m_gestureEnabled{nullptr};
    QCheckBox *m_hueSvcEnabled{nullptr};
    QCheckBox *m_quickAdjustEnabled{nullptr};
    QSpinBox *m_popupIconSize{nullptr};
    QComboBox *m_rgbDisplayMode{nullptr};
    QSpinBox *m_huePopupWidth{nullptr};
    QSpinBox *m_huePopupHeight{nullptr};
    QSpinBox *m_hueControlsWidth{nullptr};
    QSpinBox *m_hueControlsFontSize{nullptr};
    QCheckBox *m_sizeSliderEnabled{nullptr};
    QCheckBox *m_opacitySliderEnabled{nullptr};
    QCheckBox *m_flowSliderEnabled{nullptr};
    QCheckBox *m_layerOpacitySliderEnabled{nullptr};
    QCheckBox *m_colorHistoryEnabled{nullptr};
    QSpinBox *m_colorHistoryTotal{nullptr};
    QSpinBox *m_colorHistoryIconSize{nullptr};
    QCheckBox *m_brushHistoryEnabled{nullptr};
    QSpinBox *m_brushHistoryTotal{nullptr};
    QSpinBox *m_brushHistoryIconSize{nullptr};
    QLineEdit *m_altEraseKey{nullptr};
    QLineEdit *m_preserveAlphaKey{nullptr};
    QLineEdit *m_selectOutlineKey{nullptr};
    QCheckBox *m_toolOptionsEnabled{nullptr};
    QComboBox *m_toolOptionsPosition{nullptr};
    QTextEdit *m_blendModes{nullptr};
    QTableWidget *m_tempBrushSets{nullptr};
};

#endif
