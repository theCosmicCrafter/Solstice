/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuShiftedCopyPass.h"

#include "KisGpuBuffer.h"
#include "KisGpuCommandList.h"
#include "KisGpuComputePipeline.h"
#include "KisGpuContext.h"

#include <cstring>

namespace
{
const quint32 CopyShiftedRgba32f[] = {
#include "copy_shifted_rgba32f.spv.inc"
};

const quint32 CopyShiftedRgba16f[] = {
#include "copy_shifted_rgba16f.spv.inc"
};

struct PushConstants {
    VkDeviceAddress dstTiles;
    VkDeviceAddress srcTiles;
    quint32 dstTileCount;
    quint32 dstGridWidth;
    quint32 srcGridWidth;
    quint32 srcGridHeight;
    qint32 shift[2];
    qint32 padding[2];
    qint32 clip[4];
    float fill[4];
};
static_assert(sizeof(PushConstants) == 80, "must match the shader push constant block");
} // namespace

KisGpuShiftedCopyPass::KisGpuShiftedCopyPass(KisGpuContext &context)
    : m_context(context)
{
}

KisGpuShiftedCopyPass::~KisGpuShiftedCopyPass() = default;

std::unique_ptr<KisGpuShiftedCopyPass>
KisGpuShiftedCopyPass::create(KisGpuContext &context, KisGpuTileFormat format, QString *errorMessage)
{
    std::unique_ptr<KisGpuShiftedCopyPass> pass(new KisGpuShiftedCopyPass(context));
    const bool f16 = format == KisGpuTileFormat::RGBA16F;
    pass->m_pipeline = context.sharedComputePipeline(f16 ? CopyShiftedRgba16f : CopyShiftedRgba32f,
                                                     f16 ? sizeof(CopyShiftedRgba16f) : sizeof(CopyShiftedRgba32f),
                                                     sizeof(PushConstants),
                                                     errorMessage);
    return pass->m_pipeline ? std::move(pass) : nullptr;
}

bool KisGpuShiftedCopyPass::record(KisGpuCommandList &commands, const Copy &copy, QString *errorMessage)
{
    if (copy.dstTiles.isEmpty() || copy.clip.isEmpty()) {
        return true;
    }
    if (copy.dstGridWidth <= 0 || copy.srcGridWidth < 0 || copy.srcGridHeight < 0
        || copy.srcTiles.size() != copy.srcGridWidth * copy.srcGridHeight) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("invalid shifted copy grid");
        }
        return false;
    }

    // One table: destination addresses, then source addresses (at least one
    // entry, so the source table address is always valid).
    const int srcEntries = qMax(1, copy.srcTiles.size());
    const VkDeviceSize bytes = VkDeviceSize(copy.dstTiles.size() + srcEntries) * sizeof(VkDeviceAddress);
    if (!m_tables || m_tables->size() < bytes) {
        m_tables = KisGpuBuffer::create(m_context, bytes, KisGpuBuffer::Location::Upload, errorMessage);
        if (!m_tables) {
            return false;
        }
    }
    quint8 *table = static_cast<quint8 *>(m_tables->mapped());
    const size_t dstBytes = size_t(copy.dstTiles.size()) * sizeof(VkDeviceAddress);
    std::memcpy(table, copy.dstTiles.constData(), dstBytes);
    if (copy.srcTiles.isEmpty()) {
        std::memset(table + dstBytes, 0, sizeof(VkDeviceAddress));
    } else {
        std::memcpy(table + dstBytes,
                    copy.srcTiles.constData(),
                    size_t(copy.srcTiles.size()) * sizeof(VkDeviceAddress));
    }

    PushConstants constants{};
    constants.dstTiles = m_tables->deviceAddress();
    constants.srcTiles = m_tables->deviceAddress() + dstBytes;
    constants.dstTileCount = quint32(copy.dstTiles.size());
    constants.dstGridWidth = quint32(copy.dstGridWidth);
    constants.srcGridWidth = quint32(copy.srcGridWidth);
    constants.srcGridHeight = quint32(copy.srcGridHeight);
    constants.shift[0] = copy.shift.x();
    constants.shift[1] = copy.shift.y();
    constants.clip[0] = copy.clip.left();
    constants.clip[1] = copy.clip.top();
    constants.clip[2] = copy.clip.right() + 1;
    constants.clip[3] = copy.clip.bottom() + 1;
    std::memcpy(constants.fill, copy.fill, sizeof(constants.fill));
    m_pipeline->dispatch(commands.commandBuffer(), constants, quint32(copy.dstTiles.size()));
    return true;
}
