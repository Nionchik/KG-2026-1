
#include "RenderingSystem.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <locale>
#include <codecvt>

using namespace DirectX;

static std::wstring Utf8ToWide(const std::string& utf8)
{
  if (utf8.empty()) return std::wstring();
  int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
  std::wstring wstr(size_needed, 0);
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wstr[0], size_needed);
  return wstr;
}

static XMFLOAT3 CalculateNormal(const XMFLOAT3& v0, const XMFLOAT3& v1, const XMFLOAT3& v2)
{
  XMFLOAT3 e1(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z);
  XMFLOAT3 e2(v2.x - v0.x, v2.y - v0.y, v2.z - v0.z);
  XMFLOAT3 n;
  n.x = e1.y * e2.z - e1.z * e2.y;
  n.y = e1.z * e2.x - e1.x * e2.z;
  n.z = e1.x * e2.y - e1.y * e2.x;
  float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
  if (len > 0) { n.x /= len; n.y /= len; n.z /= len; }
  return n;
}
RenderingSystem::RenderingSystem()
  : m_WorldMatrix(XMMatrixIdentity())
  , m_ViewMatrix(XMMatrixIdentity())
  , m_ProjectionMatrix(XMMatrixIdentity())
  , m_MinBounds(0, 0, 0)
  , m_MaxBounds(0, 0, 0)
  , m_Center(0, 0, 0)
  , m_Radius(1.0f)
  , m_CameraPosition(0, 0, 0)
  , m_CameraTarget(0, 0, 0)
  , m_CameraDistance(0)
  , m_CameraRotationX(0)
  , m_CameraRotationY(0)
  , m_RotationAngle(0)
{
}

RenderingSystem::~RenderingSystem()
{
  Cleanup();
}

bool RenderingSystem::Initialize(HWND hwnd, int width, int height)
{
  m_Width = width;
  m_Height = height;

  try
  {
    if (!InitializeDirect3D(hwnd)
      || !m_GBuffer.Initialize(m_Device.Get(), static_cast<UINT>(width), static_cast<UINT>(height))
      || !LoadShaders())
      throw std::runtime_error("Failed to initialize DirectX 12 pipeline");

    CreateWhiteDummyTexture();
    if (!LoadModel("sponza.obj"))
      throw std::runtime_error("Failed to load sponza.obj");
    // В качестве сцены загружается модель Sponza, указанная в задании.

    CreateConstantBuffers();

    SetupMatrices();

    m_Initialized = true;
    return true;
  }
  catch (const std::exception& e)
  {
    OutputDebugStringA(e.what());
    return false;
  }
}

bool RenderingSystem::InitializeDirect3D(HWND hwnd)
{
  HRESULT hr;

#ifdef _DEBUG
  ComPtr<ID3D12Debug> debugController;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    debugController->EnableDebugLayer();
#endif

  ComPtr<IDXGIFactory4> factory;
  hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) return false;

  hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device));
  if (FAILED(hr))
  {
    ComPtr<IDXGIAdapter> warpAdapter;
    factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
    hr = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device));
    if (FAILED(hr)) return false;
  }

  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  hr = m_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CommandQueue));
  if (FAILED(hr)) return false;

  DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
  swapChainDesc.BufferCount = FrameCount;
  swapChainDesc.BufferDesc.Width = m_Width;
  swapChainDesc.BufferDesc.Height = m_Height;
  swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swapChainDesc.OutputWindow = hwnd;
  swapChainDesc.SampleDesc.Count = 1;
  swapChainDesc.Windowed = TRUE;

  ComPtr<IDXGISwapChain> swapChain;
  hr = factory->CreateSwapChain(m_CommandQueue.Get(), &swapChainDesc, &swapChain);
  if (FAILED(hr)) return false;
  hr = swapChain.As(&m_SwapChain);
  if (FAILED(hr)) return false;

  m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
  rtvHeapDesc.NumDescriptors = FrameCount;
  rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  hr = m_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_RtvHeap));
  if (FAILED(hr)) return false;

  m_RtvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < FrameCount; i++)
  {
    hr = m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&m_RenderTargets[i]));
    if (FAILED(hr)) return false;
    m_Device->CreateRenderTargetView(m_RenderTargets[i].Get(), nullptr, rtvHandle);
    rtvHandle.ptr += m_RtvDescriptorSize;
  }

  D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
  dsvHeapDesc.NumDescriptors = 1;
  dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  hr = m_Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_DsvHeap));
  if (FAILED(hr)) return false;

  D3D12_RESOURCE_DESC depthDesc = {};
  depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  depthDesc.Width = m_Width;
  depthDesc.Height = m_Height;
  depthDesc.DepthOrArraySize = 1;
  depthDesc.MipLevels = 1;
  depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
  depthDesc.SampleDesc.Count = 1;
  depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  D3D12_CLEAR_VALUE depthClear = {};
  depthClear.Format = DXGI_FORMAT_D32_FLOAT;
  depthClear.DepthStencil.Depth = 1.0f;

  D3D12_HEAP_PROPERTIES defaultHeap = {};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

  hr = m_Device->CreateCommittedResource(
    &defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc,
    D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
    IID_PPV_ARGS(&m_DepthStencil));
  if (FAILED(hr)) return false;

  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
  dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
  dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  m_Device->CreateDepthStencilView(m_DepthStencil.Get(), &dsvDesc,
    m_DsvHeap->GetCPUDescriptorHandleForHeapStart());

  D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
  srvHeapDesc.NumDescriptors = MaxTextures;
  srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr = m_Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_SrvHeap));
  if (FAILED(hr)) return false;

  m_SrvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  for (UINT i = 0; i < FrameCount; i++)
  {
    hr = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_CommandAllocators[i]));
    if (FAILED(hr)) return false;
  }

  hr = m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
    m_CommandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_CommandList));
  if (FAILED(hr)) return false;
  m_CommandList->Close();

  m_Viewport = { 0.0f, 0.0f, (float)m_Width, (float)m_Height, 0.0f, 1.0f };
  m_ScissorRect = { 0, 0, m_Width, m_Height };

  hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence));
  if (FAILED(hr)) return false;
  m_FenceValues[0] = m_FenceValues[1] = 0;
  m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (!m_FenceEvent) return false;

  return true;
}

