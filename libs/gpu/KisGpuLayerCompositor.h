/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPULAYERCOMPOSITOR_H
#define KISGPULAYERCOMPOSITOR_H

#include <QRect>
#include <QString>
#include <QVector>

#include <memory>

#include "KisGpuTilePool.h"
#include "KisGpuVulkanFunctions.h"
#include "kritagpu_export.h"

class KisGpuContext;
class KisGpuCommandList;
class KisGpuBuffer;
class KisGpuComputePipeline;

/// Blend modes of shaders/composite_layers.comp (values are shader constants).
enum class KisGpuBlendOp : quint32 {
    Over = 0,
    Multiply = 1,
    Screen = 2,
    Add = 3,
    Subtract = 4,
    Darken = 5,
    Lighten = 6,
    Difference = 7,
    Overlay = 8,
    HardLight = 9,
    Exclusion = 10,
    Erase = 11, // Brush compositing only; not exposed by the layer blend-op mapping.
    LinearBurn = 12,
    LinearLight = 13,
    PinLight = 14,
    SoftLightSvg = 15,
    SoftLightPhotoshop = 16,
    ColorDodge = 17,
    ColorBurn = 18,
    Divide = 19,
    VividLight = 20,
    HardMix = 21,
    HardMixPhotoshop = 22,
    HardMixSofterPhotoshop = 23,
    GrainMerge = 24,
    GrainExtract = 25,
    Negation = 26,
    Allanon = 27,
    Hue = 28,
    Saturation = 29,
    Color = 30,
    Luminosity = 31,
    DarkerColor = 32,
    LighterColor = 33,
    // Phase 4.107: the other HSX modes (composite_blend.glsl decodes the order)
    Tint = 34,
    IncreaseSaturation = 35,
    DecreaseSaturation = 36,
    IncreaseLuminosity = 37,
    DecreaseLuminosity = 38,
    ColorHsi = 39,
    HueHsi = 40,
    SaturationHsi = 41,
    IncreaseSaturationHsi = 42,
    DecreaseSaturationHsi = 43,
    Intensity = 44,
    IncreaseIntensity = 45,
    DecreaseIntensity = 46,
    ColorHsl = 47,
    HueHsl = 48,
    SaturationHsl = 49,
    IncreaseSaturationHsl = 50,
    DecreaseSaturationHsl = 51,
    Lightness = 52,
    IncreaseLightness = 53,
    DecreaseLightness = 54,
    ColorHsv = 55,
    HueHsv = 56,
    SaturationHsv = 57,
    IncreaseSaturationHsv = 58,
    DecreaseSaturationHsv = 59,
    Value = 60,
    IncreaseValue = 61,
    DecreaseValue = 62,
    Count,
};

/// F16 brush modes covered by the dab and indirect-painting parity matrices.
inline bool kisGpuSupportsHalfBrushBlend(KisGpuBlendOp op)
{
    // Every mode since phase 4.107.
    return op < KisGpuBlendOp::Count;
}

/**
 * Composites layers bottom to top onto destination tiles, writing only the
 * pixels inside a clip rect (GPU engine phase 2, projection).
 *
 * Tiles form a grid of gridWidth columns; tile t covers grid pixels
 * ((t % gridWidth) * 64, (t / gridWidth) * 64) .. +64. The clip rect is in
 * the same grid pixel coordinates.
 *
 * Tables live in an internal host-visible buffer: do not record a new pass
 * while the previous one is still executing.
 */
class KRITAGPU_EXPORT KisGpuLayerCompositor
{
public:
    struct Mask {
        const quint8 *data = nullptr;
        QRect bounds; // Tightly packed coverage bytes, in tile-grid pixel coordinates.
    };
    struct Layer {
        KisGpuBlendOp op = KisGpuBlendOp::Over;
        float opacity = 1.0f;
        bool alphaLocked = false;
        quint32 channelMask = 0xf; // RGBA bits; partial channels are used by brushes.
        bool halfBrush = false; // F16 Normal/Erase scalar arithmetic, for Wash only
        bool explicitChannelFlags = false;
        /// Copy the layer's pixels (@p fill where it has no tile) instead of
        /// blending, ignoring the other fields and coverage (COMPOSITE_COPY base copy).
        bool replace = false;
        float fill[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // tile memory order, as float
    };

    ~KisGpuLayerCompositor();

    static std::unique_ptr<KisGpuLayerCompositor>
    create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage = nullptr);

    /**
     * Compiles the context's shared pipelines for @p format (basic and, with
     * @p extended, the extended blend modes) so a later create()/record()
     * does not wait for compilation. Thread-safe; returns false on failure.
     */
    static bool
    preparePipelines(KisGpuContext &context, KisGpuTileFormat format, bool extended, QString *errorMessage = nullptr);

    /**
     * @param layerTiles layer-major, layerTiles[layer * tileCount + tile]; 0 skips the tile
     * @param dstTiles destination tiles (read and written)
     * @param mask optional single-layer coverage; copied into the
     * internal table buffer before return, with a 16 MiB limit
     */
    bool record(KisGpuCommandList &commands,
                const QVector<VkDeviceAddress> &layerTiles,
                const QVector<VkDeviceAddress> &dstTiles,
                const QVector<Layer> &layers,
                int gridWidth,
                const QRect &clip,
                QString *errorMessage = nullptr,
                const Mask *mask = nullptr);

private:
    explicit KisGpuLayerCompositor(KisGpuContext &context);

    KisGpuContext &m_context;
    // Shared by all compositors of the context (KisGpuContext::sharedComputePipeline).
    std::shared_ptr<KisGpuComputePipeline> m_pipeline;
    std::shared_ptr<KisGpuComputePipeline> m_extendedPipeline;
    bool m_f16 = false;
    std::unique_ptr<KisGpuBuffer> m_tables;
};

#endif // KISGPULAYERCOMPOSITOR_H
