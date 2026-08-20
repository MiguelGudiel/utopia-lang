#pragma once
#include "utopia/AST/ASTContext.hpp"
#include "utopia/Common/Diagnostics.hpp"
#include "utopia/Common/Types.hpp"
#include "utopia/Sema/SymbolTable.hpp"
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace utopia {

class SemaContext {
public:
  ASTContext &astCtx;
  SymbolTable symTable;
  std::string_view currentFile;
  const ModuleNode *currentModule = nullptr;
  std::unordered_map<std::string_view, const DeclNode *> templateRegistry;
  DiagnosticsEngine &diags;
  bool isAssignTarget = false;
  std::vector<std::string> namespaceStack;
  const FunctionDeclNode *currentFunction = nullptr;

  /* Nesting depth of Dart-style const contexts (const expressions and
   * initializers of const variables). While non-zero, constructor calls
   * that resolve to const constructors are evaluated as canonical const
   * objects (implicit const, Dart 2 rule). */
  int constContextDepth = 0;

  explicit SemaContext(ASTContext &ast, DiagnosticsEngine &de,
                       std::string_view path)
      : astCtx(ast), currentFile(path), diags(de) {
    currentFunctionReturn = astCtx.VoidTy;
  }

  void setCurrentFunction(const FunctionDeclNode *f) { currentFunction = f; }
  const FunctionDeclNode *getCurrentFunction() const { return currentFunction; }

  void pushNamespace(std::string_view ns) {
    namespaceStack.push_back(std::string(ns));
  }
  void popNamespace() { namespaceStack.pop_back(); }

  std::string getCurrentNamespace() const {
    std::string ns;
    for (size_t i = 0; i < namespaceStack.size(); ++i) {
      ns += namespaceStack[i];
      if (i < namespaceStack.size() - 1)
        ns += ".";
    }
    return ns;
  }

  void pushScope(ScopeKind kind = ScopeKind::Regular) {
    symTable.pushScope(kind);
  }
  void popScope() { symTable.popScope(); }

  /* Expected function signature stack, used to type lambdas from their
   * assignment/argument context (Dart-style inference). */
  std::vector<const Type *> expectedFunctionTypes;
  uint64_t lambdaCounter = 0;

  void pushExpectedFunctionType(const Type *t) {
    expectedFunctionTypes.push_back(t);
  }
  void popExpectedFunctionType() { expectedFunctionTypes.pop_back(); }
  const Type *getExpectedFunctionType() const {
    if (expectedFunctionTypes.empty())
      return nullptr;
    return expectedFunctionTypes.back();
  }

  /**
   * Compares two types structurally by string representation to safely handle
   * unresolved template parameters during the initial collection pass.
   */
  bool isSameType(const Type *a, const Type *b) const {
    if (a == b)
      return true;
    if (!a || !b)
      return false;
    return a->toString() == b->toString();
  }

  void addUsing(std::string_view ns) {
    if (!symTable.getScopes().empty()) {
      auto &usings = const_cast<Scope &>(symTable.getCurrentScope()).usings;
      std::string nsStr(ns);

      /* Prevent duplicate using directives within the same scope */
      bool exists = false;
      for (const auto &u : usings) {
        if (u == nsStr) {
          exists = true;
          break;
        }
      }

      if (!exists) {
        usings.push_back(nsStr);
      }
    }
  }

  size_t getUsingsCount() const {
    if (symTable.getScopes().empty())
      return 0;
    return symTable.getCurrentScope().usings.size();
  }

  /* Truncates the current scope's usings back to a snapshot taken with
   * getUsingsCount(), dropping directives added in between. */
  void resizeUsings(size_t count) {
    if (!symTable.getScopes().empty()) {
      const_cast<Scope &>(symTable.getCurrentScope()).usings.resize(count);
    }
  }

  void addDecl(std::string_view name, const DeclNode *decl) {
    if (decl->declFilePath.empty()) {
      const_cast<DeclNode *>(decl)->declFilePath = currentFile;
    }

    const auto &scopes = symTable.getScopes();
    if (scopes.empty())
      return;

    const auto &currentScope = scopes.back();
    if (auto it = currentScope.symbols.find(name);
        it != currentScope.symbols.end()) {
      for (const DeclNode *existing : it->second) {
        /* Constructors natively share the exact name of their parent
           record type. This permits Record declarations and Function
           declarations to coexist under the same identifier. */
        bool isRecordAndCtor = ((existing->kind == NodeKind::ClassDecl ||
                                 existing->kind == NodeKind::StructDecl ||
                                 existing->kind == NodeKind::UnionDecl ||
                                 existing->kind == NodeKind::AnnotationDecl) &&
                                decl->kind == NodeKind::FunctionDecl) ||
                               ((decl->kind == NodeKind::ClassDecl ||
                                 decl->kind == NodeKind::StructDecl ||
                                 decl->kind == NodeKind::UnionDecl ||
                                 decl->kind == NodeKind::AnnotationDecl) &&
                                existing->kind == NodeKind::FunctionDecl);

        if (decl->kind == NodeKind::FunctionDecl &&
            existing->kind == NodeKind::FunctionDecl) {
          auto *funcA = static_cast<const FunctionDeclNode *>(decl);
          auto *funcB = static_cast<const FunctionDeclNode *>(existing);
          bool sameSignature = true;

          if (funcA->params.size() != funcB->params.size()) {
            sameSignature = false;
          } else {
            for (size_t i = 0; i < funcA->params.size(); ++i) {
              if (!isSameType(funcA->params[i]->type, funcB->params[i]->type)) {
                sameSignature = false;
                break;
              }
            }
          }

          /* The const qualifier is part of the overload-resolution signature. */
          if (sameSignature && funcA->isMethod && funcB->isMethod) {
            if (funcA->isConst != funcB->isConst) {
              sameSignature = false;
            }
          }

          if (sameSignature) {
            reportError(decl->line, decl->column, decl->length,
                        "Redefinition of function '" + std::string(name) +
                            "' with the same signature.");
            return;
          }
        } else if (decl->kind == NodeKind::NamespaceDecl &&
                   existing->kind == NodeKind::NamespaceDecl) {
          /* Namespace reopening is allowed. */
          return;
        } else if (!isRecordAndCtor) {
          reportError(decl->line, decl->column, decl->length,
                      "Redefinition of '" + std::string(name) + "'.");
          return;
        }
      }
    }

    // Prevent shadowing variables from parent control-flow or
    // parameter scopes, explicitly allowing 'this' to be redefined across
    // boundaries
    if (scopes.size() > 1 && name != "this") {
      for (auto it = scopes.rbegin() + 1; it != scopes.rend(); ++it) {
        if (it->kind == ScopeKind::Regular) {
          break;
        }
        if (it->symbols.find(name) != it->symbols.end()) {
          reportError(decl->line, decl->column, decl->length,
                      "Redefinition of '" + std::string(name) +
                          "'. Shadows a parameter or control-flow variable.");
          return;
        }
      }
    }

    symTable.addSymbol(name, decl);
  }

  llvm::SmallVector<const DeclNode *, 2> lookup(std::string_view name) const {
    auto res = symTable.lookupExact(name, currentModule);
    if (!res.empty())
      return res;

    std::string ns = getCurrentNamespace();
    while (!ns.empty()) {
      res = symTable.lookupExact(ns + "." + std::string(name), currentModule);
      if (!res.empty())
        return res;

      size_t pos = ns.find_last_of('.');
      if (pos != std::string_view::npos) {
        ns = ns.substr(0, pos);
      } else {
        break;
      }
    }

    /* Search active using directives in reverse scope order to resolve
     * qualified namespace symbols. */
    for (auto it = symTable.getScopes().rbegin();
         it != symTable.getScopes().rend(); ++it) {
      for (const auto &u : it->usings) {
        std::string qualifiedName = u + "." + std::string(name);
        res = symTable.lookupExact(qualifiedName, currentModule);
        if (!res.empty())
          return res;
      }
    }
    return {};
  }

  SemaResult reportError(int line, int col, int len, const std::string &msg) {
    diags.report(
        {DiagLevel::Error, line, col, len, msg, std::string(currentFile)});

    /* Synchronize internal error state to guarantee pipeline termination
     * upon semantic validation failure */
    errors.push_back(ErrorInfo{line, col, len, msg});

    return std::unexpected(ErrorInfo{line, col, len, msg});
  }

  bool hasErrors() const { return !errors.empty(); }
  const std::vector<ErrorInfo> &getErrors() const { return errors; }

  void setFunctionReturnType(const Type *type) { currentFunctionReturn = type; }
  const Type *getFunctionReturnType() const { return currentFunctionReturn; }

  size_t getScopeDepth() const { return symTable.getDepth(); }

  void setCurrentFile(std::string_view path) { currentFile = path; }

  void setCurrentRecordContext(const RecordType *r) {
    currentRecordContext = r;
  }
  const RecordType *getCurrentRecordContext() const {
    return currentRecordContext;
  }

  void enterLoop() { loopDepth++; }
  void exitLoop() { loopDepth--; }
  bool isInLoop() const { return loopDepth > 0; }

  void enterSwitch() { switchDepth++; }
  void exitSwitch() { switchDepth--; }
  bool isInBreakable() const { return loopDepth > 0 || switchDepth > 0; }

  /* Virtual-method slot registry. Every polymorphic method in the program
   * is assigned a slot derived from its (sorted) name, shared by every
   * class hierarchy: dispatch through a base class or an interface pointer
   * loads the slot the interface itself was assigned, so all implementors
   * must agree on that slot. Name keys keep the assignment independent of
   * declaration order; inserting a name between existing ones re-ranks the
   * later names, which forces re-stamping every indexed method. */
  uint32_t assignVTableSlot(FunctionDeclNode *method) {
    std::string key(method->name);
    auto it = vtableSlots.find(key);
    if (it != vtableSlots.end()) {
      method->vtableIndex = it->second;
      if (std::find(vtableIndexedMethods.begin(), vtableIndexedMethods.end(),
                    method) == vtableIndexedMethods.end()) {
        vtableIndexedMethods.push_back(method);
      }
      return it->second;
    }

    auto pos = std::lower_bound(vtableSlotNames.begin(),
                                vtableSlotNames.end(), key);
    vtableSlotNames.insert(pos, key);
    for (size_t i = 0; i < vtableSlotNames.size(); i++) {
      vtableSlots[vtableSlotNames[i]] = static_cast<uint32_t>(i);
    }
    for (auto *indexed : vtableIndexedMethods) {
      indexed->vtableIndex = vtableSlots[std::string(indexed->name)];
    }
    method->vtableIndex = vtableSlots[key];
    vtableIndexedMethods.push_back(method);
    return method->vtableIndex;
  }

private:
  std::vector<ErrorInfo> errors;
  const Type *currentFunctionReturn;
  const RecordType *currentRecordContext = nullptr;
  int loopDepth = 0;
  int switchDepth = 0;
  std::unordered_map<std::string, uint32_t> vtableSlots;
  std::vector<std::string> vtableSlotNames;
  std::vector<FunctionDeclNode *> vtableIndexedMethods;
};

class ScopeGuard {
  SemaContext &ctx;

public:
  explicit ScopeGuard(SemaContext &c, ScopeKind kind = ScopeKind::Regular)
      : ctx(c) {
    ctx.pushScope(kind);
  }
  ~ScopeGuard() { ctx.popScope(); }
};

class LoopGuard {
  SemaContext &ctx;

public:
  explicit LoopGuard(SemaContext &c) : ctx(c) { ctx.enterLoop(); }
  ~LoopGuard() { ctx.exitLoop(); }
};

class SwitchGuard {
  SemaContext &ctx;

public:
  explicit SwitchGuard(SemaContext &c) : ctx(c) { ctx.enterSwitch(); }
  ~SwitchGuard() { ctx.exitSwitch(); }
};

} // namespace utopia