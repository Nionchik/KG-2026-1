#pragma once

#include "Framework.h"
#include "Timer.h"

struct Vertex
{
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
    XMFLOAT2 UV;
};

struct alignas(256) SceneConstants
{
    XMFLOAT4X4 ViewProjection;
    XMFLOAT4X4 InverseViewProjection;
    XMFLOAT4 CameraPosition;
    XMFLOAT4 LightDirection;
    XMFLOAT4 LightColor;
    XMFLOAT4 Options;
};

class RenderingSystem
{
  public:
    static constexpr UINT FrameCount = 2;
    void Initialize(HWND windowHandle, UINT width, UINT height);
    void Render();
    void Cleanup();
    void Resize(UINT width, UINT height);
    void SetCameraInput(float forward, float right, float turn, float vertical);
    void ToggleIBL();
    bool IsInitialized() const { return m_initialized; }

  private:
    void InitializeDirect3D();
    void LoadShaders();
    void CreateAssets();
    void UpdateCamera(float deltaTime);
    void PopulateCommandList();
    void WaitForGPU();
    void MoveToNextFrame();

    HWND m_windowHandle = nullptr;
    UINT m_width = 1;
    UINT m_height = 1;
    XMFLOAT3 m_cameraPosition = {0.0f, 0.0f, -9.0f};
    float m_cameraRotationY = 0.0f;
    float m_moveForward = 0.0f;
    float m_moveRight = 0.0f;
    float m_moveTurn = 0.0f;
    float m_moveVertical = 0.0f;
    XMMATRIX m_view = XMMatrixIdentity();
    XMMATRIX m_projection = XMMatrixIdentity();
    ComPtr<ID3D12Device> m_device;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[FrameCount];
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_modelPipeline;
    ComPtr<ID3D12PipelineState> m_skyPipeline;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_textureHeap;
    ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    ComPtr<ID3D12Resource> m_depthBuffer;
    ComPtr<ID3D12Resource> m_textures[7];
    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_constantBuffer;
    ComPtr<ID3D12Fence> m_fence;
    D3D12_VERTEX_BUFFER_VIEW m_vertexView{};
    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT m_scissorRect{};
    UINT m_vertexCount = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT m_frameIndex = 0;
    UINT m_constantSize = 0;
    UINT m_prefilterMipCount = 1;
    UINT64 m_fenceValues[FrameCount]{};
    uint8_t *m_mappedConstants = nullptr;
    HANDLE m_fenceEvent = nullptr;
    bool m_initialized = false;
    bool m_iblEnabled = true;
    Timer m_timer;
};
