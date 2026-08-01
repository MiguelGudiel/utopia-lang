#pragma once
#include "utopia/Common/Types.hpp"
#include "utopia/Lexer/Token.hpp"
#include <cstdint>
#include <llvm/ADT/ArrayRef.h>
#include <string_view>
#include <unordered_set>

namespace utopia {

enum class NodeKind : uint8_t {
  Number,
  Boolean,
  Char,
  Rune,
  String,
  Variable,
  BinaryOp,
  UnaryOp,
  Module,
  Annotation,
  AnnotationDecl,
  TypedefDecl,
  VarDecl,
  Assign,
  Block,
  If,
  For,
  While,
  Switch,
  Case,
  Break,
  Continue,
  FunctionDecl,
  FunctionCall,
  Return,
  Cast,
  ParamDecl,
  StructDecl,
  UnionDecl,
  ClassDecl,
  MemberAccess,
  ArraySubscript,
  ArrayLiteral,
  New,
  Delete,
  Null,
  EnumDecl,
  EnumMember,
  ImplicitCast
};

struct ASTNode {
  NodeKind kind;
  int line;
  int column;
  int length;
  int endLine;
  std::string_view docString;
  std::string_view trailingComment;

  explicit ASTNode(NodeKind k, int l = 0, int c = 0, int len = 0)
      : kind(k), line(l), column(c), length(len), endLine(l) {}
};

struct StmtNode : public ASTNode {
  explicit StmtNode(NodeKind k, int l = 0, int c = 0, int len = 0)
      : ASTNode(k, l, c, len) {}
};

struct ExprNode : public ASTNode {
  mutable const Type *exprType = nullptr;
  mutable bool isLValue =
      false; // Tracks if the expression represents a persistent memory location
  bool hasParens = false;

  explicit ExprNode(NodeKind k, int l = 0, int c = 0, int len = 0)
      : ASTNode(k, l, c, len) {}
};

struct AnnotationNode : public ASTNode {
  std::string_view name;
  llvm::ArrayRef<ExprNode *> args;
  AnnotationNode(std::string_view n, llvm::ArrayRef<ExprNode *> a, int l, int c,
                 int len)
      : ASTNode(NodeKind::Annotation, l, c, len), name(n), args(a) {}
};

struct FunctionDeclNode;

struct DeclNode : public ASTNode {
  llvm::ArrayRef<AnnotationNode *> annotations;
  bool hasPublicMod = false;
  bool hasPrivateMod = false;
  std::string_view declFilePath;

  bool isTemplate = false;
  llvm::ArrayRef<std::string_view> templateParams;

  explicit DeclNode(NodeKind k, int l = 0, int c = 0, int len = 0)
      : ASTNode(k, l, c, len) {}

  /**
   * Resolves visibility state based on modifier override and implicitly
   * inferred privacy rules (`_` prefix).
   */
  bool isPublic(std::string_view declName) const {
    if (hasPrivateMod)
      return false;
    if (hasPublicMod)
      return true;
    return !declName.starts_with("_");
  }
};

struct EnumMemberNode : public DeclNode {
  std::string_view name;
  ExprNode *initializer;
  mutable int64_t evaluatedValue = 0; // Evaluated statically during Sema

  EnumMemberNode(std::string_view n, ExprNode *init, int l, int c, int len)
      : DeclNode(NodeKind::EnumMember, l, c, len), name(n), initializer(init) {}
};

struct EnumDeclNode : public DeclNode {
  std::string_view name;
  const Type *underlyingType;
  llvm::ArrayRef<EnumMemberNode *> members;
  mutable const EnumType *enumType = nullptr;

