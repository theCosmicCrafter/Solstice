/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUTILEBACKEND_H
#define KISGPUTILEBACKEND_H

#include <QMutex>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <KisGpuTilePool.h>

#include "kritaimage_export.h"

class KisGpuContext;
class KisGpuCommandList;
class KisGpuBuffer;
class KisTileData;
struct KisTileGpuState;

/**
 * Process-wide owner of the Vulkan context and the GPU tile pools used by
 * GPU-resident paint devices (GPU engine phase 1, docs/agent/gpu-engine.md).
 *
 * The backend is created on first use and intentionally never destroyed:
 * tile data can be released during static destruction, after which the
 * backend must still be able to accept its slots back.
 */
class KRITAIMAGE_EXPORT KisGpuTileBackend
{
public:
    /// Creates the backend on first call; nullptr if no usable Vulkan device exists.
    static KisGpuTileBackend *instance();
    /// The backend if it has already been created, without trying to create it.
    static KisGpuTileBackend *existingInstance();
    static QString unavailableReason();
    /**
     * The name of the graphics card when the engine did not start because
     * it was not verified with it and the user has not allowed other cards
     * (KisGpuEngineSettings::unverifiedDeviceAllowed()); empty otherwise.
     * Before instance() was first called this asks Vulkan which card it
     * would be (no device is created), so that the settings can say it
     * before the first document starts the engine.
     */
    static QString unverifiedDeviceName();
    /**
     * GPU engine (Solstice, phase 4.91): compiles the dab and layer compositor
     * pipelines (RGBA32F/F16, basic and extended blend modes) ahead of the
     * first stroke. Blocking and thread-safe; meant for a background thread.
     * Pipelines that are already compiled are reused. Returns false if the
     * backend is unavailable or a compilation failed (later use retries).
     */
    static bool preparePipelines();

    KisGpuContext &context();

    /// Pool for tile data of @p pixelSize bytes per pixel: 16 (RGBA F32) or 8 (RGBA F16).
    KisGpuTilePool *pool(qint32 pixelSize);

    /**
     * Pins the GPU state of @p td, creating or restoring its slot as needed.
     * A new state has a valid CPU copy and a stale GPU copy.
     * Each successful call must be paired with unpinStates(). The caller
     * keeps the tile data alive until unpinning.
     * nullptr if the pixel size is unsupported or GPU memory is exhausted.
     */
    KisTileGpuState *pinState(KisTileData *td, QString *errorMessage = nullptr);
    /// Releases the pins after publishing lastUse, or abandoning the recorded work.
    void unpinStates(const QVector<KisTileGpuState *> &states);
    /// Caller holds the swap lock for writing. Failed readback keeps GPU content resident.
    bool tryEvict(KisTileData *td);
    /// Caller holds store lifetime and swap write locks; at most 256 tiles.
    /// Returns bytes actually released; failed readbacks keep their GPU copies.
    quint64 tryEvictBatch(const QVector<KisTileData *> &tiles);
    /// Evict idle tiles, preferring historical/CPU-current tiles, up to this many bytes.
    quint64 evictTiles(quint64 bytes);
    quint64 memoryBudget() const;
    quint64 reservedTileBytes() const;
    /// Tests/development: callers must not change this while GPU accesses are prepared.
    void setMemoryBudgetForTesting(quint64 bytes);

    /// Synchronous download of one stale tile data (KisTileGpuHooks::ensureCpuValid).
    void ensureCpuValid(KisTileData *td);
    /// Downloads every tile data whose CPU copy is stale, batched into few submissions.
    void downloadToCpu(const QVector<KisTileData *> &tiles);

    /// Deferred release of the state's slot once the GPU passed state->lastUse.
    void destroyState(KisTileGpuState *state, qint32 pixelSize);

    /// Keeps @p resource alive until the timeline reaches @p timelineValue.
    void retireAfter(quint64 timelineValue, std::shared_ptr<void> resource);
    /// Frees deferred slots and retired resources whose GPU work has completed.
    void collectGarbage();
    /// Waits for all GPU work, then collects all garbage.
    void flush();

    /// Slots in use for @p pixelSize, including slots waiting for deferred release.
    quint32 allocatedSlots(qint32 pixelSize);

    /**
     * Tiles whose GPU copy could not be read back at all: their latest
     * content is lost and they show the content from before the GPU work
     * (KisTileGpuState::ContentLost).
     */
    quint64 contentLossCount() const;
    /// Tiles read back to the CPU by successful downloads since startup (measurements).
    quint64 downloadedTileCount() const;
    /**
     * True after GPU content was lost: the engine accepts no more GPU work
     * (KisGpuTileAccess::isSupported() is false) and the CPU path takes over.
     */
    bool hasFailed() const;
    /**
     * @p listener is called once, on the thread that detects it, when the
     * engine enters the failed state (GPU content was lost). The UI uses it
     * to tell the user (libs/ui/KisGpuEngineUi). Set it before GPU work
     * starts; an empty function removes it.
     */
    static void setFailureListener(std::function<void()> listener);

    /// Tests: the next @p count download submissions fail before submitting.
    void injectDownloadFailuresForTesting(int count);
    /// Tests: readback buffers larger than @p maxBytes cannot be allocated.
    void injectReadbackLimitForTesting(quint64 maxBytes);
    /// Tests: leave the failed state entered after injected failures.
    void resetFailureForTesting();

    /**
     * Serializes "check that uploads are still current and that the engine
     * has not failed, submit, publish flags" across all tile accesses
     * (KisGpuTileAccess::submitAndFinish), and entering the failed state.
     * Lock order: before any KisTileGpuState::mutex. Never lock tiles
     * (KisTileData::blockSwapping) while holding it: a download may need it.
     */
    QMutex &residencyMutex();

private:
    explicit KisGpuTileBackend(std::unique_ptr<KisGpuContext> context);
    void collectGarbageImpl(size_t maxRetired);
    /// Returns the tiles that are still stale because the download failed.
    QVector<KisTileData *> downloadBatch(const QVector<KisTileData *> &tiles);

    std::unique_ptr<KisGpuContext> m_context;
    std::unique_ptr<KisGpuTilePool> m_pool32;
    std::unique_ptr<KisGpuTilePool> m_pool16;

    struct DeferredSlot {
        KisGpuTilePool *pool;
        quint32 slot;
        quint64 timelineValue;
    };
    QMutex m_garbageMutex;
    std::vector<DeferredSlot> m_deferredSlots;
    std::vector<std::pair<quint64, std::shared_ptr<void>>> m_retired;

    /// Guards the download command list and readback buffer.
    /// Lock order: KisTileGpuState::mutex (sorted by address) before m_transferMutex.
    QMutex m_transferMutex;
    std::unique_ptr<KisGpuCommandList> m_transferCommands;
    std::unique_ptr<KisGpuBuffer> m_readback;

    QMutex m_residencyMutex;
    std::atomic<quint64> m_memoryBudget{0};
    std::atomic<int> m_injectedDownloadFailures{0};
    std::atomic<quint64> m_injectedReadbackLimit{~quint64(0)};
    std::atomic<quint64> m_lostTiles{0};
    std::atomic<quint64> m_downloadedTiles{0};
    std::atomic<bool> m_failed{false};
};

#endif // KISGPUTILEBACKEND_H