bool RenderingSystem::LoadShaders()
{
  ComPtr<ID3DBlob> geometryVs;
  ComPtr<ID3DBlob> geometryPs;
  ComPtr<ID3DBlob> lightingVs;
  ComPtr<ID3DBlob> lightingPs;
  ComPtr<ID3DBlob> errorBlob;
  UINT compileFlags = 0;
#ifdef _DEBUG
  compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

  auto compileShader = [&](const char* entryPoint, const char* target, ComPtr<ID3DBlob>& shader) -> bool
  {
    errorBlob.Reset();
    HRESULT result = D3DCompileFromFile(
      L"Shaders.hlsl",
      nullptr,
      D3D_COMPILE_STANDARD_FILE_INCLUDE,
      entryPoint,
      target,
      compileFlags,
      0,
      &shader,
      &errorBlob);
    if (FAILED(result) && errorBlob)
      OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
    return SUCCEEDED(result);
  };

  if (!compileShader("GeometryVS", "vs_5_0", geometryVs)
    || !compileShader("GeometryPS", "ps_5_0", geometryPs)
    || !compileShader("LightingVS", "vs_5_0", lightingVs)
    || !compileShader("LightingPS", "ps_5_0", lightingPs))
    return false;

  D3D12_DESCRIPTOR_RANGE materialSrvRange = {};
  materialSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  materialSrvRange.NumDescriptors = 1;
  materialSrvRange.BaseShaderRegister = 0;
  materialSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER geometryRootParameters[2] = {};
  geometryRootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  geometryRootParameters[0].Descriptor.ShaderRegister = 0;
  geometryRootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  geometryRootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  geometryRootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
  geometryRootParameters[1].DescriptorTable.pDescriptorRanges = &materialSrvRange;
  geometryRootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC materialSampler = {};
  materialSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  materialSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  materialSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  materialSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  materialSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  materialSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  materialSampler.MinLOD = 0;
  materialSampler.MaxLOD = D3D12_FLOAT32_MAX;
  materialSampler.ShaderRegister = 0;
  materialSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC geometryRootSignatureDesc = {};
  geometryRootSignatureDesc.NumParameters = _countof(geometryRootParameters);
  geometryRootSignatureDesc.pParameters = geometryRootParameters;
  geometryRootSignatureDesc.NumStaticSamplers = 1;
  geometryRootSignatureDesc.pStaticSamplers = &materialSampler;
  geometryRootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> signatureBlob, signatureErrorBlob;
  HRESULT hr = D3D12SerializeRootSignature(
    &geometryRootSignatureDesc,
    D3D_ROOT_SIGNATURE_VERSION_1,
    &signatureBlob,
    &signatureErrorBlob);
  if (FAILED(hr))
  {
    if (signatureErrorBlob) OutputDebugStringA((char*)signatureErrorBlob->GetBufferPointer());
    return false;
  }

  hr = m_Device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
    IID_PPV_ARGS(&m_GeometryRootSignature));
  if (FAILED(hr)) return false;

  D3D12_INPUT_ELEMENT_DESC inputLayout[] =
  {
      { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
      { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
      { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryPsoDesc = {};
  geometryPsoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
  geometryPsoDesc.pRootSignature = m_GeometryRootSignature.Get();
  geometryPsoDesc.VS = { geometryVs->GetBufferPointer(), geometryVs->GetBufferSize() };
  geometryPsoDesc.PS = { geometryPs->GetBufferPointer(), geometryPs->GetBufferSize() };

  D3D12_RASTERIZER_DESC rasterDesc = {};
  rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
  
  
  rasterDesc.CullMode = D3D12_CULL_MODE_NONE;
  rasterDesc.FrontCounterClockwise = FALSE;
  rasterDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
  rasterDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
  rasterDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
  rasterDesc.DepthClipEnable = TRUE;
  rasterDesc.MultisampleEnable = FALSE;
  rasterDesc.AntialiasedLineEnable = FALSE;
  rasterDesc.ForcedSampleCount = 0;
  rasterDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
  geometryPsoDesc.RasterizerState = rasterDesc;

  D3D12_BLEND_DESC blendDesc = {};
  blendDesc.AlphaToCoverageEnable = FALSE;
  blendDesc.IndependentBlendEnable = FALSE;
  for (UINT target = 0; target < GBuffer::TargetCount; ++target)
    blendDesc.RenderTarget[target].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  geometryPsoDesc.BlendState = blendDesc;

  D3D12_DEPTH_STENCIL_DESC dsDesc = {};
  dsDesc.DepthEnable = TRUE;
  dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  dsDesc.StencilEnable = FALSE;
  geometryPsoDesc.DepthStencilState = dsDesc;

  geometryPsoDesc.SampleMask = UINT_MAX;
  geometryPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  geometryPsoDesc.NumRenderTargets = GBuffer::TargetCount;
  geometryPsoDesc.RTVFormats[GBuffer::Albedo] = GBuffer::GetFormat(GBuffer::Albedo);
  geometryPsoDesc.RTVFormats[GBuffer::Normal] = GBuffer::GetFormat(GBuffer::Normal);
  geometryPsoDesc.RTVFormats[GBuffer::Position] = GBuffer::GetFormat(GBuffer::Position);
  geometryPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  geometryPsoDesc.SampleDesc.Count = 1;

  hr = m_Device->CreateGraphicsPipelineState(&geometryPsoDesc, IID_PPV_ARGS(&m_GeometryPipelineState));
  if (FAILED(hr)) return false;

  D3D12_DESCRIPTOR_RANGE gBufferSrvRange = {};
  gBufferSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  gBufferSrvRange.NumDescriptors = GBuffer::TargetCount;
  gBufferSrvRange.BaseShaderRegister = 0;
  gBufferSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER lightingRootParameters[2] = {};
  lightingRootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  lightingRootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
  lightingRootParameters[0].DescriptorTable.pDescriptorRanges = &gBufferSrvRange;
  lightingRootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  lightingRootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  lightingRootParameters[1].Descriptor.ShaderRegister = 0;
  lightingRootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC gBufferSampler = materialSampler;
  gBufferSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  gBufferSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  gBufferSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  gBufferSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

  D3D12_ROOT_SIGNATURE_DESC lightingRootSignatureDesc = {};
  lightingRootSignatureDesc.NumParameters = _countof(lightingRootParameters);
  lightingRootSignatureDesc.pParameters = lightingRootParameters;
  lightingRootSignatureDesc.NumStaticSamplers = 1;
  lightingRootSignatureDesc.pStaticSamplers = &gBufferSampler;

  signatureBlob.Reset();
  signatureErrorBlob.Reset();
  hr = D3D12SerializeRootSignature(
    &lightingRootSignatureDesc,
    D3D_ROOT_SIGNATURE_VERSION_1,
    &signatureBlob,
    &signatureErrorBlob);
  if (FAILED(hr))
  {
    if (signatureErrorBlob) OutputDebugStringA((char*)signatureErrorBlob->GetBufferPointer());
    return false;
  }

  hr = m_Device->CreateRootSignature(
    0,
    signatureBlob->GetBufferPointer(),
    signatureBlob->GetBufferSize(),
    IID_PPV_ARGS(&m_LightingRootSignature));
  if (FAILED(hr)) return false;

  D3D12_GRAPHICS_PIPELINE_STATE_DESC lightingPsoDesc = {};
  lightingPsoDesc.pRootSignature = m_LightingRootSignature.Get();
  lightingPsoDesc.VS = { lightingVs->GetBufferPointer(), lightingVs->GetBufferSize() };
  lightingPsoDesc.PS = { lightingPs->GetBufferPointer(), lightingPs->GetBufferSize() };
  lightingPsoDesc.RasterizerState = rasterDesc;
  lightingPsoDesc.BlendState = blendDesc;
  lightingPsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  lightingPsoDesc.DepthStencilState = {};
  lightingPsoDesc.DepthStencilState.DepthEnable = FALSE;
  lightingPsoDesc.DepthStencilState.StencilEnable = FALSE;
  lightingPsoDesc.SampleMask = UINT_MAX;
  lightingPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  lightingPsoDesc.NumRenderTargets = 1;
  lightingPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  lightingPsoDesc.SampleDesc.Count = 1;

  hr = m_Device->CreateGraphicsPipelineState(&lightingPsoDesc, IID_PPV_ARGS(&m_LightingPipelineState));
  if (FAILED(hr)) return false;

  return true;
}

bool RenderingSystem::LoadModel(const std::string& filename)
{
  std::ifstream file(filename);
  if (!file.is_open()) return false;

  std::vector<XMFLOAT3> positions;
  std::vector<XMFLOAT3> normals;
  std::vector<XMFLOAT2> texCoords;

  int currentMaterialIndex = -1;
  UINT currentSubsetStart = 0;
  std::string line;

  while (std::getline(file, line))
  {
    std::istringstream iss(line);
    std::string type;
    iss >> type;

    if (type == "v")
    {
      XMFLOAT3 p;
      iss >> p.x >> p.y >> p.z;
      positions.push_back(p);
    }
    else if (type == "vt")
    {
      XMFLOAT2 t;
      iss >> t.x >> t.y;
      texCoords.push_back(t);
    }
    else if (type == "vn")
    {
      XMFLOAT3 n;
      iss >> n.x >> n.y >> n.z;
      normals.push_back(n);
    }
    else if (type == "f")
    {
      struct FaceIndex { int p = -1; int t = -1; int n = -1; };
      std::vector<FaceIndex> face;
      std::string token;
      while (iss >> token)
      {
        FaceIndex index;
        size_t firstSlash = token.find('/');
        size_t secondSlash = firstSlash == std::string::npos ? std::string::npos : token.find('/', firstSlash + 1);

        auto parseIndex = [](const std::string& value, size_t count) -> int
          {
            if (value.empty()) return -1;
            int raw = std::stoi(value);
            return raw > 0 ? raw - 1 : static_cast<int>(count) + raw;
          };

        if (firstSlash == std::string::npos)
        {
          index.p = parseIndex(token, positions.size());
        }
        else
        {
          index.p = parseIndex(token.substr(0, firstSlash), positions.size());
          if (secondSlash == std::string::npos)
          {
            index.t = parseIndex(token.substr(firstSlash + 1), texCoords.size());
          }
          else
          {
            index.t = parseIndex(token.substr(firstSlash + 1, secondSlash - firstSlash - 1), texCoords.size());
            index.n = parseIndex(token.substr(secondSlash + 1), normals.size());
          }
        }
        face.push_back(index);
      }

      
      for (size_t triangle = 1; triangle + 1 < face.size(); ++triangle)
      {
        const FaceIndex triangleIndices[3] = { face[0], face[triangle], face[triangle + 1] };
        for (const FaceIndex& index : triangleIndices)
        {
          if (index.p < 0 || index.p >= static_cast<int>(positions.size()))
            return false;

          Vertex vert = {};
          vert.Position = positions[index.p];
          vert.Normal = index.n >= 0 && index.n < static_cast<int>(normals.size())
            ? normals[index.n] : XMFLOAT3(0, 1, 0);
          vert.TexCoord = index.t >= 0 && index.t < static_cast<int>(texCoords.size())
            ? texCoords[index.t] : XMFLOAT2(0, 0);

          m_Vertices.push_back(vert);
          m_Indices.push_back(static_cast<UINT>(m_Indices.size()));
        }
      }
    }
    else if (type == "mtllib")
    {
      std::string mtlFile;
      iss >> mtlFile;
      size_t pos = filename.find_last_of("/\\");
      std::string mtlPath = (pos == std::string::npos) ? mtlFile : filename.substr(0, pos + 1) + mtlFile;
      LoadMaterials(mtlPath);
    }
    else if (type == "usemtl")
    {
      std::string mtlName;
      iss >> mtlName;
      auto it = std::find_if(m_Materials.begin(), m_Materials.end(),
        [&](const Material& m) { return m.Name == mtlName; });
      if (it != m_Materials.end())
      {
        if (currentMaterialIndex != -1 && currentSubsetStart < (UINT)m_Indices.size())
        {
          Subset sub;
          sub.IndexStart = currentSubsetStart;
          sub.IndexCount = (UINT)m_Indices.size() - currentSubsetStart;
          sub.MaterialIndex = currentMaterialIndex;
          m_Subsets.push_back(sub);
        }
        currentMaterialIndex = (int)(it - m_Materials.begin());
        currentSubsetStart = (UINT)m_Indices.size();
      }
    }
  }

  file.close();

  if (currentMaterialIndex != -1 && currentSubsetStart < (UINT)m_Indices.size())
  {
    Subset sub;
    sub.IndexStart = currentSubsetStart;
    sub.IndexCount = (UINT)m_Indices.size() - currentSubsetStart;
    sub.MaterialIndex = currentMaterialIndex;
    m_Subsets.push_back(sub);
  }

  if (normals.empty())
  {
    for (size_t i = 0; i < m_Indices.size(); i += 3)
    {
      XMFLOAT3 n = CalculateNormal(
        m_Vertices[m_Indices[i]].Position,
        m_Vertices[m_Indices[i + 1]].Position,
        m_Vertices[m_Indices[i + 2]].Position);
      m_Vertices[m_Indices[i]].Normal = n;
      m_Vertices[m_Indices[i + 1]].Normal = n;
      m_Vertices[m_Indices[i + 2]].Normal = n;
    }
  }

  if (m_Materials.empty())
  {
    Material defaultMat;
    defaultMat.Name = "default";
    defaultMat.SrvIndex = -1;
    m_Materials.push_back(defaultMat);
  }

  if (m_Subsets.empty() && !m_Indices.empty())
  {
    Subset whole;
    whole.IndexStart = 0;
    whole.IndexCount = (UINT)m_Indices.size();
    whole.MaterialIndex = 0;
    m_Subsets.push_back(whole);
  }

  if (m_Subsets.size() > MaxSubsets)
    return false;

  UINT vbSize = (UINT)(m_Vertices.size() * sizeof(Vertex));
  UINT ibSize = (UINT)(m_Indices.size() * sizeof(UINT));

  D3D12_HEAP_PROPERTIES uploadHeap = {};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC vbDesc = {};
  vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  vbDesc.Width = vbSize;
  vbDesc.Height = 1;
  vbDesc.DepthOrArraySize = 1;
  vbDesc.MipLevels = 1;
  vbDesc.Format = DXGI_FORMAT_UNKNOWN;
  vbDesc.SampleDesc.Count = 1;
  vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  D3D12_RESOURCE_DESC ibDesc = vbDesc;
  ibDesc.Width = ibSize;

  ThrowIfFailed(m_Device->CreateCommittedResource(
    &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
    IID_PPV_ARGS(&m_VertexBuffer)));
  ThrowIfFailed(m_Device->CreateCommittedResource(
    &uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
    IID_PPV_ARGS(&m_IndexBuffer)));

  void* pData;
  m_VertexBuffer->Map(0, nullptr, &pData);
  memcpy(pData, m_Vertices.data(), vbSize);
  m_VertexBuffer->Unmap(0, nullptr);

  m_IndexBuffer->Map(0, nullptr, &pData);
  memcpy(pData, m_Indices.data(), ibSize);
  m_IndexBuffer->Unmap(0, nullptr);

  m_VBView.BufferLocation = m_VertexBuffer->GetGPUVirtualAddress();
  m_VBView.StrideInBytes = sizeof(Vertex);
  m_VBView.SizeInBytes = vbSize;

  m_IBView.BufferLocation = m_IndexBuffer->GetGPUVirtualAddress();
  m_IBView.Format = DXGI_FORMAT_R32_UINT;
  m_IBView.SizeInBytes = ibSize;

  if (!m_Vertices.empty())
  {
    m_MinBounds = m_MaxBounds = m_Vertices[0].Position;
    for (const auto& v : m_Vertices)
    {
      m_MinBounds.x = min(m_MinBounds.x, v.Position.x);
      m_MinBounds.y = min(m_MinBounds.y, v.Position.y);
      m_MinBounds.z = min(m_MinBounds.z, v.Position.z);
      m_MaxBounds.x = max(m_MaxBounds.x, v.Position.x);
      m_MaxBounds.y = max(m_MaxBounds.y, v.Position.y);
      m_MaxBounds.z = max(m_MaxBounds.z, v.Position.z);
    }
    m_Center.x = (m_MinBounds.x + m_MaxBounds.x) * 0.5f;
    m_Center.y = (m_MinBounds.y + m_MaxBounds.y) * 0.5f;
    m_Center.z = (m_MinBounds.z + m_MaxBounds.z) * 0.5f;
    m_Radius = 0;
    for (const auto& v : m_Vertices)
    {
      float dx = v.Position.x - m_Center.x;
      float dy = v.Position.y - m_Center.y;
      float dz = v.Position.z - m_Center.z;
      m_Radius = max(m_Radius, sqrtf(dx * dx + dy * dy + dz * dz));
    }
  }

  return true;
}

bool RenderingSystem::LoadMaterials(const std::string& mtlPath)
{
  std::ifstream file(mtlPath);
  if (!file.is_open()) return false;

  const size_t separator = mtlPath.find_last_of("/\\");
  const std::string materialDirectory = separator == std::string::npos
    ? std::string() : mtlPath.substr(0, separator + 1);

  std::string line;
  Material currentMat;
  bool inMaterial = false;

  auto finishMaterial = [&]()
    {
      if (!inMaterial) return;
      if (!currentMat.DiffuseTexture.empty())
      {
        int srvIndex = -1;
        const std::wstring fullPath = Utf8ToWide(materialDirectory + currentMat.DiffuseTexture);
        if (LoadTexture(fullPath, srvIndex))
          currentMat.SrvIndex = srvIndex;
        else
          OutputDebugStringW((L"Could not load material texture: " + fullPath + L"\n").c_str());
      }
      m_Materials.push_back(currentMat);
    };

  while (std::getline(file, line))
  {
    std::istringstream iss(line);
    std::string type;
    iss >> type;

    if (type == "newmtl")
    {
      finishMaterial();
      currentMat = Material();
      iss >> currentMat.Name;
      inMaterial = true;
    }
    else if (type == "Kd")
    {
      float r, g, b;
      iss >> r >> g >> b;
      currentMat.Diffuse = XMFLOAT4(r, g, b, 1.0f);
    }
    else if (type == "Ks")
    {
      float r, g, b;
      iss >> r >> g >> b;
      currentMat.Specular = XMFLOAT4(r, g, b, 1.0f);
    }
    else if (type == "Ns")
    {
      iss >> currentMat.Shininess;
    }
    else if (type == "map_Kd")
    {
      std::getline(iss >> std::ws, currentMat.DiffuseTexture);
    }
  }

  finishMaterial();
  return true;
}

bool RenderingSystem::LoadTexture(const std::wstring& path, int& outSrvIndex)
{
  const auto cached = m_TextureCache.find(path);
  if (cached != m_TextureCache.end())
  {
    outSrvIndex = cached->second;
    return true;
  }

  if (m_NextSrvIndex >= MaxTextures)
    return false;

  TextureLoader::TextureData texData;
  if (!TextureLoader::LoadFromFile(path, texData))
    return false;

  ComPtr<ID3D12CommandAllocator> tempAlloc;
  ComPtr<ID3D12GraphicsCommandList> tempList;
  ThrowIfFailed(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tempAlloc)));
  ThrowIfFailed(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tempAlloc.Get(), nullptr, IID_PPV_ARGS(&tempList)));

  ComPtr<ID3D12Resource> texture, uploadBuf;
  if (!TextureLoader::CreateTexture(m_Device.Get(), tempList.Get(), texData, texture, uploadBuf))
    return false;

  tempList->Close();

  ID3D12CommandList* lists[] = { tempList.Get() };
  m_CommandQueue->ExecuteCommandLists(1, lists);

  m_FenceValues[m_FrameIndex]++;
  ThrowIfFailed(m_CommandQueue->Signal(m_Fence.Get(), m_FenceValues[m_FrameIndex]));
  WaitForGPU();

  outSrvIndex = static_cast<int>(m_NextSrvIndex++);

  D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
  srvHandle.ptr += outSrvIndex * m_SrvDescriptorSize;

  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format = texData.format;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;

  m_Device->CreateShaderResourceView(texture.Get(), &srvDesc, srvHandle);

  m_TextureUploads.push_back(texture);
  m_TextureUploads.push_back(uploadBuf);
  m_TextureCache.emplace(path, outSrvIndex);

  return true;
}

