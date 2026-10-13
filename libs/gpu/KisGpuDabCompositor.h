/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KISGPUDABCOMPOSITOR_H
#define KISGPUDABCOMPOSITOR_H
#include "KisGpuVulkanFunctions.h"
#include "kritagpu_export.h"
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <memory>
class KisGpuContext;
class KisGpuCommandList;
class KisGpuComputePipeline;
class KisGpuBuffer;
class KRITAGPU_EXPORT KisGpuDabCompositor
{
public:
    static constexpr quint64 MaxUploadBytes = quint64(64) << 20;
    enum class CompositeMode : quint32 {
        Normal,
        AlphaDarkenHard,
        AlphaDarkenCreamy,
        Erase,
        Multiply,
        Screen,
        Add,
        Subtract,
        Darken,
        Lighten,
        Difference,
        Overlay,
        HardLight,
        Exclusion,
        LinearBurn,
        LinearLight,
        PinLight,
        SoftLightSvg,
        SoftLightPhotoshop,
        ColorDodge,
        ColorBurn,
        Divide,
        VividLight,
        HardMix,
        HardMixPhotoshop,
        HardMixSofterPhotoshop,
        GrainMerge,
        GrainExtract,
        Negation,
        Allanon,
        Hue,
        Saturation,
        Color,
        Luminosity,
        DarkerColor,
        LighterColor,
        Tint,
        IncreaseSaturation,
        DecreaseSaturation,
        IncreaseLuminosity,
        DecreaseLuminosity,
        ColorHsi,
        HueHsi,
        SaturationHsi,
        IncreaseSaturationHsi,
        DecreaseSaturationHsi,
        Intensity,
        IncreaseIntensity,
        DecreaseIntensity,
        ColorHsl,
        HueHsl,
        SaturationHsl,
        IncreaseSaturationHsl,
        DecreaseSaturationHsl,
        Lightness,
        IncreaseLightness,
        DecreaseLightness,
        ColorHsv,
        HueHsv,
        SaturationHsv,
        IncreaseSaturationHsv,
        DecreaseSaturationHsv,
        Value,
        IncreaseValue,
        DecreaseValue,
        Count
    };
    /**
     * A default or Gaussian circle auto-brush dab evaluated on the GPU
     * instead of uploaded pixels (RGBA32F or RGBA16F; KisProceduralCircleDab
     * in kritaimage defines the math), optionally textured (RGBA32F;
     * KisProceduralDabTexture), or an image tip (kind 3, RGBA32F;
     * KisImageTipSampler). Shader layout: 224 bytes.
     */
    struct Circle {
        float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float centerX = 0.0f, centerY = 0.0f;
        float cosa = 1.0f, sina = 0.0f;
        // kind 0: default circle
        float xcoef = 0.0f, ycoef = 0.0f; // ycoef is shared with kind 1
        float fadeX = 0.0f, fadeY = 0.0f;
        // kind 1: Gaussian circle
        float distfactor = 0.0f, center = 0.0f, alphafactor = 0.0f, radius = 0.0f;
        float fadeStart = 0.0f, fadeStartValue = 0.0f, fadeCoeff = 0.0f;
        quint32 kind = 0; // 0 default, 1 Gaussian, 2 Soft (curve), 3 image tip
        quint32 antialias = 0;
        quint32 halfPixels = 0; // RGBA16F: alpha rounded to half like Imath
        VkDeviceAddress curveTable = 0; // kind 2: set by record()
        float curveResolution = 0.0f; // kind 2
        quint32 padding1[3] = {0, 0, 0};
        // Pattern texture: set by record() from Dab::texture; zero means none.
        VkDeviceAddress texture = 0;
        VkDeviceAddress textureLut = 0;
        qint32 textureOriginX = 0, textureOriginY = 0;
        quint32 textureWidth = 0, textureHeight = 0;
        quint32 textureMode = 0, textureSoft = 0; // KisMaskingBrushCompositeFuncTypes
        float textureStrength = 0.0f, textureInvertedStrength = 0.0f;
        float textureWeight = 0.0f;
        quint32 textureBrushFlips = 0;
        quint32 padding2[2] = {0, 0};
        // Image tip (kind 3): addresses set by record() from Dab::imageTexels
        // and Dab::imageRows. The level is imageWidth x imageHeight ARGB32
        // texels; rows are (x, length, fx, fy) per dab row, unused for an
        // identity tip (the level's interior).
        VkDeviceAddress imageTexels = 0;
        VkDeviceAddress imageRows = 0;
        qint32 imageWidth = 0, imageHeight = 0;
        qint32 imageFdx = 0, imageFdy = 0;
        quint32 imageIdentity = 0;
        // KisProceduralCircleDab::ImageApplication; the table address is set
        // by record() from Dab::imageTable (unused for the alpha mask)
        quint32 imageApplication = 0;
        VkDeviceAddress imageTable = 0;
    };
    struct Dab {
        const void *pixels = nullptr; // RGBA32F, or RGBA16F when pixelSize is 8; unused when generated
        QPoint origin;
        QSize size;
        float opacity = 1.0f;
        float flow = 1.0f;
        float averageOpacity = 0.0f;
        quint32 mirrorFlags = 0; // source-coordinate reflection: horizontal=1, vertical=2
        QRect clip; // document coordinates; null means the entire dab
        bool generated = false; // evaluate circle in the size x size dab instead of reading pixels
        Circle circle;
        const float *curveTable = nullptr; // Soft kind: uploaded once per distinct pointer
        int curveTableSize = 0;
        // Textured generated dab: alpha8 texels (circle.textureWidth x
        // textureHeight) and the 256-entry byte-to-float table
        // (KoLuts::Uint8ToFloat), each uploaded once per distinct pointer.
        const quint8 *texture = nullptr;
        const float *textureLut = nullptr;
        // Image tip (kind 3, "Image tip dabs"): the ARGB32 level texels and
        // size.height() rows of 4 ints, each uploaded once per distinct
        // pointer; textureLut is the byte-to-float table.
        const quint32 *imageTexels = nullptr;
        const qint32 *imageRows = nullptr;
        // the application's table (1024 floats, 1536 for the gradient map)
        const float *imageTable = nullptr;
        int imageTableSize = 0;
    };
    struct Mask {
        const quint8 *pixels;
        QRect bounds; // contiguous 8-bit coverage in document coordinates
    };
    ~KisGpuDabCompositor();
    static std::unique_ptr<KisGpuDabCompositor> create(KisGpuContext &context, QString *error = nullptr);
    /// Compiles the context's shared F32/F16, basic/extended dab pipelines
    /// ahead of use (phase 4.91). Thread-safe; false on failure.
    static bool preparePipelines(KisGpuContext &context, QString *error = nullptr);
    /// Required reusable allocation, rounded to 256 KiB; zero on invalid/oversized input.
    static quint64 requiredUploadBytes(int tileCount,
                                       const QVector<Dab> &dabs,
                                       const Mask *mask = nullptr,
                                       QString *error = nullptr,
                                       int pixelSize = 16);
    quint64 uploadBytes() const;
    /// Caller must first wait for the last recording to finish.
    void releaseUpload();
    /// RGBA32F modes, or the supported major RGBA16F modes. Wait before reusing this object.
    /// F16 channelMask bit 4 distinguishes explicit flags from an empty set.
    /// The host-visible source/table/mask buffer is capped at 64 MiB.
    /// Optional origins select sparse, distinct destination tiles instead of a
    /// rectangular grid. There must be exactly one document origin per address.
    bool record(KisGpuCommandList &commands,
                const QVector<VkDeviceAddress> &tiles,
                int gridWidth,
                QPoint origin,
                const QVector<Dab> &dabs,
                CompositeMode mode,
                const Mask *mask = nullptr,
                quint32 channelMask = 0xf,
                QString *error = nullptr,
                const QVector<QPoint> &tileOrigins = {},
                int pixelSize = 16);

private:
    explicit KisGpuDabCompositor(KisGpuContext &context);
    KisGpuContext &m_context;
    // Shared by all compositors of the context (KisGpuContext::sharedComputePipeline).
    std::shared_ptr<KisGpuComputePipeline> m_pipeline;
    std::shared_ptr<KisGpuComputePipeline> m_extendedPipeline;
    std::shared_ptr<KisGpuComputePipeline> m_halfPipeline;
    std::shared_ptr<KisGpuComputePipeline> m_halfExtendedPipeline;
    // Textured dabs (phase 4.102), RGBA32F
    std::shared_ptr<KisGpuComputePipeline> m_texturedPipeline;
    std::shared_ptr<KisGpuComputePipeline> m_texturedExtendedPipeline;
    // RGBA16F image tips (and textures), phase 4.105
    std::shared_ptr<KisGpuComputePipeline> m_halfTexturedPipeline;
    std::shared_ptr<KisGpuComputePipeline> m_halfTexturedExtendedPipeline;
    std::unique_ptr<KisGpuBuffer> m_upload;
};
#endif
