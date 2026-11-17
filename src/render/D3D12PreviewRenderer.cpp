#include "render/D3D12PreviewRenderer.h"

#if defined(_WIN32)

#include "io/TextureLoader.h"

#include <glad/gl.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

namespace mf {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT AlignTo(UINT value, UINT alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

std::string HResultText(HRESULT result) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08lx",
                  static_cast<unsigned long>(result));
    return buffer;
}

std::string WideToUtf8(const wchar_t* text) {
    if (!text || !text[0]) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    return result;
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferDescription(UINT64 size) {
    D3D12_RESOURCE_DESC description = {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Alignment = 0;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;
    return description;
}

D3D12_RESOURCE_DESC TextureDescription(UINT width, UINT height, DXGI_FORMAT format,
                                       D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC description = {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Alignment = 0;
    description.Width = std::max<UINT>(1, width);
    description.Height = std::max<UINT>(1, height);
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;
    return description;
}

void TransitionResource(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!resource || before == after) return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commandList->ResourceBarrier(1, &barrier);
}

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

static_assert(sizeof(PreviewConstants) <= 1280, "The preview constant buffer is too small.");

} // namespace

D3D12PreviewRenderer::~D3D12PreviewRenderer() {
    Shutdown();
}

bool D3D12PreviewRenderer::Probe(std::string* details, std::string* error) {
    if (details) details->clear();
    if (error) error->clear();
    ComPtr<IDXGIFactory1> factory;
    const HRESULT factoryResult = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factoryResult)) {
        if (error) *error = "CreateDXGIFactory1 failed: " + HResultText(factoryResult);
        return false;
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        DXGI_ADAPTER_DESC1 description = {};
        if (FAILED(adapter->GetDesc1(&description))) continue;
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            if (details) *details = WideToUtf8(description.Description);
            return true;
        }
    }
    if (error) *error = "No DirectX 12 capable hardware adapter was found.";
    return false;
}

bool D3D12PreviewRenderer::CreateDevice(std::string* error) {
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    // The debug layer is optional; ignore failure when the Graphics Tools
    // feature is not installed.
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ComPtr<IDXGIFactory2> factory;
    HRESULT result = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        if (error) *error = "CreateDXGIFactory2 failed: " + HResultText(result);
        return false;
    }

    ComPtr<IDXGIAdapter1> chosen;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        DXGI_ADAPTER_DESC1 description = {};
        if (FAILED(adapter->GetDesc1(&description))) continue;
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            chosen = adapter;
            m_AdapterName = WideToUtf8(description.Description);
            break;
        }
    }

    result = chosen ? D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&m_Device))
                    : D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&m_Device));
    if (FAILED(result) || !m_Device) {
        if (error) *error = "D3D12CreateDevice failed: " + HResultText(result);
        return false;
    }
    if (m_AdapterName.empty()) m_AdapterName = "DirectX 12 device";

    D3D12_COMMAND_QUEUE_DESC queueDescription = {};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    result = m_Device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_CommandQueue));
    if (FAILED(result)) {
        if (error) *error = "CreateCommandQueue failed: " + HResultText(result);
        return false;
    }

    result = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&m_CommandAllocator));
    if (FAILED(result)) {
        if (error) *error = "CreateCommandAllocator failed: " + HResultText(result);
        return false;
    }
    result = m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         m_CommandAllocator.Get(), nullptr,
                                         IID_PPV_ARGS(&m_CommandList));
    if (FAILED(result)) {
        if (error) *error = "CreateCommandList failed: " + HResultText(result);
        return false;
    }
    m_CommandList->Close();

    result = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence));
    if (FAILED(result)) {
        if (error) *error = "CreateFence failed: " + HResultText(result);
        return false;
    }
    m_FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_FenceEvent) {
        if (error) *error = "CreateEvent failed.";
        return false;
    }

    result = m_Device->CreateCommittedResource(
        &HeapProperties(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
        &BufferDescription(kConstantBufferSize), D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_ConstantBuffer));
    if (FAILED(result)) {
        if (error) *error = "Could not create the constant buffer: " + HResultText(result);
        return false;
    }
    result = m_ConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_ConstantMapped));
    if (FAILED(result)) {
        if (error) *error = "Could not map the constant buffer: " + HResultText(result);
        return false;
    }
    return true;
}

