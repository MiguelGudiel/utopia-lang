#include "utopia/Common/Logger.hpp"
#include "utopia/Sema/Sema.hpp"
#include <string>

namespace utopia {

bool canImplicitlyCast(const Type *from, const Type *to,
                       bool allowUserDefined) {
  if (!from || !to)
    return false;

  if (from == to)
    return true;

  const Type *baseFrom = from;
  if (from->isReferenceType()) {
    baseFrom = static_cast<const ReferenceType *>(from)->getPointeeType();
  } else if (from->getKind() == TypeKind::RValueReference) {
    baseFrom = static_cast<const RValueReferenceType *>(from)->getPointeeType();
  }

  const Type *baseTo = to;
  if (to->isReferenceType()) {
    baseTo = static_cast<const ReferenceType *>(to)->getPointeeType();
  } else if (to->getKind() == TypeKind::RValueReference) {
    baseTo = static_cast<const RValueReferenceType *>(to)->getPointeeType();
  }

  if (baseFrom == baseTo)
    return true;

  /* Resolve underlying entity traits to bypass opaque typedefs and const
   * qualifiers */
  const Type *unqualFrom = baseFrom->getUnqualifiedType();
  const Type *unqualTo = baseTo->getUnqualifiedType();

  /* Implicit Array to ListLiteralView intrinsic resolution */
  if (unqualFrom->getKind() == TypeKind::Array &&
      unqualTo->getKind() == TypeKind::Struct) {
    auto *arrFrom = static_cast<const ArrayType *>(unqualFrom);
    auto *recTo = static_cast<const RecordType *>(unqualTo);
    if (recTo->getName().starts_with("ListLiteralView_")) {
      std::string expectedName = "ListLiteralView_";
      std::string argStr = arrFrom->getElementType()->toString();
      for (char &c : argStr) {
        if (!isalnum(c))
          c = '_';
      }
      expectedName += argStr;
      if (recTo->getName() == expectedName) {
        return true;
      }
    }
  }

  /* Support casting from an internal TypeVal to a user-defined generic 'Type'
   * class */
  if (unqualFrom->isBuiltinType() &&
      static_cast<const BuiltinType *>(unqualFrom)->getBuiltinKind() ==
          BuiltinKind::TypeVal) {
    if (unqualTo->getKind() == TypeKind::Class &&
        static_cast<const RecordType *>(unqualTo)->getName() == "Type")
      return true;
    if (unqualTo->isBuiltinType() &&
        static_cast<const BuiltinType *>(unqualTo)->getBuiltinKind() ==
            BuiltinKind::TypeVal)
      return true;
  }

  /* Process user-defined single-argument conversion constructors */
  if (allowUserDefined && (unqualTo->getKind() == TypeKind::Class ||
                           unqualTo->getKind() == TypeKind::Struct ||
                           unqualTo->getKind() == TypeKind::Union)) {
    auto *recTy = static_cast<const RecordType *>(unqualTo);
    if (auto *decl = recTy->getDeclaration()) {
      llvm::ArrayRef<FunctionDeclNode *> ctors;
      if (decl->kind == NodeKind::ClassDecl)
        ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
      else if (decl->kind == NodeKind::StructDecl)
        ctors = static_cast<const StructDeclNode *>(decl)->constructors;
      else if (decl->kind == NodeKind::UnionDecl)
        ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

      for (auto *ctor : ctors) {
        if (ctor->params.size() == 1) {
          if (canImplicitlyCast(from, ctor->params[0]->type, false))
            return true;
        }
      }
    }
  }

  if (unqualFrom->getKind() == TypeKind::Enum &&
      unqualTo->getKind() == TypeKind::Enum) {
    return unqualFrom == unqualTo;
  }

  if (unqualFrom->getKind() == TypeKind::Array &&
      unqualTo->getKind() == TypeKind::Array) {
    auto *arrFrom = static_cast<const ArrayType *>(unqualFrom);
    auto *arrTo = static_cast<const ArrayType *>(unqualTo);

    /* Allow empty or un-typed array literals to bind to expected target array
     * type */
    if (arrFrom->getSize() == 0 || arrFrom->getElementType()->isVoid())
      return true;

    if (arrFrom->getSize() == arrTo->getSize())
      return canImplicitlyCast(arrFrom->getElementType(),
                               arrTo->getElementType(), allowUserDefined);
  }

  if (unqualFrom == unqualTo) {
    if (!baseFrom->isConstQualified() || baseTo->isConstQualified())
      return true;
  }

  if (unqualFrom->getKind() == TypeKind::Array && unqualTo->isPointerType()) {
    const Type *elemTy =
        static_cast<const ArrayType *>(unqualFrom)->getElementType();
    const Type *toPointee =
        static_cast<const PointerType *>(unqualTo)->getPointeeType();
    if (toPointee->isVoid() ||
        elemTy->getUnqualifiedType() == toPointee->getUnqualifiedType())
      return true;
  }

  if (unqualFrom->isPointerType() && unqualTo->isPointerType()) {
    const Type *fromPointee =
        static_cast<const PointerType *>(unqualFrom)->getPointeeType();
    const Type *toPointee =
        static_cast<const PointerType *>(unqualTo)->getPointeeType();

    /* Universal null pointer interoperability */
    if (toPointee->isVoid() || fromPointee->isVoid())
      return true;

    /* Enforce strict parameter structural equality for function pointer
     * assignments */
    if (fromPointee->getKind() == TypeKind::Function &&
        toPointee->getKind() == TypeKind::Function) {
      auto fF = static_cast<const FunctionType *>(fromPointee);
      auto fT = static_cast<const FunctionType *>(toPointee);
      if (fF->getReturnType()->getUnqualifiedType() !=
          fT->getReturnType()->getUnqualifiedType())
        return false;
      if (fF->getParamTypes().size() != fT->getParamTypes().size())
        return false;
      for (size_t i = 0; i < fF->getParamTypes().size(); i++) {
        if (fF->getParamTypes()[i]->getUnqualifiedType() !=
            fT->getParamTypes()[i]->getUnqualifiedType())
          return false;
      }
      return true;
    }

    return fromPointee->getUnqualifiedType() == toPointee->getUnqualifiedType();
  }

  return unqualFrom->isNumeric() && unqualTo->isNumeric();
}

void TypeCheckPass::checkImplicitCastWarning(const Type *from, const Type *to,
                                             const ASTNode *node) {
  if (!from || !to || !node)
    return;
  const Type *unqualFrom = from->getUnqualifiedType();
  const Type *unqualTo = to->getUnqualifiedType();

  if (unqualFrom == unqualTo)
    return;

  if (unqualFrom->isNumeric() && unqualTo->isNumeric()) {
    auto fKind = static_cast<const BuiltinType *>(unqualFrom)->getBuiltinKind();
    auto tKind = static_cast<const BuiltinType *>(unqualTo)->getBuiltinKind();

    bool loss = false;
    bool signMismatch = false;

    bool isFromInteger = unqualFrom->isInteger();
    bool isToInteger = unqualTo->isInteger();

    auto getSize = [](BuiltinKind k) {
      switch (k) {
      case BuiltinKind::Int8:
      case BuiltinKind::UInt8:
        return 1;
      case BuiltinKind::Int16:
      case BuiltinKind::UInt16:
        return 2;
      case BuiltinKind::Int32:
      case BuiltinKind::UInt32:
        return 4;
      case BuiltinKind::Int64:
      case BuiltinKind::UInt64:
        return 8;
      default:
        return 0;
      }
    };

    if (unqualFrom->isFloat() && unqualTo->isInteger()) {
      loss = true;
    } else if (fKind == BuiltinKind::Float64 && tKind == BuiltinKind::Float32) {
      loss = true;
    } else if (isFromInteger && isToInteger) {
      if (getSize(fKind) > getSize(tKind)) {
        loss = true;
      }

      bool isFromUnsigned =
          (fKind == BuiltinKind::UInt8 || fKind == BuiltinKind::UInt16 ||
           fKind == BuiltinKind::UInt32 || fKind == BuiltinKind::UInt64);
      bool isToUnsigned =
          (tKind == BuiltinKind::UInt8 || tKind == BuiltinKind::UInt16 ||
           tKind == BuiltinKind::UInt32 || tKind == BuiltinKind::UInt64);

      if (isFromUnsigned != isToUnsigned) {
        if (!isFromUnsigned && isToUnsigned) {
          // Signed to Unsigned: Always risky (negative values wrap to massive
          // unsigned ones)
          signMismatch = true;
        } else if (isFromUnsigned && !isToUnsigned) {
          // Unsigned to Signed: Safe only if the destination type is strictly
          // larger
          if (getSize(tKind) <= getSize(fKind)) {
            signMismatch = true;
          }
        }
      }
    }

    if (loss || signMismatch) {
      bool isLiteralFit = false;
      const NumberNode *numNode = nullptr;
      bool isNegative = false;

      if (node->kind == NodeKind::Number) {
        numNode = static_cast<const NumberNode *>(node);
      } else if (node->kind == NodeKind::UnaryOp) {
        auto uop = static_cast<const UnaryOpNode *>(node);
        if (uop->op == "-" && uop->expr->kind == NodeKind::Number) {
          numNode = static_cast<const NumberNode *>(uop->expr);
          isNegative = true;
        } else if (uop->op == "+" && uop->expr->kind == NodeKind::Number) {
          numNode = static_cast<const NumberNode *>(uop->expr);
        }
      }

      if (numNode) {
        try {
          if (numNode->isFloat) {
            if (unqualFrom->isFloat() && unqualTo->isFloat()) {
              isLiteralFit = true;
            }
          } else {
            uint64_t uval = std::stoull(std::string(numNode->raw), nullptr, 0);
            if (unqualTo->isInteger()) {
              int64_t sval = static_cast<int64_t>(isNegative ? -uval : uval);
              isLiteralFit = true;
              switch (tKind) {
              case BuiltinKind::Int8:
                if (sval < -128 || sval > 127)
                  isLiteralFit = false;
                break;
              case BuiltinKind::UInt8:
                if (isNegative || uval > 255)
                  isLiteralFit = false;
                break;
              case BuiltinKind::Int16:
                if (sval < -32768 || sval > 32767)
                  isLiteralFit = false;
                break;
              case BuiltinKind::UInt16:
                if (isNegative || uval > 65535)
                  isLiteralFit = false;
                break;
              case BuiltinKind::Int32:
                if (sval < -2147483648LL || sval > 2147483647LL)
                  isLiteralFit = false;
                break;
              case BuiltinKind::UInt32:
                if (isNegative || uval > 4294967295ULL)
                  isLiteralFit = false;
                break;
              default:
                break;
              }
            }
          }
        } catch (...) {
        }
      }

      if (!isLiteralFit) {
        if (loss) {
          ctx->diags.report({DiagLevel::Warning, node->line, node->column,
                             node->length,
                             "Implicit conversion from '" + from->toString() +
                                 "' to '" + to->toString() +
                                 "' may lose precision. Use an explicit cast "
                                 "to suppress this warning.",
                             std::string(ctx->currentFile), node->endLine});
        } else if (signMismatch) {
          ctx->diags.report(
              {DiagLevel::Warning, node->line, node->column, node->length,
               "Implicit conversion changes signedness from '" +
                   from->toString() + "' to '" + to->toString() +
                   "'. Use an explicit cast to suppress this warning.",
               std::string(ctx->currentFile), node->endLine});
        }
      }
    }
  }
}

ExprNode *TypeCheckPass::performImplicitConversion(ExprNode *expr,
                                                   const Type *to) {
  if (!expr || !expr->exprType || !to)
    return expr;

  if (expr->kind == NodeKind::ImplicitCast)
    return expr;

  const Type *unqualTo = to->getUnqualifiedType();
  const Type *unqualFrom = expr->exprType->getUnqualifiedType();

  /* Intercept Array to ListLiteralView intrinsic conversion */
  if (unqualFrom->getKind() == TypeKind::Array &&
      unqualTo->getKind() == TypeKind::Struct) {
    auto *recTo = static_cast<const RecordType *>(unqualTo);
    if (recTo->getName().starts_with("ListLiteralView_")) {
      auto *castNode = ctx->astCtx.create<ImplicitCastNode>(
          expr, to, nullptr, expr->line, expr->column, expr->length);
      castNode->exprType = to;
      castNode->isLValue = false;
      return castNode;
    }
  }

  /* Prioritize built-in or exact primitive type mappings */
  if (canImplicitlyCast(expr->exprType, to, false))
    return expr;

  /* Intercept and rewrite aggregate initialization to invoke conversion
   * constructors */
  if (unqualTo->getKind() == TypeKind::Class ||
      unqualTo->getKind() == TypeKind::Struct ||
      unqualTo->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqualTo);
    if (auto *decl = recTy->getDeclaration()) {
      llvm::ArrayRef<FunctionDeclNode *> ctors;
      if (decl->kind == NodeKind::ClassDecl)
        ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
      else if (decl->kind == NodeKind::StructDecl)
        ctors = static_cast<const StructDeclNode *>(decl)->constructors;
      else
        ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

      for (auto *ctor : ctors) {
        if (ctor->params.size() == 1) {
          if (canImplicitlyCast(expr->exprType, ctor->params[0]->type, false)) {
            ExprNode *innerExpr = expr;
            if (expr->exprType->getUnqualifiedType() !=
                ctor->params[0]->type->getUnqualifiedType()) {
              innerExpr =
                  performImplicitConversion(expr, ctor->params[0]->type);
            }

            auto *castNode = ctx->astCtx.create<ImplicitCastNode>(
                innerExpr, to, ctor, expr->line, expr->column, expr->length);
            castNode->exprType = to;
            castNode->isLValue = to->isReferenceType();
            return castNode;
          }
        }
      }
    }
  }

  return expr;
}

} // namespace utopia