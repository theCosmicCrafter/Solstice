/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisLazyToolsRenameDialog.h"
#include "KisLazyToolsRenamePresets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <kis_config.h>
#include <kis_node_view_color_scheme.h>
#include <klocalizedstring.h>

namespace
{
class RenameLayerDialog : public QDialog
{
public:
    RenameLayerDialog(QWidget *parent, const KisLazyToolsRenameDialog::Apply &onApply, const QString &presetPath)
        : QDialog(parent)
        , m_apply(onApply)
        , m_presetPath(presetPath)
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

        bool opened = false;
        const auto presets = KisLazyToolsRenamePresets::load(m_presetPath, &opened);
        if (opened) {
            int buttonIndex = 0;
            for (const auto &preset : presets) {
                const QString &name = preset.name;
                const int label = preset.label;
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
            m_colorCombo->setItemData(index,
                                      index == 0 ? i18n("None") : KisLazyToolsRenamePresets::colorLabelName(index),
                                      Qt::ToolTipRole);
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
                KisLazyToolsRenamePresets::append(m_presetPath, name, label);
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
        if (!name.isEmpty() && m_apply(name, label))
            accept();
    }

    KisLazyToolsRenameDialog::Apply m_apply;
    QString m_presetPath;
    QComboBox *m_colorCombo{nullptr};
    QLineEdit *m_nameInput{nullptr};
    QCheckBox *m_savePreset{nullptr};
};

} // namespace

QDialog *KisLazyToolsRenameDialog::create(QWidget *parent, const Apply &apply, const QString &presetPath)
{
    return new RenameLayerDialog(parent, apply, presetPath);
}
