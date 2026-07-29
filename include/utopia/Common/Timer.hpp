#pragma once
#include "utopia/Common/Logger.hpp"
#include <chrono>
#include <string>

namespace utopia {

class ScopedTimer {
public:
  using Clock = std::chrono::high_resolution_clock;

  ScopedTimer(const std::string &name) : name(name), start(Clock::now()) {}

  ~ScopedTimer() {
    if (Logger::isDebugEnabled()) {
      auto end = Clock::now();
      auto ms = std::chrono::duration<double, std::milli>(end - start).count();
      Logger::debug("[Timing] " + name + ": " + std::to_string(ms) + " ms");
    }
  }

private:
  std::string name;
  Clock::time_point start;
};

} // namespace utopia