#include "RenderingSystem.h"

#include <cmath>
#include <cstring>
#include <random>

using namespace DirectX;

namespace
{
constexpr float CameraNear = 0.1f;
constexpr float CameraFar = 250.0f;
constexpr float CameraFov = XMConvertToRadians(65.0f);
constexpr UINT CounterBufferSize = D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT;

UINT AlignConstantBufferSize(UINT size)
{
    return (size + 255u) & ~255u;
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferDescription(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
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
    description.Flags = flags;
    return description;
}

void FillRasterizerDescription(D3D12_RASTERIZER_DESC &description)
{
    description.FillMode = D3D12_FILL_MODE_SOLID;
    description.CullMode = D3D12_CULL_MODE_NONE;
    description.FrontCounterClockwise = FALSE;
    description.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    description.DepthClipEnable = TRUE;
    description.MultisampleEnable = FALSE;
    description.AntialiasedLineEnable = FALSE;
    description.ForcedSampleCount = 0;
    description.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
}
}

static_assert(sizeof(Particle) == 64, "Particle layout must match HLSL");

void RenderingSystem::Initialize(HWND windowHandle, UINT width, UINT height)
{
    m_windowHandle = windowHandle;
    m_width = (std::max)(width, 1u);
    m_height = (std::max)(height, 1u);

    InitializeDirect3D();
    LoadShaders();
    CreateParticleResources();
    CreateConstantBuffers();
    UpdateCamera(0.0f);
    m_initialized = true;
}

void RenderingSystem::InitializeDirect3D()
{
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        debugController->EnableDebugLayer();
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
            break;
        adapter.Reset();
    }
    if (!adapter)
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
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
    auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDescription,
                                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                                    IID_PPV_ARGS(&m_depthBuffer)));
    m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr,
                                     m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0].Get(), nullptr,
                                              IID_PPV_ARGS(&m_commandList)));
    ThrowIfFailed(m_commandList->Close());

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValues[0] = 1;
    m_fenceValues[1] = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw std::runtime_error("Could not create fence event");

    m_viewport = {0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    m_scissorRect = {0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
}

void RenderingSystem::LoadShaders()
{
    ComPtr<ID3DBlob> computeShader;
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> geometryShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> platformVertexShader;
    ComPtr<ID3DBlob> platformPixelShader;
    ComPtr<ID3DBlob> errors;
    UINT shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    shaderFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compile = [&](const char *entryPoint, const char *target, ComPtr<ID3DBlob> &result) {
        errors.Reset();
        const HRESULT status = D3DCompileFromFile(L"Shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                  entryPoint, target, shaderFlags, 0, &result, &errors);
        if (FAILED(status) && errors)
            OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
        ThrowIfFailed(status);
    };
    compile("ParticleCS", "cs_5_1", computeShader);
    compile("ParticleVS", "vs_5_1", vertexShader);
    compile("ParticleGS", "gs_5_1", geometryShader);
    compile("ParticlePS", "ps_5_1", pixelShader);
    compile("PlatformVS", "vs_5_1", platformVertexShader);
    compile("PlatformPS", "ps_5_1", platformPixelShader);

    D3D12_DESCRIPTOR_RANGE computeRange{};
    computeRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    computeRange.NumDescriptors = 2;
    computeRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER computeParameters[2]{};
    computeParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    computeParameters[0].DescriptorTable.pDescriptorRanges = &computeRange;
    computeParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    computeParameters[1].Descriptor.ShaderRegister = 0;
    computeParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC computeRootDescription{};
    computeRootDescription.NumParameters = _countof(computeParameters);
    computeRootDescription.pParameters = computeParameters;

    ComPtr<ID3DBlob> serializedRoot;
    ThrowIfFailed(D3D12SerializeRootSignature(&computeRootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRoot,
                                              &errors));
    ThrowIfFailed(m_device->CreateRootSignature(0, serializedRoot->GetBufferPointer(), serializedRoot->GetBufferSize(),
                                                IID_PPV_ARGS(&m_computeRootSignature)));

    D3D12_DESCRIPTOR_RANGE renderRange{};
    renderRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    renderRange.NumDescriptors = 1;
    renderRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER renderParameters[2]{};
    renderParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    renderParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    renderParameters[0].DescriptorTable.pDescriptorRanges = &renderRange;
    renderParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    renderParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    renderParameters[1].Descriptor.ShaderRegister = 0;
    renderParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC renderRootDescription{};
    renderRootDescription.NumParameters = _countof(renderParameters);
    renderRootDescription.pParameters = renderParameters;
    renderRootDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    serializedRoot.Reset();
    ThrowIfFailed(D3D12SerializeRootSignature(&renderRootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRoot,
                                              &errors));
    ThrowIfFailed(m_device->CreateRootSignature(0, serializedRoot->GetBufferPointer(), serializedRoot->GetBufferSize(),
                                                IID_PPV_ARGS(&m_renderRootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC computePipeline{};
    computePipeline.pRootSignature = m_computeRootSignature.Get();
    computePipeline.CS = {computeShader->GetBufferPointer(), computeShader->GetBufferSize()};
    ThrowIfFailed(m_device->CreateComputePipelineState(&computePipeline, IID_PPV_ARGS(&m_computePipelineState)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC renderPipeline{};
    renderPipeline.pRootSignature = m_renderRootSignature.Get();
    renderPipeline.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    renderPipeline.GS = {geometryShader->GetBufferPointer(), geometryShader->GetBufferSize()};
    renderPipeline.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    FillRasterizerDescription(renderPipeline.RasterizerState);
    renderPipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    renderPipeline.DepthStencilState.DepthEnable = TRUE;
    renderPipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    renderPipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    renderPipeline.SampleMask = UINT_MAX;
    renderPipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    renderPipeline.NumRenderTargets = 1;
    renderPipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    renderPipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    renderPipeline.SampleDesc.Count = 1;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&renderPipeline, IID_PPV_ARGS(&m_renderPipelineState)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC platformPipeline = renderPipeline;
    platformPipeline.VS = {platformVertexShader->GetBufferPointer(), platformVertexShader->GetBufferSize()};
    platformPipeline.GS = {};
    platformPipeline.PS = {platformPixelShader->GetBufferPointer(), platformPixelShader->GetBufferSize()};
    platformPipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&platformPipeline,
                                                        IID_PPV_ARGS(&m_platformPipelineState)));
}

void RenderingSystem::CreateParticleResources()
{
    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDescription{};
    descriptorHeapDescription.NumDescriptors = 6;
    descriptorHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&descriptorHeapDescription,
                                                 IID_PPV_ARGS(&m_particleDescriptorHeap)));
    m_particleDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    const UINT64 particleBufferSize = static_cast<UINT64>(sizeof(Particle)) * ParticleCount;
    const auto particleDescription =
        BufferDescription(particleBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto counterDescription =
        BufferDescription(CounterBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);

    for (UINT index = 0; index < 2; ++index)
    {
        ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &particleDescription,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&m_particleBuffers[index])));
        ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &counterDescription,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&m_particleCounters[index])));
    }

    std::vector<Particle> particles(ParticleCount);
    std::mt19937 generator(2026);
    std::uniform_real_distribution<float> angleDistribution(0.0f, XM_2PI);
    std::uniform_real_distribution<float> horizontalDistribution(0.6f, 2.2f);
    std::uniform_real_distribution<float> verticalDistribution(7.0f, 12.0f);
    std::uniform_real_distribution<float> lifetimeDistribution(1.6f, 3.4f);
    std::uniform_real_distribution<float> sizeDistribution(0.08f, 0.18f);
    std::uniform_real_distribution<float> colorDistribution(0.0f, 1.0f);
    const XMFLOAT3 emitter(0.0f, 0.0f, 20.0f);

    for (UINT index = 0; index < ParticleCount; ++index)
    {
        const float angle = angleDistribution(generator);
        const float horizontalSpeed = horizontalDistribution(generator);
        const float lifetime = lifetimeDistribution(generator);
        const float age = static_cast<float>(index) / ParticleCount * lifetime;
        const XMFLOAT3 velocity(std::cos(angle) * horizontalSpeed, verticalDistribution(generator),
                               std::sin(angle) * horizontalSpeed);
        Particle particle{};
        particle.Position = {emitter.x + velocity.x * age,
                             emitter.y + 0.15f + velocity.y * age - 4.905f * age * age,
                             emitter.z + velocity.z * age};
        particle.Age = age;
        particle.Velocity = {velocity.x, velocity.y - 9.81f * age, velocity.z};
        particle.Lifetime = lifetime;
        const float colorFactor = colorDistribution(generator);
        particle.Color = {0.10f + 0.55f * colorFactor, 0.45f + 0.45f * colorFactor, 1.0f, 1.0f};
        particle.Size = sizeDistribution(generator);
        particle.Seed = generator();
        particles[index] = particle;
    }

    auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const auto particleUploadDescription = BufferDescription(particleBufferSize);
    const auto counterUploadDescription = BufferDescription(sizeof(UINT));
    ComPtr<ID3D12Resource> particleUploads[2];
    ComPtr<ID3D12Resource> counterUploads[2];

    for (UINT index = 0; index < 2; ++index)
    {
        ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                                                        &particleUploadDescription,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&particleUploads[index])));
        ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                                                        &counterUploadDescription,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&counterUploads[index])));
        void *mapped = nullptr;
        ThrowIfFailed(particleUploads[index]->Map(0, nullptr, &mapped));
        if (index == 0)
            std::memcpy(mapped, particles.data(), static_cast<size_t>(particleBufferSize));
        else
            std::memset(mapped, 0, static_cast<size_t>(particleBufferSize));
        particleUploads[index]->Unmap(0, nullptr);

        ThrowIfFailed(counterUploads[index]->Map(0, nullptr, &mapped));
        const UINT initialCount = index == 0 ? ParticleCount : 0;
        std::memcpy(mapped, &initialCount, sizeof(initialCount));
        counterUploads[index]->Unmap(0, nullptr);
    }

    ThrowIfFailed(m_commandAllocators[0]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[0].Get(), nullptr));
    for (UINT index = 0; index < 2; ++index)
    {
        m_commandList->CopyBufferRegion(m_particleBuffers[index].Get(), 0, particleUploads[index].Get(), 0,
                                        particleBufferSize);
        m_commandList->CopyBufferRegion(m_particleCounters[index].Get(), 0, counterUploads[index].Get(), 0,
                                        sizeof(UINT));
    }

    D3D12_RESOURCE_BARRIER barriers[4]{};
    for (UINT index = 0; index < 2; ++index)
    {
        barriers[index * 2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index * 2].Transition.pResource = m_particleBuffers[index].Get();
        barriers[index * 2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[index * 2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[index * 2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[index * 2 + 1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index * 2 + 1].Transition.pResource = m_particleCounters[index].Get();
        barriers[index * 2 + 1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[index * 2 + 1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[index * 2 + 1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    m_commandList->ResourceBarrier(_countof(barriers), barriers);
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList *lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGPU();

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDescription{};
    uavDescription.Format = DXGI_FORMAT_UNKNOWN;
    uavDescription.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDescription.Buffer.NumElements = ParticleCount;
    uavDescription.Buffer.StructureByteStride = sizeof(Particle);
    uavDescription.Buffer.CounterOffsetInBytes = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_particleDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    auto createUav = [&](UINT particleIndex) {
        m_device->CreateUnorderedAccessView(m_particleBuffers[particleIndex].Get(),
                                            m_particleCounters[particleIndex].Get(), &uavDescription, handle);
        handle.ptr += m_particleDescriptorSize;
    };
    createUav(0);
    createUav(1);
    createUav(1);
    createUav(0);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = DXGI_FORMAT_UNKNOWN;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Buffer.NumElements = ParticleCount;
    srvDescription.Buffer.StructureByteStride = sizeof(Particle);
    m_device->CreateShaderResourceView(m_particleBuffers[0].Get(), &srvDescription, handle);
    handle.ptr += m_particleDescriptorSize;
    m_device->CreateShaderResourceView(m_particleBuffers[1].Get(), &srvDescription, handle);
}
// Два StructuredBuffer поочерёдно используются как Consume- и Append-буфер.

void RenderingSystem::CreateConstantBuffers()
{
    m_computeConstantSize = AlignConstantBufferSize(sizeof(ComputeConstants));
    m_renderConstantSize = AlignConstantBufferSize(sizeof(RenderConstants));
    auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    auto computeDescription = BufferDescription(static_cast<UINT64>(m_computeConstantSize) * FrameCount);
    auto renderDescription = BufferDescription(static_cast<UINT64>(m_renderConstantSize) * FrameCount);
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &computeDescription,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&m_computeConstantBuffer)));
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &renderDescription,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&m_renderConstantBuffer)));
    ThrowIfFailed(m_computeConstantBuffer->Map(0, nullptr,
                                               reinterpret_cast<void **>(&m_mappedComputeConstants)));
    ThrowIfFailed(m_renderConstantBuffer->Map(0, nullptr, reinterpret_cast<void **>(&m_mappedRenderConstants)));
}

