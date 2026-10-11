/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPaintOpOptionsModel.h"

#include <QHash>

#include <KisPaintOpPresetUpdateProxy.h>
#include <brushengine/kis_locked_properties_proxy.h>
#include <brushengine/kis_locked_properties_server.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_assert.h>
#include <kis_signal_auto_connection.h>

KisPaintOpOptionStateBase::KisPaintOpOptionStateBase(const QString &id)
    : m_id(id)
{
}

KisPaintOpOptionStateBase::~KisPaintOpOptionStateBase()
{
}

QString KisPaintOpOptionStateBase::id() const
{
    return m_id;
}

struct KisPaintOpOptionsModel::Private {
    std::vector<std::unique_ptr<KisPaintOpOptionStateBase>> options;
    std::vector<QSet<QString>> optionKeys;
    QHash<QString, int> indexById;
    QMultiHash<int, int> dependentsBySource;
    QStringList preservedKeys;
    QStringList sharedKeys;

    KisPaintOpPresetSP preset;
    KisSignalAutoConnectionsStore presetConnections;

    bool needsFullRewrite = true;
    bool isWriting = false;
    bool isReading = false;
};

namespace
{
QSet<QString> writtenKeys(const KisPaintOpOptionStateBase *option)
{
    KisPropertiesConfigurationSP scratch = new KisPropertiesConfiguration();
    option->write(scratch.data());
    const QList<QString> keys = scratch->getPropertiesKeys();
    return QSet<QString>(keys.begin(), keys.end());
}
} // namespace

KisPaintOpOptionsModel::KisPaintOpOptionsModel(QObject *parent)
    : QObject(parent)
    , m_d(new Private)
{
}

KisPaintOpOptionsModel::~KisPaintOpOptionsModel()
{
}

void KisPaintOpOptionsModel::addOption(KisPaintOpOptionStateBase *state)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(state);
    KIS_SAFE_ASSERT_RECOVER(!m_d->indexById.contains(state->id()))
    {
        delete state;
        return;
    }

    const int index = int(m_d->options.size());
    m_d->options.emplace_back(state);
    m_d->optionKeys.emplace_back(writtenKeys(state));
    m_d->indexById.insert(state->id(), index);

    state->watch([this, index]() {
        slotOptionChanged(index);
    });
}

void KisPaintOpOptionsModel::addDependency(const QString &dependentId, const QString &sourceId)
{
    const int dependent = m_d->indexById.value(dependentId, -1);
    const int source = m_d->indexById.value(sourceId, -1);
    KIS_SAFE_ASSERT_RECOVER_RETURN(dependent >= 0 && source >= 0 && dependent != source);

    if (!m_d->dependentsBySource.contains(source, dependent)) {
        m_d->dependentsBySource.insert(source, dependent);
    }
}

QList<KisPaintOpOptionStateBase *> KisPaintOpOptionsModel::options() const
{
    QList<KisPaintOpOptionStateBase *> result;
    for (const auto &option : m_d->options) {
        result << option.get();
    }
    return result;
}

KisPaintOpOptionStateBase *KisPaintOpOptionsModel::option(const QString &id) const
{
    const int index = m_d->indexById.value(id, -1);
    return index >= 0 ? m_d->options[size_t(index)].get() : nullptr;
}

void KisPaintOpOptionsModel::setPreservedKeys(const QStringList &keys)
{
    m_d->preservedKeys = keys;
}

QStringList KisPaintOpOptionsModel::preservedKeys() const
{
    return m_d->preservedKeys;
}

void KisPaintOpOptionsModel::addSharedKey(const QString &key)
{
    if (!m_d->sharedKeys.contains(key)) {
        m_d->sharedKeys << key;
    }
}

void KisPaintOpOptionsModel::attachPreset(KisPaintOpPresetSP preset)
{
    detachPreset();
    if (!preset) {
        return;
    }

    m_d->preset = preset;
    m_d->presetConnections.addConnection(preset->updateProxy(),
                                         SIGNAL(sigSettingsKeysChanged(QSet<QString>, bool)),
                                         this,
                                         SLOT(slotSettingsKeysChanged(QSet<QString>, bool)));

    readAll(preset->settings().data());
    m_d->needsFullRewrite = true;
}

void KisPaintOpOptionsModel::detachPreset()
{
    m_d->presetConnections.clear();
    m_d->preset.clear();
    m_d->needsFullRewrite = true;
}

KisPaintOpPresetSP KisPaintOpOptionsModel::preset() const
{
    return m_d->preset;
}

bool KisPaintOpOptionsModel::isAttachedTo(const KisPropertiesConfiguration *config) const
{
    return m_d->preset && config && m_d->preset->settings().data() == config;
}

void KisPaintOpOptionsModel::readAll(const KisPropertiesConfiguration *config)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);

    QList<int> indexes;
    for (int i = 0; i < int(m_d->options.size()); i++) {
        indexes << i;
    }

    // the locked-properties proxy may write locked values while reading,
    // so it needs a non-const target, like KisPaintOpSettingsWidget does
    KisPropertiesConfiguration *mutableConfig = const_cast<KisPropertiesConfiguration *>(config);
    KisLockedPropertiesProxySP proxy =
        KisLockedPropertiesServer::instance()->createLockedPropertiesProxy(mutableConfig);

    const bool oldReading = m_d->isReading;
    m_d->isReading = true;
    Q_FOREACH (int index, indexes) {
        m_d->options[size_t(index)]->read(proxy.data());
        updateOptionKeys(index);
    }
    m_d->isReading = oldReading;
}

