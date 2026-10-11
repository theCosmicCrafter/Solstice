/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisImageTipSampler.h"

#include <QRgba64>
#include <algorithm>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

/*
 * GPU engine (Solstice, "Image tip dabs"): the deterministic createImage()
 * (user decision 2026-10-11): QPainter's output depends on the CPU (Qt
 * replaces the conversions with AVX2/SSE4 versions that round differently,
 * and the SSE4 store uses rcpps, whose results differ between vendors), so
 * the CPU brush now draws image tips with this code, which the GPU repeats
 * bit for bit. Derived from Qt 6.8's raster engine (dkazakov/qtbase
 * fe3936871d, the build's Qt): QPainter::drawImage(QPointF) ->
 * QRasterPaintEngine::drawImage(QRectF ...) with no antialiasing ->
 * fillRect_normalized() of the rounded target rect for a TxScale matrix;
 * QSpanData::setupMatrix(); blend_src_generic_rgb64() (the destination is
 * Format_ARGB32) with fetchTransformedBilinear64_uint32() and
 * storeARGB32FromRGBA64PM(), using the portable conversions
 * QRgba64::premultiplied() and unpremultiplied() and the 16-bit
 * interpolation of interpolate_4_pixels_rgb64(). Differences from QPainter:
 * at most one 8-bit step per channel from those roundings, and rotated dabs
 * cover whole rows instead of QRasterizer spans.
 */
namespace
{
// fetchTransformedBilinear_pixelBounds<BlendTransformedBilinear>()
inline void qtPixelBounds(int l1, int l2, int &v1, int &v2)
{
    if (v1 < l1)
        v2 = v1 = l1;
    else if (v1 >= l2)
        v2 = v1 = l2;
    else
        v2 = v1 + 1;
}

// QRgba64::unpremultiplied_64bit(): fa for each 16-bit alpha
const quint64 *unpremultiplyFactors()
{
    static const QVector<quint64> factors = []() {
        QVector<quint64> values(65536, 0);
        for (quint64 a = 1; a < 65536; ++a)
            values[int(a)] = (Q_UINT64_C(0xffff00008000) + a / 2) / a;
        return values;
    }();
    return factors.constData();
}

#if !(defined(__SSE2__) || defined(_M_X64))
// QRgba64::toArgb32() for one 16-bit channel
inline quint32 to8(quint32 c)
{
    c += 128;
    return (c - (c >> 8)) >> 8;
}
#endif

// unpremultiplied() and toArgb32() of an interpolated QRgba64
inline quint32 storePixel(quint64 value, const quint64 *factors)
{
    const quint32 a = quint32(value >> 48);
    if (a != 0xffff && a != 0) {
        const quint64 fa = factors[a];
        quint64 unpremultiplied = quint64(a) << 48;
        for (int i = 0; i < 3; ++i) {
            const quint64 c = (value >> (16 * i)) & 0xffff;
            unpremultiplied |= (((c * fa + 0x80000000) >> 32) & 0xffff) << (16 * i);
        }
        value = unpremultiplied;
    }
#if defined(__SSE2__) || defined(_M_X64)
    // the SSE2 toArgb32() of qrgba64_p.h
    __m128i v = _mm_cvtsi64_si128(qint64(value));
    v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(3, 0, 1, 2));
    v = _mm_unpacklo_epi16(v, _mm_setzero_si128());
    v = _mm_add_epi32(v, _mm_set1_epi32(128));
    v = _mm_sub_epi32(v, _mm_srli_epi32(v, 8));
    v = _mm_srli_epi32(v, 8);
    v = _mm_packs_epi32(v, v);
    v = _mm_packus_epi16(v, v);
    return quint32(_mm_cvtsi128_si32(v));
#else
    return (to8(a) << 24) | (to8(quint32(value) & 0xffff) << 16) | (to8(quint32(value >> 16) & 0xffff) << 8)
        | to8(quint32(value >> 32) & 0xffff);
#endif
}

#if defined(__SSE2__) || defined(_M_X64)
// storePixel() for a value in the low 64 bits of an SSE register
inline quint32 storePixelSse(__m128i v, const quint64 *factors)
{
    const quint32 a = quint32(_mm_extract_epi16(v, 3));
    if (a != 0xffff && a != 0)
        return storePixel(quint64(_mm_cvtsi128_si64(v)), factors);
    v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(3, 0, 1, 2));
    v = _mm_unpacklo_epi16(v, _mm_setzero_si128());
    v = _mm_add_epi32(v, _mm_set1_epi32(128));
    v = _mm_sub_epi32(v, _mm_srli_epi32(v, 8));
    v = _mm_srli_epi32(v, 8);
    v = _mm_packs_epi32(v, v);
    v = _mm_packus_epi16(v, v);
    return quint32(_mm_cvtsi128_si32(v));
}