void RenderingSystem::CreateWhiteDummyTexture()
{
  uint8_t whitePixel[4] = { 255,255,255,255 };
  D3D12_RESOURCE_DESC texDesc = {};
  texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texDesc.Width = 1;
  texDesc.Height = 1;
  texDesc.DepthOrArraySize = 1;
  texDesc.MipLevels = 1;
  texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texDesc.SampleDesc.Count = 1;

  D3D12_HEAP_PROPERTIES defaultHeap = {};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

  ComPtr<ID3D12Resource> texture;
  ThrowIfFailed(m_Device->CreateCommittedResource(
    &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
    IID_PPV_ARGS(&texture)));

  UINT64 uploadSize = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

  D3D12_HEAP_PROPERTIES uploadHeap = {};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC bufferDesc = {};
  bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufferDesc.Width = uploadSize;
  bufferDesc.Height = 1;
  bufferDesc.DepthOrArraySize = 1;
  bufferDesc.MipLevels = 1;
  bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
  bufferDesc.SampleDesc.Count = 1;
  bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ComPtr<ID3D12Resource> uploadBuffer;
  ThrowIfFailed(m_Device->CreateCommittedResource(
    &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
    IID_PPV_ARGS(&uploadBuffer)));

  void* mapped;
  uploadBuffer->Map(0, nullptr, &mapped);
  memcpy(mapped, whitePixel, uploadSize);
  uploadBuffer->Unmap(0, nullptr);

  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = uploadBuffer.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Offset = 0;
  src.PlacedFootprint.Footprint.Format = texDesc.Format;
  src.PlacedFootprint.Footprint.Width = 1;
  src.PlacedFootprint.Footprint.Height = 1;
  src.PlacedFootprint.Footprint.Depth = 1;
  src.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = texture.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;

  ComPtr<ID3D12CommandAllocator> tempAlloc;
  ComPtr<ID3D12GraphicsCommandList> tempList;
  ThrowIfFailed(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tempAlloc)));
  ThrowIfFailed(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tempAlloc.Get(), nullptr, IID_PPV_ARGS(&tempList)));

  tempList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = texture.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  tempList->ResourceBarrier(1, &barrier);
  tempList->Close();

  ID3D12CommandList* lists[] = { tempList.Get() };
  m_CommandQueue->ExecuteCommandLists(1, lists);

  
  m_FenceValues[m_FrameIndex]++;
  ThrowIfFailed(m_CommandQueue->Signal(m_Fence.Get(), m_FenceValues[m_FrameIndex]));
  WaitForGPU();

  D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;
  m_Device->CreateShaderResourceView(texture.Get(), &srvDesc, srvHandle);

  m_TextureUploads.push_back(texture);
  m_TextureUploads.push_back(uploadBuffer);
}

