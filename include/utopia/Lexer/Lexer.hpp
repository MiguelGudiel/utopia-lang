#pragma once
#include "utopia/Lexer/Token.hpp"
#include <string_view>
#include <vector>

namespace utopia {

class Lexer {
public:
  explicit Lexer(std::string_view sourceCode)
      : source(sourceCode), cursor(0), line(1), col(1) {}
  std::vector<Token> tokenize();

private:
  std::string_view source;
  size_t cursor;
  int line;
  int col;

  Token nextToken();
  void advance();
  void skipWhitespace();

  /** Returns the length (1‑4 bytes) of a UTF‑8 sequence from its first byte. */
  int getUTF8CharLength(unsigned char c);
};

} // namespace utopia