// _mm_mulhi_epu16 weights (low 64 bits: 0x10000 - dist, high: dist)
inline __m128i horizontalWeights(quint32 dist)
{
    return _mm_unpacklo_epi64(_mm_set1_epi16(short(0x10000 - dist)), _mm_set1_epi16(short(dist)));
}
#endif

/**
 * One described pixel: fetchTransformedBilinear64_uint32() (16.16 position
 * fx, fy), interpolate_4_pixels_rgb64() (mulhi per 16-bit channel), then
 * unpremultiplied() and toArgb32().
 */
inline quint32
samplePixel(const quint64 *src, int stride, int width, int height, int fx, int fy, const quint64 *factors)
{
    int x1 = fx >> 16;
    int x2 = x1 + 1;
    int y1 = fy >> 16;
    int y2 = y1 + 1;
    if (Q_UNLIKELY(x1 < 0 || x1 >= width - 1))
        qtPixelBounds(0, width - 1, x1, x2);
    if (Q_UNLIKELY(y1 < 0 || y1 >= height - 1))
        qtPixelBounds(0, height - 1, y1, y2);
    const quint64 *top = src + qsizetype(y1) * stride;
    const quint64 tl = top[x1];
    const quint64 tr = top[x2];
    const quint32 distx = quint32(fx) & 0xffff;
    const quint32 disty = quint32(fy) & 0xffff;
    quint64 bl = 0;
    quint64 br = 0;
    if (disty) {
        const quint64 *bottom = src + qsizetype(y2) * stride;
        bl = bottom[x1];
        br = bottom[x2];
    }
    if ((tl | tr | bl | br) == 0)
        return 0;

    quint32 c[4];
#if defined(__SSE2__) || defined(_M_X64)
    // the SSE2 interpolate_4_pixels_rgb64() of Qt
    __m128i vt = _mm_set_epi64x(qint64(tr), qint64(tl));
    if (disty) {
        const __m128i vb = _mm_set_epi64x(qint64(br), qint64(bl));
        vt = _mm_add_epi16(_mm_mulhi_epu16(vt, _mm_set1_epi16(short(0x10000 - disty))),
                           _mm_mulhi_epu16(vb, _mm_set1_epi16(short(disty))));
    }
    if (distx) {
        vt = _mm_mulhi_epu16(vt, horizontalWeights(distx));
        vt = _mm_add_epi16(vt, _mm_srli_si128(vt, 8));
    }
    Q_UNUSED(c);
    return storePixelSse(vt, factors);
#else
    for (int i = 0; i < 4; ++i) {
        const int shift = 16 * i;
        quint32 left = quint32(tl >> shift) & 0xffff;
        quint32 right = quint32(tr >> shift) & 0xffff;
        if (disty) {
            left = (((left * (0x10000 - disty)) >> 16) + (((quint32(bl >> shift) & 0xffff) * disty) >> 16)) & 0xffff;
            right = (((right * (0x10000 - disty)) >> 16) + (((quint32(br >> shift) & 0xffff) * disty) >> 16)) & 0xffff;
        }
        if (distx)
            left = (((left * (0x10000 - distx)) >> 16) + ((right * distx) >> 16)) & 0xffff;
        c[i] = left;
    }
#endif
    // c: red, green, blue, alpha (QRgba64 order)
#if !(defined(__SSE2__) || defined(_M_X64))
    return storePixel(quint64(c[0]) | (quint64(c[1]) << 16) | (quint64(c[2]) << 32) | (quint64(c[3]) << 48), factors);
#endif
}
} // namespace

quint32 KisImageTipSampler::pixel(const KisImageTipGeometry &geometry, int x, int y)
{
    const QImage &source = geometry.source;
    if (geometry.identity) {
        return reinterpret_cast<const quint32 *>(source.constScanLine(y + 1))[x + 1];
    }
    if (y < 0 || y >= geometry.rows.size())
        return 0;
    const KisImageTipGeometry::Row &row = geometry.rows[y];
    if (x < row.x || x >= row.x + row.length)
        return 0;
    const int step = x - row.x;
    return samplePixel(geometry.premultiplied->constData(),
                       source.width(),
                       source.width(),
                       source.height(),
                       row.fx + step * geometry.fdx,
                       row.fy + step * geometry.fdy,
                       unpremultiplyFactors());
}

