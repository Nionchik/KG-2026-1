#include "TextureLoader.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

bool TextureLoader::LoadFromFile(const std::wstring& path, TextureData& out)
{
  int utf8Length = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (utf8Length <= 0) return false;

  std::string narrowPath(static_cast<size_t>(utf8Length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrowPath.data(), utf8Length, nullptr, nullptr);

  stbi_set_flip_vertically_on_load(1);
  int w, h, channels;
  unsigned char* data = stbi_load(narrowPath.c_str(), &w, &h, &channels, 4);
  if (!data) return false;

  out.width = (UINT)w;
  out.height = (UINT)h;
  out.format = DXGI_FORMAT_R8G8B8A8_UNORM;
  out.rowPitch = (UINT)w * 4;
  out.pixels.assign(data, data + (size_t)w * h * 4);

  stbi_image_free(data);
  return true;
}

bool TextureLoader::CreateTexture(
  ID3D12Device* device,
  ID3D12GraphicsCommandList* cmdList,
  const TextureData& data,
  ComPtr<ID3D12Resource>& texture,
  ComPtr<ID3D12Resource>& uploadBuf)
{
  D3D12_RESOURCE_DESC texDesc = {};
  texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texDesc.Width = data.width;
  texDesc.Height = data.height;
  texDesc.DepthOrArraySize = 1;
  texDesc.MipLevels = 1;
  texDesc.Format = data.format;
  texDesc.SampleDesc.Count = 1;
  texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_HEAP_PROPERTIES defaultHeap = {};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

  HRESULT hr = device->CreateCommittedResource(
    &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
    IID_PPV_ARGS(&texture));
  if (FAILED(hr)) return false;

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT numRows = 0;
  UINT64 rowSize = 0;
  UINT64 uploadSize = 0;
  device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &uploadSize);

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

  hr = device->CreateCommittedResource(
    &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
    IID_PPV_ARGS(&uploadBuf));
  if (FAILED(hr)) return false;

  uint8_t* mappedData = nullptr;
  if (FAILED(uploadBuf->Map(0, nullptr, reinterpret_cast<void**>(&mappedData)))) return false;
  for (UINT row = 0; row < numRows; ++row)
  {
    memcpy(
      mappedData + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
      data.pixels.data() + static_cast<size_t>(row) * data.rowPitch,
      static_cast<size_t>(rowSize));
  }
  uploadBuf->Unmap(0, nullptr);

  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = uploadBuf.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = footprint;

  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = texture.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;

  cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = texture.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cmdList->ResourceBarrier(1, &barrier);

  return true;
}
