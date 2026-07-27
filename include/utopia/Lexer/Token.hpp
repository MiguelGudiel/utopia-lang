#pragma once
#include <cstdint>
#include <string_view>

namespace utopia {

enum class TokenType : uint8_t {
  EOF_TOK,
  IDENTIFIER,
  NUMBER,
  CHAR_LITERAL,
  RUNE_LITERAL,
  STRING_LITERAL,
  TYPE_KW,
  CONST_KW,
  ANNOTATION_KW,
  EXTERN_KW,
  REQUIRED_KW,
  ELLIPSIS,
  AS,
  PLUS,
  MINUS,
  STAR,
  SLASH,
  AMPERSAND,
  AT,
  STRUCT_KW,
  CLASS_KW,
  THIS_KW,
  DOT,
  TILDE,
  ASSIGN,
  COLON,
  TRUE_KW,
  FALSE_KW,
  LPAREN,
  RPAREN,
  LBRACE,
  RBRACE,
  COMMA,
  SEMICOLON,
  RETURN,
  COMMENT,
  UNKNOWN
};

/**
 * zero-copy slice of the input source buffer.
 */
struct Token {
  TokenType type;
  std::string_view value;
  int line;
  int column;
};

} // namespace utopia