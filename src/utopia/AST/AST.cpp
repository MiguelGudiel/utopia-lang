#include "utopia/AST/AST.hpp"

namespace utopia {
bool ModuleNode::exports(
    std::string_view targetFilePath,
    std::unordered_set<const ModuleNode *> &visited) const {
  if (visited.contains(this))
    return false;
  visited.insert(this);

  for (const auto *exp : exportedModules) {
    if (exp->filePath == targetFilePath)
      return true;
    if (exp->exports(targetFilePath, visited))
      return true;
  }
  return false;
}

bool ModuleNode::canSee(std::string_view targetFilePath) const {
  if (this->filePath == targetFilePath)
    return true;

  std::unordered_set<const ModuleNode *> visited;
  visited.insert(this);

  /* Only traverse explicitly imported modules for local scope visibility.
   * Exported modules are isolated from local access unless explicitly imported,
   * matching strict module encapsulation semantics. */
  for (const auto *imp : importedModules) {
    if (imp->filePath == targetFilePath)
      return true;
    /* Allow transitive visibility ONLY through the dependency's explicit
     * exports */
    if (imp->exports(targetFilePath, visited))
      return true;
  }

  return false;
}
} // namespace utopia