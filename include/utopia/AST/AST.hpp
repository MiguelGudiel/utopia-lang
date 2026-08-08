#pragma once
#include "utopia/Common/Types.hpp"
#include <cstdint>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/Casting.h>
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
  ImplicitCast,
  TypeLiteral,
  TernaryOp,
  NamespaceDecl,
  Using
};

struct ASTNode {
  NodeKind kind;
  int line;
  int column;
  int length;
  int endLine;
  std::string_view docString;
  std::string_view trailingComment;
  std::string_view endComment;

  explicit ASTNode(NodeKind k, int l = 0, int c = 0, int len = 0)
      : kind(k), line(l), column(c), length(len), endLine(l) {}
};

struct StmtNode : public ASTNode {
  explicit StmtNode(NodeKind k, int l = 0, int c = 0, int len = 0)
      : ASTNode(k, l, c, len) {}

  static bool classof(const ASTNode *node) {
    switch (node->kind) {
    case NodeKind::Block:
    case NodeKind::If:
    case NodeKind::For:
    case NodeKind::While:
    case NodeKind::Switch:
    case NodeKind::Break:
    case NodeKind::Continue:
    case NodeKind::Return:
    case NodeKind::Using:
      return true;
    default:
      return false;
    }
  }
};

struct ExprNode : public ASTNode {
  mutable const Type *exprType = nullptr;
  mutable bool isLValue = false;
  bool hasParens = false;
  mutable const Type *representedType = nullptr;

  explicit ExprNode(NodeKind k, int l = 0, int c = 0, int len = 0)
      : ASTNode(k, l, c, len) {}

  static bool classof(const ASTNode *node) {
    switch (node->kind) {
    case NodeKind::Number:
    case NodeKind::Boolean:
    case NodeKind::Char:
    case NodeKind::Rune:
    case NodeKind::String:
    case NodeKind::Variable:
    case NodeKind::BinaryOp:
    case NodeKind::UnaryOp:
    case NodeKind::TernaryOp:
    case NodeKind::Assign:
    case NodeKind::FunctionCall:
    case NodeKind::Cast:
    case NodeKind::MemberAccess:
    case NodeKind::ArraySubscript:
    case NodeKind::ArrayLiteral:
    case NodeKind::New:
    case NodeKind::Delete:
    case NodeKind::Null:
    case NodeKind::ImplicitCast:
    case NodeKind::TypeLiteral:
      return true;
    default:
      return false;
    }
  }
};

struct TypeLiteralNode : public ExprNode {
  TypeLiteralNode(const Type *t, int l, int c, int len)
      : ExprNode(NodeKind::TypeLiteral, l, c, len) {
    representedType = t;
  }

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::TypeLiteral;
  }
};

struct AnnotationNode : public ASTNode {
  std::string_view name;
  llvm::ArrayRef<ExprNode *> args;

  /* Indicates whether the node was parsed with a trailing comma to force
   * formatting splits. */
  bool hasTrailingComma = false;

  AnnotationNode(std::string_view n, llvm::ArrayRef<ExprNode *> a, int l, int c,
                 int len)
      : ASTNode(NodeKind::Annotation, l, c, len), name(n), args(a) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Annotation;
  }
};

struct FunctionDeclNode;

struct DeclNode : public ASTNode {
  llvm::ArrayRef<AnnotationNode *> annotations;
  bool hasPublicMod = false;
  bool hasPrivateMod = false;
  std::string_view declFilePath;
  std::string_view fqName; // Fully Qualified Name (ej. mi.name.space.Class)

  /* Exact token location for LSP tooling */
  int identifierColumn = 0;
  int identifierLength = 0;

  bool isTemplate = false;
  llvm::ArrayRef<std::string_view> templateParams;

  /* Resolved attributes */
  uint64_t alignment = 0;
  bool isPacked = false;

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

    size_t pos = declName.find_last_of('.');
    if (pos != std::string_view::npos) {
      declName = declName.substr(pos + 1);
    }
    return !declName.starts_with("_");
  }

  static bool classof(const ASTNode *node) {
    switch (node->kind) {
    case NodeKind::AnnotationDecl:
    case NodeKind::TypedefDecl:
    case NodeKind::VarDecl:
    case NodeKind::FunctionDecl:
    case NodeKind::ParamDecl:
    case NodeKind::StructDecl:
    case NodeKind::UnionDecl:
    case NodeKind::ClassDecl:
    case NodeKind::EnumDecl:
    case NodeKind::EnumMember:
    case NodeKind::NamespaceDecl:
      return true;
    default:
      return false;
    }
  }
};

