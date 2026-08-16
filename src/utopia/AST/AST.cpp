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
  std::unordered_set<const ModuleNode *> visited;
  return canSeeHelper(targetFilePath, visited);
}

bool ModuleNode::canSeeHelper(
    std::string_view targetFilePath,
    std::unordered_set<const ModuleNode *> &visited) const {
  if (visited.contains(this))
    return false;
  visited.insert(this);

  if (this->filePath == targetFilePath)
    return true;

  /* Traverse the full transitive import graph so symbols and namespaces
   * declared in transitively imported modules are visible downstream
   * (imports behave transitively, like most mainstream languages). */
  for (const auto *imp : importedModules) {
    if (imp->canSeeHelper(targetFilePath, visited))
      return true;
  }

  /* Re-exported modules remain reachable without a direct import. */
  for (const auto *exp : exportedModules) {
    if (exp->canSeeHelper(targetFilePath, visited))
      return true;
  }

  return false;
}
} // namespace utopia