#include "RenderingSystem.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

using namespace DirectX;

RenderingSystem::RenderingSystem()
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
    if (!InitializeDirect3D(hwnd) || !LoadShaders())
      throw std::runtime_error("Failed to initialize DirectX 12");

    CreateSphereMesh();
    CreateScene();
    CreateConstantBuffer();
    BuildOctree();
    UpdateCamera();
    UpdateTopCamera();
    m_Initialized = true;
    return true;
  }
  catch (const std::exception &error)
  {
    OutputDebugStringA(error.what());
    return false;
  }
}

bool RenderingSystem::InitializeDirect3D(HWND hwnd)
{
#ifdef _DEBUG
  ComPtr<ID3D12Debug> debugController;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    debugController->EnableDebugLayer();
#endif

  ComPtr<IDXGIFactory4> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    return false;

  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device))))
  {
    ComPtr<IDXGIAdapter> warpAdapter;
    if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter))) ||
        FAILED(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device))))
      return false;
  }

  D3D12_COMMAND_QUEUE_DESC queueDescription = {};
  queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(m_Device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_CommandQueue))))
    return false;

  DXGI_SWAP_CHAIN_DESC swapChainDescription = {};
  swapChainDescription.BufferCount = FrameCount;
  swapChainDescription.BufferDesc.Width = m_Width;
  swapChainDescription.BufferDesc.Height = m_Height;
  swapChainDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swapChainDescription.OutputWindow = hwnd;
  swapChainDescription.SampleDesc.Count = 1;
  swapChainDescription.Windowed = TRUE;

  ComPtr<IDXGISwapChain> swapChain;
  if (FAILED(factory->CreateSwapChain(m_CommandQueue.Get(), &swapChainDescription, &swapChain)) ||
      FAILED(swapChain.As(&m_SwapChain)))
    return false;

  m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription = {};
  rtvHeapDescription.NumDescriptors = FrameCount;
  rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  if (FAILED(m_Device->CreateDescriptorHeap(&rtvHeapDescription, IID_PPV_ARGS(&m_RtvHeap))))
    return false;

  m_RtvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (UINT frame = 0; frame < FrameCount; ++frame)
  {
    if (FAILED(m_SwapChain->GetBuffer(frame, IID_PPV_ARGS(&m_RenderTargets[frame]))))
      return false;
    m_Device->CreateRenderTargetView(m_RenderTargets[frame].Get(), nullptr, rtvHandle);
    rtvHandle.ptr += m_RtvDescriptorSize;
  }

  D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDescription = {};
  dsvHeapDescription.NumDescriptors = 1;
  dsvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  if (FAILED(m_Device->CreateDescriptorHeap(&dsvHeapDescription, IID_PPV_ARGS(&m_DsvHeap))))
    return false;

  D3D12_RESOURCE_DESC depthDescription = {};
  depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  depthDescription.Width = m_Width;
  depthDescription.Height = m_Height;
  depthDescription.DepthOrArraySize = 1;
  depthDescription.MipLevels = 1;
  depthDescription.Format = DXGI_FORMAT_D32_FLOAT;
  depthDescription.SampleDesc.Count = 1;
  depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  D3D12_CLEAR_VALUE depthClear = {};
  depthClear.Format = DXGI_FORMAT_D32_FLOAT;
  depthClear.DepthStencil.Depth = 1.0f;

  D3D12_HEAP_PROPERTIES defaultHeap = {};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
  if (FAILED(m_Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDescription,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                               IID_PPV_ARGS(&m_DepthStencil))))
    return false;

  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDescription = {};
  dsvDescription.Format = DXGI_FORMAT_D32_FLOAT;
  dsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  m_Device->CreateDepthStencilView(m_DepthStencil.Get(), &dsvDescription,
                                   m_DsvHeap->GetCPUDescriptorHandleForHeapStart());

  for (UINT frame = 0; frame < FrameCount; ++frame)
  {
    if (FAILED(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&m_CommandAllocators[frame]))))
      return false;
  }

  if (FAILED(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocators[0].Get(), nullptr,
                                         IID_PPV_ARGS(&m_CommandList))))
    return false;
  if (FAILED(m_CommandList->Close()))
    return false;

  if (FAILED(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence))))
    return false;
  m_FenceValues[0] = 1;
  m_FenceValues[1] = 1;
  m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (!m_FenceEvent)
    return false;

  m_Viewport = {0.0f, 0.0f, static_cast<float>(m_Width), static_cast<float>(m_Height), 0.0f, 1.0f};
  m_ScissorRect = {0, 0, m_Width, m_Height};
  return true;
}

