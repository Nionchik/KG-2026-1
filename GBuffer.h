#pragma once

#include "Framework.h"
#include <array>

class GBuffer
{
public:
  static constexpr UINT TargetCount = 3;

  enum Target : UINT
  {
    Albedo = 0,
    Normal = 1,
    Position = 2
  };

  bool Initialize(ID3D12Device* device, UINT width, UINT height);
  bool Resize(UINT width, UINT height);

  void TransitionToRenderTargets(ID3D12GraphicsCommandList* commandList);
  void TransitionToShaderResources(ID3D12GraphicsCommandList* commandList);
  void ClearAndBind(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);

  ID3D12DescriptorHeap* GetSrvHeap() const { return m_SrvHeap.Get(); }
  D3D12_GPU_DESCRIPTOR_HANDLE GetSrvStart() const { return m_SrvHeap->GetGPUDescriptorHandleForHeapStart(); }

  static DXGI_FORMAT GetFormat(Target target);

private:
  bool CreateResources(UINT width, UINT height);
  D3D12_CPU_DESCRIPTOR_HANDLE GetRtvHandle(UINT index) const;
  D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandle(UINT index) const;

  ID3D12Device* m_Device = nullptr;
  UINT m_Width = 0;
  UINT m_Height = 0;
  UINT m_RtvDescriptorSize = 0;
  UINT m_SrvDescriptorSize = 0;
  bool m_InRenderTargetState = false;

  ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
  ComPtr<ID3D12DescriptorHeap> m_SrvHeap;
  std::array<ComPtr<ID3D12Resource>, TargetCount> m_Targets;
};