struct EnumMemberNode : public DeclNode {
  std::string_view name;
  ExprNode *initializer;
  mutable int64_t evaluatedValue = 0;

  EnumMemberNode(std::string_view n, ExprNode *init, int l, int c, int len)
      : DeclNode(NodeKind::EnumMember, l, c, len), name(n), initializer(init) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::EnumMember;
  }
};

struct EnumDeclNode : public DeclNode {
  std::string_view name;
  const Type *underlyingType;
  llvm::ArrayRef<EnumMemberNode *> members;
  mutable const EnumType *enumType = nullptr;

  /* Indicates whether the node was parsed with a trailing comma to force
   * formatting splits. */
  bool hasTrailingComma = false;

  EnumDeclNode(std::string_view n, const Type *u, int l, int c, int len)
      : DeclNode(NodeKind::EnumDecl, l, c, len), name(n), underlyingType(u) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::EnumDecl;
  }
};

struct NumberNode : public ExprNode {
  std::string_view raw;
  bool isFloat;
  NumberNode(std::string_view r, bool f, int l, int c, int len)
      : ExprNode(NodeKind::Number, l, c, len), raw(r), isFloat(f) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Number;
  }
};

struct BoolNode : public ExprNode {
  bool value;
  BoolNode(bool v, int l, int c, int len)
      : ExprNode(NodeKind::Boolean, l, c, len), value(v) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Boolean;
  }
};

struct CharNode : public ExprNode {
  uint8_t value;
  CharNode(uint8_t v, int l, int c, int len)
      : ExprNode(NodeKind::Char, l, c, len), value(v) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Char;
  }
};

struct RuneNode : public ExprNode {
  uint32_t value;
  RuneNode(uint32_t v, int l, int c, int len)
      : ExprNode(NodeKind::Rune, l, c, len), value(v) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Rune;
  }
};

struct StringNode : public ExprNode {
  std::string_view value;
  StringNode(std::string_view v, int l, int c, int len)
      : ExprNode(NodeKind::String, l, c, len), value(v) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::String;
  }
};

struct NullNode : public ExprNode {
  NullNode(int l, int c, int len) : ExprNode(NodeKind::Null, l, c, len) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Null;
  }
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

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Variable;
  }
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

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::TypedefDecl;
  }
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

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::UnaryOp;
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

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::BinaryOp;
  }
};

struct TernaryOpNode : public ExprNode {
  ExprNode *condition;
  ExprNode *trueExpr;
  ExprNode *falseExpr;
  mutable const Type *promotedType = nullptr;

  TernaryOpNode(ExprNode *cond, ExprNode *tExpr, ExprNode *fExpr, int l, int c,
                int len)
      : ExprNode(NodeKind::TernaryOp, l, c, len), condition(cond),
        trueExpr(tExpr), falseExpr(fExpr) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::TernaryOp;
  }
};

struct VarDeclNode : public DeclNode {
  const Type *type;
  std::string_view varName;
  std::string mangledName;
  ExprNode *initializer;
  bool isGlobal = false;
  bool isStatic = false;
  bool isWeak = false;
  bool isExtern = false;
  std::string_view externAlias;
  std::string_view rawTypeStr;

  /* Reference to the resolved copy constructor for aggregate initialization */
  mutable const FunctionDeclNode *copyCtor = nullptr;
  mutable bool isInitialized = false;

  VarDeclNode(const Type *t, std::string_view n, ExprNode *init, int l, int c,
              int len)
      : DeclNode(NodeKind::VarDecl, l, c, len), type(t), varName(n),
        initializer(init), isInitialized(init != nullptr) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::VarDecl;
  }
};

struct AssignNode : public ExprNode {
  std::string_view op;
  ExprNode *target;
  ExprNode *value;
  mutable const FunctionDeclNode *overloadedOperator = nullptr;

  AssignNode(std::string_view o, ExprNode *t, ExprNode *v, int l, int c,
             int len)
      : ExprNode(NodeKind::Assign, l, c, len), op(o), target(t), value(v) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Assign;
  }
};

struct BlockNode : public StmtNode {
  llvm::ArrayRef<ASTNode *> statements;
  bool isExpressionBody = false;
  bool hasBraces = true;

  BlockNode(int l, int c) : StmtNode(NodeKind::Block, l, c, 1) {}
  void finalize(int endCol) { this->length = endCol - this->column; }

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Block;
  }
};

