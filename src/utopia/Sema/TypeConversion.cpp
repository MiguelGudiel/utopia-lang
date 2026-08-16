#include "utopia/Sema/Sema.hpp"
#include <string>

namespace utopia {

/* Scores how close a conversion-constructor parameter type is to the source
 * type: exact match wins, then same signedness, then smallest width delta.
 * A naive first-match selection would always pick the narrowest ctor (e.g.
 * String(int8)) for `x as String`, truncating every wider value. */
static int conversionCtorScore(const Type *src, const Type *param) {
  const Type *s = src->getUnqualifiedType();
  const Type *p = param->getUnqualifiedType();
  if (s == p)
    return 1000;
  if (!s->isNumeric() || !p->isNumeric())
    return 0;
  if (s->getKind() != TypeKind::Builtin || p->getKind() != TypeKind::Builtin)
    return 0;

  auto sKind = static_cast<const BuiltinType *>(s)->getBuiltinKind();
  auto pKind = static_cast<const BuiltinType *>(p)->getBuiltinKind();

  auto sizeOf = [](BuiltinKind k) {
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
    default:
      return 8;
    }
  };
  auto isSignedKind = [](BuiltinKind k) {
    return k >= BuiltinKind::Int8 && k <= BuiltinKind::Int64;
  };
  auto diff = [](int a, int b) { return a > b ? a - b : b - a; };

  if (s->isInteger() && p->isInteger()) {
    int score = 500;
    if (isSignedKind(sKind) == isSignedKind(pKind))
      score += 100;
    score -= diff(sizeOf(sKind), sizeOf(pKind)) * 50;
    return score;
  }
  if (s->isFloat() && p->isFloat()) {
    return 400 - diff(sizeOf(sKind), sizeOf(pKind)) * 50;
  }
  if (s->isInteger() && p->isFloat())
    return 100;
  if (s->isFloat() && p->isInteger())
    return 50;
  return 0;
}

