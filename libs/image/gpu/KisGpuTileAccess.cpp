/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuTileAccess.h"
#include "KisPaintTrace.h"

#include "KisGpuTileBackend.h"

#include <KisGpuBuffer.h>
#include <KisGpuCommandList.h>
#include <KisGpuContext.h>
#include <KisGpuTilePool.h>

#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>

#include <QMutexLocker>
#include <QSet>

#include <cstring>

#include <kis_debug.h>

#include "kis_datamanager.h"
#include "kis_paint_device.h"
#include "tiles3/kis_tile.h"
#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_tile_data_store.h"

namespace
{
constexpr int TileSize = 64;

int floorDiv(int value, int divisor)
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

struct Entry {
    KisTileSP tile;
    /// Tile data the GPU accesses (the private copy after copy-on-write).
    KisTileData *tileData = nullptr;
    KisTileGpuState *state = nullptr;
    /// Tile data replaced by copy-on-write, referenced until complete().
    KisTileData *replaced = nullptr;
    /// tileData holds a reference of this access (read-only accesses): a CPU
    /// writer may replace the tile's data and free it meanwhile.
    bool referenced = false;
    /// tileData is a fresh tile data whose content only the GPU work produces.
    bool contentPending = false;
    int generation = 0;
    /// The CPU content was snapshotted at prepare() for an upload.
    bool uploaded = false;
    VkDeviceSize stagingOffset = 0;
    /// At submission the GPU copy was already current: the upload was skipped.
    bool uploadSkipped = false;
};
} // namespace

struct KisGpuTileAccess::Private {
    KisPaintDeviceSP device;
    KisDataManagerSP dataManager;
    Mode mode = ReadOnly;
    QRect grid;
    QPoint deviceOffset;
    qint32 pixelSize = 0;

    QVector<Entry> entries;
    /// Copy-on-write sources that this access uploads so that their clones
    /// can be made by GPU slot copies (e.g. the shared default tile data of
    /// tiles that were just cleared). Kept alive by the entries' `replaced`.
    QVector<Entry> sourceEntries;
    QVector<KisTileGpuState *> pins;
    std::shared_ptr<KisGpuBuffer> staging;
    bool prepared = false;
    bool finished = false;

    void releaseReplaced();
    void restorePendingContent();
    KisTileGpuState *pin(KisTileData *td, QString *errorMessage = nullptr)
    {
        KisTileGpuState *state = KisGpuTileBackend::instance()->pinState(td, errorMessage);
        if (state)
            pins << state;
        return state;
    }
    void releasePins()
    {
        if (pins.isEmpty()) {
            return;
        }
        KisGpuTileBackend::existingInstance()->unpinStates(pins);
        pins.clear();
    }
};

bool KisGpuTileAccess::isSupported(KisPaintDeviceSP device, QString *reason)
{
    auto fail = [reason](const QString &message) {
        if (reason) {
            *reason = message;
        }
        return false;
    };

    const KoColorSpace *cs = device ? device->colorSpace() : nullptr;
    if (!cs) {
        return fail(QStringLiteral("no paint device"));
    }
    if (cs->colorModelId() != RGBAColorModelID
        || (cs->colorDepthId() != Float32BitsColorDepthID && cs->colorDepthId() != Float16BitsColorDepthID)) {
        return fail(QStringLiteral("color space %1 is not RGBA F32/F16").arg(cs->id()));
    }
    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    if (!backend) {
        return fail(QStringLiteral("GPU engine unavailable: %1").arg(KisGpuTileBackend::unavailableReason()));
    }
    if (backend->hasFailed()) {
        return fail(QStringLiteral("GPU engine stopped after GPU data could not be read back"));
    }
    return true;
}

