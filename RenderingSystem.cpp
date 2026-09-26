#include "RenderingSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <d3dcompiler.h>
#include <dxgi1_6.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
constexpr float CameraNear = 0.5f;
constexpr float CameraFar = 250.0f;
constexpr float CameraFov = XMConvertToRadians(65.0f);
constexpr float CascadeLambda = 0.75f;

UINT AlignConstantBufferSize(UINT size)
{
    return (size + 255u) & ~255u;
}

D3D12_HEAP_PROPERTIES UploadHeapProperties()
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_HEAP_PROPERTIES DefaultHeapProperties()
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferDescription(UINT64 size)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}
}

void RenderingSystem::Initialize(HWND windowHandle, UINT width, UINT height)
{
    m_windowHandle = windowHandle;
    m_width = (std::max)(width, 1u);
    m_height = (std::max)(height, 1u);

    InitializeDirect3D();
    LoadShaders();
    CreateCubeMesh();
    CreateScene();
    CreateConstantBuffers();
    CreateShadowResources();

    m_initialized = true;
}

void RenderingSystem::InitializeDirect3D()
{
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
    }
#endif

    ComPtr<IDXGIFactory6> factory;
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++index)
    {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
        {
            break;
        }
        adapter.Reset();
    }

    ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_commandQueue)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
    swapChainDescription.BufferCount = FrameCount;
    swapChainDescription.Width = m_width;
    swapChainDescription.Height = m_height;
    swapChainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDescription.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(m_commandQueue.Get(), m_windowHandle, &swapChainDescription, nullptr,
                                                  nullptr, &swapChain));
    ThrowIfFailed(swapChain.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription{};
    rtvHeapDescription.NumDescriptors = FrameCount;
    rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDescription, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDescription{};
    dsvHeapDescription.NumDescriptors = 1;
    dsvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDescription, IID_PPV_ARGS(&m_dsvHeap)));

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT frame = 0; frame < FrameCount; ++frame)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(frame, IID_PPV_ARGS(&m_renderTargets[frame])));
        m_device->CreateRenderTargetView(m_renderTargets[frame].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
        ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&m_commandAllocators[frame])));
    }

    D3D12_RESOURCE_DESC depthDescription{};
    depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDescription.Width = m_width;
    depthDescription.Height = m_height;
    depthDescription.DepthOrArraySize = 1;
    depthDescription.MipLevels = 1;
    depthDescription.Format = DXGI_FORMAT_D32_FLOAT;
    depthDescription.SampleDesc.Count = 1;
    depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;
    auto defaultHeap = DefaultHeapProperties();
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDescription,
                                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                                    IID_PPV_ARGS(&m_depthBuffer)));
    m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0].Get(), nullptr,
                                              IID_PPV_ARGS(&m_commandList)));
    ThrowIfFailed(m_commandList->Close());

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr)
    {
        throw std::runtime_error("Could not create a fence event");
    }

    m_viewport = {0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    m_scissorRect = {0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
}

void RenderingSystem::LoadShaders()
{
    ComPtr<ID3DBlob> mainVertexShader;
    ComPtr<ID3DBlob> mainPixelShader;
    ComPtr<ID3DBlob> shadowVertexShader;
    ComPtr<ID3DBlob> errors;
    UINT shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    shaderFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ThrowIfFailed(D3DCompileFromFile(L"Shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "MainVS", "vs_5_1",
                                     shaderFlags, 0, &mainVertexShader, &errors));
    errors.Reset();
    ThrowIfFailed(D3DCompileFromFile(L"Shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "MainPS", "ps_5_1",
                                     shaderFlags, 0, &mainPixelShader, &errors));
    errors.Reset();
    ThrowIfFailed(D3DCompileFromFile(L"Shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ShadowVS", "vs_5_1",
                                     shaderFlags, 0, &shadowVertexShader, &errors));

    D3D12_DESCRIPTOR_RANGE shadowRange{};
    shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors = 1;
    shadowRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER mainParameters[3]{};
    mainParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    mainParameters[0].Descriptor.ShaderRegister = 0;
    mainParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    mainParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    mainParameters[1].Descriptor.ShaderRegister = 1;
    mainParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    mainParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    mainParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    mainParameters[2].DescriptorTable.pDescriptorRanges = &shadowRange;
    mainParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC comparisonSampler{};
    comparisonSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    comparisonSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    comparisonSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    comparisonSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    comparisonSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    comparisonSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    comparisonSampler.MaxLOD = D3D12_FLOAT32_MAX;
    comparisonSampler.ShaderRegister = 0;
    comparisonSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC mainRootDescription{};
    mainRootDescription.NumParameters = 3;
    mainRootDescription.pParameters = mainParameters;
    mainRootDescription.NumStaticSamplers = 1;
    mainRootDescription.pStaticSamplers = &comparisonSampler;
    mainRootDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRoot;
    ThrowIfFailed(
        D3D12SerializeRootSignature(&mainRootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRoot, &errors));
    ThrowIfFailed(m_device->CreateRootSignature(0, serializedRoot->GetBufferPointer(), serializedRoot->GetBufferSize(),
                                                IID_PPV_ARGS(&m_mainRootSignature)));

    D3D12_ROOT_PARAMETER shadowParameter{};
    shadowParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    shadowParameter.Descriptor.ShaderRegister = 0;
    shadowParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC shadowRootDescription{};
    shadowRootDescription.NumParameters = 1;
    shadowRootDescription.pParameters = &shadowParameter;
    shadowRootDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    serializedRoot.Reset();
    ThrowIfFailed(
        D3D12SerializeRootSignature(&shadowRootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRoot, &errors));
    ThrowIfFailed(m_device->CreateRootSignature(0, serializedRoot->GetBufferPointer(), serializedRoot->GetBufferSize(),
                                                IID_PPV_ARGS(&m_shadowRootSignature)));

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC depthStencil{};
    depthStencil.DepthEnable = TRUE;
    depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC mainPipeline{};
    mainPipeline.pRootSignature = m_mainRootSignature.Get();
    mainPipeline.VS = {mainVertexShader->GetBufferPointer(), mainVertexShader->GetBufferSize()};
    mainPipeline.PS = {mainPixelShader->GetBufferPointer(), mainPixelShader->GetBufferSize()};
    mainPipeline.BlendState = blend;
    mainPipeline.SampleMask = UINT_MAX;
    mainPipeline.RasterizerState = rasterizer;
    mainPipeline.DepthStencilState = depthStencil;
    mainPipeline.InputLayout = {inputLayout, _countof(inputLayout)};
    mainPipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    mainPipeline.NumRenderTargets = 1;
    mainPipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    mainPipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    mainPipeline.SampleDesc.Count = 1;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&mainPipeline, IID_PPV_ARGS(&m_mainPipelineState)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPipeline = mainPipeline;
    shadowPipeline.pRootSignature = m_shadowRootSignature.Get();
    shadowPipeline.VS = {shadowVertexShader->GetBufferPointer(), shadowVertexShader->GetBufferSize()};
    shadowPipeline.PS = {};
    shadowPipeline.NumRenderTargets = 0;
    shadowPipeline.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    shadowPipeline.RasterizerState.DepthBias = 1000;
    shadowPipeline.RasterizerState.SlopeScaledDepthBias = 1.5f;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&shadowPipeline, IID_PPV_ARGS(&m_shadowPipelineState)));
}

