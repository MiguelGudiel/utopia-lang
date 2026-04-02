#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/AST/ASTVisitor.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>
#include <map>
#include <memory>
#include <utopia/Common/Types.hpp>
#include <vector>

namespace utopia {

class CodeGen : public ASTVisitor {
public:
  CodeGen();
  void generate(ProgramNode *program);
  void optimize(int level);
  void saveToFile(const std::string &filename);
  void emitObjectFile(const std::string &filename);

private:
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
  std::unique_ptr<llvm::IRBuilder<>> builder;
  std::unique_ptr<llvm::TargetMachine> targetMachine;

  // LIFO Stack for Variable Scoping
  std::vector<std::map<std::string, llvm::AllocaInst *>> valueScopes;
  std::vector<std::map<std::string, TypeInfo>> typeScopes;
  std::map<std::string, llvm::Value *> stringPool;

  // LIFO stacks for loop jumping
  std::vector<llvm::BasicBlock *> breakTargets;
  std::vector<llvm::BasicBlock *> continueTargets;

  std::map<std::string, TypeInfo> functionTypes;

  std::map<std::string, StructDeclNode *> structASTs;
  std::map<std::string, llvm::StructType *> structTypes;
  std::map<std::string, std::map<std::string, int>> structMemberIndices;
  std::map<std::string, std::map<std::string, TypeInfo>> structMemberTypes;

  llvm::Value *currentVal = nullptr;
  TypeInfo currentType;
  TypeInfo currentReturnType;
  llvm::Value *currentLValue = nullptr;
  bool isLValueContext = false;

  void enterScope();
  void exitScope();
  llvm::AllocaInst *lookupValue(const std::string &name);
  TypeInfo lookupType(const std::string &name);

  TypeInfo parseTypeString(const std::string &typeName) const;
  llvm::Type *getLLVMType(const std::string &typeName);
  llvm::Type *getLLVMType(const TypeInfo &type);

  llvm::Value *castValue(llvm::Value *value, const TypeInfo &from,
                         const TypeInfo &to);
  llvm::Value *getOrCreateString(const std::string &str);

  llvm::FunctionCallee getMallocPrototype();
  llvm::FunctionCallee getFreePrototype();
  llvm::FunctionCallee getPrintfPrototype();

  void visit(ThisNode *node) override;
  void visit(StructDeclNode *node) override;
  void visit(MemberAccessNode *node) override;
  void visit(BlockNode *node) override;
  void visit(NullLiteralNode *node) override;
  void visit(IfNode *node) override;
  void visit(WhileNode *node) override;
  void visit(ForNode *node) override;
  void visit(BreakNode *node) override;
  void visit(ContinueNode *node) override;
  void visit(NullAssertNode *node) override;
  void visit(NumberNode *node) override;
  void visit(FloatNode *node) override;
  void visit(BoolNode *node) override;
  void visit(StringNode *node) override;
  void visit(UnaryMinusNode *node) override;
  void visit(VariableNode *node) override;
  void visit(AddressOfNode *node) override;
  void visit(DerefNode *node) override;
  void visit(NewNode *node) override;
  void visit(DeleteNode *node) override;
  void visit(BinaryOpNode *node) override;
  void visit(CallNode *node) override;
  void visit(AssignNode *node) override;
  void visit(VarDeclNode *node) override;
  void visit(ReturnNode *node) override;
  void visit(FunctionNode *node) override;
  void visit(ProgramNode *node) override;
};

} // namespace utopia