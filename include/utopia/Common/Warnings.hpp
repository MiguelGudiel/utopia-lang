#pragma once
/* Machine-identifiable warning kinds for the Utopia compiler. Every warning
 * the compiler can emit belongs to a kind, so users can:
 *   - disable a whole family from build.yaml / build.utp,
 *   - suppress a single occurrence with an '// @ignore-warning <kind>'
 *     comment on the line above,
 *   - suppress a kind for an entire file with '// @ignore-warnings <kind>'
 *     placed at the top of the file,
 * and editors can attach code actions (quick fixes) to the reported
 * diagnostics. */

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace utopia {

enum class WarningKind : uint8_t {
  None = 0,
  UnusedImport,
  UnusedFunction,
  UnusedMethod,
  UnusedVariable,
  UnusedField,
  UnusedParameter,
  UnusedUsing,
  UnusedType,
  NodiscardIgnored,
  Deprecated,
  ImplicitCast,
  UninitializedVariable,
};

/* Stable identifier of a warning kind (e.g. "unused_import"). Empty for
 * WarningKind::None. */
const char *warningName(WarningKind kind);

/* Short human description used in hover/diagnostic tooling. */
const char *warningDescription(WarningKind kind);

/* Whether editors can offer an automatic fix for this warning. */
bool warningHasFix(WarningKind kind);

/* Parses a warning name back into its kind; None when unknown. */
WarningKind warningFromName(std::string_view name);

/* Every warning kind except None, in a stable order. */
const std::vector<WarningKind> &allWarningKinds();

/* Per-project configuration (build.yaml 'build.warnings', build.utp) */

/* Enabled/disabled state for every warning kind. All warnings are enabled by
 * default; callers disable individual kinds from the project manifest. */
class WarningConfig {
public:
  bool enabled(WarningKind kind) const {
    if (kind == WarningKind::None)
      return true;
    auto it = disabled.find(kind);
    if (it == disabled.end())
      return true;
    return it->second;
  }

  /* Disables a warning kind; unknown names are ignored. Returns whether the
   * name was a known warning. */
  bool disable(std::string_view name) {
    WarningKind kind = warningFromName(name);
    if (kind == WarningKind::None)
      return false;
    disabled[kind] = false;
    return true;
  }

  void disable(WarningKind kind) { disabled[kind] = false; }

private:
  std::unordered_map<WarningKind, bool> disabled;
};

/* In-source suppression directives */

/* '// @ignore-warning unused_import' directly above a line suppresses that
 * warning kind on that line; '// @ignore-warnings a, b' (plural, placed at
 * the top of the file) suppresses the listed kinds for the whole file. */
struct WarningSuppressions {
  std::unordered_set<std::string> fileKinds;
  /* 1-based line -> set of suppressed kind names on that line. */
  std::unordered_map<int, std::unordered_set<std::string>> lineKinds;

  bool suppresses(WarningKind kind, int line) const {
    if (kind == WarningKind::None)
      return false;
    const char *name = warningName(kind);
    if (fileKinds.find(name) != fileKinds.end())
      return true;
    auto it = lineKinds.find(line);
    return it != lineKinds.end() &&
           it->second.find(name) != it->second.end();
  }
};

/* Scans source text for '// @ignore-warning' / '// @ignore-warnings'
 * directives. Line directives apply to the line directly below the comment;
 * plural directives apply to the whole file. */
WarningSuppressions collectWarningSuppressions(std::string_view text);

/* Merge for multi-line (block) comments found by the scanner. */
void mergeWarningSuppressions(WarningSuppressions &into,
                              const WarningSuppressions &from);

} // namespace utopia