void RenderingSystem::CreateConstantBuffers()
{
  m_ConstantBufferSlotSize = (sizeof(ConstantBufferData) + 255) & ~255;
  UINT bufferSize = m_ConstantBufferSlotSize * MaxSubsets * FrameCount;

  D3D12_HEAP_PROPERTIES uploadHeap = {};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC bufferDesc = {};
  bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufferDesc.Width = bufferSize;
  bufferDesc.Height = 1;
  bufferDesc.DepthOrArraySize = 1;
  bufferDesc.MipLevels = 1;
  bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
  bufferDesc.SampleDesc.Count = 1;
  bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ThrowIfFailed(m_Device->CreateCommittedResource(
    &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
    IID_PPV_ARGS(&m_ConstantBuffer)));

  m_ConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_MappedConstantData));

  m_LightingConstantBufferSlotSize = (sizeof(LightingConstants) + 255) & ~255;
  bufferDesc.Width = static_cast<UINT64>(m_LightingConstantBufferSlotSize) * FrameCount;
  ThrowIfFailed(m_Device->CreateCommittedResource(
    &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
    IID_PPV_ARGS(&m_LightingConstantBuffer)));

  m_LightingConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_MappedLightingConstantData));
}

void RenderingSystem::SetupMatrices()
{
  m_WorldMatrix = XMMatrixIdentity();
  const float modelHeight = m_MaxBounds.y - m_MinBounds.y;
  m_CameraPosition = XMFLOAT3(
    m_Center.x,
    m_MinBounds.y + modelHeight * 0.24f,
    m_Center.z);
  m_CameraRotationY = XM_PIDIV2;
  UpdateCamera();
}

