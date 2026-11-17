/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISABRPARSER_H
#define KISABRPARSER_H

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include "kritabrush_export.h"

/**
 * Reads a Photoshop brush file (.abr) into its parts: the sampled brush
 * tips, and the raw pattern, preset descriptor and folder sections, which
 * later stages interpret. Versions 1 and 2 (a list of brushes) and 6 to 10
 * (8BIM sections) are read; tips may be 8 or 16 bits, raw or RLE.
 *
 * Reader operations check requested ranges and report warnings for
 * unreadable entries; parsing can return partial contents. See
 * docs/agent/abr-import.md (Solstice).
 */
class BRUSH_EXPORT KisAbrParser
{
public:
    /// The largest sampled tip edge accepted, in pixels
    static const int maxEdge = 8192;
    /// Decoded tip-byte budget per legacy list or samp section
    static const qint64 maxTotalBytes = qint64(512) << 20;

    struct Sample {
        /// the tip's position among the file's brushes, from 1 (tips that
        /// cannot be read keep their numbers)
        int index = 0;
        /// the identifier that a preset's `sampledData` refers to (version
        /// 6 and later), empty otherwise
        QString id;
        /// the name stored with the tip (version 2), empty otherwise
        QString name;
        /// the tip as a gray image, white where the brush does not paint
        /// (as the ABR brushes have always been read)
        QImage image;
        int depth = 8;
    };

    struct Contents {
        int version = 0;
        int subversion = 0;
        QVector<Sample> samples;
        /// the computed (elliptic) brushes of versions 1 and 2, which are
        /// not read
        int skippedComputedBrushes = 0;
        /// the 8BIM `patt`, `desc` and `phry` sections, without their
        /// headers
        QByteArray patterns;
        QByteArray descriptors;
        QByteArray hierarchy;
        QStringList warnings;
    };

    /// Reads @p data; false when nothing could be read (an unknown version
    /// or no tips and no descriptor data)
    static bool parse(const QByteArray &data, Contents *contents);
};

#endif // KISABRPARSER_H
