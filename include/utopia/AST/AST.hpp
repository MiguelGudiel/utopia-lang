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
  ForIn,
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
  MapLiteral,
  New,
  Delete,
  ConstExpr,
  DestructorCall,
  Null,
  EnumDecl,
  EnumMember,
  ImplicitCast,
  TypeLiteral,
  TernaryOp,
  Lambda,
  Await,
  NamespaceDecl,
  Using,
  Is,
  Try,
  Catch,
  Throw,
  Assert
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
    case NodeKind::ForIn:
    case NodeKind::While:
    case NodeKind::Switch:
    case NodeKind::Break:
    case NodeKind::Continue:
    case NodeKind::Return:
    case NodeKind::Using:
    case NodeKind::Try:
    case NodeKind::Throw:
    case NodeKind::Assert:
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

  /* Dart-style const semantics (set by Sema):
   *   isConstExpr    - the expression was validated as a compile-time
   *                    constant expression (may be implicit: inside a const
   *                    context, constructor calls are const).
   *   constKey       - canonicalization key for const objects/values:
   *                    identical keys mean identical instances (same
   *                    address). Empty for plain scalar constants that do
   *                    not produce canonical storage. */
  mutable bool isConstExpr = false;
  mutable std::string constKey;

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
    case NodeKind::ConstExpr:
    case NodeKind::Null:
    case NodeKind::ImplicitCast:
    case NodeKind::TypeLiteral:
    case NodeKind::Lambda:
    case NodeKind::Await:
    case NodeKind::Is:
      return true;
    default:
      return false;
    }
  }
};

struct ParamDeclNode;
struct BlockNode;
struct FunctionDeclNode;
struct FunctionCallNode;
struct NewExprNode;

struct LambdaNode : public ExprNode {
  /* Parameter list. Parameters may carry a null 'type' when the type is
   * inferred from the expected function signature (Dart-style lambdas). */
  llvm::ArrayRef<ParamDeclNode *> params;

  /* Body: either an expression (Dart '=> expr') or a statement block. */
  ExprNode *exprBody = nullptr;
  BlockNode *body = nullptr;
  bool isExpressionBody = false;

  /* Optional explicit return type: 'int (int a) => a + 1'. */
  const Type *explicitReturnType = nullptr;

  /* Resolved during semantic analysis. */
  const FunctionDeclNode *synthesizedFunc = nullptr;
  std::string_view mangledName;
  bool unresolved = false;

  LambdaNode(int l, int c, int len)
      : ExprNode(NodeKind::Lambda, l, c, len) {}

  /* 'async' lambdas compile to a coroutine returning a Future. */
  bool isAsync = false;

  /* A captured enclosing local or parameter, copied by value into the
   * closure's environment at creation (the closure value is a pointer to
   * the environment; its first slot holds the synthesized function). */
  struct Capture {
    std::string_view name;
    const DeclNode *decl;
    const Type *type;
  };

  /* Enclosing locals/params referenced by the body, populated during
   * semantic analysis. A non-empty list makes this a closure: the lambda
   * value becomes the environment pointer and calls route through
   * env->fn(env, ...). */
  llvm::ArrayRef<Capture> captures;

  bool hasCaptures() const { return !captures.empty(); }

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Lambda;
  }
};

struct AwaitExprNode : public ExprNode {
  ExprNode *expr;

  /* Set by Sema when the await's result is consumed as the Future itself
   * (e.g. 'Future<int> a = await getA();'): the operand is passed through
   * without unwrapping its value. */
  mutable bool keepFuture = false;

  AwaitExprNode(ExprNode *e, int l, int c, int len)
      : ExprNode(NodeKind::Await, l, c, len), expr(e) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Await;
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

/* Constraint bases for template parameters, mirroring Dart's
 * 'T extends X' (see TemplateConstraint). */
enum class TemplateConstraintKind : uint8_t {
  None,
  /* Pseudo-types (compile-time only): */
  Object,          /* any type */
  Record,          /* any struct/class/union */
  Number,          /* any integer or floating-point type */
  Integer,         /* any integer type */
  FloatingPoint,   /* float32/float64 */
  /* A real class or interface: the argument must extend/implement it. */
  Class
};

/* One constraint per template parameter ('T extends X' with X a class or
 * one of the pseudo-types above). None means unconstrained. */
struct TemplateConstraint {
  TemplateConstraintKind kind = TemplateConstraintKind::None;
  const Type *classType = nullptr;

