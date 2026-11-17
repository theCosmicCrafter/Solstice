/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISBRUSHBASEDOPTIONSTATES_H
#define KISBRUSHBASEDOPTIONSTATES_H

#include <functional>

#include <lager/cursor.hpp>
#include <lager/reader.hpp>
#include <lager/state.hpp>

#include <KisPaintOpOptionsModel.h>
#include <kritapaintop_export.h>

#include "KisBrushTipOptionData.h"
#include "KisFilterOptionData.h"
#include "KisPaintingModeOptionData.h"
#include "KisTextureOptionData.h"

/**
 * Option states of brush-based engines for KisPaintOpOptionsModel
 * (docs/agent/brush-option-shared-model.md).
 *
 * The brush tip and the masking brush need more than a configuration to read
 * their data (the resources, the brush tip size), so they cannot use
 * KisPaintOpOptionState<Data>. Their reads and writes match
 * KisBrushOptionWidget and KisMaskingBrushOption.
 */

using KisResourcesInterfaceGetter = std::function<KisResourcesInterfaceSP()>;

class PAINTOP_EXPORT KisBrushTipOptionState : public KisPaintOpOptionStateBase
{
public:
    KisBrushTipOptionState(const QString &id, KisBrushOptionWidgetFlags flags);
    ~KisBrushTipOptionState() override;

    /// The resources the brush is loaded from; the global resources until set.
    void setResourcesInterfaceGetter(KisResourcesInterfaceGetter getter);

    lager::cursor<KisBrushTipOptionData> cursor() const;
    KisBrushTipOptionData data() const;
    KisBrushOptionWidgetFlags flags() const;

    lager::reader<qreal> effectiveBrushSize() const;
    lager::reader<bool> lightnessModeEnabled() const;

    void read(const KisPropertiesConfiguration *config) override;
    void write(KisPropertiesConfiguration *config) const override;
    void watch(std::function<void()> callback) override;

private:
    mutable lager::state<KisBrushTipOptionData, lager::automatic_tag> m_state;
    KisBrushOptionWidgetFlags m_flags;
    KisResourcesInterfaceGetter m_resources;
};

class PAINTOP_EXPORT KisMaskingBrushOptionState : public KisPaintOpOptionStateBase
{
public:
    /// @p masterBrushSize is the brush tip size; read the brush tip first.
    KisMaskingBrushOptionState(const QString &id, lager::reader<qreal> masterBrushSize);
    ~KisMaskingBrushOptionState() override;

    void setResourcesInterfaceGetter(KisResourcesInterfaceGetter getter);

    lager::cursor<KisMaskingBrushOptionData> cursor() const;
    KisMaskingBrushOptionData data() const;
    lager::reader<qreal> masterBrushSize() const;

    lager::reader<bool> maskingBrushEnabled() const;

    /// Like KisMaskingBrushOption::readOptionSetting(), the preserve mode
    /// starts after the read value is set.
    void read(const KisPropertiesConfiguration *config) override;
    void write(KisPropertiesConfiguration *config) const override;
    void watch(std::function<void()> callback) override;

private:
    mutable lager::state<KisMaskingBrushOptionData, lager::automatic_tag> m_state;
    lager::reader<qreal> m_masterBrushSize;
    KisResourcesInterfaceGetter m_resources;
};

namespace KisBrushBasedOptionStates
{
/// What KisTextureOptionWidget writes: the pattern embedded.
PAINTOP_EXPORT KisTextureOptionData bakeTextureOption(const KisTextureOptionData &data,
                                                      KisResourcesInterfaceSP resourcesInterface);

/// What KisFilterOptionWidget writes: the fallback filter while none is set.
PAINTOP_EXPORT KisFilterOptionData bakeFilterOption(const KisFilterOptionData &data);

/// What KisPaintingModeOptionWidget writes: wash while the masking brush is
/// enabled.
PAINTOP_EXPORT KisPaintingModeOptionData bakePaintingModeOption(const KisPaintingModeOptionData &data,
                                                                bool maskingBrushEnabled);
} // namespace KisBrushBasedOptionStates

#endif // KISBRUSHBASEDOPTIONSTATES_H
