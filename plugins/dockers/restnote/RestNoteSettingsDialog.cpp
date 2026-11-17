/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RestNoteSettingsDialog.h"
#include "RestNoteConfig.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <klocalizedstring.h>

namespace
{
QSpinBox *spinBox(int minimum, int maximum, int value, const QString &suffix, QWidget *parent)
{
    auto *spin = new QSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setValue(value);
    spin->setSuffix(suffix);
    return spin;
}
} // namespace

bool RestNoteSettingsDialog::edit(RestNoteConfig &config, QWidget *parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(i18n("Rest Note Settings"));
    dialog.setMinimumWidth(360);
    auto *outer = new QVBoxLayout(&dialog);
    auto *cycleGroup = new QGroupBox(i18n("Work / Break cycle"), &dialog);
    auto *cycleForm = new QFormLayout(cycleGroup);
    auto *work = spinBox(1, 180, config.workMinutes, i18n(" min"), cycleGroup);
    auto *rest = spinBox(1, 60, config.breakMinutes, i18n(" min"), cycleGroup);
    cycleForm->addRow(i18n("Work duration:"), work);
    cycleForm->addRow(i18n("Break duration:"), rest);
    outer->addWidget(cycleGroup);

    auto *microGroup = new QGroupBox(i18n("Eye break reminder (20-20-20)"), &dialog);
    auto *microLayout = new QVBoxLayout(microGroup);
    auto *microEnabled = new QCheckBox(i18n("Enable eye break reminders"), microGroup);
    microEnabled->setChecked(config.microEnabled);
    microLayout->addWidget(microEnabled);
    auto *microForm = new QFormLayout;
    auto *interval = spinBox(1, 60, config.microIntervalMinutes, i18n(" min"), microGroup);
    auto *duration = spinBox(5, 120, config.microDurationSeconds, i18n(" sec"), microGroup);
    auto *skip = spinBox(0, 600, config.microSkipThresholdSeconds, i18n(" sec"), microGroup);
    microForm->addRow(i18n("Interval:"), interval);
    microForm->addRow(i18n("Duration:"), duration);
    microForm->addRow(i18n("Skip if a long break is due within:"), skip);
    microLayout->addLayout(microForm);
    outer->addWidget(microGroup);
    for (auto *control : {interval, duration, skip}) {
        control->setEnabled(microEnabled->isChecked());
        QObject::connect(microEnabled, &QCheckBox::toggled, control, &QWidget::setEnabled);
    }

    auto *idleGroup = new QGroupBox(i18n("Idle detection"), &dialog);
    auto *idleLayout = new QVBoxLayout(idleGroup);
    auto *idleEnabled = new QCheckBox(i18n("Enable idle detection"), idleGroup);
    idleEnabled->setChecked(config.idleEnabled);
    idleLayout->addWidget(idleEnabled);
    auto *idleForm = new QFormLayout;
    auto *idleThreshold = spinBox(5, 3600, config.idleThresholdSeconds, i18n(" sec"), idleGroup);
    idleForm->addRow(i18n("Idle threshold:"), idleThreshold);
    idleLayout->addLayout(idleForm);
    idleThreshold->setEnabled(idleEnabled->isChecked());
    QObject::connect(idleEnabled, &QCheckBox::toggled, idleThreshold, &QWidget::setEnabled);
    outer->addWidget(idleGroup);

    auto *overlayGroup = new QGroupBox(i18n("Overlay appearance"), &dialog);
    auto *overlayForm = new QFormLayout(overlayGroup);
    auto *overlayTitle = spinBox(6, 200, config.overlayTitleFontSize, i18n(" px"), overlayGroup);
    auto *overlayMessage = spinBox(6, 200, config.overlayMessageFontSize, i18n(" px"), overlayGroup);
    auto *overlaySkip = spinBox(6, 200, config.overlaySkipFontSize, i18n(" px"), overlayGroup);
    overlayForm->addRow(i18n("Title font size:"), overlayTitle);
    overlayForm->addRow(i18n("Message font size:"), overlayMessage);
    overlayForm->addRow(i18n("Skip font size:"), overlaySkip);
    outer->addWidget(overlayGroup);

    auto *toastGroup = new QGroupBox(i18n("Toast appearance"), &dialog);
    auto *toastForm = new QFormLayout(toastGroup);
    auto *margin = spinBox(0, 200, config.toastMargin, i18n(" px"), toastGroup);
    auto *toastWidth = spinBox(100, 800, config.toastWidth, i18n(" px"), toastGroup);
    auto *toastHeight = spinBox(50, 400, config.toastHeight, i18n(" px"), toastGroup);
    auto *toastTitle = spinBox(6, 48, config.toastTitleFontSize, i18n(" px"), toastGroup);
    auto *toastMessage = spinBox(6, 48, config.toastMessageFontSize, i18n(" px"), toastGroup);
    toastForm->addRow(i18n("Margin:"), margin);
    toastForm->addRow(i18n("Width:"), toastWidth);
    toastForm->addRow(i18n("Height:"), toastHeight);
    toastForm->addRow(i18n("Title font size:"), toastTitle);
    toastForm->addRow(i18n("Message font size:"), toastMessage);
    outer->addWidget(toastGroup);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    config.workMinutes = work->value();
    config.breakMinutes = rest->value();
    config.microEnabled = microEnabled->isChecked();
    config.microIntervalMinutes = interval->value();
    config.microDurationSeconds = duration->value();
    config.microSkipThresholdSeconds = skip->value();
    config.idleEnabled = idleEnabled->isChecked();
    config.idleThresholdSeconds = idleThreshold->value();
    config.overlayTitleFontSize = overlayTitle->value();
    config.overlayMessageFontSize = overlayMessage->value();
    config.overlaySkipFontSize = overlaySkip->value();
    config.toastMargin = margin->value();
    config.toastWidth = toastWidth->value();
    config.toastHeight = toastHeight->value();
    config.toastTitleFontSize = toastTitle->value();
    config.toastMessageFontSize = toastMessage->value();
    return true;
}