void RenderingSystem::SetCameraInput(float forward, float right, float turn, float vertical)
{
  m_CameraForwardInput = forward;
  m_CameraRightInput = right;
  m_CameraTurnInput = turn;
  m_CameraVerticalInput = vertical;
}

void RenderingSystem::UpdateCamera()
{
  const float deltaTime = min(m_Timer.GetDeltaTime(), 0.05f);
  const float movementSpeed = max(m_MaxBounds.x - m_MinBounds.x, m_MaxBounds.z - m_MinBounds.z) * 0.12f;
  m_CameraRotationY += m_CameraTurnInput * deltaTime * 1.6f;

  const XMVECTOR forward = XMVectorSet(sinf(m_CameraRotationY), 0.0f, cosf(m_CameraRotationY), 0.0f);
  const XMVECTOR right = XMVectorSet(cosf(m_CameraRotationY), 0.0f, -sinf(m_CameraRotationY), 0.0f);
  XMVECTOR position = XMLoadFloat3(&m_CameraPosition);
  position += forward * (m_CameraForwardInput * movementSpeed * deltaTime);
  position += right * (m_CameraRightInput * movementSpeed * deltaTime);
  position += XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f) * (m_CameraVerticalInput * movementSpeed * deltaTime);
  XMStoreFloat3(&m_CameraPosition, position);

  const float margin = 40.0f;
  m_CameraPosition.x = min(max(m_CameraPosition.x, m_MinBounds.x + margin), m_MaxBounds.x - margin);
  m_CameraPosition.y = min(max(m_CameraPosition.y, m_MinBounds.y + margin), m_MaxBounds.y - margin);
  m_CameraPosition.z = min(max(m_CameraPosition.z, m_MinBounds.z + margin), m_MaxBounds.z - margin);

  XMVECTOR eye = XMLoadFloat3(&m_CameraPosition);
  XMVECTOR up = XMVectorSet(0, 1, 0, 0);
  m_ViewMatrix = XMMatrixLookToLH(eye, forward, up);

  float aspect = (float)m_Width / (float)m_Height;
  m_ProjectionMatrix = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, m_Radius * 10.0f);
}