bool RenderingSystem::LoadShaders()
{
  ComPtr<ID3DBlob> vertexShader;
  ComPtr<ID3DBlob> pixelShader;
  ComPtr<ID3DBlob> errors;
  UINT flags = 0;
#ifdef _DEBUG
  flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

  HRESULT result = D3DCompileFromFile(L"Shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0",
                                      flags, 0, &vertexShader, &errors);
  if (FAILED(result))
  {
    if (errors)
      OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
    return false;
  }

  errors.Reset();
  result = D3DCompileFromFile(L"Shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", flags, 0,
                              &pixelShader, &errors);
  if (FAILED(result))
  {
    if (errors)
      OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
    return false;
  }

  D3D12_ROOT_PARAMETER rootParameter = {};
  rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  rootParameter.Descriptor.ShaderRegister = 0;
  rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription = {};
  rootSignatureDescription.NumParameters = 1;
  rootSignatureDescription.pParameters = &rootParameter;
  rootSignatureDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                   D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                                   D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                                   D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

  ComPtr<ID3DBlob> signature;
  ComPtr<ID3DBlob> signatureErrors;
  if (FAILED(D3D12SerializeRootSignature(&rootSignatureDescription, D3D_ROOT_SIGNATURE_VERSION_1, &signature,
                                         &signatureErrors)))
    return false;
  if (FAILED(m_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                           IID_PPV_ARGS(&m_RootSignature))))
    return false;

  D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

  D3D12_RASTERIZER_DESC rasterizer = {};
  rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
  rasterizer.CullMode = D3D12_CULL_MODE_NONE;
  rasterizer.DepthClipEnable = TRUE;

  D3D12_BLEND_DESC blend = {};
  blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

  D3D12_DEPTH_STENCIL_DESC depthStencil = {};
  depthStencil.DepthEnable = TRUE;
  depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription = {};
  pipelineDescription.InputLayout = {inputLayout, _countof(inputLayout)};
  pipelineDescription.pRootSignature = m_RootSignature.Get();
  pipelineDescription.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
  pipelineDescription.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
  pipelineDescription.RasterizerState = rasterizer;
  pipelineDescription.BlendState = blend;
  pipelineDescription.DepthStencilState = depthStencil;
  pipelineDescription.SampleMask = UINT_MAX;
  pipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipelineDescription.NumRenderTargets = 1;
  pipelineDescription.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pipelineDescription.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  pipelineDescription.SampleDesc.Count = 1;

  return SUCCEEDED(m_Device->CreateGraphicsPipelineState(&pipelineDescription, IID_PPV_ARGS(&m_PipelineState)));
}

