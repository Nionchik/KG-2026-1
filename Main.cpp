#include "InputDevice.h"
#include "RenderingSystem.h"
#include "Window.h"

static void SetWorkingDirectoryToExecutable()
{
    wchar_t executablePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, executablePath, MAX_PATH) == 0)
    {
        return;
    }
    std::wstring directory(executablePath);
    const size_t separator = directory.find_last_of(L"/\\");
    if (separator != std::wstring::npos)
    {
        SetCurrentDirectoryW(directory.substr(0, separator).c_str());
    }
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    SetWorkingDirectoryToExecutable();
    const HRESULT comStatus = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comStatus))
        return 1;

    Window window(hInstance, 1280, 720, L"Lab11 | PBR + IBL");
    RenderingSystem renderer;
    InputDevice input;

    if (!window.Initialize())
    {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    window.SetKeyCallback([&](WPARAM key, bool pressed) {
        if (pressed)
        {
            input.OnKeyDown(key);
        }
        else
        {
            input.OnKeyUp(key);
        }
    });
    window.SetResizeCallback([&](int width, int height) {
        if (renderer.IsInitialized())
        {
            renderer.Resize(width, height);
        }
    });

    try
    {
        renderer.Initialize(window.GetHandle(), window.GetWidth(), window.GetHeight());
    }
    catch (const std::exception &exception)
    {
        MessageBoxA(nullptr, exception.what(), "DirectX 12 error", MB_OK | MB_ICONERROR);
        return 1;
    }

    window.Show(nCmdShow);
    bool iblKeyWasPressed = false;
    while (window.ProcessMessages())
    {
        input.Update();
        const bool iblKeyPressed = input.IsKeyPressed('I');
        if (iblKeyPressed && !iblKeyWasPressed)
            renderer.ToggleIBL();
        iblKeyWasPressed = iblKeyPressed;
        float forward = 0.0f;
        float right = 0.0f;
        float turn = 0.0f;
        float vertical = 0.0f;
        if (input.IsKeyPressed('W'))
        {
            forward += 1.0f;
        }
        if (input.IsKeyPressed('S'))
        {
            forward -= 1.0f;
        }
        if (input.IsKeyPressed('D'))
        {
            right += 1.0f;
        }
        if (input.IsKeyPressed('A'))
        {
            right -= 1.0f;
        }
        if (input.IsKeyPressed(VK_RIGHT))
        {
            turn += 1.0f;
        }
        if (input.IsKeyPressed(VK_LEFT))
        {
            turn -= 1.0f;
        }
        if (input.IsKeyPressed(VK_UP))
        {
            vertical += 1.0f;
        }
        if (input.IsKeyPressed(VK_DOWN))
        {
            vertical -= 1.0f;
        }
        renderer.SetCameraInput(forward, right, turn, vertical);

        try
        {
            renderer.Render();
        }
        catch (const std::exception &exception)
        {
            MessageBoxA(nullptr, exception.what(), "DirectX 12 error", MB_OK | MB_ICONERROR);
            break;
        }

        if (input.IsKeyPressed(VK_ESCAPE))
        {
            PostQuitMessage(0);
        }
    }

    renderer.Cleanup();
    CoUninitialize();
    return 0;
}
