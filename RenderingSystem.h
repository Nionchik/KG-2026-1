#pragma once

#include "Framework.h"
#include "Timer.h"

struct Particle
{
    XMFLOAT3 Position;
    float Age;
    XMFLOAT3 Velocity;
    float Lifetime;
    XMFLOAT4 Color;
    float Size;
    UINT Seed;
    XMFLOAT2 Padding;
};

struct alignas(256) ComputeConstants
{
    float DeltaTime;
    float TotalTime;
    UINT ParticleCount;
    float Padding0;
    XMFLOAT3 EmitterPosition;
    float Padding1;
};

struct alignas(256) RenderConstants
{
    XMFLOAT4X4 ViewProjection;
    XMFLOAT4 CameraRight;
    XMFLOAT4 CameraUp;
};

class RenderingSystem
{
  public:
    static constexpr UINT FrameCount = 2;
    static constexpr UINT ParticleCount = 5000;

    void Initialize(HWND windowHandle, UINT width, UINT height);
    void Render();
    void Cleanup();
    void Resize(UINT width, UINT height);
    void SetCameraInput(float forward, float right, float turn, float vertical);
    bool IsInitialized() const { return m_initialized; }

  private:
    void InitializeDirect3D();
    void LoadShaders();
    void CreateParticleResources();
    void CreateConstantBuffers();
    void UpdateCamera(float deltaTime);
    void PopulateCommandList();
    void WaitForGPU();
    void MoveToNextFrame();

    HWND m_windowHandle = nullptr;
    UINT m_width = 1;
    UINT m_height = 1;
    XMFLOAT3 m_cameraPosition = {0.0f, 6.0f, -20.0f};
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
    ComPtr<ID3D12RootSignature> m_computeRootSignature;
    ComPtr<ID3D12RootSignature> m_renderRootSignature;
    ComPtr<ID3D12PipelineState> m_computePipelineState;
    ComPtr<ID3D12PipelineState> m_renderPipelineState;
    ComPtr<ID3D12PipelineState> m_platformPipelineState;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_particleDescriptorHeap;
    ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    ComPtr<ID3D12Resource> m_depthBuffer;
    ComPtr<ID3D12Resource> m_particleBuffers[2];
    ComPtr<ID3D12Resource> m_particleCounters[2];
    ComPtr<ID3D12Resource> m_computeConstantBuffer;
    ComPtr<ID3D12Resource> m_renderConstantBuffer;
    ComPtr<ID3D12Fence> m_fence;

    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT m_scissorRect{};
    UINT m_rtvDescriptorSize = 0;
    UINT m_particleDescriptorSize = 0;
    UINT m_frameIndex = 0;
    UINT m_sourceParticleBuffer = 0;
    UINT m_computeConstantSize = 0;
    UINT m_renderConstantSize = 0;
    UINT64 m_fenceValues[FrameCount]{};
    uint8_t *m_mappedComputeConstants = nullptr;
    uint8_t *m_mappedRenderConstants = nullptr;
    HANDLE m_fenceEvent = nullptr;
    bool m_initialized = false;
    Timer m_timer;
};
