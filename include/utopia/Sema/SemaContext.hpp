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

  explicit SemaContext(ASTContext &ast, DiagnosticsEngine &de,
                       std::string_view path)
      : astCtx(ast), currentFile(path), diags(de) {
    currentFunctionReturn = astCtx.VoidTy;
  }

  void pushScope() { symTable.pushScope(); }
  void popScope() { symTable.popScope(); }

  void addDecl(std::string_view name, const DeclNode *decl) {
    if (decl->declFilePath.empty()) {
      const_cast<DeclNode *>(decl)->declFilePath = currentFile;
    }
    symTable.addSymbol(name, decl);
  }

  llvm::SmallVector<const DeclNode *, 2> lookup(std::string_view name) const {
    return symTable.lookup(name);
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

private:
  std::vector<ErrorInfo> errors;
  const Type *currentFunctionReturn;
  const RecordType *currentRecordContext = nullptr;
};

class ScopeGuard {
  SemaContext &ctx;

public:
  explicit ScopeGuard(SemaContext &c) : ctx(c) { ctx.pushScope(); }
  ~ScopeGuard() { ctx.popScope(); }
};

} // namespace utopia