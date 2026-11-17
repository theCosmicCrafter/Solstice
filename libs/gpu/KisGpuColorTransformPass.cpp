/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuColorTransformPass.h"

#include "KisGpuBuffer.h"
#include "KisGpuCommandList.h"
#include "KisGpuComputePipeline.h"
#include "KisGpuContext.h"

#include <cstring>

namespace
{
const quint32 ColorTransformRgba32f[] = {
#include "color_transform_rgba32f.spv.inc"
};

const quint32 ColorTransformRgba16f[] = {
#include "color_transform_rgba16f.spv.inc"
};

struct PushConstants {
    VkDeviceAddress dstTiles;
    VkDeviceAddress srcTiles;
    VkDeviceAddress mask;
    VkDeviceAddress tables;
    quint32 dstTileCount;
    quint32 dstGridWidth;
    quint32 srcGridWidth;
    quint32 srcGridHeight;
    qint32 clip[4];
    qint32 shift[2];
    qint32 maskOrigin[2];
    qint32 maskSize[2];
    quint32 kind;
    quint32 padding;
    float fill[4];
};
static_assert(sizeof(PushConstants) == 112, "must match the shader push constant block");

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

KisGpuColorTransformPass::KisGpuColorTransformPass(KisGpuContext &context, bool f16)
    : m_context(context)
    , m_f16(f16)
{
}

KisGpuColorTransformPass::~KisGpuColorTransformPass() = default;

std::unique_ptr<KisGpuColorTransformPass>
KisGpuColorTransformPass::create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage)
{
    const bool f16 = format == KisGpuTileFormat::RGBA16F;
    std::unique_ptr<KisGpuColorTransformPass> pass(new KisGpuColorTransformPass(context, f16));
    pass->m_pipeline =
        context.sharedComputePipeline(f16 ? ColorTransformRgba16f : ColorTransformRgba32f,
                                      f16 ? sizeof(ColorTransformRgba16f) : sizeof(ColorTransformRgba32f),
                                      sizeof(PushConstants),
                                      errorMessage);
    return pass->m_pipeline ? std::move(pass) : nullptr;
}

bool KisGpuColorTransformPass::record(KisGpuCommandList &commands, const Pass &pass, QString *errorMessage)
{
    auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    if (pass.dstTiles.isEmpty() || pass.clip.isEmpty()) {
        return true;
    }
    if (pass.dstGridWidth <= 0 || pass.srcGridWidth < 0 || pass.srcGridHeight < 0
        || pass.srcTiles.size() != pass.srcGridWidth * pass.srcGridHeight) {
        return fail(QStringLiteral("invalid color transform grid"));
    }
    if (pass.kind > ColorReference) {
        return fail(QStringLiteral("unknown color transform"));
    }
    const bool usesTables = pass.kind == ChannelTables || pass.kind == ColorReference;
    if (usesTables
        && (!m_f16 || !pass.tables || !pass.tablesId || pass.tableBytes <= 0 || pass.tableBytes % 4
            || pass.tableBytes > MaxTableBytes
            || (pass.kind == ChannelTables && pass.tableBytes != ChannelTablesBytes))) {
        return fail(QStringLiteral("color transform tables need RGBA F16 tiles and a valid table block"));
    }
    const qint64 maskBytes = pass.mask ? qint64(pass.maskBounds.width()) * pass.maskBounds.height() : 0;
    if (pass.mask && (pass.maskBounds.isEmpty() || maskBytes > MaxMaskBytes)) {
        return fail(QStringLiteral("invalid or oversized coverage mask"));
    }

    if (usesTables && m_channelTablesId != pass.tablesId) {
        const VkDeviceSize tableBytes = VkDeviceSize(pass.tableBytes);
        if (!m_channelTables || m_channelTables->size() < tableBytes) {
            m_channelTablesId = 0;
            m_channelTables = KisGpuBuffer::create(m_context, tableBytes, KisGpuBuffer::Location::Upload, errorMessage);
            if (!m_channelTables) {
                return false;
            }
        }
        std::memcpy(m_channelTables->mapped(), pass.tables, size_t(tableBytes));
        m_channelTablesId = pass.tablesId;
    }

    // One table: destination addresses, source addresses (at least one entry,
    // so the address is always valid), then the coverage bytes padded to words.
    const int srcEntries = qMax(1, pass.srcTiles.size());
    const size_t dstBytes = size_t(pass.dstTiles.size()) * sizeof(VkDeviceAddress);
    const VkDeviceSize maskOffset = alignUp(dstBytes + size_t(srcEntries) * sizeof(VkDeviceAddress), 16);
    const VkDeviceSize paddedMaskBytes = alignUp(VkDeviceSize(qMax<qint64>(maskBytes, 4)), 4);
    const VkDeviceSize bytes = maskOffset + paddedMaskBytes;
    if (!m_tables || m_tables->size() < bytes) {
        m_tables = KisGpuBuffer::create(m_context, bytes, KisGpuBuffer::Location::Upload, errorMessage);
        if (!m_tables) {
            return false;
        }
    }
    quint8 *table = static_cast<quint8 *>(m_tables->mapped());
    std::memcpy(table, pass.dstTiles.constData(), dstBytes);
    if (pass.srcTiles.isEmpty()) {
        std::memset(table + dstBytes, 0, sizeof(VkDeviceAddress));
    } else {
        std::memcpy(table + dstBytes,
                    pass.srcTiles.constData(),
                    size_t(pass.srcTiles.size()) * sizeof(VkDeviceAddress));
    }
    std::memset(table + maskOffset + paddedMaskBytes - 4, 0, 4);
    if (pass.mask) {
        std::memcpy(table + maskOffset, pass.mask, size_t(maskBytes));
    }

    PushConstants constants{};
    constants.dstTiles = m_tables->deviceAddress();
    constants.srcTiles = m_tables->deviceAddress() + dstBytes;
    constants.mask = m_tables->deviceAddress() + maskOffset;
    constants.tables = usesTables ? m_channelTables->deviceAddress() : m_tables->deviceAddress();
    constants.dstTileCount = quint32(pass.dstTiles.size());
    constants.dstGridWidth = quint32(pass.dstGridWidth);
    constants.srcGridWidth = quint32(pass.srcGridWidth);
    constants.srcGridHeight = quint32(pass.srcGridHeight);
    constants.clip[0] = pass.clip.left();
    constants.clip[1] = pass.clip.top();
    constants.clip[2] = pass.clip.right() + 1;
    constants.clip[3] = pass.clip.bottom() + 1;
    constants.shift[0] = pass.shift.x();
    constants.shift[1] = pass.shift.y();
    if (pass.mask) {
        constants.maskOrigin[0] = pass.maskBounds.x();
        constants.maskOrigin[1] = pass.maskBounds.y();
        constants.maskSize[0] = pass.maskBounds.width();
        constants.maskSize[1] = pass.maskBounds.height();
    }
    constants.kind = pass.kind;
    std::memcpy(constants.fill, pass.fill, sizeof(constants.fill));
    m_pipeline->dispatch(commands.commandBuffer(), constants, quint32(pass.dstTiles.size()));
    return true;
}