void RenderingSystem::CreateSphereMesh()
{
  constexpr UINT latitudeSegments = 12;
  constexpr UINT longitudeSegments = 18;

  for (UINT latitude = 0; latitude <= latitudeSegments; ++latitude)
  {
    const float phi = XM_PI * static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
    const float y = cosf(phi);
    const float ringRadius = sinf(phi);
    for (UINT longitude = 0; longitude <= longitudeSegments; ++longitude)
    {
      const float theta = XM_2PI * static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
      const XMFLOAT3 normal(ringRadius * cosf(theta), y, ringRadius * sinf(theta));
      m_Vertices.push_back({normal, normal});
    }
  }

  for (UINT latitude = 0; latitude < latitudeSegments; ++latitude)
  {
    for (UINT longitude = 0; longitude < longitudeSegments; ++longitude)
    {
      const UINT first = latitude * (longitudeSegments + 1) + longitude;
      const UINT second = first + longitudeSegments + 1;
      m_Indices.push_back(first);
      m_Indices.push_back(second);
      m_Indices.push_back(first + 1);
      m_Indices.push_back(first + 1);
      m_Indices.push_back(second);
      m_Indices.push_back(second + 1);
    }
  }

  const UINT vertexBufferSize = static_cast<UINT>(m_Vertices.size() * sizeof(Vertex));
  const UINT indexBufferSize = static_cast<UINT>(m_Indices.size() * sizeof(UINT));
  D3D12_HEAP_PROPERTIES uploadHeap = {};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC bufferDescription = {};
  bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufferDescription.Width = vertexBufferSize;
  bufferDescription.Height = 1;
  bufferDescription.DepthOrArraySize = 1;
  bufferDescription.MipLevels = 1;
  bufferDescription.SampleDesc.Count = 1;
  bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ThrowIfFailed(m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&m_VertexBuffer)));

  bufferDescription.Width = indexBufferSize;
  ThrowIfFailed(m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&m_IndexBuffer)));

  void *mappedData = nullptr;
  ThrowIfFailed(m_VertexBuffer->Map(0, nullptr, &mappedData));
  memcpy(mappedData, m_Vertices.data(), vertexBufferSize);
  m_VertexBuffer->Unmap(0, nullptr);
  ThrowIfFailed(m_IndexBuffer->Map(0, nullptr, &mappedData));
  memcpy(mappedData, m_Indices.data(), indexBufferSize);
  m_IndexBuffer->Unmap(0, nullptr);

  m_VertexBufferView.BufferLocation = m_VertexBuffer->GetGPUVirtualAddress();
  m_VertexBufferView.StrideInBytes = sizeof(Vertex);
  m_VertexBufferView.SizeInBytes = vertexBufferSize;
  m_IndexBufferView.BufferLocation = m_IndexBuffer->GetGPUVirtualAddress();
  m_IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
  m_IndexBufferView.SizeInBytes = indexBufferSize;
}

void RenderingSystem::CreateScene()
{
  std::mt19937 generator(2026);
  std::uniform_real_distribution<float> xDistribution(-100.0f, 100.0f);
  std::uniform_real_distribution<float> yDistribution(-35.0f, 35.0f);
  std::uniform_real_distribution<float> zDistribution(0.0f, 220.0f);
  std::uniform_real_distribution<float> radiusDistribution(0.6f, 1.6f);
  std::uniform_real_distribution<float> colorDistribution(0.25f, 1.0f);
  m_Objects.reserve(ObjectCount);

  for (UINT index = 0; index < ObjectCount; ++index)
  {
    SceneObject object = {};
    object.Position = XMFLOAT3(xDistribution(generator), yDistribution(generator), zDistribution(generator));
    object.Radius = radiusDistribution(generator);
    object.Color =
        XMFLOAT4(colorDistribution(generator), colorDistribution(generator), colorDistribution(generator), 1.0f);
    object.Bounds = BoundingSphere(object.Position, object.Radius);
    m_Objects.push_back(object);
  }

  // На сцене создаются 3000 объектов со случайными положениями и ограничивающими сферами.
}

void RenderingSystem::CreateConstantBuffer()
{
  m_ConstantBufferStride = (sizeof(ConstantBufferData) + 255) & ~255;
  D3D12_HEAP_PROPERTIES uploadHeap = {};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC bufferDescription = {};
  bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  // Два набора констант на кадр: основной ракурс и верхняя демонстрационная камера.
  bufferDescription.Width = static_cast<UINT64>(m_ConstantBufferStride) * ObjectCount * 2 * FrameCount;
  bufferDescription.Height = 1;
  bufferDescription.DepthOrArraySize = 1;
  bufferDescription.MipLevels = 1;
  bufferDescription.SampleDesc.Count = 1;
  bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ThrowIfFailed(m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&m_ConstantBuffer)));
  ThrowIfFailed(m_ConstantBuffer->Map(0, nullptr, reinterpret_cast<void **>(&m_MappedConstantData)));
}

