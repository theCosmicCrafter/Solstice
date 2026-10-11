/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisGpuDabCompositor.h"
#include "KisGpuBuffer.h"
#include "KisGpuCommandList.h"
#include "KisGpuComputePipeline.h"
#include "KisGpuContext.h"
#include <QHash>
#include <QSet>
#include <algorithm>
#include <cstring>
namespace
{
const quint32 Shader[] = {
#include "paint_dabs.spv.inc"
};
const quint32 ExtendedShader[] = {
#include "paint_dabs_extended.spv.inc"
};
const quint32 TexturedShader[] = {
#include "paint_dabs_textured.spv.inc"
};
const quint32 TexturedExtendedShader[] = {
#include "paint_dabs_textured_extended.spv.inc"
};
const quint32 HalfShader[] = {
#include "paint_dabs_rgba16f.spv.inc"
};
const quint32 HalfExtendedShader[] = {
#include "paint_dabs_extended_rgba16f.spv.inc"
};
struct DabRecord {
    VkDeviceAddress pixels;
    qint32 x, y, width, height;
    float opacity;
    float flow, averageOpacity;
    quint32 mirrorFlags, padding[2];
    qint32 clipX, clipY, clipWidth, clipHeight;
};
struct TileRecord {
    VkDeviceAddress pixels;
    qint32 x, y;
};
struct PushConstants {
    VkDeviceAddress tiles, dabs;
    quint32 gridWidth, dabCount;
    qint32 x, y;
    quint32 mode, channelMask;
    VkDeviceAddress mask;
    qint32 maskX, maskY, maskWidth, maskHeight;
};
static_assert(sizeof(DabRecord) == 64 && sizeof(PushConstants) == 64, "shader layout");
static_assert(sizeof(KisGpuDabCompositor::Circle) == 224, "shader circle layout");
constexpr quint32 GeneratedDabFlag = 4;
static_assert(sizeof(TileRecord) == 16, "shader tile layout");
VkDeviceSize aligned(VkDeviceSize bytes)
{
    return (bytes + 15) & ~VkDeviceSize(15);
}
struct UploadLayout {
    VkDeviceSize tableOffset = 0, maskOffset = 0, maskBytes = 0, capacity = 0;
    QVector<VkDeviceSize> offsets;
    QHash<const void *, VkDeviceSize> sourceOffsets;
    QHash<const float *, VkDeviceSize> tableOffsets; // Soft curve tables and texture tables
    QHash<const quint8 *, VkDeviceSize> textureOffsets; // pattern textures
    QHash<const void *, VkDeviceSize> imageOffsets; // image tip levels and row tables
};
bool planUpload(int tileCount,
                const QVector<KisGpuDabCompositor::Dab> &dabs,
                const KisGpuDabCompositor::Mask *mask,
                UploadLayout &layout,
                QString *error,
                int pixelSize)
{
    if ((pixelSize != 8 && pixelSize != 16) || tileCount <= 0 || tileCount > 65535 || dabs.isEmpty()
        || dabs.size() > 65536)
        return false;
    layout.tableOffset = aligned(VkDeviceSize(tileCount) * sizeof(TileRecord));
    VkDeviceSize bytes = layout.tableOffset + VkDeviceSize(dabs.size()) * sizeof(DabRecord);
    // Consecutive identical dabs commonly share their fixed paint device.
    QHash<const void *, QSize> sourceSizes;
    for (const auto &dab : dabs) {
        if (dab.generated) {
            // Parameters instead of pixels, for the destination format.
            if ((pixelSize == 8) != (dab.circle.halfPixels != 0) || dab.size.isEmpty() || dab.mirrorFlags > 3)
                return false;
            if (dab.circle.kind == 2) {
                // The kernel reads index + 1 for distances up to 1.
                if (!dab.curveTable || dab.curveTableSize < int(dab.circle.curveResolution) + 2)
                    return false;
            }
            const bool textured = dab.texture != nullptr;
            if (textured
                && (pixelSize != 16 || !dab.textureLut || dab.circle.textureWidth == 0 || dab.circle.textureHeight == 0
                    || dab.circle.textureMode > 13 || dab.circle.textureBrushFlips > 3))
                return false;
            const bool imageTip = dab.circle.kind == 3;
            if (imageTip) {
                // RGBA32F; identity tips read texel (x + 1, y + 1) of the level.
                const KisGpuDabCompositor::Circle &c = dab.circle;
                const int tableSize = c.imageApplication == 3 ? 1536 : 1024;
                if (c.imageApplication > 3
                    || (c.imageApplication != 0 && (!dab.imageTable || dab.imageTableSize < tableSize)))
                    return false;
                if (pixelSize != 16 || !dab.textureLut || !dab.imageTexels || c.imageWidth < 2 || c.imageHeight < 2
                    || qint64(c.imageWidth) * c.imageHeight > qint64(KisGpuDabCompositor::MaxUploadBytes / 4)
                    || (c.imageIdentity ? dab.size != QSize(c.imageWidth - 2, c.imageHeight - 2) : !dab.imageRows))
                    return false;
            } else if (dab.circle.kind > 2) {
                return false;
            }
            layout.offsets << bytes;
            bytes += sizeof(KisGpuDabCompositor::Circle);
            if (imageTip && !layout.imageOffsets.contains(dab.imageTexels)) {
                layout.imageOffsets.insert(dab.imageTexels, bytes);
                bytes += aligned(VkDeviceSize(dab.circle.imageWidth) * dab.circle.imageHeight * 4);
            }
            if (imageTip && !dab.circle.imageIdentity && !layout.imageOffsets.contains(dab.imageRows)) {
                layout.imageOffsets.insert(dab.imageRows, bytes);
                bytes += aligned(VkDeviceSize(dab.size.height()) * 16);
            }
            if (imageTip && !layout.tableOffsets.contains(dab.textureLut)) {
                layout.tableOffsets.insert(dab.textureLut, bytes);
                bytes += aligned(256 * sizeof(float));
            }
            if (imageTip && dab.circle.imageApplication != 0 && !layout.tableOffsets.contains(dab.imageTable)) {
                layout.tableOffsets.insert(dab.imageTable, bytes);
                bytes += aligned(VkDeviceSize(dab.imageTableSize) * sizeof(float));
            }
            if (dab.circle.kind == 2 && !layout.tableOffsets.contains(dab.curveTable)) {
                layout.tableOffsets.insert(dab.curveTable, bytes);
                bytes += aligned(VkDeviceSize(dab.curveTableSize) * sizeof(float));
            }
            if (textured && !layout.textureOffsets.contains(dab.texture)) {
                layout.textureOffsets.insert(dab.texture, bytes);
                // The shader reads packed uint words, including the last partial word.
                bytes += aligned(VkDeviceSize(dab.circle.textureWidth) * dab.circle.textureHeight);
            }
            if (textured && !layout.tableOffsets.contains(dab.textureLut)) {
                layout.tableOffsets.insert(dab.textureLut, bytes);
                bytes += aligned(256 * sizeof(float));
            }
            continue;
        }
        if (!dab.pixels || dab.size.isEmpty() || dab.mirrorFlags > 3
            || quint64(dab.size.width()) * dab.size.height() > KisGpuDabCompositor::MaxUploadBytes / pixelSize)
            return false;
        if (sourceSizes.contains(dab.pixels) && sourceSizes.value(dab.pixels) != dab.size)
            return false;
        sourceSizes.insert(dab.pixels, dab.size);
        if (!layout.sourceOffsets.contains(dab.pixels)) {
            layout.sourceOffsets.insert(dab.pixels, bytes);
            bytes += aligned(VkDeviceSize(dab.size.width()) * dab.size.height() * pixelSize);
        }
        layout.offsets << layout.sourceOffsets.value(dab.pixels);
    }
    layout.maskOffset = bytes;

    if (mask) {
        if (!mask->pixels || mask->bounds.isEmpty())
            return false;
        layout.maskBytes = VkDeviceSize(mask->bounds.width()) * mask->bounds.height();
        // The shader reads packed uint words, including the last partial word.
        bytes += aligned(layout.maskBytes);
    }
    if (bytes > KisGpuDabCompositor::MaxUploadBytes) {
        if (error)
            *error = QStringLiteral("GPU brush staging budget reached");
        return false;
    }

    constexpr VkDeviceSize quantum = 256 * 1024;
    layout.capacity = (bytes + quantum - 1) & ~(quantum - 1);
    return true;
}
} // namespace
KisGpuDabCompositor::KisGpuDabCompositor(KisGpuContext &context)
    : m_context(context)
{
}
KisGpuDabCompositor::~KisGpuDabCompositor() = default;
quint64 KisGpuDabCompositor::requiredUploadBytes(int tileCount,
                                                 const QVector<Dab> &dabs,
                                                 const Mask *mask,
                                                 QString *error,
                                                 int pixelSize)
{
    UploadLayout layout;
    return planUpload(tileCount, dabs, mask, layout, error, pixelSize) ? layout.capacity : 0;
}
quint64 KisGpuDabCompositor::uploadBytes() const
{
    return m_upload ? m_upload->size() : 0;
}
void KisGpuDabCompositor::releaseUpload()
{
    m_upload.reset();
}
std::unique_ptr<KisGpuDabCompositor> KisGpuDabCompositor::create(KisGpuContext &context, QString *error)
{
    std::unique_ptr<KisGpuDabCompositor> result(new KisGpuDabCompositor(context));
    result->m_pipeline = context.sharedComputePipeline(Shader, sizeof(Shader), sizeof(PushConstants), error);
    return result->m_pipeline ? std::move(result) : nullptr;
}
bool KisGpuDabCompositor::preparePipelines(KisGpuContext &context, QString *error)
{
    return context.sharedComputePipeline(Shader, sizeof(Shader), sizeof(PushConstants), error)
        && context.sharedComputePipeline(ExtendedShader, sizeof(ExtendedShader), sizeof(PushConstants), error)
        && context.sharedComputePipeline(HalfShader, sizeof(HalfShader), sizeof(PushConstants), error)
        && context.sharedComputePipeline(HalfExtendedShader, sizeof(HalfExtendedShader), sizeof(PushConstants), error)
        && context.sharedComputePipeline(TexturedShader, sizeof(TexturedShader), sizeof(PushConstants), error)
        && context.sharedComputePipeline(TexturedExtendedShader,
                                         sizeof(TexturedExtendedShader),
                                         sizeof(PushConstants),
                                         error);
}
bool KisGpuDabCompositor::record(KisGpuCommandList &commands,
                                 const QVector<VkDeviceAddress> &tiles,
                                 int gridWidth,
                                 QPoint origin,
                                 const QVector<Dab> &dabs,
                                 CompositeMode mode,
                                 const Mask *mask,
                                 quint32 channelMask,
                                 QString *error,
                                 const QVector<QPoint> &tileOrigins,
                                 int pixelSize)
{
    if (tiles.isEmpty() || dabs.isEmpty())
        return true;
    if (gridWidth <= 0 || tiles.size() > 65535)
        return false;
    if (!tileOrigins.isEmpty() && tileOrigins.size() != tiles.size())
        return false;
    const bool half = pixelSize == 8;
    if (half && mode > CompositeMode::PinLight && mode != CompositeMode::SoftLightSvg
        && mode != CompositeMode::ColorDodge && mode != CompositeMode::ColorBurn
        && !(mode >= CompositeMode::Hue && mode <= CompositeMode::Luminosity))
        return false;
    if (quint32(mode) >= quint32(CompositeMode::Count) || channelMask > (half ? 0x1fu : 0xfu)
        || ((mode == CompositeMode::AlphaDarkenHard || mode == CompositeMode::AlphaDarkenCreamy)
            && (channelMask & 15) != 0xf))
        return false;
    UploadLayout layout;
    if (!planUpload(tiles.size(), dabs, mask, layout, error, pixelSize))
        return false;
    const bool extended = mode >= CompositeMode::SoftLightSvg;
    // Textured and image tip dabs (kind 3) use the DAB_TEXTURE builds.
    const bool textured = std::any_of(dabs.cbegin(), dabs.cend(), [](const Dab &dab) {
        return dab.generated && (dab.texture || dab.circle.kind == 3);
    });
    if (textured && !extended && !m_texturedPipeline) {
        m_texturedPipeline =
            m_context.sharedComputePipeline(TexturedShader, sizeof(TexturedShader), sizeof(PushConstants), error);
        if (!m_texturedPipeline)
            return false;
    }
    if (textured && extended && !m_texturedExtendedPipeline) {
        m_texturedExtendedPipeline = m_context.sharedComputePipeline(TexturedExtendedShader,
                                                                     sizeof(TexturedExtendedShader),
                                                                     sizeof(PushConstants),
                                                                     error);
        if (!m_texturedExtendedPipeline)
            return false;
    }
    if (half && extended && !m_halfExtendedPipeline) {
        m_halfExtendedPipeline = m_context.sharedComputePipeline(HalfExtendedShader,
                                                                 sizeof(HalfExtendedShader),
                                                                 sizeof(PushConstants),
                                                                 error);
        if (!m_halfExtendedPipeline)
            return false;
    }
    if (half && !extended && !m_halfPipeline) {
        m_halfPipeline = m_context.sharedComputePipeline(HalfShader, sizeof(HalfShader), sizeof(PushConstants), error);
        if (!m_halfPipeline)
            return false;
    }
    if (!half && extended && !m_extendedPipeline) {
        m_extendedPipeline =
            m_context.sharedComputePipeline(ExtendedShader, sizeof(ExtendedShader), sizeof(PushConstants), error);
        if (!m_extendedPipeline)
            return false;
    }
    const auto bytes = layout.capacity;
    const auto tableOffset = layout.tableOffset;
    const auto maskOffset = layout.maskOffset;
    const auto maskBytes = layout.maskBytes;
    const auto &offsets = layout.offsets;
    auto &sourceOffsets = layout.sourceOffsets;
    if (!m_upload || m_upload->size() < bytes) {
        // Previous work has completed; do not temporarily hold both allocations.
        m_upload.reset();
        m_upload = KisGpuBuffer::create(m_context, bytes, KisGpuBuffer::Location::Upload, error);
        if (!m_upload)
            return false;
    }
    auto *out = static_cast<quint8 *>(m_upload->mapped());
    if (mask) {
        std::memset(out + maskOffset, 0, size_t(aligned(maskBytes)));
        std::memcpy(out + maskOffset, mask->pixels, size_t(maskBytes));
    }
    auto *tileRecords = reinterpret_cast<TileRecord *>(out);
    for (int i = 0; i < tiles.size(); ++i) {
        const QPoint tileOrigin =
            tileOrigins.isEmpty() ? origin + QPoint(i % gridWidth, i / gridWidth) * 64 : tileOrigins[i];
        tileRecords[i] = {tiles[i], tileOrigin.x(), tileOrigin.y()};
    }
    auto *records = reinterpret_cast<DabRecord *>(out + tableOffset);
    QSet<const float *> copiedTables;
    QSet<const quint8 *> copiedTextures;
    QSet<const void *> copiedImages;
    for (int i = 0; i < dabs.size(); ++i) {
        const auto &dab = dabs[i];
        const QRect clip = dab.clip.isNull() ? QRect(dab.origin, dab.size) : dab.clip;
        records[i] = {m_upload->deviceAddress() + offsets[i],
                      dab.origin.x(),
                      dab.origin.y(),
                      dab.size.width(),
                      dab.size.height(),
                      dab.opacity,
                      dab.flow,
                      dab.averageOpacity,
                      dab.mirrorFlags | (dab.generated ? GeneratedDabFlag : 0),
                      {0, 0},
                      clip.x(),
                      clip.y(),
                      clip.width(),
                      clip.height()};
        if (dab.generated) {
            KisGpuDabCompositor::Circle circle = dab.circle;
            if (circle.kind == 2) {
                const VkDeviceSize tableOffset = layout.tableOffsets.value(dab.curveTable);
                circle.curveTable = m_upload->deviceAddress() + tableOffset;
                // Shared tables are copied once; every record keeps its address.
                if (!copiedTables.contains(dab.curveTable)) {
                    copiedTables.insert(dab.curveTable);
                    std::memcpy(out + tableOffset, dab.curveTable, size_t(dab.curveTableSize) * sizeof(float));
                }
            }
            if (dab.texture) {
                const VkDeviceSize textureOffset = layout.textureOffsets.value(dab.texture);
                const VkDeviceSize lutOffset = layout.tableOffsets.value(dab.textureLut);
                circle.texture = m_upload->deviceAddress() + textureOffset;
                circle.textureLut = m_upload->deviceAddress() + lutOffset;
                if (!copiedTextures.contains(dab.texture)) {
                    copiedTextures.insert(dab.texture);
                    const size_t textureBytes = size_t(circle.textureWidth) * circle.textureHeight;
                    std::memset(out + textureOffset, 0, size_t(aligned(textureBytes)));
                    std::memcpy(out + textureOffset, dab.texture, textureBytes);
                }
                if (!copiedTables.contains(dab.textureLut)) {
                    copiedTables.insert(dab.textureLut);
                    std::memcpy(out + lutOffset, dab.textureLut, 256 * sizeof(float));
                }
            } else {
                circle.texture = 0;
                circle.textureLut = 0;
            }
            if (circle.kind == 3) {
                const VkDeviceSize texelOffset = layout.imageOffsets.value(dab.imageTexels);
                const VkDeviceSize lutOffset = layout.tableOffsets.value(dab.textureLut);
                circle.imageTexels = m_upload->deviceAddress() + texelOffset;
                circle.textureLut = m_upload->deviceAddress() + lutOffset;
                if (!copiedImages.contains(dab.imageTexels)) {
                    copiedImages.insert(dab.imageTexels);
                    std::memcpy(out + texelOffset,
                                dab.imageTexels,
                                size_t(circle.imageWidth) * size_t(circle.imageHeight) * 4);
                }
                if (!circle.imageIdentity) {
                    const VkDeviceSize rowsOffset = layout.imageOffsets.value(dab.imageRows);
                    circle.imageRows = m_upload->deviceAddress() + rowsOffset;
                    if (!copiedImages.contains(dab.imageRows)) {
                        copiedImages.insert(dab.imageRows);
                        std::memcpy(out + rowsOffset, dab.imageRows, size_t(dab.size.height()) * 16);
                    }
                } else {
                    circle.imageRows = 0;
                }
                if (!copiedTables.contains(dab.textureLut)) {
                    copiedTables.insert(dab.textureLut);
                    std::memcpy(out + lutOffset, dab.textureLut, 256 * sizeof(float));
                }
                if (circle.imageApplication != 0) {
                    const VkDeviceSize tableOffset = layout.tableOffsets.value(dab.imageTable);
                    circle.imageTable = m_upload->deviceAddress() + tableOffset;
                    if (!copiedTables.contains(dab.imageTable)) {
                        copiedTables.insert(dab.imageTable);
                        std::memcpy(out + tableOffset, dab.imageTable, size_t(dab.imageTableSize) * sizeof(float));
                    }
                } else {
                    circle.imageTable = 0;
                }
            } else {
                circle.imageTexels = 0;
                circle.imageRows = 0;
                circle.imageTable = 0;
            }
            std::memcpy(out + offsets[i], &circle, sizeof(circle));
        } else if (sourceOffsets.remove(dab.pixels)) {
            std::memcpy(out + offsets[i], dab.pixels, size_t(dab.size.width()) * dab.size.height() * pixelSize);
        }
    }
    PushConstants params{m_upload->deviceAddress(),
                         m_upload->deviceAddress() + tableOffset,
                         quint32(gridWidth),
                         quint32(dabs.size()),
                         origin.x(),
                         origin.y(),
                         quint32(mode),
                         channelMask,
                         mask ? m_upload->deviceAddress() + maskOffset : 0,
                         mask ? mask->bounds.x() : 0,
                         mask ? mask->bounds.y() : 0,
                         mask ? mask->bounds.width() : 0,
                         mask ? mask->bounds.height() : 0};
    commands.computeBarrier();
    (half           ? (extended ? m_halfExtendedPipeline : m_halfPipeline)
         : textured ? (extended ? m_texturedExtendedPipeline : m_texturedPipeline)
         : extended ? m_extendedPipeline
                    : m_pipeline)
        ->dispatch(commands.commandBuffer(), params, quint32(tiles.size()));
    return true;
}