void D3D12PreviewRenderer::CreateDescriptorHeap() {
    if (!m_Device) return;
    D3D12_DESCRIPTOR_HEAP_DESC description = {};
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    description.NumDescriptors = 32;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_Device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&m_DescriptorHeap)))) {
        m_DescriptorHeap.Reset();
        return;
    }
    m_DescriptorSize =
        m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_GPU_DESCRIPTOR_HANDLE base = m_DescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    m_SrvTableGpu = base;
    m_UavTableGpu.ptr = base.ptr + static_cast<SIZE_T>(kSrvCount) * m_DescriptorSize;
}

bool D3D12PreviewRenderer::CreateRootSignature(std::string* error) {
    D3D12_ROOT_PARAMETER parameters[3] = {};

    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].Descriptor.RegisterSpace = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = kSrvCount;
    srvRange.BaseShaderRegister = 1;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 15;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &uavRange;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 17;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC description = {};
    description.NumParameters = 3;
    description.pParameters = parameters;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> signatureError;
    HRESULT result = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &signatureError);
    if (FAILED(result)) {
        if (error) {
            *error = "D3D12SerializeRootSignature failed: " + HResultText(result);
            if (signatureError) {
                *error += "\n";
                *error += static_cast<const char*>(signatureError->GetBufferPointer());
            }
        }
        return false;
    }
    result = m_Device->CreateRootSignature(0, signature->GetBufferPointer(),
                                           signature->GetBufferSize(),
                                           IID_PPV_ARGS(&m_RootSignature));
    if (FAILED(result)) {
        if (error) *error = "CreateRootSignature failed: " + HResultText(result);
        return false;
    }
    return true;
}

bool D3D12PreviewRenderer::CompileShader(const std::string& source, std::string* error) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> compileErrors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT result = D3DCompile(source.c_str(), source.size(), "MFPreviewCS", nullptr,
                                      nullptr, "main", "cs_5_1", flags, 0, &bytecode,
                                      &compileErrors);
    if (FAILED(result) || !bytecode) {
        m_Pipeline.Reset();
        m_ShaderLog = "DirectX 12 HLSL compilation failed (" + HResultText(result) + "):\n";
        if (compileErrors) {
            m_ShaderLog += static_cast<const char*>(compileErrors->GetBufferPointer());
        }
        if (error) *error = m_ShaderLog;
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC description = {};
    description.pRootSignature = m_RootSignature.Get();
    description.CS.pShaderBytecode = bytecode->GetBufferPointer();
    description.CS.BytecodeLength = bytecode->GetBufferSize();
    const HRESULT pipelineResult =
        m_Device->CreateComputePipelineState(&description, IID_PPV_ARGS(&m_Pipeline));
    if (FAILED(pipelineResult)) {
        m_Pipeline.Reset();
        m_ShaderLog = "CreateComputePipelineState failed: " + HResultText(pipelineResult);
        if (error) *error = m_ShaderLog;
        return false;
    }
    m_LastShaderSource = source;
    m_ShaderLog = "DirectX 12 compute shader compiled successfully.";
    return true;
}

bool D3D12PreviewRenderer::Initialize(std::string* error) {
    if (m_Device) return true;
    if (!CreateDevice(error)) return false;
    if (!CreateRootSignature(error)) return false;
    CreateDescriptorHeap();
    if (!m_DescriptorHeap) {
        if (error) *error = "Could not create the descriptor heap.";
        return false;
    }
    if (!UploadCheckerboard(0)) {
        if (error) *error = "Could not create the DirectX 12 fallback texture.";
        return false;
    }
    m_LastError.clear();
    return true;
}

void D3D12PreviewRenderer::WaitForFence() {
    if (!m_CommandQueue || !m_Fence || !m_FenceEvent) return;
    const UINT64 value = ++m_FenceValue;
    if (FAILED(m_CommandQueue->Signal(m_Fence.Get(), value))) return;
    if (m_Fence->GetCompletedValue() < value) {
        m_Fence->SetEventOnCompletion(value, m_FenceEvent);
        WaitForSingleObject(m_FenceEvent, INFINITE);
    }
}

bool D3D12PreviewRenderer::ExecuteCommandList() {
    if (!m_CommandList || !m_CommandQueue) return false;
    const HRESULT closeResult = m_CommandList->Close();
    if (FAILED(closeResult)) {
        m_LastError = "CommandList::Close failed: " + HResultText(closeResult);
        return false;
    }
    ID3D12CommandList* lists[] = {m_CommandList.Get()};
    m_CommandQueue->ExecuteCommandLists(1, lists);
    WaitForFence();
    return true;
}

