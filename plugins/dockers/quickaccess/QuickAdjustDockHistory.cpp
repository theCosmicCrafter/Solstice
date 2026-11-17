/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAdjustDock.h"

#include <QGridLayout>
#include <QPixmap>
#include <QSignalBlocker>
#include <QToolButton>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_canvas_resource_provider.h>
#include <kconfiggroup.h>
#include <klocalizedstring.h>
#include <ksharedconfig.h>

QWidget *QuickAdjustDock::createColorHistoryWidget()
{
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    const int colorCount = qBound(2, config.readEntry("ColorHistoryTotal", 14), 40);
    const int columns = (colorCount + 1) / 2;
    const int buttonSize = qBound(16, config.readEntry("ColorHistoryIconSize", 30), 64);

    auto *group = new QWidget(this);
    auto *grid = new QGridLayout(group);
    grid->setContentsMargins(4, 4, 4, 4);
    grid->setHorizontalSpacing(1);
    grid->setVerticalSpacing(1);
    for (int i = 0; i < colorCount; ++i) {
        auto *button = new QToolButton(group);
        button->setFixedSize(buttonSize, buttonSize);
        button->setAutoRaise(false);
        connect(button, &QToolButton::clicked, this, [this, i] {
            selectHistoryColor(i);
        });
        grid->addWidget(button, i / columns, i % columns);
        m_colorButtons.append(button);
    }
    grid->setColumnStretch(columns, 1);
    updateColorHistoryButtons();
    return group;
}

QWidget *QuickAdjustDock::createBrushHistoryWidget()
{
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));
    const int brushCount = qBound(2, config.readEntry("BrushHistoryTotal", 14), 40);
    const int columns = (brushCount + 1) / 2;
    const int buttonSize = qBound(16, config.readEntry("BrushHistoryIconSize", 34), 64);

    auto *group = new QWidget(this);
    auto *grid = new QGridLayout(group);
    grid->setContentsMargins(4, 4, 4, 4);
    grid->setHorizontalSpacing(1);
    grid->setVerticalSpacing(1);
    for (int i = 0; i < brushCount; ++i) {
        auto *button = new QToolButton(group);
        button->setFixedSize(buttonSize, buttonSize);
        button->setIconSize(QSize(buttonSize - 4, buttonSize - 4));
        button->setStyleSheet(
            QStringLiteral("QToolButton { background-color: #b0b0b0; border: 1px solid #888; border-radius: 4px; }"));
        connect(button, &QToolButton::clicked, this, [this, i] {
            selectHistoryBrush(i);
        });
        grid->addWidget(button, i / columns, i % columns);
        m_brushButtons.append(button);
    }
    grid->setColumnStretch(columns, 1);
    updateBrushHistoryButtons();
    return group;
}

void QuickAdjustDock::loadColorHistory()
{
    m_colorHistory = m_resourceProvider ? m_resourceProvider->colorHistory() : QList<KoColor>();
    while (m_colorHistory.size() > m_colorButtons.size())
        m_colorHistory.removeLast();
    updateColorHistoryButtons();
}

void QuickAdjustDock::updateColorHistoryButtons()
{
    for (int i = 0; i < m_colorButtons.size(); ++i) {
        QToolButton *button = m_colorButtons.at(i);
        if (i < m_colorHistory.size()) {
            const QColor color = m_colorHistory.at(i).toQColor();
            button->setEnabled(true);
            button->setToolTip(i18nc("@info:tooltip", "RGB: %1, %2, %3", color.red(), color.green(), color.blue()));
            button->setStyleSheet(
                QStringLiteral("QToolButton { background-color: %1; border: 1px solid #888; border-radius: 4px; }")
                    .arg(color.name(QColor::HexArgb)));
        } else {
            button->setEnabled(false);
            button->setToolTip(QString());
            button->setStyleSheet(QStringLiteral(
                "QToolButton { background-color: #b0b0b0; border: 1px solid #888; border-radius: 4px; }"));
        }
    }
}

void QuickAdjustDock::selectHistoryColor(int index)
{
    if (m_resourceProvider && index >= 0 && index < m_colorHistory.size())
        m_resourceProvider->setFGColor(m_colorHistory.at(index));
}

void QuickAdjustDock::updateBrushHistoryButtons()
{
    for (int i = 0; i < m_brushButtons.size(); ++i) {
        QToolButton *button = m_brushButtons.at(i);
        if (i < m_brushHistory.size() && m_brushHistory.at(i)) {
            const KisPaintOpPresetSP preset = m_brushHistory.at(i);
            button->setEnabled(true);
            button->setIcon(QIcon(QPixmap::fromImage(preset->image())));
            button->setToolTip(i18nc("@info:tooltip", "Brush: %1", preset->name()));
        } else {
            button->setEnabled(false);
            button->setIcon(QIcon());
            button->setToolTip(QString());
        }
    }
}

void QuickAdjustDock::selectHistoryBrush(int index)
{
    if (m_resourceProvider && index >= 0 && index < m_brushHistory.size())
        m_resourceProvider->setPaintOpPreset(m_brushHistory.at(index));
}

void QuickAdjustDock::slotForegroundColorChanged(const KoColor &color)
{
    if (!m_resourceProvider || m_resourceProvider->eraserMode())
        return;
    m_colorHistory.removeAll(color);
    m_colorHistory.prepend(color);
    while (m_colorHistory.size() > m_colorButtons.size())
        m_colorHistory.removeLast();
    m_resourceProvider->setColorHistory(m_colorHistory);
    updateColorHistoryButtons();
}

void QuickAdjustDock::slotBrushPresetChanged(const KisPaintOpPresetSP preset)
{
    if (!preset)
        return;
    if (!m_brushToggleButtons.isEmpty()) {
        const KisPaintOpSettingsSP settings = preset->settings();
        for (auto it = m_brushToggleButtons.begin(); it != m_brushToggleButtons.end(); ++it) {
            const QSignalBlocker blocker(it.value());
            const bool supported = settings && settings->hasProperty(it.key());
            it.value()->setEnabled(supported);
            it.value()->setChecked(supported && settings->getBool(it.key()));
        }
    }
    for (auto it = m_brushHistory.begin(); it != m_brushHistory.end();) {
        if (*it && (*it)->name() == preset->name())
            it = m_brushHistory.erase(it);
        else
            ++it;
    }
    m_brushHistory.prepend(preset);
    while (m_brushHistory.size() > m_brushButtons.size())
        m_brushHistory.removeLast();
    updateBrushHistoryButtons();
}
