#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace utopia {

enum class TokenType {
  EOF_TOK,
  IDENTIFIER,
  NUMBER,
  FLOAT_LITERAL,
  STRING,
  KW_IMPORT,
  KW_INT,
  KW_FLOAT,
  KW_STRING_TYPE,
  KW_BOOL,
  KW_UINT,
  KW_VOID,
  KW_CONST,
  KW_TRUE,
  KW_FALSE,
  KW_CHAR,
  KW_RETURN,
  KW_NEW,
  KW_DELETE,
  KW_MOVE,
  LPAREN,
  RPAREN,
  LBRACE,
  RBRACE,
  SEMICOLON,
  ASSIGN,
  PLUS,
  MINUS,
  STAR,
  SLASH,
  PERCENT,
  PLUS_PLUS,
  MINUS_MINUS,
  PLUS_EQ,
  MINUS_EQ,
  STAR_EQ,
  SLASH_EQ,
  COMMA,
  AMPERSAND,
  QUESTION,
  BANG,
  AT,       // @
  TILDE,    // ~
  LBRACKET, // [
  RBRACKET, // ]
  KW_NULL,
  KW_IF,
  KW_ELSE,
  KW_WHILE,
  KW_FOR,
  KW_BREAK,
  KW_CONTINUE,
  KW_INLINE,
  KW_FORCE_INLINE,
  KW_STRUCT,
  KW_CLASS,
  KW_PUBLIC,
  KW_PRIVATE,
  KW_THIS,
  KW_REQUIRED,
  DOT, // .
  NEQ, // !=
  EQ,  // ==
  LT,  // <
  GT,  // >
  LTE, // <=
  GTE, // >=
  AND, // &&
  OR,  // ||
  UNKNOWN
};

struct Token {
  TokenType type;
  std::string value;
  int line;
  int column;
};

class Lexer {
public:
  explicit Lexer(std::string_view sourceCode);
  std::vector<Token> tokenize();

private:
  std::string_view source;
  size_t cursor = 0;

  int currentLine = 1;
  int currentColumn = 1;

  void skipWhitespace();
  Token nextToken();

  void advanceCursor();
};

} // namespace utopia