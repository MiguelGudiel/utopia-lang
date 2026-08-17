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

/* Pending destructor invocation for an object instance. */
struct CleanupInfo {
  llvm::Value *instancePtr = nullptr;
  const FunctionDeclNode *destructor = nullptr;
  const Type *type = nullptr;
  /* Optional i1 flag that must be true for the cleanup to run: used for
   * temporaries created conditionally (e.g. ternary branches), where the
   * object may never have been constructed. */
  llvm::Value *guard = nullptr;
};

struct LifetimeInfo {
  llvm::AllocaInst *allocaInst = nullptr;
  uint64_t size = 0;
};

struct CGLocalScope {
  std::unordered_map<std::string, SymbolInfo> symbols;
  std::vector<CleanupInfo> cleanups;
  std::vector<LifetimeInfo> lifetimes;
};

struct LoopInfo {
  llvm::BasicBlock *continueBlock;
  llvm::BasicBlock *breakBlock;
  size_t scopeDepth;
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
  void addCleanup(llvm::Value *ptr, const FunctionDeclNode *dtor,
                  const Type *type = nullptr, llvm::Value *guard = nullptr) {
    if (!scopes.empty()) {
      scopes.back().cleanups.push_back({ptr, dtor, type, guard});
    }
  }

  size_t getCleanupCount() const {
    return scopes.empty() ? 0 : scopes.back().cleanups.size();
  }

  void popCleanup() {
    if (!scopes.empty() && !scopes.back().cleanups.empty()) {
      scopes.back().cleanups.pop_back();
    }
  }

  /* Removes a registered cleanup (used for Return Value Optimization to
   * transfer ownership) */
  void removeCleanup(llvm::Value *ptr) {
    for (auto &scope : scopes) {
      for (auto it = scope.cleanups.begin(); it != scope.cleanups.end(); ++it) {
        if (it->instancePtr == ptr) {
          scope.cleanups.erase(it);
          return;
        }
      }
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

  void pushLoop(llvm::BasicBlock *cont, llvm::BasicBlock *brk) {
    loops.push_back({cont, brk, scopes.size()});
  }

  void popLoop() { loops.pop_back(); }
  const LoopInfo &getCurrentLoop() const { return loops.back(); }

  /* Fetches the closest valid loop block explicitly ignoring switch jumps */
  const LoopInfo &getCurrentLoopForContinue() const {
    for (auto it = loops.rbegin(); it != loops.rend(); ++it) {
      if (it->continueBlock != nullptr) {
        return *it;
      }
    }
    return loops.front();
  }

private:
  std::vector<CGLocalScope> scopes;
  std::vector<LoopInfo> loops;
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