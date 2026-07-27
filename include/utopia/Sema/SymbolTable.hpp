#pragma once
#include "utopia/AST/AST.hpp"
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <string_view>
#include <vector>

namespace utopia {

/*
 * Represents a single lexical scope. 
 * Utilizes SmallVector to minimize heap allocations for typical symbol lookups 
 * (usually 1 declaration per name, scaling up for function overloads).
 */
class Scope {
public:
  llvm::StringMap<llvm::SmallVector<const DeclNode *, 2>> symbols;
};

class SymbolTable {
public:
  SymbolTable() { pushScope(); }
  ~SymbolTable() { popScope(); }

  void pushScope() { scopes.push_back(Scope()); }
  void popScope() { scopes.pop_back(); }

  void addSymbol(std::string_view name, const DeclNode *decl) {
    scopes.back().symbols[name].push_back(decl);
  }

  /* Performs a top-down search across scopes. Hides outer scope symbols on collision. */
  llvm::SmallVector<const DeclNode *, 2> lookup(std::string_view name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto found = it->symbols.find(name);
      if (found != it->symbols.end()) {
        return found->second;
      }
    }
    return {};
  }

  size_t getDepth() const { return scopes.size(); }

private:
  std::vector<Scope> scopes;
};

} // namespace utopia