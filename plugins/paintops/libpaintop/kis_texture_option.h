/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2012 Boudewijn Rempt <boud@valdyas.org>
 * SPDX-FileCopyrightText: 2021 Deif Lou <ginoba@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef KIS_TEXTURE_OPTION_H
#define KIS_TEXTURE_OPTION_H

#include <kritapaintop_export.h>

#include <kis_paint_device.h>
#include <kis_cached_paint_device.h>
#include <kis_types.h>
#include <resources/KoAbstractGradient.h>
#include <resources/KoCachedGradient.h>

#include "KisTextureMaskInfo.h"

struct KisProceduralDabTexture;
class KoColorSpace;

class KoPattern;
class KoResource;
class KisPropertiesConfiguration;
class KisResourcesInterface;

#include <KisStandardOptions.h>
#include <KisTextureOptionData.h>


class PAINTOP_EXPORT KisTextureOption
{
public:
    KisTextureOption(const KisPropertiesConfiguration *setting, KisResourcesInterfaceSP resourcesInterface,
                     KoCanvasResourcesInterfaceSP canvasResourcesInterface,
                     int levelOfDetail,
                     KisBrushTextureFlags flags = None);

    bool m_enabled {false};

    /**
     * @brief apply combine the texture map with the dab
     * @param dab the colored, final representation of the dab, after mirroring and everything.
     * @param offset the position of the dab on the image. used to calculate the position of the mask pattern
     * @param info the paint information
     */
    void apply(KisFixedPaintDeviceSP dab, const QPoint& offset, const KisPaintInformation & info);
    static QList<KoResourceLoadResult> prepareLinkedResources(const KisPropertiesConfigurationSP setting, KisResourcesInterfaceSP resourcesInterface);
    static QList<KoResourceLoadResult> prepareEmbeddedResources(const KisPropertiesConfigurationSP setting, KisResourcesInterfaceSP resourcesInterface);
    bool applyingGradient() const;

    /**
     * GPU engine (Solstice): the texture apply() would give the dab at
     * @p offset, as a GPU description (KisProceduralDabTexture), or null
     * when it cannot run on the GPU: disabled or invalid, Lightness and
     * Gradient, or a dab color space other than RGBA F32. @p brushFlips are
     * the brush tip reflections of the dab (before texturing).
     */
    QSharedPointer<const KisProceduralDabTexture> gpuDescription(const QPoint &offset,
                                                                 const KisPaintInformation &info,
                                                                 const KoColorSpace *dabColorSpace,
                                                                 quint32 brushFlips) const;

    static bool applyingGradient(const KisPropertiesConfiguration *settings);
    static bool requiresEffectiveCompositeOp(const KisPropertiesConfiguration *settings);

private:
    void applyLightness(KisFixedPaintDeviceSP dab, const QPoint& offset, const KisPaintInformation& info);
    void applyGradient(KisFixedPaintDeviceSP dab, const QPoint& offset, const KisPaintInformation& info);
    void fillProperties(const KisPropertiesConfiguration *setting, KisResourcesInterfaceSP resourcesInterface, KoCanvasResourcesInterfaceSP canvasResourcesInterface);
    int effectiveOffsetX(const KisPaintInformation &info) const;
    int effectiveOffsetY(const KisPaintInformation &info) const;
    /// The masking composite id of an alpha-only texturing mode, or empty.
    static QString alphaCompositeOpId(KisTextureOptionData::TexturingMode mode);
    /// Composites the mask patch (alpha8, @p maskPatchRect) into the dab
    /// alpha; shared by apply() and the GPU description's CPU applier.
    static void applyAlphaMask(KisFixedPaintDeviceSP dab,
                               KisPaintDeviceSP maskPatch,
                               const QRect &maskPatchRect,
                               const QString &compositeOpId,
                               qreal strength,
                               bool useSoftTexturing);
    friend class KisTextureCpuApplier;

private:

    int m_offsetX {0};
    int m_offsetY {0};
    bool m_isRandomOffsetX {false};
    bool m_isRandomOffsetY {false};
    KisTextureOptionData::TexturingMode m_texturingMode {KisTextureOptionData::MULTIPLY};
    bool m_useSoftTexturing {false};
    KoAbstractGradientSP m_gradient;
    KoCachedGradient m_cachedGradient;

    int m_levelOfDetail {0};

private:
    KisStrengthOption m_strengthOption;
    KisTextureMaskInfoSP m_maskInfo;
    KisBrushTextureFlags m_flags;
    KisCachedPaintDevice m_cachedPaintDevice;
};

#endif // KIS_TEXTURE_OPTION_H
