#include "RenderingSystem.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
constexpr float CameraNear = 0.1f;
constexpr float CameraFar = 250.0f;
constexpr float CameraFov = XMConvertToRadians(60.0f);

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES result{};
    result.Type = type;
    result.CreationNodeMask = 1;
    result.VisibleNodeMask = 1;
    return result;
}

D3D12_RESOURCE_DESC BufferDescription(UINT64 size)
{
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Width = size;
    result.Height = 1;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return result;
}

std::vector<uint8_t> ReadFile(const std::wstring &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open asset file");
    const auto size = file.tellg();
    if (size <= 0)
        throw std::runtime_error("Empty asset file");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file)
        throw std::runtime_error("Cannot read asset file");
    return bytes;
}

UINT ReadUInt(const std::vector<uint8_t> &bytes, size_t offset)
{
    if (offset + sizeof(UINT) > bytes.size())
        throw std::runtime_error("Invalid asset header");
    UINT result;
    std::memcpy(&result, bytes.data() + offset, sizeof(result));
    return result;
}

struct TextureData
{
    UINT Width = 0;
    UINT Height = 0;
    UINT MipCount = 1;
    UINT ArraySize = 1;
    DXGI_FORMAT Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool Cube = false;
    std::vector<uint8_t> Pixels;
};

TextureData LoadJpeg(const wchar_t *path)
{
    ComPtr<IWICImagingFactory> factory;
    ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmapDecoder> decoder;
    ThrowIfFailed(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnLoad, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    ThrowIfFailed(decoder->GetFrame(0, &frame));
    ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(factory->CreateFormatConverter(&converter));
    ThrowIfFailed(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                        nullptr, 0, WICBitmapPaletteTypeCustom));
    TextureData result;
    ThrowIfFailed(converter->GetSize(&result.Width, &result.Height));
    result.Pixels.resize(static_cast<size_t>(result.Width) * result.Height * 4);
    ThrowIfFailed(converter->CopyPixels(nullptr, result.Width * 4, static_cast<UINT>(result.Pixels.size()),
                                        result.Pixels.data()));
    return result;
}

TextureData LoadDDS(const wchar_t *path)
{
    const auto bytes = ReadFile(path);
    if (bytes.size() < 148 || ReadUInt(bytes, 0) != 0x20534444 || ReadUInt(bytes, 84) != 0x30315844)
        throw std::runtime_error("Expected DDS with DX10 header");
    TextureData result;
    result.Width = ReadUInt(bytes, 16);
    result.Height = ReadUInt(bytes, 12);
    result.MipCount = (std::max)(ReadUInt(bytes, 28), 1u);
    result.Format = static_cast<DXGI_FORMAT>(ReadUInt(bytes, 128));
    result.Cube = (ReadUInt(bytes, 136) & 4) != 0;
    result.ArraySize = ReadUInt(bytes, 140) * (result.Cube ? 6 : 1);
    if (ReadUInt(bytes,132) != 3 || result.ArraySize != (result.Cube ? 6u : 1u) ||
        (result.Format != DXGI_FORMAT_BC6H_UF16 && result.Format != DXGI_FORMAT_R32G32_FLOAT))
        throw std::runtime_error("Unsupported supplied DDS format");
    result.Pixels.assign(bytes.begin() + 148, bytes.end());
    return result;
}

void FillRasterizer(D3D12_RASTERIZER_DESC &description)
{
    description.FillMode = D3D12_FILL_MODE_SOLID;
    description.CullMode = D3D12_CULL_MODE_NONE;
    description.DepthClipEnable = TRUE;
    description.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
}
}

static_assert(sizeof(Vertex) == 32, "Mesh vertex layout mismatch");

