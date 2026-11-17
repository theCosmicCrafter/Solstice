/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUCOLORTRANSFORM_H
#define KISGPUCOLORTRANSFORM_H

#include <QSharedPointer>
#include <QVector>

#include "kritaimage_export.h"

#include "kis_default_bounds_base.h"
#include "kis_types.h"

class KoColorSpace;
class KoColorTransformation;

/**
 * Solstice (GPU engine, phases 4.115 and 4.116, docs/agent/gpu-engine.md):
 * a filter in a form the GPU engine can compute, with the CPU's result.
 * KisFilter::gpuColorTransform() returns one for the filters and
 * configurations that have such a form.
 *
 * Always compiled; it only describes the transformation.
 */
class KRITAIMAGE_EXPORT KisGpuColorTransform
{
public:
    enum Kind {
        /// KisDesaturateAdjustment: arithmetic the GPU repeats step by step.
        Desaturate,
        /// One table per channel, indexed by the bit pattern of the half
        /// value (RGBA F16 only): the output channel depends on nothing but
        /// the same input channel.
        ChannelTables,
        /// The Color Reference filter (RGBA F16 only): its arithmetic
        /// between two color conversions that are measured into tables.
        ColorReference
    };

    static constexpr int ChannelTableSize = 65536;
    /// Entries of the Color Reference filter's reference table.
    static constexpr int ColorReferenceTableSize = 1024;

    /// What KisColorReferenceFilter::processImpl() reads from its configuration.
    struct ColorReferenceParameters {
        QVector<float> table; ///< ColorReferenceTableSize RGB triples in the working space
        double amount = 1.0; ///< 0..1
        bool keepBrightness = true;
        quint32 channelMask = 7; ///< enabled color channels: bit 0 red, 1 green, 2 blue
    };

    /**
     * The GPU form of the filter of @p config for a filter layer or filter
     * mask of @p node's image, on devices in @p cs with the bounds
     * @p bounds. Null when the node is filtered on the CPU: the GPU
     * projection is off for the image, the filter or its configuration has
     * no GPU form, the update runs at a reduced level of detail or in
     * wrap-around mode, the GPU lacks 64-bit float shaders, or
     * KRITA_GPU_FILTER_LAYER=0 is set (a development switch).
     */
    static QSharedPointer<const KisGpuColorTransform>
    forNode(KisNodeSP node, KisFilterConfigurationSP config, const KoColorSpace *cs, KisDefaultBoundsBaseSP bounds);

    /**
     * The desaturate filter of @p type (0 lightness, 1 BT.709, 2 BT.601,
     * 3 average, 4 minimum, 5 maximum) in @p cs. Null unless @p cs is
     * RGBA F32/F16 and the type is one of these.
     */
    static QSharedPointer<const KisGpuColorTransform> desaturate(const KoColorSpace *cs, int type);

    /**
     * Channel tables of @p transformation, which the caller knows to treat
     * every channel on its own (per-channel curves, for instance). Every
     * table entry is computed by @p transformation itself, so looking a
     * pixel up gives exactly what transform() gives.
     *
     * Null unless @p cs is RGBA F16, or when sample pixels with unrelated
     * channel values show that the channels are not independent after all.
     */
    static QSharedPointer<const KisGpuColorTransform> channelTables(const KoColorSpace *cs,
                                                                    const KoColorTransformation *transformation);

    /**
     * The Color Reference filter (plugins/filters/solsticeeffects) on a
     * device in @p cs: every pixel is converted to @p workingSpace (RGBA
     * F32), recolored there, and converted back.
     *
     * Both conversions come from the CPU's converters: the one to the
     * working space as a table per channel over all 65536 half values, the
     * one back as a sorted table per channel that holds, for every half
     * value, the smallest working value converted to it (found by
     * bisection; the conversion is monotonic). The tables are built once
     * per pair of color spaces and kept.
     *
     * Null unless @p cs is RGBA F16 and @p workingSpace RGBA F32, or when
     * the conversions do not treat every channel on its own (profiles with
     * different primaries) or the measured tables fail their check
     * against the converters.
     */
    static QSharedPointer<const KisGpuColorTransform> colorReference(const KoColorSpace *cs,
                                                                     const KoColorSpace *workingSpace,
                                                                     const ColorReferenceParameters &parameters);

    Kind kind() const
    {
        return m_kind;
    }

    int desaturateType() const
    {
        return m_desaturateType;
    }

    /// 4 * ChannelTableSize half bit patterns: the R, G, B and A tables.
    const quint16 *tables() const
    {
        return m_tables.constData();
    }

    /// The tables as the GPU pass takes them: the channel tables, or the
    /// Color Reference block of 32-bit words (shaders/color_transform.comp).
    const void *tableData() const;
    qint64 tableBytes() const;

    /// Unique for every set of tables in this process.
    quint64 tablesId() const
    {
        return m_tablesId;
    }

    /// What the GPU computes for @p nPixels pixels (ChannelTables only; tests).
    void applyTables(const quint16 *src, quint16 *dst, int nPixels) const;

private:
    KisGpuColorTransform() = default;

    Kind m_kind = Desaturate;
    int m_desaturateType = 0;
    QVector<quint16> m_tables;
    QVector<quint32> m_words;
    quint64 m_tablesId = 0;

    static quint64 nextTablesId();
};

using KisGpuColorTransformSP = QSharedPointer<const KisGpuColorTransform>;

#endif // KISGPUCOLORTRANSFORM_H