  EnumDeclNode(std::string_view n, const Type *u, int l, int c, int len)
      : DeclNode(NodeKind::EnumDecl, l, c, len), name(n), underlyingType(u) {}
};

struct NumberNode : public ExprNode {
  std::string_view raw;
  bool isFloat;
  NumberNode(std::string_view r, bool f, int l, int c, int len)
      : ExprNode(NodeKind::Number, l, c, len), raw(r), isFloat(f) {}
};

struct BoolNode : public ExprNode {
  bool value;
  BoolNode(bool v, int l, int c, int len)
      : ExprNode(NodeKind::Boolean, l, c, len), value(v) {}
};

struct CharNode : public ExprNode {
  uint8_t value;
  CharNode(uint8_t v, int l, int c, int len)
      : ExprNode(NodeKind::Char, l, c, len), value(v) {}
};

struct RuneNode : public ExprNode {
  uint32_t value;
  RuneNode(uint32_t v, int l, int c, int len)
      : ExprNode(NodeKind::Rune, l, c, len), value(v) {}
};

struct StringNode : public ExprNode {
  std::string_view value;
  StringNode(std::string_view v, int l, int c, int len)
      : ExprNode(NodeKind::String, l, c, len), value(v) {}
};

struct NullNode : public ExprNode {
  NullNode(int l, int c, int len) : ExprNode(NodeKind::Null, l, c, len) {}
};

struct VariableNode : public ExprNode {
  std::string_view name;
  bool isField = false;
  uint32_t fieldIndex = 0;
  const Type *parentType = nullptr;
  mutable const DeclNode *resolvedDecl = nullptr;

  llvm::ArrayRef<const Type *> templateArgs;

  VariableNode(std::string_view n, int l, int c, int len)
      : ExprNode(NodeKind::Variable, l, c, len), name(n) {}
};

struct TypedefDeclNode : public DeclNode {
  std::string_view aliasName;
  mutable const Type *targetType;
  std::string_view targetEntityName;
  const AliasType *aliasType;
  std::string_view rawTargetTypeStr;

  TypedefDeclNode(std::string_view name, const Type *target, int l, int c,
                  int len)
      : DeclNode(NodeKind::TypedefDecl, l, c, len), aliasName(name),
        targetType(target), aliasType(nullptr) {}
};

struct UnaryOpNode : public ExprNode {
  std::string_view op;
  ExprNode *expr;
  bool isPostfix;
  mutable const FunctionDeclNode *overloadedOperator = nullptr;

  UnaryOpNode(std::string_view o, ExprNode *e, int l, int c,
              bool postfix = false)
      : ExprNode(NodeKind::UnaryOp, l, c), op(o), expr(e), isPostfix(postfix) {
    if (postfix) {
      this->length = (e->column + e->length) - c + o.length();
    } else {
      this->length = (e->column + e->length) - c;
    }
  }
};

struct BinaryOpNode : public ExprNode {
  std::string_view op;
  ExprNode *left;
  ExprNode *right;
  mutable const Type *promotedType = nullptr;
  mutable const FunctionDeclNode *overloadedOperator = nullptr;

  BinaryOpNode(std::string_view o, ExprNode *l, ExprNode *r, int ln, int c)
      : ExprNode(NodeKind::BinaryOp, ln, c), op(o), left(l), right(r) {
    this->length = (right->column + right->length) - this->column;
  }
};

struct VarDeclNode : public DeclNode {
  const Type *type;
  std::string_view varName;
  std::string mangledName;
  ExprNode *initializer;
  bool isGlobal = false;
  bool isStatic = false;
  std::string_view rawTypeStr;

  /* Reference to the resolved copy constructor for aggregate initialization */
  mutable const FunctionDeclNode *copyCtor = nullptr;

  VarDeclNode(const Type *t, std::string_view n, ExprNode *init, int l, int c,
              int len)
      : DeclNode(NodeKind::VarDecl, l, c, len), type(t), varName(n),
        initializer(init) {}
};

struct AssignNode : public ExprNode {
  std::string_view op;
  ExprNode *target;
  ExprNode *value;
  mutable const FunctionDeclNode *overloadedOperator = nullptr;

