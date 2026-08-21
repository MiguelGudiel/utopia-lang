#pragma once
#include "utopia/Common/Diagnostics.hpp"
#include "utopia/Lexer/Token.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace utopia {

class Lexer {
public:
  /* 'diags' is optional: drivers that can surface diagnostics (ModuleLoader)
   * pass it so unterminated literals and comments are reported as real
   * errors; standalone users (formatter previews) may pass nullptr. */
  Lexer(std::string_view sourceCode, DiagnosticsEngine *diags = nullptr,
        std::string filePath = "");
  std::vector<Token> tokenize();

private:
  std::string_view source;
  size_t cursor;
  int line;
  int col;

  DiagnosticsEngine *diags;
  std::string filePath;

  Token nextToken();
  Token parseToken();
  void advance();
  void skipWhitespace();
  void reportError(int line, int col, std::string_view message);

  int getUTF8CharLength(unsigned char c);
};

} // namespace utopia