struct ReturnNode : public StmtNode {
  ExprNode *value;
  ReturnNode(ExprNode *v, int l, int c, int len)
      : StmtNode(NodeKind::Return, l, c, len), value(v) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Return;
  }
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

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::ParamDecl;
  }
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
  bool isWeak = false;
  bool isIntrinsic = false;
  bool isVirtual = false;
  bool isOverride = false;
  std::string_view intrinsicName;
  mutable std::string_view externAlias;
  std::string_view callingConv = "cdecl";
  const RecordType *parentRecord = nullptr;
  std::string_view rawReturnTypeStr;

  /* Intrinsic function attributes inferred during semantic analysis */
  mutable bool isReadNone = false;
  mutable bool isReadOnly = false;
  mutable bool isNoFree = false;
  mutable bool isNoSync = false;
  mutable bool isWillReturn = false;
  mutable bool isMustProgress = false;

  /* Virtual method resolution offset */
  uint32_t vtableIndex = 0;

  /* Indicates whether the node was parsed with a trailing comma to force
   * formatting splits. */
  bool hasTrailingComma = false;

  FunctionDeclNode(const Type *ret, std::string_view n, int l, int c,
                   bool isC = false, bool isMeth = false, bool isExt = false,
                   bool isVar = false, bool isImpl = false)
      : DeclNode(NodeKind::FunctionDecl, l, c), returnType(ret), name(n),
        body(nullptr), isConst(isC), isMethod(isMeth), isExtern(isExt),
        isVariadic(isVar), isImplicit(isImpl) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::FunctionDecl;
  }
};

struct FunctionCallNode : public ExprNode {
  ExprNode *target;
  llvm::ArrayRef<ExprNode *> args;
  llvm::ArrayRef<std::string_view> argNames;
  const FunctionDeclNode *resolvedFunc = nullptr;
  llvm::ArrayRef<ExprNode *> rawArgs;
  llvm::ArrayRef<std::string_view> rawArgNames;
  bool hasRawArgs = false;

  /* Indicates whether the node was parsed with a trailing comma to force
   * formatting splits. */
  bool hasTrailingComma = false;

  FunctionCallNode(ExprNode *t, llvm::ArrayRef<ExprNode *> a,
                   llvm::ArrayRef<std::string_view> n, int l, int c, int len)
      : ExprNode(NodeKind::FunctionCall, l, c, len), target(t), args(a),
        argNames(n) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::FunctionCall;
  }
};

struct CastNode : public ExprNode {
  ExprNode *expr;
  const Type *targetType;
  std::string_view rawTargetTypeStr;

  const FunctionDeclNode *conversionConstructor = nullptr;

  CastNode(ExprNode *e, const Type *target, int l, int c, int len)
      : ExprNode(NodeKind::Cast, l, c, len), expr(e), targetType(target) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Cast;
  }
};

struct ForNode : public StmtNode {
  ASTNode *initStatement;
  ExprNode *condition;
  ExprNode *increment;
  BlockNode *body;

  ForNode(ASTNode *init, ExprNode *cond, ExprNode *inc, BlockNode *b, int l,
          int c, int len)
      : StmtNode(NodeKind::For, l, c, len), initStatement(init),
        condition(cond), increment(inc), body(b) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::For;
  }
};

struct WhileNode : public StmtNode {
  ExprNode *condition;
  BlockNode *body;

  WhileNode(ExprNode *cond, BlockNode *b, int l, int c, int len)
      : StmtNode(NodeKind::While, l, c, len), condition(cond), body(b) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::While;
  }
};

struct CaseNode : public ASTNode {
  ExprNode *value;
  llvm::ArrayRef<ASTNode *> statements;

  CaseNode(ExprNode *v, llvm::ArrayRef<ASTNode *> stmts, int l, int c, int len)
      : ASTNode(NodeKind::Case, l, c, len), value(v), statements(stmts) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Case;
  }
};

struct SwitchNode : public StmtNode {
  ExprNode *condition;
  llvm::ArrayRef<CaseNode *> cases;
  bool hasDefault;

  SwitchNode(ExprNode *cond, llvm::ArrayRef<CaseNode *> c, bool hd, int l,
             int col, int len)
      : StmtNode(NodeKind::Switch, l, col, len), condition(cond), cases(c),
        hasDefault(hd) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Switch;
  }
};

struct BreakNode : public StmtNode {
  BreakNode(int l, int c, int len) : StmtNode(NodeKind::Break, l, c, len) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Break;
  }
};

struct ContinueNode : public StmtNode {
  ContinueNode(int l, int c, int len)
      : StmtNode(NodeKind::Continue, l, c, len) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Continue;
  }
};

struct IfNode : public StmtNode {
  ExprNode *condition;
  BlockNode *thenBlock;
  ASTNode *elseBlock;

