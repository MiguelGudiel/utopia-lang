#pragma once
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/CodeGen/BackendContext.hpp"
#include "utopia/CodeGen/CodeGenContext.hpp"
#include "utopia/CodeGen/DebugInfoEmitter.hpp"
#include "utopia/CodeGen/TBAAManager.hpp"
#include "utopia/Common/Diagnostics.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace utopia {
class Intrinsic;

class CodeGen : public ASTVisitor<CodeGen, llvm::Value *> {
  friend class Intrinsic;
  friend class DebugInfoEmitter;
  friend class TBAAManager;

public:
  CodeGen(BackendContext &bCtx, llvm::Module &llvmMod, DiagnosticsEngine &diags,
          bool emitDebugInfo, std::string filePath);

  llvm::Value *dispatch(const ASTNode *node);

  llvm::Value *visit(const NumberNode *node);
  llvm::Value *visit(const BoolNode *node);
  llvm::Value *visit(const CharNode *node);
  llvm::Value *visit(const RuneNode *node);
  llvm::Value *visit(const StringNode *node);
  llvm::Value *visit(const VariableNode *node);
  llvm::Value *visit(const UnaryOpNode *node);
  llvm::Value *visit(const BinaryOpNode *node);
  llvm::Value *visit(const TernaryOpNode *node);
  llvm::Value *visit(const VarDeclNode *node);
  llvm::Value *visit(const AssignNode *node);
  llvm::Value *visit(const BlockNode *node);
  llvm::Value *visit(const FunctionDeclNode *node);
  llvm::Value *visit(const FunctionCallNode *node);
  llvm::Value *visit(const IfNode *node);
  llvm::Value *visit(const ForNode *node);
  llvm::Value *visit(const WhileNode *node);
  llvm::Value *visit(const SwitchNode *node);
  llvm::Value *visit(const CaseNode *node) { return nullptr; }
  llvm::Value *visit(const BreakNode *node);
  llvm::Value *visit(const ContinueNode *node);
  llvm::Value *visit(const ReturnNode *node);
  llvm::Value *visit(const CastNode *node);
  llvm::Value *visit(const ParamDeclNode *node);
  llvm::Value *visit(const ModuleNode *node);
  llvm::Value *visit(const MemberAccessNode *node);
  llvm::Value *visit(const UnionDeclNode *node);
  llvm::Value *visit(const StructDeclNode *node);
  llvm::Value *visit(const ClassDeclNode *node);
  llvm::Value *visit(const AnnotationDeclNode *node);
  llvm::Value *visit(const TypedefDeclNode *node);
  llvm::Value *visit(const AnnotationNode *node);
  llvm::Value *visit(const ArraySubscriptNode *node);
  llvm::Value *visit(const NewExprNode *node);
  llvm::Value *visit(const DeleteExprNode *node);
  llvm::Value *visit(const TypeLiteralNode *node);
  llvm::Value *visit(const ArrayLiteralNode *node);
  llvm::Value *visit(const NullNode *node);
  llvm::Value *visit(const EnumDeclNode *node);
  llvm::Value *visit(const EnumMemberNode *node);
  llvm::Value *visit(const ImplicitCastNode *node);

  llvm::Value *createImplicitCast(llvm::Value *src, llvm::Type *destTy);
  void emitLoopCleanups(size_t targetDepth);

private:
  BackendContext &backend;
  llvm::LLVMContext &ctx;
  llvm::Module &mod;
  llvm::IRBuilder<> builder;
  CodeGenContext cgCtx;
  DiagnosticsEngine &diags;
  const FunctionDeclNode *currentFunc = nullptr;
  llvm::AllocaInst *lastTemporaryAlloca = nullptr;
  std::string currentFilePath;

  DebugInfoEmitter diEmitter;
  TBAAManager tbaaManager;

  void emitLocation(const ASTNode *node);

  llvm::Type *getLLVMType(const Type *type);
  llvm::Value *getLValue(const ExprNode *node);
  llvm::Constant *evaluateAsConstant(const ExprNode *node);
  llvm::Function *getOrCreateFunction(const FunctionDeclNode *node);

  llvm::LoadInst *createTBAALoad(llvm::Type *llTy, llvm::Value *ptr,
                                 const Type *utopiaTy,
                                 const llvm::Twine &name = "");
  llvm::LoadInst *createTBAALoad(llvm::Type *llTy, llvm::Value *ptr,
                                 llvm::MDNode *tbaaTag,
                                 const llvm::Twine &name = "");

  llvm::StoreInst *createTBAAStore(llvm::Value *val, llvm::Value *ptr,
                                   const Type *utopiaTy);
  llvm::StoreInst *createTBAAStore(llvm::Value *val, llvm::Value *ptr,
                                   llvm::MDNode *tbaaTag);

  llvm::Constant *createTypeReflectionConstant(const Type *t,
                                               llvm::StructType *structTy);

  /* Lifetime Intrinsic Emission */
  void emitLifetimeStart(llvm::AllocaInst *allocaInst, uint64_t size);
  void emitLifetimeEnd(llvm::AllocaInst *allocaInst, uint64_t size);

  void emitConstructorCall(const FunctionCallNode *node,
                           llvm::Value *targetAddr);
  void emitArrayLiteralInit(llvm::Value *targetAddr, const Type *targetType,
                            const ExprNode *initExpr);
  llvm::AllocaInst *createEntryBlockAlloca(llvm::Type *type,
                                           const std::string &varName);
  void emitDefaultInitialization(llvm::Value *ptr, const Type *type);
  void emitCleanupCall(llvm::Value *ptr, const FunctionDeclNode *dtor);
  void emitScopeCleanups();
};

} // namespace utopia