void RenderingSystem::BuildOctree()
{
  m_OctreeRoot = std::make_unique<OctreeNode>();
  m_OctreeRoot->Bounds = BoundingBox(XMFLOAT3(0.0f, 0.0f, 110.0f), XMFLOAT3(105.0f, 40.0f, 115.0f));
  m_OctreeRoot->ObjectIndices.resize(m_Objects.size());
  std::iota(m_OctreeRoot->ObjectIndices.begin(), m_OctreeRoot->ObjectIndices.end(), 0u);
  SubdivideOctree(*m_OctreeRoot, 0);
}

void RenderingSystem::SubdivideOctree(OctreeNode &node, UINT depth)
{
  if (depth >= 6 || node.ObjectIndices.size() <= 24)
    return;

  const XMFLOAT3 childExtents(node.Bounds.Extents.x * 0.5f, node.Bounds.Extents.y * 0.5f, node.Bounds.Extents.z * 0.5f);
  std::array<std::vector<UINT>, 8> childObjects;
  std::vector<UINT> remainingObjects;

  for (UINT objectIndex : node.ObjectIndices)
  {
    const SceneObject &object = m_Objects[objectIndex];
    int selectedChild = -1;
    for (int childIndex = 0; childIndex < 8; ++childIndex)
    {
      const XMFLOAT3 childCenter(node.Bounds.Center.x + ((childIndex & 1) ? childExtents.x : -childExtents.x),
                                 node.Bounds.Center.y + ((childIndex & 2) ? childExtents.y : -childExtents.y),
                                 node.Bounds.Center.z + ((childIndex & 4) ? childExtents.z : -childExtents.z));
      if (fabsf(object.Position.x - childCenter.x) + object.Radius <= childExtents.x &&
          fabsf(object.Position.y - childCenter.y) + object.Radius <= childExtents.y &&
          fabsf(object.Position.z - childCenter.z) + object.Radius <= childExtents.z)
      {
        selectedChild = childIndex;
        break;
      }
    }

    if (selectedChild >= 0)
      childObjects[selectedChild].push_back(objectIndex);
    else
      remainingObjects.push_back(objectIndex);
  }

  node.ObjectIndices = std::move(remainingObjects);
  for (int childIndex = 0; childIndex < 8; ++childIndex)
  {
    if (childObjects[childIndex].empty())
      continue;
    node.Children[childIndex] = std::make_unique<OctreeNode>();
    node.Children[childIndex]->Bounds =
        BoundingBox(XMFLOAT3(node.Bounds.Center.x + ((childIndex & 1) ? childExtents.x : -childExtents.x),
                             node.Bounds.Center.y + ((childIndex & 2) ? childExtents.y : -childExtents.y),
                             node.Bounds.Center.z + ((childIndex & 4) ? childExtents.z : -childExtents.z)),
                    childExtents);
    node.Children[childIndex]->ObjectIndices = std::move(childObjects[childIndex]);
    SubdivideOctree(*node.Children[childIndex], depth + 1);
  }
}

// Октодерево группирует ограничивающие сферы объектов по положению в сцене.

void RenderingSystem::AddNodeObjects(const OctreeNode &node, std::vector<UINT> &output) const
{
  output.insert(output.end(), node.ObjectIndices.begin(), node.ObjectIndices.end());
  for (const auto &child : node.Children)
  {
    if (child)
      AddNodeObjects(*child, output);
  }
}

void RenderingSystem::QueryOctree(const OctreeNode &node, const BoundingFrustum &frustum,
                                  std::vector<UINT> &output) const
{
  const ContainmentType relation = frustum.Contains(node.Bounds);
  if (relation == DISJOINT)
    return;
  if (relation == CONTAINS)
  {
    AddNodeObjects(node, output);
    return;
  }

  for (UINT objectIndex : node.ObjectIndices)
  {
    if (frustum.Intersects(m_Objects[objectIndex].Bounds))
      output.push_back(objectIndex);
  }
  for (const auto &child : node.Children)
  {
    if (child)
      QueryOctree(*child, frustum, output);
  }
}

// Проверка узла позволяет отбросить целую невидимую группу объектов одним тестом.

