#pragma once
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Format/Piece.hpp"
#include <memory>
#include <vector>

namespace utopia {

/* Tracks the contents of a nested tree of argument lists and collection
 * literals to determine which should be eagerly split. Defined in
 * PieceFactory.cpp. */
struct ExpressionContents;

class PieceFactory : public ASTVisitor<PieceFactory, Piece *> {
public:
  std::vector<std::unique_ptr<Piece>> arena;
  int pageWidth;

  template <typename T, typename... Args> T *create(Args &&...args) {
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    T *raw = ptr.get();
    arena.push_back(std::move(ptr));
    return raw;
  }

  explicit PieceFactory(int pageWidth = 80);

  ~PieceFactory();

  Piece *dispatchExpr(const ExprNode *node);

  Piece *dispatchStmt(const ASTNode *node);

  /* Builds a statement piece, appending `;` to expression statements. */
  Piece *statementPiece(const ASTNode *node);

  /* Whether [node] is an expression that may be block formatted when it
   * appears as a list element. */
  bool canBlockFormat(const ExprNode *node) const;

  /* Given a list of arguments, determines which (if any) should be given block
   * formatting. Returns its index or -1. */
  int candidateBlockArgument(const llvm::ArrayRef<ExprNode *> &args,
                             const llvm::ArrayRef<std::string_view> &names);

  /* Builds a ListPiece for a bracket-delimited set of arguments or elements.
   * If [allowBlockArgument] is true, one element may receive block formatting.
   */
  Piece *buildList(const llvm::ArrayRef<ExprNode *> &args,
                   const llvm::ArrayRef<std::string_view> &names,
                   const char *leftBracket, const char *rightBracket,
                   ListStyle style = ListStyle{}, bool allowBlockArgument = false,
                   bool blockShaped = true);

  /* Builds a ChainPiece for a series of method calls and property accesses, or
   * nullptr if there is no chain. */
  Piece *buildChain(const ExprNode *node);

  /* The number of spaces of leading indentation on the first line. */
  int getPageWidth() const { return pageWidth; }

  /* Depth of named-argument expression contexts. Set when visiting the value
   * expression of a named argument. */
  int namedArgDepth = 0;

  std::unique_ptr<ExpressionContents> contents;

  Piece *visit(const NamespaceDeclNode *node);
  Piece *visit(const UsingNode *node);
  Piece *visit(const TryStmtNode *node);
  Piece *visit(const ThrowStmtNode *node);
  Piece *visit(const AssertStmtNode *node);
  Piece *visit(const NumberNode *node);
  Piece *visit(const BoolNode *node);
  Piece *visit(const CharNode *node);
  Piece *visit(const RuneNode *node);
  Piece *visit(const StringNode *node);
  Piece *visit(const VariableNode *node);
  Piece *visit(const UnaryOpNode *node);
  Piece *visit(const AwaitExprNode *node);
  Piece *visit(const BinaryOpNode *node);
  Piece *visit(const TernaryOpNode *node);
  Piece *visit(const LambdaNode *node);
  Piece *visit(const ModuleNode *node);
  Piece *visit(const AnnotationNode *node);
  Piece *visit(const AnnotationDeclNode *node);
  Piece *visit(const TypedefDeclNode *node);
  Piece *visit(const VarDeclNode *node);
  Piece *visit(const EnumDeclNode *node);
  Piece *visit(const EnumMemberNode *node);
  Piece *visit(const AssignNode *node);
  Piece *visit(const BlockNode *node);
  Piece *visit(const IfNode *node);
  Piece *visit(const ForNode *node);
  Piece *visit(const ForInNode *node);
  Piece *visit(const WhileNode *node);
  Piece *visit(const SwitchNode *node);
  Piece *visit(const CaseNode *node);
  Piece *visit(const BreakNode *node);
  Piece *visit(const ContinueNode *node);
  Piece *visit(const FunctionDeclNode *node);
  Piece *visit(const FunctionCallNode *node);
  Piece *visit(const ReturnNode *node);
  Piece *visit(const CastNode *node);
  Piece *visit(const IsExprNode *node);
  Piece *visit(const ParamDeclNode *node);
  Piece *visit(const UnionDeclNode *node);
  Piece *visit(const StructDeclNode *node);
  Piece *visit(const ClassDeclNode *node);
  Piece *visit(const MemberAccessNode *node);
  Piece *visit(const ArraySubscriptNode *node);
  Piece *visit(const ArrayLiteralNode *node);
  Piece *visit(const MapLiteralNode *node);
  Piece *visit(const NewExprNode *node);
  Piece *visit(const DeleteExprNode *node);
  Piece *visit(const ConstExprNode *node);
  Piece *visit(const DestructorCallNode *node);
  Piece *visit(const TypeLiteralNode *node);
  Piece *visit(const NullNode *node);
  Piece *visit(const ImplicitCastNode *node);
};

} // namespace utopia