void RenderingSystem::CreateCubeMesh()
{
    const Vertex vertices[] = {
        {{-1, -1, -1}, {0, 0, -1}}, {{-1, 1, -1}, {0, 0, -1}},  {{1, 1, -1}, {0, 0, -1}},  {{1, -1, -1}, {0, 0, -1}},
        {{1, -1, 1}, {0, 0, 1}},    {{1, 1, 1}, {0, 0, 1}},     {{-1, 1, 1}, {0, 0, 1}},   {{-1, -1, 1}, {0, 0, 1}},
        {{-1, -1, 1}, {-1, 0, 0}},  {{-1, 1, 1}, {-1, 0, 0}},   {{-1, 1, -1}, {-1, 0, 0}}, {{-1, -1, -1}, {-1, 0, 0}},
        {{1, -1, -1}, {1, 0, 0}},   {{1, 1, -1}, {1, 0, 0}},    {{1, 1, 1}, {1, 0, 0}},    {{1, -1, 1}, {1, 0, 0}},
        {{-1, 1, -1}, {0, 1, 0}},   {{-1, 1, 1}, {0, 1, 0}},    {{1, 1, 1}, {0, 1, 0}},    {{1, 1, -1}, {0, 1, 0}},
        {{-1, -1, 1}, {0, -1, 0}},  {{-1, -1, -1}, {0, -1, 0}}, {{1, -1, -1}, {0, -1, 0}}, {{1, -1, 1}, {0, -1, 0}}};
    const uint16_t indices[] = {0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
                                12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23};

    auto uploadHeap = UploadHeapProperties();
    auto vertexDescription = BufferDescription(sizeof(vertices));
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vertexDescription,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&m_vertexBuffer)));
    void *data = nullptr;
    ThrowIfFailed(m_vertexBuffer->Map(0, nullptr, &data));
    std::memcpy(data, vertices, sizeof(vertices));
    m_vertexBuffer->Unmap(0, nullptr);
    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.SizeInBytes = sizeof(vertices);
    m_vertexBufferView.StrideInBytes = sizeof(Vertex);

    auto indexDescription = BufferDescription(sizeof(indices));
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &indexDescription,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&m_indexBuffer)));
    ThrowIfFailed(m_indexBuffer->Map(0, nullptr, &data));
    std::memcpy(data, indices, sizeof(indices));
    m_indexBuffer->Unmap(0, nullptr);
    m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_indexBufferView.SizeInBytes = sizeof(indices);
    m_indexBufferView.Format = DXGI_FORMAT_R16_UINT;
}

