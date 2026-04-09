#pragma once
#include "utopia/Lexer/Lexer.hpp"
#include <string>
#include <vector>

namespace utopia {

class Formatter {
public:
  explicit Formatter(const std::vector<Token> &tokens, int tabSize = 2);
  std::string format();

private:
  const std::vector<Token> &tokens;
  int tabSize;
  int indentLevel = 0;
  std::string result;

  void appendIndent();
  bool isPointerStar(size_t index);
  bool isBinaryOp(TokenType type);
  bool isTypeToken(TokenType type);
};

} // namespace utopia