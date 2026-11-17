/*
 *  SPDX-FileCopyrightText: 2014 Manuel Riecke <spell1337@gmail.com>
 * 
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "posterize.h"
#include <stdlib.h>
#include <vector>

#include <QPoint>
#include <QTime>

#include <klocalizedstring.h>

#include <kis_debug.h>
#include <kpluginfactory.h>

#include <kis_processing_information.h>
#include <kis_types.h>
#include <kis_selection.h>
#include <kis_layer.h>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_registry.h>
#include <kis_global.h>

#include <KoColorModelStandardIds.h>
#include <KoColorSpaceMaths.h>
#include <filter/kis_color_transformation_configuration.h>
#include <widgets/kis_multi_integer_filter_widget.h>

K_PLUGIN_FACTORY_WITH_JSON(PosterizeFactory, "kritaposterize.json", registerPlugin<Posterize>();)

Posterize::Posterize(QObject *parent, const QVariantList &)
    : QObject(parent)
{
    KisFilterRegistry::instance()->add(KisFilterSP(new KisFilterPosterize()));
}

Posterize::~Posterize()
{
}

KisFilterPosterize::KisFilterPosterize() : KisColorTransformationFilter(id(), FiltersCategoryArtisticId, i18n("&Posterize..."))
{
    setColorSpaceIndependence(FULLY_INDEPENDENT);
    setSupportsPainting(true);
    setShowConfigurationWidget(true);
}

KoColorTransformation* KisFilterPosterize::createTransformation(const KoColorSpace* cs, const KisFilterConfigurationSP config) const
{
    return new KisPosterizeColorTransformation(config->getInt("steps", 16), cs);
}

KisPosterizeColorTransformation::KisPosterizeColorTransformation(int steps, const KoColorSpace* cs) : m_colorSpace(cs), m_psize(cs->pixelSize())
{
    m_step = KoColorSpaceMathsTraits<quint16>::max / steps;
    m_halfStep = m_step / 2;
    m_fromConversion = KoColorSpaceRegistry::instance()->createColorConverter(
        m_colorSpace,
        KoColorSpaceRegistry::instance()->rgb16("sRGB-elle-V2-srgbtrc.icc"),
        KoColorConversionTransformation::internalRenderingIntent(),
        KoColorConversionTransformation::internalConversionFlags());
    m_toConversion = KoColorSpaceRegistry::instance()->createColorConverter(
        KoColorSpaceRegistry::instance()->rgb16("sRGB-elle-V2-srgbtrc.icc"),
        m_colorSpace,
        KoColorConversionTransformation::internalRenderingIntent(),
        KoColorConversionTransformation::internalConversionFlags());
}

KisGpuColorTransformSP KisFilterPosterize::createGpuColorTransform(const KoColorSpace *cs, const KisFilterConfigurationSP config) const
{
    if (!cs || cs->colorModelId() != RGBAColorModelID || cs->colorDepthId() != Float16BitsColorDepthID) {
        return KisGpuColorTransformSP();
    }

    const KisPosterizeColorTransformation transformation(config->getInt("steps", 16), cs);

    bool independent = false;
    {
        QMutexLocker locker(&m_independentChannelsMutex);
        auto it = m_independentChannels.find(cs);
        if (it == m_independentChannels.end()) {
            it = m_independentChannels.insert(cs, transformation.convertsChannelsIndependently());
        }
        independent = it.value();
    }
    return independent ? KisGpuColorTransform::channelTables(cs, &transformation) : KisGpuColorTransformSP();
}

namespace {
/**
 * Converts 65536 pixels whose channel @p channel runs through every 16-bit
 * pattern while the other channels hold @p others (@p channel < 0: every
 * channel runs), and returns the results.
 */
