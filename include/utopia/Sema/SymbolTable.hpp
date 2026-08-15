#pragma once
#include "utopia/AST/AST.hpp"
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <string_view>
#include <vector>

namespace utopia {

enum class ScopeKind { Regular, FunctionParams, ControlFlowInit };

/*
 * Represents a single lexical scope.
 * Utilizes SmallVector to minimize heap allocations for typical symbol lookups
 * (usually 1 declaration per name, scaling up for function overloads).
 */
class Scope {
public:
  llvm::StringMap<llvm::SmallVector<const DeclNode *, 2>> symbols;
  std::vector<std::string> usings;
  ScopeKind kind = ScopeKind::Regular;
};

class SymbolTable {
public:
  SymbolTable() { pushScope(ScopeKind::Regular); }
  ~SymbolTable() { popScope(); }

  void pushScope(ScopeKind kind = ScopeKind::Regular) {
    Scope s;
    s.kind = kind;
    scopes.push_back(std::move(s));
  }

  void popScope() { scopes.pop_back(); }

  void addSymbol(std::string_view name, const DeclNode *decl) {
    scopes.back().symbols[name].push_back(decl);
  }

  /*
   * Registers a symbol in the module (global) scope regardless of the current
   * lexical scope. Used for template instantiations whose mangled names must
   * remain resolvable from any function without polluting local scopes.
   */
  void addGlobalSymbol(std::string_view name, const DeclNode *decl) {
    if (scopes.empty())
      return;
    scopes.front().symbols[name].push_back(decl);
  }

  /**
   * Retrieves the current lexical scope to perform localized redefinition
   * checks.
   */
  const Scope &getCurrentScope() const { return scopes.back(); }
  const std::vector<Scope> &getScopes() const { return scopes; }

  /* Performs a top-down search across scopes. Hides outer scope symbols on
   * collision. */
  llvm::SmallVector<const DeclNode *, 2>
  lookupExact(std::string_view name,
              const ModuleNode *currentModule = nullptr) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto found = it->symbols.find(name);
      if (found != it->symbols.end()) {
        llvm::SmallVector<const DeclNode *, 2> results;
        for (const auto *decl : found->second) {
          if (!currentModule || currentModule->canSee(decl->declFilePath)) {
            results.push_back(decl);
          }
        }
        if (!results.empty())
          return results;
      }
    }
    return {};
  }

  size_t getDepth() const { return scopes.size(); }

private:
  std::vector<Scope> scopes;
};

} // namespace utopia