void KisPaintOpOptionsModel::writeAll(KisPropertiesConfiguration *config) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);

    KisLockedPropertiesProxySP proxy = KisLockedPropertiesServer::instance()->createLockedPropertiesProxy(config);
    for (const auto &option : m_d->options) {
        option->write(proxy.data());
    }
}

QSet<QString> KisPaintOpOptionsModel::optionKeys(const QString &id) const
{
    const int index = m_d->indexById.value(id, -1);
    return index >= 0 ? m_d->optionKeys[size_t(index)] : QSet<QString>();
}

bool KisPaintOpOptionsModel::needsFullRewrite() const
{
    return m_d->needsFullRewrite;
}

void KisPaintOpOptionsModel::updateOptionKeys(int index)
{
    m_d->optionKeys[size_t(index)] = writtenKeys(m_d->options[size_t(index)].get());
}

void KisPaintOpOptionsModel::slotOptionChanged(int index)
{
    if (m_d->isReading || m_d->isWriting || !m_d->preset) {
        return;
    }

    KisPaintOpSettingsSP settings = m_d->preset->settings();
    KIS_SAFE_ASSERT_RECOVER_RETURN(settings);

    m_d->isWriting = true;
    {
        KisPaintOpPreset::UpdatedPostponer postponer(m_d->preset);

        if (m_d->needsFullRewrite) {
            settings->resetSettings(m_d->preservedKeys);
            writeAll(settings.data());
            for (int i = 0; i < int(m_d->options.size()); i++) {
                updateOptionKeys(i);
            }
            m_d->needsFullRewrite = false;
        } else {
            // the changed option and, transitively, the options whose baked
            // data depends on it
            QList<int> indexes{index};
            for (int i = 0; i < indexes.size(); i++) {
                Q_FOREACH (int dependent, m_d->dependentsBySource.values(indexes[i])) {
                    if (!indexes.contains(dependent)) {
                        indexes << dependent;
                    }
                }
            }

            Q_FOREACH (int i, indexes) {
                writeOption(i, settings.data());
            }
        }
    }
    m_d->isWriting = false;

    Q_EMIT sigPresetSettingsWritten();
}

void KisPaintOpOptionsModel::writeOption(int index, KisPaintOpSettings *settings)
{
    KisPaintOpOptionStateBase *option = m_d->options[size_t(index)].get();

    KisPropertiesConfigurationSP scratch = new KisPropertiesConfiguration();
    // a shared document is patched, not rewritten from scratch
    Q_FOREACH (const QString &key, m_d->sharedKeys) {
        if (settings->hasProperty(key)) {
            scratch->setProperty(key, settings->getProperty(key));
        }
    }
    option->write(scratch.data());
    const QList<QString> newKeyList = scratch->getPropertiesKeys();
    const QSet<QString> newKeys(newKeyList.begin(), newKeyList.end());

    Q_FOREACH (const QString &key, m_d->optionKeys[size_t(index)] - newKeys) {
        settings->removeProperty(key);
    }

    KisLockedPropertiesProxySP proxy = KisLockedPropertiesServer::instance()->createLockedPropertiesProxy(settings);
    const QMap<QString, QVariant> values = scratch->getProperties();
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        proxy->setProperty(it.key(), it.value());
    }

    m_d->optionKeys[size_t(index)] = newKeys;
}

void KisPaintOpOptionsModel::readOptions(const QList<int> &indexes)
{
    KisPaintOpSettingsSP settings = m_d->preset->settings();
    KIS_SAFE_ASSERT_RECOVER_RETURN(settings);

    KisLockedPropertiesProxySP proxy =
        KisLockedPropertiesServer::instance()->createLockedPropertiesProxy(settings.data());

    m_d->isReading = true;
    Q_FOREACH (int index, indexes) {
        m_d->options[size_t(index)]->read(proxy.data());
        updateOptionKeys(index);
    }
    m_d->isReading = false;
}

void KisPaintOpOptionsModel::slotSettingsKeysChanged(const QSet<QString> &keys, bool allKeys)
{
    // our own writes and the writes the locked-properties proxy performs
    // while we are reading are already reflected in the model
    if (m_d->isWriting || m_d->isReading || !m_d->preset) {
        return;
    }

    QList<int> indexes;
    bool hasUnknownKeys = allKeys;

    if (!allKeys) {
        Q_FOREACH (const QString &key, keys) {
            bool found = false;
            for (int i = 0; i < int(m_d->optionKeys.size()); i++) {
                if (m_d->optionKeys[size_t(i)].contains(key)) {
                    if (!indexes.contains(i)) {
                        indexes << i;
                    }
                    found = true;
                }
            }
            hasUnknownKeys |= !found;
        }
    }

    if (hasUnknownKeys) {
        // a key no option currently writes may still be read by one
        // (e.g. a disabled option's keys), so read everything; equal
        // values do not change the states
        indexes.clear();
        for (int i = 0; i < int(m_d->options.size()); i++) {
            indexes << i;
        }
    } else {
        // the options whose values derive from a changed one read again
        // after it: a masking brush that follows the master size must
        // follow a size changed on the canvas, or the next edit writes a
        // coefficient for the old size
        for (int i = 0; i < indexes.size(); i++) {
            Q_FOREACH (int dependent, m_d->dependentsBySource.values(indexes[i])) {
                if (!indexes.contains(dependent)) {
                    indexes << dependent;
                }
            }
        }
    }

    readOptions(indexes);

    if (allKeys) {
        // the settings were replaced or reset; normalize them again on the
        // next write
        m_d->needsFullRewrite = true;
    }
}
