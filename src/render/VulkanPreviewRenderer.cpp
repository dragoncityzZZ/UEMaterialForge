#include "render/VulkanPreviewRenderer.h"

#if defined(_WIN32)

#include "io/TextureLoader.h"
#include "util/PathUtil.h"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

namespace mf {
namespace {

namespace fs = std::filesystem;

constexpr int kMaxMaterialTextures = 12;
constexpr uint32_t kUniformBufferSize = 1280;

struct PreviewConstants {
    float cameraPos[4] = {};
    float cameraRight[4] = {};
    float cameraUp[4] = {};
    float cameraForward[4] = {};
    float resolution[4] = {};
    float time = 0.0f;
    float meshType = 0.0f;
    float twoSided = 0.0f;
    float padding = 0.0f;
    float parameters[64][4] = {};
};

static_assert(sizeof(PreviewConstants) <= kUniformBufferSize,
              "The Vulkan preview uniform buffer is too small.");

std::string VulkanResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    default: break;
    }
    return "VkResult(" + std::to_string(static_cast<int>(result)) + ")";
}

bool Check(VkResult result, const char* operation, std::string* error) {
    if (result == VK_SUCCESS) return true;
    if (error) *error = std::string(operation) + " failed: " + VulkanResultName(result);
    return false;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

std::string WideToUtf8(const wchar_t* text) {
    if (!text || !text[0]) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    return result;
}

std::string FindGlslangExecutable() {
    std::vector<fs::path> candidates;
    candidates.push_back(PathUtil::ExecutableDirectory() / "tools" / "glslang" / "glslang.exe");
    candidates.push_back(PathUtil::ExecutableDirectory() / "glslang.exe");
    const fs::path root = PathUtil::FindProjectRoot();
    if (!root.empty()) {
        candidates.push_back(root / "third_party" / "glslang" / "bin" / "glslang.exe");
    }
    candidates.push_back(PathUtil::CurrentDirectory() / "third_party" / "glslang" / "bin" /
                         "glslang.exe");
    if (const char* sdk = std::getenv("VULKAN_SDK"); sdk && sdk[0]) {
        const fs::path sdkPath(sdk);
        candidates.push_back(sdkPath / "Bin" / "glslangValidator.exe");
        candidates.push_back(sdkPath / "Bin" / "glslang.exe");
    }
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return PathUtil::ToUtf8(candidate);
        }
    }
    return {};
}

} // namespace

VulkanPreviewRenderer::~VulkanPreviewRenderer() {
    Shutdown();
}

bool VulkanPreviewRenderer::Probe(std::string* details, std::string* error) {
    if (details) details->clear();
    if (error) error->clear();
    if (FindGlslangExecutable().empty()) {
        if (error) {
            *error = "The Vulkan shader compiler (tools/glslang/glslang.exe) was not found.";
        }
        return false;
    }
    HMODULE library = LoadLibraryA("vulkan-1.dll");
    if (!library) {
        if (error) *error = "vulkan-1.dll is not installed.";
        return false;
    }
    auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(library, "vkGetInstanceProcAddr"));
    if (!getInstanceProcAddr) {
        FreeLibrary(library);
        if (error) *error = "vulkan-1.dll does not export vkGetInstanceProcAddr.";
        return false;
    }
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        getInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!createInstance) {
        FreeLibrary(library);
        if (error) *error = "Could not load vkCreateInstance.";
        return false;
    }
    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "MaterialForge";
    application.applicationVersion = VK_MAKE_VERSION(1, 2, 0);
    application.pEngineName = "MaterialForge";
    application.engineVersion = VK_MAKE_VERSION(1, 2, 0);
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (createInstance(&createInfo, nullptr, &instance) != VK_SUCCESS || !instance) {
        FreeLibrary(library);
        if (error) *error = "Could not create a Vulkan instance.";
        return false;
    }
    auto enumerateDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        getInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
    auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        getInstanceProcAddr(instance, "vkDestroyInstance"));
    bool found = false;
    if (enumerateDevices && getProperties) {
        uint32_t count = 0;
        if (enumerateDevices(instance, &count, nullptr) == VK_SUCCESS && count > 0) {
            std::vector<VkPhysicalDevice> devices(count);
            if (enumerateDevices(instance, &count, devices.data()) == VK_SUCCESS) {
                for (VkPhysicalDevice device : devices) {
                    VkPhysicalDeviceProperties properties = {};
                    getProperties(device, &properties);
                    if (details) *details = properties.deviceName;
                    found = true;
                    break;
                }
            }
        }
    }
    if (destroyInstance) destroyInstance(instance, nullptr);
    FreeLibrary(library);
    if (!found && error) *error = "No Vulkan physical device was found.";
    return found;
}

bool VulkanPreviewRenderer::LoadVulkanLibrary(std::string* error) {
    m_Library = LoadLibraryA("vulkan-1.dll");
    if (!m_Library) {
        if (error) *error = "vulkan-1.dll is not installed.";
        return false;
    }
    m_Functions.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(static_cast<HMODULE>(m_Library), "vkGetInstanceProcAddr"));
    if (!m_Functions.vkGetInstanceProcAddr) {
        if (error) *error = "vulkan-1.dll does not export vkGetInstanceProcAddr.";
        return false;
    }
    auto loadGlobal = [&](const char* name) -> PFN_vkVoidFunction {
        return m_Functions.vkGetInstanceProcAddr(nullptr, name);
    };
    m_Functions.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        loadGlobal("vkCreateInstance"));
    m_Functions.vkEnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        loadGlobal("vkEnumerateInstanceVersion"));
    return m_Functions.vkCreateInstance != nullptr;
}

