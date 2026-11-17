/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISPAINTOPOPTIONSMODEL_H
#define KISPAINTOPOPTIONSMODEL_H

#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QScopedPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <lager/cursor.hpp>
#include <lager/reader.hpp>
#include <lager/state.hpp>

#include <kis_properties_configuration.h>
#include <kis_types.h>

#include "kritaui_export.h"

/**
 * One paint-op option's state, stored outside of any option widget.
 *
 * The state holds the raw (unbaked) option data. Values that an option
 * derives from other options before saving ("baking", e.g. disabling a
 * curve option through an external link) are applied in write() only.
 */
class KRITAUI_EXPORT KisPaintOpOptionStateBase
{
public:
    KisPaintOpOptionStateBase(const QString &id);
    virtual ~KisPaintOpOptionStateBase();

    QString id() const;

    /**
     * Reads the option from \p config, starting from the current state, the
     * same way KisPaintOpOption::readOptionSetting() does. The state is
     * updated only when the read value differs from the current one.
     */
    virtual void read(const KisPropertiesConfiguration *config) = 0;

    /**
     * Writes the baked option data to \p config.
     */
    virtual void write(KisPropertiesConfiguration *config) const = 0;

    /**
     * Calls \p callback after every change of the state. Called once by
     * KisPaintOpOptionsModel when the option is added.
     */
    virtual void watch(std::function<void()> callback) = 0;

private:
    QString m_id;
};

/**
 * Typed option state. \p Data must provide read(const
 * KisPropertiesConfiguration*), write(KisPropertiesConfiguration*) const and
 * operator==. Per-option writes require write() to depend only on the data
 * (no read-modify-write of the target configuration), except for the
 * model's shared keys (KisPaintOpOptionsModel::addSharedKey()).
 */
template<typename Data>
class KisPaintOpOptionState : public KisPaintOpOptionStateBase
{
public:
    using BakeFunction = std::function<Data(const Data &)>;

    KisPaintOpOptionState(const QString &id, const Data &initialData, BakeFunction bake = {})
        : KisPaintOpOptionStateBase(id)
        , m_state(lager::make_state(initialData, lager::automatic_tag{}))
        , m_bake(std::move(bake))
    {
    }

    lager::cursor<Data> cursor() const
    {
        return m_state;
    }

    lager::reader<Data> reader() const
    {
        return m_state;
    }

    Data data() const
    {
        return m_state.get();
    }

    Data bakedData() const
    {
        return m_bake ? m_bake(m_state.get()) : m_state.get();
    }

    void read(const KisPropertiesConfiguration *config) override
    {
        Data data = m_state.get();
        data.read(config);
        m_state.set(data);
    }

    void write(KisPropertiesConfiguration *config) const override
    {
        bakedData().write(config);
    }

    void watch(std::function<void()> callback) override
    {
        m_state.watch([callback](const Data &) {
            callback();
        });
    }

private:
    mutable lager::state<Data, lager::automatic_tag> m_state;
    BakeFunction m_bake;
};

/**
 * Holds the option states of one paint engine and keeps them synchronized
 * with the settings of the attached preset.
 *
 * - After the initial rewrite, an option change writes its keys and those of
 *   its transitive dependents. Previously owned keys no longer written are removed.
 * - The first write after a preset is attached or its settings are replaced
 *   rewrites all settings (as the legacy Brush Editor did). This drops legacy
 *   keys that are read but never written, such as Custom<id>/Curve<id>.
 * - Changes made by others (toolbar, uniform properties, locked options,
 *   reloads) arrive through KisPaintOpPresetUpdateProxy::sigSettingsKeysChanged()
 *   and update their owners and transitive dependents (all options for unknown keys).
 *
 * Reads and writes go through KisLockedPropertiesProxy, like the legacy
 * KisPaintOpSettingsWidget::setConfiguration()/writeConfiguration().
 *
 * See docs/agent/brush-option-shared-model.md.
 */
class KRITAUI_EXPORT KisPaintOpOptionsModel : public QObject
{
    Q_OBJECT
public:
    explicit KisPaintOpOptionsModel(QObject *parent = nullptr);
    ~KisPaintOpOptionsModel() override;

    /**
     * Creates and registers a typed option state. The model owns it.
     */
    template<typename Data>
    KisPaintOpOptionState<Data> *
    addOption(const QString &id, const Data &initialData, typename KisPaintOpOptionState<Data>::BakeFunction bake = {})
    {
        auto *state = new KisPaintOpOptionState<Data>(id, initialData, std::move(bake));
        addOption(state);
        return state;
    }

    /**
     * Registers \p state and takes its ownership. Option ids must be unique.
     */
    void addOption(KisPaintOpOptionStateBase *state);

    /**
     * Declares that the written (baked) data of option \p dependentId depends
     * on the state of option \p sourceId, e.g. the painting mode on the
     * masking brush. A change of the source then writes the dependent option
     * too. External source changes also reread dependents. Dependencies are
     * followed transitively.
     */
    void addDependency(const QString &dependentId, const QString &sourceId);

    QList<KisPaintOpOptionStateBase *> options() const;
    KisPaintOpOptionStateBase *option(const QString &id) const;

    /**
     * Keys owned by other components (e.g. the Brush Editor's LOD settings)
     * that a full rewrite must keep.
     */
    void setPreservedKeys(const QStringList &keys);
    QStringList preservedKeys() const;

    /**
     * Declares \p key a document that several options read and patch, each
     * its own part (MyPaint's "MyPaint/json"). A per-option write then
     * starts from the key's current value in the preset instead of an empty
     * configuration, so the other options' parts are kept. A full rewrite
     * relies on the settings keeping the key (resetSettings()).
     */
    void addSharedKey(const QString &key);

    /**
     * Starts synchronizing with \p preset: reads all options from its
     * settings and schedules a full rewrite for the next write.
     */
    void attachPreset(KisPaintOpPresetSP preset);
    void detachPreset();
    KisPaintOpPresetSP preset() const;

    /**
     * True when \p config is the settings object of the attached preset.
     */
    bool isAttachedTo(const KisPropertiesConfiguration *config) const;

    /**
     * Reads all options from \p config without writing anything back.
     */
    void readAll(const KisPropertiesConfiguration *config);

    /**
     * Writes all options to \p config through the locked-properties proxy.
     */
    void writeAll(KisPropertiesConfiguration *config) const;

    /**
     * Keys each option would write for its current state.
     */
    QSet<QString> optionKeys(const QString &id) const;

    bool needsFullRewrite() const;

Q_SIGNALS:
    /**
     * Emitted after the model has written option changes to the attached
     * preset.
     */
    void sigPresetSettingsWritten();

private Q_SLOTS:
    void slotSettingsKeysChanged(const QSet<QString> &keys, bool allKeys);

private:
    void slotOptionChanged(int index);
    void writeOption(int index, KisPaintOpSettings *settings);
    void readOptions(const QList<int> &indexes);
    void updateOptionKeys(int index);

private:
    struct Private;
    const QScopedPointer<Private> m_d;
};

#endif // KISPAINTOPOPTIONSMODEL_H
