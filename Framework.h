#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winnls.h>
#include <functional>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <DirectXColors.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <cstdint>
#include <cassert>
#include <iostream>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

inline void ThrowIfFailed(HRESULT hr)
{
  if (FAILED(hr))
    throw std::runtime_error("DirectX call failed");
}