  TemplateConstraint() = default;
  explicit TemplateConstraint(TemplateConstraintKind k) : kind(k) {}
  TemplateConstraint(const Type *cls)
      : kind(TemplateConstraintKind::Class), classType(cls) {}
};

struct DeclNode : public ASTNode {
  llvm::ArrayRef<AnnotationNode *> annotations;
  bool hasPublicMod = false;
  bool hasPrivateMod = false;
  bool hasProtectedMod = false;
  std::string_view declFilePath;
  std::string_view fqName; // Fully qualified name (e.g. 'NS.Class')

  /* Exact token location for LSP tooling */
  int identifierColumn = 0;
  int identifierLength = 0;

  bool isTemplate = false;
  llvm::ArrayRef<std::string_view> templateParams;
  /* Constraints for 'T extends X', parallel to templateParams. */
  llvm::ArrayRef<TemplateConstraint> templateConstraints;

  /* Template specialization ('class List<int>', 'class Pair<T, int>').
   * specializationArgs holds the pattern: concrete types for complete
   * specializations, a mix of concrete types and TemplateParamType for
   * partial ones. */
  bool isTemplateSpecialization = false;
  llvm::ArrayRef<const Type *> specializationArgs;
  /* The primary template's fully qualified name ('NS.List'). */
  std::string_view specializationBaseName;
  /* Raw source of the '<...>' template list, for the formatter. */
  std::string_view rawTemplateListStr;

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

  bool isProtected(std::string_view declName) const {
    if (hasPrivateMod || hasPublicMod)
      return false;
    if (hasProtectedMod)
      return true;
    return false;
  }

  bool isPrivate(std::string_view declName) const {
    if (hasPublicMod || hasProtectedMod)
      return false;
    if (hasPrivateMod)
      return true;

    size_t pos = declName.find_last_of('.');
    if (pos != std::string_view::npos) {
      declName = declName.substr(pos + 1);
    }
    return declName.starts_with("_");
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
  bool isFinal = false;
  std::string_view externAlias;
  std::string_view rawTypeStr;

  /* Dart-style const: set by Sema when the declaration initializer was
   * validated as a constant expression; constKey holds its serialized
   * value (see ConstExprNode). */
  mutable bool isConstExpr = false;
  mutable std::string constKey;

  /* Reference to the resolved copy constructor for aggregate initialization */
  mutable const FunctionDeclNode *copyCtor = nullptr;
  mutable bool isInitialized = false;

  /* Closure support: true when a capturing lambda was assigned to this
   * variable, so its value is a closure environment pointer instead of a
   * plain function address. Calls through such a variable route through
   * env->fn(env, ...), and assigning a plain function to it (or passing it
   * to a plain function-pointer parameter) is rejected by Sema. */
  mutable bool mayHoldClosure = false;

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

  /* Set by the parser when the node is a constructor initializer-list entry
   * (': this.field = expr'): such assignments are the only context allowed
   * to write to 'final' fields. */
  bool isFieldInit = false;

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

  /* Set by Sema when an async function returns a Future<T> of its value
   * type ('return fut;'): the codegen awaits the future before completing
   * the enclosing coroutine. */
  mutable bool implicitAwait = false;

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

  /* Dart-style initializing formal: 'Class(this.field)' declares a
   * parameter whose value is stored into 'field' by the constructor. The
   * type stays null until Sema resolves it from the record's fields. */
  bool isThisParam = false;

  ParamDeclNode(const Type *t, std::string_view n, ExprNode *defVal, bool isN,
                bool isReq, int l, int c, int len)
      : DeclNode(NodeKind::ParamDecl, l, c, len), type(t), name(n),
        defaultValue(defVal), isNamed(isN), isRequired(isReq) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::ParamDecl;
  }
};

struct FunctionCallNode;

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
  bool isAbstract = false;
  /* 'async' functions compile to coroutines that return a Future<T> of
   * their declared return type. */
  bool isAsync = false;
  /* Set by Sema when the body (not nested lambdas) contains an await. */
  mutable bool hasAwait = false;
  /* For async functions: Future<returnType>, resolved during Sema. */
  mutable const Type *effectiveReturnType = nullptr;
  std::string_view intrinsicName;
  mutable std::string_view externAlias;
  std::string_view callingConv = "cdecl";
  const RecordType *parentRecord = nullptr;
  std::string_view rawReturnTypeStr;

