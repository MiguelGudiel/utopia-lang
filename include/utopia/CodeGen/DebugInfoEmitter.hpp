#pragma once
#include "utopia/AST/AST.hpp"
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <unordered_map>
#include <vector>

namespace utopia {
class CodeGen;

/*
 * Manages the generation of LLVM debug information metadata.
 */
class DebugInfoEmitter {
public:
  DebugInfoEmitter(llvm::Module &mod, bool emitDebugInfo);

  bool isEnabled() const { return emitDebugInfo; }

  void initializeModule(const ModuleNode *node);
  void finalize();

  void emitLocation(llvm::IRBuilder<> &builder, const ASTNode *node);
  llvm::DIType *getDIType(CodeGen &cg, const Type *type);

  void pushLexicalBlock(const ASTNode *node, llvm::LLVMContext &ctx);
  void popLexicalBlock();

  void emitFunctionStart(CodeGen &cg, llvm::Function *func,
                         const FunctionDeclNode *node);
  void emitFunctionEnd();

  void emitLocalVariable(CodeGen &cg, llvm::IRBuilder<> &builder,
                         llvm::AllocaInst *alloca, const VarDeclNode *node);
  void emitParameterVariable(CodeGen &cg, llvm::IRBuilder<> &builder,
                             llvm::AllocaInst *alloca,
                             const ParamDeclNode *node, unsigned argNo);
  void emitGlobalVariable(CodeGen &cg, llvm::GlobalVariable *gvar,
                          const VarDeclNode *node, const std::string &bindName);

  llvm::DIBuilder *getBuilder() const { return dBuilder.get(); }
  llvm::DIFile *getFile() const { return diFile; }
  llvm::DIScope *getCurrentScope() const {
    return lexicalBlocks.empty() ? diCU : lexicalBlocks.back();
  }

private:
  bool emitDebugInfo;
  llvm::Module &mod;
  std::unique_ptr<llvm::DIBuilder> dBuilder;
  llvm::DICompileUnit *diCU = nullptr;
  llvm::DIFile *diFile = nullptr;
  std::vector<llvm::DIScope *> lexicalBlocks;
  std::unordered_map<const Type *, llvm::DIType *> debugTypes;
};

} // namespace utopia