void RenderingSystem::UpdateLightingConstants()
{
  LightingConstants lights = {};
  lights.EyePosition = XMFLOAT4(m_CameraPosition.x, m_CameraPosition.y, m_CameraPosition.z, 1.0f);
  lights.DirectionalDirection = XMFLOAT4(0.35f, -1.0f, 0.25f, 0.0f);
  lights.DirectionalColorAndIntensity = XMFLOAT4(0.08f, 0.35f, 1.0f, 2.2f);
  lights.AmbientColor = XMFLOAT4(0.035f, 0.035f, 0.035f, 1.0f);
  // Эти параметры задают направленный источник света Directional.

  const float modelWidth = max(m_MaxBounds.x - m_MinBounds.x, 1.0f);
  const float modelHeight = max(m_MaxBounds.y - m_MinBounds.y, 1.0f);
  const float modelDepth = max(m_MaxBounds.z - m_MinBounds.z, 1.0f);
  lights.SpotPositionAndRange = XMFLOAT4(
    m_Center.x,
    m_MinBounds.y + modelHeight * 0.72f,
    m_Center.z,
    modelHeight * 0.85f);
  lights.SpotDirectionAndInnerCone = XMFLOAT4(0.0f, -1.0f, 0.0f, cosf(XMConvertToRadians(16.0f)));
  lights.SpotColorAndOuterCone = XMFLOAT4(0.25f, 1.0f, 0.35f, cosf(XMConvertToRadians(36.0f)));
  // Эти параметры размещают источник Spot внутри Sponza и задают его конус.

  constexpr UINT pointLightCount = 4;
  for (UINT index = 0; index < pointLightCount; ++index)
  {
    const float positionFactor = (static_cast<float>(index) + 0.5f) / static_cast<float>(pointLightCount);
    const float side = (index % 2 == 0) ? -0.18f : 0.18f;
    lights.PointLights[index].PositionAndRange = XMFLOAT4(
      m_MinBounds.x + modelWidth * positionFactor,
      m_MinBounds.y + modelHeight * 0.28f,
      m_Center.z + modelDepth * side,
      modelWidth * 0.10f);
    lights.PointLights[index].ColorAndIntensity = XMFLOAT4(
      1.0f,
      0.18f,
      0.12f,
      2.6f);
  }
  lights.PointLightInfo = XMFLOAT4(static_cast<float>(pointLightCount), 0.0f, 0.0f, 0.0f);
  // Четыре точечных источника равномерно распределяются внутри сцены.

  uint8_t* destination = m_MappedLightingConstantData
    + static_cast<size_t>(m_FrameIndex) * m_LightingConstantBufferSlotSize;
  memcpy(destination, &lights, sizeof(lights));
}