  FunctionCallNode *superCall = nullptr;

  /* Constructor initializer-list entries (': this.field = expr'), stored
   * as assignments. They run after the declaration field initializers,
   * before the constructor body, matching Dart's execution order. */
  llvm::ArrayRef<AssignNode *> fieldInitializers;

  /* Dart-style named constructor: 'Foo.named(...)'. 'name' holds the
   * simple constructor name ("named"); the class name comes from
   * parentRecord. */
  bool isNamedCtor = false;

  mutable bool isReadNone = false;
  mutable bool isReadOnly = false;
  mutable bool isNoFree = false;
  mutable bool isNoSync = false;
  mutable bool isWillReturn = false;
  mutable bool isMustProgress = false;

  /* Closure support: when true, this synthesized function is the body of a
   * capturing lambda. Its first parameter is the closure environment
   * (a pointer to {refcount, fn, dtor, captures...}) and 'captureParams'
   * holds one hidden variable per captured name, bound to the matching
   * environment slot while the body is dispatched. */
  bool hasCaptureEnv = false;
  llvm::ArrayRef<VarDeclNode *> captureParams;
  mutable bool hasEH = false;
  mutable bool mayUnwind = false;

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
  bool isSuperCall = false;

  /* Memory.construct<T>(ptr, args...): the type checker lowers the call
   * into a placement NewExprNode that the codegen visits instead. */
  NewExprNode *loweredNew = nullptr;

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

/* Dart/C#-style type test: 'expr is Type' and 'expr is! Type'. Returns
 * whether the dynamic type of the expression is 'targetType' or a subtype
 * of it. When the static type cannot decide the answer, Sema marks the node
 * for a runtime vtable-based check ('needsRuntimeCheck') and resolves the
 * promoted target type for 'if (x is T)' type promotion. */
struct IsExprNode : public ExprNode {
  ExprNode *expr;
  const Type *targetType;
  bool isNegated = false;
  std::string_view rawTargetTypeStr;

  /* Set by Sema when the result is known at compile time (0 = false,
   * 1 = true, -1 = runtime check required). Already accounts for
   * 'isNegated'. */
  mutable int staticResult = -1;

  /* The operand's resolved class type (behind pointer/reference), used by
   * codegen to reach the object's vtable. */
  const ClassType *operandClassType = nullptr;

  /* The type a variable referenced by this test is promoted to inside an
   * 'if' block, preserving the operand's pointer/reference indirection
   * ('x is T' with 'x: Base*' promotes x to 'T*'). Null when the operand is
   * not a simple variable. */
  mutable const Type *promotionType = nullptr;

  IsExprNode(ExprNode *e, const Type *target, int l, int c, int len)
      : ExprNode(NodeKind::Is, l, c, len), expr(e), targetType(target) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Is;
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

/* Dart-style 'for (var x in iterable)' loop.
 *
 * The header variable (loopVar) mirrors a normal declaration:
 *   'var x'    -> type AutoTy, copies each element into 'x' per iteration
 *   'final x'  -> type AutoTy, non-rebindable copy
 *   'var& x'   -> type AutoTy + isRefBinding: binds a reference to the
 *                 element (no copy); fixed up by Sema to Reference(T)
 *   'final& x' -> type AutoTy + isRefBinding + isFinal: const reference
 *   'T x'      -> explicit element type (copies/checks against T)
 *   'T& x'     -> explicit reference binding
 *
 * Iteration is a structural protocol with zero indirection, not an Iterable
 * hierarchy: '<expr>.iterator()' must return a value whose type provides
 * 'bool moveNext()' and an element accessor 'current()' (T&). List and Map
 * are plain records; nothing inherits an Iterable and no vtables exist.
 *
 * Sema lowers the loop to plain code:
 *   T& __range = <expr>;              // no copy; rvalues live in a temp
 *   var __it = __range.iterator();
 *   while (__it.moveNext()) {
 *     <loopVar> = __it.current();
 *     <body>
 *   }
 * The lowered tree is stored in 'desugared' and CodeGen executes it as-is
 * (the optimizer inlines iterator()/moveNext()/current() into a plain
 * index loop).
 */
struct ForInNode : public StmtNode {
  VarDeclNode *loopVar;
  ExprNode *iterable;
  BlockNode *body;
  bool isRefBinding = false;
  mutable ASTNode *desugared = nullptr;