void RenderingSystem::Initialize(HWND windowHandle, UINT width, UINT height)
{
    m_windowHandle = windowHandle;
    m_width = (std::max)(width, 1u);
    m_height = (std::max)(height, 1u);
    InitializeDirect3D();
    LoadShaders();
    CreateAssets();
    CreateFrameResources();
    UpdateCamera(0);
    m_initialized = true;
    UpdateTitle();
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
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_device->CreateRenderTargetView(m_renderTargets[frame].Get(), &view, rtvHandle);
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
    ComPtr<ID3DBlob> vs, ps, quadVS, lightingPS, postPS;
    auto compile = [&](const char *entry, const char *target, ComPtr<ID3DBlob> &shader) {
        ComPtr<ID3DBlob> errors;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        const HRESULT hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                               entry, target, flags, 0, &shader, &errors);
        if (FAILED(hr) && errors)
            throw std::runtime_error(static_cast<const char *>(errors->GetBufferPointer()));
        ThrowIfFailed(hr);
    };
    compile("ModelVS", "vs_5_1", vs);
    compile("GBufferPS", "ps_5_1", ps);
    compile("FullscreenVS", "vs_5_1", quadVS);
    compile("LightingPS", "ps_5_1", lightingPS);
    compile("PostProcessPS", "ps_5_1", postPS);

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 11;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (UINT index = 0; index < 2; ++index)
    {
        auto &sampler = samplers[index];
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW =
            index == 0 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.ShaderRegister = index;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = 2;
    root.pParameters = parameters;
    root.NumStaticSamplers = 2;
    root.pStaticSamplers = samplers;
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized, errors;
    ThrowIfFailed(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    ThrowIfFailed(m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                IID_PPV_ARGS(&m_rootSignature)));

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = m_rootSignature.Get();
    pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pipeline.InputLayout = {layout, _countof(layout)};
    FillRasterizer(pipeline.RasterizerState);
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.DepthStencilState.DepthEnable = TRUE;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pipeline.SampleMask = UINT_MAX;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 3;
    for (UINT index=0; index<3; ++index)
        pipeline.RTVFormats[index] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipeline.BlendState.IndependentBlendEnable = TRUE;
    for (UINT index=1; index<3; ++index)
        pipeline.BlendState.RenderTarget[index] = pipeline.BlendState.RenderTarget[0];
    pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pipeline.SampleDesc.Count = 1;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_modelPipeline)));
    pipeline.VS = {quadVS->GetBufferPointer(),quadVS->GetBufferSize()};
    pipeline.PS = {lightingPS->GetBufferPointer(),lightingPS->GetBufferSize()};
    pipeline.InputLayout = {};
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipeline.RTVFormats[1] = pipeline.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&m_lightingPipeline)));
    pipeline.PS = {postPS->GetBufferPointer(),postPS->GetBufferSize()};
    pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&m_postPipeline)));
}

void RenderingSystem::CreateFrameResources()
{
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.NumDescriptors = 4;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&m_frameRtvHeap)));
    auto rtv = m_frameRtvHeap->GetCPUDescriptorHandleForHeapStart();
    auto srv = m_textureHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT srvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srv.ptr += static_cast<UINT64>(7)*srvSize;
    auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    for (UINT index=0; index<4; ++index)
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = m_width;
        description.Height = m_height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = description.Format;
        ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&description,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,&clear,IID_PPV_ARGS(&m_frameTextures[index])));
        m_device->CreateRenderTargetView(m_frameTextures[index].Get(),nullptr,rtv);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = description.Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_frameTextures[index].Get(),&view,srv);
        rtv.ptr += m_rtvDescriptorSize;
        srv.ptr += srvSize;
    }
    // Создаются три текстуры G-buffer и HDR-текстура результата освещения для постобработки.
}

