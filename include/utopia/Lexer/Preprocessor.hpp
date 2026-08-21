#pragma once
#include "utopia/Common/Diagnostics.hpp"
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace utopia {

struct CondState {
  bool parentActive;
  bool conditionMet;
  bool currentlyActive;
};

class Preprocessor {
public:
  explicit Preprocessor(std::string_view sourceCode,
                        const std::unordered_set<std::string> &macros = {},
                        DiagnosticsEngine *diags = nullptr,
                        std::string_view filePath = "",
                        bool isFormatting = false);

  std::string process();

private:
  std::string_view source;
  size_t cursor;
  std::unordered_set<std::string> definedMacros;
  std::vector<CondState> condStack;

  DiagnosticsEngine *diags;
  std::string filePath;
  bool isFormatting;
  int line;
  int col;
  int inactiveStartLine;

  bool skipMode() const {
    return !condStack.empty() && !condStack.back().currentlyActive;
  }

  void reportError(std::string_view message);
  void reportWarning(std::string_view message);

  int getUTF8CharLength(unsigned char c);
  void advance();
  void processDirective();
  bool evaluateCondition(std::string_view expr);
};

} // namespace utopia