  IfNode(ExprNode *cond, BlockNode *tb, ASTNode *eb, int l, int c, int len)
      : StmtNode(NodeKind::If, l, c, len), condition(cond), thenBlock(tb),
        elseBlock(eb) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::If;
  }
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

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Module;
  }
};

struct AnnotationDeclNode : public DeclNode {
  std::string_view name;
  llvm::ArrayRef<VarDeclNode *> fields;
  FunctionDeclNode *constructor;

  mutable const RecordType *recordType = nullptr;

  AnnotationDeclNode(std::string_view n, int l, int c, int len)
      : DeclNode(NodeKind::AnnotationDecl, l, c, len), name(n),
        constructor(nullptr) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::AnnotationDecl;
  }
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

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::UnionDecl;
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

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::StructDecl;
  }
};

struct ClassDeclNode : public DeclNode {
  std::string_view name;
  llvm::ArrayRef<VarDeclNode *> fields;
  llvm::ArrayRef<FunctionDeclNode *> methods;
  llvm::ArrayRef<FunctionDeclNode *> constructors;
  FunctionDeclNode *destructor;

  const Type *baseClass = nullptr;
  llvm::ArrayRef<const Type *> interfaces;

  mutable const RecordType *recordType = nullptr;
  bool isOpaque = false;

  ClassDeclNode(std::string_view n, int l, int c, int len)
      : DeclNode(NodeKind::ClassDecl, l, c, len), name(n), destructor(nullptr) {
  }

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::ClassDecl;
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
  const DeclNode *resolvedDecl = nullptr;

  /* Storage for explicit template arguments applied to method access */
  llvm::ArrayRef<const Type *> templateArgs;

  MemberAccessNode(ExprNode *obj, std::string_view mem, int l, int c, int len)
      : ExprNode(NodeKind::MemberAccess, l, c, len), object(obj),
        memberName(mem) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::MemberAccess;
  }
};

struct ArraySubscriptNode : public ExprNode {
  ExprNode *base;
  ExprNode *index;
  mutable const FunctionDeclNode *overloadedOperator = nullptr;

  ArraySubscriptNode(ExprNode *b, ExprNode *i, int l, int c, int len)
      : ExprNode(NodeKind::ArraySubscript, l, c, len), base(b), index(i) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::ArraySubscript;
  }
};

struct ArrayLiteralNode : public ExprNode {
  llvm::ArrayRef<ExprNode *> elements;

  /* Indicates whether the node was parsed with a trailing comma to force
   * formatting splits. */
  bool hasTrailingComma = false;

  ArrayLiteralNode(llvm::ArrayRef<ExprNode *> elems, int l, int c, int len)
      : ExprNode(NodeKind::ArrayLiteral, l, c, len), elements(elems) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::ArrayLiteral;
  }
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

  /* Indicates whether the node was parsed with a trailing comma to force
   * formatting splits. */
  bool hasTrailingComma = false;

  NewExprNode(const Type *allocTy, ExprNode *arrSize,
              llvm::ArrayRef<ExprNode *> a, llvm::ArrayRef<std::string_view> n,
              bool hasParens, int l, int c, int len)
      : ExprNode(NodeKind::New, l, c, len), allocatedType(allocTy),
        arraySize(arrSize), args(a), argNames(n), hasParens(hasParens) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::New;
  }
};

struct DeleteExprNode : public ExprNode {
  ExprNode *ptr;
  bool isArray;

  DeleteExprNode(ExprNode *p, bool isArr, int l, int c, int len)
      : ExprNode(NodeKind::Delete, l, c, len), ptr(p), isArray(isArr) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Delete;
  }
};

struct ImplicitCastNode : public ExprNode {
  ExprNode *expr;
  const Type *targetType;
  const FunctionDeclNode *conversionConstructor = nullptr;

  ImplicitCastNode(ExprNode *e, const Type *target,
                   const FunctionDeclNode *ctor, int l, int c, int len)
      : ExprNode(NodeKind::ImplicitCast, l, c, len), expr(e),
        targetType(target), conversionConstructor(ctor) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::ImplicitCast;
  }
};

struct NamespaceDeclNode : public DeclNode {
  std::string_view name;
  llvm::ArrayRef<ASTNode *> statements;
  bool isFileScoped = false;

  NamespaceDeclNode(std::string_view n, int l, int c, int len)
      : DeclNode(NodeKind::NamespaceDecl, l, c, len), name(n) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::NamespaceDecl;
  }
};

struct UsingNode : public StmtNode {
  std::string_view name;

  UsingNode(std::string_view n, int l, int c, int len)
      : StmtNode(NodeKind::Using, l, c, len), name(n) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Using;
  }
};
} // namespace utopia