  ForInNode(VarDeclNode *var, ExprNode *iter, BlockNode *b, int l, int c,
            int len)
      : StmtNode(NodeKind::ForIn, l, c, len), loopVar(var), iterable(iter),
        body(b) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::ForIn;
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

  /* Source spans of the import/export directives, parallel to rawImports and
   * rawExports: from the 'import'/'export' keyword to the terminating
   * semicolon. Used by LSP tooling and warning reporting. */
  struct DirectiveInfo {
    std::string_view path;
    int line;
    int column;
    int endLine;
    int endColumn;
    /* The module loaded for this directive (null when resolution failed).
     * Filled by the parser from the module loader. */
    const ModuleNode *resolvedModule = nullptr;
  };
  llvm::ArrayRef<DirectiveInfo> importInfo;
  llvm::ArrayRef<DirectiveInfo> exportInfo;

  /* One top-level source item of a module, in file order. The formatter
   * switches to this order when preprocessor directives are present, since
   * reordering (import hoisting, member sorting) would move code across
   * conditional branches. */
  struct TopLevelItem {
    enum class Kind : uint8_t { Import, Export, Directive, Statement };
    Kind kind;
    /* Import/export path or the raw directive line ('#if X'), without the
     * leading '#'. */
    std::string_view text;
    ASTNode *node = nullptr; /* Statement items */
    /* Leading comments of an import/export directive. Kept so the ordered
     * formatter path can print them right before the directive instead of
     * hoisting them to the top of the module. */
    std::string_view doc;
  };
  llvm::ArrayRef<TopLevelItem> topLevelItems;

  llvm::ArrayRef<ASTNode *> statements;
  std::vector<ASTNode *> instantiatedTemplates;

  explicit ModuleNode(std::string_view path)
      : ASTNode(NodeKind::Module), filePath(path) {}

  bool canSee(std::string_view targetFilePath) const;
  bool canSeeHelper(
      std::string_view targetFilePath,
      std::unordered_set<const ModuleNode *> &visited) const;
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

  /* Raw source text of the base class and interfaces (parallel to
   * 'interfaces'): the resolved types carry fully qualified names, which
   * must not leak into reformatted output. */
  std::string_view rawBaseClassStr;
  llvm::ArrayRef<std::string_view> rawInterfaces;

  mutable const RecordType *recordType = nullptr;
  bool isOpaque = false;
  bool isAbstract = false;

  /* Dart 'final class': cannot be extended or implemented by any other
   * class. */
  bool isFinal = false;

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

  bool isSuperAccess = false;

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

struct MapLiteralNode : public ExprNode {
  llvm::ArrayRef<ExprNode *> keys;
  llvm::ArrayRef<ExprNode *> values;

  /* Indicates whether the node was parsed with a trailing comma to force
   * formatting splits. */
  bool hasTrailingComma = false;

  MapLiteralNode(llvm::ArrayRef<ExprNode *> ks, llvm::ArrayRef<ExprNode *> vs,
                 int l, int c, int len)
      : ExprNode(NodeKind::MapLiteral, l, c, len), keys(ks), values(vs) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::MapLiteral;
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

  /* C++-style placement new: 'new (ptr) T(...)' constructs in existing
   * memory instead of allocating. Only produced internally as the lowering
   * target of Memory.construct<T>(ptr, args...). */
  ExprNode *placementExpr = nullptr;

  /* Set when constructing a record that has no user-defined constructors
   * (trivially copyable) with a single value argument: the destination is
   * bitwise-copied from the argument instead of going through a ctor. */
  bool implicitCopyInit = false;

  /* Dart-style named constructor: 'new Foo.named(...)'. */
  std::string_view namedCtorName;

  /* Resolved custom allocator / deallocator ('operator new' /
   * 'operator delete' on the class or at file scope); null means the
   * default malloc/free. */
  const FunctionDeclNode *allocator = nullptr;
  const FunctionDeclNode *deallocator = nullptr;

