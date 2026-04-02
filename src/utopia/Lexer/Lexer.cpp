// File: src/utopia/Lexer/Lexer.cpp
#include "utopia/Lexer/Lexer.hpp"
#include <cctype>

namespace utopia {

Lexer::Lexer(std::string_view sourceCode) : source(sourceCode) {}

void Lexer::advanceCursor() {
  if (cursor < source.length()) {
    if (source[cursor] == '\n') {
      currentLine++;
      currentColumn = 1;
    } else {
      currentColumn++;
    }
    cursor++;
  }
}

void Lexer::skipWhitespace() {
  while (cursor < source.length()) {
    if (std::isspace(static_cast<unsigned char>(source[cursor]))) {
      advanceCursor();
    } else if (cursor + 1 < source.length() && source[cursor] == '/' &&
               source[cursor + 1] == '/') {
      while (cursor < source.length() && source[cursor] != '\n') {
        advanceCursor();
      }
    } else {
      break;
    }
  }
}

Token Lexer::nextToken() {
  skipWhitespace();

  int startLine = currentLine;
  int startCol = currentColumn;

  if (cursor >= source.length())
    return {TokenType::EOF_TOK, "", currentLine, currentColumn};

  char c = source[cursor];

  auto isUtf8Alpha = [](unsigned char c) -> bool {
    return std::isalpha(c) || c >= 128;
  };
  auto isUtf8Alnum = [](unsigned char c) -> bool {
    return std::isalnum(c) || c >= 128;
  };

  if (isUtf8Alpha(static_cast<unsigned char>(c)) || c == '_') {
    std::string val;
    while (cursor < source.length() &&
           (isUtf8Alnum(static_cast<unsigned char>(source[cursor])) ||
            source[cursor] == '_')) {
      val += source[cursor];
      advanceCursor();
    }
    if (val == "import")
      return {TokenType::KW_IMPORT, val, startLine, startCol};
    if (val == "int")
      return {TokenType::KW_INT, val, startLine, startCol};
    if (val == "float")
      return {TokenType::KW_FLOAT, val, startLine, startCol};
    if (val == "String")
      return {TokenType::KW_STRING_TYPE, val, startLine, startCol};
    if (val == "bool")
      return {TokenType::KW_BOOL, val, startLine, startCol};
    if (val == "uint")
      return {TokenType::KW_UINT, val, startLine, startCol};
    if (val == "void")
      return {TokenType::KW_VOID, val, startLine, startCol};
    if (val == "return")
      return {TokenType::KW_RETURN, val, startLine, startCol};
    if (val == "const")
      return {TokenType::KW_CONST, val, startLine, startCol};
    if (val == "true")
      return {TokenType::KW_TRUE, val, startLine, startCol};
    if (val == "false")
      return {TokenType::KW_FALSE, val, startLine, startCol};
    if (val == "new")
      return {TokenType::KW_NEW, val, startLine, startCol};
    if (val == "delete")
      return {TokenType::KW_DELETE, val, startLine, startCol};
    if (val == "null")
      return {TokenType::KW_NULL, val, startLine, startCol};
    if (val == "if")
      return {TokenType::KW_IF, val, startLine, startCol};
    if (val == "else")
      return {TokenType::KW_ELSE, val, startLine, startCol};
    if (val == "while")
      return {TokenType::KW_WHILE, val, startLine, startCol};
    if (val == "for")
      return {TokenType::KW_FOR, val, startLine, startCol};
    if (val == "break")
      return {TokenType::KW_BREAK, val, startLine, startCol};
    if (val == "continue")
      return {TokenType::KW_CONTINUE, val, startLine, startCol};
    if (val == "inline")
      return {TokenType::KW_INLINE, val, startLine, startCol};
    if (val == "force_inline")
      return {TokenType::KW_FORCE_INLINE, val, startLine, startCol};
    if (val == "struct")
      return {TokenType::KW_STRUCT, val, startLine, startCol};
    if (val == "class")
      return {TokenType::KW_CLASS, val, startLine, startCol};
    if (val == "public")
      return {TokenType::KW_PUBLIC, val, startLine, startCol};
    if (val == "private")
      return {TokenType::KW_PRIVATE, val, startLine, startCol};
    if (val == "this")
      return {TokenType::KW_THIS, val, startLine, startCol};
    if (val == "required")
      return {TokenType::KW_REQUIRED, val, startLine, startCol};
    return {TokenType::IDENTIFIER, val, startLine, startCol};
  }

  if (std::isdigit(static_cast<unsigned char>(c))) {
    std::string val;
    bool hasDot = false;
    while (cursor < source.length() &&
           (std::isdigit(static_cast<unsigned char>(source[cursor])) ||
            source[cursor] == '.')) {
      if (source[cursor] == '.') {
        if (hasDot)
          break;
        hasDot = true;
      }
      val += source[cursor];
      advanceCursor();
    }
    return {hasDot ? TokenType::FLOAT_LITERAL : TokenType::NUMBER, val,
            startLine, startCol};
  }

  if (c == '"' || c == '\'') {
    char quote = c;
    advanceCursor();
    std::string val;
    while (cursor < source.length() && source[cursor] != quote) {
      val += source[cursor];
      advanceCursor();
    }
    advanceCursor();
    return {TokenType::STRING, val, startLine, startCol};
  }

  advanceCursor();
  switch (c) {
  case '(':
    return {TokenType::LPAREN, "(", startLine, startCol};
  case ')':
    return {TokenType::RPAREN, ")", startLine, startCol};
  case '{':
    return {TokenType::LBRACE, "{", startLine, startCol};
  case '}':
    return {TokenType::RBRACE, "}", startLine, startCol};
  case ';':
    return {TokenType::SEMICOLON, ";", startLine, startCol};
  case '+':
    return {TokenType::PLUS, "+", startLine, startCol};
  case '-':
    return {TokenType::MINUS, "-", startLine, startCol};
  case '*':
    return {TokenType::STAR, "*", startLine, startCol};
  case '/':
    return {TokenType::SLASH, "/", startLine, startCol};
  case '%':
    return {TokenType::PERCENT, "%", startLine, startCol};
  case ',':
    return {TokenType::COMMA, ",", startLine, startCol};
  case '?':
    return {TokenType::QUESTION, "?", startLine, startCol};
  case '=':
    if (cursor < source.length() && source[cursor] == '=') {
      advanceCursor();
      return {TokenType::EQ, "==", startLine, startCol};
    }
    return {TokenType::ASSIGN, "=", startLine, startCol};
  case '!':
    if (cursor < source.length() && source[cursor] == '=') {
      advanceCursor();
      return {TokenType::NEQ, "!=", startLine, startCol};
    }
    return {TokenType::BANG, "!", startLine, startCol};
  case '<':
    if (cursor < source.length() && source[cursor] == '=') {
      advanceCursor();
      return {TokenType::LTE, "<=", startLine, startCol};
    }
    return {TokenType::LT, "<", startLine, startCol};
  case '>':
    if (cursor < source.length() && source[cursor] == '=') {
      advanceCursor();
      return {TokenType::GTE, ">=", startLine, startCol};
    }
    return {TokenType::GT, ">", startLine, startCol};
  case '&':
    if (cursor < source.length() && source[cursor] == '&') {
      advanceCursor();
      return {TokenType::AND, "&&", startLine, startCol};
    }
    return {TokenType::AMPERSAND, "&", startLine, startCol};
  case '|':
    if (cursor < source.length() && source[cursor] == '|') {
      advanceCursor();
      return {TokenType::OR, "||", startLine, startCol};
    }
    return {TokenType::UNKNOWN, "|", startLine, startCol};
  case '.':
    return {TokenType::DOT, ".", startLine, startCol};
  case '@':
    return {TokenType::AT, "@", startLine, startCol};
  case '~':
    return {TokenType::TILDE, "~", startLine, startCol};
  default:
    return {TokenType::UNKNOWN, std::string(1, c), startLine, startCol};
  }
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;
  Token tok = nextToken();
  while (tok.type != TokenType::EOF_TOK) {
    tokens.push_back(tok);
    tok = nextToken();
  }
  tokens.push_back({TokenType::EOF_TOK, ""});
  return tokens;
}
} // namespace utopia