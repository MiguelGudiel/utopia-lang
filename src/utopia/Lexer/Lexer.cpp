#include "utopia/Lexer/Lexer.hpp"
#include <cctype>

namespace utopia {

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;
  Token tok = nextToken();
  while (tok.type != TokenType::EOF_TOK) {
    tokens.push_back(tok);
    tok = nextToken();
  }
  tokens.push_back(tok);
  return tokens;
}

int Lexer::getUTF8CharLength(unsigned char c) {
  if ((c & 0x80) == 0)
    return 1;
  if ((c & 0xE0) == 0xC0)
    return 2;
  if ((c & 0xF0) == 0xE0)
    return 3;
  if ((c & 0xF8) == 0xF0)
    return 4;
  return 1;
}

void Lexer::skipWhitespace() {
  while (cursor < source.length() &&
         std::isspace(static_cast<unsigned char>(source[cursor]))) {
    advance();
  }
}

void Lexer::advance() {
  if (cursor < source.length()) {
    int charLen = getUTF8CharLength(source[cursor]);
    if (source[cursor] == '\n') {
      line++;
      col = 1;
    } else {
      col += charLen;
    }
    cursor += charLen;
  }
}

static bool isTypeKeyword(std::string_view id) {
  return id == "int" || id == "int32" || id == "int64" || id == "int16" ||
         id == "int8" || id == "uint" || id == "uint32" || id == "uint64" ||
         id == "uint16" || id == "uint8" || id == "float" || id == "float32" ||
         id == "float64" || id == "double" || id == "char" || id == "rune" ||
         id == "usize_t" || id == "bool" || id == "void";
}

