/*
 *  SPDX-FileCopyrightText: 2004, 2006-2007 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "filter/kis_filter.h"

#include <QString>

#include "filter/kis_filter_configuration.h"
#include "kis_bookmarked_configuration_manager.h"
#include "kis_paint_device.h"
#include "kis_processing_information.h"
#include "kis_selection.h"
#include "kis_transaction.h"
#include "kis_types.h"
#include <KoCompositeOpRegistry.h>
#include <KoUpdater.h>
#include <kis_painter.h>
#ifdef HAVE_KRITA_GPU_ENGINE
#include "gpu/KisGpuTileAccess.h"
#endif

KisFilter::KisFilter(const KoID& _id, const KoID & category, const QString & entry)
    : KisBaseProcessor(_id, category, entry),
      m_supportsLevelOfDetail(false)
{
    init(id() + "_filter_bookmarks");
}

KisFilter::~KisFilter()
{
}

void KisFilter::process(KisPaintDeviceSP device,
                        const QRect& applyRect,
                        const KisFilterConfigurationSP config,
                        KoUpdater* progressUpdater) const
{
    process(device, device, KisSelectionSP(), applyRect, config, progressUpdater);
}

void KisFilter::process(const KisPaintDeviceSP src,
                        KisPaintDeviceSP dst,
                        KisSelectionSP selection,
                        const QRect& applyRect,
                        const KisFilterConfigurationSP config,
                        KoUpdater* progressUpdater ) const
{
    KIS_SAFE_ASSERT_RECOVER_NOOP(config->hasLocalResourcesSnapshot());

    if (applyRect.isEmpty()) return;
    QRect needRect = neededRect(applyRect, config, src->defaultBounds()->currentLevelOfDetail());

#ifdef HAVE_KRITA_GPU_ENGINE
    // CPU filters and composition-source conversion use iterators. Download
    // their input (including convolution margins) in batches before reading.
    KisGpuTileAccess::syncToCpu(src, needRect);
#endif

    KisPaintDeviceSP temporary;
    KisTransaction *transaction = 0;

    bool weirdDstColorSpace =
        dst->colorSpace() != dst->compositionSourceColorSpace() &&
        *dst->colorSpace() != *dst->compositionSourceColorSpace();

    if(src == dst && !selection && !weirdDstColorSpace) {
        temporary = src;
    }
    else {
        temporary = dst->createCompositionSourceDevice(src, needRect);
        transaction = new KisTransaction(temporary);
    }

    try {
        QScopedPointer<KoDummyUpdaterHolder> updaterHolder;

        if (!progressUpdater) {
            // TODO: remove dependency on KoUpdater, depend on KoProgressProxy,
            //       it is more lightweight
            updaterHolder.reset(new KoDummyUpdaterHolder());
            progressUpdater = updaterHolder->updater();
        }

        processImpl(temporary, applyRect, config, progressUpdater);
    }
    catch (const std::bad_alloc&) {
        warnKrita << "Filter" << name() << "failed to allocate enough memory to run.";
    }


    if(transaction) {
        delete transaction;
#ifdef HAVE_KRITA_GPU_ENGINE
        // A selected/different destination may still contain GPU-only pixels.
        KisGpuTileAccess::syncToCpu(dst, applyRect);
#endif
        KisPainter::copyAreaOptimized(applyRect.topLeft(), temporary, dst, applyRect, selection);
    }
}

QRect KisFilter::neededRect(const QRect & rect, const KisFilterConfigurationSP c, int lod) const
{
    Q_UNUSED(c);
    Q_UNUSED(lod);
    return rect;
}

QRect KisFilter::changedRect(const QRect & rect, const KisFilterConfigurationSP c, int lod) const
{
    Q_UNUSED(c);
    Q_UNUSED(lod);
    return rect;
}

bool KisFilter::supportsLevelOfDetail(const KisFilterConfigurationSP config, int lod) const
{
    Q_UNUSED(config);
    Q_UNUSED(lod);
    return m_supportsLevelOfDetail;
}

void KisFilter::setSupportsLevelOfDetail(bool value)
{
    m_supportsLevelOfDetail = value;
}

KisGpuColorTransformSP KisFilter::gpuColorTransform(const KoColorSpace *cs, const KisFilterConfigurationSP config) const
{
    Q_UNUSED(cs);
    Q_UNUSED(config);
    return KisGpuColorTransformSP();
}

bool KisFilter::needsTransparentPixels(const KisFilterConfigurationSP config, const KoColorSpace *cs) const
{
    Q_UNUSED(config);
    Q_UNUSED(cs);

    return false;
}

bool KisFilter::prefersSingleCall(KisPaintDeviceSP device, const KisFilterConfigurationSP config) const
{
    Q_UNUSED(device);
    Q_UNUSED(config);

    return false;
}

bool KisFilter::configurationAllowedForMask(KisFilterConfigurationSP config) const
{
    Q_UNUSED(config);
    return supportsAdjustmentLayers();
}

void KisFilter::fixLoadedFilterConfigurationForMasks(KisFilterConfigurationSP config) const
{
    Q_UNUSED(config);
}
