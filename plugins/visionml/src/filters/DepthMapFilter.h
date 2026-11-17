#ifndef DEPTH_MAP_FILTER_H
#define DEPTH_MAP_FILTER_H

#include "VisionML.h"
#include "filter/kis_filter.h"

class DepthMapFilter : public KisFilter
{
public:
    DepthMapFilter(QSharedPointer<VisionModels> vision);

    void processImpl(KisPaintDeviceSP device,
                     const QRect &applyRect,
                     const KisFilterConfigurationSP,
                     KoUpdater *) const override;

    static inline KoID id()
    {
        return KoID("depth_map", i18n("Depth Map"));
    }

    KisConfigWidget *
    createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP, bool useForMasks) const override;
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP) const override;

    QRect changedRect(const QRect &, const KisFilterConfigurationSP, int lod) const override;
    QRect neededRect(const QRect &, const KisFilterConfigurationSP, int lod) const override;

private:
    QSharedPointer<VisionModels> m_vision;
    VisionMLErrorReporter m_report;
};

#endif // DEPTH_MAP_FILTER_H
