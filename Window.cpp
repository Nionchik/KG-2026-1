#include "Window.h"

Window::Window(HINSTANCE hInstance, int width, int height, const wchar_t* title)
  : m_hInstance(hInstance)
  , m_hWnd(nullptr)
  , m_Width(width)
  , m_Height(height)
  , m_Title(title)
  , m_ClassName(L"DX12WindowClass")
{
}

Window::~Window()
{
  if (m_hWnd)
    DestroyWindow(m_hWnd);
}

bool Window::Initialize()
{
  RegisterWindowClass();

  m_hWnd = CreateWindowExW(
    0,
    m_ClassName.c_str(),
    m_Title.c_str(),
    WS_OVERLAPPEDWINDOW,
    CW_USEDEFAULT, CW_USEDEFAULT,
    m_Width, m_Height,
    nullptr,
    nullptr,
    m_hInstance,
    this
  );

  return m_hWnd != nullptr;
}

void Window::Show(int nCmdShow)
{
  ShowWindow(m_hWnd, nCmdShow);
  UpdateWindow(m_hWnd);
}

bool Window::ProcessMessages()
{
  MSG msg = {};
  while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
    if (msg.message == WM_QUIT)
      return false;
  }
  return true;
}

void Window::RegisterWindowClass()
{
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = StaticWindowProc;
  wc.hInstance = m_hInstance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = m_ClassName.c_str();

  RegisterClassExW(&wc);
}

LRESULT CALLBACK Window::StaticWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  Window* pWindow = nullptr;
  if (msg == WM_NCCREATE)
  {
    CREATESTRUCTW* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
    pWindow = reinterpret_cast<Window*>(pCreate->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWindow));
  }
  else
  {
    pWindow = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }

  if (pWindow)
    return pWindow->HandleMessage(hwnd, msg, wParam, lParam);

  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Window::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg)
  {
  case WM_SIZE:
    if (wParam != SIZE_MINIMIZED)
    {
      m_Width = LOWORD(lParam);
      m_Height = HIWORD(lParam);
      if (m_ResizeCallback)
        m_ResizeCallback(m_Width, m_Height);
    }
    return 0;

  case WM_KEYDOWN:
    if (m_KeyCallback)
      m_KeyCallback(wParam, true);
    return 0;

  case WM_KEYUP:
    if (m_KeyCallback)
      m_KeyCallback(wParam, false);
    return 0;

  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}