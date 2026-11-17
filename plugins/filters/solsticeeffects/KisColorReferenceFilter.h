/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_COLOR_REFERENCE_FILTER_H
#define KIS_COLOR_REFERENCE_FILTER_H
#include <QMutex>
#include <QVector>
#include <filter/kis_filter.h>
class KisColorReferenceFilter : public KisFilter
{
public:
    KisColorReferenceFilter();
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resources) const override;
    KisConfigWidget *
    createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP device, bool useForMasks) const override;
    bool supportsLevelOfDetail(const KisFilterConfigurationSP, int) const override
    {
        return true;
    }
    QRect neededRect(const QRect &rect, const KisFilterConfigurationSP, int) const override
    {
        return rect;
    }
    QRect changedRect(const QRect &rect, const KisFilterConfigurationSP, int) const override
    {
        return rect;
    }
    void processImpl(KisPaintDeviceSP device,
                     const QRect &rect,
                     const KisFilterConfigurationSP config,
                     KoUpdater *progress) const override;
    // GPU engine (phase 4.116, docs/agent/color-reference.md): what
    // processImpl() computes, for filter layers and filter masks.
    KisGpuColorTransformSP gpuColorTransform(const KoColorSpace *cs,
                                             const KisFilterConfigurationSP config) const override;

private:
    struct GpuCacheEntry {
        const KoColorSpace *colorSpace = nullptr;
        QString table;
        double amount = 0;
        bool keepBrightness = true;
        quint32 channelMask = 0;
        KisGpuColorTransformSP transform;
    };
    // The few configurations in use; a transform holds 1.5 MiB of tables.
    mutable QMutex m_gpuCacheMutex;
    mutable QVector<GpuCacheEntry> m_gpuCache;
};
#endif