void RenderingSystem::PopulateCommandList()
{
  ThrowIfFailed(m_CommandAllocators[m_FrameIndex]->Reset());
  ThrowIfFailed(m_CommandList->Reset(m_CommandAllocators[m_FrameIndex].Get(), nullptr));

  m_Timer.Tick();
  m_WorldMatrix = XMMatrixIdentity();

  UpdateCamera();
  UpdateLightingConstants();

  XMMATRIX view = m_ViewMatrix;
  XMMATRIX proj = m_ProjectionMatrix;
  XMMATRIX wit = XMMatrixTranspose(XMMatrixInverse(nullptr, m_WorldMatrix));

  XMFLOAT3 eyePos = m_CameraPosition;
  XMFLOAT3 lightDir(0.3f, -1.0f, 0.5f);
  XMFLOAT4 lightColor(1, 1, 1, 1);
  XMFLOAT4 ambient(0.2f, 0.2f, 0.2f, 1.0f);

  for (size_t i = 0; i < m_Subsets.size(); ++i)
  {
    const Subset& sub = m_Subsets[i];
    int matIdx = (sub.MaterialIndex >= 0 && sub.MaterialIndex < (int)m_Materials.size()) ? sub.MaterialIndex : 0;
    const Material& mat = m_Materials[matIdx];

    UINT slot = m_FrameIndex * MaxSubsets + (UINT)i;
    UINT8* dest = m_MappedConstantData + slot * m_ConstantBufferSlotSize;

    ConstantBufferData cb = {};
    XMStoreFloat4x4(&cb.World, XMMatrixTranspose(m_WorldMatrix));
    XMStoreFloat4x4(&cb.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&cb.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&cb.WorldInvTranspose, XMMatrixTranspose(wit));
    cb.LightDir = XMFLOAT4(lightDir.x, lightDir.y, lightDir.z, 0.0f);
    cb.LightColor = lightColor;
    cb.AmbientColor = ambient;
    cb.EyePos = XMFLOAT4(eyePos.x, eyePos.y, eyePos.z, 1.0f);
    cb.MaterialDiffuse = mat.Diffuse;
    cb.MaterialSpecular = mat.Specular;
    cb.SpecularPower = mat.Shininess;
    cb.TotalTime = m_Timer.GetTotalTime();
    cb.TexTilingX = m_TexTiling.x;
    cb.TexTilingY = m_TexTiling.y;
    cb.TexScrollX = m_TexScroll.x;
    cb.TexScrollY = m_TexScroll.y;
    cb.HasTexture = (mat.SrvIndex >= 0) ? 1 : 0;

    memcpy(dest, &cb, sizeof(ConstantBufferData));
  }

  D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
  m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

  m_CommandList->RSSetViewports(1, &m_Viewport);
  m_CommandList->RSSetScissorRects(1, &m_ScissorRect);

  
  m_GBuffer.TransitionToRenderTargets(m_CommandList.Get());
  m_GBuffer.ClearAndBind(m_CommandList.Get(), dsvHandle);
  m_CommandList->SetPipelineState(m_GeometryPipelineState.Get());
  m_CommandList->SetGraphicsRootSignature(m_GeometryRootSignature.Get());

  ID3D12DescriptorHeap* materialHeaps[] = { m_SrvHeap.Get() };
  m_CommandList->SetDescriptorHeaps(1, materialHeaps);
  m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_CommandList->IASetVertexBuffers(0, 1, &m_VBView);
  m_CommandList->IASetIndexBuffer(&m_IBView);

  for (size_t i = 0; i < m_Subsets.size(); ++i)
  {
    const Subset& sub = m_Subsets[i];
    if (sub.IndexCount == 0) continue;

    int matIdx = (sub.MaterialIndex >= 0 && sub.MaterialIndex < (int)m_Materials.size()) ? sub.MaterialIndex : 0;
    const Material& mat = m_Materials[matIdx];

    UINT slot = m_FrameIndex * MaxSubsets + (UINT)i;
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_ConstantBuffer->GetGPUVirtualAddress() + slot * m_ConstantBufferSlotSize;
    m_CommandList->SetGraphicsRootConstantBufferView(0, cbAddr);

    int srvIdx = (mat.SrvIndex >= 0) ? mat.SrvIndex : 0;
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = m_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    srvHandle.ptr += srvIdx * m_SrvDescriptorSize;
    m_CommandList->SetGraphicsRootDescriptorTable(1, srvHandle);

    m_CommandList->DrawIndexedInstanced(sub.IndexCount, 1, sub.IndexStart, 0, 0);
  }
  // Первый проход записывает геометрию Sponza в GBuffer.

  
  m_GBuffer.TransitionToShaderResources(m_CommandList.Get());

  D3D12_RESOURCE_BARRIER toRenderTarget = {};
  toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toRenderTarget.Transition.pResource = m_RenderTargets[m_FrameIndex].Get();
  toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_CommandList->ResourceBarrier(1, &toRenderTarget);

  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtvHandle.ptr += static_cast<SIZE_T>(m_FrameIndex) * m_RtvDescriptorSize;
  m_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
  const float clearColor[] = { 0.025f, 0.04f, 0.075f, 1.0f };
  m_CommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

  m_CommandList->SetPipelineState(m_LightingPipelineState.Get());
  m_CommandList->SetGraphicsRootSignature(m_LightingRootSignature.Get());
  ID3D12DescriptorHeap* gBufferHeaps[] = { m_GBuffer.GetSrvHeap() };
  m_CommandList->SetDescriptorHeaps(1, gBufferHeaps);
  m_CommandList->SetGraphicsRootDescriptorTable(0, m_GBuffer.GetSrvStart());

  D3D12_GPU_VIRTUAL_ADDRESS lightingConstantsAddress = m_LightingConstantBuffer->GetGPUVirtualAddress()
    + static_cast<UINT64>(m_FrameIndex) * m_LightingConstantBufferSlotSize;
  m_CommandList->SetGraphicsRootConstantBufferView(1, lightingConstantsAddress);
  m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_CommandList->DrawInstanced(3, 1, 0, 0);
  // Второй проход читает GBuffer и рассчитывает итоговое освещение всего кадра.

  D3D12_RESOURCE_BARRIER toPresent = {};
  toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toPresent.Transition.pResource = m_RenderTargets[m_FrameIndex].Get();
  toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_CommandList->ResourceBarrier(1, &toPresent);

  ThrowIfFailed(m_CommandList->Close());
}

