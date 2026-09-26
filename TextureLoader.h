#pragma once
#include "Framework.h"
#include <vector>

class TextureLoader
{
public:
  struct TextureData
  {
    std::vector<uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT rowPitch = 0;
  };

  static bool LoadFromFile(const std::wstring& path, TextureData& out);

  static bool CreateTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const TextureData& data,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& uploadBuf);
};