/*
 * SPDX-FileCopyrightText: 2009 Cyrille Berger <cberger@cberger.net>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef _KIS_COLOR_TRANSFORMATION_FILTER_H_
#define _KIS_COLOR_TRANSFORMATION_FILTER_H_

#include "kis_filter.h"
#include "kritaimage_export.h"
#include "gpu/KisGpuColorTransform.h"

/**
 * This is a base class for filters that implement a filter for
 * KoColorTransformationFactory based filters.
 */
class KRITAIMAGE_EXPORT KisColorTransformationFilter : public KisFilter
{
public:
    KisColorTransformationFilter(const KoID& id, const KoID & category, const QString & entry);
    ~KisColorTransformationFilter() override;
    void processImpl(KisPaintDeviceSP device,
                             const QRect& applyRect,
                             const KisFilterConfigurationSP config,
                             KoUpdater* progressUpdater
                             ) const override;
    /**
     * Create the color transformation that will be applied on the device.
     */
    virtual KoColorTransformation* createTransformation(const KoColorSpace* cs, const KisFilterConfigurationSP config) const = 0;

    /**
     * Solstice (GPU engine, phase 4.115): this filter with @p config in a
     * form the GPU engine computes with the CPU's result, or null (the
     * default) when it has none and runs on the CPU. See
     * KisGpuColorTransform and docs/agent/gpu-engine.md.
     */
    virtual KisGpuColorTransformSP createGpuColorTransform(const KoColorSpace *cs, const KisFilterConfigurationSP config) const;

    /**
     * createGpuColorTransform(), cached in @p config when it is a
     * KisColorTransformationConfiguration (until one of its properties
     * changes).
     */
    KisGpuColorTransformSP gpuColorTransform(const KoColorSpace *cs, const KisFilterConfigurationSP config) const override;

    KisFilterConfigurationSP factoryConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;
};

#endif
