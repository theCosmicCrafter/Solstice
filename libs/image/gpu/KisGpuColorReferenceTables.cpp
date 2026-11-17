/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuColorTransform.h"

#include <KoColorConversionTransformation.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <half.h>
#include <limits>
#include <memory>
#include <random>
#include <vector>

#include <kis_debug.h>

/*
 * The Color Reference filter's two color conversions, measured from the
 * CPU's converters (GPU engine phase 4.116, docs/agent/gpu-engine.md).
 *
 * The filter converts every pixel to its working space (RGBA F32), recolors
 * it there and converts it back. LittleCMS does the conversions; its
 * arithmetic cannot be repeated in a shader. For profiles that share their
 * primaries both conversions treat every channel on its own, so:
 *
 *  - forward (RGBA F16 -> working): a table of 65536 floats per channel;
 *  - backward (working -> RGBA F16): the conversion is monotonic, so for
 *    every half value there is a smallest working value converted to it or
 *    to a larger one. These thresholds are found by bisection over the
 *    float values and stored ascending; a binary search then gives the
 *    half value of any working value.
 */
namespace
{
constexpr int HalfCount = 65536;
/// Half values in ascending order: -inf, the negative ones, zero, the
/// positive ones, +inf (the numbering of shaders/color_transform.comp).
constexpr int ThresholdCount = 63489;
constexpr int ZeroNumber = 31744;
constexpr int TableSize = KisGpuColorTransform::ColorReferenceTableSize;
/// Layout of the block of 32-bit words the shader reads.
constexpr int HeaderWords = 16;

struct ConversionTables {
    QVector<float> forward; ///< 3 * HalfCount
    QVector<float> thresholds; ///< 3 * ThresholdCount
};

bool hasDepth(const KoColorSpace *cs, const KoID &depth)
{
    return cs && cs->colorModelId() == RGBAColorModelID && cs->colorDepthId() == depth;
}

quint16 halfBitsOfNumber(int number)
{
    if (number == 0) {
        return 0xfc00;
    }
    return number < ZeroNumber ? quint16(0x8000 | (ZeroNumber - number)) : quint16(number - ZeroNumber);
}

float halfValue(quint16 bits)
{
    half value;
    value.setBits(bits);
    return float(value);
}

/// Finite floats in ascending order as unsigned keys (-0 and +0 share one).
quint32 keyOfFloat(float value)
{
    quint32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x80000000u) ? 0x80000000u - (bits & 0x7fffffffu) : 0x80000000u + bits;
}

