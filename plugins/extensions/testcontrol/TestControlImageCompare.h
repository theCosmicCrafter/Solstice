/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTCONTROLIMAGECOMPARE_H
#define TESTCONTROLIMAGECOMPARE_H

#include <QImage>
#include <QString>

/**
 * Tolerance comparison of an image against a reference, for expectations that
 * cannot be exact hashes (GPU paths, filters, other platforms). Depends on
 * QtGui only.
 */
namespace TestControlImageCompare
{
struct Tolerance {
    /// Largest allowed difference of one channel (0-255) of a pixel.
    int maxChannelDiff = 0;
    /// Number of pixels allowed to exceed maxChannelDiff.
    qint64 maxMismatchPixels = 0;
};

struct Result {
    /// Empty when the images could be compared.
    QString error;
    /// Largest difference of any channel of any pixel.
    int maxChannelDiff = 0;
    /// Pixels with a channel difference above the tolerance.
    qint64 mismatchPixels = 0;
    bool matches = false;
};

/**
 * Compares @p image with @p reference channel by channel in straight
 * (non-premultiplied) ARGB32. Fully transparent pixels compare equal whatever
 * their color. The sizes must match.
 */
Result compare(const QImage &image, const QImage &reference, const Tolerance &tolerance);
} // namespace TestControlImageCompare

#endif // TESTCONTROLIMAGECOMPARE_H
