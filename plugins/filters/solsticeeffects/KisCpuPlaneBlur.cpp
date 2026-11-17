/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisCpuPlaneBlur.h"
#include <KoUpdater.h>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace SolsticeEffects
{
FloatPlane::FloatPlane(const QRect &bounds)
    : rect(bounds)
{
    if (rect.isEmpty())
        return;
    const size_t width = size_t(rect.width()), height = size_t(rect.height());
    if (width > values.max_size() / height)
        throw std::length_error("CPU plane too large");
    values.resize(width * height);
}
float &FloatPlane::at(int x, int y)
{
    Q_ASSERT(rect.contains(x, y));
    return values[size_t(y - rect.y()) * rect.width() + (x - rect.x())];
}
float FloatPlane::at(int x, int y) const
{
    Q_ASSERT(rect.contains(x, y));
    return values[size_t(y - rect.y()) * rect.width() + (x - rect.x())];
}
int gaussianHalo(double sigma)
{
    return std::isfinite(sigma) && sigma > 0 ? int(std::ceil(3.0 * qMin(sigma, 128.0))) : 0;
}
bool gaussianBlur(const FloatPlane &source, FloatPlane &output, double sigma, KoUpdater *progress)
{
    const auto cancelled = [&] {
        return progress && progress->interrupted();
    };
    if (cancelled())
        return false;
    if (output.rect.isEmpty())
        return true;
    const int radius = gaussianHalo(sigma);
    if (!source.rect.contains(output.rect.adjusted(-radius, -radius, radius, radius)))
        return false;
    std::vector<double> weights(2 * radius + 1, 1.0);
    double sum = 0;
    sigma = qMin(sigma, 128.0);
    for (int i = -radius; i <= radius; ++i) {
        const double weight = radius ? std::exp(-.5 * (i / sigma) * (i / sigma)) : 1.0;
        weights[i + radius] = weight;
        sum += weight;
    }
    for (auto &weight : weights)
        weight /= sum;
    FloatPlane horizontal(output.rect.adjusted(0, -radius, 0, radius));
    FloatPlane result(output.rect);
    for (int y = horizontal.rect.top(); y <= horizontal.rect.bottom(); ++y) {
        if (cancelled())
            return false;
        for (int x = horizontal.rect.left(); x <= horizontal.rect.right(); ++x) {
            double value = 0;
            for (int i = -radius; i <= radius; ++i)
                value += source.at(x + i, y) * weights[i + radius];
            horizontal.at(x, y) = float(value);
        }
    }
    for (int y = result.rect.top(); y <= result.rect.bottom(); ++y) {
        if (cancelled())
            return false;
        for (int x = result.rect.left(); x <= result.rect.right(); ++x) {
            double value = 0;
            for (int i = -radius; i <= radius; ++i)
                value += horizontal.at(x, y + i) * weights[i + radius];
            result.at(x, y) = float(value);
        }
    }
    if (cancelled())
        return false;
    output.values.swap(result.values);
    return true;
}
namespace
{
constexpr int blockSize = 16;
int blockStart(int coordinate)
{
    const int remainder = coordinate % blockSize;
    return coordinate - (remainder < 0 ? remainder + blockSize : remainder);
}
std::array<int, 3> boxRadii(double sigma)
{
    // Three equal boxes of radius r have variance r*(r+1).
    // Replacing one with radius r+1 adds (2*r+2)/3 to that variance.
    const int r = int(std::floor((std::sqrt(1 + 4 * sigma * sigma) - 1) / 2));
    const int larger = qBound(0, qRound(3 * (sigma * sigma - r * (r + 1)) / (2 * r + 2)), 3);
    std::array<int, 3> radii;
    for (int i = 0; i < 3; ++i)
        radii[i] = r + (i >= 3 - larger ? 1 : 0);
    return radii;
}
bool boxBlur(const FloatPlane &source, FloatPlane &output, int radius, KoUpdater *progress)
{
    const auto cancelled = [&] {
        return progress && progress->interrupted();
    };
    const double divisor = 2 * radius + 1;
    FloatPlane horizontal(output.rect.adjusted(0, -radius - blockSize + 1, 0, radius));
    // Rebuild every 16 document pixels. Unlike a row-wide rolling sum, the
    // operation order is independent of the requested patch's origin/size.
    for (int y = horizontal.rect.top(); y <= horizontal.rect.bottom(); ++y) {
        if (cancelled())
            return false;
        for (int anchor = blockStart(output.rect.left()); anchor <= output.rect.right(); anchor += blockSize) {
            double sum = 0;
            for (int dx = -radius; dx <= radius; ++dx)
                sum += source.at(anchor + dx, y);
            const int last = qMin(anchor + blockSize - 1, output.rect.right());
            for (int x = anchor; x <= last; ++x) {
                if (x >= output.rect.left())
                    horizontal.at(x, y) = float(sum / divisor);
                if (x < last) {
                    sum -= source.at(x - radius, y);
                    sum += source.at(x + radius + 1, y);
                }
            }
        }
    }
    std::vector<double> sums(output.rect.width());
    for (int anchor = blockStart(output.rect.top()); anchor <= output.rect.bottom(); anchor += blockSize) {
        if (cancelled())
            return false;
        std::fill(sums.begin(), sums.end(), 0.0);
        for (int dy = -radius; dy <= radius; ++dy)
            for (int x = output.rect.left(); x <= output.rect.right(); ++x)
                sums[x - output.rect.left()] += horizontal.at(x, anchor + dy);
        const int last = qMin(anchor + blockSize - 1, output.rect.bottom());
        for (int y = anchor; y <= last; ++y) {
            for (int x = output.rect.left(); x <= output.rect.right(); ++x) {
                double &sum = sums[x - output.rect.left()];
                if (y >= output.rect.top())
                    output.at(x, y) = float(sum / divisor);
                if (y < last) {
                    sum -= horizontal.at(x, y - radius);
                    sum += horizontal.at(x, y + radius + 1);
                }
            }
        }
    }
    return !cancelled();
}
double boundedSigma(double sigma)
{
    return std::isfinite(sigma) && sigma > 0 ? qMin(sigma, 128.0) : 0;
}
} // namespace
int softBlurHalo(double sigma)
{
    sigma = boundedSigma(sigma);
    if (sigma <= 2)
        return gaussianHalo(sigma);
    const auto radii = boxRadii(sigma);
    return radii[0] + radii[1] + radii[2] + 3 * (blockSize - 1);
}
bool softBlur(const FloatPlane &source, FloatPlane &output, double sigma, KoUpdater *progress)
{
    sigma = boundedSigma(sigma);
    if (sigma <= 2)
        return gaussianBlur(source, output, sigma, progress);
    if (progress && progress->interrupted())
        return false;
    if (output.rect.isEmpty())
        return true;
    const int halo = softBlurHalo(sigma);
    if (!source.rect.contains(output.rect.adjusted(-halo, -halo, halo, halo)))
        return false;
    const auto radii = boxRadii(sigma);
    int remaining = halo - radii[0] - blockSize + 1;
    FloatPlane first(output.rect.adjusted(-remaining, -remaining, remaining, remaining));
    if (!boxBlur(source, first, radii[0], progress))
        return false;
    remaining -= radii[1] + blockSize - 1;
    FloatPlane second(output.rect.adjusted(-remaining, -remaining, remaining, remaining));
    if (!boxBlur(first, second, radii[1], progress))
        return false;
    first.values.clear();
    first.values.shrink_to_fit();
    FloatPlane result(output.rect);
    if (!boxBlur(second, result, radii[2], progress))
        return false;
    output.values.swap(result.values);
    return true;
}
bool maximumFilter(const FloatPlane &source, FloatPlane &output, int radius, KoUpdater *progress)
{
    if (radius < 0 || radius > 4096 || (progress && progress->interrupted()))
        return false;
    if (output.rect.isEmpty())
        return true;
    if (!source.rect.contains(output.rect.adjusted(-radius, -radius, radius, radius)))
        return false;
    // van Herk/Gil-Werman: a window of length k spans at most two blocks.
    // Max has no accumulation rounding, so blocks need not share patch origins.
    const int k = 2 * radius + 1;
    const auto scan = [k](const std::vector<float> &line, std::vector<float> &prefix, std::vector<float> &suffix) {
        const int n = int(line.size());
        prefix.resize(n);
        suffix.resize(n);
        for (int i = 0; i < n; ++i)
            prefix[i] = i % k == 0 ? line[i] : qMax(prefix[i - 1], line[i]);
        for (int i = n - 1; i >= 0; --i)
            suffix[i] = i == n - 1 || (i + 1) % k == 0 ? line[i] : qMax(suffix[i + 1], line[i]);
    };
    FloatPlane horizontal(output.rect.adjusted(0, -radius, 0, radius)), result(output.rect);
    std::vector<float> line, prefix, suffix;
    line.resize(output.rect.width() + 2 * radius);
    for (int y = horizontal.rect.top(); y <= horizontal.rect.bottom(); ++y) {
        if (progress && progress->interrupted())
            return false;
        for (int i = 0; i < int(line.size()); ++i)
            line[i] = source.at(output.rect.left() - radius + i, y);
        scan(line, prefix, suffix);
        for (int i = 0; i < output.rect.width(); ++i)
            horizontal.at(output.rect.left() + i, y) = qMax(suffix[i], prefix[i + 2 * radius]);
    }
    line.resize(output.rect.height() + 2 * radius);
    for (int x = output.rect.left(); x <= output.rect.right(); ++x) {
        if (progress && progress->interrupted())
            return false;
        for (int i = 0; i < int(line.size()); ++i)
            line[i] = horizontal.at(x, output.rect.top() - radius + i);
        scan(line, prefix, suffix);
        for (int i = 0; i < output.rect.height(); ++i)
            result.at(x, output.rect.top() + i) = qMax(suffix[i], prefix[i + 2 * radius]);
    }
    if (progress && progress->interrupted())
        return false;
    output.values.swap(result.values);
    return true;
}
} // namespace SolsticeEffects
