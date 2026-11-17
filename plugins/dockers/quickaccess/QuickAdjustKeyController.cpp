/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAdjustKeyController.h"

#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisResourceServerProvider.h>
#include <KisViewManager.h>
#include <kconfiggroup.h>
#include <kis_action.h>
#include <kis_action_manager.h>
#include <kis_canvas_resource_provider.h>
#include <kis_paintop_box.h>
#include <ksharedconfig.h>

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>

QuickAdjustKeyController::QuickAdjustKeyController(QObject *parent)
    : QObject(parent)
{
    qApp->installEventFilter(this);
}

QuickAdjustKeyController::~QuickAdjustKeyController()
{
    deactivate();
    if (qApp)
        qApp->removeEventFilter(this);
}

bool QuickAdjustKeyController::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ApplicationDeactivate
        || (event->type() == QEvent::WindowDeactivate && watched == m_activeWindow)) {
        deactivate();
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease)
        return false;
    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (keyEvent->isAutoRepeat())
        return false;

    if (event->type() == QEvent::KeyPress) {
        if (m_mode == Mode::None && ownerWindow() && ownerWindow() == KisPart::instance()->currentMainwindow()
            && !textEditorHasFocus()) {
            m_activeWindow = ownerWindow();
            m_activeViewManager = m_activeWindow->viewManager();
            activateForKey(keyEvent);
            if (m_mode == Mode::None) {
                m_activeWindow.clear();
                m_activeViewManager.clear();
            }
        }
    } else if (m_mode != Mode::None && keyEvent->key() == m_activeKey) {
        deactivate();
    }
    return false;
}

KisMainWindow *QuickAdjustKeyController::ownerWindow() const
{
    for (QObject *object = parent(); object; object = object->parent()) {
        if (auto *window = qobject_cast<KisMainWindow *>(object))
            return window;
    }
    return nullptr;
}

KisCanvasResourceProvider *QuickAdjustKeyController::resourceProvider() const
{
    return m_activeViewManager ? m_activeViewManager->canvasResourceProvider() : nullptr;
}

bool QuickAdjustKeyController::selectPreset(const KisPaintOpPresetSP &preset) const
{
    if (!preset)
        return false;
    if (!m_activeViewManager)
        return false;
    if (KisPaintopBox *paintOpBox = m_activeViewManager->paintOpBox()) {
        paintOpBox->resourceSelected(preset);
        return true;
    }
    if (KisCanvasResourceProvider *provider = m_activeViewManager->canvasResourceProvider()) {
        provider->setPaintOpPreset(preset);
        return true;
    }
    return false;
}

bool QuickAdjustKeyController::activateForKey(QKeyEvent *event)
{
    KisCanvasResourceProvider *provider = resourceProvider();
    if (!provider)
        return false;
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessAdjust"));

    if (matches(event, config.readEntry("AltEraseKey", QString()))) {
        m_mode = Mode::Eraser;
        m_originalToggleState = provider->eraserMode();
        provider->setEraserMode(true);
    } else if (matches(event, config.readEntry("PreserveAlphaKey", QString()))) {
        m_mode = Mode::PreserveAlpha;
        m_originalToggleState = provider->globalAlphaLock();
        provider->setGlobalAlphaLock(true);
    } else if (matches(event, config.readEntry("SelectOutlineKey", QString()))) {
        m_mode = Mode::FreehandSelection;
        if (m_activeViewManager) {
            if (KisAction *action =
                    m_activeViewManager->actionManager()->actionByName(QStringLiteral("KisToolSelectOutline")))
                action->trigger();
        }
    } else {
        const QJsonDocument document =
            QJsonDocument::fromJson(config.readEntry("TempBrushSets", QStringLiteral("[]")).toUtf8());
        for (const QJsonValue &value : document.array()) {
            const QJsonObject object = value.toObject();
            if (!matches(event, object.value(QStringLiteral("key")).toString()))
                continue;
            const auto resources =
                KisResourceServerProvider::instance()->paintOpPresetServer()->resourceModel()->resourcesForName(
                    object.value(QStringLiteral("brush")).toString());
            if (resources.isEmpty())
                return false;
            const KisPaintOpPresetSP preset = resources.constFirst().dynamicCast<KisPaintOpPreset>();
            if (!preset)
                return false;
            m_originalPreset = provider->currentPreset();
            m_originalSize = provider->size();
            m_sizeScale = object.value(QStringLiteral("size_scale")).toDouble();
            if (!selectPreset(preset)) {
                m_originalPreset.clear();
                m_sizeScale = 0.0;
                return false;
            }
            m_mode = Mode::BrushPreset;
            if (m_sizeScale > 0.0)
                provider->setSize(m_originalSize * m_sizeScale);
            break;
        }
    }

    if (m_mode != Mode::None) {
        m_activeKey = event->key();
        return true;
    }
    return false;
}

void QuickAdjustKeyController::deactivate()
{
    KisCanvasResourceProvider *provider = resourceProvider();
    if (provider) {
        if (m_mode == Mode::Eraser) {
            provider->setEraserMode(m_originalToggleState);
        } else if (m_mode == Mode::PreserveAlpha) {
            provider->setGlobalAlphaLock(m_originalToggleState);
        } else if (m_mode == Mode::FreehandSelection) {
            if (m_activeViewManager) {
                if (KisAction *action =
                        m_activeViewManager->actionManager()->actionByName(QStringLiteral("KritaShape/KisToolBrush")))
                    action->trigger();
            }
        } else if (m_mode == Mode::BrushPreset && m_originalPreset) {
            selectPreset(m_originalPreset);
            if (m_sizeScale > 0.0)
                provider->setSize(m_originalSize);
        }
    }
    m_mode = Mode::None;
    m_activeWindow.clear();
    m_activeViewManager.clear();
    m_activeKey = 0;
    m_originalPreset.clear();
    m_sizeScale = 0.0;
}

bool QuickAdjustKeyController::matches(QKeyEvent *event, const QString &sequence)
{
    if (sequence.trimmed().isEmpty())
        return false;
    QString normalized = sequence.trimmed();
    normalized.remove(QLatin1Char(' '));
    const QKeySequence parsed = QKeySequence::fromString(normalized, QKeySequence::PortableText);
    if (parsed.isEmpty())
        return false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QKeyCombination configured = parsed[0];
    const int configuredKey = configured.key();
    const Qt::KeyboardModifiers configuredModifiers = configured.keyboardModifiers();
#else
    // In Qt5, QKeySequence::operator[] returns the combined key + modifier int.
    const int configured = parsed[0];
    const int configuredKey = configured & ~Qt::KeyboardModifierMask;
    const Qt::KeyboardModifiers configuredModifiers =
        Qt::KeyboardModifiers(static_cast<Qt::KeyboardModifier>(configured & Qt::KeyboardModifierMask));
#endif
    const Qt::KeyboardModifiers relevant =
        event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    return event->key() == configuredKey && relevant == configuredModifiers;
}

bool QuickAdjustKeyController::textEditorHasFocus()
{
    QWidget *focus = QApplication::focusWidget();
    return qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus) || qobject_cast<QPlainTextEdit *>(focus)
        || qobject_cast<QAbstractSpinBox *>(focus)
        || (qobject_cast<QComboBox *>(focus) && qobject_cast<QComboBox *>(focus)->isEditable());
}