QImage KisImageTipSampler::render(const KisImageTipGeometry &geometry)
{
    const QImage &source = geometry.source;
    if (geometry.identity) {
        return source.copy(1, 1, source.width() - 2 * 1, source.height() - 2 * 1);
    }
    QImage image(geometry.size, QImage::Format_ARGB32);
    image.fill(0);
    const quint64 *src = geometry.premultiplied->constData();
    const quint64 *factors = unpremultiplyFactors();
    const int width = source.width();
    const int height = source.height();
    QVector<quint64> columns; // scale only: the vertical blend of each source column
    for (int y = 0; y < geometry.size.height() && y < geometry.rows.size(); ++y) {
        const KisImageTipGeometry::Row &row = geometry.rows[y];
        if (!row.length)
            continue;
        quint32 *line = reinterpret_cast<quint32 *>(image.scanLine(y)) + row.x;
        int fx = row.fx;
        int fy = row.fy;
        if (geometry.fdy != 0) {
            for (int i = 0; i < row.length; ++i) {
                line[i] = samplePixel(src, width, width, height, fx, fy, factors);
                fx += geometry.fdx;
                fy += geometry.fdy;
            }
            continue;
        }
        // fdy == 0: the same interpolation, with the vertical step (the
        // same weights for the whole row) done once per source column
        int y1 = fy >> 16;
        int y2;
        qtPixelBounds(0, height - 1, y1, y2);
        const quint32 disty = quint32(fy) & 0xffff;
        const quint64 *top = src + qsizetype(y1) * width;
        const quint64 *bottom = src + qsizetype(y2) * width;
        columns.resize(width);
        quint64 *blended = columns.data();
        int first = fx >> 16;
        int last = (fx + (row.length - 1) * geometry.fdx) >> 16;
        if (first > last)
            std::swap(first, last);
        first = qBound(0, first, width - 1);
        last = qBound(0, last + 1, width - 1);
        if (!disty) {
            for (int c = first; c <= last; ++c)
                blended[c] = top[c];
        } else {
#if defined(__SSE2__) || defined(_M_X64)
            const __m128i wt = _mm_set1_epi16(short(0x10000 - disty));
            const __m128i wb = _mm_set1_epi16(short(disty));
            int c = first;
            for (; c + 1 <= last; c += 2) {
                const __m128i vt = _mm_loadu_si128(reinterpret_cast<const __m128i *>(top + c));
                const __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i *>(bottom + c));
                _mm_storeu_si128(reinterpret_cast<__m128i *>(blended + c),
                                 _mm_add_epi16(_mm_mulhi_epu16(vt, wt), _mm_mulhi_epu16(vb, wb)));
            }
            for (; c <= last; ++c) {
                const __m128i vt = _mm_cvtsi64_si128(qint64(top[c]));
                const __m128i vb = _mm_cvtsi64_si128(qint64(bottom[c]));
                blended[c] =
                    quint64(_mm_cvtsi128_si64(_mm_add_epi16(_mm_mulhi_epu16(vt, wt), _mm_mulhi_epu16(vb, wb))));
            }
#else
            for (int c = first; c <= last; ++c) {
                quint64 value = 0;
                for (int i = 0; i < 4; ++i) {
                    const int shift = 16 * i;
                    const quint32 t = quint32(top[c] >> shift) & 0xffff;
                    const quint32 b = quint32(bottom[c] >> shift) & 0xffff;
                    value |= quint64((((t * (0x10000 - disty)) >> 16) + ((b * disty) >> 16)) & 0xffff) << shift;
                }
                blended[c] = value;
            }
#endif
        }
        for (int i = 0; i < row.length; ++i) {
            int x1 = fx >> 16;
            int x2 = x1 + 1;
            if (Q_UNLIKELY(x1 < 0 || x1 >= width - 1))
                qtPixelBounds(0, width - 1, x1, x2);
            const quint64 left = blended[x1];
            const quint64 right = blended[x2];
            const quint32 distx = quint32(fx) & 0xffff;
            fx += geometry.fdx;
            if ((left | right) == 0) {
                line[i] = 0;
                continue;
            }
#if defined(__SSE2__) || defined(_M_X64)
            __m128i vt = _mm_cvtsi64_si128(qint64(left));
            if (distx) {
                vt = _mm_mulhi_epu16(_mm_set_epi64x(qint64(right), qint64(left)), horizontalWeights(distx));
                vt = _mm_add_epi16(vt, _mm_srli_si128(vt, 8));
            }
            line[i] = storePixelSse(vt, factors);
#else
            quint64 value = left;
            if (distx) {
                value = 0;
                for (int k = 0; k < 4; ++k) {
                    const int shift = 16 * k;
                    const quint32 l = quint32(left >> shift) & 0xffff;
                    const quint32 r = quint32(right >> shift) & 0xffff;
                    value |= quint64((((l * (0x10000 - distx)) >> 16) + ((r * distx) >> 16)) & 0xffff) << shift;
                }
            }
            line[i] = storePixel(value, factors);
#endif
        }
    }
    return image;
}

QSharedPointer<const QVector<quint64>> KisImageTipSampler::premultiply(const QImage &level)
{
    QSharedPointer<QVector<quint64>> premultiplied(new QVector<quint64>(level.width() * level.height()));
    for (int y = 0; y < level.height(); ++y) {
        const quint32 *line = reinterpret_cast<const quint32 *>(level.constScanLine(y));
        quint64 *out = premultiplied->data() + qsizetype(y) * level.width();
        for (int x = 0; x < level.width(); ++x) {
            out[x] = quint64(QRgba64::fromArgb32(line[x]).premultiplied());
        }
    }
    return premultiplied;
}