void RenderingSystem::CreateScene()
{
    m_scene.clear();
    m_scene.push_back({{0.0f, -2.0f, 95.0f}, {85.0f, 0.5f, 120.0f}, {0.24f, 0.27f, 0.31f, 1.0f}});

    constexpr int pillarCount = 11;
    for (int index = 0; index < pillarCount; ++index)
    {
        const float height = 2.0f + static_cast<float>(index) * 0.75f;
        const float positionX = (static_cast<float>(index) - (pillarCount - 1) * 0.5f) * 14.0f;
        const float colorFactor = static_cast<float>(index) / static_cast<float>(pillarCount - 1);
        m_scene.push_back({{positionX, height - 1.5f, 35.0f},
                           {1.8f, height, 1.8f},
                           {0.25f + colorFactor * 0.25f, 0.42f, 0.62f - colorFactor * 0.18f, 1.0f}});
    }
}

void RenderingSystem::CreateConstantBuffers()
{
    m_drawConstantSize = AlignConstantBufferSize(sizeof(DrawConstants));
    m_shadowConstantSize = AlignConstantBufferSize(sizeof(ShadowConstants));
    const UINT64 drawBufferSize =
        static_cast<UINT64>(FrameCount) * (CascadeCount + 1) * MaxObjects * m_drawConstantSize;
    const UINT64 shadowBufferSize = static_cast<UINT64>(FrameCount) * m_shadowConstantSize;
    auto uploadHeap = UploadHeapProperties();
    auto drawDescription = BufferDescription(drawBufferSize);
    auto shadowDescription = BufferDescription(shadowBufferSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &drawDescription,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&m_drawConstantBuffer)));
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &shadowDescription,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&m_shadowConstantBuffer)));
    ThrowIfFailed(m_drawConstantBuffer->Map(0, nullptr, reinterpret_cast<void **>(&m_mappedDrawConstants)));
    ThrowIfFailed(m_shadowConstantBuffer->Map(0, nullptr, reinterpret_cast<void **>(&m_mappedShadowConstants)));
}

