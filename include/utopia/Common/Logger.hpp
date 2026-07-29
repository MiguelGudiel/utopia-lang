#pragma once
#include <string_view>

namespace utopia {

/**
 * Defines standard logging severity levels.
 */
enum class LogLevel { Debug, Info, Warning, Error };

/**
 * Runtime logging utility.
 * Completely replaces compile-time #ifdef checks at the call sites,
 * evaluating log visibility at runtime to keep codebase clean and readable.
 */
class Logger {
private:
  static LogLevel currentLevel;

public:
  static void setLevel(LogLevel level);
  static bool isDebugEnabled();

  static void debug(std::string_view message);
  static void info(std::string_view message);
  static void warning(std::string_view message);
  static void error(std::string_view message);
};

} // namespace utopia