const FunctionDeclNode *findBestConversionCtor(const Type *from,
                                               const RecordType *recTy) {
  if (!recTy)
    return nullptr;
  auto *decl = recTy->getDeclaration();
  if (!decl)
    return nullptr;

  llvm::ArrayRef<FunctionDeclNode *> ctors;
  if (auto *classDecl = llvm::dyn_cast<ClassDeclNode>(decl))
    ctors = classDecl->constructors;
  else if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(decl))
    ctors = structDecl->constructors;
  else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(decl))
    ctors = unionDecl->constructors;
  else
    return nullptr;

  const FunctionDeclNode *best = nullptr;
  int bestScore = -1;
  for (auto *ctor : ctors) {
    if (!ctor || ctor->params.size() != 1)
      continue;
    if (!canImplicitlyCast(from, ctor->params[0]->type, false))
      continue;
    int score = conversionCtorScore(from, ctor->params[0]->type);
    if (score > bestScore) {
      bestScore = score;
      best = ctor;
    }
  }
  return best;
}

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

  /* Recursive evaluation of both direct inheritance and interface
   * implementations */
  auto isSubclassOrImplements = [&](const ClassType *cFrom,
                                    const ClassType *cTo, auto &self) -> bool {
    if (!cFrom || !cTo)
      return false;
    if (cFrom == cTo)
      return true;

    if (cFrom->getBaseClass()) {
      if (auto *pBase = llvm::dyn_cast<ClassType>(
              cFrom->getBaseClass()->getUnqualifiedType())) {
        if (self(pBase, cTo, self))
          return true;
      }
    }

    for (const Type *iface : cFrom->getInterfaces()) {
      if (auto *pIface =
              llvm::dyn_cast<ClassType>(iface->getUnqualifiedType())) {
        if (self(pIface, cTo, self))
          return true;
      }
    }

    return false;
  };

  /* Subclass or Interface to Base Class Upcasting */
  if (unqualFrom->getKind() == TypeKind::Class &&
      unqualTo->getKind() == TypeKind::Class) {
    const ClassType *cFrom = static_cast<const ClassType *>(unqualFrom);
    const ClassType *cTo = static_cast<const ClassType *>(unqualTo);
    if (isSubclassOrImplements(cFrom, cTo, isSubclassOrImplements)) {
      return true;
    }
  }

  /* Implicit Array to ListLiteralView intrinsic resolution */
  if (auto *arrFrom = llvm::dyn_cast<ArrayType>(unqualFrom)) {
    if (auto *recTo = llvm::dyn_cast<RecordType>(unqualTo)) {
      std::string_view recName = recTo->getName();
      std::string_view marker = "ListLiteralView_";
      size_t pos = recName.find(marker);
      if (pos != std::string_view::npos) {
        std::string expectedSuffix = std::string(marker);
        std::string argStr = arrFrom->getElementType()->toString();
        for (char &c : argStr) {
          if (!isalnum(c))
            c = '_';
        }
        expectedSuffix += argStr;
        if (recName.ends_with(expectedSuffix)) {
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

  if (allowUserDefined) {
    if (auto *recTy = llvm::dyn_cast<RecordType>(unqualTo)) {
      if (findBestConversionCtor(from, recTy) != nullptr)
        return true;
    }
  }

  if (llvm::isa<EnumType>(unqualFrom)) {
    if (llvm::isa<EnumType>(unqualTo))
      return unqualFrom == unqualTo;
    /* Enums convert implicitly to their underlying integer type (and
     * promote like it), as documented: "Enums convert to their underlying
     * integer type and participate in integer arithmetic." Narrowing
     * conversions (e.g. int32-backed enum to uint8) are NOT implicit. */
    if (!unqualTo->isInteger())
      return false;
    const Type *underlying =
        static_cast<const EnumType *>(unqualFrom)->getUnderlyingType();
    if (unqualTo == underlying)
      return true;
    auto intWidth = [](const Type *t) {
      auto k = static_cast<const BuiltinType *>(t)
                   ->getBuiltinKind();
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
      default:
        return 8;
      }
    };
    auto isUnsigned = [](const Type *t) {
      auto k = static_cast<const BuiltinType *>(t)
                   ->getBuiltinKind();
      switch (k) {
      case BuiltinKind::UInt8:
      case BuiltinKind::UInt16:
      case BuiltinKind::UInt32:
      case BuiltinKind::UInt64:
      case BuiltinKind::USize:
        return true;
      default:
        return false;
      }
    };
    if (intWidth(unqualTo) > intWidth(underlying))
      return true;
    if (intWidth(unqualTo) == intWidth(underlying))
      return isUnsigned(unqualTo) == isUnsigned(underlying);
    return false;
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
        if (isSubclassOrImplements(cFrom, cTo, isSubclassOrImplements)) {
          return true;
        }
      }

      /* Enforce strict parameter structural equality for function pointer
       * assignments. Untyped lambda placeholders ('auto' components) act as
       * wildcards so an unresolved lambda matches any expected signature. */
      if (auto *fF = llvm::dyn_cast<FunctionType>(fromPointee)) {
        if (auto *fT = llvm::dyn_cast<FunctionType>(toPointee)) {
          auto isAuto = [](const Type *t) {
            return t->getUnqualifiedType()->getKind() == TypeKind::Auto;
          };
          if (fF->getReturnType()->getUnqualifiedType() !=
              fT->getReturnType()->getUnqualifiedType()) {
            if (!isAuto(fF->getReturnType()))
              return false;
          }
          if (fF->getParamTypes().size() != fT->getParamTypes().size())
            return false;
          for (size_t i = 0; i < fF->getParamTypes().size(); i++) {
            if (fF->getParamTypes()[i]->getUnqualifiedType() !=
                fT->getParamTypes()[i]->getUnqualifiedType()) {
              if (!isAuto(fF->getParamTypes()[i]))
                return false;
            }
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

  /* Strip reference layers off the target so user-defined conversions (e.g.
   * uint8* -> const String&) reach the conversion-constructor path; the
   * resulting cast node keeps the full reference type so codegen returns
   * the temporary's address. */
  const Type *targetBase = to;
  if (targetBase->isReferenceType()) {
    targetBase = static_cast<const ReferenceType *>(targetBase)
                     ->getPointeeType();
  } else if (targetBase->getKind() == TypeKind::RValueReference) {
    targetBase = static_cast<const RValueReferenceType *>(targetBase)
                     ->getPointeeType();
  }

  const Type *unqualTo = targetBase->getUnqualifiedType();
  const Type *unqualFrom = expr->exprType->getUnqualifiedType();

  /* Intercept Array to ListLiteralView intrinsic conversion */
  if (llvm::isa<ArrayType>(unqualFrom)) {
    if (auto *recTo = llvm::dyn_cast<RecordType>(unqualTo)) {
      if (recTo->getName().find("ListLiteralView_") != std::string_view::npos) {
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
    if (const FunctionDeclNode *ctor =
            findBestConversionCtor(expr->exprType, recTy)) {
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
        innerExpr = performImplicitConversion(expr, ctor->params[0]->type);
      }

      auto *castNode = ctx->astCtx.create<ImplicitCastNode>(
          innerExpr, to, ctor, expr->line, expr->column, expr->length);
      castNode->exprType = to;
      castNode->isLValue = to->isReferenceType();
      return castNode;
    }
  }

  return expr;
}

} // namespace utopia