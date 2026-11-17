/*
 * SPDX-FileCopyrightText: 2017 Boudewijn Rempt <boud@valdyas.org>
 * SPDX-FileCopyrightText: 2020 L. E. Segovia <amy@amyspark.me>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "QMic.h"

#include <KisSolsticePaths.h>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QScopeGuard>
#include <kis_action_manager.h>
#include <memory>

#include <KisViewManager.h>
#include <KoJsonTrader.h>
#include <kis_action.h>
#include <kis_debug.h>
#include <klocalizedstring.h>
#include <kpluginfactory.h>
#include <kis_image.h>

#include "kis_qmic_plugin_interface.h"

K_PLUGIN_FACTORY_WITH_JSON(QMicFactory, "kritaqmic.json", registerPlugin<QMic>();)

QMic::QMic(QObject *parent, const QVariantList &)
    : KisActionPlugin(parent)
    , m_qmicAction(createAction("QMic"))
    , m_againAction(createAction("QMicAgain"))
{
    m_qmicAction->setActivationFlags(KisAction::ACTIVE_DEVICE);
    connect(m_qmicAction, SIGNAL(triggered()), this, SLOT(slotQMic()));

    m_againAction->setActivationFlags(KisAction::ACTIVE_DEVICE);
    m_againAction->setEnabled(false);
    connect(m_againAction, SIGNAL(triggered()), this, SLOT(slotQMicAgain()));
    // Action-manager GUI refreshes must not enable Reapply before an output
    // was accepted, or either action while a runtime is active.
    connect(m_againAction, &QAction::changed, this, [this]() {
        if (m_running || !m_hasAppliedFilter)
            m_againAction->setEnabled(false);
    });
    connect(m_qmicAction, &QAction::changed, this, [this]() {
        if (m_running)
            m_qmicAction->setEnabled(false);
    });
}

void QMic::slotQMicAgain()
{
    slotQMic(true);
}

void QMic::slotQMic(bool again)
{
    // The hosted runtime has process-wide state, shared by all main windows.
    static bool runtimeActive = false;
    const auto manager = viewManager();
    if (runtimeActive || !manager || !manager->image() || (again && !m_hasAppliedFilter))
        return;
    runtimeActive = true;
    m_running = true;
    const auto restoreActions = qScopeGuard([this, manager]() {
        runtimeActive = false;
        m_running = false;
        if (manager)
            manager->actionManager()->updateGUI();
    });
    m_qmicAction->setEnabled(false);
    m_againAction->setEnabled(false);

    // G'MIC caches its paths on first use. Configure them before plugin creation.
    const QString dataPath = KisSolsticePaths::defaultResourceDir() + QStringLiteral("/gmic");
    const QString settingsPath = KisSolsticePaths::configFilePath(QStringLiteral("gmic-qt.ini"));
    if (!QDir().mkpath(dataPath) || !QDir().mkpath(QFileInfo(settingsPath).absolutePath())) {
        QMessageBox::warning(manager->mainWindowAsQWidget(),
                             i18n("G'MIC"),
                             i18n("Cannot create the G'MIC settings folder."));
        return;
    }
    qputenv("GMIC_PATH", QDir::toNativeSeparators(dataPath).toUtf8());
    qputenv("SOLSTICE_GMIC_SETTINGS", settingsPath.toUtf8());

    std::unique_ptr<KisQmicPluginInterface> plugin;

    // find the krita-gmic-qt plugin
    const QList<KoJsonTrader::Plugin> offers = KoJsonTrader::instance()->query("Krita/GMic", QString());
    if (offers.isEmpty()) {
        QMessageBox::warning(qApp->activeWindow(),
                             i18nc("@title:window", "Solstice"),
                             i18n("The GMic plugin is not installed or could not be loaded."));
        return;
    }

    for (const auto &loader : offers) {
        auto *factory = qobject_cast<KPluginFactory *>(loader.instance());
        if (!factory) {
            warnPlugins << "(GMic) This is not a Krita plugin: " << loader.fileName() << loader.errorString();

            continue;
        }

        auto *pluginBase = factory->create<QObject>();

        plugin.reset(qobject_cast<KisQmicPluginInterface *>(pluginBase));

        if (!plugin) {
            delete pluginBase;
            warnPlugins << "(GMic) This is not a valid GMic-Qt plugin: " << loader.fileName();
            continue;
        }

        break;
    }

    if (!plugin) {
        QMessageBox::warning(qApp->activeWindow(),
                             i18nc("@title:window", "Solstice"),
                             i18n("Solstice cannot launch the gmic-qt plugin. No bundled library found."));
        return;
    }

    if (manager->blockUntilOperationsFinished(manager->image())) {
        auto image = std::make_shared<KisImageInterface>(manager.data());
        connect(image.get(), &KisImageInterface::outputApplied, this, [this]() {
            m_hasAppliedFilter = true;
        });
        connect(image.get(), &KisImageInterface::errorOccurred, this, [manager](const QString &message) {
            if (manager)
                QMessageBox::warning(manager->mainWindowAsQWidget(), i18n("G'MIC"), message);
        });
        int status = plugin->launch(image, again);

        dbgPlugins << "pluginFinished" << status;
    }

    plugin.reset();

}

#include "QMic.moc"