KisGpuTileAccess::KisGpuTileAccess(KisPaintDeviceSP device, const QRect &rect, Mode mode)
    : d(new Private)
{
    d->device = device;
    d->dataManager = device->dataManager();
    d->mode = mode;
    d->deviceOffset = QPoint(device->x(), device->y());
    d->pixelSize = device->pixelSize();
    // Like a writable CPU iterator: cached bounds must not survive the write.
    if (mode != ReadOnly)
        device->invalidateCachedBounds();

    if (!rect.isEmpty()) {
        const QRect dmRect = rect.translated(-d->deviceOffset);
        const int left = floorDiv(dmRect.left(), TileSize);
        const int top = floorDiv(dmRect.top(), TileSize);
        const int right = floorDiv(dmRect.right(), TileSize);
        const int bottom = floorDiv(dmRect.bottom(), TileSize);
        d->grid = QRect(QPoint(left, top), QPoint(right, bottom));
    }
}

KisGpuTileAccess::~KisGpuTileAccess()
{
    if (d->prepared && !d->finished) {
        warnKrita << "GPU engine: tile access destroyed without submitAndFinish()";
        KisGpuTileBackend *backend = KisGpuTileBackend::existingInstance();
        if (backend) {
            backend->context().waitIdle();
        }
        d->restorePendingContent();
        d->releasePins();
        d->releaseReplaced();
    }
}

KisGpuTileAccess::Mode KisGpuTileAccess::mode() const
{
    return d->mode;
}

QRect KisGpuTileAccess::tileGrid() const
{
    return d->grid;
}

int KisGpuTileAccess::tileCount() const
{
    return d->grid.width() * d->grid.height();
}

QPoint KisGpuTileAccess::tileOrigin(int col, int row) const
{
    return QPoint(col * TileSize, row * TileSize) + d->deviceOffset;
}

bool KisGpuTileAccess::prepare(KisGpuCommandList &commands, QString *errorMessage)
{
    return prepareImpl(commands, errorMessage, nullptr);
}

bool KisGpuTileAccess::prepare(KisGpuCommandList &commands, UploadArena &uploads, QString *errorMessage)
{
    return prepareImpl(commands, errorMessage, &uploads);
}