  NewExprNode(const Type *allocTy, ExprNode *arrSize,
              llvm::ArrayRef<ExprNode *> a, llvm::ArrayRef<std::string_view> n,
              bool hasParens, int l, int c, int len)
      : ExprNode(NodeKind::New, l, c, len), allocatedType(allocTy),
        arraySize(arrSize), args(a), argNames(n), hasParens(hasParens) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::New;
  }
};

/* Dart-style const expression: 'const Foo(1, 2)' or 'const <expr>'.
 * Sema validates the inner expression as a compile-time constant and, for
 * const object creations, computes 'constKey' so identical constructions
 * canonicalize to the same static instance. */
struct ConstExprNode : public ExprNode {
  ExprNode *expr;

  ConstExprNode(ExprNode *e, int l, int c, int len)
      : ExprNode(NodeKind::ConstExpr, l, c, len), expr(e) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::ConstExpr;
  }
};

struct DeleteExprNode : public ExprNode {
  ExprNode *ptr;
  bool isArray;
  const FunctionDeclNode *deallocator = nullptr;

  DeleteExprNode(ExprNode *p, bool isArr, int l, int c, int len)
      : ExprNode(NodeKind::Delete, l, c, len), ptr(p), isArray(isArr) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Delete;
  }
};

/* Manual destructor call: 'obj.~TypeName()'. */
struct DestructorCallNode : public ExprNode {
  ExprNode *object;
  const Type *targetType;
  const FunctionDeclNode *destructor = nullptr;

  DestructorCallNode(ExprNode *obj, const Type *t, int l, int c, int len)
      : ExprNode(NodeKind::DestructorCall, l, c, len), object(obj),
        targetType(t) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::DestructorCall;
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

/* One 'catch' clause of a try statement. Never dispatched on its own: the
 * visitor for TryStmtNode walks the clauses directly. */
struct CatchClauseNode : public ASTNode {
  /* The caught type (null for 'catch (...)'). References and r-value
   * references bind to the thrown object without copying. */
  const Type *catchType;
  std::string_view rawTypeStr;
  /* Optional binding variable name (empty for 'catch (...)' and for
   * unnamed clauses). */
  std::string_view varName;
  BlockNode *body;
  bool isCatchAll = false;

  /* For catch-by-value of a record with a custom destructor: the copy
   * constructor used to copy the thrown value into the local variable. */
  mutable const FunctionDeclNode *copyCtor = nullptr;

  CatchClauseNode(const Type *t, std::string_view vn, BlockNode *b, int l,
                  int c, int len)
      : ASTNode(NodeKind::Catch, l, c, len), catchType(t), varName(vn),
        body(b) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Catch;
  }
};

/* C++-style exception handling: 'try { ... } catch (T e) { ... }'. The
 * clauses are matched in order at runtime; 'catch (...)' catches every
 * type. */
struct TryStmtNode : public StmtNode {
  BlockNode *body;
  llvm::ArrayRef<CatchClauseNode *> clauses;

  TryStmtNode(BlockNode *b, llvm::ArrayRef<CatchClauseNode *> c, int l, int col,
              int len)
      : StmtNode(NodeKind::Try, l, col, len), body(b), clauses(c) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Try;
  }
};

/* 'throw expr;' raises an exception with C++ semantics (any type can be
 * thrown). A bare 'throw;' (value == nullptr) rethrows the exception
 * currently being handled and is only valid inside a catch clause. */
struct ThrowStmtNode : public StmtNode {
  ExprNode *value;

  /* Set by Sema when the statement is a bare 'throw;' inside a catch. */
  mutable bool isRethrow = false;

  /* Copy constructor used to move the thrown value into the exception
   * storage (records with a custom destructor only). */
  mutable const FunctionDeclNode *copyCtor = nullptr;

  ThrowStmtNode(ExprNode *v, int l, int c, int len)
      : StmtNode(NodeKind::Throw, l, c, len), value(v) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Throw;
  }
};

/* 'assert(expr)': evaluates the condition and aborts with the source
 * location when it is false. No-op when NDEBUG is defined. */
struct AssertStmtNode : public StmtNode {
  ExprNode *condition;

  /* Set by Sema when NDEBUG is defined: the statement compiles to nothing,
   * mirroring C/C++ release builds. */
  mutable bool isNoOp = false;

  AssertStmtNode(ExprNode *cond, int l, int c, int len)
      : StmtNode(NodeKind::Assert, l, c, len), condition(cond) {}

  static bool classof(const ASTNode *node) {
    return node->kind == NodeKind::Assert;
  }
};
} // namespace utopia