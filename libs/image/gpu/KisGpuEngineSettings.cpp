/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuEngineSettings.h"

#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>

#include "kis_image_config.h"

QString KisGpuEngineSettings::enabledKey()
{
    return QStringLiteral("Solstice/GpuEngine");
}

QString KisGpuEngineSettings::convertPolicyKey()
{
    return QStringLiteral("Solstice/GpuEngineConvertDocuments");
}

QString KisGpuEngineSettings::brushKey()
{
    return QStringLiteral("Solstice/GpuBrush");
}

QString KisGpuEngineSettings::unverifiedDeviceKey()
{
    return QStringLiteral("Solstice/GpuEngineUnverifiedDevice");
}

bool KisGpuEngineSettings::unverifiedDeviceAllowedInConfig()
{
    return KisImageConfig(true).readEntry<bool>(unverifiedDeviceKey(), false);
}

void KisGpuEngineSettings::setUnverifiedDeviceAllowedInConfig(bool allowed)
{
    KisImageConfig(false).writeEntry<bool>(unverifiedDeviceKey(), allowed);
}

bool KisGpuEngineSettings::unverifiedDeviceAllowed()
{
    return qEnvironmentVariableIntValue("KRITA_GPU_ALLOW_UNVERIFIED_DEVICE") == 1 || unverifiedDeviceAllowedInConfig();
}

bool KisGpuEngineSettings::enabledInConfig()
{
    return KisImageConfig(true).readEntry<bool>(enabledKey(), false);
}

void KisGpuEngineSettings::setEnabledInConfig(bool enabled)
{
    KisImageConfig(false).writeEntry<bool>(enabledKey(), enabled);
}

bool KisGpuEngineSettings::brushEnabledInConfig()
{
    // on by default since 2026-10-10: input to display about 5 ms against
    // 18-22 ms with the CPU brush (docs/agent/gpu-work-priorities.md)
    return KisImageConfig(true).readEntry<bool>(brushKey(), true);
}

void KisGpuEngineSettings::setBrushEnabledInConfig(bool enabled)
{
    KisImageConfig(false).writeEntry<bool>(brushKey(), enabled);
}

KisGpuEngineSettings::ConvertPolicy KisGpuEngineSettings::convertPolicy()
{
    const int value = KisImageConfig(true).readEntry<int>(convertPolicyKey(), int(Ask));
    return value == int(Convert) || value == int(Keep) || value == int(Convert16) ? ConvertPolicy(value) : Ask;
}

void KisGpuEngineSettings::setConvertPolicy(ConvertPolicy policy)
{
    KisImageConfig(false).writeEntry<int>(convertPolicyKey(), int(policy));
}

bool KisGpuEngineSettings::isGpuColorSpace(const KoColorSpace *colorSpace)
{
    return colorSpace && colorSpace->colorModelId() == RGBAColorModelID
        && (colorSpace->colorDepthId() == Float32BitsColorDepthID
            || colorSpace->colorDepthId() == Float16BitsColorDepthID);
}

const KoColorSpace *KisGpuEngineSettings::conversionTarget(const KoColorSpace *colorSpace)
{
    return conversionTarget(colorSpace, Convert);
}

const KoColorSpace *KisGpuEngineSettings::conversionTarget(const KoColorSpace *colorSpace, ConvertPolicy policy)
{
    if (!colorSpace || isGpuColorSpace(colorSpace)) {
        return nullptr;
    }
    KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
    const QString model = RGBAColorModelID.id();
    const QString depth = policy == Convert16 ? Float16BitsColorDepthID.id() : Float32BitsColorDepthID.id();
    if (colorSpace->colorModelId() == RGBAColorModelID && colorSpace->profile()) {
        if (const KoColorSpace *sameProfile = registry->colorSpace(model, depth, colorSpace->profile())) {
            return sameProfile;
        }
    }
    return registry->colorSpace(model, depth, QString());
}