void RenderingSystem::Render()
{
  if (!m_Initialized) return;

  PopulateCommandList();

  ID3D12CommandList* lists[] = { m_CommandList.Get() };
  m_CommandQueue->ExecuteCommandLists(1, lists);

  ThrowIfFailed(m_SwapChain->Present(1, 0));

  MoveToNextFrame();
}

void RenderingSystem::WaitForGPU()
{
  const UINT64 fenceValue = m_FenceValues[m_FrameIndex];
  ThrowIfFailed(m_CommandQueue->Signal(m_Fence.Get(), fenceValue));
  if (m_Fence->GetCompletedValue() < fenceValue)
  {
    ThrowIfFailed(m_Fence->SetEventOnCompletion(fenceValue, m_FenceEvent));
    WaitForSingleObject(m_FenceEvent, INFINITE);
  }
}

void RenderingSystem::MoveToNextFrame()
{
  const UINT64 currentFence = m_FenceValues[m_FrameIndex];
  ThrowIfFailed(m_CommandQueue->Signal(m_Fence.Get(), currentFence));

  m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

  if (m_Fence->GetCompletedValue() < m_FenceValues[m_FrameIndex])
  {
    ThrowIfFailed(m_Fence->SetEventOnCompletion(m_FenceValues[m_FrameIndex], m_FenceEvent));
    WaitForSingleObject(m_FenceEvent, INFINITE);
  }

  m_FenceValues[m_FrameIndex] = currentFence + 1;
}

void RenderingSystem::Resize(int width, int height)
{
  if (!m_Initialized) return;
  if (width <= 0 || height <= 0) return;
  if (m_Width == width && m_Height == height) return;

  m_Width = width;
  m_Height = height;

  WaitForGPU();
  if (!m_GBuffer.Resize(static_cast<UINT>(width), static_cast<UINT>(height)))
    throw std::runtime_error("Failed to resize G-buffer");

  for (UINT i = 0; i < FrameCount; i++)
  {
    m_RenderTargets[i].Reset();
    m_FenceValues[i] = m_FenceValues[m_FrameIndex];
  }

  DXGI_SWAP_CHAIN_DESC desc;
  m_SwapChain->GetDesc(&desc);
  ThrowIfFailed(m_SwapChain->ResizeBuffers(FrameCount, width, height, desc.BufferDesc.Format, desc.Flags));

  m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < FrameCount; i++)
  {
    ThrowIfFailed(m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&m_RenderTargets[i])));
    m_Device->CreateRenderTargetView(m_RenderTargets[i].Get(), nullptr, rtvHandle);
    rtvHandle.ptr += m_RtvDescriptorSize;
  }

  m_DepthStencil.Reset();
  D3D12_RESOURCE_DESC depthDesc = {};
  depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  depthDesc.Width = width;
  depthDesc.Height = height;
  depthDesc.DepthOrArraySize = 1;
  depthDesc.MipLevels = 1;
  depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
  depthDesc.SampleDesc.Count = 1;
  depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  D3D12_CLEAR_VALUE depthClear = {};
  depthClear.Format = DXGI_FORMAT_D32_FLOAT;
  depthClear.DepthStencil.Depth = 1.0f;

  D3D12_HEAP_PROPERTIES defaultHeap = {};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

  ThrowIfFailed(m_Device->CreateCommittedResource(
    &defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc,
    D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
    IID_PPV_ARGS(&m_DepthStencil)));

  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
  dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
  dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  m_Device->CreateDepthStencilView(m_DepthStencil.Get(), &dsvDesc,
    m_DsvHeap->GetCPUDescriptorHandleForHeapStart());

  m_Viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
  m_ScissorRect = { 0, 0, width, height };

  UpdateCamera();
}

void RenderingSystem::Cleanup()
{
  if (!m_Initialized) return;

  WaitForGPU();

  if (m_FenceEvent)
  {
    CloseHandle(m_FenceEvent);
    m_FenceEvent = nullptr;
  }

  m_Initialized = false;
}
