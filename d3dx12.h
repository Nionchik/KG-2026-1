#pragma once

#include <d3d12.h>

struct CD3DX12_HEAP_PROPERTIES : public D3D12_HEAP_PROPERTIES
{
  CD3DX12_HEAP_PROPERTIES() = default;
  explicit CD3DX12_HEAP_PROPERTIES(const D3D12_HEAP_PROPERTIES& o) : D3D12_HEAP_PROPERTIES(o) {}

  explicit CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE type)
  {
    Type = type;
    CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    CreationNodeMask = 1;
    VisibleNodeMask = 1;
  }
};

struct CD3DX12_RESOURCE_DESC : public D3D12_RESOURCE_DESC
{
  CD3DX12_RESOURCE_DESC() = default;
  explicit CD3DX12_RESOURCE_DESC(const D3D12_RESOURCE_DESC& o) : D3D12_RESOURCE_DESC(o) {}

  static inline CD3DX12_RESOURCE_DESC Buffer(UINT64 width)
  {
    CD3DX12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = width;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
  }

  static inline CD3DX12_RESOURCE_DESC Tex2D(DXGI_FORMAT format, UINT64 width, UINT height)
  {
    CD3DX12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return desc;
  }
};

struct CD3DX12_RANGE : public D3D12_RANGE
{
  CD3DX12_RANGE() = default;
  explicit CD3DX12_RANGE(const D3D12_RANGE& o) : D3D12_RANGE(o) {}
  CD3DX12_RANGE(SIZE_T begin, SIZE_T end)
  {
    Begin = begin;
    End = end;
  }
};