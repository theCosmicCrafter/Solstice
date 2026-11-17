/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuColorTransform.h"

#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorTransformation.h>

#include <atomic>
#include <random>

#include <kis_debug.h>

#include "KisGpuMergeBatch.h"
#include "filter/kis_filter.h"
#include "filter/kis_filter_configuration.h"
#include "filter/kis_filter_registry.h"
#include "kis_default_bounds_base.h"
#include "kis_node.h"
#ifdef HAVE_KRITA_GPU_ENGINE
#include "KisGpuProjectionCompositor.h"
#endif

namespace
{
constexpr int Channels = 4;
constexpr int TableSize = KisGpuColorTransform::ChannelTableSize;
/// Pixels with unrelated channel values that must agree with the tables.
constexpr int SampleCount = 4096;

std::atomic<quint64> s_nextTablesId{1};

bool isRgbaFloat(const KoColorSpace *cs, bool f16Only)
{
    if (!cs || cs->colorModelId() != RGBAColorModelID) {
        return false;
    }
    return cs->colorDepthId() == Float16BitsColorDepthID || (!f16Only && cs->colorDepthId() == Float32BitsColorDepthID);
}

/// A finite half: ordinary values in 0..1 most of the time, the rest any
/// finite bit pattern (negative, tiny and HDR values).
quint16 sampleBits(std::mt19937 &random)
{
    const quint32 value = random();
    quint16 bits = quint16(value >> 16);
    switch (value & 7) {
    case 0:
        return 0x0000; // 0.0
    case 1:
        return 0x3c00; // 1.0
    case 2:
    case 3:
        break;
    default:
        // 0x3000..0x3bff: 0.125 to just below 1.0
        return quint16(0x3000 + bits % 0x0c00);
    }
    if ((bits & 0x7c00) == 0x7c00) {
        bits &= quint16(~0x0400); // not Inf or NaN
    }
    return bits;
}
} // namespace

QSharedPointer<const KisGpuColorTransform> KisGpuColorTransform::forNode(KisNodeSP node,
                                                                         KisFilterConfigurationSP config,
                                                                         const KoColorSpace *cs,
                                                                         KisDefaultBoundsBaseSP bounds)
{
#ifdef HAVE_KRITA_GPU_ENGINE
    // Development switch, like KRITA_GPU_LAYER_STYLE (docs/agent/gpu-engine.md).
    static const bool disabled = qgetenv("KRITA_GPU_FILTER_LAYER") == "0";
    if (disabled || !node || !config || !cs || !bounds || !KisGpuMergeBatch::mayCompositeOnGpu(node)
        || !KisGpuProjectionCompositor::supportsColorTransform()) {
        return {};
    }
    if (bounds->wrapAroundMode() || bounds->currentLevelOfDetail() > 0) {
        return {};
    }
    KisFilterSP filter = KisFilterRegistry::instance()->value(config->name());
    return filter ? filter->gpuColorTransform(cs, config) : QSharedPointer<const KisGpuColorTransform>();
#else
    Q_UNUSED(node);
    Q_UNUSED(config);
    Q_UNUSED(cs);
    Q_UNUSED(bounds);
    return {};
#endif
}

QSharedPointer<const KisGpuColorTransform> KisGpuColorTransform::desaturate(const KoColorSpace *cs, int type)
{
    if (!isRgbaFloat(cs, false) || type < 0 || type > 5) {
        return {};
    }
    QSharedPointer<KisGpuColorTransform> transform(new KisGpuColorTransform());
    transform->m_kind = Desaturate;
    transform->m_desaturateType = type;
    return transform;
}

QSharedPointer<const KisGpuColorTransform>
KisGpuColorTransform::channelTables(const KoColorSpace *cs, const KoColorTransformation *transformation)
{
    if (!isRgbaFloat(cs, true) || !transformation) {
        return {};
    }

    // Pixel i has the bit pattern i in every channel: with independent
    // channels its result holds entry i of all four tables.
    QVector<quint16> src(Channels * TableSize);
    QVector<quint16> dst(Channels * TableSize);
    for (int i = 0; i < TableSize; ++i) {
        for (int c = 0; c < Channels; ++c) {
            src[Channels * i + c] = quint16(i);
        }
    }
    transformation->transform(reinterpret_cast<const quint8 *>(src.constData()),
                              reinterpret_cast<quint8 *>(dst.data()),
                              TableSize);

    QSharedPointer<KisGpuColorTransform> transform(new KisGpuColorTransform());
    transform->m_kind = ChannelTables;
    transform->m_tables.resize(Channels * TableSize);
    for (int i = 0; i < TableSize; ++i) {
        for (int c = 0; c < Channels; ++c) {
            transform->m_tables[c * TableSize + i] = dst[Channels * i + c];
        }
    }

    // The caller vouches for the independence of the channels; this catches
    // a transformation that plainly mixes them.
    std::mt19937 random(20261112);
    QVector<quint16> samples(Channels * SampleCount);
    QVector<quint16> expected(Channels * SampleCount);
    QVector<quint16> looked(Channels * SampleCount);
    for (quint16 &bits : samples) {
        bits = sampleBits(random);
    }
    transformation->transform(reinterpret_cast<const quint8 *>(samples.constData()),
                              reinterpret_cast<quint8 *>(expected.data()),
                              SampleCount);
    transform->applyTables(samples.constData(), looked.data(), SampleCount);
    if (looked != expected) {
        dbgImage << "GPU color transform: the channels of the transformation are not independent in" << cs->id();
        return {};
    }

    transform->m_tablesId = nextTablesId();
    return transform;
}

quint64 KisGpuColorTransform::nextTablesId()
{
    return s_nextTablesId++;
}

const void *KisGpuColorTransform::tableData() const
{
    return m_kind == ColorReference ? static_cast<const void *>(m_words.constData())
                                    : static_cast<const void *>(m_tables.constData());
}

qint64 KisGpuColorTransform::tableBytes() const
{
    return m_kind == ColorReference ? qint64(m_words.size()) * qint64(sizeof(quint32))
                                    : qint64(m_tables.size()) * qint64(sizeof(quint16));
}

void KisGpuColorTransform::applyTables(const quint16 *src, quint16 *dst, int nPixels) const
{
    const quint16 *tables = m_tables.constData();
    for (int i = 0; i < nPixels; ++i) {
        for (int c = 0; c < Channels; ++c) {
            dst[Channels * i + c] = tables[c * TableSize + src[Channels * i + c]];
        }
    }
}
