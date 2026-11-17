#pragma once

#include "codegen/GlslGenerator.h"
#include "core/Graph.h"
#include "render/HlslPreviewShader.h"
#include "render/PreviewBackend.h"

#include <string>
#include <vector>
#include <cstdint>

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

namespace mf {

struct ImageData;

#if defined(_WIN32)
class D3D12PreviewRenderer final : public PreviewBackend {
public:
    D3D12PreviewRenderer() = default;
    ~D3D12PreviewRenderer() override;

    D3D12PreviewRenderer(const D3D12PreviewRenderer&) = delete;
    D3D12PreviewRenderer& operator=(const D3D12PreviewRenderer&) = delete;

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
    bool HasFramebuffer() const override { return m_OutputTexture != nullptr; }
    const std::string& ShaderLog() const override { return m_ShaderLog; }
    const std::string& TextureDiagnostics() const override { return m_TextureDiagnostics; }
    const char* MeshName() const override { return PreviewMeshName(m_Mesh); }

    const char* BackendName() const override { return "DirectX 12"; }
    std::string BackendDetails() const override;
    const std::string& LastError() const override { return m_LastError; }

    static bool Probe(std::string* details, std::string* error);

private:
    struct GpuTexture {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        int width = 0;
        int height = 0;
        int srvIndex = -1;
    };

    bool CreateDevice(std::string* error);
    bool CreatePipeline(std::string* error);
    bool CompileShader(const std::string& source, std::string* error);
    bool CreateRootSignature(std::string* error);
    void CreateDescriptorHeap();
    bool CreateSizeDependentResources(int width, int height, std::string* error);
    void DestroySizeDependentResources();
    bool LoadTextures(const HlslPreviewShader& shader, std::string* error);
    bool UploadTexture(const std::string& path, int srvIndex, GpuTexture* target,
                       std::string* error);
    bool UploadCheckerboard(int srvIndex);
    bool UploadImage(const ImageData& image, int srvIndex, GpuTexture* target,
                     std::string* error = nullptr);
    void WriteSrv(int srvIndex, ID3D12Resource* resource);
    void WriteUav(int uavIndex, ID3D12Resource* resource);
    bool ExecuteCommandList();
    void WaitForFence();
    bool UpdateConstants(const MaterialGraph& graph, float timeSeconds);
    void UploadColorToOpenGL(int width, int height);

    Microsoft::WRL::ComPtr<ID3D12Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_CommandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_CommandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CommandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
    HANDLE m_FenceEvent = nullptr;
    UINT64 m_FenceValue = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_Pipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_DescriptorHeap;
    D3D12_GPU_DESCRIPTOR_HANDLE m_SrvTableGpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_UavTableGpu = {};
    UINT m_DescriptorSize = 0;
    static constexpr UINT kSrvCount = 16;
    static constexpr UINT kMaxParameters = 64;
    static constexpr UINT kConstantBufferSize = 1280;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_OutputTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ReadbackBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ConstantBuffer;
    uint8_t* m_ConstantMapped = nullptr;

    int m_Width = 0;
    int m_Height = 0;
    unsigned int m_ColorTexture = 0;

    std::vector<GpuTexture> m_Textures;
    GpuTexture m_DefaultTexture;
    std::vector<ShaderParameterBinding> m_Parameters;
    HlslPreviewShader m_Shader;
    std::string m_LastShaderSource;
    std::string m_ShaderLog;
    std::string m_TextureDiagnostics;
    std::string m_LastError;
    std::string m_AdapterName;
    bool m_MaterialValid = false;

    PreviewMesh m_Mesh = PreviewMesh::Sphere;
    PreviewCamera m_Camera;
    std::vector<uint8_t> m_ReadbackPixels;
    bool m_UploadPending = true;
};
#endif

} // namespace mf
