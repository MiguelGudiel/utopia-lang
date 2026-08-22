#include "utopia/Common/Warnings.hpp"
#include <cctype>
#include <cstdint>

namespace utopia {

static const struct WarningMeta {
  WarningKind kind;
  const char *name;
  const char *description;
  bool hasFix;
} kWarnings[] = {
    {WarningKind::UnusedImport,
     "unused_import",
     "Imported module is never referenced",
     true},
    {WarningKind::UnusedFunction,
     "unused_function",
     "Private function is never called",
     true},
    {WarningKind::UnusedMethod,
     "unused_method",
     "Private method is never called",
     true},
    {WarningKind::UnusedVariable,
     "unused_variable",
     "Variable is declared but never used",
     true},
    {WarningKind::UnusedField,
     "unused_field",
     "Private field is never referenced",
     true},
    {WarningKind::UnusedParameter,
     "unused_parameter",
     "Parameter is never used in the body",
     true},
    {WarningKind::UnusedUsing,
     "unused_using",
     "Using directive never resolves a referenced symbol",
     true},
    {WarningKind::UnusedType,
     "unused_type",
     "Private record/enum/alias is never referenced",
     true},
    {WarningKind::NodiscardIgnored,
     "nodiscard_ignored",
     "Return value of an @nodiscard function is ignored",
     false},
    {WarningKind::Deprecated,
     "deprecated",
     "Use of a deprecated declaration",
     false},
    {WarningKind::ImplicitCast,
     "implicit_cast",
     "Implicit narrowing or lossy conversion",
     false},
    {WarningKind::UninitializedVariable,
     "uninitialized_variable",
     "Use of a variable before it is initialized",
     false},
};

const char *warningName(WarningKind kind) {
  for (const auto &m : kWarnings) {
    if (m.kind == kind)
      return m.name;
  }
  return "";
}

const char *warningDescription(WarningKind kind) {
  for (const auto &m : kWarnings) {
    if (m.kind == kind)
      return m.description;
  }
  return "";
}

bool warningHasFix(WarningKind kind) {
  for (const auto &m : kWarnings) {
    if (m.kind == kind)
      return m.hasFix;
  }
  return false;
}

WarningKind warningFromName(std::string_view name) {
  for (const auto &m : kWarnings) {
    if (name == m.name)
      return m.kind;
  }
  return WarningKind::None;
}

const std::vector<WarningKind> &allWarningKinds() {
  static const std::vector<WarningKind> kinds = [] {
    std::vector<WarningKind> out;
    for (const auto &m : kWarnings)
      out.push_back(m.kind);
    return out;
  }();
  return kinds;
}

namespace {

/* Parses the text after '@' in a directive comment. Returns false when the
 * line does not carry a directive; on success 'isFileScope' tells whether the
 * plural form was used and 'names' receives the comma/space separated warning
 * names. */
bool parseDirectiveLine(std::string_view line, bool &isFileScope,
                        std::vector<std::string> &names) {
  size_t at = line.find('@');
  if (at == std::string_view::npos)
    return false;

  std::string_view rest = line.substr(at + 1);
  size_t start = rest.find_first_not_of(" \t");
  if (start == std::string_view::npos)
    return false;
  rest = rest.substr(start);

  std::string_view keyword = "ignore-warnings";
  if (rest.starts_with(keyword)) {
    isFileScope = true;
  } else {
    keyword = "ignore-warning";
    if (!rest.starts_with(keyword))
      return false;
    isFileScope = false;
  }
  rest = rest.substr(keyword.size());

  while (true) {
    size_t begin = rest.find_first_not_of(" \t,");
    if (begin == std::string_view::npos)
      break;
    size_t end = rest.find_first_of(" \t,", begin);
    if (end == std::string_view::npos) {
      names.emplace_back(rest.substr(begin));
      break;
    }
    names.emplace_back(rest.substr(begin, end - begin));
    rest = rest.substr(end);
  }
  return !names.empty();
}

/* True when the line contains actual code (anything that is not a comment or
 * blank). */
bool lineHasCode(std::string_view src) {
  size_t pos = 0;
  bool inString = false;
  while (pos < src.size()) {
    char c = src[pos];
    if (inString) {
      if (c == '"' || c == '\'') {
        inString = false;
      }
      pos++;
      continue;
    }
    if (c == '"' || c == '\'') {
      inString = true;
      pos++;
      continue;
    }
    if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/')
      return false;
    if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '*') {
      size_t end = src.find("*/", pos + 2);
      if (end == std::string_view::npos)
        return false;
      pos = end + 2;
      continue;
    }
    if (!std::isspace(static_cast<unsigned char>(c)))
      return true;
    pos++;
  }
  return false;
}

} // namespace

WarningSuppressions collectWarningSuppressions(std::string_view text) {
  WarningSuppressions out;

  size_t pos = 0;
  int line = 1;
  size_t lineStart = 0;
  bool seenCode = false;

  auto processLine = [&](size_t begin, size_t end) {
    std::string_view src = text.substr(begin, end - begin);
    bool hasCode = lineHasCode(src);
    if (hasCode)
      seenCode = true;

    size_t contentStart = src.find_first_not_of(" \t\r");
    if (contentStart == std::string_view::npos)
      return;
    if (src[contentStart] != '/' || contentStart + 1 >= src.size())
      return;

    std::string_view commentLine;
    if (src[contentStart + 1] == '/') {
      commentLine = src.substr(contentStart + 2);
    } else if (src[contentStart + 1] == '*') {
      size_t close = src.find("*/", contentStart + 2);
      commentLine = src.substr(contentStart + 2,
                               close == std::string_view::npos
                                   ? std::string_view::npos
                                   : close - contentStart - 2);
    } else {
      return;
    }
    if (commentLine.find('@') == std::string_view::npos)
      return;

    bool isFileScope = false;
    std::vector<std::string> names;
    if (!parseDirectiveLine(commentLine, isFileScope, names))
      return;

    if (isFileScope) {
      /* File-scope directives are only honored in the leading comment block,
       * before any code: the user places them at the very top of the file. */
      if (!seenCode) {
        for (const auto &n : names)
          out.fileKinds.insert(n);
      }
    } else {
      /* A line directive on its own line suppresses the line directly below
       * it; trailing the code it suppresses that same line. */
      out.lineKinds[hasCode ? line : line + 1].insert(names.begin(),
                                                      names.end());
    }
  };

  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    size_t end = (nl == std::string_view::npos) ? text.size() : nl;
    processLine(pos, end);
    if (nl == std::string_view::npos)
      break;
    pos = nl + 1;
    line++;
  }
  return out;
}

void mergeWarningSuppressions(WarningSuppressions &into,
                              const WarningSuppressions &from) {
  into.fileKinds.insert(from.fileKinds.begin(), from.fileKinds.end());
  for (const auto &[line, kinds] : from.lineKinds) {
    into.lineKinds[line].insert(kinds.begin(), kinds.end());
  }
}

} // namespace utopia