void RenderingSystem::CollectVisibleObjects()
{
  m_VisibleObjects.clear();
  if (m_CullingMode == 0)
  {
    m_VisibleObjects.resize(m_Objects.size());
    std::iota(m_VisibleObjects.begin(), m_VisibleObjects.end(), 0u);
    return;
  }

  BoundingFrustum viewFrustum;
  BoundingFrustum::CreateFromMatrix(viewFrustum, m_ProjectionMatrix);
  BoundingFrustum worldFrustum;
  viewFrustum.Transform(worldFrustum, XMMatrixInverse(nullptr, m_ViewMatrix));

  if (m_CullingMode == 1)
  {
    for (UINT index = 0; index < m_Objects.size(); ++index)
    {
      if (worldFrustum.Intersects(m_Objects[index].Bounds))
        m_VisibleObjects.push_back(index);
    }
  }
  else if (m_OctreeRoot)
  {
    QueryOctree(*m_OctreeRoot, worldFrustum, m_VisibleObjects);
  }
}

// Frustum culling оставляет для отрисовки только сферы внутри области видимости камеры.

void RenderingSystem::SetCameraInput(float forward, float right, float turn, float vertical)
{
  m_ForwardMovement = std::clamp(forward, -1.0f, 1.0f);
  m_RightMovement = std::clamp(right, -1.0f, 1.0f);
  m_TurnMovement = std::clamp(turn, -1.0f, 1.0f);
  m_VerticalMovement = std::clamp(vertical, -1.0f, 1.0f);
}

void RenderingSystem::SetCullingMode(int mode)
{
  m_CullingMode = std::clamp(mode, 0, 2);
}

void RenderingSystem::UpdateCamera()
{
  const float speed = 30.0f;
  const float deltaTime = min(m_Timer.GetDeltaTime(), 0.05f);
  m_CameraRotationY += m_TurnMovement * deltaTime * 1.6f;

  const XMVECTOR forward = XMVectorSet(sinf(m_CameraRotationY), 0.0f, cosf(m_CameraRotationY), 0.0f);
  const XMVECTOR right = XMVectorSet(cosf(m_CameraRotationY), 0.0f, -sinf(m_CameraRotationY), 0.0f);
  XMVECTOR position = XMLoadFloat3(&m_CameraPosition);
  position += forward * (m_ForwardMovement * speed * deltaTime);
  position += right * (m_RightMovement * speed * deltaTime);
  position += XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f) * (m_VerticalMovement * speed * deltaTime);
  XMStoreFloat3(&m_CameraPosition, position);

  m_CameraPosition.x = std::clamp(m_CameraPosition.x, -100.0f, 100.0f);
  m_CameraPosition.y = std::clamp(m_CameraPosition.y, -40.0f, 40.0f);
  m_CameraPosition.z = std::clamp(m_CameraPosition.z, -40.0f, 210.0f);

  const XMVECTOR eye = XMLoadFloat3(&m_CameraPosition);
  m_ViewMatrix = XMMatrixLookToLH(eye, forward, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
  const float aspect = static_cast<float>(m_Width) / static_cast<float>(m_Height);
  m_ProjectionMatrix = XMMatrixPerspectiveFovLH(XMConvertToRadians(65.0f), aspect, 0.1f, 400.0f);
}

void RenderingSystem::UpdateTopCamera()
{
  // Статичная камера расположена над серединой сцены и не участвует в culling.
  // Она только визуализирует список объектов, отобранный основной камерой.
  const XMVECTOR eye = XMVectorSet(0.0f, 300.0f, 110.0f, 1.0f);
  const XMVECTOR target = XMVectorSet(0.0f, 0.0f, 110.0f, 1.0f);
  const XMVECTOR up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
  m_TopViewMatrix = XMMatrixLookAtLH(eye, target, up);
  m_TopProjectionMatrix = XMMatrixOrthographicLH(230.0f, 260.0f, 0.1f, 500.0f);

  const float insetWidth = min(420.0f, max(250.0f, m_Width * 0.34f));
  const float insetHeight = insetWidth * 0.70f;
  const float insetLeft = max(12.0f, static_cast<float>(m_Width) - insetWidth - 18.0f);
  m_TopViewport = { insetLeft, 18.0f, insetWidth, insetHeight, 0.0f, 1.0f };
  m_TopScissorRect = {
    static_cast<LONG>(insetLeft), 18,
    static_cast<LONG>(insetLeft + insetWidth), static_cast<LONG>(18.0f + insetHeight)
  };
}

