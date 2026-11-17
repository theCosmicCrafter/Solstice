/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUCONTEXT_H
#define KISGPUCONTEXT_H

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QVector>

#include <memory>

#include "KisGpuVulkanFunctions.h"
#include "kritagpu_export.h"

class KisGpuComputePipeline;

struct KRITAGPU_EXPORT KisGpuDeviceInfo {
    QString name;
    quint32 vendorId = 0;
    quint32 deviceId = 0;
    quint32 apiVersion = 0;
    QString driverName;
    QString driverInfo;
    QByteArray deviceUuid;
    QByteArray driverUuid;
    float timestampPeriodNs = 0.0f;
    quint32 maxComputeWorkGroupInvocations = 0;
    VkDeviceSize deviceLocalBytes = 0;
    /// Size of the largest DEVICE_LOCAL | HOST_VISIBLE heap (Resizable BAR).
    VkDeviceSize hostVisibleDeviceLocalBytes = 0;
    bool supportsExternalMemoryInterop = false;
    /// shaderFloat64 is enabled (GPU transforms reproduce the CPU's double math).
    bool supportsFloat64 = false;

    bool isNvidia() const;
    /**
     * Heuristic: NVIDIA PCI device ids starting at 0x2B00 belong to the
     * Blackwell (GB20x) generation. Blackwell is the only tested target.
     */
    bool isBlackwell() const;
    /**
     * The engine was verified with this kind of device: NVIDIA Blackwell,
     * the only hardware it has been developed and tested on (user
     * decision, 2026-11-12). Other devices that meet the requirements can
     * run it, but only when the user asks for that
     * (Options::requireVerifiedDevice).
     */
    bool isVerified() const;
    QString summary() const;
};

/// Optional host timestamps in std::chrono::steady_clock nanoseconds.
/// Zero driver timestamps mean submission was refused before the driver call.
struct KisGpuSubmitTiming {
    qint64 lockStartNs = 0;
    qint64 lockAcquiredNs = 0;
    qint64 driverStartNs = 0;
    qint64 driverEndNs = 0;
};

/**
 * Process-level Vulkan instance, device, and compute queue for the GPU engine.
 *
 * The context is created explicitly (create()) and owned by its user. It is
 * thread-safe for submission: recording command buffers is the caller's job,
 * while queue access and the timeline semaphore are serialized here.
 *
 * Every submission signals the context timeline semaphore with a strictly
 * increasing value; wait(value) blocks until that work has completed.
 */
class KRITAGPU_EXPORT KisGpuContext
{
public:
    struct Options {
        /// Enables VK_LAYER_KHRONOS_validation when it is installed.
        /// Also enabled by the KRITA_GPU_VALIDATION=1 environment variable.
        bool enableValidation = false;
        /// Request the Win32 external memory/semaphore extensions needed for
        /// OpenGL interop. Missing extensions are reported, not fatal.
        bool requestInterop = true;
        /// Refuse a device the engine was not verified with
        /// (KisGpuDeviceInfo::isVerified()). The application sets this
        /// unless the user allowed other devices; tests leave it off.
        bool requireVerifiedDevice = false;
    };

    ~KisGpuContext();

    KisGpuContext(const KisGpuContext &) = delete;
    KisGpuContext &operator=(const KisGpuContext &) = delete;

    /**
     * @param unverifiedDevice receives the selected device when it was
     *        refused because of Options::requireVerifiedDevice (its name is
     *        empty otherwise)
     */
    static std::unique_ptr<KisGpuContext>
    create(const Options &options, QString *errorMessage, KisGpuDeviceInfo *unverifiedDevice = nullptr);
    static std::unique_ptr<KisGpuContext> create(QString *errorMessage);

    /**
     * The device create() would select, without creating a Vulkan device:
     * for telling the user which graphics card the engine would run on
     * before it starts. The name is empty when there is no usable device.
     */
    static KisGpuDeviceInfo probeDevice(QString *errorMessage = nullptr);

    /**
     * Whether the selected device @p info may run the engine when
     * @p requireVerified is set. KRITA_GPU_TREAT_DEVICE_AS_UNVERIFIED=1
     * makes every device count as unverified (development: to see the
     * refusal on verified hardware).
     */
    static bool acceptsDevice(const KisGpuDeviceInfo &info, bool requireVerified);

    /**
     * Whether a physical device of @p type may run the engine. CPU devices
     * (software Vulkan such as lavapipe or SwiftShader, e.g. in virtual
     * machines) are refused: they are slower than the CPU paths, which then
     * run instead. @p allowSoftware accepts them (tests, diagnostics); the
     * engine passes KRITA_GPU_ALLOW_SOFTWARE_DEVICE=1.
     */
    static bool acceptsDeviceType(VkPhysicalDeviceType type, bool allowSoftware);

    const KisGpuVulkanFunctions &vk() const;
    const KisGpuDeviceInfo &deviceInfo() const;

    VkInstance instance() const;
    VkPhysicalDevice physicalDevice() const;
    VkDevice device() const;
    quint32 queueFamilyIndex() const;
    bool validationEnabled() const;
    /// Number of validation-layer errors reported so far (0 without validation).
    int validationErrorCount() const;

    /**
     * Returns a memory type index that has all @p required flags, preferring
     * one that also has @p preferred flags; -1 if none is compatible.
     */
    int findMemoryType(quint32 typeBits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) const;
    VkMemoryPropertyFlags memoryTypeFlags(int memoryTypeIndex) const;

    /**
     * Submits @p commandBuffer to the compute queue.
     *
     * @param waitSemaphores optional extra semaphores (e.g. interop) to wait for
     * @param signalSemaphores optional extra semaphores to signal
     * @return timeline value signaled when the work completes, or 0 on failure
     */
    quint64 submit(VkCommandBuffer commandBuffer,
                   const QVector<VkSemaphoreSubmitInfo> &waitSemaphores = {},
                   const QVector<VkSemaphoreSubmitInfo> &signalSemaphores = {});
    /// Submits several command buffers in one batch, executed in order.
    quint64 submit(const QVector<VkCommandBuffer> &commandBuffers,
                   const QVector<VkSemaphoreSubmitInfo> &waitSemaphores = {},
                   const QVector<VkSemaphoreSubmitInfo> &signalSemaphores = {},
                   KisGpuSubmitTiming *timing = nullptr);

    /// Blocks until the timeline reaches @p value. Returns false on timeout/device loss.
    bool wait(quint64 value, quint64 timeoutNs = UINT64_MAX) const;
    quint64 completedValue() const;
    void waitIdle();

    /// Tests: the next @p count submissions fail without reaching the queue.
    void injectSubmitFailuresForTesting(int count);

    /**
     * GPU engine (Solstice, phase 4.91): compiles a compute pipeline once per
     * context and embedded SPIR-V array; later calls share it. Thread-safe:
     * concurrent callers of the same pipeline wait for one compilation, other
     * pipelines compile in parallel. Failures are not cached. The context
     * releases its references before destroying the device.
     */
    std::shared_ptr<KisGpuComputePipeline> sharedComputePipeline(const quint32 *spirv,
                                                                 size_t spirvSizeBytes,
                                                                 quint32 pushConstantSize,
                                                                 QString *errorMessage = nullptr);
    /// Number of shared pipelines compiled so far (tests and diagnostics).
    int sharedComputePipelineCompileCount() const;

private:
    KisGpuContext();

    struct Private;
    std::unique_ptr<Private> d;
};

#endif // KISGPUCONTEXT_H
