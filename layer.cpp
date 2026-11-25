#include <vulkan/vk_layer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {
struct InstanceDispatch {
    VkInstance handle;
    PFN_vkGetInstanceProcAddr getProc;
    PFN_GetPhysicalDeviceProcAddr getPhysicalProc;
    PFN_vkDestroyInstance destroy;
    PFN_vkGetPhysicalDeviceProperties properties;
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr getProc;
    PFN_vkDestroyDevice destroy;
    PFN_vkCreateSampler createSampler;
    float maxBias;
};

std::mutex dispatchMutex;
std::unordered_map<void*, InstanceDispatch> instances;
std::unordered_map<void*, DeviceDispatch> devices;

template <typename Handle>
void* dispatchKey(Handle handle) {
    // Instance and physical-device handles share the loader's instance dispatch key.
    void* key;
    std::memcpy(&key, handle, sizeof key);
    return key;
}

template <typename Map, typename Handle>
typename Map::mapped_type lookup(const Map& map, Handle handle) {
    if (!handle)
        return {};
    std::lock_guard lock(dispatchMutex);
    const auto entry = map.find(dispatchKey(handle));
    return entry == map.end() ? typename Map::mapped_type{} : entry->second;
}

template <typename LayerInfo, typename CreateInfo>
LayerInfo* findLink(const CreateInfo* createInfo, VkStructureType type) {
    const auto* entry = static_cast<const VkBaseInStructure*>(createInfo->pNext);
    for (; entry; entry = entry->pNext) {
        if (entry->sType == type) {
            auto* info = reinterpret_cast<const LayerInfo*>(entry);
            if (info->function == VK_LAYER_LINK_INFO)
                return const_cast<LayerInfo*>(info);
        }
    }
    return nullptr;
}

float setting(const char* name, float fallback) {
    const char* value = std::getenv(name);
    if (!value)
        return fallback;
    char* end;
    const float parsed = std::strtof(value, &end);
    return end != value && *end == '\0' && std::isfinite(parsed) && parsed >= 0
        ? parsed : fallback;
}

VKAPI_ATTR VkResult VKAPI_CALL createInstance(
    const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance) {
    auto* link = findLink<VkLayerInstanceCreateInfo>(info, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!link || !link->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    // Advance the loader's link before calling the next layer's create function.
    const auto next = *link->u.pLayerInfo;
    link->u.pLayerInfo = next.pNext;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(next.pfnNextGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = create(info, allocator, instance);
    if (result != VK_SUCCESS)
        return result;
    const InstanceDispatch dispatch{
        *instance, next.pfnNextGetInstanceProcAddr, next.pfnNextGetPhysicalDeviceProcAddr,
        reinterpret_cast<PFN_vkDestroyInstance>(next.pfnNextGetInstanceProcAddr(*instance, "vkDestroyInstance")),
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(next.pfnNextGetInstanceProcAddr(*instance, "vkGetPhysicalDeviceProperties"))
    };
    try {
        std::lock_guard lock(dispatchMutex);
        instances.emplace(dispatchKey(*instance), dispatch);
    } catch (...) {
        dispatch.destroy(*instance, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    const auto dispatch = lookup(instances, instance);
    if (!dispatch.destroy)
        return;
    {
        std::lock_guard lock(dispatchMutex);
        instances.erase(dispatchKey(instance));
    }
    dispatch.destroy(instance, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL createDevice(
    VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDevice* device) {
    auto* link = findLink<VkLayerDeviceCreateInfo>(info, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    const auto instance = lookup(instances, physical);
    if (!link || !link->u.pLayerInfo || !instance.properties)
        return VK_ERROR_INITIALIZATION_FAILED;
    // Device creation has its own link chain and supplies the downstream device functions.
    const auto next = *link->u.pLayerInfo;
    link->u.pLayerInfo = next.pNext;
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(next.pfnNextGetInstanceProcAddr(instance.handle, "vkCreateDevice"));
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    // Cache the selected GPU's sampler bias limit for subsequent sampler creation.
    VkPhysicalDeviceProperties properties{};
    instance.properties(physical, &properties);
    const VkResult result = create(physical, info, allocator, device);
    if (result != VK_SUCCESS)
        return result;
    const auto get = next.pfnNextGetDeviceProcAddr;
    const DeviceDispatch dispatch{
        get,
        reinterpret_cast<PFN_vkDestroyDevice>(get(*device, "vkDestroyDevice")),
        reinterpret_cast<PFN_vkCreateSampler>(get(*device, "vkCreateSampler")),
        properties.limits.maxSamplerLodBias
    };
    try {
        std::lock_guard lock(dispatchMutex);
        devices.emplace(dispatchKey(*device), dispatch);
    } catch (...) {
        dispatch.destroy(*device, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    const auto dispatch = lookup(devices, device);
    if (!dispatch.destroy)
        return;
    {
        std::lock_guard lock(dispatchMutex);
        devices.erase(dispatchKey(device));
    }
    dispatch.destroy(device, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL createSampler(
    VkDevice device, const VkSamplerCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkSampler* sampler) {
    const auto dispatch = lookup(devices, device);
    if (!dispatch.createSampler)
        return VK_ERROR_INITIALIZATION_FAILED;
    // Copy the create info to preserve the caller's data, including its extension chain.
    VkSamplerCreateInfo adjusted = *info;
    if (!info->compareEnable && !info->unnormalizedCoordinates && info->maxLod > info->minLod
        && !(info->flags & VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT)) {
        const float bias = setting("TF2_LOD_BIAS", 15.0f);
        const float maxMip = setting("TF2_LOD_MAX_MIP", 5.0f);
        adjusted.mipLodBias = std::clamp(info->mipLodBias + bias, -dispatch.maxBias, dispatch.maxBias);
        // Respect the sampler's minimum LOD and any stricter cap set by the game.
        adjusted.maxLod = std::clamp(maxMip, info->minLod, info->maxLod);
        if (std::getenv("TF2_LOD_TRACE"))
            std::fprintf(stderr, "TF2 texture LOD: bias %.2f -> %.2f, max mip %.2f -> %.2f\n",
                         info->mipLodBias, adjusted.mipLodBias, info->maxLod, adjusted.maxLod);
    }
    return dispatch.createSampler(device, &adjusted, allocator, sampler);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getDeviceProc(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(getDeviceProc);
    if (std::strcmp(name, "vkCreateSampler") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(createSampler);
    if (std::strcmp(name, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
    const auto dispatch = lookup(devices, device);
    return dispatch.getProc ? dispatch.getProc(device, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getInstanceProc(VkInstance instance, const char* name) {
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(getInstanceProc);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(getDeviceProc);
    if (std::strcmp(name, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(createInstance);
    if (std::strcmp(name, "vkDestroyInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(destroyInstance);
    if (std::strcmp(name, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(createDevice);
    if (std::strcmp(name, "vkCreateSampler") == 0 || std::strcmp(name, "vkDestroyDevice") == 0)
        return getDeviceProc(nullptr, name);
    const auto dispatch = lookup(instances, instance);
    return dispatch.getProc ? dispatch.getProc(instance, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getPhysicalProc(VkInstance instance, const char* name) {
    const auto dispatch = lookup(instances, instance);
    return dispatch.getPhysicalProc ? dispatch.getPhysicalProc(instance, name) : nullptr;
}
}

extern "C" __attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        version->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = getInstanceProc;
    version->pfnGetDeviceProcAddr = getDeviceProc;
    version->pfnGetPhysicalDeviceProcAddr = getPhysicalProc;
    return VK_SUCCESS;
}