void RenderingSystem::CreateShadowResources()
{
    D3D12_DESCRIPTOR_HEAP_DESC dsvDescription{};
    dsvDescription.NumDescriptors = CascadeCount;
    dsvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvDescription, IID_PPV_ARGS(&m_shadowDsvHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC srvDescription{};
    srvDescription.NumDescriptors = 1;
    srvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvDescription, IID_PPV_ARGS(&m_shadowSrvHeap)));

    D3D12_RESOURCE_DESC shadowDescription{};
    shadowDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadowDescription.Width = ShadowMapSize;
    shadowDescription.Height = ShadowMapSize;
    shadowDescription.DepthOrArraySize = CascadeCount;
    shadowDescription.MipLevels = 1;
    shadowDescription.Format = DXGI_FORMAT_R32_TYPELESS;
    shadowDescription.SampleDesc.Count = 1;
    shadowDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    auto defaultHeap = DefaultHeapProperties();
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &shadowDescription,
                                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                                                    IID_PPV_ARGS(&m_shadowMap)));

    const UINT dsvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT cascade = 0; cascade < CascadeCount; ++cascade)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D32_FLOAT;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.ArraySize = 1;
        view.Texture2DArray.FirstArraySlice = cascade;
        view.Texture2DArray.MipSlice = 0;
        m_device->CreateDepthStencilView(m_shadowMap.Get(), &view, dsvHandle);
        dsvHandle.ptr += dsvSize;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC shadowView{};
    shadowView.Format = DXGI_FORMAT_R32_FLOAT;
    shadowView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    shadowView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowView.Texture2DArray.ArraySize = CascadeCount;
    shadowView.Texture2DArray.MipLevels = 1;
    m_device->CreateShaderResourceView(m_shadowMap.Get(), &shadowView,
                                       m_shadowSrvHeap->GetCPUDescriptorHandleForHeapStart());

    m_shadowViewport = {0.0f, 0.0f, static_cast<float>(ShadowMapSize), static_cast<float>(ShadowMapSize), 0.0f, 1.0f};
    m_shadowScissorRect = {0, 0, static_cast<LONG>(ShadowMapSize), static_cast<LONG>(ShadowMapSize)};
    m_shadowReadable = false;
}
// Четыре слоя depth-текстуры хранят отдельную карту теней для каждого каскада.

void RenderingSystem::SetCameraInput(float forward, float right, float turn, float vertical)
{
    m_moveForward = std::clamp(forward, -1.0f, 1.0f);
    m_moveRight = std::clamp(right, -1.0f, 1.0f);
    m_moveTurn = std::clamp(turn, -1.0f, 1.0f);
    m_moveVertical = std::clamp(vertical, -1.0f, 1.0f);
}

void RenderingSystem::UpdateCamera(float deltaTime)
{
    const float speed = 18.0f;
    deltaTime = (std::min)(deltaTime, 0.05f);
    m_cameraRotationY += m_moveTurn * deltaTime * 1.6f;

    const XMVECTOR forward = XMVectorSet(std::sin(m_cameraRotationY), 0.0f, std::cos(m_cameraRotationY), 0.0f);
    const XMVECTOR right = XMVectorSet(std::cos(m_cameraRotationY), 0.0f, -std::sin(m_cameraRotationY), 0.0f);
    XMVECTOR position = XMLoadFloat3(&m_cameraPosition);
    position += forward * (m_moveForward * speed * deltaTime);
    position += right * (m_moveRight * speed * deltaTime);
    position += XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f) * (m_moveVertical * speed * deltaTime);
    XMStoreFloat3(&m_cameraPosition, position);

    m_cameraPosition.x = std::clamp(m_cameraPosition.x, -50.0f, 50.0f);
    m_cameraPosition.y = std::clamp(m_cameraPosition.y, 1.0f, 45.0f);
    m_cameraPosition.z = std::clamp(m_cameraPosition.z, -38.0f, 210.0f);

    const XMVECTOR eye = XMLoadFloat3(&m_cameraPosition);
    const XMVECTOR viewForward = XMVector3Normalize(
        XMVectorSet(std::sin(m_cameraRotationY), -0.08f, std::cos(m_cameraRotationY), 0.0f));
    m_view = XMMatrixLookToLH(eye, viewForward, XMVectorSet(0, 1, 0, 0));
    m_projection = XMMatrixPerspectiveFovLH(CameraFov, static_cast<float>(m_width) / static_cast<float>(m_height),
                                            CameraNear, CameraFar);
}

