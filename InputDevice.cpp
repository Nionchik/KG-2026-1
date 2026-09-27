#include "InputDevice.h"

InputDevice::InputDevice()
{
  int keys[] = { VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_SPACE, VK_ESCAPE,
                 'W', 'A', 'S', 'D', 'Q', 'E', 'R', 'F' };
  for (int key : keys)
    m_KeyStates[key] = KeyState();
}

void InputDevice::Update()
{
  for (auto& pair : m_KeyStates)
    pair.second.prevPressed = pair.second.pressed;
}

void InputDevice::OnKeyDown(WPARAM key)
{
  int keyInt = static_cast<int>(key);
  if (m_KeyStates.find(keyInt) != m_KeyStates.end())
    m_KeyStates[keyInt].pressed = true;
}

void InputDevice::OnKeyUp(WPARAM key)
{
  int keyInt = static_cast<int>(key);
  if (m_KeyStates.find(keyInt) != m_KeyStates.end())
    m_KeyStates[keyInt].pressed = false;
}

bool InputDevice::IsKeyPressed(int key) const
{
  auto it = m_KeyStates.find(key);
  return it != m_KeyStates.end() && it->second.pressed;
}

bool InputDevice::IsKeyDown(int key) const
{
  auto it = m_KeyStates.find(key);
  return it != m_KeyStates.end() && it->second.pressed && !it->second.prevPressed;
}

bool InputDevice::IsKeyUp(int key) const
{
  auto it = m_KeyStates.find(key);
  return it != m_KeyStates.end() && !it->second.pressed && it->second.prevPressed;
}