/*
 *  SPDX-FileCopyrightText: 2004, 2006-2007 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef _KIS_FILTER_H_
#define _KIS_FILTER_H_

#include <list>

#include <QString>

#include <klocalizedstring.h>

#include "KoID.h"
#include "KoColorSpace.h"

#include "kis_types.h"
#include "kis_base_processor.h"
#include "gpu/KisGpuColorTransform.h"

#include "kritaimage_export.h"

/**
 * Basic interface of a Krita filter.
 */
class KRITAIMAGE_EXPORT KisFilter : public KisBaseProcessor
{
public:

    /**
     * Construct a Krita filter
     */
    KisFilter(const KoID& id, const KoID & category, const QString & entry);
    ~KisFilter() override;

    /**
     * Override this function with the implementation of your filter.
     *
     * This is a low level function that expects all the conditions
     * for the @param device be met. Use usual process() methods
     * instead.
     *
     * @param device the paint device to filter
     * @param applyRect the rectangle where the filter is applied
     * @param config the parameters of the filter
     * @param progressUpdater to pass on the progress the filter is making
     */
    virtual void processImpl(KisPaintDeviceSP device,
                             const QRect& applyRect,
                             const KisFilterConfigurationSP config,
                             KoUpdater* progressUpdater = 0 ) const = 0;

    /**
     * Filter \p src device and write the result into \p dst device.
     * If \p dst is an alpha color space device, it will get special
     * treatment.
     *
     * @param src the source paint device
     * @param dst the destination paint device
     * @param selection the selection
     * @param applyRect the rectangle where the filter is applied
     * @param config the parameters of the filter
     * @param progressUpdater to pass on the progress the filter is making
     */
    void process(const KisPaintDeviceSP src,
                 KisPaintDeviceSP dst,
                 KisSelectionSP selection,
                 const QRect& applyRect,
                 const KisFilterConfigurationSP config,
                 KoUpdater* progressUpdater = 0 ) const;


    /**
     * A convenience method for a two-device process() function
     */
    void process(KisPaintDeviceSP device,
                 const QRect& applyRect,
                 const KisFilterConfigurationSP config,
                 KoUpdater* progressUpdater = 0 ) const;

    /**
     * Some filters need pixels outside the current processing rect to compute the new
     * value (for instance, convolution filters)
     */
    virtual QRect neededRect(const QRect & rect, const KisFilterConfigurationSP config, int lod) const;

    /**
    * Similar to @ref neededRect : some filters will alter a lot of pixels that are
    * near to each other at the same time. So when you changed a single rectangle
    * in a device, the actual rectangle that will feel the influence of this change
    * might be bigger. Use this function to determine that rect.
     */
    virtual QRect changedRect(const QRect & rect, const KisFilterConfigurationSP config, int lod) const;

    /**
     * Returns true if the filter is capable of handling LoD scaled planes
     * when generating preview.
     */
    virtual bool supportsLevelOfDetail(const KisFilterConfigurationSP config, int lod) const;

    /**
     * Solstice (GPU engine, phases 4.115 and 4.116): this filter with
     * @p config in a form the GPU engine computes with the CPU's result for
     * a device in @p cs, or null (the default) when it has none and runs on
     * the CPU. Used for filter layers and filter masks; see
     * KisGpuColorTransform and docs/agent/gpu-engine.md.
     */
    virtual KisGpuColorTransformSP gpuColorTransform(const KoColorSpace *cs, const KisFilterConfigurationSP config) const;

    virtual bool needsTransparentPixels(const KisFilterConfigurationSP config, const KoColorSpace *cs) const;

    /**
     * Solstice: true if processImpl() should get the whole rect of @p device
     * in one call instead of concurrent patches, e.g. because it runs on the
     * GPU, where one large call is faster. The default is false.
     */
    virtual bool prefersSingleCall(KisPaintDeviceSP device, const KisFilterConfigurationSP config) const;

    virtual bool configurationAllowedForMask(KisFilterConfigurationSP config) const;
    virtual void fixLoadedFilterConfigurationForMasks(KisFilterConfigurationSP config) const;

protected:

    QString configEntryGroup() const;
    void setSupportsLevelOfDetail(bool value);


private:
    bool m_supportsLevelOfDetail;
};


#endif
