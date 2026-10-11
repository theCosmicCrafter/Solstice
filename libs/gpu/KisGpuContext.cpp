/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisGpuContext.h"
#include "KisGpuComputePipeline.h"

#include <QAtomicInt>
#include <QVector>

#include <chrono>
#include <cstring>
#include <map>
#include <utility>

#include <kis_debug.h>

namespace
{
constexpr quint32 NvidiaVendorId = 0x10DE;
constexpr quint32 RequiredApiVersion = VK_API_VERSION_1_3;
const char *const ValidationLayerName = "VK_LAYER_KHRONOS_validation";

QVector<const char *> interopExtensions()
{
#ifdef Q_OS_WIN
    return {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"};
#else
    return {};
#endif
}

bool hasExtension(const QVector<VkExtensionProperties> &available, const char *name)
{
    for (const VkExtensionProperties &extension : available) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

QString versionString(quint32 version)
{
    return QStringLiteral("%1.%2.%3")
        .arg(VK_API_VERSION_MAJOR(version))
        .arg(VK_API_VERSION_MINOR(version))
        .arg(VK_API_VERSION_PATCH(version));
}
} // namespace

bool KisGpuContext::acceptsDeviceType(VkPhysicalDeviceType type, bool allowSoftware)
{
    return allowSoftware || type != VK_PHYSICAL_DEVICE_TYPE_CPU;
}

bool KisGpuDeviceInfo::isNvidia() const
{
    return vendorId == NvidiaVendorId;
}

bool KisGpuDeviceInfo::isBlackwell() const
{
    return isNvidia() && deviceId >= 0x2B00;
}

QString KisGpuDeviceInfo::summary() const
{
    return QStringLiteral("%1 (vendor 0x%2, device 0x%3, Vulkan %4, %5 %6, VRAM %7 MiB, ReBAR %8 MiB, interop %9%10)")
        .arg(name)
        .arg(vendorId, 4, 16, QLatin1Char('0'))
        .arg(deviceId, 4, 16, QLatin1Char('0'))
        .arg(versionString(apiVersion))
        .arg(driverName, driverInfo)
        .arg(deviceLocalBytes >> 20)
        .arg(hostVisibleDeviceLocalBytes >> 20)
        .arg(supportsExternalMemoryInterop ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(isBlackwell() ? QStringLiteral(", Blackwell") : QString());
}

struct KisGpuContext::Private {
    KisGpuVulkanFunctions vk;
    KisGpuDeviceInfo info;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    quint32 queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkSemaphore timeline = VK_NULL_HANDLE;
    bool validation = false;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    QAtomicInt validationErrors;

    QMutex queueMutex;
    quint64 lastSubmitted = 0;
    int injectedSubmitFailures = 0; // guarded by queueMutex

    struct SharedPipeline {
        QMutex mutex; // held while compiling
        std::shared_ptr<KisGpuComputePipeline> pipeline;
    };
    QMutex pipelinesMutex;
    std::map<std::pair<const quint32 *, quint32>, std::shared_ptr<SharedPipeline>> pipelines;
    QAtomicInt pipelineCompiles;

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT types,
                                                        const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                        void *userData);

    bool createInstance(const Options &options, QString *errorMessage);
    bool selectPhysicalDevice(QString *errorMessage);
    bool createDevice(const Options &options, QString *errorMessage);
};

KisGpuContext::KisGpuContext()
    : d(new Private)
{
}

std::shared_ptr<KisGpuComputePipeline> KisGpuContext::sharedComputePipeline(const quint32 *spirv,
                                                                            size_t spirvSizeBytes,
                                                                            quint32 pushConstantSize,
                                                                            QString *errorMessage)
{
    std::shared_ptr<Private::SharedPipeline> entry;
    {
        QMutexLocker locker(&d->pipelinesMutex);
        auto &slot = d->pipelines[std::make_pair(spirv, pushConstantSize)];
        if (!slot)
            slot = std::make_shared<Private::SharedPipeline>();
        entry = slot;
    }
    QMutexLocker locker(&entry->mutex);
    if (!entry->pipeline) {
        std::unique_ptr<KisGpuComputePipeline> pipeline =
            KisGpuComputePipeline::create(*this, spirv, spirvSizeBytes, pushConstantSize, errorMessage);
        if (!pipeline)
            return nullptr;
        entry->pipeline = std::move(pipeline);
        d->pipelineCompiles.ref();
    }
    return entry->pipeline;
}

int KisGpuContext::sharedComputePipelineCompileCount() const
{
    return d->pipelineCompiles.loadAcquire();
}

KisGpuContext::~KisGpuContext()
{
    if (d->device) {
        d->vk.vkDeviceWaitIdle(d->device);
        // Shared pipelines not referenced elsewhere are destroyed here.
        d->pipelines.clear();
        if (d->timeline) {
            d->vk.vkDestroySemaphore(d->device, d->timeline, nullptr);
        }
        d->vk.vkDestroyDevice(d->device, nullptr);
    }
    if (d->messenger) {
        d->destroyMessenger(d->instance, d->messenger, nullptr);
    }
    if (d->instance) {
        d->vk.vkDestroyInstance(d->instance, nullptr);
    }
}

VkBool32 KisGpuContext::Private::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                               VkDebugUtilsMessageTypeFlagsEXT types,
                                               const VkDebugUtilsMessengerCallbackDataEXT *data,
                                               void *userData)
{
    Q_UNUSED(types);
    Private *d = static_cast<Private *>(userData);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        d->validationErrors.ref();
        qWarning().noquote() << "Vulkan validation error:" << data->pMessage;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        qWarning().noquote() << "Vulkan validation warning:" << data->pMessage;
    }
    return VK_FALSE;
}