void RenderingSystem::SetCameraInput(float forward, float right, float turn, float vertical)
{
    m_moveForward = std::clamp(forward, -1.0f, 1.0f);
    m_moveRight = std::clamp(right, -1.0f, 1.0f);
    m_moveTurn = std::clamp(turn, -1.0f, 1.0f);
    m_moveVertical = std::clamp(vertical, -1.0f, 1.0f);
}

void RenderingSystem::UpdateCamera(float deltaTime)
{
    const float speed = 14.0f;
    deltaTime = (std::min)(deltaTime, 0.05f);
    m_cameraRotationY += m_moveTurn * deltaTime * 1.6f;

    const XMVECTOR forward = XMVectorSet(std::sin(m_cameraRotationY), 0.0f, std::cos(m_cameraRotationY), 0.0f);
    const XMVECTOR right = XMVectorSet(std::cos(m_cameraRotationY), 0.0f, -std::sin(m_cameraRotationY), 0.0f);
    XMVECTOR position = XMLoadFloat3(&m_cameraPosition);
    position += forward * (m_moveForward * speed * deltaTime);
    position += right * (m_moveRight * speed * deltaTime);
    position += XMVectorSet(0, 1, 0, 0) * (m_moveVertical * speed * deltaTime);
    XMStoreFloat3(&m_cameraPosition, position);

    m_cameraPosition.x = std::clamp(m_cameraPosition.x, -60.0f, 60.0f);
    m_cameraPosition.y = std::clamp(m_cameraPosition.y, 1.0f, 50.0f);
    m_cameraPosition.z = std::clamp(m_cameraPosition.z, -40.0f, 100.0f);

    const XMVECTOR eye = XMLoadFloat3(&m_cameraPosition);
    m_view = XMMatrixLookToLH(eye, forward, XMVectorSet(0, 1, 0, 0));
    m_projection = XMMatrixPerspectiveFovLH(CameraFov, static_cast<float>(m_width) / m_height,
                                            CameraNear, CameraFar);
}

