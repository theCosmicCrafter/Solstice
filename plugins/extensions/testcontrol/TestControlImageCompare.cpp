/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestControlImageCompare.h"

#include <QtGlobal>

#include <algorithm>
#include <cstdlib>

namespace TestControlImageCompare
{
Result compare(const QImage &image, const QImage &reference, const Tolerance &tolerance)
{
    Result result;
    if (image.isNull() || reference.isNull()) {
        result.error = QStringLiteral("An image is missing or could not be decoded");
        return result;
    }
    if (image.size() != reference.size()) {
        result.error = QStringLiteral("Size %1x%2 differs from the reference's %3x%4")
                           .arg(image.width())
                           .arg(image.height())
                           .arg(reference.width())
                           .arg(reference.height());
        return result;
    }

    const QImage a = image.convertToFormat(QImage::Format_ARGB32);
    const QImage b = reference.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *lineA = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *lineB = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            const QRgb pa = lineA[x];
            const QRgb pb = lineB[x];
            int diff = std::abs(qAlpha(pa) - qAlpha(pb));
            if (qAlpha(pa) != 0 || qAlpha(pb) != 0) {
                diff = std::max({diff,
                                 std::abs(qRed(pa) - qRed(pb)),
                                 std::abs(qGreen(pa) - qGreen(pb)),
                                 std::abs(qBlue(pa) - qBlue(pb))});
            }
            result.maxChannelDiff = std::max(result.maxChannelDiff, diff);
            if (diff > tolerance.maxChannelDiff) {
                result.mismatchPixels++;
            }
        }
    }
    result.matches = result.mismatchPixels <= tolerance.maxMismatchPixels;
    return result;
}
} // namespace TestControlImageCompare