bool VulkanPreviewRenderer::CreateInstance(std::string* error) {
    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "MaterialForge";
    application.applicationVersion = VK_MAKE_VERSION(1, 2, 0);
    application.pEngineName = "MaterialForge";
    application.engineVersion = VK_MAKE_VERSION(1, 2, 0);
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &application;
    const VkResult result = m_Functions.vkCreateInstance(&createInfo, nullptr, &m_Instance);
    if (!Check(result, "vkCreateInstance", error)) return false;

    auto loadInstance = [&](const char* name) -> PFN_vkVoidFunction {
        return m_Functions.vkGetInstanceProcAddr(m_Instance, name);
    };
    m_Functions.vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        loadInstance("vkDestroyInstance"));
    m_Functions.vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        loadInstance("vkEnumeratePhysicalDevices"));
    m_Functions.vkGetPhysicalDeviceProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            loadInstance("vkGetPhysicalDeviceProperties"));
    m_Functions.vkGetPhysicalDeviceFormatProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
            loadInstance("vkGetPhysicalDeviceFormatProperties"));
    m_Functions.vkGetPhysicalDeviceQueueFamilyProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            loadInstance("vkGetPhysicalDeviceQueueFamilyProperties"));
    m_Functions.vkGetPhysicalDeviceMemoryProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            loadInstance("vkGetPhysicalDeviceMemoryProperties"));
    m_Functions.vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
        loadInstance("vkCreateDevice"));
    m_Functions.vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        loadInstance("vkGetDeviceProcAddr"));
    return true;
}

bool VulkanPreviewRenderer::PickPhysicalDevice(std::string* error) {
    uint32_t count = 0;
    if (!Check(m_Functions.vkEnumeratePhysicalDevices(m_Instance, &count, nullptr),
               "vkEnumeratePhysicalDevices", error)) {
        return false;
    }
    if (count == 0) {
        if (error) *error = "No Vulkan physical devices are available.";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (!Check(m_Functions.vkEnumeratePhysicalDevices(m_Instance, &count, devices.data()),
               "vkEnumeratePhysicalDevices", error)) {
        return false;
    }
    for (VkPhysicalDevice device : devices) {
        uint32_t familyCount = 0;
        m_Functions.vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        m_Functions.vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount,
                                                             families.data());
        for (uint32_t i = 0; i < familyCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                m_PhysicalDevice = device;
                m_QueueFamily = i;
                break;
            }
        }
        if (m_PhysicalDevice != VK_NULL_HANDLE) break;
    }
    if (m_PhysicalDevice == VK_NULL_HANDLE) {
        if (error) *error = "No Vulkan device with a compute queue was found.";
        return false;
    }
    VkPhysicalDeviceProperties properties = {};
    m_Functions.vkGetPhysicalDeviceProperties(m_PhysicalDevice, &properties);
    m_DeviceName = properties.deviceName;
    m_ApiVersion = std::to_string(VK_VERSION_MAJOR(properties.apiVersion)) + "." +
                   std::to_string(VK_VERSION_MINOR(properties.apiVersion)) + "." +
                   std::to_string(VK_VERSION_PATCH(properties.apiVersion));
    VkFormatProperties formatProperties = {};
    m_Functions.vkGetPhysicalDeviceFormatProperties(
        m_PhysicalDevice, VK_FORMAT_R8G8B8A8_UNORM, &formatProperties);
    if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        if (error) {
            *error = "The Vulkan device does not support an R8G8B8A8 storage image.";
        }
        return false;
    }
    return true;
}

bool VulkanPreviewRenderer::CreateDevice(std::string* error) {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = m_QueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    if (!Check(m_Functions.vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device),
               "vkCreateDevice", error)) {
        return false;
    }
    auto loadDevice = [&](const char* name) -> PFN_vkVoidFunction {
        return m_Functions.vkGetDeviceProcAddr(m_Device, name);
    };
    m_Functions.vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
        loadDevice("vkDestroyDevice"));
    m_Functions.vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
        loadDevice("vkGetDeviceQueue"));
    m_Functions.vkDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
        loadDevice("vkDeviceWaitIdle"));
    m_Functions.vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(
        loadDevice("vkCreateCommandPool"));
    m_Functions.vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
        loadDevice("vkDestroyCommandPool"));
    m_Functions.vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
        loadDevice("vkAllocateCommandBuffers"));
    m_Functions.vkFreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(
        loadDevice("vkFreeCommandBuffers"));
    m_Functions.vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
        loadDevice("vkBeginCommandBuffer"));
    m_Functions.vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
        loadDevice("vkEndCommandBuffer"));
    m_Functions.vkResetCommandBuffer = reinterpret_cast<PFN_vkResetCommandBuffer>(
        loadDevice("vkResetCommandBuffer"));
    m_Functions.vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(loadDevice("vkQueueSubmit"));
    m_Functions.vkQueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(
        loadDevice("vkQueueWaitIdle"));
    m_Functions.vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(loadDevice("vkCreateBuffer"));
    m_Functions.vkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(
        loadDevice("vkDestroyBuffer"));
    m_Functions.vkGetBufferMemoryRequirements =
        reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
            loadDevice("vkGetBufferMemoryRequirements"));
    m_Functions.vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(
        loadDevice("vkAllocateMemory"));
    m_Functions.vkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(loadDevice("vkFreeMemory"));
    m_Functions.vkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(
        loadDevice("vkBindBufferMemory"));
    m_Functions.vkCreateImage = reinterpret_cast<PFN_vkCreateImage>(loadDevice("vkCreateImage"));
    m_Functions.vkDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(
        loadDevice("vkDestroyImage"));
    m_Functions.vkGetImageMemoryRequirements =
        reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
            loadDevice("vkGetImageMemoryRequirements"));
    m_Functions.vkBindImageMemory = reinterpret_cast<PFN_vkBindImageMemory>(
        loadDevice("vkBindImageMemory"));
    m_Functions.vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(
        loadDevice("vkCreateImageView"));
    m_Functions.vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
        loadDevice("vkDestroyImageView"));
    m_Functions.vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(
        loadDevice("vkCreateShaderModule"));
    m_Functions.vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
        loadDevice("vkDestroyShaderModule"));
    m_Functions.vkCreateDescriptorSetLayout =
        reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
            loadDevice("vkCreateDescriptorSetLayout"));
    m_Functions.vkDestroyDescriptorSetLayout =
        reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
            loadDevice("vkDestroyDescriptorSetLayout"));
    m_Functions.vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(
        loadDevice("vkCreatePipelineLayout"));
    m_Functions.vkDestroyPipelineLayout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(
        loadDevice("vkDestroyPipelineLayout"));
    m_Functions.vkCreateComputePipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(
        loadDevice("vkCreateComputePipelines"));
    m_Functions.vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(
        loadDevice("vkDestroyPipeline"));
    m_Functions.vkCreateDescriptorPool = reinterpret_cast<PFN_vkCreateDescriptorPool>(
        loadDevice("vkCreateDescriptorPool"));
    m_Functions.vkDestroyDescriptorPool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(
        loadDevice("vkDestroyDescriptorPool"));
    m_Functions.vkAllocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(
        loadDevice("vkAllocateDescriptorSets"));
    m_Functions.vkUpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(
        loadDevice("vkUpdateDescriptorSets"));
    m_Functions.vkCreateSampler = reinterpret_cast<PFN_vkCreateSampler>(
        loadDevice("vkCreateSampler"));
    m_Functions.vkDestroySampler = reinterpret_cast<PFN_vkDestroySampler>(
        loadDevice("vkDestroySampler"));
    m_Functions.vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(
        loadDevice("vkCmdBindPipeline"));
    m_Functions.vkCmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
        loadDevice("vkCmdBindDescriptorSets"));
    m_Functions.vkCmdDispatch = reinterpret_cast<PFN_vkCmdDispatch>(
        loadDevice("vkCmdDispatch"));
    m_Functions.vkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(
        loadDevice("vkCmdPipelineBarrier"));
    m_Functions.vkCmdCopyBufferToImage = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(
        loadDevice("vkCmdCopyBufferToImage"));
    m_Functions.vkCmdCopyImageToBuffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
        loadDevice("vkCmdCopyImageToBuffer"));
    m_Functions.vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(loadDevice("vkMapMemory"));
    m_Functions.vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(loadDevice("vkUnmapMemory"));

    m_Functions.vkGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_Queue);
    return m_Queue != VK_NULL_HANDLE;
}

