#pragma once

#include "codegen/GlslGenerator.h"
#include "core/Graph.h"
#include "render/HlslPreviewShader.h"
#include "render/PreviewBackend.h"

#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

namespace mf {

struct ImageData;

#if defined(_WIN32)
class VulkanPreviewRenderer final : public PreviewBackend {
public:
    VulkanPreviewRenderer() = default;
    ~VulkanPreviewRenderer() override;

    VulkanPreviewRenderer(const VulkanPreviewRenderer&) = delete;
    VulkanPreviewRenderer& operator=(const VulkanPreviewRenderer&) = delete;

    bool Initialize(std::string* error = nullptr) override;
    void Shutdown() override;

    bool SetMaterial(const GeneratedShader& material, std::string* error = nullptr) override;
    void Render(const MaterialGraph& graph, float timeSeconds, int width, int height) override;

    void SetMesh(PreviewMesh mesh) override { m_Mesh = mesh; }
    PreviewMesh GetMesh() const override { return m_Mesh; }
    void Orbit(float deltaX, float deltaY) override;
    void Zoom(float wheelDelta) override;
    PreviewCamera& Camera() override { return m_Camera; }
    const PreviewCamera& Camera() const override { return m_Camera; }

    unsigned int ColorTexture() const override { return m_ColorTexture; }
    int RenderWidth() const override { return m_Width; }
    int RenderHeight() const override { return m_Height; }
    bool ReadPixelTopLeft(int x, int y, float rgba[4]) const override;
    bool HasMaterial() const override { return m_MaterialValid; }
    bool HasFramebuffer() const override { return m_OutputImage != VK_NULL_HANDLE; }
    const std::string& ShaderLog() const override { return m_ShaderLog; }
    const std::string& TextureDiagnostics() const override { return m_TextureDiagnostics; }
    const char* MeshName() const override { return PreviewMeshName(m_Mesh); }

    const char* BackendName() const override { return "Vulkan"; }
    std::string BackendDetails() const override;
    const std::string& LastError() const override { return m_LastError; }

    static bool Probe(std::string* details, std::string* error);

private:
    struct Functions {
        PFN_vkCreateInstance vkCreateInstance = nullptr;
        PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;
        PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
        PFN_vkDestroyInstance vkDestroyInstance = nullptr;
        PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
        PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
        PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = nullptr;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
        PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
        PFN_vkCreateDevice vkCreateDevice = nullptr;
        PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
        PFN_vkDestroyDevice vkDestroyDevice = nullptr;
        PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
        PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
        PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
        PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
        PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
        PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
        PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
        PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
        PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
        PFN_vkQueueSubmit vkQueueSubmit = nullptr;
        PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
        PFN_vkCreateBuffer vkCreateBuffer = nullptr;
        PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
        PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
        PFN_vkAllocateMemory vkAllocateMemory = nullptr;
        PFN_vkFreeMemory vkFreeMemory = nullptr;
        PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
        PFN_vkCreateImage vkCreateImage = nullptr;
        PFN_vkDestroyImage vkDestroyImage = nullptr;
        PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
        PFN_vkBindImageMemory vkBindImageMemory = nullptr;
        PFN_vkCreateImageView vkCreateImageView = nullptr;
        PFN_vkDestroyImageView vkDestroyImageView = nullptr;
        PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
        PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
        PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
        PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
        PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
        PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
        PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
        PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
        PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
        PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
        PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
        PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
        PFN_vkCreateSampler vkCreateSampler = nullptr;
        PFN_vkDestroySampler vkDestroySampler = nullptr;
        PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
        PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
        PFN_vkCmdDispatch vkCmdDispatch = nullptr;
        PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
        PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
        PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = nullptr;
        PFN_vkMapMemory vkMapMemory = nullptr;
        PFN_vkUnmapMemory vkUnmapMemory = nullptr;
    };

    struct GpuBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };

    struct GpuTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
    };

    bool LoadVulkanLibrary(std::string* error);
    bool CreateInstance(std::string* error);
    bool PickPhysicalDevice(std::string* error);
    bool CreateDevice(std::string* error);
    bool CreateDescriptorSetLayout(std::string* error);
    bool CreatePipelineLayout(std::string* error);
    bool CreateCommands(std::string* error);
    bool CreateSampler(std::string* error);
    bool CreateUniformBuffer(std::string* error);
    bool CreateSizeDependentResources(int width, int height, std::string* error);
    void DestroySizeDependentResources();
    bool CreatePipelineFromSpirv(const std::vector<uint32_t>& spirv, std::string* error);
    bool CompileShaderToSpirv(const std::string& source, std::vector<uint32_t>* spirv,
                              std::string* error);
    bool LoadTextures(const HlslPreviewShader& shader, std::string* error);
    bool UploadTexture(const std::string& path, GpuTexture* target, std::string* error);
    bool UploadImage(const ImageData& image, GpuTexture* target, std::string* error);
    bool UploadCheckerboard(GpuTexture* target);
    bool CreateImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageUsageFlags usage, GpuTexture* target, std::string* error);
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties, GpuBuffer* target,
                      std::string* error);
    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    bool BeginSingleTimeCommands(std::string* error);
    bool EndSingleTimeCommands(std::string* error);
    void TransitionImage(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
    void UpdateDescriptorSet();
    bool UpdateConstants(const MaterialGraph& graph, float timeSeconds);
    void UploadColorToOpenGL(int width, int height);
    void DestroyTexture(GpuTexture& texture);

    void* m_Library = nullptr;
    Functions m_Functions;

    VkInstance m_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    VkQueue m_Queue = VK_NULL_HANDLE;
    uint32_t m_QueueFamily = 0;
    VkCommandPool m_CommandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    VkSampler m_Sampler = VK_NULL_HANDLE;
    VkPipeline m_Pipeline = VK_NULL_HANDLE;

    GpuBuffer m_UniformBuffer;
    GpuTexture m_DefaultTexture;
    std::vector<GpuTexture> m_Textures;

    VkImage m_OutputImage = VK_NULL_HANDLE;
    VkDeviceMemory m_OutputMemory = VK_NULL_HANDLE;
    VkImageView m_OutputView = VK_NULL_HANDLE;
    GpuBuffer m_ReadbackBuffer;

    int m_Width = 0;
    int m_Height = 0;
    unsigned int m_ColorTexture = 0;
    std::vector<uint8_t> m_ReadbackPixels;

    std::vector<ShaderParameterBinding> m_Parameters;
    HlslPreviewShader m_Shader;
    std::string m_LastShaderSource;
    std::string m_ShaderLog;
    std::string m_TextureDiagnostics;
    std::string m_LastError;
    std::string m_DeviceName;
    std::string m_ApiVersion;
    bool m_MaterialValid = false;

    PreviewMesh m_Mesh = PreviewMesh::Sphere;
    PreviewCamera m_Camera;
};
#endif

} // namespace mf
