#include "utopia/Lexer/Preprocessor.hpp"
#include "utopia/Common/Logger.hpp"
#include <cctype>
#include <stdexcept>

namespace utopia {

class PPExprParser {
  std::string_view str;
  size_t pos = 0;
  const std::unordered_set<std::string> &macros;

  void skipWhitespace() {
    while (pos < str.length() &&
             std::isspace(static_cast<unsigned char>(str[pos]))) {
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
    while (pos < str.length() &&
           (std::isalnum(static_cast<unsigned char>(str[pos])) ||
            str[pos] == '_')) {
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

Preprocessor::Preprocessor(std::string_view sourceCode,
                           const std::unordered_set<std::string> &macros,
                           DiagnosticsEngine *diags, std::string_view filePath,
                           bool isFormatting)
    : source(sourceCode), cursor(0), definedMacros(macros), diags(diags),
      filePath(std::string(filePath)), isFormatting(isFormatting), line(1),
      col(1), inactiveStartLine(-1) {}

int Preprocessor::getUTF8CharLength(unsigned char c) {
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

void Preprocessor::advance() {
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

bool Preprocessor::evaluateCondition(std::string_view expr) {
  PPExprParser parser(expr, definedMacros);
  return parser.eval();
}

void Preprocessor::processDirective() {
  int directiveLine = this->line;
  bool wasSkipping = skipMode();

  advance();

  auto skipSpaces = [&]() {
    while (cursor < source.length() &&
           (source[cursor] == ' ' || source[cursor] == '\t')) {
      advance();
    }
  };

  skipSpaces();

  size_t kwStart = cursor;
  while (cursor < source.length() &&
           std::isalpha(static_cast<unsigned char>(source[cursor]))) {
    advance();
  }
  std::string_view kw(source.data() + kwStart, cursor - kwStart);

  skipSpaces();
  size_t argStart = cursor;
  while (cursor < source.length() && source[cursor] != '\n') {
    advance();
  }
  std::string_view args(source.data() + argStart, cursor - argStart);

  size_t commentPos = args.find("//");
  if (commentPos != std::string_view::npos) {
    args = args.substr(0, commentPos);
  }

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

  bool isSkipping = skipMode();

  /* Report the inactive block range strictly excluding the directive lines */
  if (wasSkipping && diags && !isFormatting && inactiveStartLine != -1 &&
      directiveLine > inactiveStartLine) {
    // diags->report({DiagLevel::Inactive, inactiveStartLine, 1, 0,
    //                "Inactive preprocessor block", filePath, directiveLine});
  }

  if (isSkipping) {
    inactiveStartLine = directiveLine + 1;
  } else {
    inactiveStartLine = -1;
  }
}

std::string Preprocessor::process() {
  std::string output;
  output.reserve(source.length());

  bool isStartOfLine = true;

  while (cursor < source.length()) {
    char c = source[cursor];
    unsigned char uc = static_cast<unsigned char>(c);

    if (c == '\n') {
      output += '\n';
      isStartOfLine = true;
      advance();
      continue;
    }

    if (std::isspace(uc)) {
      output += (skipMode() && !isFormatting) ? ' ' : c;
      advance();
      continue;
    }

    if (isStartOfLine && c == '#') {
      size_t start = cursor;
      processDirective();
      size_t end = cursor;

      for (size_t i = start; i < end; ++i) {
        if (source[i] == '\n') {
          output += '\n';
        } else {
          output += isFormatting ? source[i] : ' ';
        }
      }
      continue;
    }

    isStartOfLine = false;

    /* Copy the whole UTF-8 character: advance() skips the full sequence, so
     * copying a single byte would drop the continuation bytes of every
     * multi-byte character and corrupt the output. */
    int charLen = getUTF8CharLength(uc);
    if (skipMode() && !isFormatting) {
      for (int i = 0; i < charLen; ++i)
        output += ' ';
    } else {
      for (int i = 0; i < charLen && cursor + i < source.length(); ++i)
        output += source[cursor + i];
    }
    advance();
  }

  if (skipMode() && diags && !isFormatting && inactiveStartLine != -1 &&
      line >= inactiveStartLine) {
    // diags->report({DiagLevel::Inactive, inactiveStartLine, 1, 0,
    //                "Inactive preprocessor block", filePath, line + 1});
  }

  if (!condStack.empty()) {
    Logger::error("Preprocessor error: Unclosed #if block at end of file");
  }

  return output;
}

} // namespace utopia