void RenderingSystem::CalculateCascadeMatrices()
{
    for (UINT index = 0; index < CascadeCount; ++index)
    {
        const float fraction = static_cast<float>(index + 1) / static_cast<float>(CascadeCount);
        const float logarithmic = CameraNear * std::pow(CameraFar / CameraNear, fraction);
        const float uniform = CameraNear + (CameraFar - CameraNear) * fraction;
        m_cascadeSplits[index] = CascadeLambda * logarithmic + (1.0f - CascadeLambda) * uniform;
    }
    // Смешанное логарифмическое распределение повышает детализацию ближних каскадов.

    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    const float tangent = std::tan(CameraFov * 0.5f);
    const XMVECTOR camera = XMLoadFloat3(&m_cameraPosition);
    const XMVECTOR forward = XMVector3Normalize(
        XMVectorSet(std::sin(m_cameraRotationY), -0.08f, std::cos(m_cameraRotationY), 0.0f));
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), forward));
    const XMVECTOR up = XMVector3Normalize(XMVector3Cross(forward, right));
    const XMVECTOR lightDirection = XMVector3Normalize(XMLoadFloat3(&m_lightDirection));

    float previousSplit = CameraNear;
    for (UINT cascade = 0; cascade < CascadeCount; ++cascade)
    {
        const float currentSplit = m_cascadeSplits[cascade];
        XMVECTOR corners[8];
        UINT cornerIndex = 0;
        for (float distance : {previousSplit, currentSplit})
        {
            const float halfHeight = tangent * distance;
            const float halfWidth = halfHeight * aspect;
            const XMVECTOR center = camera + forward * distance;
            corners[cornerIndex++] = center - right * halfWidth - up * halfHeight;
            corners[cornerIndex++] = center - right * halfWidth + up * halfHeight;
            corners[cornerIndex++] = center + right * halfWidth + up * halfHeight;
            corners[cornerIndex++] = center + right * halfWidth - up * halfHeight;
        }

        XMVECTOR center = XMVectorZero();
        for (const XMVECTOR &corner : corners)
        {
            center += corner;
        }
        center /= 8.0f;

        float radius = 0.0f;
        for (const XMVECTOR &corner : corners)
        {
            radius = (std::max)(radius, XMVectorGetX(XMVector3Length(corner - center)));
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;

        const XMVECTOR lightPosition = center - lightDirection * (radius * 2.0f + 50.0f);
        const XMMATRIX lightView = XMMatrixLookAtLH(lightPosition, center, XMVectorSet(0, 1, 0, 0));
        float minimumX = FLT_MAX;
        float minimumY = FLT_MAX;
        float minimumZ = FLT_MAX;
        float maximumX = -FLT_MAX;
        float maximumY = -FLT_MAX;
        float maximumZ = -FLT_MAX;
        for (const XMVECTOR &corner : corners)
        {
            const XMVECTOR transformed = XMVector3TransformCoord(corner, lightView);
            minimumX = (std::min)(minimumX, XMVectorGetX(transformed));
            minimumY = (std::min)(minimumY, XMVectorGetY(transformed));
            minimumZ = (std::min)(minimumZ, XMVectorGetZ(transformed));
            maximumX = (std::max)(maximumX, XMVectorGetX(transformed));
            maximumY = (std::max)(maximumY, XMVectorGetY(transformed));
            maximumZ = (std::max)(maximumZ, XMVectorGetZ(transformed));
        }

        minimumZ -= radius;
        maximumZ += radius;
        const XMMATRIX lightProjection =
            XMMatrixOrthographicOffCenterLH(minimumX, maximumX, minimumY, maximumY, minimumZ, maximumZ);
        m_cascadeMatrices[cascade] = lightView * lightProjection;
        previousSplit = currentSplit;
    }
}

