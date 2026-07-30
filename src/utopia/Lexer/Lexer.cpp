#include "utopia/Lexer/Lexer.hpp"
#include "utopia/Common/Logger.hpp"
#include <cctype>
#include <stdexcept>

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

Lexer::Lexer(std::string_view sourceCode)
    : source(sourceCode), cursor(0), line(1), col(1) {

  // Register default target platform macros
#if defined(_WIN32)
  definedMacros.insert("_WIN32");
#elif defined(__APPLE__)
  definedMacros.insert("__APPLE__");
#elif defined(__ANDROID__)
  definedMacros.insert("__ANDROID__");
#elif defined(__linux__) || defined(__gnu_linux__)
  definedMacros.insert("__gnu_linux__");
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  definedMacros.insert("__BSD__");
  definedMacros.insert("__FreeBSD__");
  definedMacros.insert("__NetBSD__");
  definedMacros.insert("__OpenBSD__");
#endif

  // Register default target architecture macros
#if defined(__x86_64__) || defined(_M_X64)
  definedMacros.insert("x64");
  definedMacros.insert("x86_64");
#elif defined(__i386) || defined(_M_IX86)
  definedMacros.insert("x86");
#elif defined(__aarch64__) || defined(_M_ARM64)
  definedMacros.insert("arm64");
#elif defined(__arm__) || defined(_M_ARM)
  definedMacros.insert("arm");
#endif
}

static bool isTypeKeyword(std::string_view id) {
  return id == "int" || id == "int32" || id == "int64" || id == "int16" ||
         id == "int8" || id == "uint" || id == "uint32" || id == "uint64" ||
         id == "uint16" || id == "uint8" || id == "float" || id == "float32" ||
         id == "float64" || id == "double" || id == "char" || id == "rune" ||
         id == "usize_t" || id == "bool" || id == "void";
}

Token Lexer::nextToken() {
  while (true) {
    skipWhitespace();

    if (cursor >= source.length()) {
      if (!condStack.empty()) {
        Logger::error("Preprocessor error: Unclosed #if block at end of file");
      }
      return {TokenType::EOF_TOK, {}, line, col};
    }

    if (source[cursor] == '#') {
      processDirective();
      continue;
    }

    Token tok = parseToken();

    if (skipMode()) {
      if (tok.type == TokenType::EOF_TOK) {
        return tok;
      }
      // Silently consume the token and read the next one if the block is
      // disabled
      continue;
    }

    return tok;
  }
}

Token Lexer::parseToken() {
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
    if (id == "enum")
      return {TokenType::ENUM_KW, id, startLine, startCol};
    if (id == "for")
      return {TokenType::FOR_KW, id, startLine, startCol};
    if (id == "while")
      return {TokenType::WHILE_KW, id, startLine, startCol};
    if (id == "break")
      return {TokenType::BREAK_KW, id, startLine, startCol};
    if (id == "continue")
      return {TokenType::CONTINUE_KW, id, startLine, startCol};
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
    if (id == "static")
      return {TokenType::STATIC_KW, id, startLine, startCol};
    if (id == "required")
      return {TokenType::REQUIRED_KW, id, startLine, startCol};
    if (id == "public")
      return {TokenType::PUBLIC_KW, id, startLine, startCol};
    if (id == "private")
      return {TokenType::PRIVATE_KW, id, startLine, startCol};
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
    if (id == "operator")
      return {TokenType::OPERATOR_KW, id, startLine, startCol};
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
    if (cursor < source.length() && source[cursor] == '+') {
      advance();
      return {TokenType::PLUS_PLUS, std::string_view(start, 2), startLine,
              startCol};
    }
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::PLUS_EQ, std::string_view(start, 2), startLine,
              startCol};
    }
    return {TokenType::PLUS, std::string_view(start, 1), startLine, startCol};
  case '-':
    if (cursor < source.length() && source[cursor] == '-') {
      advance();
      return {TokenType::MINUS_MINUS, std::string_view(start, 2), startLine,
              startCol};
    }
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::MINUS_EQ, std::string_view(start, 2), startLine,
              startCol};
    }
    return {TokenType::MINUS, std::string_view(start, 1), startLine, startCol};
  case '*':
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::STAR_EQ, std::string_view(start, 2), startLine,
              startCol};
    }
    return {TokenType::STAR, std::string_view(start, 1), startLine, startCol};
  case '/':
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::SLASH_EQ, std::string_view(start, 2), startLine,
              startCol};
    }
    return {TokenType::SLASH, std::string_view(start, 1), startLine, startCol};
  case '%':
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::PERCENT_EQ, std::string_view(start, 2), startLine,
              startCol};
    }
    return {TokenType::PERCENT, std::string_view(start, 1), startLine,
            startCol};
  case '&':
    if (cursor < source.length() && source[cursor] == '&') {
      advance();
      return {TokenType::LOGICAL_AND, std::string_view(start, 2), startLine,
              startCol};
    }
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::AMPERSAND_EQ, std::string_view(start, 2), startLine,
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
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::PIPE_EQ, std::string_view(start, 2), startLine,
              startCol};
    }
    return {TokenType::PIPE, std::string_view(start, 1), startLine, startCol};
  case '^':
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::CARET_EQ, std::string_view(start, 2), startLine,
              startCol};
    }
    return {TokenType::CARET, std::string_view(start, 1), startLine, startCol};
  case '<':
    if (cursor < source.length() && source[cursor] == '<') {
      advance();
      if (cursor < source.length() && source[cursor] == '=') {
        advance();
        return {TokenType::LSHIFT_EQ, std::string_view(start, 3), startLine,
                startCol};
      }
      return {TokenType::LSHIFT, std::string_view(start, 2), startLine,
              startCol};
    }
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::LE, std::string_view(start, 2), startLine, startCol};
    }
    return {TokenType::LT, std::string_view(start, 1), startLine, startCol};
  case '>':
    if (cursor < source.length() && source[cursor] == '>') {
      advance();
      if (cursor < source.length() && source[cursor] == '=') {
        advance();
        return {TokenType::RSHIFT_EQ, std::string_view(start, 3), startLine,
                startCol};
      }
      return {TokenType::RSHIFT, std::string_view(start, 2), startLine,
              startCol};
    }
    if (cursor < source.length() && source[cursor] == '=') {
      advance();
      return {TokenType::GE, std::string_view(start, 2), startLine, startCol};
    }
    return {TokenType::GT, std::string_view(start, 1), startLine, startCol};
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