void D3D12PreviewRenderer::WriteSrv(int srvIndex, ID3D12Resource* resource) {
    if (!m_Device || !m_DescriptorHeap || !resource) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC description = {};
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Texture2D.MostDetailedMip = 0;
    description.Texture2D.MipLevels = 1;
    description.Texture2D.PlaneSlice = 0;
    description.Texture2D.ResourceMinLODClamp = 0.0f;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(srvIndex) * m_DescriptorSize;
    m_Device->CreateShaderResourceView(resource, &description, handle);
}

void D3D12PreviewRenderer::WriteUav(int uavIndex, ID3D12Resource* resource) {
    if (!m_Device || !m_DescriptorHeap || !resource) return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC description = {};
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    description.Texture2D.MipSlice = 0;
    description.Texture2D.PlaneSlice = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kSrvCount + uavIndex) * m_DescriptorSize;
    m_Device->CreateUnorderedAccessView(resource, nullptr, &description, handle);
}

bool D3D12PreviewRenderer::UploadImage(const ImageData& image, int srvIndex,
                                       GpuTexture* target, std::string* error) {
    if (!m_Device || image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
        if (error) *error = "Invalid image data.";
        return false;
    }
    const UINT width = static_cast<UINT>(image.width);
    const UINT height = static_cast<UINT>(image.height);
    ComPtr<ID3D12Resource> texture;
    HRESULT result = m_Device->CreateCommittedResource(
        &HeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
        &TextureDescription(width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                            D3D12_RESOURCE_FLAG_NONE),
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
    if (FAILED(result)) {
        if (error) *error = "Could not create a texture: " + HResultText(result);
        return false;
    }

    const UINT rowPitch = AlignTo(width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 uploadSize = static_cast<UINT64>(rowPitch) * height;
    ComPtr<ID3D12Resource> upload;
    result = m_Device->CreateCommittedResource(
        &HeapProperties(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
        &BufferDescription(uploadSize), D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&upload));
    if (FAILED(result)) {
        if (error) *error = "Could not create a texture upload buffer: " + HResultText(result);
        return false;
    }
    void* mapped = nullptr;
    result = upload->Map(0, nullptr, &mapped);
    if (FAILED(result) || !mapped) {
        if (error) *error = "Could not map the texture upload buffer.";
        return false;
    }
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(static_cast<uint8_t*>(mapped) + static_cast<std::size_t>(y) * rowPitch,
                    image.rgba.data() + static_cast<std::size_t>(y) * width * 4,
                    static_cast<std::size_t>(width) * 4);
    }
    upload->Unmap(0, nullptr);

    m_CommandAllocator->Reset();
    m_CommandList->Reset(m_CommandAllocator.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = 0;
    source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    source.PlacedFootprint.Footprint.Width = width;
    source.PlacedFootprint.Footprint.Height = height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = rowPitch;
    m_CommandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    TransitionResource(m_CommandList.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    if (!ExecuteCommandList()) {
        if (error) *error = m_LastError;
        return false;
    }

    WriteSrv(srvIndex, texture.Get());
    if (target) {
        target->resource = texture;
        target->width = image.width;
        target->height = image.height;
        target->srvIndex = srvIndex;
    }
    return true;
}

bool D3D12PreviewRenderer::UploadTexture(const std::string& path, int srvIndex,
                                         GpuTexture* target, std::string* error) {
    ImageData image;
    std::string loadError;
    if (!LoadImageRGBA(path, image, &loadError)) {
        if (error) *error = loadError;
        return false;
    }
    return UploadImage(image, srvIndex, target, error);
}

bool D3D12PreviewRenderer::UploadCheckerboard(int srvIndex) {
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
    return UploadImage(image, srvIndex, &m_DefaultTexture);
}

bool D3D12PreviewRenderer::LoadTextures(const HlslPreviewShader& shader,
                                        std::string* error) {
    m_TextureDiagnostics.clear();
    m_Textures.clear();
    if (!UploadCheckerboard(0)) {
        if (error) *error = "Could not upload the fallback texture.";
        return false;
    }
    // Fill every SRV slot with the fallback first so partially bound materials
    // never sample an uninitialized descriptor.
    for (UINT i = 0; i < kSrvCount; ++i) {
        WriteSrv(static_cast<int>(i), m_DefaultTexture.resource.Get());
    }
    for (std::size_t i = 0; i < shader.textures.size(); ++i) {
        const ShaderTextureBinding& binding = shader.textures[i];
        const int srvIndex = 1 + static_cast<int>(i);
        GpuTexture texture;
        std::string loadError;
        if (!UploadTexture(binding.assetPath, srvIndex, &texture, &loadError)) {
            if (!m_TextureDiagnostics.empty()) m_TextureDiagnostics += "\n";
            m_TextureDiagnostics += "Texture fallback for " + binding.uniformName + ": " +
                                    loadError;
            WriteSrv(srvIndex, m_DefaultTexture.resource.Get());
        } else {
            m_Textures.push_back(std::move(texture));
        }
    }
    return true;
}

bool D3D12PreviewRenderer::SetMaterial(const GeneratedShader& material, std::string* error) {
    if (!m_Device) {
        if (error) *error = "The DirectX 12 device is not initialized.";
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
    if (shader.source != m_LastShaderSource || !m_Pipeline) {
        std::string compileError;
        if (!CompileShader(shader.source, &compileError)) {
            m_MaterialValid = false;
            if (error) *error = compileError;
            return false;
        }
    }
    std::string textureError;
    if (!LoadTextures(shader, &textureError)) {
        m_MaterialValid = false;
        m_ShaderLog = textureError;
        if (error) *error = textureError;
        return false;
    }
    m_MaterialValid = true;
    m_UploadPending = true;
    m_LastError.clear();
    if (error) error->clear();
    return true;
}

void D3D12PreviewRenderer::DestroySizeDependentResources() {
    m_OutputTexture.Reset();
    m_ReadbackBuffer.Reset();
    if (m_ColorTexture != 0) {
        glDeleteTextures(1, &m_ColorTexture);
        m_ColorTexture = 0;
    }
    m_ReadbackPixels.clear();
    m_Width = 0;
    m_Height = 0;
}

bool D3D12PreviewRenderer::CreateSizeDependentResources(int width, int height,
                                                        std::string* error) {
    DestroySizeDependentResources();
    m_Width = std::max(2, width);
    m_Height = std::max(2, height);

    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = {};
    formatSupport.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport,
                                               sizeof(formatSupport)))) {
        if (!(formatSupport.Support1 &
              D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) {
            if (error) {
                *error = "This GPU does not support a typed R8G8B8A8 UAV for the preview.";
            }
            return false;
        }
    }

    HRESULT result = m_Device->CreateCommittedResource(
        &HeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
        &TextureDescription(static_cast<UINT>(m_Width), static_cast<UINT>(m_Height),
                            DXGI_FORMAT_R8G8B8A8_UNORM,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_OutputTexture));
    if (FAILED(result)) {
        if (error) *error = "Could not create the preview output texture: " + HResultText(result);
        return false;
    }
    WriteUav(0, m_OutputTexture.Get());

    const UINT rowPitch = AlignTo(static_cast<UINT>(m_Width) * 4,
                                  D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 readbackSize = static_cast<UINT64>(rowPitch) * static_cast<UINT>(m_Height);
    result = m_Device->CreateCommittedResource(
        &HeapProperties(D3D12_HEAP_TYPE_READBACK), D3D12_HEAP_FLAG_NONE,
        &BufferDescription(readbackSize), D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_ReadbackBuffer));
    if (FAILED(result)) {
        if (error) *error = "Could not create the preview readback buffer: " + HResultText(result);
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
    m_UploadPending = true;
    return true;
}

void D3D12PreviewRenderer::UploadColorToOpenGL(int width, int height) {
    if (m_ColorTexture == 0 || m_ReadbackPixels.empty()) return;
    glBindTexture(GL_TEXTURE_2D, m_ColorTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                    m_ReadbackPixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

bool D3D12PreviewRenderer::UpdateConstants(const MaterialGraph& graph, float timeSeconds) {
    if (!m_ConstantMapped) return false;
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
    constants.cameraRight[3] = 0.0f;
    constants.cameraUp[0] = up.x;
    constants.cameraUp[1] = up.y;
    constants.cameraUp[2] = up.z;
    constants.cameraUp[3] = 0.0f;
    constants.cameraForward[0] = forward.x;
    constants.cameraForward[1] = forward.y;
    constants.cameraForward[2] = forward.z;
    constants.cameraForward[3] = 0.0f;
    constants.resolution[0] = static_cast<float>(m_Width);
    constants.resolution[1] = static_cast<float>(m_Height);
    constants.resolution[2] = tanHalfFov;
    constants.resolution[3] = aspect;
    constants.time = timeSeconds;
    constants.meshType = m_Mesh == PreviewMesh::Plane ? 1.0f : 0.0f;
    constants.twoSided = graph.Settings().twoSided ? 1.0f : 0.0f;

    for (std::size_t i = 0;
         i < m_Parameters.size() && i < static_cast<std::size_t>(kMaxParameters); ++i) {
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
    std::memcpy(m_ConstantMapped, &constants, sizeof(constants));
    return true;
}

void D3D12PreviewRenderer::Render(const MaterialGraph& graph, float timeSeconds, int width,
                                  int height) {
    if (!m_Device || !m_Pipeline || !m_MaterialValid || width < 2 || height < 2) {
        return;
    }
    if (width != m_Width || height != m_Height || !m_OutputTexture) {
        std::string error;
        if (!CreateSizeDependentResources(width, height, &error)) {
            m_LastError = error;
            return;
        }
    }
    if (!UpdateConstants(graph, timeSeconds)) {
        return;
    }

    m_CommandAllocator->Reset();
    m_CommandList->Reset(m_CommandAllocator.Get(), m_Pipeline.Get());
    m_CommandList->SetComputeRootSignature(m_RootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = {m_DescriptorHeap.Get()};
    m_CommandList->SetDescriptorHeaps(1, heaps);
    m_CommandList->SetComputeRootConstantBufferView(
        0, m_ConstantBuffer->GetGPUVirtualAddress());
    m_CommandList->SetComputeRootDescriptorTable(1, m_SrvTableGpu);
    m_CommandList->SetComputeRootDescriptorTable(2, m_UavTableGpu);

    const UINT groupsX = static_cast<UINT>((m_Width + 7) / 8);
    const UINT groupsY = static_cast<UINT>((m_Height + 7) / 8);
    m_CommandList->Dispatch(groupsX, groupsY, 1);

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    uavBarrier.UAV.pResource = m_OutputTexture.Get();
    m_CommandList->ResourceBarrier(1, &uavBarrier);
    TransitionResource(m_CommandList.Get(), m_OutputTexture.Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = m_ReadbackBuffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Offset = 0;
    destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    destination.PlacedFootprint.Footprint.Width = static_cast<UINT>(m_Width);
    destination.PlacedFootprint.Footprint.Height = static_cast<UINT>(m_Height);
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = AlignTo(
        static_cast<UINT>(m_Width) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = m_OutputTexture.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    m_CommandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    TransitionResource(m_CommandList.Get(), m_OutputTexture.Get(),
                       D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!ExecuteCommandList()) {
        return;
    }

    void* mapped = nullptr;
    if (SUCCEEDED(m_ReadbackBuffer->Map(0, nullptr, &mapped)) && mapped) {
        const UINT rowPitch = AlignTo(static_cast<UINT>(m_Width) * 4,
                                      D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        for (int y = 0; y < m_Height; ++y) {
            std::memcpy(m_ReadbackPixels.data() +
                            static_cast<std::size_t>(y) * m_Width * 4,
                        static_cast<const uint8_t*>(mapped) +
                            static_cast<std::size_t>(y) * rowPitch,
                        static_cast<std::size_t>(m_Width) * 4);
        }
        m_ReadbackBuffer->Unmap(0, nullptr);
        UploadColorToOpenGL(m_Width, m_Height);
        m_UploadPending = false;
    }
}

void D3D12PreviewRenderer::Orbit(float deltaX, float deltaY) {
    m_Camera.yaw -= deltaX * 0.008f;
    m_Camera.pitch = Clamp(m_Camera.pitch + deltaY * 0.008f, -1.35f, 1.35f);
}

void D3D12PreviewRenderer::Zoom(float wheelDelta) {
    m_Camera.distance = Clamp(m_Camera.distance - wheelDelta * 0.25f, 1.8f, 7.5f);
}

bool D3D12PreviewRenderer::ReadPixelTopLeft(int x, int y, float rgba[4]) const {
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

std::string D3D12PreviewRenderer::BackendDetails() const {
    return "DirectX 12 / " + m_AdapterName + " / compute preview";
}

void D3D12PreviewRenderer::Shutdown() {
    if (m_CommandQueue && m_Fence) {
        WaitForFence();
    }
    if (m_ConstantBuffer && m_ConstantMapped) {
        m_ConstantBuffer->Unmap(0, nullptr);
        m_ConstantMapped = nullptr;
    }
    DestroySizeDependentResources();
    m_Textures.clear();
    m_DefaultTexture = GpuTexture{};
    m_Pipeline.Reset();
    m_RootSignature.Reset();
    m_DescriptorHeap.Reset();
    m_ConstantBuffer.Reset();
    m_ReadbackBuffer.Reset();
    m_OutputTexture.Reset();
    m_CommandList.Reset();
    m_CommandAllocator.Reset();
    m_CommandQueue.Reset();
    m_Fence.Reset();
    m_Device.Reset();
    if (m_FenceEvent) {
        CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }
    m_MaterialValid = false;
    m_ShaderLog.clear();
}

} // namespace mf

#endif

