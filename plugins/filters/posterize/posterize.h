/*
 *  SPDX-FileCopyrightText: 2014 Manuel Riecke <spell1337@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef POSTERIZE_H
#define POSTERIZE_H

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QVariant>
#include "filter/kis_color_transformation_filter.h"
#include "kis_config_widget.h"

class Posterize : public QObject
{
    Q_OBJECT
public:
    Posterize(QObject *parent, const QVariantList &);
    ~Posterize() override;
};

class KisFilterPosterize : public KisColorTransformationFilter
{
public:
    KisFilterPosterize();
public:
    KoColorTransformation* createTransformation(const KoColorSpace* cs, const KisFilterConfigurationSP config) const override;
    // Solstice (GPU engine, phase 4.115)
    KisGpuColorTransformSP createGpuColorTransform(const KoColorSpace *cs, const KisFilterConfigurationSP config) const override;
    KisConfigWidget* createConfigurationWidget(QWidget* parent, const KisPaintDeviceSP dev, bool useForMasks) const override;
    static inline KoID id() {
        return KoID("posterize", i18n("Posterize"));
    }
protected:
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;
private:
    // Solstice: KisPosterizeColorTransformation::convertsChannelsIndependently() per color space
    mutable QMutex m_independentChannelsMutex;
    mutable QHash<const KoColorSpace*, bool> m_independentChannels;
};

class KisPosterizeColorTransformation : public KoColorTransformation
{
public:
    KisPosterizeColorTransformation(int steps, const KoColorSpace* cs);
    ~KisPosterizeColorTransformation() override;
    void transform(const quint8* src, quint8* dst, qint32 nPixels) const override;

    /**
     * Solstice (GPU engine, phase 4.115): true when the conversions to and
     * from 16-bit sRGB turn every channel of an RGBA F16 pixel into the
     * same channel of the result without looking at the other channels
     * (the profiles share their primaries, so no matrix mixes them). The
     * posterized result is then a function of each channel on its own.
     *
     * Checked on every value of every channel, with the other channels at
     * several values, because one differing 16-bit step would move a pixel
     * to another posterization level.
     */
    bool convertsChannelsIndependently() const;
private:
    const KoColorSpace* m_colorSpace;
    quint32 m_psize;
    quint16 m_step;
    quint16 m_halfStep;
    KoColorConversionTransformation* m_fromConversion;
    KoColorConversionTransformation* m_toConversion;
};

#endif