void RenderingSystem::WriteDrawConstants(UINT slot, const SceneObject &object, const XMMATRIX &viewProjection)
{
    DrawConstants constants{};
    const XMMATRIX world = XMMatrixScaling(object.Scale.x, object.Scale.y, object.Scale.z) *
                           XMMatrixTranslation(object.Position.x, object.Position.y, object.Position.z);
    XMStoreFloat4x4(&constants.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&constants.ViewProjection, XMMatrixTranspose(viewProjection));
    constants.Color = object.Color;
    std::memcpy(m_mappedDrawConstants + static_cast<size_t>(slot) * m_drawConstantSize, &constants, sizeof(constants));
}

void RenderingSystem::RenderShadowMaps()
{
    if (m_shadowReadable)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_shadowMap.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &barrier);
    }

    m_commandList->SetPipelineState(m_shadowPipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_shadowRootSignature.Get());
    m_commandList->RSSetViewports(1, &m_shadowViewport);
    m_commandList->RSSetScissorRects(1, &m_shadowScissorRect);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->IASetIndexBuffer(&m_indexBufferView);

    const UINT dsvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT frameBase = m_frameIndex * (CascadeCount + 1) * MaxObjects;
    for (UINT cascade = 0; cascade < CascadeCount; ++cascade)
    {
        m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);
        for (UINT objectIndex = 0; objectIndex < m_scene.size(); ++objectIndex)
        {
            const UINT slot = frameBase + (cascade + 1) * MaxObjects + objectIndex;
            WriteDrawConstants(slot, m_scene[objectIndex], m_cascadeMatrices[cascade]);
            m_commandList->SetGraphicsRootConstantBufferView(0, m_drawConstantBuffer->GetGPUVirtualAddress() +
                                                                    static_cast<UINT64>(slot) * m_drawConstantSize);
            m_commandList->DrawIndexedInstanced(36, 1, 0, 0, 0);
        }
        dsvHandle.ptr += dsvSize;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_shadowMap.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);
    m_shadowReadable = true;
}
// Теневой проход заполняет четыре каскада до основной отрисовки сцены.

void RenderingSystem::PopulateCommandList()
{
    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    m_timer.Tick();
    UpdateCamera(m_timer.GetDeltaTime());
    CalculateCascadeMatrices();

    ShadowConstants shadowConstants{};
    for (UINT cascade = 0; cascade < CascadeCount; ++cascade)
    {
        XMStoreFloat4x4(&shadowConstants.LightViewProjection[cascade], XMMatrixTranspose(m_cascadeMatrices[cascade]));
    }
    shadowConstants.CascadeSplits = {m_cascadeSplits[0], m_cascadeSplits[1], m_cascadeSplits[2], m_cascadeSplits[3]};
    shadowConstants.LightDirection = {m_lightDirection.x, m_lightDirection.y, m_lightDirection.z, 0.0f};
    shadowConstants.CameraPosition = {m_cameraPosition.x, m_cameraPosition.y, m_cameraPosition.z, 1.0f};
    const XMVECTOR cameraForward = XMVector3Normalize(
        XMVectorSet(std::sin(m_cameraRotationY), -0.08f, std::cos(m_cameraRotationY), 0.0f));
    XMStoreFloat4(&shadowConstants.CameraForward, cameraForward);
    shadowConstants.ShadowMapSize = {static_cast<float>(ShadowMapSize), static_cast<float>(ShadowMapSize),
                                     1.0f / static_cast<float>(ShadowMapSize),
                                     1.0f / static_cast<float>(ShadowMapSize)};
    std::memcpy(m_mappedShadowConstants + static_cast<size_t>(m_frameIndex) * m_shadowConstantSize, &shadowConstants,
                sizeof(shadowConstants));

    RenderShadowMaps();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clearColor[] = {0.035f, 0.055f, 0.08f, 1.0f};
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);
    m_commandList->SetPipelineState(m_mainPipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_mainRootSignature.Get());
    ID3D12DescriptorHeap *heaps[] = {m_shadowSrvHeap.Get()};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetGraphicsRootDescriptorTable(2, m_shadowSrvHeap->GetGPUDescriptorHandleForHeapStart());
    m_commandList->SetGraphicsRootConstantBufferView(1, m_shadowConstantBuffer->GetGPUVirtualAddress() +
                                                            static_cast<UINT64>(m_frameIndex) * m_shadowConstantSize);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->IASetIndexBuffer(&m_indexBufferView);

    const UINT frameBase = m_frameIndex * (CascadeCount + 1) * MaxObjects;
    const XMMATRIX viewProjection = m_view * m_projection;
    for (UINT objectIndex = 0; objectIndex < m_scene.size(); ++objectIndex)
    {
        const UINT slot = frameBase + objectIndex;
        WriteDrawConstants(slot, m_scene[objectIndex], viewProjection);
        m_commandList->SetGraphicsRootConstantBufferView(0, m_drawConstantBuffer->GetGPUVirtualAddress() +
                                                                static_cast<UINT64>(slot) * m_drawConstantSize);
        m_commandList->DrawIndexedInstanced(36, 1, 0, 0, 0);
    }

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    m_commandList->ResourceBarrier(1, &barrier);
    ThrowIfFailed(m_commandList->Close());
}