bool KisGpuTileAccess::prepareImpl(KisGpuCommandList &commands, QString *errorMessage, UploadArena *arena)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(!d->prepared, false);
    d->prepared = true;

    auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    KisGpuTileBackend *backend = KisGpuTileBackend::instance();
    if (!backend) {
        return fail(QStringLiteral("GPU engine unavailable"));
    }
    if (backend->hasFailed()) {
        return fail(QStringLiteral("GPU engine stopped after GPU data could not be read back"));
    }
    KisGpuTilePool *pool = backend->pool(d->pixelSize);
    if (!pool) {
        return fail(QStringLiteral("Unsupported pixel size %1").arg(d->pixelSize));
    }

    const bool writes = d->mode != ReadOnly;
    QVector<int> uploads;
    QVector<quint32> copySources;
    QVector<quint32> copyTargets;
    QSet<KisTileData *> uploadedTileData;
    QSet<KisTileData *> scheduledSources;

    {
        KisPaintTrace::Scope trace("tile_access.resolve_tiles", d.get(), nullptr, KisPaintTrace::currentFlow());
        d->entries.reserve(tileCount());
        for (int row = d->grid.top(); row <= d->grid.bottom(); row++) {
            for (int col = d->grid.left(); col <= d->grid.right(); col++) {
                Entry entry;
                entry.tile = d->dataManager->getTile(col, row, writes);

                bool contentFromGpuCopy = false;
                if (writes) {
                    bool isLocked = false;
                    entry.replaced = entry.tile->detachForExternalWrite(
                        [&](KisTileData *source) -> KisTileData * {
                            KisTileGpuState *sourceState = source->gpuState();
                            if (d->mode == ReadWrite) {
                                // The clone's content comes from a GPU slot copy of
                                // the source; a source that is not GPU-current is
                                // uploaded once by this access (many cleared tiles
                                // share one default tile data).
                                sourceState = d->pin(source);
                                if (!sourceState) {
                                    return source->clone(); // no GPU memory: CPU clone, uploaded below
                                }
                                if (!sourceState->gpuValid() && !scheduledSources.contains(source)) {
                                    scheduledSources.insert(source);
                                    Entry sourceEntry;
                                    sourceEntry.tileData = source;
                                    sourceEntry.state = sourceState;
                                    sourceEntry.generation = sourceState->generation();
                                    sourceEntry.uploaded = true;
                                    d->sourceEntries << sourceEntry;
                                }
                                contentFromGpuCopy = true;
                            } else if (sourceState) {
                                // Protect the old CPU snapshot against eviction and
                                // disk swapping until the clone has copied it.
                                sourceState = d->pin(source);
                            }

                            // The content is produced on the GPU (slot copy or
                            // overwrite). The CPU buffer still gets the source's
                            // CPU content: never a meaningless buffer, even if the
                            // GPU copy is lost later.
                            KisTileData *clone;
                            if (sourceState && sourceState->slot != KisTileGpuState::InvalidSlot) {
                                // The pin prevents eviction and disk swapping.
                                clone = KisTileDataStore::instance()->duplicateCpuSnapshot(source);
                            } else {
                                source->blockSwapping();
                                clone = KisTileDataStore::instance()->duplicateCpuSnapshot(source);
                                source->unblockSwapping();
                            }
                            return clone;
                        },
                        &isLocked);
                    if (isLocked) {
                        d->restorePendingContent();
                        return fail(QStringLiteral("Tile (%1, %2) is locked by a CPU user").arg(col).arg(row));
                    }
                    if (!entry.replaced) {
                        // Writing in place: pre-made COW clones would become stale.
                        entry.tile->tileData()->dropClones();
                    }
                    entry.contentPending = entry.replaced && entry.tile->tileData()->gpuState() == nullptr
                        && (d->mode == WriteOnly || contentFromGpuCopy);
                }

                if (writes) {
                    // Merge jobs never share a projection tile, and the tile
                    // was detached above: its data is this access's alone.
                    entry.tileData = entry.tile->tileData();
                } else {
                    // A layer's tiles can be written on the CPU meanwhile: by
                    // a stroke, or by another merge job when the device is
                    // off the image's tile grid (a moved layer with masks).
                    // Copy-on-write then frees the data this access reads.
                    entry.tileData = entry.tile->referencedTileData();
                    entry.referenced = true;
                }
                entry.state = d->pin(entry.tileData, errorMessage);
                if (!entry.state) {
                    d->entries << entry;
                    d->restorePendingContent();
                    return false;
                }
                entry.generation = entry.state->generation();

                if (contentFromGpuCopy) {
                    copySources << entry.replaced->gpuState()->slot;
                    copyTargets << entry.state->slot;
                } else if (d->mode != WriteOnly && !entry.state->gpuValid()
                           && !uploadedTileData.contains(entry.tileData)) {
                    // Absent tiles all share the default tile data: upload it once.
                    uploadedTileData.insert(entry.tileData);
                    uploads << d->entries.size();
                    entry.uploaded = true;
                }
                d->entries << entry;
            }
        }
    }

    QVector<Entry *> uploadEntries;
    for (int index : uploads) {
        uploadEntries << &d->entries[index];
    }
    for (Entry &sourceEntry : d->sourceEntries) {
        uploadEntries << &sourceEntry;
    }

    if (!uploadEntries.isEmpty()) {
        KisPaintTrace::Scope trace("tile_access.stage_uploads", d.get(), nullptr, KisPaintTrace::currentFlow());
        const VkDeviceSize tileBytes = pool->tileBytes();
        const VkDeviceSize bytes = tileBytes * uploadEntries.size();
        VkDeviceSize baseOffset = 0;
        if (arena && arena->m_current && arena->m_current->size() - arena->m_used >= bytes) {
            d->staging = arena->m_current;
            baseOffset = arena->m_used;
        } else {
            // Grow only after observing multiple uploads in this submission.
            // A single dirty layer must not reserve space for resident layers.
            const VkDeviceSize capacity = arena && arena->m_current && bytes >= 256 * 1024
                ? qMax(bytes, qMin(arena->m_chunkBytes, 2 * arena->m_current->size()))
                : bytes;
            d->staging = KisGpuBuffer::create(backend->context(),
                                              capacity,
                                              KisGpuBuffer::Location::Staging,
                                              capacity == bytes ? errorMessage : nullptr);
            // Pooling is optional: retry the original exact allocation if the
            // larger chunk could not be obtained.
            if (!d->staging && capacity != bytes) {
                d->staging =
                    KisGpuBuffer::create(backend->context(), bytes, KisGpuBuffer::Location::Staging, errorMessage);
            }
            if (d->staging && arena) {
                arena->m_current = d->staging;
                arena->m_used = 0;
                ++arena->m_allocationCount;
                arena->m_reservedBytes += d->staging->size();
            }
        }
        if (!d->staging) {
            d->restorePendingContent();
            return false;
        }
        if (arena)
            arena->m_used += bytes;

        // Snapshot the CPU content now; whether it is uploaded is decided at
        // submission (submitAndFinish), when it is known whether the GPU copy
        // has become current in the meantime.
        quint8 *out = static_cast<quint8 *>(d->staging->mapped());
        VkDeviceSize offset = baseOffset;
        for (Entry *entry : uploadEntries) {
            KisTileData *td = entry->tileData;
            td->blockSwapping();
            std::memcpy(out + offset, td->data(), size_t(tileBytes));
            td->unblockSwapping();
            entry->stagingOffset = offset;
            offset += tileBytes;
        }
    }

    if (!copySources.isEmpty()) {
        pool->recordSlotCopies(commands, copySources, copyTargets);
        commands.computeBarrier();
    }
    return true;
}

