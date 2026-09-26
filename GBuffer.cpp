#include "GBuffer.h"

DXGI_FORMAT GBuffer::GetFormat(Target target)
{
  switch (target)
  {
  case Albedo:
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  case Normal:
  case Position:
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

bool GBuffer::Initialize(ID3D12Device* device, UINT width, UINT height)
{
  m_Device = device;
  if (!m_Device) return false;

  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
  rtvHeapDesc.NumDescriptors = TargetCount;
  rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  if (FAILED(m_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_RtvHeap))))
    return false;

  D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
  srvHeapDesc.NumDescriptors = TargetCount;
  srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(m_Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_SrvHeap))))
    return false;

  m_RtvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  m_SrvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return CreateResources(width, height);
}

bool GBuffer::Resize(UINT width, UINT height)
{
  if (!m_Device || width == 0 || height == 0) return false;
  if (width == m_Width && height == m_Height) return true;

  for (auto& target : m_Targets)
    target.Reset();
  return CreateResources(width, height);
}

bool GBuffer::CreateResources(UINT width, UINT height)
{
  m_Width = width;
  m_Height = height;
  m_InRenderTargetState = false;

  const float clearColors[TargetCount][4] =
  {
    { 0.025f, 0.04f, 0.075f, 1.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f }
  };

  for (UINT index = 0; index < TargetCount; ++index)
  {
    const DXGI_FORMAT format = GetFormat(static_cast<Target>(index));

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProperties = {};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = format;
    for (UINT channel = 0; channel < 4; ++channel)
      clearValue.Color[channel] = clearColors[index][channel];

    HRESULT hr = m_Device->CreateCommittedResource(
      &heapProperties,
      D3D12_HEAP_FLAG_NONE,
      &textureDesc,
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
      &clearValue,
      IID_PPV_ARGS(&m_Targets[index]));
    if (FAILED(hr)) return false;

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = format;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_Device->CreateRenderTargetView(m_Targets[index].Get(), &rtvDesc, GetRtvHandle(index));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    m_Device->CreateShaderResourceView(m_Targets[index].Get(), &srvDesc, GetSrvCpuHandle(index));
  }

  return true;
}

void GBuffer::TransitionToRenderTargets(ID3D12GraphicsCommandList* commandList)
{
  if (m_InRenderTargetState) return;

  D3D12_RESOURCE_BARRIER barriers[TargetCount] = {};
  for (UINT index = 0; index < TargetCount; ++index)
  {
    barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[index].Transition.pResource = m_Targets[index].Get();
    barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  }
  commandList->ResourceBarrier(TargetCount, barriers);
  m_InRenderTargetState = true;
}

void GBuffer::TransitionToShaderResources(ID3D12GraphicsCommandList* commandList)
{
  if (!m_InRenderTargetState) return;

  D3D12_RESOURCE_BARRIER barriers[TargetCount] = {};
  for (UINT index = 0; index < TargetCount; ++index)
  {
    barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[index].Transition.pResource = m_Targets[index].Get();
    barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  }
  commandList->ResourceBarrier(TargetCount, barriers);
  m_InRenderTargetState = false;
}

void GBuffer::ClearAndBind(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[TargetCount] = {};
  const float clearColors[TargetCount][4] =
  {
    { 0.025f, 0.04f, 0.075f, 1.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f }
  };

  for (UINT index = 0; index < TargetCount; ++index)
  {
    rtvHandles[index] = GetRtvHandle(index);
    commandList->ClearRenderTargetView(rtvHandles[index], clearColors[index], 0, nullptr);
  }

  commandList->OMSetRenderTargets(TargetCount, rtvHandles, FALSE, &dsvHandle);
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetRtvHandle(UINT index) const
{
  D3D12_CPU_DESCRIPTOR_HANDLE handle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<SIZE_T>(index) * m_RtvDescriptorSize;
  return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetSrvCpuHandle(UINT index) const
{
  D3D12_CPU_DESCRIPTOR_HANDLE handle = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<SIZE_T>(index) * m_SrvDescriptorSize;
  return handle;
}
