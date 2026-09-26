#pragma once
#include "Framework.h"

class Window
{
public:
  Window(HINSTANCE hInstance, int width, int height, const wchar_t* title);
  ~Window();

  bool Initialize();
  void Show(int nCmdShow = SW_SHOW);
  bool ProcessMessages();

  HWND GetHandle() const { return m_hWnd; }
  int GetWidth() const { return m_Width; }
  int GetHeight() const { return m_Height; }

  void SetResizeCallback(std::function<void(int, int)> callback) { m_ResizeCallback = callback; }
  void SetKeyCallback(std::function<void(WPARAM, bool)> callback) { m_KeyCallback = callback; }

private:
  static LRESULT CALLBACK StaticWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  void RegisterWindowClass();

  HINSTANCE m_hInstance;
  HWND m_hWnd;
  int m_Width;
  int m_Height;
  std::wstring m_Title;
  std::wstring m_ClassName;

  std::function<void(int, int)> m_ResizeCallback;
  std::function<void(WPARAM, bool)> m_KeyCallback;
};