  AssignNode(std::string_view o, ExprNode *t, ExprNode *v, int l, int c,
             int len)
      : ExprNode(NodeKind::Assign, l, c, len), op(o), target(t), value(v) {}
};

struct BlockNode : public StmtNode {
  llvm::ArrayRef<ASTNode *> statements;
  bool isExpressionBody = false;

  BlockNode(int l, int c) : StmtNode(NodeKind::Block, l, c, 1) {}
  void finalize(int endCol) { this->length = endCol - this->column; }
};

struct ReturnNode : public StmtNode {
  ExprNode *value;
  ReturnNode(ExprNode *v, int l, int c, int len)
      : StmtNode(NodeKind::Return, l, c, len), value(v) {}
};

struct ParamDeclNode : public DeclNode {
  const Type *type;
  std::string_view name;
  ExprNode *defaultValue;
  bool isNamed;
  bool isRequired;
  std::string_view rawTypeStr;

  ParamDeclNode(const Type *t, std::string_view n, ExprNode *defVal, bool isN,
                bool isReq, int l, int c, int len)
      : DeclNode(NodeKind::ParamDecl, l, c, len), type(t), name(n),
        defaultValue(defVal), isNamed(isN), isRequired(isReq) {}
};

struct FunctionDeclNode : public DeclNode {
  const Type *returnType;
  std::string_view name;
  std::string mangledName;
  llvm::ArrayRef<ParamDeclNode *> params;
  BlockNode *body;
  bool isConst;
  bool isMethod;
  bool isExtern;
  bool isVariadic;
  bool isImplicit;
  bool isStatic = false;
  mutable std::string_view externAlias;
  const RecordType *parentRecord = nullptr;
  std::string_view rawReturnTypeStr;

  /* Intrinsic function attributes inferred during semantic analysis */
  mutable bool isReadNone = false;
  mutable bool isReadOnly = false;
  mutable bool isNoFree = false;
  mutable bool isNoSync = false;
  mutable bool isWillReturn = false;
  mutable bool isMustProgress = false;

  FunctionDeclNode(const Type *ret, std::string_view n, int l, int c,
                   bool isC = false, bool isMeth = false, bool isExt = false,
                   bool isVar = false, bool isImpl = false)
      : DeclNode(NodeKind::FunctionDecl, l, c), returnType(ret), name(n),
        body(nullptr), isConst(isC), isMethod(isMeth), isExtern(isExt),
        isVariadic(isVar), isImplicit(isImpl) {}
};

struct FunctionCallNode : public ExprNode {
  ExprNode *target;
  llvm::ArrayRef<ExprNode *> args;
  llvm::ArrayRef<std::string_view> argNames;
  const FunctionDeclNode *resolvedFunc = nullptr;
  llvm::ArrayRef<ExprNode *> rawArgs;
  llvm::ArrayRef<std::string_view> rawArgNames;
  bool hasRawArgs = false;

  FunctionCallNode(ExprNode *t, llvm::ArrayRef<ExprNode *> a,
                   llvm::ArrayRef<std::string_view> n, int l, int c, int len)
      : ExprNode(NodeKind::FunctionCall, l, c, len), target(t), args(a),
        argNames(n) {}
};

struct CastNode : public ExprNode {
  ExprNode *expr;
  const Type *targetType;
  std::string_view rawTargetTypeStr;

  CastNode(ExprNode *e, const Type *target, int l, int c, int len)
      : ExprNode(NodeKind::Cast, l, c, len), expr(e), targetType(target) {}
};

struct ForNode : public StmtNode {
  ASTNode *initStatement; // It can be a VarDeclNode or an ExprNode (or null).
  ExprNode *condition;    // Optional
  ExprNode *increment;    // Optional
  BlockNode *body;

  ForNode(ASTNode *init, ExprNode *cond, ExprNode *inc, BlockNode *b, int l,
          int c, int len)
      : StmtNode(NodeKind::For, l, c, len), initStatement(init),
        condition(cond), increment(inc), body(b) {}
};

struct WhileNode : public StmtNode {
  ExprNode *condition;
  BlockNode *body;

