#pragma once
#include <memory>
#include <string>
#include <vector>

namespace utopia {

class ASTVisitor;

class ASTNode {
public:
  int line = 0;
  int column = 0;
  int endLine = 0;
  int endColumn = 0;

  virtual ~ASTNode() = default;

  void setRange(int l, int c, int el, int ec) {
    line = l;
    column = c;
    endLine = el;
    endColumn = ec;
  }

  // Double dispatch vectoring
  virtual void accept(ASTVisitor *visitor) = 0;
};

// Expressions evaluate to a value
class ExprNode : public ASTNode {};

// Statements execute an action but yield no value
class StmtNode : public ASTNode {};

class BlockNode : public StmtNode {
public:
  std::vector<std::unique_ptr<ASTNode>> statements;
  explicit BlockNode(std::vector<std::unique_ptr<ASTNode>> stmts)
      : statements(std::move(stmts)) {}
  void accept(ASTVisitor *visitor) override;
};

class NullLiteralNode : public ExprNode {
public:
  void accept(ASTVisitor *visitor) override;
};

class IfNode : public StmtNode {
public:
  std::unique_ptr<ExprNode> condition;
  std::vector<std::unique_ptr<ASTNode>> thenBody;
  std::vector<std::unique_ptr<ASTNode>> elseBody;

  IfNode(std::unique_ptr<ExprNode> cond,
         std::vector<std::unique_ptr<ASTNode>> tBody)
      : condition(std::move(cond)), thenBody(std::move(tBody)) {}

  void accept(ASTVisitor *visitor) override;
};

class WhileNode : public StmtNode {
public:
  std::unique_ptr<ExprNode> condition;
  std::vector<std::unique_ptr<ASTNode>> body;

  WhileNode(std::unique_ptr<ExprNode> cond,
            std::vector<std::unique_ptr<ASTNode>> b)
      : condition(std::move(cond)), body(std::move(b)) {}

  void accept(ASTVisitor *visitor) override;
};

class ForNode : public StmtNode {
public:
  std::unique_ptr<ASTNode> init;
  std::unique_ptr<ExprNode> condition;
  std::unique_ptr<ASTNode> update;
  std::vector<std::unique_ptr<ASTNode>> body;

  ForNode(std::unique_ptr<ASTNode> i, std::unique_ptr<ExprNode> c,
          std::unique_ptr<ASTNode> u, std::vector<std::unique_ptr<ASTNode>> b)
      : init(std::move(i)), condition(std::move(c)), update(std::move(u)),
        body(std::move(b)) {}

  void accept(ASTVisitor *visitor) override;
};

class NullAssertNode : public ExprNode {
public:
  std::unique_ptr<ExprNode> operand;
  explicit NullAssertNode(std::unique_ptr<ExprNode> op)
      : operand(std::move(op)) {}

  void accept(ASTVisitor *visitor) override;
};

class NumberNode : public ExprNode {
public:
  int value;
  explicit NumberNode(int v) : value(v) {}
  void accept(ASTVisitor *visitor) override;
};

class FloatNode : public ExprNode {
public:
  double value;
  explicit FloatNode(double v) : value(v) {}
  void accept(ASTVisitor *visitor) override;
};

class BoolNode : public ExprNode {
public:
  bool value;
  explicit BoolNode(bool v) : value(v) {}
  void accept(ASTVisitor *visitor) override;
};

class StringNode : public ExprNode {
public:
  std::string value;
  explicit StringNode(std::string v) : value(std::move(v)) {}
  void accept(ASTVisitor *visitor) override;
};

class VariableNode : public ExprNode {
public:
  std::string name;
  explicit VariableNode(std::string n) : name(std::move(n)) {}
  void accept(ASTVisitor *visitor) override;
};

class AddressOfNode : public ExprNode {
public:
  std::unique_ptr<ExprNode> operand;
  explicit AddressOfNode(std::unique_ptr<ExprNode> op)
      : operand(std::move(op)) {}
  void accept(ASTVisitor *visitor) override;
};

class DerefNode : public ExprNode {
public:
  std::unique_ptr<ExprNode> operand;
  explicit DerefNode(std::unique_ptr<ExprNode> op) : operand(std::move(op)) {}
  void accept(ASTVisitor *visitor) override;
};

class NewNode : public ExprNode {
public:
  std::string typeName;
  std::unique_ptr<ExprNode> initializer;

  explicit NewNode(std::string tName, std::unique_ptr<ExprNode> init = nullptr)
      : typeName(std::move(tName)), initializer(std::move(init)) {}

  void accept(ASTVisitor *visitor) override;
};

class DeleteNode : public StmtNode {
public:
  std::unique_ptr<ExprNode> pointerExpr;
  explicit DeleteNode(std::unique_ptr<ExprNode> ptr)
      : pointerExpr(std::move(ptr)) {}

  void accept(ASTVisitor *visitor) override;
};

class BreakNode : public StmtNode {
public:
  void accept(ASTVisitor *visitor) override;
};

class ContinueNode : public StmtNode {
public:
  void accept(ASTVisitor *visitor) override;
};

enum class InlineState { None, Inline, ForceInline };

class BinaryOpNode : public ExprNode {
public:
  std::string op;
  std::unique_ptr<ExprNode> left;
  std::unique_ptr<ExprNode> right;

  BinaryOpNode(std::string o, std::unique_ptr<ExprNode> l,
               std::unique_ptr<ExprNode> r)
      : op(std::move(o)), left(std::move(l)), right(std::move(r)) {}

  void accept(ASTVisitor *visitor) override;
};

class CallNode : public ExprNode {
public:
  std::string callee;
  std::vector<std::unique_ptr<ExprNode>> arguments;

  CallNode(std::string name, std::vector<std::unique_ptr<ExprNode>> args)
      : callee(std::move(name)), arguments(std::move(args)) {}

  void accept(ASTVisitor *visitor) override;
};

class AssignNode : public StmtNode {
public:
  std::unique_ptr<ExprNode> target;
  std::unique_ptr<ExprNode> value;

  AssignNode(std::unique_ptr<ExprNode> t, std::unique_ptr<ExprNode> v)
      : target(std::move(t)), value(std::move(v)) {}

  void accept(ASTVisitor *visitor) override;
};

class VarDeclNode : public StmtNode {
public:
  std::string typeName;
  std::string name;
  bool isConst;
  std::unique_ptr<ExprNode> initializer;

  VarDeclNode(std::string t, std::string n, bool c,
              std::unique_ptr<ExprNode> init)
      : typeName(std::move(t)), name(std::move(n)), isConst(c),
        initializer(std::move(init)) {}

  void accept(ASTVisitor *visitor) override;
};

class ReturnNode : public StmtNode {
public:
  std::unique_ptr<ExprNode> returnValue;
  explicit ReturnNode(std::unique_ptr<ExprNode> val)
      : returnValue(std::move(val)) {}

  void accept(ASTVisitor *visitor) override;
};

class FunctionNode : public ASTNode {
public:
  InlineState inlineState;
  std::string returnType;
  std::string name;
  std::vector<std::pair<std::string, std::string>> args;
  std::vector<std::unique_ptr<ASTNode>> body;

  FunctionNode(InlineState is, std::string retT, std::string n,
               std::vector<std::pair<std::string, std::string>> a)
      : inlineState(is), returnType(std::move(retT)), name(std::move(n)),
        args(std::move(a)) {}

  void accept(ASTVisitor *visitor) override;
};

class ProgramNode : public ASTNode {
public:
  std::vector<std::unique_ptr<FunctionNode>> functions;

  void accept(ASTVisitor *visitor) override;
};

} // namespace utopia