void RenderingSystem::PopulateCommandList()
{
    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    m_timer.Tick();
    UpdateCamera(m_timer.GetDeltaTime());
    const UINT destinationParticleBuffer = 1u - m_sourceParticleBuffer;

    ComputeConstants computeConstants{};
    computeConstants.DeltaTime = (std::min)(m_timer.GetDeltaTime(), 0.05f);
    computeConstants.TotalTime = m_timer.GetTotalTime();
    computeConstants.ParticleCount = ParticleCount;
    computeConstants.EmitterPosition = {0.0f, 0.0f, 20.0f};
    std::memcpy(m_mappedComputeConstants + static_cast<size_t>(m_frameIndex) * m_computeConstantSize,
                &computeConstants, sizeof(computeConstants));

    const XMMATRIX viewProjection = m_view * m_projection;
    const XMMATRIX inverseView = XMMatrixInverse(nullptr, m_view);
    RenderConstants renderConstants{};
    XMStoreFloat4x4(&renderConstants.ViewProjection, XMMatrixTranspose(viewProjection));
    XMStoreFloat4(&renderConstants.CameraRight, XMVectorSetW(inverseView.r[0], 0.0f));
    XMStoreFloat4(&renderConstants.CameraUp, XMVectorSetW(inverseView.r[1], 0.0f));
    std::memcpy(m_mappedRenderConstants + static_cast<size_t>(m_frameIndex) * m_renderConstantSize,
                &renderConstants, sizeof(renderConstants));

    ID3D12DescriptorHeap *descriptorHeaps[] = {m_particleDescriptorHeap.Get()};
    m_commandList->SetDescriptorHeaps(1, descriptorHeaps);
    m_commandList->SetPipelineState(m_computePipelineState.Get());
    m_commandList->SetComputeRootSignature(m_computeRootSignature.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE computeHandle = m_particleDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    if (m_sourceParticleBuffer == 1)
        computeHandle.ptr += static_cast<UINT64>(2) * m_particleDescriptorSize;
    m_commandList->SetComputeRootDescriptorTable(0, computeHandle);
    m_commandList->SetComputeRootConstantBufferView(
        1, m_computeConstantBuffer->GetGPUVirtualAddress() +
               static_cast<UINT64>(m_frameIndex) * m_computeConstantSize);
    m_commandList->Dispatch((ParticleCount + 255u) / 256u, 1, 1);

    D3D12_RESOURCE_BARRIER uavBarriers[2]{};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].UAV.pResource = m_particleBuffers[destinationParticleBuffer].Get();
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].UAV.pResource = m_particleCounters[destinationParticleBuffer].Get();
    m_commandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);

    D3D12_RESOURCE_BARRIER particleToRead{};
    particleToRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    particleToRead.Transition.pResource = m_particleBuffers[destinationParticleBuffer].Get();
    particleToRead.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    particleToRead.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    particleToRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &particleToRead);

    D3D12_RESOURCE_BARRIER renderTargetBarrier{};
    renderTargetBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    renderTargetBarrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    renderTargetBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    renderTargetBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    renderTargetBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &renderTargetBarrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
    const float clearColor[] = {0.008f, 0.014f, 0.030f, 1.0f};
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);
    m_commandList->SetGraphicsRootSignature(m_renderRootSignature.Get());
    m_commandList->SetDescriptorHeaps(1, descriptorHeaps);

    D3D12_GPU_DESCRIPTOR_HANDLE particleSrv = m_particleDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    particleSrv.ptr += static_cast<UINT64>(4 + destinationParticleBuffer) * m_particleDescriptorSize;
    m_commandList->SetGraphicsRootDescriptorTable(0, particleSrv);
    m_commandList->SetGraphicsRootConstantBufferView(
        1, m_renderConstantBuffer->GetGPUVirtualAddress() +
               static_cast<UINT64>(m_frameIndex) * m_renderConstantSize);

    m_commandList->SetPipelineState(m_platformPipelineState.Get());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(36, 1, 0, 0);

    m_commandList->SetPipelineState(m_renderPipelineState.Get());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    m_commandList->DrawInstanced(ParticleCount, 1, 0, 0);

    std::swap(particleToRead.Transition.StateBefore, particleToRead.Transition.StateAfter);
    m_commandList->ResourceBarrier(1, &particleToRead);
    std::swap(renderTargetBarrier.Transition.StateBefore, renderTargetBarrier.Transition.StateAfter);
    m_commandList->ResourceBarrier(1, &renderTargetBarrier);
    ThrowIfFailed(m_commandList->Close());

    ID3D12CommandList *commandLists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    ThrowIfFailed(m_swapChain->Present(1, 0));
    m_sourceParticleBuffer = destinationParticleBuffer;
}
// после каждого кадра Append- и Consume-буферы меняются ролями