void KisGpuTileAccess::recordUploads(KisGpuCommandList &commands)
{
    KisGpuTileBackend *backend = KisGpuTileBackend::existingInstance();
    KisGpuTilePool *pool = backend->pool(d->pixelSize);

    QVector<VkDeviceSize> offsets;
    QVector<quint32> slots;
    QVector<Entry *> all;
    for (Entry &entry : d->entries) {
        all << &entry;
    }
    for (Entry &entry : d->sourceEntries) {
        all << &entry;
    }
    for (Entry *entryPointer : all) {
        Entry &entry = *entryPointer;
        if (!entry.uploaded) {
            continue;
        }
        // GpuValid implies the slot holds the content of the current
        // generation (KisTileGpuState): it is at least as new as our snapshot.
        if (entry.state->gpuValid()) {
            entry.uploadSkipped = true;
            continue;
        }
        offsets << entry.stagingOffset;
        slots << entry.state->slot;
    }
    if (!slots.isEmpty()) {
        pool->recordUploadAt(commands.preamble(), *d->staging, offsets, slots);
    }
}

QVector<VkDeviceAddress> KisGpuTileAccess::addresses() const
{
    QVector<VkDeviceAddress> result;
    KisGpuTileBackend *backend = KisGpuTileBackend::existingInstance();
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(backend && d->prepared, result);

    KisGpuTilePool *pool = backend->pool(d->pixelSize);
    result.reserve(d->entries.size());
    for (const Entry &entry : d->entries) {
        result << (entry.state ? pool->deviceAddress(entry.state->slot) : 0);
    }
    return result;
}