void RenderingSystem::Render()
{
    if (!m_initialized)
    {
        return;
    }
    PopulateCommandList();
    ID3D12CommandList *commandLists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    ThrowIfFailed(m_swapChain->Present(1, 0));
    MoveToNextFrame();
}

void RenderingSystem::WaitForGPU()
{
    const UINT64 value = ++m_fenceValues[m_frameIndex];
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), value));
    if (m_fence->GetCompletedValue() < value)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(value, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void RenderingSystem::MoveToNextFrame()
{
    const UINT64 currentFence = ++m_fenceValues[m_frameIndex];
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), currentFence));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_fenceValues[m_frameIndex] = currentFence;
}

void RenderingSystem::Resize(UINT width, UINT height)
{
    if (!m_initialized || width == 0 || height == 0)
    {
        return;
    }
    WaitForGPU();
    m_width = width;
    m_height = height;
    for (auto &renderTarget : m_renderTargets)
    {
        renderTarget.Reset();
    }
    m_depthBuffer.Reset();

    DXGI_SWAP_CHAIN_DESC swapDescription{};
    ThrowIfFailed(m_swapChain->GetDesc(&swapDescription));
    ThrowIfFailed(m_swapChain->ResizeBuffers(FrameCount, width, height, swapDescription.BufferDesc.Format,
                                             swapDescription.Flags));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT frame = 0; frame < FrameCount; ++frame)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(frame, IID_PPV_ARGS(&m_renderTargets[frame])));
        m_device->CreateRenderTargetView(m_renderTargets[frame].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    D3D12_RESOURCE_DESC depthDescription{};
    depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDescription.Width = width;
    depthDescription.Height = height;
    depthDescription.DepthOrArraySize = 1;
    depthDescription.MipLevels = 1;
    depthDescription.Format = DXGI_FORMAT_D32_FLOAT;
    depthDescription.SampleDesc.Count = 1;
    depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    auto defaultHeap = DefaultHeapProperties();
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDescription,
                                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                                                    IID_PPV_ARGS(&m_depthBuffer)));
    m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    m_viewport.Width = static_cast<float>(width);
    m_viewport.Height = static_cast<float>(height);
    m_scissorRect.right = static_cast<LONG>(width);
    m_scissorRect.bottom = static_cast<LONG>(height);
}

void RenderingSystem::Cleanup()
{
    if (!m_device)
    {
        return;
    }
    WaitForGPU();
    if (m_drawConstantBuffer && m_mappedDrawConstants)
    {
        m_drawConstantBuffer->Unmap(0, nullptr);
        m_mappedDrawConstants = nullptr;
    }
    if (m_shadowConstantBuffer && m_mappedShadowConstants)
    {
        m_shadowConstantBuffer->Unmap(0, nullptr);
        m_mappedShadowConstants = nullptr;
    }
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_initialized = false;
}
