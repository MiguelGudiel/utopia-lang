#pragma once
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Common/Types.hpp"
#include "utopia/Sema/SemaContext.hpp"
#include <memory>
#include <unordered_set>
#include <vector>

namespace utopia {

/*
 * Strips indirection to evaluate core type compatibility, including both
 * LValue and RValue references.
 */
bool canImplicitlyCast(const Type *from, const Type *to,
                       bool allowUserDefined = true);

/*
 * Picks the most specific single-parameter conversion constructor of a
 * record type for converting `from`, or returns nullptr when none is
 * compatible. The best match is the ctor whose parameter type is closest to
 * the source type (exact match first, then matching signedness and smallest
 * width delta); a naive first-match selection always picks the narrowest
 * ctor (e.g. String(int8)) and truncates wider values.
 */
const FunctionDeclNode *findBestConversionCtor(const Type *from,
                                               const RecordType *recTy);

/*
 * True when the two types are identical after stripping references and
 * qualifiers. Overload scoring uses this instead of a plain
 * canImplicitlyCast(..., false): that check still accepts narrowing
 * integer conversions (int32 -> uint8), which would let a char-pushing
 * 'operator+(uint8)' outrank the String conversion constructor in
 * 'string + int'.
 */
bool typesMatchExactly(const Type *a, const Type *b);

class SemaPass {
public:
  virtual ~SemaPass() = default;
  virtual bool run(const ModuleNode *module, SemaContext &ctx) = 0;
  virtual const char *getName() const = 0;
};

class DeclCollectorPass : public SemaPass,
                          public ASTVisitor<DeclCollectorPass, void> {
  std::unordered_set<const ModuleNode *> visitedModules;

public:
  SemaContext *ctx = nullptr;
  bool run(const ModuleNode *module, SemaContext &context) override;
  const char *getName() const override { return "DeclarationCollector"; }

  void visit(const ModuleNode *node);
  void visit(const FunctionDeclNode *node);
  void visit(const VarDeclNode *node);
  void visit(const UnionDeclNode *node);
  void visit(const StructDeclNode *node);
  void visit(const ClassDeclNode *node);
  void visit(const TypedefDeclNode *node);
  void visit(const AnnotationDeclNode *node);
  void visit(const EnumDeclNode *node);
  void visit(const EnumMemberNode *node) {}
  void visit(const NamespaceDeclNode *node);
  void visit(const UsingNode *node);

  void visit(const AnnotationNode *node) {}
  void visit(const NumberNode *) {}
  void visit(const BoolNode *) {}
  void visit(const CharNode *) {}
  void visit(const RuneNode *) {}
  void visit(const StringNode *) {}
  void visit(const VariableNode *) {}
  void visit(const UnaryOpNode *) {}
  void visit(const BinaryOpNode *) {}
  void visit(const TernaryOpNode *) {}
  void visit(const LambdaNode *) {}
  void visit(const AwaitExprNode *) {}
  void visit(const AssignNode *) {}
  void visit(const BlockNode *) {}
  void visit(const FunctionCallNode *) {}
  void visit(const IfNode *node);
  void visit(const ForNode *node);
  void visit(const ForInNode *node);
  void visit(const WhileNode *node);
  void visit(const SwitchNode *node);
  void visit(const CaseNode *node) {}
  void visit(const TryStmtNode *node);
  void visit(const ThrowStmtNode *node);
  void visit(const AssertStmtNode *node);
  void visit(const BreakNode *node) {}
  void visit(const ContinueNode *node) {}
  void visit(const ReturnNode *) {}
  void visit(const CastNode *) {}
  void visit(const IsExprNode *) {}
  void visit(const ParamDeclNode *) {}
  void visit(const MemberAccessNode *) {}
  void visit(const ArraySubscriptNode *) {}
  void visit(const ArrayLiteralNode *) {}
  void visit(const MapLiteralNode *) {}
  void visit(const NewExprNode *) {}
  void visit(const DeleteExprNode *) {}
  void visit(const ConstExprNode *) {}
  void visit(const DestructorCallNode *) {}
  void visit(const TypeLiteralNode *) {}
  void visit(const NullNode *) {}
  void visit(const ImplicitCastNode *node) {}
};

class TypeCheckPass : public SemaPass,
                      public ASTVisitor<TypeCheckPass, SemaResult> {
  SemaContext *ctx = nullptr;
  std::unordered_set<const ModuleNode *> visitedModules;

private:
  const FunctionDeclNode *
  resolveOverloadedOperator(const Type *lhsType, std::string_view opName,
                            const std::vector<ExprNode *> &args);

  /* All non-template functions visited by the pass; drives the mayUnwind
   * fixed-point analysis in run(). */
  std::vector<const FunctionDeclNode *> allFunctions;

  /* True when the function's own body contains a 'throw'/'try' statement or
   * calls a function that may unwind (the callee's mayUnwind flag must be
   * current, so this is only invoked from the fixed-point loop). */
  bool computeFunctionMayUnwind(const FunctionDeclNode *fn);

  /* Dart-style type promotion: 'if (x is T) { ... }' narrows the type of the
   * resolved declaration to T (with the operand's pointer/reference
   * indirection preserved) while the block is checked. */
  struct Promotion {
    const DeclNode *decl;
    const Type *type;
    bool active;
  };
  std::vector<Promotion> promotions;

  /* Collects the promotions implied by a boolean condition: 'thenP' applies
   * when the condition is true, 'elseP' when it is false. Handles 'is' /
   * 'is!' directly and '&&' / '||' compositions. */
  void collectPromotions(const ExprNode *cond, std::vector<Promotion> &thenP,
                         std::vector<Promotion> &elseP);

public:
  bool run(const ModuleNode *module, SemaContext &context) override;
  const char *getName() const override { return "TypeChecker"; }

  // Check type access visibility to avoid escaping private types across files
  bool checkTypeVisibility(const Type *type, const ASTNode *node);
  const Type *resolveIfTemplate(const Type *t);
  void checkImplicitCastWarning(const Type *from, const Type *to,
                                const ASTNode *node);
  ExprNode *performImplicitConversion(ExprNode *expr, const Type *to);

  /* Semantic context accessor for the template-deduction machinery. */
  SemaContext &getContext() const { return *ctx; }

  /* Analyzes an AST node to emit warnings if a nodiscard value is ignored */
  void checkNodiscard(const ASTNode *node);
  void checkDeprecated(const DeclNode *decl, const ASTNode *node);

  SemaResult visit(const NumberNode *node);
  SemaResult visit(const BoolNode *node);
  SemaResult visit(const CharNode *node);
  SemaResult visit(const RuneNode *node);
  SemaResult visit(const StringNode *node);
  SemaResult visit(const VariableNode *node);
  SemaResult visit(const UnaryOpNode *node);
  SemaResult visit(const BinaryOpNode *node);
  SemaResult visit(const TernaryOpNode *node);
  SemaResult visit(const VarDeclNode *node);
  SemaResult visit(const AssignNode *node);
  SemaResult visit(const BlockNode *node);
  SemaResult visit(const FunctionDeclNode *node);
  SemaResult visit(const FunctionCallNode *node);
  SemaResult visit(const IfNode *node);
  SemaResult visit(const ForNode *node);
  SemaResult visit(const ForInNode *node);
  SemaResult visit(const WhileNode *node);
  SemaResult lowerForInArray(const ForInNode *node, const Type *arrayTy,
                             int line, int col, int len);
  SemaResult visit(const SwitchNode *node);
  SemaResult visit(const CaseNode *node) { return ctx->astCtx.VoidTy; }
  SemaResult visit(const BreakNode *node);
  SemaResult visit(const ContinueNode *node);
  SemaResult visit(const ReturnNode *node);
  SemaResult visit(const CastNode *node);
  SemaResult visit(const IsExprNode *node);
  SemaResult visit(const TryStmtNode *node);
  SemaResult visit(const ThrowStmtNode *node);
  SemaResult visit(const AssertStmtNode *node);
  SemaResult visit(const ParamDeclNode *node);
  SemaResult visit(const ModuleNode *node);
  SemaResult visit(const UnionDeclNode *node);
  SemaResult visit(const StructDeclNode *node);
  SemaResult visit(const ClassDeclNode *node);
  SemaResult visit(const MemberAccessNode *node);
  SemaResult visit(const AnnotationDeclNode *node);
  SemaResult visit(const TypedefDeclNode *node);
  SemaResult visit(const AnnotationNode *node);
  SemaResult visit(const ArraySubscriptNode *node);
  SemaResult visit(const ArrayLiteralNode *node);
  SemaResult visit(const MapLiteralNode *node);
  SemaResult visit(const NewExprNode *node);
  SemaResult visit(const DeleteExprNode *node);
  SemaResult visit(const ConstExprNode *node);
  SemaResult visit(const DestructorCallNode *node);

  /* Dart-style const expressions */

  /* Validates 'expr' as a compile-time constant expression and computes its
   * canonicalization key. 'inConstContext' enables implicit const (a
   * constructor call inside a const expression is treated as const).
   * On success returns true and fills 'out' with the serialized constant
   * value ('i:5', 's:hi', 'o:key', ...). */
  bool constEvaluate(const ExprNode *expr, bool inConstContext,
                     std::string &out);

  /* Serializes the value of a const object's field from its creation call
   * (this-params, initializer-list entries, declaration initializers). */
  bool constObjectFieldValue(const ExprNode *creation,
                             std::string_view fieldName, std::string &out);

  /* Validates a const constructor declaration (Dart rules). */
  void checkConstConstructor(const FunctionDeclNode *ctor);

  /* Records the per-object field values of a canonical const object so
   * 'const b = a.field' can be evaluated. */
  void recordConstObjectFields(const std::string &objectKey,
                               const ExprNode *creation);

  /* Active while evaluating a const constructor call: maps its parameters
   * to the caller's argument expressions, so 'super(param)' and
   * ': this.field = param' inside the const initializer resolve like Dart's
   * const constructor parameters. */
  std::vector<std::pair<const ParamDeclNode *, const ExprNode *>>
      constParamEnv;
  SemaResult visit(const TypeLiteralNode *node);
  SemaResult visit(const NullNode *node);
  SemaResult visit(const EnumDeclNode *node);
  SemaResult visit(const EnumMemberNode *node);
  SemaResult visit(const ImplicitCastNode *node);
  SemaResult visit(const LambdaNode *node);
  SemaResult visit(const AwaitExprNode *node);

  /* Resolves the prelude's 'Future' template instantiated with 'valueType'.
   * Returns the resolved record type, or nullptr if async is unavailable. */
  const Type *getFutureType(const Type *valueType);

  /* Instantiates a method-level template ('static Future<R> value<R>(...)')
   * with the given type arguments (resolved), registers it on the record and
   * returns the instantiated function. */
  const FunctionDeclNode *instantiateMethodTemplate(
      const FunctionDeclNode *tmplDecl, const RecordType *recordTy,
      llvm::ArrayRef<const Type *> explicitArgs);

  /* C++-style template argument deduction ('findMax(100, 250)'): deduces
   * the template arguments of a function/method template from the call
   * arguments, using the same P/A deduction rules as C++. 'explicitArgs'
   * may be a (possibly empty) prefix of the template arguments spelled out
   * at the call site; the remaining parameters are deduced. On success
   * 'outArgs' receives one resolved type per template parameter. */
  bool deduceTemplateArguments(
      const FunctionDeclNode *tmplDecl,
      llvm::ArrayRef<const Type *> argTypes,
      llvm::ArrayRef<std::string_view> argNames,
      llvm::ArrayRef<const Type *> explicitArgs,
      std::vector<const Type *> &outArgs, std::string &outError);

  /* Class-template argument deduction ('new Box(42)' / 'Box(42)'): tries
   * every constructor of the template record as a deduction source. */
  bool deduceClassTemplateArguments(
      const DeclNode *tmplDecl, llvm::ArrayRef<const Type *> argTypes,
      llvm::ArrayRef<std::string_view> argNames,
      std::vector<const Type *> &outArgs, std::string &outError);

  /* Returns true when 't' (possibly behind a pointer/reference) is a
   * Future<T>; 'outValue' receives T when non-null. */
  static bool unwrapFutureType(const Type *t, const Type **outValue);

  /* Re-types unresolved lambda arguments against the matched parameter
   * signatures. Returns an error result if a lambda cannot be resolved. */
  SemaResult resolveLambdaArgs(const FunctionDeclNode *fn,
                               const std::vector<ExprNode *> &resolvedArgs);
  SemaResult resolveLambdaArgs(const FunctionType *fTy,
                               const std::vector<ExprNode *> &resolvedArgs);
  SemaResult visit(const NamespaceDeclNode *node);
  SemaResult visit(const UsingNode *node);

  /* Memory.construct<T>(ptr, args...): lowers the call into a placement
   * NewExprNode so constructor resolution and codegen are shared with
   * 'new'. 'node' keeps the original arguments (the destination pointer is
   * node->args[0]); the lowered node is stored in node->loweredNew.
   * 'templateArgs' carries the explicit type arguments (may be empty, in
   * which case T is deduced from the destination pointer). */
  SemaResult checkMemoryConstruct(const FunctionCallNode *node,
                                  llvm::ArrayRef<const Type *> templateArgs);

  /* Memory.destruct(ptr): verifies the argument is a typed pointer so the
   * pointee's destructor can be resolved at codegen. */
  SemaResult checkMemoryDestruct(const FunctionCallNode *node);

};

class ControlFlowPass : public SemaPass,
                        public ASTVisitor<ControlFlowPass, void> {
  SemaContext *ctx = nullptr;
  bool isReachable = true;
  bool alreadyInUnreachable = false;
  bool isAssignTarget = false;
  std::unordered_map<const VarDeclNode *, bool> initStates;
  std::unordered_set<const ModuleNode *> visitedModules;

public:
  bool run(const ModuleNode *module, SemaContext &context) override;
  const char *getName() const override { return "ControlFlowAnalyzer"; }

  void visit(const ModuleNode *node);
  void visit(const FunctionDeclNode *node);
  void visit(const BlockNode *node);
  void visit(const IfNode *node);
  void visit(const ForNode *node);
  void visit(const ForInNode *node);
  void visit(const WhileNode *node);
  void visit(const SwitchNode *node);
  void visit(const CaseNode *node);
  void visit(const BreakNode *node);
  void visit(const ContinueNode *node);
  void visit(const ReturnNode *node);
  void visit(const VarDeclNode *node);
  void visit(const AssignNode *node);
  void visit(const VariableNode *node);
  void visit(const UnaryOpNode *node);
  void visit(const BinaryOpNode *node);
  void visit(const TernaryOpNode *node);
  void visit(const LambdaNode *) {}
  void visit(const FunctionCallNode *node);
  void visit(const CastNode *node);
  void visit(const IsExprNode *node);
  void visit(const MemberAccessNode *node);
  void visit(const ArraySubscriptNode *node);
  void visit(const ArrayLiteralNode *node);
  void visit(const MapLiteralNode *node);
  void visit(const NewExprNode *node);
  void visit(const DeleteExprNode *node);
  void visit(const ConstExprNode *) {}
  void visit(const DestructorCallNode *node);
  void visit(const TryStmtNode *node);
  void visit(const ThrowStmtNode *node);
  void visit(const AssertStmtNode *node);
  void visit(const NamespaceDeclNode *node);
  void visit(const UsingNode *node);
  void visit(const AwaitExprNode *node);
  void visit(const ImplicitCastNode *node);

  void visit(const NumberNode *) {}
  void visit(const BoolNode *) {}
  void visit(const CharNode *) {}
  void visit(const RuneNode *) {}
  void visit(const StringNode *) {}
  void visit(const NullNode *) {}
  void visit(const TypeLiteralNode *) {}
  void visit(const AnnotationNode *) {}
  void visit(const AnnotationDeclNode *) {}
  void visit(const TypedefDeclNode *) {}
  void visit(const EnumDeclNode *) {}
  void visit(const EnumMemberNode *) {}
  void visit(const ParamDeclNode *) {}
  void visit(const UnionDeclNode *) {}
  void visit(const StructDeclNode *) {}
  void visit(const ClassDeclNode *) {}
};

class SemaPipeline {
  std::vector<std::unique_ptr<SemaPass>> passes;

public:
  SemaPipeline();
  bool run(const ModuleNode *module, SemaContext &ctx);
};

} // namespace utopia