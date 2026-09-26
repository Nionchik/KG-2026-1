#include "Timer.h"

Timer::Timer()
{
  m_StartTime = std::chrono::high_resolution_clock::now();
  m_PreviousTime = m_StartTime;
}

void Timer::Tick()
{
  auto currentTime = std::chrono::high_resolution_clock::now();
  auto delta = std::chrono::duration<float>(currentTime - m_PreviousTime);
  m_DeltaTime = delta.count();
  auto total = std::chrono::duration<float>(currentTime - m_StartTime);
  m_TotalTime = total.count();
  m_PreviousTime = currentTime;
}