void RenderingSystem::DrawVisibleObjects(const XMMATRIX& viewProjection, UINT passIndex)
{
  const UINT frameOffset = (m_FrameIndex * 2 + passIndex) * ObjectCount;
  for (UINT drawIndex = 0; drawIndex < m_VisibleObjects.size(); ++drawIndex)
  {
    const SceneObject &object = m_Objects[m_VisibleObjects[drawIndex]];
    const XMMATRIX world = XMMatrixScaling(object.Radius, object.Radius, object.Radius) *
                           XMMatrixTranslation(object.Position.x, object.Position.y, object.Position.z);
    ConstantBufferData constants = {};
    XMStoreFloat4x4(&constants.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&constants.ViewProjection, XMMatrixTranspose(viewProjection));
    constants.Color = object.Color;
    constants.LightDirection = XMFLOAT4(0.35f, -0.8f, -0.45f, 0.0f);

    const UINT slot = frameOffset + drawIndex;
    memcpy(m_MappedConstantData + static_cast<size_t>(slot) * m_ConstantBufferStride, &constants, sizeof(constants));
    const D3D12_GPU_VIRTUAL_ADDRESS constantBufferAddress =
      m_ConstantBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(slot) * m_ConstantBufferStride;
    m_CommandList->SetGraphicsRootConstantBufferView(0, constantBufferAddress);
    m_CommandList->DrawIndexedInstanced(static_cast<UINT>(m_Indices.size()), 1, 0, 0, 0);
  }
}

void RenderingSystem::PopulateCommandList()
{
  ThrowIfFailed(m_CommandAllocators[m_FrameIndex]->Reset());
  ThrowIfFailed(m_CommandList->Reset(m_CommandAllocators[m_FrameIndex].Get(), m_PipelineState.Get()));

  m_Timer.Tick();
  UpdateCamera();
  CollectVisibleObjects();

  D3D12_RESOURCE_BARRIER toRenderTarget = {};
  toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toRenderTarget.Transition.pResource = m_RenderTargets[m_FrameIndex].Get();
  toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_CommandList->ResourceBarrier(1, &toRenderTarget);

  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtvHandle.ptr += static_cast<SIZE_T>(m_FrameIndex) * m_RtvDescriptorSize;
  const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
  m_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
  const float clearColor[] = {0.018f, 0.024f, 0.045f, 1.0f};
  m_CommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
  m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
  m_CommandList->RSSetViewports(1, &m_Viewport);
  m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
  m_CommandList->SetGraphicsRootSignature(m_RootSignature.Get());
  m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_CommandList->IASetVertexBuffers(0, 1, &m_VertexBufferView);
  m_CommandList->IASetIndexBuffer(&m_IndexBufferView);

  // Основной ракурс: culling уже сформировал m_VisibleObjects для этой камеры.
  DrawVisibleObjects(XMMatrixMultiply(m_ViewMatrix, m_ProjectionMatrix), 0);

  // Второй ракурс сверху. Важно: здесь нет нового culling — рисуется тот же
  // список m_VisibleObjects, поэтому inset демонстрирует результат отсечения.
  const float insetClear[] = {0.008f, 0.015f, 0.030f, 1.0f};
  m_CommandList->ClearRenderTargetView(rtvHandle, insetClear, 1, &m_TopScissorRect);
  m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &m_TopScissorRect);
  m_CommandList->RSSetViewports(1, &m_TopViewport);
  m_CommandList->RSSetScissorRects(1, &m_TopScissorRect);
  DrawVisibleObjects(XMMatrixMultiply(m_TopViewMatrix, m_TopProjectionMatrix), 1);

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
  if (!m_Initialized)
    return;
  PopulateCommandList();
  ID3D12CommandList *commandLists[] = {m_CommandList.Get()};
  m_CommandQueue->ExecuteCommandLists(1, commandLists);
  ThrowIfFailed(m_SwapChain->Present(1, 0));
  MoveToNextFrame();
}