quint64 KisGpuTileAccess::submitAndFinish(KisGpuCommandList &commands,
                                          const QVector<KisGpuTileAccess *> &accesses,
                                          const QVector<VkSemaphoreSubmitInfo> &waitSemaphores,
                                          const QVector<VkSemaphoreSubmitInfo> &signalSemaphores)
{
    KisGpuTileBackend *backend = KisGpuTileBackend::existingInstance();
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(backend, 0);

    // Main commands no longer change. Only generation-dependent uploads in
    // the preamble must be recorded while holding the residency mutex.
    const bool mainReady = [&] {
        KisPaintTrace::Scope trace("tile_submit.finish_main", backend, &commands, KisPaintTrace::currentFlow());
        return commands.finishMainRecording();
    }();
    KisGpuSubmitTiming submitTiming;
    const bool traceSubmission = KisPaintTrace::enabled();
    quint64 value = 0;
    {
        // Deciding which uploads still run, checking for the failed state,
        // submitting, and publishing the flags must not interleave with
        // another access doing the same (an older upload could land after a
        // newer one was published) nor with the engine entering the failed
        // state (KisGpuTileBackend::downloadToCpu takes this mutex for that).
        auto locker = [&] {
            KisPaintTrace::Scope trace("tile_submit.lock", backend, &commands, KisPaintTrace::currentFlow());
            return QMutexLocker(&backend->residencyMutex());
        }();
        KisPaintTrace::Scope holdTrace("residency.hold.submit", backend, &commands, KisPaintTrace::currentFlow());

        if (backend->hasFailed()) {
            dbgImage << "GPU engine: stopped; not submitting";
        } else if (mainReady) {
            // Uploads run first, in the preamble of the same submission.
            {
                KisPaintTrace::Scope trace("tile_submit.record_uploads",
                                           &commands,
                                           nullptr,
                                           KisPaintTrace::currentFlow());
                for (KisGpuTileAccess *access : accesses) {
                    access->recordUploads(commands);
                }
            }
            {
                KisPaintTrace::Scope trace("tile_submit.queue", &commands, nullptr, KisPaintTrace::currentFlow());
                value = commands.submit(waitSemaphores, signalSemaphores, traceSubmission ? &submitTiming : nullptr);
            }
        }
        // Not submitted (refused above): end the recording so that the list
        // can be begun again. A failed submit() already ended it.
        commands.abandon();

        {
            KisPaintTrace::Scope trace("tile_submit.publish", &commands, nullptr, KisPaintTrace::currentFlow());
            for (KisGpuTileAccess *access : accesses) {
                access->publish(value);
            }
        }
    }

    // Outside the residency mutex: restoring may download from the GPU,
    // which can enter the failed state (and take the mutex).
    {
        KisPaintTrace::Scope trace("tile_submit.complete", &commands, nullptr, KisPaintTrace::currentFlow());
        for (KisGpuTileAccess *access : accesses) {
            if (!value) {
                access->d->restorePendingContent();
            }
            access->complete(value);
        }
    }
    // Record saved timestamps after residency and completion locks are released.
    if (traceSubmission) {
        const quint64 flow = KisPaintTrace::currentFlow();
        KisPaintTrace::externalSpan("tile_submit.queue_lock",
                                    backend,
                                    &commands,
                                    submitTiming.lockStartNs,
                                    submitTiming.lockAcquiredNs,
                                    flow);
        if (submitTiming.driverStartNs) {
            KisPaintTrace::externalSpan("tile_submit.queue_prepare",
                                        backend,
                                        &commands,
                                        submitTiming.lockAcquiredNs,
                                        submitTiming.driverStartNs,
                                        flow);
            KisPaintTrace::externalSpan("tile_submit.driver",
                                        backend,
                                        &commands,
                                        submitTiming.driverStartNs,
                                        submitTiming.driverEndNs,
                                        flow);
        }
    }
    return value;
}

void KisGpuTileAccess::finishUnsubmitted(KisGpuCommandList &commands, const QVector<KisGpuTileAccess *> &accesses)
{
    commands.abandon();
    for (KisGpuTileAccess *access : accesses) {
        if (access->d->prepared && !access->d->finished) {
            access->publish(0);
            access->d->restorePendingContent();
            access->complete(0);
        }
    }
}

