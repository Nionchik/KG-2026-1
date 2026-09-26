
#pragma once
#include "Framework.h"
#include "Timer.h"
#include "TextureLoader.h"
#include "GBuffer.h"
#include <vector>
#include <string>
#include <unordered_map>

struct Vertex
{
  XMFLOAT3 Position;
  XMFLOAT3 Normal;
  XMFLOAT2 TexCoord;
  XMFLOAT4 Tangent;
};

struct alignas(256) ConstantBufferData
{
  XMFLOAT4X4 World;
  XMFLOAT4X4 View;
  XMFLOAT4X4 Proj;
  XMFLOAT4X4 WorldInvTranspose;
  XMFLOAT4 LightDir;
  XMFLOAT4 LightColor;
  XMFLOAT4 AmbientColor;
  XMFLOAT4 EyePos;
  XMFLOAT4 MaterialDiffuse;
  XMFLOAT4 MaterialSpecular;
  float SpecularPower;
  float TotalTime;
  float TexTilingX;
  float TexTilingY;
  float TexScrollX;
  float TexScrollY;
  int HasTexture;
  float Pad[3];
  float TessellationMin;
  float TessellationMax;
  float TessellationNear;
  float TessellationFar;
  float DisplacementScale;
  float TessellationPad[3];
};

struct Material
{
  std::string Name;
  XMFLOAT4 Diffuse = { 0.8f, 0.8f, 0.8f, 1.0f };
  XMFLOAT4 Specular = { 0.5f, 0.5f, 0.5f, 1.0f };
  float Shininess = 32.0f;
  std::string DiffuseTexture;
  int SrvIndex = -1;
};

struct Subset
{
  UINT IndexStart;
  UINT IndexCount;
  int MaterialIndex;
};

struct PointLightData
{
  XMFLOAT4 PositionAndRange;
  XMFLOAT4 ColorAndIntensity;
};

struct alignas(256) LightingConstants
{
  XMFLOAT4 EyePosition;
  XMFLOAT4 DirectionalDirection;
  XMFLOAT4 DirectionalColorAndIntensity;
  XMFLOAT4 SpotPositionAndRange;
  XMFLOAT4 SpotDirectionAndInnerCone;
  XMFLOAT4 SpotColorAndOuterCone;
  XMFLOAT4 AmbientColor;
  PointLightData PointLights[8];
  XMFLOAT4 PointLightInfo;
};

class RenderingSystem
{
public:
  static constexpr UINT FrameCount = 2;
  static constexpr UINT MaxSubsets = 512;
  static constexpr UINT MaxTextures = 128;

  RenderingSystem();
  ~RenderingSystem();

  bool Initialize(HWND hwnd, int width, int height);
  void Render();
  void Cleanup();
  void Resize(int width, int height);
  void SetCameraInput(float forward, float right, float turn, float vertical);
  void ToggleTessellation() { m_TessellationEnabled = !m_TessellationEnabled; }
  bool IsTessellationEnabled() const { return m_TessellationEnabled; }
  bool IsInitialized() const { return m_Initialized; }

  bool LoadModel(const std::string& filename);
  void SetTexTiling(float x, float y) { m_TexTiling = XMFLOAT2(x, y); }
  void SetTexScroll(float x, float y) { m_TexScroll = XMFLOAT2(x, y); }

private:
  bool InitializeDirect3D(HWND hwnd);
  bool LoadShaders();
  bool LoadMaterials(const std::string& mtlPath);
  bool LoadTexture(const std::wstring& path, int& outSrvIndex);
  void CreateWhiteDummyTexture();
  void CreateConstantBuffers();
  void SetupMatrices();
  void UpdateCamera();
  void UpdateLightingConstants();
  void PopulateCommandList();
  void WaitForGPU();
  void MoveToNextFrame();

  std::vector<Vertex> m_Vertices;
  std::vector<UINT> m_Indices;
  std::vector<Material> m_Materials;
  std::vector<Subset> m_Subsets;
  XMFLOAT3 m_MinBounds, m_MaxBounds, m_Center;
  float m_Radius;

  XMMATRIX m_WorldMatrix;
  XMMATRIX m_ViewMatrix;
  XMMATRIX m_ProjectionMatrix;
  XMFLOAT3 m_CameraPosition;
  XMFLOAT3 m_CameraTarget;
  float m_CameraDistance;
  float m_CameraRotationX, m_CameraRotationY;
  float m_CameraForwardInput = 0.0f;
  float m_CameraRightInput = 0.0f;
  float m_CameraTurnInput = 0.0f;
  float m_CameraVerticalInput = 0.0f;
  float m_RotationAngle;

  XMFLOAT2 m_TexTiling = { 1.0f, 1.0f };
  XMFLOAT2 m_TexScroll = { 0.0f, 0.0f };

  ComPtr<ID3D12Device> m_Device;
  ComPtr<IDXGISwapChain3> m_SwapChain;
  ComPtr<ID3D12CommandQueue> m_CommandQueue;
  ComPtr<ID3D12GraphicsCommandList> m_CommandList;
  ComPtr<ID3D12CommandAllocator> m_CommandAllocators[FrameCount];
  ComPtr<ID3D12RootSignature> m_GeometryRootSignature;
  ComPtr<ID3D12PipelineState> m_GeometryPipelineState;
  ComPtr<ID3D12PipelineState> m_FlatGeometryPipelineState;
  ComPtr<ID3D12RootSignature> m_LightingRootSignature;
  ComPtr<ID3D12PipelineState> m_LightingPipelineState;
  ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
  ComPtr<ID3D12DescriptorHeap> m_DsvHeap;
  ComPtr<ID3D12DescriptorHeap> m_SrvHeap;
  ComPtr<ID3D12Resource> m_RenderTargets[FrameCount];
  ComPtr<ID3D12Resource> m_DepthStencil;
  ComPtr<ID3D12Resource> m_VertexBuffer;
  ComPtr<ID3D12Resource> m_IndexBuffer;
  ComPtr<ID3D12Resource> m_ConstantBuffer;
  ComPtr<ID3D12Resource> m_LightingConstantBuffer;
  ComPtr<ID3D12Fence> m_Fence;

  UINT m_RtvDescriptorSize = 0;
  UINT m_SrvDescriptorSize = 0;
  UINT m_FrameIndex = 0;
  UINT64 m_FenceValues[FrameCount] = {};
  HANDLE m_FenceEvent = nullptr;

  D3D12_VERTEX_BUFFER_VIEW m_VBView = {};
  D3D12_INDEX_BUFFER_VIEW m_IBView = {};
  D3D12_VIEWPORT m_Viewport = {};
  D3D12_RECT m_ScissorRect = {};

  UINT m_ConstantBufferSlotSize = 0;
  uint8_t* m_MappedConstantData = nullptr;
  UINT m_LightingConstantBufferSlotSize = 0;
  uint8_t* m_MappedLightingConstantData = nullptr;

  int m_Width = 0;
  int m_Height = 0;
  bool m_Initialized = false;
  bool m_TessellationEnabled = true;
  Timer m_Timer;
  GBuffer m_GBuffer;

  std::vector<ComPtr<ID3D12Resource>> m_TextureUploads;
  std::unordered_map<std::wstring, int> m_TextureCache;
  UINT m_NextSrvIndex = 1;
};
