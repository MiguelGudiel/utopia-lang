#pragma once
#include "utopia/AST/ASTContext.hpp"
#include "utopia/Common/Diagnostics.hpp"
#include "utopia/Common/Types.hpp"
#include "utopia/Sema/SymbolTable.hpp"
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

  explicit SemaContext(ASTContext &ast, DiagnosticsEngine &de,
                       std::string_view path)
      : astCtx(ast), currentFile(path), diags(de) {
    currentFunctionReturn = astCtx.VoidTy;
  }

  void pushScope(ScopeKind kind = ScopeKind::Regular) {
    symTable.pushScope(kind);
  }
  void popScope() { symTable.popScope(); }

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

          /* Method const qualifier evaluates into the overload resolution
           * signature */
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
    return symTable.lookup(name, currentModule);
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

private:
  std::vector<ErrorInfo> errors;
  const Type *currentFunctionReturn;
  const RecordType *currentRecordContext = nullptr;
  int loopDepth = 0;
  int switchDepth = 0;
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