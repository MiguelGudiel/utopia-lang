#pragma once
#include "utopia/AST/AST.hpp"
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace utopia {

struct SymbolInfo {
  llvm::Value *value = nullptr;
  bool isDirectAddress = true;
};

/* Static mapping of a destroyer pending broadcast */
struct CleanupInfo {
  llvm::Value *instancePtr = nullptr;
  const FunctionDeclNode *destructor = nullptr;
};

struct LifetimeInfo {
  llvm::AllocaInst *allocaInst = nullptr;
  uint64_t size = 0;
};

/* Extended lexical scope with RAII support */
struct CGLocalScope {
  std::unordered_map<std::string, SymbolInfo> symbols;
  std::vector<CleanupInfo> cleanups;
  std::vector<LifetimeInfo> lifetimes;
};

class CodeGenContext {
public:
  CodeGenContext() { pushScope(); }

  void pushScope() { scopes.emplace_back(); }
  void popScope() { scopes.pop_back(); }

  void bind(std::string_view name, llvm::Value *val, bool isDirect = true) {
    if (!scopes.empty()) {
      scopes.back().symbols[std::string(name)] = {val, isDirect};
    }
  }

  /* Registers a dynamic object for later destruction upon exiting the block */
  void addCleanup(llvm::Value *ptr, const FunctionDeclNode *dtor) {
    if (!scopes.empty()) {
      scopes.back().cleanups.push_back({ptr, dtor});
    }
  }

  /* Registers stack memory spans to enforce precise intrinsic lifetime
   * boundaries */
  void addLifetime(llvm::AllocaInst *allocaInst, uint64_t size) {
    if (!scopes.empty()) {
      scopes.back().lifetimes.push_back({allocaInst, size});
    }
  }

  const std::vector<CGLocalScope> &getAllScopes() const { return scopes; }
  const CGLocalScope &getCurrentScope() const { return scopes.back(); }

  llvm::Value *lookup(std::string_view name) {
    return lookupDetailed(name).value;
  }

  SymbolInfo lookupDetailed(std::string_view name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      if (auto found = it->symbols.find(std::string(name));
          found != it->symbols.end()) {
        return found->second;
      }
    }
    return {};
  }

private:
  std::vector<CGLocalScope> scopes;
};

class CGScopeGuard {
  CodeGenContext &ctx;

public:
  explicit CGScopeGuard(CodeGenContext &context) : ctx(context) {
    ctx.pushScope();
  }
  ~CGScopeGuard() { ctx.popScope(); }
  CGScopeGuard(const CGScopeGuard &) = delete;
  CGScopeGuard &operator=(const CGScopeGuard &) = delete;
};

} // namespace utopia