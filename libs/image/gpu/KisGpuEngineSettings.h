/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUENGINESETTINGS_H
#define KISGPUENGINESETTINGS_H

#include <QString>

#include "kritaimage_export.h"

class KoColorSpace;

/**
 * User settings of the GPU engine (Solstice, docs/agent/gpu-engine.md),
 * stored in kritarc.
 *
 * - Solstice/GpuEngine (bool, default false): use the GPU engine. Read once
 *   per process (KisGpuMergeBatch::isEnabled()), so a change takes effect
 *   after restarting Krita. The KRITA_GPU_PROJECTION environment variable
 *   (1 or 0) overrides it for development.
 * - Solstice/GpuBrush (bool, default true): paint brush dabs on the GPU
 *   while the GPU engine is in use. Read once per process
 *   (KisGpuBrushPainter::isEnabled()); the KRITA_GPU_BRUSH environment
 *   variable (1 or 0) overrides it. Unsupported strokes use the CPU.
 * - Solstice/GpuEngineConvertDocuments (int, ConvertPolicy, default Ask):
 *   what to do with a document that is opened in another color space than
 *   RGBA float while the GPU engine is enabled.
 * - Solstice/GpuEngineUnverifiedDevice (bool, default false): also use the
 *   engine with a graphics card it was not verified with
 *   (KisGpuDeviceInfo::isVerified(): NVIDIA Blackwell). Read when the
 *   engine starts; the KRITA_GPU_ALLOW_UNVERIFIED_DEVICE=1 environment
 *   variable allows such cards too.
 */
class KRITAIMAGE_EXPORT KisGpuEngineSettings
{
public:
    enum ConvertPolicy {
        Ask = 0,
        Convert = 1,
        Keep = 2,
        Convert16 = 3, // Keep persisted values 0/1/2 compatible.
    };

    static QString enabledKey();
    static QString convertPolicyKey();
    static QString brushKey();
    static QString unverifiedDeviceKey();

    static bool enabledInConfig();
    static void setEnabledInConfig(bool enabled);

    static bool brushEnabledInConfig();
    static void setBrushEnabledInConfig(bool enabled);

    static bool unverifiedDeviceAllowedInConfig();
    static void setUnverifiedDeviceAllowedInConfig(bool allowed);
    /// The setting, or KRITA_GPU_ALLOW_UNVERIFIED_DEVICE=1.
    static bool unverifiedDeviceAllowed();

    static ConvertPolicy convertPolicy();
    static void setConvertPolicy(ConvertPolicy policy);

    /// True if the GPU engine composites documents in @p colorSpace (RGBA F32/F16).
    static bool isGpuColorSpace(const KoColorSpace *colorSpace);

    /**
     * The color space a document in @p colorSpace is converted to for the
     * GPU engine: RGBA F32 with the same profile for RGB documents (the
     * values and their appearance stay the same), RGBA F32 with the default
     * profile for other color models. Null if @p colorSpace is already a GPU
     * color space.
     */
    static const KoColorSpace *conversionTarget(const KoColorSpace *colorSpace);
    /// As above, but Convert16 selects RGBA F16; other policies retain F32.
    static const KoColorSpace *conversionTarget(const KoColorSpace *colorSpace, ConvertPolicy policy);
};

#endif // KISGPUENGINESETTINGS_H
