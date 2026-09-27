#pragma once

#include "Framework.h"
#include "Timer.h"
#include <DirectXCollision.h>
#include <array>
#include <memory>
#include <vector>

struct Vertex
{
  XMFLOAT3 Position;
  XMFLOAT3 Normal;
};

struct alignas(256) ConstantBufferData
{
  XMFLOAT4X4 World;
  XMFLOAT4X4 ViewProjection;
  XMFLOAT4 Color;
  XMFLOAT4 LightDirection;
};

struct SceneObject
{
  XMFLOAT3 Position;
  float Radius;
  XMFLOAT4 Color;
  DirectX::BoundingSphere Bounds;
};

struct OctreeNode
{
  DirectX::BoundingBox Bounds;
  std::vector<UINT> ObjectIndices;
  std::array<std::unique_ptr<OctreeNode>, 8> Children;
};

class RenderingSystem
{
public:
  static constexpr UINT FrameCount = 2;
  static constexpr UINT ObjectCount = 3000;

  RenderingSystem();
  ~RenderingSystem();

  bool Initialize(HWND hwnd, int width, int height);
  void Render();
  void Cleanup();
  void Resize(int width, int height);
  void SetCameraInput(float forward, float right, float turn, float vertical);
  void SetCullingMode(int mode);
  int GetCullingMode() const
  {
    return m_CullingMode;
  }
  UINT GetVisibleObjectCount() const
  {
    return static_cast<UINT>(m_VisibleObjects.size());
  }
  bool IsInitialized() const
  {
    return m_Initialized;
  }

private:
  bool InitializeDirect3D(HWND hwnd);
  bool LoadShaders();
  void CreateSphereMesh();
  void CreateScene();
  void CreateConstantBuffer();
  void BuildOctree();
  void SubdivideOctree(OctreeNode &node, UINT depth);
  void AddNodeObjects(const OctreeNode &node, std::vector<UINT> &output) const;
  void QueryOctree(const OctreeNode &node, const DirectX::BoundingFrustum &frustum, std::vector<UINT> &output) const;
  void CollectVisibleObjects();
  void UpdateCamera();
  void UpdateTopCamera();
  void DrawVisibleObjects(const DirectX::XMMATRIX& viewProjection, UINT passIndex);
  void PopulateCommandList();
  void WaitForGPU();
  void MoveToNextFrame();

  std::vector<Vertex> m_Vertices;
  std::vector<UINT> m_Indices;
  std::vector<SceneObject> m_Objects;
  std::vector<UINT> m_VisibleObjects;
  std::unique_ptr<OctreeNode> m_OctreeRoot;

  XMFLOAT3 m_CameraPosition = {0.0f, 0.0f, -35.0f};
  float m_ForwardMovement = 0.0f;
  float m_RightMovement = 0.0f;
  float m_TurnMovement = 0.0f;
  float m_VerticalMovement = 0.0f;
  float m_CameraRotationY = 0.0f;
  int m_CullingMode = 2;
  XMMATRIX m_ViewMatrix = XMMatrixIdentity();
  XMMATRIX m_ProjectionMatrix = XMMatrixIdentity();
  XMMATRIX m_TopViewMatrix = XMMatrixIdentity();
  XMMATRIX m_TopProjectionMatrix = XMMatrixIdentity();

  ComPtr<ID3D12Device> m_Device;
  ComPtr<IDXGISwapChain3> m_SwapChain;
  ComPtr<ID3D12CommandQueue> m_CommandQueue;
  ComPtr<ID3D12GraphicsCommandList> m_CommandList;
  ComPtr<ID3D12CommandAllocator> m_CommandAllocators[FrameCount];
  ComPtr<ID3D12RootSignature> m_RootSignature;
  ComPtr<ID3D12PipelineState> m_PipelineState;
  ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
  ComPtr<ID3D12DescriptorHeap> m_DsvHeap;
  ComPtr<ID3D12Resource> m_RenderTargets[FrameCount];
  ComPtr<ID3D12Resource> m_DepthStencil;
  ComPtr<ID3D12Resource> m_VertexBuffer;
  ComPtr<ID3D12Resource> m_IndexBuffer;
  ComPtr<ID3D12Resource> m_ConstantBuffer;
  ComPtr<ID3D12Fence> m_Fence;

  D3D12_VERTEX_BUFFER_VIEW m_VertexBufferView = {};
  D3D12_INDEX_BUFFER_VIEW m_IndexBufferView = {};
  D3D12_VIEWPORT m_Viewport = {};
  D3D12_RECT m_ScissorRect = {};
  D3D12_VIEWPORT m_TopViewport = {};
  D3D12_RECT m_TopScissorRect = {};
  UINT m_RtvDescriptorSize = 0;
  UINT m_FrameIndex = 0;
  UINT m_ConstantBufferStride = 0;
  UINT64 m_FenceValues[FrameCount] = {};
  uint8_t *m_MappedConstantData = nullptr;
  HANDLE m_FenceEvent = nullptr;
  int m_Width = 0;
  int m_Height = 0;
  bool m_Initialized = false;
  Timer m_Timer;
};