bool VulkanPreviewRenderer::CreateDescriptorSetLayout(std::string* error) {
    VkDescriptorSetLayoutBinding bindings[16] = {};
    uint32_t count = 0;

    bindings[count].binding = 0;
    bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[count].descriptorCount = 1;
    bindings[count].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    ++count;

    for (uint32_t i = 0; i <= static_cast<uint32_t>(kMaxMaterialTextures); ++i) {
        bindings[count].binding = 1 + i;
        bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[count].descriptorCount = 1;
        bindings[count].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        ++count;
    }

    bindings[count].binding = 15;
    bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[count].descriptorCount = 1;
    bindings[count].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    ++count;

    bindings[count].binding = 17;
    bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[count].descriptorCount = 1;
    bindings[count].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    ++count;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = count;
    layoutInfo.pBindings = bindings;
    if (!Check(m_Functions.vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr,
                                                      &m_DescriptorSetLayout),
               "vkCreateDescriptorSetLayout", error)) {
        return false;
    }

    VkDescriptorPoolSize sizes[4] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[1].descriptorCount = kMaxMaterialTextures + 1;
    sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[2].descriptorCount = 1;
    sizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[3].descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = sizes;
    if (!Check(m_Functions.vkCreateDescriptorPool(m_Device, &poolInfo, nullptr,
                                                  &m_DescriptorPool),
               "vkCreateDescriptorPool", error)) {
        return false;
    }
    VkDescriptorSetAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_DescriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &m_DescriptorSetLayout;
    return Check(m_Functions.vkAllocateDescriptorSets(m_Device, &allocateInfo, &m_DescriptorSet),
                 "vkAllocateDescriptorSets", error);
}

bool VulkanPreviewRenderer::CreatePipelineLayout(std::string* error) {
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_DescriptorSetLayout;
    return Check(m_Functions.vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr,
                                                    &m_PipelineLayout),
                 "vkCreatePipelineLayout", error);
}

bool VulkanPreviewRenderer::CreateCommands(std::string* error) {
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_QueueFamily;
    if (!Check(m_Functions.vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool),
               "vkCreateCommandPool", error)) {
        return false;
    }
    VkCommandBufferAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = m_CommandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    return Check(m_Functions.vkAllocateCommandBuffers(m_Device, &allocateInfo, &m_CommandBuffer),
                 "vkAllocateCommandBuffers", error);
}

bool VulkanPreviewRenderer::CreateSampler(std::string* error) {
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    return Check(m_Functions.vkCreateSampler(m_Device, &samplerInfo, nullptr, &m_Sampler),
                 "vkCreateSampler", error);
}

uint32_t VulkanPreviewRenderer::FindMemoryType(uint32_t typeBits,
                                               VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memory = {};
    m_Functions.vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

bool VulkanPreviewRenderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                         VkMemoryPropertyFlags properties, GpuBuffer* target,
                                         std::string* error) {
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(m_Functions.vkCreateBuffer(m_Device, &bufferInfo, nullptr, &target->buffer),
               "vkCreateBuffer", error)) {
        return false;
    }
    VkMemoryRequirements requirements = {};
    m_Functions.vkGetBufferMemoryRequirements(m_Device, target->buffer, &requirements);
    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, properties);
    if (!Check(m_Functions.vkAllocateMemory(m_Device, &allocateInfo, nullptr, &target->memory),
               "vkAllocateMemory", error)) {
        return false;
    }
    if (!Check(m_Functions.vkBindBufferMemory(m_Device, target->buffer, target->memory, 0),
               "vkBindBufferMemory", error)) {
        return false;
    }
    target->size = size;
    return true;
}

