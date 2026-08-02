#pragma once
#include "utopia/Lexer/Token.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace utopia {

class Lexer {
public:
  explicit Lexer(std::string_view sourceCode);
  std::vector<Token> tokenize();

private:
  std::string_view source;
  size_t cursor;
  int line;
  int col;

  Token nextToken();
  Token parseToken();
  void advance();
  void skipWhitespace();

  int getUTF8CharLength(unsigned char c);
};

} // namespace utopia