std::unique_ptr<KisGpuContext> KisGpuContext::create(QString *errorMessage)
{
    return create(Options(), errorMessage);
}

std::unique_ptr<KisGpuContext> KisGpuContext::create(const Options &options, QString *errorMessage)
{
    std::unique_ptr<KisGpuContext> context(new KisGpuContext);
    Private *d = context->d.get();

    if (!d->vk.loadGlobal(errorMessage)) {
        return nullptr;
    }
    if (!d->createInstance(options, errorMessage) || !d->selectPhysicalDevice(errorMessage)
        || !d->createDevice(options, errorMessage)) {
        return nullptr;
    }

    dbgKrita << "GPU engine device:" << d->info.summary();
    if (!d->info.isBlackwell()) {
        warnKrita << "GPU engine: device is not an NVIDIA Blackwell GPU; this configuration is untested";
    }
    return context;
}

bool KisGpuContext::Private::createInstance(const Options &options, QString *errorMessage)
{
    quint32 loaderVersion = 0;
    vk.vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < RequiredApiVersion) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Vulkan loader %1 is older than the required 1.3").arg(versionString(loaderVersion));
        }
        return false;
    }

    QVector<const char *> layers;
    const bool wantValidation = options.enableValidation || qEnvironmentVariableIntValue("KRITA_GPU_VALIDATION") == 1;
    if (wantValidation) {
        quint32 count = 0;
        vk.vkEnumerateInstanceLayerProperties(&count, nullptr);
        QVector<VkLayerProperties> available(static_cast<int>(count));
        vk.vkEnumerateInstanceLayerProperties(&count, available.data());
        for (const VkLayerProperties &layer : available) {
            if (std::strcmp(layer.layerName, ValidationLayerName) == 0) {
                layers << ValidationLayerName;
                validation = true;
            }
        }
        if (!validation) {
            warnKrita << "GPU engine: validation requested but" << ValidationLayerName << "is not installed";
        }
    }

    QVector<const char *> instanceExtensions;
    if (validation) {
        instanceExtensions << VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Solstice";
    appInfo.pEngineName = "Krita GPU engine";
    appInfo.apiVersion = RequiredApiVersion;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledLayerCount = quint32(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();
    createInfo.enabledExtensionCount = quint32(instanceExtensions.size());
    createInfo.ppEnabledExtensionNames = instanceExtensions.data();

    const VkResult result = vk.vkCreateInstance(&createInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("vkCreateInstance failed: %1").arg(kisGpuVkResultString(result));
        }
        return false;
    }
    if (!vk.loadInstance(instance, errorMessage)) {
        return false;
    }

    if (validation) {
        const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vk.vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vk.vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (createMessenger && destroyMessenger) {
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
            messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messengerInfo.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messengerInfo.pfnUserCallback = &Private::debugCallback;
            messengerInfo.pUserData = this;
            createMessenger(instance, &messengerInfo, nullptr, &messenger);
        }
    }
    return true;
}