void RenderingSystem::CreateAssets()
{
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.NumDescriptors = 11;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_textureHeap)));
    const UINT descriptorSize = m_device->GetDescriptorHandleIncrementSize(heap.Type);
    auto handle = m_textureHeap->GetCPUDescriptorHandleForHeapStart();
    auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    std::vector<ComPtr<ID3D12Resource>> uploads;
    ThrowIfFailed(m_commandAllocators[0]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[0].Get(), nullptr));

    auto uploadTexture = [&](UINT index, const TextureData &data) {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = data.Width;
        description.Height = data.Height;
        description.DepthOrArraySize = static_cast<UINT16>(data.ArraySize);
        description.MipLevels = static_cast<UINT16>(data.MipCount);
        description.Format = data.Format;
        description.SampleDesc.Count = 1;
        ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &description,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&m_textures[index])));
        const UINT subresourceCount = data.MipCount * data.ArraySize;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
        std::vector<UINT> rows(subresourceCount);
        std::vector<UINT64> rowSizes(subresourceCount);
        UINT64 uploadSize;
        m_device->GetCopyableFootprints(&description, 0, subresourceCount, 0, footprints.data(), rows.data(),
                                        rowSizes.data(), &uploadSize);
        auto uploadDescription = BufferDescription(uploadSize);
        ComPtr<ID3D12Resource> upload;
        ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&upload)));
        uint8_t *mapped = nullptr;
        ThrowIfFailed(upload->Map(0,nullptr,reinterpret_cast<void **>(&mapped)));
        size_t sourceOffset = 0;
        for (UINT sub = 0; sub < subresourceCount; ++sub)
        {
            const UINT mip = sub % data.MipCount;
            const UINT width = (std::max)(data.Width >> mip, 1u);
            const UINT height = (std::max)(data.Height >> mip, 1u);
            const UINT rowPitch = data.Format == DXGI_FORMAT_BC6H_UF16 ?
                (std::max)((width + 3) / 4, 1u) * 16 :
                width * (data.Format == DXGI_FORMAT_R32G32_FLOAT ? 8 : 4);
            const UINT rowCount = data.Format == DXGI_FORMAT_BC6H_UF16 ?
                (std::max)((height + 3) / 4, 1u) : height;
            const size_t sourceSize = static_cast<size_t>(rowPitch) * rowCount;
            if (rowCount != rows[sub] || rowPitch != rowSizes[sub] ||
                sourceOffset + sourceSize > data.Pixels.size())
                throw std::runtime_error("Invalid DDS subresource layout");
            for (UINT row = 0; row < rowCount; ++row)
                std::memcpy(mapped + footprints[sub].Offset +
                                static_cast<size_t>(row) * footprints[sub].Footprint.RowPitch,
                            data.Pixels.data() + sourceOffset + static_cast<size_t>(row) * rowPitch, rowPitch);
            sourceOffset += sourceSize;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[sub];
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = m_textures[index].Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = sub;
            m_commandList->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        }
        upload->Unmap(0,nullptr);
        uploads.push_back(upload);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_textures[index].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1,&barrier);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = index == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : data.Format;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = data.Cube ? D3D12_SRV_DIMENSION_TEXTURECUBE : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (data.Cube)
            srv.TextureCube.MipLevels = data.MipCount;
        else
            srv.Texture2D.MipLevels = data.MipCount;
        m_device->CreateShaderResourceView(m_textures[index].Get(),&srv,handle);
        handle.ptr += descriptorSize;
    };

    uploadTexture(0,LoadJpeg(L"Assets/Cerberus_A.jpg"));
    uploadTexture(1,LoadJpeg(L"Assets/Cerberus_N.jpg"));
    uploadTexture(2,LoadJpeg(L"Assets/Cerberus_M.jpg"));
    uploadTexture(3,LoadJpeg(L"Assets/Cerberus_R.jpg"));
    uploadTexture(4,LoadDDS(L"Assets/IrradianceMap_BC6U.dds"));
    uploadTexture(5,LoadDDS(L"Assets/IntegrationMap.dds"));
    const auto prefiltered = LoadDDS(L"Assets/PreFilteredEnvMap_BC6U.dds");
    uploadTexture(6,prefiltered);
    m_prefilterMipCount = prefiltered.MipCount;

    const auto mesh = ReadFile(L"Assets/Cerberus.mesh");
    m_vertexCount = ReadUInt(mesh,0);
    const UINT64 vertexSize = static_cast<UINT64>(m_vertexCount) * sizeof(Vertex);
    if (m_vertexCount == 0 || vertexSize + 4 != mesh.size() || vertexSize > UINT_MAX)
        throw std::runtime_error("Invalid Cerberus mesh");
    auto vertexDescription = BufferDescription(vertexSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &vertexDescription,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&m_vertexBuffer)));
    ComPtr<ID3D12Resource> vertexUpload;
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vertexDescription,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&vertexUpload)));
    void *mapped = nullptr;
    ThrowIfFailed(vertexUpload->Map(0,nullptr,&mapped));
    std::memcpy(mapped,mesh.data()+4,static_cast<size_t>(vertexSize));
    vertexUpload->Unmap(0,nullptr);
    m_commandList->CopyBufferRegion(m_vertexBuffer.Get(),0,vertexUpload.Get(),0,vertexSize);
    D3D12_RESOURCE_BARRIER vertexBarrier{};
    vertexBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    vertexBarrier.Transition.pResource = m_vertexBuffer.Get();
    vertexBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    vertexBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    vertexBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1,&vertexBarrier);
    m_vertexView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexView.SizeInBytes = static_cast<UINT>(vertexSize);
    m_vertexView.StrideInBytes = sizeof(Vertex);
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList *lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1,lists);
    WaitForGPU();

    m_constantSize = (sizeof(SceneConstants) + 255u) & ~255u;
    auto constantDescription = BufferDescription(static_cast<UINT64>(m_constantSize) * FrameCount);
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &constantDescription,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&m_constantBuffer)));
    ThrowIfFailed(m_constantBuffer->Map(0,nullptr,reinterpret_cast<void **>(&m_mappedConstants)));
}