void RenderingSystem::WaitForGPU()
{
  const UINT64 fenceValue = m_FenceValues[m_FrameIndex]++;
  ThrowIfFailed(m_CommandQueue->Signal(m_Fence.Get(), fenceValue));
  if (m_Fence->GetCompletedValue() < fenceValue)
  {
    ThrowIfFailed(m_Fence->SetEventOnCompletion(fenceValue, m_FenceEvent));
    WaitForSingleObject(m_FenceEvent, INFINITE);
  }
}

void RenderingSystem::MoveToNextFrame()
{
  const UINT64 currentFenceValue = m_FenceValues[m_FrameIndex];
  ThrowIfFailed(m_CommandQueue->Signal(m_Fence.Get(), currentFenceValue));
  m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();
  if (m_Fence->GetCompletedValue() < m_FenceValues[m_FrameIndex])
  {
    ThrowIfFailed(m_Fence->SetEventOnCompletion(m_FenceValues[m_FrameIndex], m_FenceEvent));
    WaitForSingleObject(m_FenceEvent, INFINITE);
  }
  m_FenceValues[m_FrameIndex] = currentFenceValue + 1;
}

void RenderingSystem::Resize(int width, int height)
{
  if (!m_Initialized || width <= 0 || height <= 0 || (m_Width == width && m_Height == height))
    return;

  WaitForGPU();
  m_Width = width;
  m_Height = height;
  for (UINT frame = 0; frame < FrameCount; ++frame)
  {
    m_RenderTargets[frame].Reset();
    m_FenceValues[frame] = m_FenceValues[m_FrameIndex];
  }

  DXGI_SWAP_CHAIN_DESC swapChainDescription = {};
  ThrowIfFailed(m_SwapChain->GetDesc(&swapChainDescription));
  ThrowIfFailed(m_SwapChain->ResizeBuffers(FrameCount, width, height, swapChainDescription.BufferDesc.Format,
                                           swapChainDescription.Flags));
  m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (UINT frame = 0; frame < FrameCount; ++frame)
  {
    ThrowIfFailed(m_SwapChain->GetBuffer(frame, IID_PPV_ARGS(&m_RenderTargets[frame])));
    m_Device->CreateRenderTargetView(m_RenderTargets[frame].Get(), nullptr, rtvHandle);
    rtvHandle.ptr += m_RtvDescriptorSize;
  }

  m_DepthStencil.Reset();
  D3D12_RESOURCE_DESC depthDescription = {};
  depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  depthDescription.Width = width;
  depthDescription.Height = height;
  depthDescription.DepthOrArraySize = 1;
  depthDescription.MipLevels = 1;
  depthDescription.Format = DXGI_FORMAT_D32_FLOAT;
  depthDescription.SampleDesc.Count = 1;
  depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE depthClear = {};
  depthClear.Format = DXGI_FORMAT_D32_FLOAT;
  depthClear.DepthStencil.Depth = 1.0f;
  D3D12_HEAP_PROPERTIES defaultHeap = {};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ThrowIfFailed(m_Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDescription,
                                                  D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                                  IID_PPV_ARGS(&m_DepthStencil)));
  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDescription = {};
  dsvDescription.Format = DXGI_FORMAT_D32_FLOAT;
  dsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  m_Device->CreateDepthStencilView(m_DepthStencil.Get(), &dsvDescription,
                                   m_DsvHeap->GetCPUDescriptorHandleForHeapStart());

  m_Viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
  m_ScissorRect = {0, 0, width, height};
  UpdateCamera();
  UpdateTopCamera();
}

void RenderingSystem::Cleanup()
{
  if (!m_Initialized)
    return;
  WaitForGPU();
  if (m_ConstantBuffer && m_MappedConstantData)
  {
    m_ConstantBuffer->Unmap(0, nullptr);
    m_MappedConstantData = nullptr;
  }
  if (m_FenceEvent)
  {
    CloseHandle(m_FenceEvent);
    m_FenceEvent = nullptr;
  }
  m_Initialized = false;
}
