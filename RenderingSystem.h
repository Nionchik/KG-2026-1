#pragma once

#include "Framework.h"
#include "Timer.h"

#include <array>
#include <vector>

struct Vertex
{
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
};

struct alignas(256) DrawConstants
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 ViewProjection;
    XMFLOAT4 Color;
};

struct alignas(256) ShadowConstants
{
    XMFLOAT4X4 LightViewProjection[4];
    XMFLOAT4 CascadeSplits;
    XMFLOAT4 LightDirection;
    XMFLOAT4 CameraPosition;
    XMFLOAT4 CameraForward;
    XMFLOAT4 ShadowMapSize;
};

struct SceneObject
{
    XMFLOAT3 Position;
    XMFLOAT3 Scale;
    XMFLOAT4 Color;
};

class RenderingSystem
{
  public:
    static constexpr UINT FrameCount = 2;
    static constexpr UINT CascadeCount = 4;
    static constexpr UINT ShadowMapSize = 1024;
    static constexpr UINT MaxObjects = 256;

    void Initialize(HWND windowHandle, UINT width, UINT height);
    void Render();
    void Cleanup();
    void Resize(UINT width, UINT height);
    void SetCameraInput(float forward, float right, float turn, float vertical);
    bool IsInitialized() const
    {
        return m_initialized;
    }

  private:
    void InitializeDirect3D();
    void LoadShaders();
    void CreateCubeMesh();
    void CreateScene();
    void CreateConstantBuffers();
    void CreateShadowResources();
    void UpdateCamera(float deltaTime);
    void CalculateCascadeMatrices();
    void RenderShadowMaps();
    void PopulateCommandList();
    void WriteDrawConstants(UINT slot, const SceneObject &object, const XMMATRIX &viewProjection);
    void WaitForGPU();
    void MoveToNextFrame();

    HWND m_windowHandle = nullptr;
    UINT m_width = 1;
    UINT m_height = 1;
    XMFLOAT3 m_cameraPosition = {0.0f, 6.0f, -30.0f};
    float m_moveRight = 0.0f;
    float m_moveForward = 0.0f;
    float m_moveTurn = 0.0f;
    float m_moveVertical = 0.0f;
    float m_cameraRotationY = 0.0f;
    XMMATRIX m_view = XMMatrixIdentity();
    XMMATRIX m_projection = XMMatrixIdentity();
    std::array<XMMATRIX, CascadeCount> m_cascadeMatrices{};
    std::array<float, CascadeCount> m_cascadeSplits{};
    XMFLOAT3 m_lightDirection = {0.45f, -1.0f, 0.35f};
    std::vector<SceneObject> m_scene;

    ComPtr<ID3D12Device> m_device;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[FrameCount];
    ComPtr<ID3D12RootSignature> m_mainRootSignature;
    ComPtr<ID3D12RootSignature> m_shadowRootSignature;
    ComPtr<ID3D12PipelineState> m_mainPipelineState;
    ComPtr<ID3D12PipelineState> m_shadowPipelineState;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_shadowDsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_shadowSrvHeap;
    ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    ComPtr<ID3D12Resource> m_depthBuffer;
    ComPtr<ID3D12Resource> m_shadowMap;
    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    ComPtr<ID3D12Resource> m_drawConstantBuffer;
    ComPtr<ID3D12Resource> m_shadowConstantBuffer;
    ComPtr<ID3D12Fence> m_fence;

    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView{};
    D3D12_VIEWPORT m_viewport{};
    D3D12_VIEWPORT m_shadowViewport{};
    D3D12_RECT m_scissorRect{};
    D3D12_RECT m_shadowScissorRect{};
    UINT m_rtvDescriptorSize = 0;
    UINT m_frameIndex = 0;
    UINT m_drawConstantSize = 0;
    UINT m_shadowConstantSize = 0;
    UINT64 m_fenceValues[FrameCount]{};
    uint8_t *m_mappedDrawConstants = nullptr;
    uint8_t *m_mappedShadowConstants = nullptr;
    HANDLE m_fenceEvent = nullptr;
    bool m_initialized = false;
    bool m_shadowReadable = false;
    Timer m_timer;
};