void RenderingSystem::UpdateTitle()
{
    std::wstring title = L"Lab12 | IBL: ";
    title += m_iblEnabled ? L"ON" : L"OFF";
    title += L" | 1 Vignette: ";
    title += m_vignetteEnabled ? L"ON" : L"OFF";
    title += L" | 2 Chromatic: ";
    title += m_chromaticEnabled ? L"ON" : L"OFF";
    title += L" | 3 Blur 10x30: ";
    title += m_gaussianBlurEnabled ? L"ON" : L"OFF";
    SetWindowTextW(m_windowHandle,title.c_str());
}

void RenderingSystem::ToggleIBL()
{
    m_iblEnabled = !m_iblEnabled;
    UpdateTitle();
}

void RenderingSystem::ToggleVignette()
{
    m_vignetteEnabled = !m_vignetteEnabled;
    UpdateTitle();
}

void RenderingSystem::ToggleChromaticAberration()
{
    m_chromaticEnabled = !m_chromaticEnabled;
    UpdateTitle();
}

void RenderingSystem::ToggleGaussianBlur()
{
    m_gaussianBlurEnabled = !m_gaussianBlurEnabled;
    UpdateTitle();
}

void RenderingSystem::SetCameraInput(float forward, float right, float turn, float vertical)
{
    m_moveForward = std::clamp(forward,-1.0f,1.0f);
    m_moveRight = std::clamp(right,-1.0f,1.0f);
    m_moveTurn = std::clamp(turn,-1.0f,1.0f);
    m_moveVertical = std::clamp(vertical,-1.0f,1.0f);
}

void RenderingSystem::UpdateCamera(float deltaTime)
{
    const float speed = 3.0f;
    deltaTime = (std::min)(deltaTime,0.05f);
    m_cameraRotationY += m_moveTurn * deltaTime * 1.6f;
    const XMVECTOR forward = XMVectorSet(std::sin(m_cameraRotationY),0,std::cos(m_cameraRotationY),0);
    const XMVECTOR right = XMVectorSet(std::cos(m_cameraRotationY),0,-std::sin(m_cameraRotationY),0);
    XMVECTOR position = XMLoadFloat3(&m_cameraPosition);
    position += forward * (m_moveForward * speed * deltaTime);
    position += right * (m_moveRight * speed * deltaTime);
    position += XMVectorSet(0,1,0,0) * (m_moveVertical * speed * deltaTime);
    XMStoreFloat3(&m_cameraPosition,position);
    m_view = XMMatrixLookToLH(position,forward,XMVectorSet(0,1,0,0));
    m_projection = XMMatrixPerspectiveFovLH(CameraFov,static_cast<float>(m_width)/m_height,CameraNear,CameraFar);
}

