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

Preprocessor::Preprocessor(std::string_view sourceCode,
                           const std::unordered_set<std::string> &macros)
    : source(sourceCode), cursor(0), definedMacros(macros) {

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

void Preprocessor::advance() {
  if (cursor < source.length()) {
    cursor++;
  }
}

bool Preprocessor::evaluateCondition(std::string_view expr) {
  PPExprParser parser(expr, definedMacros);
  return parser.eval();
}

void Preprocessor::processDirective() {
  advance();

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
}

std::string Preprocessor::process() {
  std::string output;
  output.reserve(source.length());

  bool isStartOfLine = true;

  while (cursor < source.length()) {
    char c = source[cursor];

    if (c == '\n') {
      output += '\n';
      isStartOfLine = true;
      advance();
      continue;
    }

    if (std::isspace(c)) {
      output += skipMode() ? ' ' : c;
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
          output += ' ';
        }
      }
      continue;
    }

    isStartOfLine = false;

    if (skipMode()) {
      output += ' ';
    } else {
      output += c;
    }
    advance();
  }

  if (!condStack.empty()) {
    Logger::error("Preprocessor error: Unclosed #if block at end of file");
  }

  return output;
}

} // namespace utopia