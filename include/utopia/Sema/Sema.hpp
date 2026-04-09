#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Common/Types.hpp"
#include <map>
#include <string>
#include <vector>

namespace utopia {

std::string typeToString(const TypeInfo &t);

class Sema : public ASTVisitor {
public:
  bool analyze(ProgramNode *program);
  const std::vector<ErrorInfo> &getErrors() const { return errors; }
  bool analyzeModules(const std::vector<ModuleNode *> &modules);

private:
  struct Symbol {
    TypeInfo type;
    bool isConst;
  };

  struct StructDef {
    bool isClass;
    bool hasVTable = false;
    std::string baseClass;

    struct Field {
      TypeInfo type;
      AccessModifier mod;
      int index;
      bool isStatic;
    };
    std::map<std::string, Field> fields;

    // Map method name to its index in the VTable
    std::map<std::string, int> vtableLayout;
    std::vector<std::string> vtableMethods; // Exact order of the table
    std::vector<int> constructorArities;
  };

  struct OverloadCandidate {
    std::string mangledName;
    std::vector<TypeInfo> paramTypes;
    TypeInfo returnType;
  };
  std::map<std::string, std::vector<OverloadCandidate>> overloadTable;

  std::map<std::string, StructDeclNode *> structASTs;
  std::map<std::string, StructDef> customStructs;
  std::map<std::string, std::string> copyConstructors;

  std::vector<std::map<std::string, Symbol>> scopeStack;
  std::vector<ErrorInfo> errors;

  std::map<std::string, TypeInfo> functionTypes;
  TypeInfo currentExprType;
  TypeInfo currentReturnType;
  std::string currentClass;

  std::map<std::string, Symbol> globalSymbols;
  std::string currentModuleFile;
  std::map<std::string, std::string> classModuleMap;

  int loopDepth = 0;
  bool inStaticMethod = false;
  bool isProcessingExtension = false;

  void enterScope();
  void exitScope();
  Symbol *lookup(const std::string &name);

  void reportError(ASTNode *node, const std::string &message);
  TypeInfo parseType(const std::string &typeName, ASTNode *node = nullptr);
  bool checkAssignment(const TypeInfo &target, const TypeInfo &source,
                       ASTNode *node);

  void registerOverload(const std::string &baseName,
                        const std::string &mangledName,
                        const std::vector<TypeInfo> &params,
                        const TypeInfo &ret);
  int getConversionCost(const TypeInfo &target, const TypeInfo &source);
  std::string resolveOverload(const std::string &baseName,
                              const std::vector<TypeInfo> &argTypes,
                              ASTNode *node, TypeInfo &outReturnType);

  bool methodExistsInClass(const std::string &className,
                           const std::string &methodName);
  void validateInterfaceCompliance(StructDeclNode *node,
                                   const std::string &interfaceName);
  bool hasClassField(StructDeclNode *node, const std::string &name,
                     const std::string &type);
  bool hasClassMethod(StructDeclNode *node, FunctionNode *m);
  std::string resolveParamType(StructDeclNode *node,
                               const FunctionParam &param);

public:
  std::map<ASTNode*, TypeInfo> nodeTypes;
  const std::map<std::string, StructDef> &getCustomStructs() const {
    return customStructs;
  }
  const std::map<std::string, TypeInfo> &getFunctionTypes() const {
    return functionTypes;
  }
  const std::map<std::string, std::vector<OverloadCandidate>> &
  getOverloadTable() const {
    return overloadTable;
  }

private:
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
  void visit(CastNode *node) override;
  void visit(AssignNode *node) override;
  void visit(VarDeclNode *node) override;
  void visit(ReturnNode *node) override;
  void visit(FunctionNode *node) override;
  void visit(ProgramNode *node) override;
  void visit(ModuleNode *node) override;
};

} // namespace utopia