void KisGpuTileAccess::publish(quint64 timelineValue)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(d->prepared && !d->finished);
    // Bounds computed while the GPU wrote may describe the old content.
    if (d->mode != ReadOnly && timelineValue && d->device)
        d->device->invalidateCachedBounds();
    // Include already-current COW sources, which have no upload entry.
    // Every recorded address remains protected after its CPU pin is released.
    for (KisTileGpuState *state : d->pins) {
        QMutexLocker locker(&state->mutex);
        state->lastUse = qMax(state->lastUse, timelineValue);
    }

    // When nothing was submitted (timelineValue == 0), nothing on the GPU
    // changed; submitAndFinish() restores pending content afterwards.
    for (const Entry &entry : d->entries) {
        if (!entry.state) {
            continue;
        }
        QMutexLocker locker(&entry.state->mutex);
        entry.state->lastUse = qMax(entry.state->lastUse, timelineValue);

        if (!timelineValue) {
            continue;
        }
        if (d->mode != ReadOnly) {
            entry.state->setValid(KisTileGpuState::GpuValid);
        } else if (entry.uploaded && !entry.uploadSkipped) {
            // Atomic with KisTileData::notifyCpuWrite(): a CPU write that
            // overlapped the snapshot (lock or unlock after it was taken)
            // keeps the GPU copy stale.
            entry.state->markGpuValidIfGeneration(entry.generation);
        }
    }

    // Copy-on-write sources uploaded for GPU slot copies.
    for (const Entry &entry : d->sourceEntries) {
        QMutexLocker locker(&entry.state->mutex);
        entry.state->lastUse = qMax(entry.state->lastUse, timelineValue);
        if (timelineValue && !entry.uploadSkipped) {
            entry.state->markGpuValidIfGeneration(entry.generation);
        }
    }
}

void KisGpuTileAccess::complete(quint64 timelineValue)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(d->prepared && !d->finished);
    d->finished = true;

    KisGpuTileBackend *backend = KisGpuTileBackend::existingInstance();
    KIS_SAFE_ASSERT_RECOVER_RETURN(backend);

    d->releasePins();
    d->releaseReplaced();

    if (d->staging) {
        backend->retireAfter(timelineValue, std::move(d->staging));
    }
    d->entries.clear();
    d->sourceEntries.clear();
}

void KisGpuTileAccess::Private::restorePendingContent()
{
    // The GPU work that would produce the content of fresh copy-on-write
    // clones never ran: give them the replaced content on the CPU instead,
    // so the device is unchanged (and its memento stays consistent).
    for (Entry &entry : entries) {
        if (!entry.contentPending) {
            continue;
        }
        KisTileData *fresh = entry.tile->tileData();
        entry.replaced->blockSwapping();
        fresh->blockSwapping();
        std::memcpy(fresh->data(), entry.replaced->data(), size_t(pixelSize) * TileSize * TileSize);
        fresh->unblockSwapping();
        entry.replaced->unblockSwapping();
        if (KisTileGpuState *state = fresh->gpuState()) {
            state->setValid(KisTileGpuState::CpuValid);
        }
        entry.contentPending = false;
    }
}

void KisGpuTileAccess::Private::releaseReplaced()
{
    for (Entry &entry : entries) {
        if (entry.referenced) {
            entry.referenced = false;
            entry.tileData->deref();
            entry.tileData = nullptr;
            entry.state = nullptr;
        }
        if (!entry.replaced) {
            continue;
        }
        entry.replaced->deref();
        entry.replaced = nullptr;
    }
}

void KisGpuTileAccess::syncToCpu(KisPaintDeviceSP device, const QRect &rect)
{
    KisDataManagerSP dataManager = device->dataManager();
    const QRect dmRect = rect.translated(-device->x(), -device->y());
    KisTileGpuHooks::prepareCpuRead(dataManager.data(), dmRect);
}
