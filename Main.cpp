#include "Window.h"
#include "RenderingSystem.h"
#include "InputDevice.h"

static void SetWorkingDirectoryToExecutable()
{
  wchar_t executablePath[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, executablePath, MAX_PATH) == 0)
    return;
  std::wstring directory(executablePath);
  const size_t separator = directory.find_last_of(L"/\\");
  if (separator != std::wstring::npos)
    SetCurrentDirectoryW(directory.substr(0, separator).c_str());
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                    _In_ int nCmdShow)
{
  UNREFERENCED_PARAMETER(hPrevInstance);
  UNREFERENCED_PARAMETER(lpCmdLine);
  SetWorkingDirectoryToExecutable();

  Window window(hInstance, 1280, 720, L"DirectX 12 - Frustum Culling and Octree");
  RenderingSystem renderer;
  InputDevice input;

  if (!window.Initialize())
  {
    MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
    return 1;
  }

  window.SetKeyCallback(
      [&](WPARAM key, bool pressed)
      {
        if (pressed)
          input.OnKeyDown(key);
        else
          input.OnKeyUp(key);
      });
  window.SetResizeCallback(
      [&](int width, int height)
      {
        if (renderer.IsInitialized())
          renderer.Resize(width, height);
      });

  if (!renderer.Initialize(window.GetHandle(), window.GetWidth(), window.GetHeight()))
  {
    MessageBoxW(nullptr, L"Failed to initialize DirectX 12 renderer", L"Error", MB_OK | MB_ICONERROR);
    return 1;
  }

  window.Show(nCmdShow);
  ULONGLONG lastTitleUpdate = 0;
  while (window.ProcessMessages())
  {
    input.Update();
    float forward = 0.0f;
    float right = 0.0f;
    float turn = 0.0f;
    float vertical = 0.0f;
    if (input.IsKeyPressed('W'))
      forward += 1.0f;
    if (input.IsKeyPressed('S'))
      forward -= 1.0f;
    if (input.IsKeyPressed('D'))
      right += 1.0f;
    if (input.IsKeyPressed('A'))
      right -= 1.0f;
    if (input.IsKeyPressed(VK_RIGHT))
      turn += 1.0f;
    if (input.IsKeyPressed(VK_LEFT))
      turn -= 1.0f;
    if (input.IsKeyPressed(VK_UP))
      vertical += 1.0f;
    if (input.IsKeyPressed(VK_DOWN))
      vertical -= 1.0f;
    renderer.SetCameraInput(forward, right, turn, vertical);

    if (input.IsKeyPressed('1'))
      renderer.SetCullingMode(0);
    if (input.IsKeyPressed('2'))
      renderer.SetCullingMode(1);
    if (input.IsKeyPressed('3'))
      renderer.SetCullingMode(2);

    // 1 выключает отсечение, 2 включает обычный frustum culling, 3 включает октодерево.

    renderer.Render();

    const ULONGLONG currentTime = GetTickCount64();
    if (currentTime - lastTitleUpdate >= 250)
    {
      const wchar_t *modeNames[] = {L"No culling", L"Frustum culling", L"Octree"};
      const std::wstring title = std::wstring(L"Lab8 | ") + modeNames[renderer.GetCullingMode()] + L" | Visible: " +
                                 std::to_wstring(renderer.GetVisibleObjectCount()) + L" / 3000";
      SetWindowTextW(window.GetHandle(), title.c_str());
      lastTitleUpdate = currentTime;
    }

    if (input.IsKeyPressed(VK_ESCAPE))
      PostQuitMessage(0);
  }

  renderer.Cleanup();
  return 0;
}