bool KisGpuContext::Private::selectPhysicalDevice(QString *errorMessage)
{
    quint32 count = 0;
    vk.vkEnumeratePhysicalDevices(instance, &count, nullptr);
    QVector<VkPhysicalDevice> devices(static_cast<int>(count));
    vk.vkEnumeratePhysicalDevices(instance, &count, devices.data());

    QStringList rejected;
    int bestScore = -1;
    const bool allowSoftware = qEnvironmentVariableIntValue("KRITA_GPU_ALLOW_SOFTWARE_DEVICE") == 1;

    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vk.vkGetPhysicalDeviceProperties(candidate, &properties);
        const QString name = QString::fromUtf8(properties.deviceName);

        if (!KisGpuContext::acceptsDeviceType(properties.deviceType, allowSoftware)) {
            rejected << QStringLiteral("%1: software (CPU) device").arg(name);
            continue;
        }
        if (properties.apiVersion < RequiredApiVersion) {
            rejected << QStringLiteral("%1: Vulkan %2 < 1.3").arg(name, versionString(properties.apiVersion));
            continue;
        }

        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features12.pNext = &features13;
        VkPhysicalDeviceVulkan11Features features11{};
        features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        features13.pNext = &features11;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &features12;
        vk.vkGetPhysicalDeviceFeatures2(candidate, &features);

        const bool featuresOk = features.features.shaderInt64 && features12.timelineSemaphore
            && features12.bufferDeviceAddress && features12.shaderFloat16 && features11.storageBuffer16BitAccess
            && features13.synchronization2;
        if (!featuresOk) {
            rejected << QStringLiteral("%1: missing required features").arg(name);
            continue;
        }

        quint32 familyCount = 0;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        QVector<VkQueueFamilyProperties> families(static_cast<int>(familyCount));
        vk.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

        // Prefer the universal (graphics + compute) family: the canvas will
        // later use the same queue for presentation copies.
        int family = -1;
        for (int i = 0; i < families.size(); i++) {
            const VkQueueFlags flags = families[i].queueFlags;
            if ((flags & VK_QUEUE_COMPUTE_BIT) && (flags & VK_QUEUE_GRAPHICS_BIT) && families[i].timestampValidBits) {
                family = i;
                break;
            }
        }
        if (family < 0) {
            rejected << QStringLiteral("%1: no graphics+compute queue").arg(name);
            continue;
        }

        int score = 0;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 100;
        }
        if (properties.vendorID == NvidiaVendorId) {
            score += 10;
        }
        if (score > bestScore) {
            bestScore = score;
            physicalDevice = candidate;
            queueFamily = quint32(family);
        }
    }

    if (!physicalDevice) {
        if (errorMessage) {
            *errorMessage = rejected.isEmpty()
                ? QStringLiteral("No Vulkan device found")
                : QStringLiteral("No suitable Vulkan device: %1").arg(rejected.join(QStringLiteral("; ")));
        }
        return false;
    }

    VkPhysicalDeviceIDProperties idProperties{};
    idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceDriverProperties driverProperties{};
    driverProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    idProperties.pNext = &driverProperties;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &idProperties;
    vk.vkGetPhysicalDeviceProperties2(physicalDevice, &properties);

    info.name = QString::fromUtf8(properties.properties.deviceName);
    info.vendorId = properties.properties.vendorID;
    info.deviceId = properties.properties.deviceID;
    info.apiVersion = properties.properties.apiVersion;
    info.driverName = QString::fromUtf8(driverProperties.driverName);
    info.driverInfo = QString::fromUtf8(driverProperties.driverInfo);
    info.deviceUuid = QByteArray(reinterpret_cast<const char *>(idProperties.deviceUUID), VK_UUID_SIZE);
    info.driverUuid = QByteArray(reinterpret_cast<const char *>(idProperties.driverUUID), VK_UUID_SIZE);
    info.timestampPeriodNs = properties.properties.limits.timestampPeriod;
    info.maxComputeWorkGroupInvocations = properties.properties.limits.maxComputeWorkGroupInvocations;

    vk.vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (quint32 i = 0; i < memoryProperties.memoryHeapCount; i++) {
        const VkMemoryHeap &heap = memoryProperties.memoryHeaps[i];
        if (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            info.deviceLocalBytes = qMax(info.deviceLocalBytes, heap.size);
        }
    }
    for (quint32 i = 0; i < memoryProperties.memoryTypeCount; i++) {
        const VkMemoryType &type = memoryProperties.memoryTypes[i];
        const VkMemoryPropertyFlags rebar = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        if ((type.propertyFlags & rebar) == rebar) {
            info.hostVisibleDeviceLocalBytes =
                qMax(info.hostVisibleDeviceLocalBytes, memoryProperties.memoryHeaps[type.heapIndex].size);
        }
    }

    return true;
}

