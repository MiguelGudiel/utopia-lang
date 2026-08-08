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
  if (auto *refTy = llvm::dyn_cast<ReferenceType>(from)) {
    baseFrom = refTy->getPointeeType();
  } else if (auto *rvRefTy = llvm::dyn_cast<RValueReferenceType>(from)) {
    baseFrom = rvRefTy->getPointeeType();
  }

  const Type *baseTo = to;
  if (auto *refTyTo = llvm::dyn_cast<ReferenceType>(to)) {
    baseTo = refTyTo->getPointeeType();
  } else if (auto *rvRefTyTo = llvm::dyn_cast<RValueReferenceType>(to)) {
    baseTo = rvRefTyTo->getPointeeType();
  }

  if (baseFrom == baseTo)
    return true;

  /* Resolve underlying entity traits to bypass opaque typedefs and const
   * qualifiers */
  const Type *unqualFrom = baseFrom->getUnqualifiedType();
  const Type *unqualTo = baseTo->getUnqualifiedType();

  /* Subclass to Base Class Upcasting */
  if (unqualFrom->getKind() == TypeKind::Class &&
      unqualTo->getKind() == TypeKind::Class) {
    const ClassType *cFrom = static_cast<const ClassType *>(unqualFrom);
    const ClassType *cTo = static_cast<const ClassType *>(unqualTo);
    while (cFrom) {
      if (cFrom == cTo)
        return true;
      if (cFrom->getBaseClass()) {
        cFrom = static_cast<const ClassType *>(
            cFrom->getBaseClass()->getUnqualifiedType());
      } else {
        break;
      }
    }
  }

  /* Implicit Array to ListLiteralView intrinsic resolution */
  if (auto *arrFrom = llvm::dyn_cast<ArrayType>(unqualFrom)) {
    if (auto *recTo = llvm::dyn_cast<RecordType>(unqualTo)) {
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
  }

  /* Support casting from an internal TypeVal to a user-defined generic 'Type'
   * class */
  if (auto *builtinFrom = llvm::dyn_cast<BuiltinType>(unqualFrom)) {
    if (builtinFrom->getBuiltinKind() == BuiltinKind::TypeVal) {
      if (auto *classTo = llvm::dyn_cast<ClassType>(unqualTo)) {
        if (classTo->getName() == "Type")
          return true;
      }
      if (auto *builtinTo = llvm::dyn_cast<BuiltinType>(unqualTo)) {
        if (builtinTo->getBuiltinKind() == BuiltinKind::TypeVal)
          return true;
      }
    }
  }

  /* Process user-defined single-argument conversion constructors */
  if (allowUserDefined) {
    if (auto *recTy = llvm::dyn_cast<RecordType>(unqualTo)) {
      if (auto *decl = recTy->getDeclaration()) {
        llvm::ArrayRef<FunctionDeclNode *> ctors;
        if (auto *classDecl = llvm::dyn_cast<ClassDeclNode>(decl))
          ctors = classDecl->constructors;
        else if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(decl))
          ctors = structDecl->constructors;
        else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(decl))
          ctors = unionDecl->constructors;

        for (auto *ctor : ctors) {
          if (ctor->params.size() == 1) {
            if (canImplicitlyCast(from, ctor->params[0]->type, false))
              return true;
          }
        }
      }
    }
  }

  if (llvm::isa<EnumType>(unqualFrom) && llvm::isa<EnumType>(unqualTo)) {
    return unqualFrom == unqualTo;
  }

  if (auto *arrFrom = llvm::dyn_cast<ArrayType>(unqualFrom)) {
    if (auto *arrTo = llvm::dyn_cast<ArrayType>(unqualTo)) {
      /* Allow empty or un-typed array literals to bind to expected target array
       * type */
      if (arrFrom->getSize() == 0 || arrFrom->getElementType()->isVoid())
        return true;

      if (arrFrom->getSize() == arrTo->getSize())
        return canImplicitlyCast(arrFrom->getElementType(),
                                 arrTo->getElementType(), allowUserDefined);
    }
  }

  if (unqualFrom == unqualTo) {
    if (!baseFrom->isConstQualified() || baseTo->isConstQualified())
      return true;
  }

  if (auto *arrFrom = llvm::dyn_cast<ArrayType>(unqualFrom)) {
    if (auto *ptrTo = llvm::dyn_cast<PointerType>(unqualTo)) {
      const Type *elemTy = arrFrom->getElementType();
      const Type *toPointee = ptrTo->getPointeeType();
      if (toPointee->isVoid() ||
          elemTy->getUnqualifiedType() == toPointee->getUnqualifiedType())
        return true;
    }
  }

  if (auto *ptrFrom = llvm::dyn_cast<PointerType>(unqualFrom)) {
    if (auto *ptrTo = llvm::dyn_cast<PointerType>(unqualTo)) {
      const Type *fromPointee = ptrFrom->getPointeeType();
      const Type *toPointee = ptrTo->getPointeeType();

      /* Universal null pointer interoperability */
      if (toPointee->isVoid() || fromPointee->isVoid())
        return true;

      /* Subclass to Base Class Pointer Upcasting */
      if (fromPointee->getUnqualifiedType()->getKind() == TypeKind::Class &&
          toPointee->getUnqualifiedType()->getKind() == TypeKind::Class) {
        const ClassType *cFrom =
            static_cast<const ClassType *>(fromPointee->getUnqualifiedType());
        const ClassType *cTo =
            static_cast<const ClassType *>(toPointee->getUnqualifiedType());
        while (cFrom) {
          if (cFrom == cTo)
            return true;
          if (cFrom->getBaseClass()) {
            cFrom = static_cast<const ClassType *>(
                cFrom->getBaseClass()->getUnqualifiedType());
          } else {
            break;
          }
        }
      }

      /* Enforce strict parameter structural equality for function pointer
       * assignments */
      if (auto *fF = llvm::dyn_cast<FunctionType>(fromPointee)) {
        if (auto *fT = llvm::dyn_cast<FunctionType>(toPointee)) {
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
      }

      return fromPointee->getUnqualifiedType() ==
             toPointee->getUnqualifiedType();
    }
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
      case BuiltinKind::USize:
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
           fKind == BuiltinKind::UInt32 || fKind == BuiltinKind::UInt64 ||
           fKind == BuiltinKind::USize);
      bool isToUnsigned =
          (tKind == BuiltinKind::UInt8 || tKind == BuiltinKind::UInt16 ||
           tKind == BuiltinKind::UInt32 || tKind == BuiltinKind::UInt64 ||
           tKind == BuiltinKind::USize);

      if (isFromUnsigned != isToUnsigned) {
        if (!isFromUnsigned && isToUnsigned) {
          // Signed to Unsigned: Always risky
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

      if (auto *num = llvm::dyn_cast<NumberNode>(node)) {
        numNode = num;
      } else if (auto *uop = llvm::dyn_cast<UnaryOpNode>(node)) {
        if (uop->op == "-" && llvm::isa<NumberNode>(uop->expr)) {
          numNode = llvm::cast<NumberNode>(uop->expr);
          isNegative = true;
        } else if (uop->op == "+" && llvm::isa<NumberNode>(uop->expr)) {
          numNode = llvm::cast<NumberNode>(uop->expr);
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

  if (llvm::isa<ImplicitCastNode>(expr))
    return expr;

  const Type *unqualTo = to->getUnqualifiedType();
  const Type *unqualFrom = expr->exprType->getUnqualifiedType();

  /* Intercept Array to ListLiteralView intrinsic conversion */
  if (llvm::isa<ArrayType>(unqualFrom)) {
    if (auto *recTo = llvm::dyn_cast<RecordType>(unqualTo)) {
      if (recTo->getName().starts_with("ListLiteralView_")) {
        auto *castNode = ctx->astCtx.create<ImplicitCastNode>(
            expr, to, nullptr, expr->line, expr->column, expr->length);
        castNode->exprType = to;
        castNode->isLValue = false;
        return castNode;
      }
    }
  }

  /* Prioritize built-in or exact primitive type mappings */
  if (canImplicitlyCast(expr->exprType, to, false))
    return expr;

  /* Intercept and rewrite aggregate initialization to invoke conversion
   * constructors */
  if (auto *recTy = llvm::dyn_cast<RecordType>(unqualTo)) {
    if (auto *decl = recTy->getDeclaration()) {
      llvm::ArrayRef<FunctionDeclNode *> ctors;
      if (auto *classDecl = llvm::dyn_cast<ClassDeclNode>(decl))
        ctors = classDecl->constructors;
      else if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(decl))
        ctors = structDecl->constructors;
      else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(decl))
        ctors = unionDecl->constructors;

      for (auto *ctor : ctors) {
        if (ctor->params.size() == 1) {
          if (canImplicitlyCast(expr->exprType, ctor->params[0]->type, false)) {
            /* Enforce visibility constraint on implicit conversions */
            if (!ctor->isPublic(ctor->name) &&
                ctx->getCurrentRecordContext() != recTy) {
              ctx->diags.report({DiagLevel::Error, expr->line, expr->column,
                                 expr->length,
                                 "Implicit conversion failed. Conversion "
                                 "constructor is private.",
                                 std::string(ctx->currentFile), expr->endLine});
              return expr;
            }

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