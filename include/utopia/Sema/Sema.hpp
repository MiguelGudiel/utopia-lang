#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Common/Types.hpp"
#include <map>
#include <string>
#include <vector>

namespace utopia {

class Sema : public ASTVisitor {
public:
  bool analyze(ProgramNode *program);
  const std::vector<ErrorInfo> &getErrors() const { return errors; }

private:
  struct Symbol {
    TypeInfo type;
    bool isConst;
  };

  struct StructDef {
    bool isClass;
    struct Field {
      TypeInfo type;
      AccessModifier mod;
      int index;
    };
    std::map<std::string, Field> fields;
    std::vector<int> constructorArities;
  };
  std::map<std::string, StructDef> customStructs;
  std::map<std::string, std::string>
      copyConstructors; // ClassName -> MangledName

  std::vector<std::map<std::string, Symbol>> scopeStack;
  std::vector<ErrorInfo> errors;

  std::map<std::string, TypeInfo> functionTypes;
  TypeInfo currentExprType;
  TypeInfo currentReturnType;
  std::string currentClass;

  int loopDepth = 0;

  void enterScope();
  void exitScope();
  Symbol *lookup(const std::string &name);

  void reportError(ASTNode *node, const std::string &message);
  TypeInfo parseType(const std::string &typeName, ASTNode *node = nullptr);
  bool checkAssignment(const TypeInfo &target, const TypeInfo &source,
                       ASTNode *node);

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
};

} // namespace utopia