bool KisGpuContext::Private::createDevice(const Options &options, QString *errorMessage)
{
    quint32 extensionCount = 0;
    vk.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
    QVector<VkExtensionProperties> available(static_cast<int>(extensionCount));
    vk.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, available.data());

    QVector<const char *> extensions;
    if (options.requestInterop) {
        const QVector<const char *> wanted = interopExtensions();
        bool all = !wanted.isEmpty();
        for (const char *name : wanted) {
            all = all && hasExtension(available, name);
        }
        if (all) {
            extensions += wanted;
            info.supportsExternalMemoryInterop = true;
        }
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan11Features features11{};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.storageBuffer16BitAccess = VK_TRUE;
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore = VK_TRUE;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.shaderFloat16 = VK_TRUE;
    features12.pNext = &features11;
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.synchronization2 = VK_TRUE;
    features13.pNext = &features12;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.features.shaderInt64 = VK_TRUE;
    features.pNext = &features13;
    {
        // Optional (phase 4.94): exact transform resampling uses doubles.
        VkPhysicalDeviceFeatures2 supported{};
        supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        vk.vkGetPhysicalDeviceFeatures2(physicalDevice, &supported);
        info.supportsFloat64 = supported.features.shaderFloat64 == VK_TRUE;
        features.features.shaderFloat64 = supported.features.shaderFloat64;
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &features;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = quint32(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkResult result = vk.vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
    if (result != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("vkCreateDevice failed: %1").arg(kisGpuVkResultString(result));
        }
        return false;
    }
    if (!vk.loadDevice(device, errorMessage)) {
        return false;
    }
    vk.vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VkSemaphoreTypeCreateInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue = 0;
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &timelineInfo;
    result = vk.vkCreateSemaphore(device, &semaphoreInfo, nullptr, &timeline);
    if (result != VK_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Timeline semaphore creation failed: %1").arg(kisGpuVkResultString(result));
        }
        return false;
    }
    return true;
}

const KisGpuVulkanFunctions &KisGpuContext::vk() const
{
    return d->vk;
}

const KisGpuDeviceInfo &KisGpuContext::deviceInfo() const
{
    return d->info;
}

VkInstance KisGpuContext::instance() const
{
    return d->instance;
}

VkPhysicalDevice KisGpuContext::physicalDevice() const
{
    return d->physicalDevice;
}

VkDevice KisGpuContext::device() const
{
    return d->device;
}

quint32 KisGpuContext::queueFamilyIndex() const
{
    return d->queueFamily;
}

bool KisGpuContext::validationEnabled() const
{
    return d->validation;
}

int KisGpuContext::validationErrorCount() const
{
    return d->validationErrors.loadRelaxed();
}

int KisGpuContext::findMemoryType(quint32 typeBits,
                                  VkMemoryPropertyFlags required,
                                  VkMemoryPropertyFlags preferred) const
{
    int fallback = -1;
    for (quint32 i = 0; i < d->memoryProperties.memoryTypeCount; i++) {
        if (!(typeBits & (1u << i))) {
            continue;
        }
        const VkMemoryPropertyFlags flags = d->memoryProperties.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            return int(i);
        }
        if (fallback < 0) {
            fallback = int(i);
        }
    }
    return fallback;
}

