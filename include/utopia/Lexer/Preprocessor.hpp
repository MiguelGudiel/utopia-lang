#pragma once
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
                        const std::unordered_set<std::string> &macros = {});

  std::string process();

private:
  std::string_view source;
  size_t cursor;
  std::unordered_set<std::string> definedMacros;
  std::vector<CondState> condStack;

  bool skipMode() const {
    return !condStack.empty() && !condStack.back().currentlyActive;
  }

  void advance();
  void processDirective();
  bool evaluateCondition(std::string_view expr);
};

} // namespace utopia