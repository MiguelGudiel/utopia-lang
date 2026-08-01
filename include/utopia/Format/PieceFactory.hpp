#pragma once
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Format/Piece.hpp"
#include <memory>
#include <string>
#include <vector>

namespace utopia {

/* Strips original indentation from multiline block comments to prevent
 the CodeWriter from double-indenting the internal lines. */
inline std::string formatCommentString(const std::string &raw) {
  if (raw.length() < 2 || raw.substr(0, 2) != "/*")
    return raw;

  std::string result;
  size_t start = 0;
  bool firstLine = true;
  size_t commonIndent = std::string::npos;
  bool allLinesStartWithStar = true;

  /* First pass: Calculate the common indentation of all internal lines */
  start = 0;
  while (start < raw.length()) {
    size_t end = raw.find('\n', start);
    if (end == std::string::npos)
      end = raw.length();

    if (!firstLine) {
      size_t indent = 0;
      while (start + indent < end &&
             (raw[start + indent] == ' ' || raw[start + indent] == '\t')) {
        indent++;
      }
      if (start + indent < end) {
        if (commonIndent == std::string::npos || indent < commonIndent) {
          commonIndent = indent;
        }
        if (raw[start + indent] != '*') {
          allLinesStartWithStar = false;
        }
      }
    }
    firstLine = false;
    start = end + 1;
  }

  if (commonIndent == std::string::npos)
    commonIndent = 0;

  /* Preserve the 1-space offset for standard Javadoc-style block comments */
  size_t stripCount = (allLinesStartWithStar && commonIndent > 0)
                          ? commonIndent - 1
                          : commonIndent;

  /* Second pass: Strip the calculated indentation */
  start = 0;
  firstLine = true;
  while (start < raw.length()) {
    size_t end = raw.find('\n', start);
    if (end == std::string::npos)
      end = raw.length();

    if (firstLine) {
      result += raw.substr(start, end - start);
    } else {
      result += "\n";
      size_t spacesToSkip = 0;
      while (spacesToSkip < stripCount && start + spacesToSkip < end &&
             (raw[start + spacesToSkip] == ' ' ||
              raw[start + spacesToSkip] == '\t')) {
        spacesToSkip++;
      }
      result += raw.substr(start + spacesToSkip, end - (start + spacesToSkip));
    }

    firstLine = false;
    start = end + 1;
  }

  return result;
}

class PieceFactory : public ASTVisitor<PieceFactory, Piece *> {
public:
  std::vector<std::unique_ptr<Piece>> arena;

  template <typename T, typename... Args> T *create(Args &&...args) {
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    T *raw = ptr.get();
    arena.push_back(std::move(ptr));
    return raw;
  }

  Piece* extractChain(const ExprNode* node);
  Piece *dispatchExpr(const ExprNode *node);

  Piece *dispatchStmt(const ASTNode *node);

  Piece *visit(const NumberNode *node);
  Piece *visit(const BoolNode *node);
  Piece *visit(const CharNode *node);
  Piece *visit(const RuneNode *node);
  Piece *visit(const StringNode *node);
  Piece *visit(const VariableNode *node);
  Piece *visit(const UnaryOpNode *node);
  Piece *visit(const BinaryOpNode *node);
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
  Piece *visit(const WhileNode *node);
  Piece *visit(const SwitchNode *node);
  Piece *visit(const CaseNode *node);
  Piece *visit(const BreakNode *node);
  Piece *visit(const ContinueNode *node);
  Piece *visit(const FunctionDeclNode *node);
  Piece *visit(const FunctionCallNode *node);
  Piece *visit(const ReturnNode *node);
  Piece *visit(const CastNode *node);
  Piece *visit(const ParamDeclNode *node);
  Piece *visit(const UnionDeclNode *node);
  Piece *visit(const StructDeclNode *node);
  Piece *visit(const ClassDeclNode *node);
  Piece *visit(const MemberAccessNode *node);
  Piece *visit(const ArraySubscriptNode *node);
  Piece *visit(const ArrayLiteralNode *node);
  Piece *visit(const NewExprNode *node);
  Piece *visit(const DeleteExprNode *node);
  Piece *visit(const NullNode *node);
  Piece *visit(const ImplicitCastNode *node);
};

} // namespace utopia