  WhileNode(ExprNode *cond, BlockNode *b, int l, int c, int len)
      : StmtNode(NodeKind::While, l, c, len), condition(cond), body(b) {}
};

struct CaseNode : public ASTNode {
  ExprNode *value; /* nullptr represents 'default' */
  llvm::ArrayRef<ASTNode *> statements;

  CaseNode(ExprNode *v, llvm::ArrayRef<ASTNode *> stmts, int l, int c, int len)
      : ASTNode(NodeKind::Case, l, c, len), value(v), statements(stmts) {}
};

struct SwitchNode : public StmtNode {
  ExprNode *condition;
  llvm::ArrayRef<CaseNode *> cases;
  bool hasDefault;

  SwitchNode(ExprNode *cond, llvm::ArrayRef<CaseNode *> c, bool hd, int l,
             int col, int len)
      : StmtNode(NodeKind::Switch, l, col, len), condition(cond), cases(c),
        hasDefault(hd) {}
};

struct BreakNode : public StmtNode {
  BreakNode(int l, int c, int len) : StmtNode(NodeKind::Break, l, c, len) {}
};

struct ContinueNode : public StmtNode {
  ContinueNode(int l, int c, int len)
      : StmtNode(NodeKind::Continue, l, c, len) {}
};

struct IfNode : public StmtNode {
  ExprNode *condition;
  BlockNode *thenBlock;
  ASTNode *elseBlock;

  IfNode(ExprNode *cond, BlockNode *tb, ASTNode *eb, int l, int c, int len)
      : StmtNode(NodeKind::If, l, c, len), condition(cond), thenBlock(tb),
        elseBlock(eb) {}
};

struct ModuleNode : public ASTNode {
  std::string_view filePath;
  llvm::ArrayRef<std::string_view> rawImports;
  llvm::ArrayRef<ModuleNode *> importedModules;

  llvm::ArrayRef<std::string_view> rawExports;
  llvm::ArrayRef<ModuleNode *> exportedModules;

  llvm::ArrayRef<ASTNode *> statements;
  std::vector<ASTNode *> instantiatedTemplates;

  explicit ModuleNode(std::string_view path)
      : ASTNode(NodeKind::Module), filePath(path) {}

  bool canSee(std::string_view targetFilePath) const;
  bool exports(std::string_view targetFilePath,
               std::unordered_set<const ModuleNode *> &visited) const;
};

struct AnnotationDeclNode : public DeclNode {
  std::string_view name;
  llvm::ArrayRef<VarDeclNode *> fields;
  FunctionDeclNode *constructor;

  mutable const RecordType *recordType = nullptr;

  AnnotationDeclNode(std::string_view n, int l, int c, int len)
      : DeclNode(NodeKind::AnnotationDecl, l, c, len), name(n),
        constructor(nullptr) {}
};

struct UnionDeclNode : public DeclNode {
  std::string_view name;
  llvm::ArrayRef<VarDeclNode *> fields;
  llvm::ArrayRef<FunctionDeclNode *> methods;
  llvm::ArrayRef<FunctionDeclNode *> constructors;
  FunctionDeclNode *destructor;

  mutable const RecordType *recordType = nullptr;
  bool isOpaque = false;

  UnionDeclNode(std::string_view n, int l, int c, int len)
      : DeclNode(NodeKind::UnionDecl, l, c, len), name(n), destructor(nullptr) {
  }
};

struct StructDeclNode : public DeclNode {
  std::string_view name;
  llvm::ArrayRef<VarDeclNode *> fields;
  llvm::ArrayRef<FunctionDeclNode *> methods;
  llvm::ArrayRef<FunctionDeclNode *> constructors;
  FunctionDeclNode *destructor;

  mutable const RecordType *recordType = nullptr;
  bool isOpaque = false;