Token Lexer::nextToken() {
  skipWhitespace();
  if (cursor >= source.length())
    return {TokenType::EOF_TOK, {}, line, col};

  const char *start = &source[cursor];
  int startLine = line;
  int startCol = col;
  unsigned char c = source[cursor];

  if (c == '/') {
    if (cursor + 1 < source.length() && source[cursor + 1] == '/') {
      advance();
      advance();
      while (cursor < source.length() && source[cursor] == ' ')
        advance();
      size_t startStr = cursor;
      while (cursor < source.length() && source[cursor] != '\n')
        advance();
      return {TokenType::COMMENT,
              std::string_view(source.data() + startStr, cursor - startStr),
              startLine, startCol};
    }
    if (cursor + 1 < source.length() && source[cursor + 1] == '*') {
      advance();
      advance();
      while (cursor < source.length() && source[cursor] == ' ')
        advance();
      size_t startStr = cursor;
      while (cursor + 1 < source.length() &&
             !(source[cursor] == '*' && source[cursor + 1] == '/')) {
        advance();
      }
      size_t endStr = cursor;
      if (cursor + 1 < source.length()) {
        advance();
        advance();
      }
      std::string_view val(source.data() + startStr, endStr - startStr);
      if (!val.empty() && val.back() == ' ')
        val.remove_suffix(1);
      return {TokenType::COMMENT, val, startLine, startCol};
    }
  }

  if (c == 'U' && cursor + 1 < source.length() && source[cursor + 1] == '\'') {
    advance();
    advance();
    while (cursor < source.length() && source[cursor] != '\'') {
      if (source[cursor] == '\\')
        advance();
      advance();
    }
    if (cursor < source.length())
      advance();
    return {TokenType::RUNE_LITERAL,
            std::string_view(start, cursor - (start - source.data())),
            startLine, startCol};
  }

  if (c == '\'') {
    advance();
    while (cursor < source.length() && source[cursor] != '\'') {
      if (source[cursor] == '\\')
        advance();
      advance();
    }
    if (cursor < source.length())
      advance();
    return {TokenType::CHAR_LITERAL,
            std::string_view(start, cursor - (start - source.data())),
            startLine, startCol};
  }

  if (c == '"') {
    advance();
    size_t startStr = cursor;
    while (cursor < source.length() && source[cursor] != '"') {
      if (source[cursor] == '\\')
        advance();
      advance();
    }
    size_t len = cursor - startStr;
    if (cursor < source.length()) {
      advance();
    }
    return {TokenType::STRING_LITERAL,
            std::string_view(source.data() + startStr, len), startLine,
            startCol};
  }

  if (std::isalpha(c) || c == '_' || c >= 128) {
    size_t len = 0;
    while (cursor < source.length()) {
      unsigned char nextC = source[cursor];
      if (std::isalnum(nextC) || nextC == '_' || nextC >= 128) {
        int charLen = getUTF8CharLength(nextC);
        len += charLen;
        cursor += charLen;
        col += charLen;
      } else {
        break;
      }
    }
    std::string_view id(start, len);
    if (id == "return")
      return {TokenType::RETURN, id, startLine, startCol};
    if (id == "typedef")
      return {TokenType::TYPEDEF_KW, id, startLine, startCol};
    if (id == "for")
      return {TokenType::FOR_KW, id, startLine, startCol};
    if (id == "while")
      return {TokenType::WHILE_KW, id, startLine, startCol};
    if (id == "if")
      return {TokenType::IF_KW, id, startLine, startCol};
    if (id == "else")
      return {TokenType::ELSE_KW, id, startLine, startCol};
    if (id == "as")
      return {TokenType::AS, id, startLine, startCol};
    if (id == "const")
      return {TokenType::CONST_KW, id, startLine, startCol};
    if (id == "annotation")
      return {TokenType::ANNOTATION_KW, id, startLine, startCol};
    if (id == "extern")
      return {TokenType::EXTERN_KW, id, startLine, startCol};
    if (id == "required")
      return {TokenType::REQUIRED_KW, id, startLine, startCol};
    if (id == "true")
      return {TokenType::TRUE_KW, id, startLine, startCol};
    if (id == "false")
      return {TokenType::FALSE_KW, id, startLine, startCol};
    if (id == "struct")
      return {TokenType::STRUCT_KW, id, startLine, startCol};
    if (id == "class")
      return {TokenType::CLASS_KW, id, startLine, startCol};
    if (id == "this")
      return {TokenType::THIS_KW, id, startLine, startCol};
    if (id == "new")
      return {TokenType::NEW_KW, id, startLine, startCol};
    if (id == "delete")
      return {TokenType::DELETE_KW, id, startLine, startCol};
    if (id == "null")
      return {TokenType::NULL_KW, id, startLine, startCol};
    if (id == "Function")
      return {TokenType::FUNCTION_KW, id, startLine, startCol};
    if (isTypeKeyword(id))
      return {TokenType::TYPE_KW, id, startLine, startCol};
    return {TokenType::IDENTIFIER, id, startLine, startCol};
  }

  if (std::isdigit(c)) {
    size_t startCursor = cursor;

    while (cursor < source.length() && std::isdigit(source[cursor])) {
      advance();
    }

    if (cursor < source.length() && source[cursor] == '.') {
      advance();
      while (cursor < source.length() && std::isdigit(source[cursor])) {
        advance();
      }
    }

    if (cursor < source.length() &&
        (source[cursor] == 'e' || source[cursor] == 'E')) {
      advance();
      if (cursor < source.length() &&
          (source[cursor] == '+' || source[cursor] == '-')) {
        advance();
      }
      while (cursor < source.length() && std::isdigit(source[cursor])) {
        advance();
      }
    }

    while (cursor < source.length()) {
      unsigned char nextC = source[cursor];
      if (nextC == 'f' || nextC == 'F' || nextC == 'u' || nextC == 'U' ||
          nextC == 'l' || nextC == 'L') {
        advance();
      } else {
        break;
      }
    }

    size_t len = cursor - startCursor;
    return {TokenType::NUMBER, std::string_view(start, len), startLine,
            startCol};
  }

  advance();
  switch (c) {
  case '+':
    return {TokenType::PLUS, std::string_view(start, 1), startLine, startCol};
  case '-':
    return {TokenType::MINUS, std::string_view(start, 1), startLine, startCol};
  case '*':
    return {TokenType::STAR, std::string_view(start, 1), startLine, startCol};
  case '/':
    return {TokenType::SLASH, std::string_view(start, 1), startLine, startCol};
  case '&':
    if (cursor < source.length() && source[cursor] == '&') {
      advance();
      return {TokenType::LOGICAL_AND, std::string_view(start, 2), startLine,
              startCol};
    }
    return {TokenType::AMPERSAND, std::string_view(start, 1), startLine,
            startCol};
  case '|':
    if (cursor < source.length() && source[cursor] == '|') {
      advance();
      return {TokenType::LOGICAL_OR, std::string_view(start, 2), startLine,
              startCol};
    }
    return {TokenType::UNKNOWN, std::string_view(start, 1), startLine,
            startCol};
  case '=':
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::EQ, std::string_view(start, 2), startLine, startCol};
    }
    if (cursor < source.length() && source[cursor] == '>') {
      advance();
      return {TokenType::ARROW, std::string_view(start, 2), startLine,
              startCol};
    }
    return {TokenType::ASSIGN, std::string_view(start, 1), startLine, startCol};
  case '!':
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::NEQ, std::string_view(start, 2), startLine, startCol};
    }
    return {TokenType::BANG, std::string_view(start, 1), startLine, startCol};
  case '<':
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::LE, std::string_view(start, 2), startLine, startCol};
    }
    return {TokenType::LT, std::string_view(start, 1), startLine, startCol};
  case '>':
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::GE, std::string_view(start, 2), startLine, startCol};
    }
    return {TokenType::GT, std::string_view(start, 1), startLine, startCol};
  case '@':
    return {TokenType::AT, std::string_view(start, 1), startLine, startCol};
  case '(':
    return {TokenType::LPAREN, std::string_view(start, 1), startLine, startCol};
  case ')':
    return {TokenType::RPAREN, std::string_view(start, 1), startLine, startCol};
  case '[':
    return {TokenType::LBRACKET, std::string_view(start, 1), startLine,
            startCol};
  case ']':
    return {TokenType::RBRACKET, std::string_view(start, 1), startLine,
            startCol};
  case '{':
    return {TokenType::LBRACE, std::string_view(start, 1), startLine, startCol};
  case '}':
    return {TokenType::RBRACE, std::string_view(start, 1), startLine, startCol};
  case ',':
    return {TokenType::COMMA, std::string_view(start, 1), startLine, startCol};
  case ':':
    return {TokenType::COLON, std::string_view(start, 1), startLine, startCol};
  case '.':
    if (cursor + 1 < source.length() && source[cursor] == '.' &&
        source[cursor + 1] == '.') {
      advance();
      advance();
      return {TokenType::ELLIPSIS, std::string_view(start, 3), startLine,
              startCol};
    }
    return {TokenType::DOT, std::string_view(start, 1), startLine, startCol};
  case '~':
    return {TokenType::TILDE, std::string_view(start, 1), startLine, startCol};
  case ';':
    return {TokenType::SEMICOLON, std::string_view(start, 1), startLine,
            startCol};
  default:
    return {TokenType::UNKNOWN, std::string_view(start, 1), startLine,
            startCol};
  }
}

} // namespace utopia