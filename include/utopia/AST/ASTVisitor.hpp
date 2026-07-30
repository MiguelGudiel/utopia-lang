#pragma once
#include "utopia/AST/AST.hpp"
#include <cstdlib>
#include <iostream>

namespace utopia {

template <typename Derived, typename R> class ASTVisitor {
public:
  R dispatch(const ASTNode *node) {
    /* Trap null pointers at the traversal boundary to prevent silent
     * propagation of unhandled expected types across the pipeline */
    if (!node) [[unlikely]] {
      std::cerr << "[Fatal] ASTVisitor dispatch encountered a null AST node.\n";
      std::abort();
    }

    switch (node->kind) {
    case NodeKind::Number:
      return static_cast<Derived *>(this)->visit(
          static_cast<const NumberNode *>(node));
    case NodeKind::Boolean:
      return static_cast<Derived *>(this)->visit(
          static_cast<const BoolNode *>(node));
    case NodeKind::Char:
      return static_cast<Derived *>(this)->visit(
          static_cast<const CharNode *>(node));
    case NodeKind::Rune:
      return static_cast<Derived *>(this)->visit(
          static_cast<const RuneNode *>(node));
    case NodeKind::String:
      return static_cast<Derived *>(this)->visit(
          static_cast<const StringNode *>(node));
    case NodeKind::Variable:
      return static_cast<Derived *>(this)->visit(
          static_cast<const VariableNode *>(node));
    case NodeKind::UnaryOp:
      return static_cast<Derived *>(this)->visit(
          static_cast<const UnaryOpNode *>(node));
    case NodeKind::BinaryOp:
      return static_cast<Derived *>(this)->visit(
          static_cast<const BinaryOpNode *>(node));
    case NodeKind::Module:
      return static_cast<Derived *>(this)->visit(
          static_cast<const ModuleNode *>(node));
    case NodeKind::Annotation:
      return static_cast<Derived *>(this)->visit(
          static_cast<const AnnotationNode *>(node));
    case NodeKind::AnnotationDecl:
      return static_cast<Derived *>(this)->visit(
          static_cast<const AnnotationDeclNode *>(node));
    case NodeKind::TypedefDecl:
      return static_cast<Derived *>(this)->visit(
          static_cast<const TypedefDeclNode *>(node));
    case NodeKind::VarDecl:
      return static_cast<Derived *>(this)->visit(
          static_cast<const VarDeclNode *>(node));
    case NodeKind::EnumDecl:
      return static_cast<Derived *>(this)->visit(
          static_cast<const EnumDeclNode *>(node));
    case NodeKind::EnumMember:
      return static_cast<Derived *>(this)->visit(
          static_cast<const EnumMemberNode *>(node));
    case NodeKind::Assign:
      return static_cast<Derived *>(this)->visit(
          static_cast<const AssignNode *>(node));
    case NodeKind::Block:
      return static_cast<Derived *>(this)->visit(
          static_cast<const BlockNode *>(node));
    case NodeKind::If:
      return static_cast<Derived *>(this)->visit(
          static_cast<const IfNode *>(node));
    case NodeKind::For:
      return static_cast<Derived *>(this)->visit(
          static_cast<const ForNode *>(node));
    case NodeKind::While:
      return static_cast<Derived *>(this)->visit(
          static_cast<const WhileNode *>(node));
    case NodeKind::Break:
      return static_cast<Derived *>(this)->visit(
          static_cast<const BreakNode *>(node));
    case NodeKind::Continue:
      return static_cast<Derived *>(this)->visit(
          static_cast<const ContinueNode *>(node));
    case NodeKind::FunctionDecl:
      return static_cast<Derived *>(this)->visit(
          static_cast<const FunctionDeclNode *>(node));
    case NodeKind::FunctionCall:
      return static_cast<Derived *>(this)->visit(
          static_cast<const FunctionCallNode *>(node));
    case NodeKind::Return:
      return static_cast<Derived *>(this)->visit(
          static_cast<const ReturnNode *>(node));
    case NodeKind::Cast:
      return static_cast<Derived *>(this)->visit(
          static_cast<const CastNode *>(node));
    case NodeKind::ParamDecl:
      return static_cast<Derived *>(this)->visit(
          static_cast<const ParamDeclNode *>(node));
    case NodeKind::StructDecl:
      return static_cast<Derived *>(this)->visit(
          static_cast<const StructDeclNode *>(node));
    case NodeKind::ClassDecl:
      return static_cast<Derived *>(this)->visit(
          static_cast<const ClassDeclNode *>(node));
    case NodeKind::MemberAccess:
      return static_cast<Derived *>(this)->visit(
          static_cast<const MemberAccessNode *>(node));
    case NodeKind::ArraySubscript:
      return static_cast<Derived *>(this)->visit(
          static_cast<const ArraySubscriptNode *>(node));
    case NodeKind::ArrayLiteral:
      return static_cast<Derived *>(this)->visit(
          static_cast<const ArrayLiteralNode *>(node));
    case NodeKind::New:
      return static_cast<Derived *>(this)->visit(
          static_cast<const NewExprNode *>(node));
    case NodeKind::Delete:
      return static_cast<Derived *>(this)->visit(
          static_cast<const DeleteExprNode *>(node));
    case NodeKind::Null:
      return static_cast<Derived *>(this)->visit(
          static_cast<const NullNode *>(node));
    case NodeKind::ImplicitCast:
      return static_cast<Derived *>(this)->visit(
          static_cast<const ImplicitCastNode *>(node));
    default:
      /* Dispatch failure routing to prevent silent segfaults on unmapped nodes
       */
      std::cerr << "[Fatal] ASTVisitor dispatch failure. Unhandled NodeKind: "
                << static_cast<int>(node->kind) << '\n';
      std::abort();
    }
  }
};

} // namespace utopia