float floatOfKey(quint32 key)
{
    const quint32 bits = key >= 0x80000000u ? key - 0x80000000u : (0x80000000u | (0x80000000u - key));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::unique_ptr<KoColorConversionTransformation> converter(const KoColorSpace *from, const KoColorSpace *to)
{
    // The converters KisColorReferenceFilter::processImpl() creates.
    return std::unique_ptr<KoColorConversionTransformation>(
        from->createColorConverter(to,
                                   KoColorConversionTransformation::internalRenderingIntent(),
                                   KoColorConversionTransformation::internalConversionFlags()));
}

/**
 * The working value of every half bit pattern, per channel. False when a
 * channel's result depends on the other channels or on alpha: every value
 * of every channel is converted with the others at several values.
 */
bool buildForward(const KoColorSpace *cs, const KoColorSpace *work, QVector<float> *forward)
{
    const std::unique_ptr<KoColorConversionTransformation> toWork = converter(cs, work);
    if (!toWork) {
        return false;
    }
    auto sweep = [&](int channel, quint16 others, quint16 alpha) {
        QVector<quint16> src(4 * HalfCount);
        QVector<float> dst(4 * HalfCount);
        for (int i = 0; i < HalfCount; ++i) {
            for (int c = 0; c < 3; ++c) {
                src[4 * i + c] = (channel < 0 || c == channel) ? quint16(i) : others;
            }
            src[4 * i + 3] = alpha;
        }
        toWork->transform(reinterpret_cast<const quint8 *>(src.constData()),
                          reinterpret_cast<quint8 *>(dst.data()),
                          HalfCount);
        return dst;
    };

    const QVector<float> reference = sweep(-1, 0, 0x3c00);
    forward->resize(3 * HalfCount);
    for (int i = 0; i < HalfCount; ++i) {
        for (int c = 0; c < 3; ++c) {
            (*forward)[c * HalfCount + i] = reference[4 * i + c];
        }
    }

    // half 0.0, 1.0, 0.25 and 4.0; alpha 1.0 and 0.5
    for (int channel = 0; channel < 3; ++channel) {
        for (quint16 others : {quint16(0x0000), quint16(0x3c00), quint16(0x3400), quint16(0x4400)}) {
            for (quint16 alpha : {quint16(0x3c00), quint16(0x3800)}) {
                const QVector<float> result = sweep(channel, others, alpha);
                for (int i = 0; i < HalfCount; ++i) {
                    if ((i & 0x7c00) == 0x7c00) {
                        continue; // Inf and NaN are not colors
                    }
                    if (std::memcmp(&result[4 * i + channel], &reference[4 * i + channel], sizeof(float))) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

/// The number (ascending order) of the half value the thresholds give @p value.
int numberForValue(const float *thresholds, float value)
{
    // entry 0 is -inf: at least one threshold is at or below any value
    return int(std::upper_bound(thresholds, thresholds + ThresholdCount, value) - thresholds) - 1;
}

/**
 * For every half value, per channel, the smallest working value that is
 * converted to it or to a larger one. False when the measured thresholds
 * do not reproduce the converter on sample values with unrelated channels
 * (the channels are not independent, or the conversion is not monotonic).
 */
bool buildThresholds(const KoColorSpace *cs, const KoColorSpace *work, QVector<float> *thresholds)
{
    const std::unique_ptr<KoColorConversionTransformation> fromWork = converter(work, cs);
    if (!fromWork) {
        return false;
    }
    auto convert = [&](const QVector<float> &src, QVector<quint16> &dst, int count) {
        fromWork->transform(reinterpret_cast<const quint8 *>(src.constData()),
                            reinterpret_cast<quint8 *>(dst.data()),
                            count);
    };

    const quint32 keyMin = keyOfFloat(-FLT_MAX);
    const quint32 keyMax = keyOfFloat(FLT_MAX);
    enum State : quint8 {
        Search,
        Never,
        Always
    };

    // What the ends of the float range convert to.
    float lowest[3];
    float highest[3];
    {
        QVector<float> src = {-FLT_MAX, -FLT_MAX, -FLT_MAX, 1.0f, FLT_MAX, FLT_MAX, FLT_MAX, 1.0f};
        QVector<quint16> dst(8);
        convert(src, dst, 2);
        for (int c = 0; c < 3; ++c) {
            lowest[c] = halfValue(dst[c]);
            highest[c] = halfValue(dst[4 + c]);
        }
    }

    // Bisection for all half values and the three channels at once: pixel k
    // carries the candidate of half number k in every channel.
    std::vector<float> targets(ThresholdCount);
    for (int k = 0; k < ThresholdCount; ++k) {
        targets[k] = halfValue(halfBitsOfNumber(k));
    }
    std::vector<quint32> low(3 * ThresholdCount, keyMin);
    std::vector<quint32> high(3 * ThresholdCount, keyMax);
    std::vector<quint8> state(3 * ThresholdCount, Search);
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < ThresholdCount; ++k) {
            const int index = c * ThresholdCount + k;
            if (k == 0 || lowest[c] >= targets[k]) {
                state[index] = Always;
            } else if (!(highest[c] >= targets[k])) {
                state[index] = Never;
            }
        }
    }

    QVector<float> src(4 * ThresholdCount);
    QVector<quint16> dst(4 * ThresholdCount);
    for (int round = 0; round < 32; ++round) {
        for (int k = 0; k < ThresholdCount; ++k) {
            for (int c = 0; c < 3; ++c) {
                const int index = c * ThresholdCount + k;
                src[4 * k + c] = floatOfKey(low[index] + (high[index] - low[index]) / 2);
            }
            src[4 * k + 3] = 1.0f;
        }
        convert(src, dst, ThresholdCount);
        for (int k = 0; k < ThresholdCount; ++k) {
            for (int c = 0; c < 3; ++c) {
                const int index = c * ThresholdCount + k;
                if (state[index] != Search || high[index] - low[index] <= 1) {
                    continue;
                }
                const quint32 middle = low[index] + (high[index] - low[index]) / 2;
                if (halfValue(dst[4 * k + c]) >= targets[k]) {
                    high[index] = middle;
                } else {
                    low[index] = middle;
                }
            }
        }
    }

    thresholds->resize(3 * ThresholdCount);
    for (int c = 0; c < 3; ++c) {
        float previous = -std::numeric_limits<float>::infinity();
        for (int k = 0; k < ThresholdCount; ++k) {
            const int index = c * ThresholdCount + k;
            float value = state[index] == Always ? -std::numeric_limits<float>::infinity()
                : state[index] == Never          ? std::numeric_limits<float>::infinity()
                                                 : floatOfKey(high[index]);
            // ascending, whatever a bisection met
            value = std::max(value, previous);
            (*thresholds)[index] = value;
            previous = value;
        }
    }

    // The check: working values with unrelated channels and alpha, many of
    // them right at a threshold, must convert to what the thresholds say.
    constexpr int SampleCount = 1 << 17;
    std::mt19937 random(20261112);
    std::uniform_real_distribution<float> ordinary(-0.25f, 1.25f);
    QVector<float> samples(4 * SampleCount);
    QVector<quint16> converted(4 * SampleCount);
    for (int i = 0; i < SampleCount; ++i) {
        for (int c = 0; c < 3; ++c) {
            const quint32 choice = random() % 10;
            float value;
            if (choice < 5) {
                value = ordinary(random);
            } else if (choice < 8) {
                const float threshold = (*thresholds)[c * ThresholdCount + 1 + int(random() % (ThresholdCount - 1))];
                value = std::isfinite(threshold) ? threshold : ordinary(random);
                if (std::isfinite(threshold) && (random() & 1)) {
                    value = std::nextafter(value, -std::numeric_limits<float>::infinity());
                }
            } else {
                value = floatOfKey(keyMin + quint32(quint64(random()) * (keyMax - keyMin) / 0xffffffffull));
            }
            samples[4 * i + c] = value;
        }
        const quint32 alphaChoice = random() % 3;
        samples[4 * i + 3] = alphaChoice == 0 ? 1.0f : (alphaChoice == 1 ? 0.5f : 0.03125f);
    }
    convert(samples, converted, SampleCount);
    int mismatches = 0;
    for (int i = 0; i < SampleCount; ++i) {
        for (int c = 0; c < 3; ++c) {
            const quint16 expected = converted[4 * i + c];
            const quint16 found =
                halfBitsOfNumber(numberForValue(thresholds->constData() + c * ThresholdCount, samples[4 * i + c]));
            const bool bothZero = !(expected & 0x7fff) && !(found & 0x7fff);
            if (expected != found && !bothZero) {
                ++mismatches;
            }
        }
    }
    if (mismatches) {
        dbgImage << "GPU Color Reference: the conversion from" << work->id() << "to" << cs->id()
                 << "is not reproduced by thresholds:" << mismatches << "of" << 3 * SampleCount << "samples differ";
        return false;
    }
    return true;
}

QSharedPointer<const ConversionTables> conversionTables(const KoColorSpace *cs, const KoColorSpace *work)
{
    // Color spaces live as long as the registry. Null results are kept too.
    static QMutex mutex;
    static QHash<QPair<const KoColorSpace *, const KoColorSpace *>, QSharedPointer<const ConversionTables>> cache;

    QMutexLocker locker(&mutex);
    const auto key = qMakePair(cs, work);
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) {
        return it.value();
    }
    QSharedPointer<ConversionTables> tables(new ConversionTables());
    QSharedPointer<const ConversionTables> result;
    if (buildForward(cs, work, &tables->forward) && buildThresholds(cs, work, &tables->thresholds)) {
        result = tables;
    }
    cache.insert(key, result);
    return result;
}

quint32 floatBits(float value)
{
    quint32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
} // namespace

QSharedPointer<const KisGpuColorTransform>
KisGpuColorTransform::colorReference(const KoColorSpace *cs,
                                     const KoColorSpace *workingSpace,
                                     const ColorReferenceParameters &parameters)
{
    if (!hasDepth(cs, Float16BitsColorDepthID) || !hasDepth(workingSpace, Float32BitsColorDepthID)
        || parameters.table.size() != 3 * TableSize || !std::isfinite(parameters.amount) || parameters.amount < 0.0
        || parameters.amount > 1.0 || !(parameters.channelMask & 7)) {
        return {};
    }
    const QSharedPointer<const ConversionTables> conversions = conversionTables(cs, workingSpace);
    if (!conversions) {
        return {};
    }

    QSharedPointer<KisGpuColorTransform> transform(new KisGpuColorTransform());
    transform->m_kind = ColorReference;
    QVector<quint32> &words = transform->m_words;
    words.resize(HeaderWords + 3 * TableSize + 3 * HalfCount + 3 * ThresholdCount);
    quint64 amountBits;
    std::memcpy(&amountBits, &parameters.amount, sizeof(amountBits));
    words[0] = quint32(amountBits & 0xffffffffu);
    words[1] = quint32(amountBits >> 32);
    words[2] = parameters.keepBrightness ? 1 : 0;
    words[3] = parameters.channelMask & 7;
    words[4] = ThresholdCount;
    quint32 *out = words.data() + HeaderWords;
    for (float value : parameters.table) {
        *out++ = floatBits(value);
    }
    for (float value : conversions->forward) {
        *out++ = floatBits(value);
    }
    for (float value : conversions->thresholds) {
        *out++ = floatBits(value);
    }
    transform->m_tablesId = nextTablesId();
    return transform;
}
