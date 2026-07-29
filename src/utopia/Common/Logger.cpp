#include "utopia/Common/Logger.hpp"
#include <iostream>

namespace utopia {

// Set default level based on the specific debug logs macro.
// In release mode, defaults to Info (ignoring Debug logs).
#ifdef UTOPIA_DEBUG_LOGS
LogLevel Logger::currentLevel = LogLevel::Debug;
#else
LogLevel Logger::currentLevel = LogLevel::Info;
#endif

void Logger::setLevel(LogLevel level) { currentLevel = level; }

bool Logger::isDebugEnabled() { return currentLevel <= LogLevel::Debug; }

void Logger::debug(std::string_view message) {
  if (currentLevel <= LogLevel::Debug) {
    std::cout << message << "\n";
  }
}

void Logger::info(std::string_view message) {
  if (currentLevel <= LogLevel::Info) {
    std::cout << message << "\n";
  }
}

void Logger::warning(std::string_view message) {
  if (currentLevel <= LogLevel::Warning) {
    std::cerr << message << "\n";
  }
}

void Logger::error(std::string_view message) {
  if (currentLevel <= LogLevel::Error) {
    std::cerr << message << "\n";
  }
}

} // namespace utopia