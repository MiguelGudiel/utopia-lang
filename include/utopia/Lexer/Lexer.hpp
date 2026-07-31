#pragma once
#include "utopia/Lexer/Token.hpp"
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

class Lexer {
public:
  explicit Lexer(std::string_view sourceCode,
                 const std::unordered_set<std::string> &macros = {});
  std::vector<Token> tokenize();

private:
  std::string_view source;
  size_t cursor;
  int line;
  int col;

  std::unordered_set<std::string> definedMacros;
  std::vector<CondState> condStack;

  bool skipMode() const {
    return !condStack.empty() && !condStack.back().currentlyActive;
  }

  Token nextToken();
  Token parseToken();
  void advance();
  void skipWhitespace();

  /** Returns the length (1‑4 bytes) of a UTF‑8 sequence from its first byte. */
  int getUTF8CharLength(unsigned char c);

  void processDirective();
  bool evaluateCondition(std::string_view expr);
};

} // namespace utopia