bool VulkanPreviewRenderer::CreateImage(uint32_t width, uint32_t height, VkFormat format,
                                        VkImageUsageFlags usage, GpuTexture* target,
                                        std::string* error) {
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(m_Functions.vkCreateImage(m_Device, &imageInfo, nullptr, &target->image),
               "vkCreateImage", error)) {
        return false;
    }
    VkMemoryRequirements requirements = {};
    m_Functions.vkGetImageMemoryRequirements(m_Device, target->image, &requirements);
    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex =
        FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!Check(m_Functions.vkAllocateMemory(m_Device, &allocateInfo, nullptr, &target->memory),
               "vkAllocateMemory", error)) {
        return false;
    }
    if (!Check(m_Functions.vkBindImageMemory(m_Device, target->image, target->memory, 0),
               "vkBindImageMemory", error)) {
        return false;
    }
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = target->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (!Check(m_Functions.vkCreateImageView(m_Device, &viewInfo, nullptr, &target->view),
               "vkCreateImageView", error)) {
        return false;
    }
    target->width = static_cast<int>(width);
    target->height = static_cast<int>(height);
    return true;
}

bool VulkanPreviewRenderer::BeginSingleTimeCommands(std::string* error) {
    m_Functions.vkResetCommandBuffer(m_CommandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return Check(m_Functions.vkBeginCommandBuffer(m_CommandBuffer, &beginInfo),
                 "vkBeginCommandBuffer", error);
}

bool VulkanPreviewRenderer::EndSingleTimeCommands(std::string* error) {
    if (!Check(m_Functions.vkEndCommandBuffer(m_CommandBuffer), "vkEndCommandBuffer", error)) {
        return false;
    }
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_CommandBuffer;
    if (!Check(m_Functions.vkQueueSubmit(m_Queue, 1, &submitInfo, VK_NULL_HANDLE),
               "vkQueueSubmit", error)) {
        return false;
    }
    return Check(m_Functions.vkQueueWaitIdle(m_Queue), "vkQueueWaitIdle", error);
}

void VulkanPreviewRenderer::TransitionImage(VkImage image, VkImageLayout oldLayout,
                                            VkImageLayout newLayout, VkAccessFlags srcAccess,
                                            VkAccessFlags dstAccess,
                                            VkPipelineStageFlags srcStage,
                                            VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    m_Functions.vkCmdPipelineBarrier(m_CommandBuffer, srcStage, dstStage, 0, 0, nullptr, 0,
                                     nullptr, 1, &barrier);
}

bool VulkanPreviewRenderer::UploadImage(const ImageData& image, GpuTexture* target,
                                        std::string* error) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
        if (error) *error = "Invalid image data.";
        return false;
    }
    const VkDeviceSize imageSize =
        static_cast<VkDeviceSize>(image.width) * image.height * 4;
    GpuBuffer staging;
    if (!CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &staging, error)) {
        return false;
    }
    void* mapped = nullptr;
    if (!Check(m_Functions.vkMapMemory(m_Device, staging.memory, 0, imageSize, 0, &mapped),
               "vkMapMemory", error)) {
        m_Functions.vkDestroyBuffer(m_Device, staging.buffer, nullptr);
        m_Functions.vkFreeMemory(m_Device, staging.memory, nullptr);
        return false;
    }
    std::memcpy(mapped, image.rgba.data(), static_cast<std::size_t>(imageSize));
    m_Functions.vkUnmapMemory(m_Device, staging.memory);

    GpuTexture texture;
    if (!CreateImage(static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height),
                     VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     &texture, error)) {
        m_Functions.vkDestroyBuffer(m_Device, staging.buffer, nullptr);
        m_Functions.vkFreeMemory(m_Device, staging.memory, nullptr);
        return false;
    }

    if (!BeginSingleTimeCommands(error)) {
        m_Functions.vkDestroyBuffer(m_Device, staging.buffer, nullptr);
        m_Functions.vkFreeMemory(m_Device, staging.memory, nullptr);
        DestroyTexture(texture);
        return false;
    }
    TransitionImage(texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {static_cast<uint32_t>(image.width),
                          static_cast<uint32_t>(image.height), 1};
    m_Functions.vkCmdCopyBufferToImage(m_CommandBuffer, staging.buffer, texture.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TransitionImage(texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    const bool submitted = EndSingleTimeCommands(error);
    m_Functions.vkDestroyBuffer(m_Device, staging.buffer, nullptr);
    m_Functions.vkFreeMemory(m_Device, staging.memory, nullptr);
    if (!submitted) {
        DestroyTexture(texture);
        return false;
    }
    if (target) {
        DestroyTexture(*target);
        *target = texture;
    }
    return true;
}

void VulkanPreviewRenderer::DestroyTexture(GpuTexture& texture) {
    if (texture.view != VK_NULL_HANDLE) {
        m_Functions.vkDestroyImageView(m_Device, texture.view, nullptr);
        texture.view = VK_NULL_HANDLE;
    }
    if (texture.image != VK_NULL_HANDLE) {
        m_Functions.vkDestroyImage(m_Device, texture.image, nullptr);
        texture.image = VK_NULL_HANDLE;
    }
    if (texture.memory != VK_NULL_HANDLE) {
        m_Functions.vkFreeMemory(m_Device, texture.memory, nullptr);
        texture.memory = VK_NULL_HANDLE;
    }
    texture.width = 0;
    texture.height = 0;
}

void VulkanPreviewRenderer::UpdateDescriptorSet() {
    if (m_DescriptorSet == VK_NULL_HANDLE) return;
    std::vector<VkDescriptorImageInfo> images(kMaxMaterialTextures + 1);
    for (int i = 0; i <= kMaxMaterialTextures; ++i) {
        const GpuTexture* texture = &m_DefaultTexture;
        if (i > 0 && static_cast<std::size_t>(i - 1) < m_Textures.size()) {
            texture = &m_Textures[static_cast<std::size_t>(i - 1)];
        }
        images[static_cast<std::size_t>(i)].sampler = VK_NULL_HANDLE;
        images[static_cast<std::size_t>(i)].imageView =
            texture ? texture->view : VK_NULL_HANDLE;
        images[static_cast<std::size_t>(i)].imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = m_UniformBuffer.buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = kUniformBufferSize;
    VkDescriptorImageInfo storageInfo = {};
    storageInfo.imageView = m_OutputView;
    storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo samplerInfo = {};
    samplerInfo.sampler = m_Sampler;

    VkWriteDescriptorSet writes[16] = {};
    uint32_t writeCount = 0;
    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = m_DescriptorSet;
    writes[writeCount].dstBinding = 0;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[writeCount].pBufferInfo = &bufferInfo;
    ++writeCount;
    for (int i = 0; i <= kMaxMaterialTextures; ++i) {
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = m_DescriptorSet;
        writes[writeCount].dstBinding = static_cast<uint32_t>(1 + i);
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[writeCount].pImageInfo = &images[static_cast<std::size_t>(i)];
        ++writeCount;
    }
    if (m_OutputView != VK_NULL_HANDLE) {
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = m_DescriptorSet;
        writes[writeCount].dstBinding = 15;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[writeCount].pImageInfo = &storageInfo;
        ++writeCount;
    }
    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = m_DescriptorSet;
    writes[writeCount].dstBinding = 17;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[writeCount].pImageInfo = &samplerInfo;
    ++writeCount;
    m_Functions.vkUpdateDescriptorSets(m_Device, writeCount, writes, 0, nullptr);
}

bool VulkanPreviewRenderer::LoadTextures(const HlslPreviewShader& shader, std::string* error) {
    m_TextureDiagnostics.clear();
    for (GpuTexture& texture : m_Textures) {
        DestroyTexture(texture);
    }
    m_Textures.clear();
    DestroyTexture(m_DefaultTexture);
    if (!UploadCheckerboard(&m_DefaultTexture)) {
        if (error) *error = "Could not create the Vulkan fallback texture.";
        return false;
    }
    for (const ShaderTextureBinding& binding : shader.textures) {
        GpuTexture texture;
        std::string loadError;
        if (!UploadTexture(binding.assetPath, &texture, &loadError)) {
            if (!m_TextureDiagnostics.empty()) m_TextureDiagnostics += "\n";
            m_TextureDiagnostics += "Texture fallback for " + binding.uniformName + ": " +
                                    loadError;
        } else {
            m_Textures.push_back(std::move(texture));
        }
    }
    UpdateDescriptorSet();
    return true;
}

bool VulkanPreviewRenderer::UploadTexture(const std::string& path, GpuTexture* target,
                                          std::string* error) {
    ImageData image;
    std::string loadError;
    if (!LoadImageRGBA(path, image, &loadError)) {
        if (error) *error = loadError;
        return false;
    }
    return UploadImage(image, target, error);
}

bool VulkanPreviewRenderer::UploadCheckerboard(GpuTexture* target) {
    ImageData image;
    image.width = 64;
    image.height = 64;
    image.rgba.resize(64u * 64u * 4u);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const bool dark = ((x / 8) + (y / 8)) % 2 == 0;
            const std::size_t offset = (static_cast<std::size_t>(y) * 64 + x) * 4;
            const uint8_t value = dark ? 42 : 118;
            image.rgba[offset + 0] = value;
            image.rgba[offset + 1] = static_cast<uint8_t>(value + 8);
            image.rgba[offset + 2] = static_cast<uint8_t>(value + 14);
            image.rgba[offset + 3] = 255;
        }
    }
    return UploadImage(image, target, nullptr);
}

bool VulkanPreviewRenderer::CompileShaderToSpirv(const std::string& source,
                                                 std::vector<uint32_t>* spirv,
                                                 std::string* error) {
    const std::string glslangPath = FindGlslangExecutable();
    if (glslangPath.empty()) {
        if (error) *error = "The Vulkan shader compiler (glslang.exe) was not found.";
        return false;
    }
    std::error_code ec;
    fs::path tempDirectory = fs::temp_directory_path(ec);
    if (ec) tempDirectory = PathUtil::UserConfigDirectory();
    const std::string base = "mf_vulkan_" + std::to_string(GetCurrentProcessId()) + "_" +
                             std::to_string(reinterpret_cast<uintptr_t>(this));
    const fs::path hlslPath = tempDirectory / (base + ".hlsl");
    const fs::path spvPath = tempDirectory / (base + ".spv");
    const fs::path logPath = tempDirectory / (base + ".log");
    {
        std::ofstream stream(hlslPath, std::ios::binary | std::ios::trunc);
        if (!stream) {
            if (error) *error = "Could not write the temporary HLSL shader.";
            return false;
        }
        stream.write(source.data(), static_cast<std::streamsize>(source.size()));
    }
    fs::remove(spvPath, ec);
    fs::remove(logPath, ec);

    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE logHandle = CreateFileW(logPath.wstring().c_str(), GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = logHandle;
    startup.hStdError = logHandle;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process = {};

    std::wstring commandLine = L"\"" + Utf8ToWide(glslangPath) + L"\"";
    commandLine += L" -D -V --target-env vulkan1.1 -S comp -e main -o \"";
    commandLine += spvPath.wstring();
    commandLine += L"\" \"";
    commandLine += hlslPath.wstring();
    commandLine += L"\"";
    std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back(L'\0');
    const BOOL started = CreateProcessW(
        nullptr, commandBuffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        nullptr, &startup, &process);
    std::string logText;
    DWORD exitCode = 1;
    if (started) {
        const DWORD waitResult = WaitForSingleObject(process.hProcess, 30000);
        if (waitResult == WAIT_TIMEOUT) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 2000);
        }
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    if (logHandle) CloseHandle(logHandle);
    {
        std::ifstream stream(logPath, std::ios::binary);
        if (stream) {
            logText.assign((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
        }
    }
    const bool success = started && exitCode == 0 && fs::is_regular_file(spvPath, ec);
    if (success) {
        std::ifstream stream(spvPath, std::ios::binary | std::ios::ate);
        const std::streamsize size = stream ? stream.tellg() : 0;
        if (size > 0 && size % 4 == 0) {
            spirv->resize(static_cast<std::size_t>(size) / sizeof(uint32_t));
            stream.seekg(0, std::ios::beg);
            stream.read(reinterpret_cast<char*>(spirv->data()), size);
        } else {
            spirv->clear();
        }
    }
    fs::remove(hlslPath, ec);
    fs::remove(spvPath, ec);
    fs::remove(logPath, ec);
    if (!success || spirv->empty()) {
        if (error) {
            *error = "Vulkan shader compilation failed.";
            if (!logText.empty()) {
                *error += "\n";
                *error += logText;
            }
        }
        return false;
    }
    if (error) error->clear();
    return true;
}

bool VulkanPreviewRenderer::CreatePipelineFromSpirv(const std::vector<uint32_t>& spirv,
                                                     std::string* error) {
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo moduleInfo = {};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirv.size() * sizeof(uint32_t);
    moduleInfo.pCode = spirv.data();
    if (!Check(m_Functions.vkCreateShaderModule(m_Device, &moduleInfo, nullptr, &shaderModule),
               "vkCreateShaderModule", error)) {
        return false;
    }
    VkPipelineShaderStageCreateInfo stageInfo = {};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = m_PipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = m_Functions.vkCreateComputePipelines(
        m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    m_Functions.vkDestroyShaderModule(m_Device, shaderModule, nullptr);
    if (!Check(result, "vkCreateComputePipelines", error)) {
        return false;
    }
    if (m_Pipeline != VK_NULL_HANDLE) {
        m_Functions.vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
    }
    m_Pipeline = pipeline;
    return true;
}

bool VulkanPreviewRenderer::CreateUniformBuffer(std::string* error) {
    if (!CreateBuffer(kUniformBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &m_UniformBuffer, error)) {
        return false;
    }
    return Check(m_Functions.vkMapMemory(m_Device, m_UniformBuffer.memory, 0,
                                         kUniformBufferSize, 0, &m_UniformBuffer.mapped),
                 "vkMapMemory", error);
}

bool VulkanPreviewRenderer::Initialize(std::string* error) {
    if (m_Device != VK_NULL_HANDLE) return true;
    if (FindGlslangExecutable().empty()) {
        if (error) {
            *error = "The Vulkan shader compiler (tools/glslang/glslang.exe) was not found.";
        }
        return false;
    }
    if (!LoadVulkanLibrary(error)) return false;
    if (!CreateInstance(error)) return false;
    if (!PickPhysicalDevice(error)) return false;
    if (!CreateDevice(error)) return false;
    if (!CreateCommands(error)) return false;
    if (!CreateDescriptorSetLayout(error)) return false;
    if (!CreatePipelineLayout(error)) return false;
    if (!CreateSampler(error)) return false;
    if (!CreateUniformBuffer(error)) return false;
    if (!UploadCheckerboard(&m_DefaultTexture)) {
        if (error) *error = "Could not create the Vulkan fallback texture.";
        return false;
    }
    UpdateDescriptorSet();
    m_LastError.clear();
    return true;
}

bool VulkanPreviewRenderer::SetMaterial(const GeneratedShader& material, std::string* error) {
    if (m_Device == VK_NULL_HANDLE) {
        if (error) *error = "The Vulkan device is not initialized.";
        return false;
    }
    const HlslPreviewShader shader = HlslPreviewShaderGenerator::Generate(material);
    if (!shader.success) {
        m_MaterialValid = false;
        m_ShaderLog = shader.diagnostic;
        if (error) *error = m_ShaderLog;
        return false;
    }
    m_Shader = shader;
    m_Parameters = shader.parameters;
    if (shader.source != m_LastShaderSource || m_Pipeline == VK_NULL_HANDLE) {
        std::vector<uint32_t> spirv;
        std::string compileError;
        if (!CompileShaderToSpirv(shader.source, &spirv, &compileError)) {
            m_MaterialValid = false;
            m_ShaderLog = compileError;
            if (error) *error = compileError;
            return false;
        }
        if (!CreatePipelineFromSpirv(spirv, &compileError)) {
            m_MaterialValid = false;
            m_ShaderLog = compileError;
            if (error) *error = compileError;
            return false;
        }
        m_LastShaderSource = shader.source;
        m_ShaderLog = "Vulkan compute shader compiled successfully.";
    }
    std::string textureError;
    if (!LoadTextures(shader, &textureError)) {
        m_MaterialValid = false;
        m_ShaderLog = textureError;
        if (error) *error = textureError;
        return false;
    }
    m_MaterialValid = true;
    m_LastError.clear();
    if (error) error->clear();
    return true;
}

void VulkanPreviewRenderer::DestroySizeDependentResources() {
    if (m_OutputView != VK_NULL_HANDLE) {
        m_Functions.vkDestroyImageView(m_Device, m_OutputView, nullptr);
        m_OutputView = VK_NULL_HANDLE;
    }
    if (m_OutputImage != VK_NULL_HANDLE) {
        m_Functions.vkDestroyImage(m_Device, m_OutputImage, nullptr);
        m_OutputImage = VK_NULL_HANDLE;
    }
    if (m_OutputMemory != VK_NULL_HANDLE) {
        m_Functions.vkFreeMemory(m_Device, m_OutputMemory, nullptr);
        m_OutputMemory = VK_NULL_HANDLE;
    }
    if (m_ReadbackBuffer.buffer != VK_NULL_HANDLE) {
        m_Functions.vkDestroyBuffer(m_Device, m_ReadbackBuffer.buffer, nullptr);
        m_ReadbackBuffer.buffer = VK_NULL_HANDLE;
    }
    if (m_ReadbackBuffer.memory != VK_NULL_HANDLE) {
        m_Functions.vkFreeMemory(m_Device, m_ReadbackBuffer.memory, nullptr);
        m_ReadbackBuffer.memory = VK_NULL_HANDLE;
    }
    if (m_ColorTexture != 0) {
        glDeleteTextures(1, &m_ColorTexture);
        m_ColorTexture = 0;
    }
    m_ReadbackPixels.clear();
    m_Width = 0;
    m_Height = 0;
}

bool VulkanPreviewRenderer::CreateSizeDependentResources(int width, int height,
                                                          std::string* error) {
    DestroySizeDependentResources();
    m_Width = std::max(2, width);
    m_Height = std::max(2, height);
    GpuTexture output;
    if (!CreateImage(static_cast<uint32_t>(m_Width), static_cast<uint32_t>(m_Height),
                     VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &output,
                     error)) {
        return false;
    }
    m_OutputImage = output.image;
    m_OutputMemory = output.memory;
    m_OutputView = output.view;
    if (!BeginSingleTimeCommands(error)) {
        return false;
    }
    TransitionImage(m_OutputImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (!EndSingleTimeCommands(error)) {
        return false;
    }

    if (!CreateBuffer(static_cast<VkDeviceSize>(m_Width) * m_Height * 4,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &m_ReadbackBuffer, error)) {
        return false;
    }
    m_ReadbackPixels.resize(static_cast<std::size_t>(m_Width) * m_Height * 4);

    glGenTextures(1, &m_ColorTexture);
    glBindTexture(GL_TEXTURE_2D, m_ColorTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_Width, m_Height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    UpdateDescriptorSet();
    return true;
}

bool VulkanPreviewRenderer::UpdateConstants(const MaterialGraph& graph, float timeSeconds) {
    if (!m_UniformBuffer.mapped) return false;
    PreviewConstants constants;
    const float aspect = static_cast<float>(m_Width) / static_cast<float>(std::max(m_Height, 1));
    const Vec3 target(0.0f, 0.0f, 0.0f);
    const Vec3 eye(
        m_Camera.distance * std::cos(m_Camera.pitch) * std::sin(m_Camera.yaw),
        m_Camera.distance * std::sin(m_Camera.pitch),
        m_Camera.distance * std::cos(m_Camera.pitch) * std::cos(m_Camera.yaw));
    const Vec3 forward = Normalize(target - eye);
    const Vec3 right = Normalize(Cross(forward, Vec3(0.0f, 1.0f, 0.0f)));
    const Vec3 up = Cross(right, forward);
    const float tanHalfFov = std::tan(Radians(40.0f) * 0.5f);
    constants.cameraPos[0] = eye.x;
    constants.cameraPos[1] = eye.y;
    constants.cameraPos[2] = eye.z;
    constants.cameraPos[3] = 1.0f;
    constants.cameraRight[0] = right.x;
    constants.cameraRight[1] = right.y;
    constants.cameraRight[2] = right.z;
    constants.cameraUp[0] = up.x;
    constants.cameraUp[1] = up.y;
    constants.cameraUp[2] = up.z;
    constants.cameraForward[0] = forward.x;
    constants.cameraForward[1] = forward.y;
    constants.cameraForward[2] = forward.z;
    constants.resolution[0] = static_cast<float>(m_Width);
    constants.resolution[1] = static_cast<float>(m_Height);
    constants.resolution[2] = tanHalfFov;
    constants.resolution[3] = aspect;
    constants.time = timeSeconds;
    constants.meshType = m_Mesh == PreviewMesh::Plane ? 1.0f : 0.0f;
    constants.twoSided = graph.Settings().twoSided ? 1.0f : 0.0f;
    for (std::size_t i = 0;
         i < m_Parameters.size() && i < static_cast<std::size_t>(64); ++i) {
        const ShaderParameterBinding& binding = m_Parameters[i];
        PropertyValue value = binding.defaultValue;
        const Node* node = graph.FindNode(binding.nodeId);
        if (node) {
            const auto valueIt = node->properties.find("value");
            if (valueIt != node->properties.end()) {
                value = valueIt->second;
            }
        }
        if (binding.type == PinType::Float) {
            constants.parameters[i][0] = value.number;
        } else if (binding.type == PinType::Vector4) {
            constants.parameters[i][0] = value.vec4.x;
            constants.parameters[i][1] = value.vec4.y;
            constants.parameters[i][2] = value.vec4.z;
            constants.parameters[i][3] = value.vec4.w;
        }
    }
    std::memcpy(m_UniformBuffer.mapped, &constants, sizeof(constants));
    return true;
}

void VulkanPreviewRenderer::UploadColorToOpenGL(int width, int height) {
    if (m_ColorTexture == 0 || m_ReadbackPixels.empty()) return;
    glBindTexture(GL_TEXTURE_2D, m_ColorTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                    m_ReadbackPixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void VulkanPreviewRenderer::Render(const MaterialGraph& graph, float timeSeconds, int width,
                                   int height) {
    if (m_Device == VK_NULL_HANDLE || m_Pipeline == VK_NULL_HANDLE || !m_MaterialValid ||
        width < 2 || height < 2) {
        return;
    }
    if (width != m_Width || height != m_Height || m_OutputImage == VK_NULL_HANDLE) {
        std::string error;
        if (!CreateSizeDependentResources(width, height, &error)) {
            m_LastError = error;
            return;
        }
    }
    if (!UpdateConstants(graph, timeSeconds)) {
        return;
    }
    m_Functions.vkResetCommandBuffer(m_CommandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!Check(m_Functions.vkBeginCommandBuffer(m_CommandBuffer, &beginInfo),
               "vkBeginCommandBuffer", nullptr)) {
        return;
    }
    m_Functions.vkCmdBindPipeline(m_CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
    m_Functions.vkCmdBindDescriptorSets(m_CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        m_PipelineLayout, 0, 1, &m_DescriptorSet, 0,
                                        nullptr);
    const uint32_t groupsX = static_cast<uint32_t>((m_Width + 7) / 8);
    const uint32_t groupsY = static_cast<uint32_t>((m_Height + 7) / 8);
    m_Functions.vkCmdDispatch(m_CommandBuffer, groupsX, groupsY, 1);
    TransitionImage(m_OutputImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {static_cast<uint32_t>(m_Width), static_cast<uint32_t>(m_Height), 1};
    m_Functions.vkCmdCopyImageToBuffer(m_CommandBuffer, m_OutputImage,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       m_ReadbackBuffer.buffer, 1, &region);
    TransitionImage(m_OutputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (!Check(m_Functions.vkEndCommandBuffer(m_CommandBuffer), "vkEndCommandBuffer", nullptr)) {
        return;
    }
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_CommandBuffer;
    if (!Check(m_Functions.vkQueueSubmit(m_Queue, 1, &submitInfo, VK_NULL_HANDLE),
               "vkQueueSubmit", nullptr)) {
        return;
    }
    if (!Check(m_Functions.vkQueueWaitIdle(m_Queue), "vkQueueWaitIdle", nullptr)) {
        return;
    }
    void* mapped = nullptr;
    if (Check(m_Functions.vkMapMemory(m_Device, m_ReadbackBuffer.memory, 0,
                                      m_ReadbackBuffer.size, 0, &mapped),
              "vkMapMemory", nullptr) &&
        mapped) {
        std::memcpy(m_ReadbackPixels.data(), mapped,
                    static_cast<std::size_t>(m_Width) * m_Height * 4);
        m_Functions.vkUnmapMemory(m_Device, m_ReadbackBuffer.memory);
        UploadColorToOpenGL(m_Width, m_Height);
    }
}

void VulkanPreviewRenderer::Orbit(float deltaX, float deltaY) {
    m_Camera.yaw -= deltaX * 0.008f;
    m_Camera.pitch = Clamp(m_Camera.pitch + deltaY * 0.008f, -1.35f, 1.35f);
}

void VulkanPreviewRenderer::Zoom(float wheelDelta) {
    m_Camera.distance = Clamp(m_Camera.distance - wheelDelta * 0.25f, 1.8f, 7.5f);
}

bool VulkanPreviewRenderer::ReadPixelTopLeft(int x, int y, float rgba[4]) const {
    if (!rgba || m_Width <= 0 || m_Height <= 0 ||
        m_ReadbackPixels.size() < static_cast<std::size_t>(m_Width) * m_Height * 4) {
        return false;
    }
    if (x < 0 || x >= m_Width || y < 0 || y >= m_Height) return false;
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(m_Width) +
         static_cast<std::size_t>(x)) *
        4;
    for (int i = 0; i < 4; ++i) {
        rgba[i] = static_cast<float>(m_ReadbackPixels[offset + i]) / 255.0f;
    }
    return true;
}

std::string VulkanPreviewRenderer::BackendDetails() const {
    return "Vulkan " + m_ApiVersion + " / " + m_DeviceName + " / compute preview";
}

void VulkanPreviewRenderer::Shutdown() {
    if (m_Device != VK_NULL_HANDLE) {
        m_Functions.vkDeviceWaitIdle(m_Device);
        DestroySizeDependentResources();
        for (GpuTexture& texture : m_Textures) DestroyTexture(texture);
        m_Textures.clear();
        DestroyTexture(m_DefaultTexture);
        if (m_Pipeline != VK_NULL_HANDLE) {
            m_Functions.vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
            m_Pipeline = VK_NULL_HANDLE;
        }
        if (m_Sampler != VK_NULL_HANDLE) {
            m_Functions.vkDestroySampler(m_Device, m_Sampler, nullptr);
            m_Sampler = VK_NULL_HANDLE;
        }
        if (m_DescriptorPool != VK_NULL_HANDLE) {
            m_Functions.vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
            m_DescriptorPool = VK_NULL_HANDLE;
        }
        if (m_DescriptorSetLayout != VK_NULL_HANDLE) {
            m_Functions.vkDestroyDescriptorSetLayout(m_Device, m_DescriptorSetLayout, nullptr);
            m_DescriptorSetLayout = VK_NULL_HANDLE;
        }
        if (m_PipelineLayout != VK_NULL_HANDLE) {
            m_Functions.vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
            m_PipelineLayout = VK_NULL_HANDLE;
        }
        if (m_UniformBuffer.buffer != VK_NULL_HANDLE) {
            m_Functions.vkDestroyBuffer(m_Device, m_UniformBuffer.buffer, nullptr);
            m_UniformBuffer.buffer = VK_NULL_HANDLE;
        }
        if (m_UniformBuffer.memory != VK_NULL_HANDLE) {
            m_Functions.vkFreeMemory(m_Device, m_UniformBuffer.memory, nullptr);
            m_UniformBuffer.memory = VK_NULL_HANDLE;
        }
        if (m_CommandPool != VK_NULL_HANDLE) {
            m_Functions.vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
            m_CommandPool = VK_NULL_HANDLE;
        }
        m_Functions.vkDestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;
    }
    if (m_Instance != VK_NULL_HANDLE) {
        m_Functions.vkDestroyInstance(m_Instance, nullptr);
        m_Instance = VK_NULL_HANDLE;
    }
    if (m_Library) {
        FreeLibrary(static_cast<HMODULE>(m_Library));
        m_Library = nullptr;
    }
    m_MaterialValid = false;
    m_ShaderLog.clear();
}

} // namespace mf

#endif