  StructDeclNode(std::string_view n, int l, int c, int len)
      : DeclNode(NodeKind::StructDecl, l, c, len), name(n),
        destructor(nullptr) {}
};

struct ClassDeclNode : public DeclNode {
  std::string_view name;
  llvm::ArrayRef<VarDeclNode *> fields;
  llvm::ArrayRef<FunctionDeclNode *> methods;
  llvm::ArrayRef<FunctionDeclNode *> constructors;
  FunctionDeclNode *destructor;

  mutable const RecordType *recordType = nullptr;
  bool isOpaque = false;

  ClassDeclNode(std::string_view n, int l, int c, int len)
      : DeclNode(NodeKind::ClassDecl, l, c, len), name(n), destructor(nullptr) {
  }
};

struct MemberAccessNode : public ExprNode {
  ExprNode *object;
  std::string_view memberName;
  bool isMethodRef = false;
  const FunctionDeclNode *resolvedMethod = nullptr;
  uint32_t fieldIndex = 0;

  bool isEnumMember = false;
  const EnumMemberNode *enumMember = nullptr;

  bool isStaticFieldRef = false;
  const VarDeclNode *resolvedVar = nullptr;

  /* Storage for explicit template arguments applied to method access */
  llvm::ArrayRef<const Type *> templateArgs;

  MemberAccessNode(ExprNode *obj, std::string_view mem, int l, int c, int len)
      : ExprNode(NodeKind::MemberAccess, l, c, len), object(obj),
        memberName(mem) {}
};

struct ArraySubscriptNode : public ExprNode {
  ExprNode *base;
  ExprNode *index;
  mutable const FunctionDeclNode *overloadedOperator = nullptr;

  ArraySubscriptNode(ExprNode *b, ExprNode *i, int l, int c, int len)
      : ExprNode(NodeKind::ArraySubscript, l, c, len), base(b), index(i) {}
};

struct ArrayLiteralNode : public ExprNode {
  llvm::ArrayRef<ExprNode *> elements;
  ArrayLiteralNode(llvm::ArrayRef<ExprNode *> elems, int l, int c, int len)
      : ExprNode(NodeKind::ArrayLiteral, l, c, len), elements(elems) {}
};

struct NewExprNode : public ExprNode {
  const Type *allocatedType;
  ExprNode *arraySize;
  llvm::ArrayRef<ExprNode *> args;
  llvm::ArrayRef<std::string_view> argNames;
  bool hasParens;
  const FunctionDeclNode *resolvedConstructor = nullptr;
  std::string_view rawAllocatedTypeStr;
  llvm::ArrayRef<ExprNode *> rawArgs;
  llvm::ArrayRef<std::string_view> rawArgNames;
  bool hasRawArgs = false;

  NewExprNode(const Type *allocTy, ExprNode *arrSize,
              llvm::ArrayRef<ExprNode *> a, llvm::ArrayRef<std::string_view> n,
              bool hasParens, int l, int c, int len)
      : ExprNode(NodeKind::New, l, c, len), allocatedType(allocTy),
        arraySize(arrSize), args(a), argNames(n), hasParens(hasParens) {}
};

struct DeleteExprNode : public ExprNode {
  ExprNode *ptr;
  bool isArray;

  DeleteExprNode(ExprNode *p, bool isArr, int l, int c, int len)
      : ExprNode(NodeKind::Delete, l, c, len), ptr(p), isArray(isArr) {}
};

struct ImplicitCastNode : public ExprNode {
  ExprNode *expr;
  const Type *targetType;
  const FunctionDeclNode *conversionConstructor = nullptr;

  ImplicitCastNode(ExprNode *e, const Type *target,
                   const FunctionDeclNode *ctor, int l, int c, int len)
      : ExprNode(NodeKind::ImplicitCast, l, c, len), expr(e),
        targetType(target), conversionConstructor(ctor) {}
};

} // namespace utopia