void RenderingSystem::PopulateCommandList()
{
    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(),nullptr));
    m_timer.Tick();
    UpdateCamera(m_timer.GetDeltaTime());
    const XMMATRIX viewProjection = m_view * m_projection;
    SceneConstants constants{};
    XMStoreFloat4x4(&constants.ViewProjection,XMMatrixTranspose(viewProjection));
    XMStoreFloat4x4(&constants.InverseViewProjection,
                    XMMatrixTranspose(XMMatrixInverse(nullptr,viewProjection)));
    constants.CameraPosition = {m_cameraPosition.x,m_cameraPosition.y,m_cameraPosition.z,1};
    constants.LightDirection = {-0.3f,0.8f,-0.6f,0};
    constants.LightColor = {3.0f,3.0f,3.0f,1};
    constants.Options = {m_iblEnabled ? 1.0f : 0.0f,static_cast<float>(m_prefilterMipCount-1),m_vignetteEnabled ? 1.0f : 0.0f,m_chromaticEnabled ? 1.0f : 0.0f};
    constants.BlurOptions = {m_gaussianBlurEnabled ? 1.0f : 0.0f,0.0f,0.0f,0.0f};
    std::memcpy(m_mappedConstants + static_cast<size_t>(m_frameIndex)*m_constantSize,
                &constants,sizeof(constants));


    auto transition = [&](ID3D12Resource *resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1,&barrier);
    };
    auto offscreen = m_frameRtvHeap->GetCPUDescriptorHandleForHeapStart();
    auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clear[] = {0,0,0,0};
    for (UINT index=0; index<3; ++index)
    {
        transition(m_frameTextures[index].Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto target = offscreen;
        target.ptr += static_cast<UINT64>(index)*m_rtvDescriptorSize;
        m_commandList->ClearRenderTargetView(target,clear,0,nullptr);
    }
    m_commandList->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
    m_commandList->RSSetViewports(1,&m_viewport);
    m_commandList->RSSetScissorRects(1,&m_scissorRect);
    ID3D12DescriptorHeap *heaps[] = {m_textureHeap.Get()};
    m_commandList->SetDescriptorHeaps(1,heaps);
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetGraphicsRootConstantBufferView(
        0,m_constantBuffer->GetGPUVirtualAddress()+static_cast<UINT64>(m_frameIndex)*m_constantSize);
    m_commandList->SetGraphicsRootDescriptorTable(1,m_textureHeap->GetGPUDescriptorHandleForHeapStart());
    m_commandList->OMSetRenderTargets(3,&offscreen,TRUE,&dsv);
    m_commandList->SetPipelineState(m_modelPipeline.Get());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->IASetVertexBuffers(0,1,&m_vertexView);
    m_commandList->DrawInstanced(m_vertexCount,1,0,0);
    // Геометрический проход записывает параметры модели в три MRT-текстуры G-buffer.

    for (UINT index=0; index<3; ++index)
        transition(m_frameTextures[index].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto hdr = offscreen;
    hdr.ptr += static_cast<UINT64>(3)*m_rtvDescriptorSize;
    transition(m_frameTextures[3].Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->OMSetRenderTargets(1,&hdr,FALSE,nullptr);
    m_commandList->SetPipelineState(m_lightingPipeline.Get());
    m_commandList->IASetVertexBuffers(0,1,nullptr);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_commandList->DrawInstanced(4,1,0,0);
    // Четыре вершины quad генерируются шейдером; освещение читает G-buffer без вершинного буфера.

    transition(m_frameTextures[3].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(m_renderTargets[m_frameIndex].Get(),D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<UINT64>(m_frameIndex)*m_rtvDescriptorSize;
    m_commandList->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    m_commandList->SetPipelineState(m_postPipeline.Get());
    m_commandList->DrawInstanced(4,1,0,0);
    // Постобработка читает HDR-текстуру, применяет два эффекта и выводит quad в sRGB back buffer.

    transition(m_renderTargets[m_frameIndex].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT);
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList *lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1,lists);
    ThrowIfFailed(m_swapChain->Present(1,0));
}

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
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_device->CreateRenderTargetView(m_renderTargets[frame].Get(), &view, rtvHandle);
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
    CreateFrameResources();
    UpdateCamera(0.0f);
}


void RenderingSystem::Cleanup()
{
    if (!m_device)
        return;
    WaitForGPU();
    if (m_constantBuffer && m_mappedConstants)
    {
        m_constantBuffer->Unmap(0,nullptr);
        m_mappedConstants = nullptr;
    }
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_initialized = false;
}
