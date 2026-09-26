#pragma once
#include <windows.h>
#include <unordered_map>

class InputDevice
{
public:
  InputDevice();
  void Update();
  void OnKeyDown(WPARAM key);
  void OnKeyUp(WPARAM key);

  bool IsKeyPressed(int key) const;
  bool IsKeyDown(int key) const;
  bool IsKeyUp(int key) const;

private:
  struct KeyState
  {
    bool pressed = false;
    bool prevPressed = false;
  };

  std::unordered_map<int, KeyState> m_KeyStates;
};