void RenderingSystem::Render()
{
    if (!m_initialized)
        return;
    PopulateCommandList();
    MoveToNextFrame();
}

void RenderingSystem::WaitForGPU()
{
    const UINT64 fenceValue = m_fenceValues[m_frameIndex]++;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fenceValue));
    if (m_fence->GetCompletedValue() < fenceValue)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void RenderingSystem::MoveToNextFrame()
{
    const UINT64 currentFenceValue = m_fenceValues[m_frameIndex];
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), currentFenceValue));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

void RenderingSystem::Resize(UINT width, UINT height)
{
    if (!m_initialized || width == 0 || height == 0 || (width == m_width && height == m_height))
        return;

    WaitForGPU();
    m_width = width;
    m_height = height;
    for (UINT frame = 0; frame < FrameCount; ++frame)
    {
        m_renderTargets[frame].Reset();
        m_fenceValues[frame] = m_fenceValues[m_frameIndex];
    }

    DXGI_SWAP_CHAIN_DESC swapChainDescription{};
    ThrowIfFailed(m_swapChain->GetDesc(&swapChainDescription));
    ThrowIfFailed(m_swapChain->ResizeBuffers(FrameCount, width, height, swapChainDescription.BufferDesc.Format,
                                             swapChainDescription.Flags));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT frame = 0; frame < FrameCount; ++frame)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(frame, IID_PPV_ARGS(&m_renderTargets[frame])));
        m_device->CreateRenderTargetView(m_renderTargets[frame].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    m_depthBuffer.Reset();
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
    auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDescription,
                                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                                                    IID_PPV_ARGS(&m_depthBuffer)));
    m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr,
                                     m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    m_viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    m_scissorRect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    UpdateCamera(0.0f);
}

void RenderingSystem::Cleanup()
{
    if (!m_device)
        return;
    WaitForGPU();
    if (m_computeConstantBuffer && m_mappedComputeConstants)
    {
        m_computeConstantBuffer->Unmap(0, nullptr);
        m_mappedComputeConstants = nullptr;
    }
    if (m_renderConstantBuffer && m_mappedRenderConstants)
    {
        m_renderConstantBuffer->Unmap(0, nullptr);
        m_mappedRenderConstants = nullptr;
    }
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_initialized = false;
}
