#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/AST/ASTVisitor.hpp"
#include <llvm/IR/DIBuilder.h>
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
  CodeGen(const std::string &sourceFile, bool isDebug);
  void generate(ProgramNode *program);
  void generate(ModuleNode *module, const std::string &outputObjPath,
                const std::vector<ModuleNode *> &allModules);
  void registerModules(const std::vector<ModuleNode *> &allModules);
  void optimize(int level);
  void saveToFile(const std::string &filename);
  void emitObjectFile(const std::string &filename);
  void emitAssemblyFile(const std::string &filename);

private:
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
  std::unique_ptr<llvm::IRBuilder<>> builder;
  std::unique_ptr<llvm::TargetMachine> targetMachine;

  // LIFO Stack for Variable Scoping
  std::vector<std::vector<std::pair<std::string, llvm::Value *>>> valueScopes;
  std::vector<std::map<std::string, TypeInfo>> typeScopes;
  std::map<std::string, llvm::Value *> stringPool;
  std::map<std::string, TypeInfo> globalVarTypes;

  // LIFO stacks for loop jumping
  std::vector<llvm::BasicBlock *> breakTargets;
  std::vector<llvm::BasicBlock *> continueTargets;
  std::vector<size_t> loopScopeDepths;

  std::map<std::string, llvm::GlobalVariable *> vtables;
  std::map<std::string, llvm::StructType *> vtableTypes;
  std::map<std::string, std::map<std::string, int>> vtableLayout;
  std::map<std::string, std::vector<std::string>> vtableMethods;

  std::map<std::string, StructDeclNode *> structASTs;
  std::map<std::string, llvm::StructType *> structTypes;
  std::map<std::string, std::map<std::string, int>> structMemberIndices;
  std::map<std::string, std::map<std::string, TypeInfo>> structMemberTypes;

  std::map<std::string, TypeInfo> functionTypes;
  std::map<std::string, std::vector<TypeInfo>> functionParamTypes;

  llvm::Value *currentVal = nullptr;
  TypeInfo currentType;
  std::string currentClass;
  TypeInfo currentReturnType;
  llvm::Value *currentLValue = nullptr;
  bool isLValueContext = false;

  llvm::Value *rvoTarget = nullptr;

  bool isDebug;
  std::unique_ptr<llvm::DIBuilder> dbgBuilder;
  llvm::DICompileUnit *dbgCU = nullptr;
  llvm::DIFile *dbgFile = nullptr;
  std::vector<llvm::DIScope *> dbgScopes;
  std::map<std::string, llvm::DIType *> debugTypes;

  void enterScope();
  void exitScope();
  llvm::Value *lookupValue(const std::string &name);
  TypeInfo lookupType(const std::string &name);

  TypeInfo parseTypeString(const std::string &typeName) const;
  llvm::Type *getLLVMType(const std::string &typeName);
  llvm::Type *getLLVMType(const TypeInfo &type);

  llvm::Value *castValue(llvm::Value *value, const TypeInfo &from,
                         const TypeInfo &to);
  void emitCopyOrStore(llvm::Value *destAddr, llvm::Value *srcVal,
                       const TypeInfo &targetType, const TypeInfo &srcType);
  void emitLifecycleLoop(llvm::Value *basePtr, llvm::Value *size,
                         const std::string &typeName, bool isDestructor);
  void emitScopeCleanup(size_t targetDepth);
  llvm::Value *getOrCreateString(const std::string &str);

  llvm::FunctionCallee getMallocPrototype();
  llvm::FunctionCallee getFreePrototype();
  llvm::FunctionCallee getPrintfPrototype();

  void emitLocation(ASTNode *node);
  llvm::DIType *getDebugType(const TypeInfo &type);

  void visit(ThisNode *node) override;
  void visit(SuperNode *node) override;
  void visit(StructDeclNode *node) override;
  void visit(ExtensionNode *node) override;
  void visit(MemberAccessNode *node) override;
  void visit(BlockNode *node) override;
  void visit(NullLiteralNode *node) override;
  void visit(IfNode *node) override;
  void visit(WhileNode *node) override;
  void visit(ForNode *node) override;
  void visit(BreakNode *node) override;
  void visit(ContinueNode *node) override;
  void visit(NullAssertNode *node) override;
  void visit(LogicalNotNode *node) override;
  void visit(NumberNode *node) override;
  void visit(FloatNode *node) override;
  void visit(BoolNode *node) override;
  void visit(StringNode *node) override;
  void visit(UnaryMinusNode *node) override;
  void visit(SubscriptNode *node) override;
  void visit(VariableNode *node) override;
  void visit(AddressOfNode *node) override;
  void visit(DerefNode *node) override;
  void visit(NewNode *node) override;
  void visit(DeleteNode *node) override;
  void visit(MoveNode *node) override;
  void visit(BinaryOpNode *node) override;
  void visit(CallNode *node) override;
  void visit(AssignNode *node) override;
  void visit(VarDeclNode *node) override;
  void visit(ReturnNode *node) override;
  void visit(FunctionNode *node) override;
  void visit(ProgramNode *node) override;
  void visit(ModuleNode *node) override;
};

} // namespace utopia