QVector<quint16> convertChannelSweep(const KoColorConversionTransformation *conversion, int channel, quint16 others)
{
    const int count = 65536;
    QVector<quint16> src(4 * count);
    QVector<quint16> dst(4 * count);
    for (int i = 0; i < count; i++) {
        for (int c = 0; c < 4; c++) {
            src[4 * i + c] = (channel < 0 || c == channel) ? quint16(i) : others;
        }
    }
    conversion->transform(reinterpret_cast<const quint8*>(src.constData()), reinterpret_cast<quint8*>(dst.data()), count);
    return dst;
}

bool sweepsAgree(const KoColorConversionTransformation *conversion, const QVector<quint16> &othersValues, bool halfInput)
{
    // RGBA F16 pixels are stored as R, G, B, A and 16-bit integer ones as
    // B, G, R, A: a channel arrives at the mirrored position.
    const int convertedChannel[4] = {2, 1, 0, 3};

    const QVector<quint16> reference = convertChannelSweep(conversion, -1, 0);
    for (int channel = 0; channel < 4; channel++) {
        const int resultChannel = convertedChannel[channel];
        Q_FOREACH (quint16 others, othersValues) {
            const QVector<quint16> result = convertChannelSweep(conversion, channel, others);
            for (int i = 0; i < 65536; i++) {
                // Inf and NaN are not colors
                if (halfInput && (i & 0x7c00) == 0x7c00) continue;
                if (result[4 * i + resultChannel] != reference[4 * i + resultChannel]) {
                    return false;
                }
            }
        }
    }
    return true;
}
}

bool KisPosterizeColorTransformation::convertsChannelsIndependently() const
{
    if (m_psize != 4 * sizeof(quint16) || !m_fromConversion || !m_toConversion) {
        return false;
    }
    // half 0.0, 1.0, 0.25 and 4.0; then the same idea in 16-bit integers
    return sweepsAgree(m_fromConversion, {0x0000, 0x3c00, 0x3400, 0x4400}, true)
        && sweepsAgree(m_toConversion, {0x0000, 0xffff, 0x5555, 0xaaaa}, false);
}

KisPosterizeColorTransformation::~KisPosterizeColorTransformation()
{
    delete m_fromConversion;
    delete m_toConversion;
}

KisConfigWidget* KisFilterPosterize::createConfigurationWidget(QWidget* parent, const KisPaintDeviceSP dev, bool) const
{
    Q_UNUSED(dev);
    vKisIntegerWidgetParam param;
    param.push_back(KisIntegerWidgetParam(2, 128, 16, i18n("Steps"), "steps"));
    return new KisMultiIntegerFilterWidget(id().id(), parent, id().id(), param);
}

KisFilterConfigurationSP KisFilterPosterize::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    config->setProperty("steps", 16);
    return config;
}

void KisPosterizeColorTransformation::transform(const quint8* src, quint8* dst, qint32 nPixels) const
{
    quint16 m_rgba[4];
    quint16 m_mod[4];

    while (nPixels--) {
        m_fromConversion->transform(src, reinterpret_cast<quint8 *>(m_rgba), 1);

        m_mod[0] = m_rgba[0] % m_step;
        m_mod[1] = m_rgba[1] % m_step;
        m_mod[2] = m_rgba[2] % m_step;
        m_mod[3] = m_rgba[3] % m_step;

        m_rgba[0] = m_rgba[0] + (m_mod[0] > m_halfStep ? m_step - m_mod[0] : -m_mod[0]);
        m_rgba[1] = m_rgba[1] + (m_mod[1] > m_halfStep ? m_step - m_mod[1] : -m_mod[1]);
        m_rgba[2] = m_rgba[2] + (m_mod[2] > m_halfStep ? m_step - m_mod[2] : -m_mod[2]);
        m_rgba[3] = m_rgba[3] + (m_mod[3] > m_halfStep ? m_step - m_mod[3] : -m_mod[3]);

        m_toConversion->transform(reinterpret_cast<quint8 *>(m_rgba), dst, 1);
        src += m_psize;
        dst += m_psize;
    }
}

#include "posterize.moc"
