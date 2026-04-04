#pragma once
#include <memory>
#include <string>
#include <vector>

namespace utopia {

class ASTVisitor;

enum class AccessModifier { Implicit, Public, Private };

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

class ThisNode : public ExprNode {
public:
  void accept(ASTVisitor *visitor) override;
};

class MemberAccessNode : public ExprNode {
public:
  std::unique_ptr<ExprNode> object;
  std::string field;

  MemberAccessNode(std::unique_ptr<ExprNode> obj, std::string f)
      : object(std::move(obj)), field(std::move(f)) {}

  void accept(ASTVisitor *visitor) override;
};

struct StructField {
  AccessModifier modifier;
  std::string typeName;
  std::string name;
  std::vector<std::string> decorators;
  std::unique_ptr<ExprNode> initializer = nullptr;
};

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

class LogicalNotNode : public ExprNode {
public:
  std::unique_ptr<ExprNode> operand;
  explicit LogicalNotNode(std::unique_ptr<ExprNode> op)
      : operand(std::move(op)) {}
  void accept(ASTVisitor *visitor) override;
};

class UnaryMinusNode : public ExprNode {
public:
  std::unique_ptr<ExprNode> operand;
  explicit UnaryMinusNode(std::unique_ptr<ExprNode> op)
      : operand(std::move(op)) {}
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
  std::vector<std::unique_ptr<ExprNode>> arguments;
  std::unique_ptr<ExprNode> arraySize;

  explicit NewNode(std::string tName,
                   std::vector<std::unique_ptr<ExprNode>> args = {})
      : typeName(std::move(tName)), arguments(std::move(args)),
        arraySize(nullptr) {}

  void accept(ASTVisitor *visitor) override;
};

class DeleteNode : public StmtNode {
public:
  std::unique_ptr<ExprNode> pointerExpr;
  bool isArray;

  explicit DeleteNode(std::unique_ptr<ExprNode> ptr, bool isArr = false)
      : pointerExpr(std::move(ptr)), isArray(isArr) {}

  void accept(ASTVisitor *visitor) override;
};

class MoveNode : public ExprNode {
public:
  std::unique_ptr<ExprNode> operand;
  explicit MoveNode(std::unique_ptr<ExprNode> op) : operand(std::move(op)) {}

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
  std::unique_ptr<ExprNode> object;

  CallNode(std::string name, std::vector<std::unique_ptr<ExprNode>> args,
           std::unique_ptr<ExprNode> obj = nullptr)
      : callee(std::move(name)), arguments(std::move(args)),
        object(std::move(obj)) {}

  void accept(ASTVisitor *visitor) override;
};

class AssignNode : public StmtNode {
public:
  std::unique_ptr<ExprNode> target;
  std::unique_ptr<ExprNode> value;
  std::string op;

  AssignNode(std::unique_ptr<ExprNode> t, std::unique_ptr<ExprNode> v,
             std::string o = "=")
      : target(std::move(t)), value(std::move(v)), op(std::move(o)) {}

  void accept(ASTVisitor *visitor) override;
};

class SubscriptNode : public ExprNode {
public:
  std::unique_ptr<ExprNode> object;
  std::unique_ptr<ExprNode> index;

  SubscriptNode(std::unique_ptr<ExprNode> obj, std::unique_ptr<ExprNode> idx)
      : object(std::move(obj)), index(std::move(idx)) {}

  void accept(ASTVisitor *visitor) override;
};

class VarDeclNode : public StmtNode {
public:
  std::string typeName;
  std::string name;
  bool isConst;
  std::vector<std::string> decorators;
  std::unique_ptr<ExprNode> initializer;
  std::unique_ptr<ExprNode> arraySize;

  VarDeclNode(std::string t, std::string n, bool c,
              std::unique_ptr<ExprNode> init)
      : typeName(std::move(t)), name(std::move(n)), isConst(c),
        initializer(std::move(init)), arraySize(nullptr) {}

  void accept(ASTVisitor *visitor) override;
};

class ReturnNode : public StmtNode {
public:
  std::unique_ptr<ExprNode> returnValue;
  explicit ReturnNode(std::unique_ptr<ExprNode> val)
      : returnValue(std::move(val)) {}

  void accept(ASTVisitor *visitor) override;
};

struct FunctionParam {
  std::string type;
  std::string name;
  bool isRequired = false;
  bool isThisAssign = false;
};

class FunctionNode : public ASTNode {
public:
  InlineState inlineState;
  AccessModifier access;
  std::vector<std::string> decorators;
  std::string returnType;
  std::string name;
  std::vector<FunctionParam> args;
  std::vector<std::unique_ptr<ASTNode>> body;

  bool isMethod;
  bool isConstructor;
  bool isDestructor = false;
  std::string className;

  FunctionNode(InlineState is, AccessModifier acc,
               std::vector<std::string> decs, std::string retT, std::string n,
               std::vector<FunctionParam> a, bool isMeth = false,
               bool isCtor = false, bool isDtor = false, std::string cName = "")
      : inlineState(is), access(acc), decorators(std::move(decs)),
        returnType(std::move(retT)), name(std::move(n)), args(std::move(a)),
        isMethod(isMeth), isConstructor(isCtor), isDestructor(isDtor),
        className(std::move(cName)) {}

  void accept(ASTVisitor *visitor) override;
};

class StructDeclNode : public ASTNode {
public:
  std::string name;
  bool isClass;
  std::vector<std::string> decorators;
  std::vector<StructField> fields;
  std::vector<std::unique_ptr<FunctionNode>> methods;

  StructDeclNode(std::string n, bool c, std::vector<StructField> f)
      : name(std::move(n)), isClass(c), fields(std::move(f)) {}

  void accept(ASTVisitor *visitor) override;
};

class ProgramNode : public ASTNode {
public:
  std::vector<std::unique_ptr<StructDeclNode>> structs;
  std::vector<std::unique_ptr<FunctionNode>> functions;

  void accept(ASTVisitor *visitor) override;
};

} // namespace utopia