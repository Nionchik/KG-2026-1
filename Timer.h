#pragma once
#include <chrono>

class Timer
{
public:
  Timer();
  void Tick();
  float GetDeltaTime() const { return m_DeltaTime; }
  float GetTotalTime() const { return m_TotalTime; }

private:
  std::chrono::high_resolution_clock::time_point m_StartTime;
  std::chrono::high_resolution_clock::time_point m_PreviousTime;
  float m_DeltaTime = 0.0f;
  float m_TotalTime = 0.0f;
};