VkMemoryPropertyFlags KisGpuContext::memoryTypeFlags(int memoryTypeIndex) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(memoryTypeIndex >= 0
                                             && quint32(memoryTypeIndex) < d->memoryProperties.memoryTypeCount,
                                         0);
    return d->memoryProperties.memoryTypes[memoryTypeIndex].propertyFlags;
}

quint64 KisGpuContext::submit(VkCommandBuffer commandBuffer,
                              const QVector<VkSemaphoreSubmitInfo> &waitSemaphores,
                              const QVector<VkSemaphoreSubmitInfo> &signalSemaphores)
{
    QVector<VkCommandBuffer> commandBuffers;
    if (commandBuffer) {
        commandBuffers << commandBuffer;
    }
    return submit(commandBuffers, waitSemaphores, signalSemaphores);
}

quint64 KisGpuContext::submit(const QVector<VkCommandBuffer> &commandBuffers,
                              const QVector<VkSemaphoreSubmitInfo> &waitSemaphores,
                              const QVector<VkSemaphoreSubmitInfo> &signalSemaphores,
                              KisGpuSubmitTiming *timing)
{
    const auto stamp = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    if (timing) {
        *timing = {};
        timing->lockStartNs = stamp();
    }
    QMutexLocker locker(&d->queueMutex);
    if (timing)
        timing->lockAcquiredNs = stamp();

    if (d->injectedSubmitFailures > 0) {
        d->injectedSubmitFailures--;
        warnKrita << "GPU engine: injected submission failure";
        return 0;
    }

    const quint64 value = d->lastSubmitted + 1;

    QVector<VkSemaphoreSubmitInfo> signalInfos = signalSemaphores;
    VkSemaphoreSubmitInfo timelineSignal{};
    timelineSignal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    timelineSignal.semaphore = d->timeline;
    timelineSignal.value = value;
    timelineSignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signalInfos << timelineSignal;

    QVector<VkCommandBufferSubmitInfo> commandInfos;
    for (VkCommandBuffer commandBuffer : commandBuffers) {
        VkCommandBufferSubmitInfo commandInfo{};
        commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        commandInfo.commandBuffer = commandBuffer;
        commandInfos << commandInfo;
    }

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = quint32(waitSemaphores.size());
    submitInfo.pWaitSemaphoreInfos = waitSemaphores.data();
    submitInfo.commandBufferInfoCount = quint32(commandInfos.size());
    submitInfo.pCommandBufferInfos = commandInfos.isEmpty() ? nullptr : commandInfos.data();
    submitInfo.signalSemaphoreInfoCount = quint32(signalInfos.size());
    submitInfo.pSignalSemaphoreInfos = signalInfos.data();

    if (timing)
        timing->driverStartNs = stamp();
    const VkResult result = d->vk.vkQueueSubmit2(d->queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (timing)
        timing->driverEndNs = stamp();
    if (result != VK_SUCCESS) {
        warnKrita << "GPU engine: vkQueueSubmit2 failed:" << kisGpuVkResultString(result);
        return 0;
    }
    d->lastSubmitted = value;
    return value;
}

bool KisGpuContext::wait(quint64 value, quint64 timeoutNs) const
{
    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &d->timeline;
    waitInfo.pValues = &value;
    return d->vk.vkWaitSemaphores(d->device, &waitInfo, timeoutNs) == VK_SUCCESS;
}

quint64 KisGpuContext::completedValue() const
{
    quint64 value = 0;
    d->vk.vkGetSemaphoreCounterValue(d->device, d->timeline, &value);
    return value;
}

void KisGpuContext::injectSubmitFailuresForTesting(int count)
{
    QMutexLocker locker(&d->queueMutex);
    d->injectedSubmitFailures = count;
}

void KisGpuContext::waitIdle()
{
    QMutexLocker locker(&d->queueMutex);
    d->vk.vkDeviceWaitIdle(d->device);
}