class PPExprParser {
  std::string_view str;
  size_t pos = 0;
  const std::unordered_set<std::string> &macros;

  void skipWhitespace() {
    while (pos < str.length() && std::isspace(str[pos])) {
      pos++;
    }
  }

  bool match(std::string_view tok) {
    skipWhitespace();
    if (pos + tok.length() <= str.length() &&
        str.substr(pos, tok.length()) == tok) {
      pos += tok.length();
      return true;
    }
    return false;
  }

  std::string_view matchId() {
    skipWhitespace();
    size_t start = pos;
    while (pos < str.length() && (std::isalnum(str[pos]) || str[pos] == '_')) {
      pos++;
    }
    return str.substr(start, pos - start);
  }

  bool parsePrimary() {
    skipWhitespace();
    if (match("!")) {
      return !parsePrimary();
    }
    if (match("(")) {
      bool val = parseOrExpr();
      match(")");
      return val;
    }
    std::string_view id = matchId();
    if (!id.empty()) {
      if (id == "true")
        return true;
      if (id == "false")
        return false;
      return macros.count(std::string(id)) > 0;
    }
    return false;
  }

  bool parseEquality() {
    bool left = parsePrimary();
    while (true) {
      if (match("==")) {
        left = (left == parsePrimary());
      } else if (match("!=")) {
        left = (left != parsePrimary());
      } else {
        break;
      }
    }
    return left;
  }

  bool parseAndExpr() {
    bool left = parseEquality();
    while (match("&&")) {
      bool right = parseEquality();
      left = left && right;
    }
    return left;
  }

  bool parseOrExpr() {
    bool left = parseAndExpr();
    while (match("||")) {
      bool right = parseAndExpr();
      left = left || right;
    }
    return left;
  }

public:
  PPExprParser(std::string_view s, const std::unordered_set<std::string> &m)
      : str(s), macros(m) {}

  bool eval() { return parseOrExpr(); }
};

bool Lexer::evaluateCondition(std::string_view expr) {
  PPExprParser parser(expr, definedMacros);
  return parser.eval();
}

void Lexer::processDirective() {
  advance(); // Consume '#'

  auto skipSpaces = [&]() {
    while (cursor < source.length() &&
           (source[cursor] == ' ' || source[cursor] == '\t')) {
      advance();
    }
  };

  skipSpaces();

  size_t kwStart = cursor;
  while (cursor < source.length() && std::isalpha(source[cursor])) {
    advance();
  }
  std::string_view kw(source.data() + kwStart, cursor - kwStart);

  skipSpaces();
  size_t argStart = cursor;
  while (cursor < source.length() && source[cursor] != '\n') {
    advance();
  }
  std::string_view args(source.data() + argStart, cursor - argStart);

  // Strip single-line comments from arguments, keeping C# inline compatibility
  size_t commentPos = args.find("//");
  if (commentPos != std::string_view::npos) {
    args = args.substr(0, commentPos);
  }

  // Trim trailing whitespaces / carriage returns
  while (!args.empty() &&
         (args.back() == ' ' || args.back() == '\r' || args.back() == '\t')) {
    args.remove_suffix(1);
  }

  if (kw == "if") {
    bool pActive = condStack.empty() ? true : condStack.back().currentlyActive;
    bool cond = pActive && evaluateCondition(args);
    condStack.push_back({pActive, cond, cond});
  } else if (kw == "elif") {
    if (condStack.empty()) {
      Logger::error("Preprocessor error: #elif without #if");
      return;
    }
    auto &state = condStack.back();
    if (state.parentActive && !state.conditionMet) {
      bool cond = evaluateCondition(args);
      if (cond) {
        state.conditionMet = true;
        state.currentlyActive = true;
      } else {
        state.currentlyActive = false;
      }
    } else {
      state.currentlyActive = false;
    }
  } else if (kw == "else") {
    if (condStack.empty()) {
      Logger::error("Preprocessor error: #else without #if");
      return;
    }
    auto &state = condStack.back();
    if (state.parentActive && !state.conditionMet) {
      state.conditionMet = true;
      state.currentlyActive = true;
    } else {
      state.currentlyActive = false;
    }
  } else if (kw == "endif") {
    if (condStack.empty()) {
      Logger::error("Preprocessor error: #endif without #if");
      return;
    }
    condStack.pop_back();
  } else if (kw == "define") {
    if (!skipMode() && !args.empty()) {
      definedMacros.insert(std::string(args));
    }
  } else if (kw == "undef") {
    if (!skipMode() && !args.empty()) {
      definedMacros.erase(std::string(args));
    }
  } else if (kw == "error") {
    if (!skipMode()) {
      Logger::error(std::string(args));
      throw std::runtime_error("Preprocessor #error: " + std::string(args));
    }
  } else if (kw == "warning") {
    if (!skipMode()) {
      Logger::warning(std::string(args));
    }
  } else {
    if (!skipMode()) {
      Logger::warning("Unknown preprocessor directive: " + std::string(kw));
    }
  }
}

} // namespace utopia