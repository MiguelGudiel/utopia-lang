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
        /* Empty or un-typed literals ('[]', 'void[0]') bind to any view:
         * the constructor just sees a zero-length range. */
        if (arrFrom->getSize() == 0 || arrFrom->getElementType()->isVoid())
          return true;
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
        /* String literals type as 'const uint8*', so '[ "a" ]' only binds to
         * List<String> by promoting the elements (the view name 'String'
         * stays unambiguous in the mangled suffix). */
        if (arrFrom->getElementType()->getUnqualifiedType()->isPointerType()) {
          const Type *pointee = static_cast<const PointerType *>(
                                    arrFrom->getElementType()
                                        ->getUnqualifiedType())
                                    ->getPointeeType()
                                    ->getUnqualifiedType();
          if (pointee->getKind() == TypeKind::Builtin &&
              static_cast<const BuiltinType *>(pointee)->getBuiltinKind() ==
                  BuiltinKind::UInt8 &&
              recName.ends_with(std::string(marker) + "String")) {
            return true;
          }
        }
        /* Nested array/map literals as elements ('[[1, 2], [3, 4]]' into
         * List<List<int32>>): the element type must convert to the view's
         * template argument, which the conversion pass below uses to retype
         * each element. The element conversion may itself need the
         * conversion-constructor path (e.g. a map literal to Map<K, V>),
         * so user-defined conversions stay enabled here. */
        if (recTo->isTemplateInstantiation() &&
            recTo->getTemplateArgs().size() == 1) {
          const Type *elemUnqual =
              arrFrom->getElementType()->getUnqualifiedType();
          if (llvm::isa<ArrayType>(elemUnqual) ||
              llvm::isa<MapLiteralType>(elemUnqual)) {
            return canImplicitlyCast(arrFrom->getElementType(),
                                     recTo->getTemplateArgs()[0], true);
          }
        }
      }
    }
  }

  /* Implicit MapLiteral to MapLiteralView intrinsic resolution: the literal
   * '{k1: v1, ...}' converts to 'MapLiteralView<K, V>' (its instantiated
   * record carries the key/value template arguments). Empty or un-typed
   * literals bind to any view, mirroring the empty-array rule for
   * ListLiteralView. String literal keys/values ('const uint8*') promote to
   * String, and nested literal values (map/array literals) are allowed: the
   * conversion pass re-types them to the view's K/V. */
  if (auto *mapFrom = llvm::dyn_cast<MapLiteralType>(unqualFrom)) {
    if (auto *recTo = llvm::dyn_cast<RecordType>(unqualTo)) {
      std::string_view recName = recTo->getName();
      std::string_view marker = "MapLiteralView_";
      if (recName.find(marker) != std::string_view::npos) {
        if (mapFrom->getSize() == 0 ||
            mapFrom->getKeyType()->isVoid() ||
            mapFrom->getValueType()->isVoid()) {
          return true;
        }
        const Type *viewKeyTy = nullptr;
        const Type *viewValTy = nullptr;
        if (recTo->isTemplateInstantiation() &&
            recTo->getTemplateArgs().size() == 2) {
          viewKeyTy = recTo->getTemplateArgs()[0]->getUnqualifiedType();
          viewValTy = recTo->getTemplateArgs()[1]->getUnqualifiedType();
        }
        if (!viewKeyTy || !viewValTy) {
          return false;
        }
        auto isStringLiteralType = [](const Type *t) {
          const Type *u = t->getUnqualifiedType();
          if (!u->isPointerType())
            return false;
          const Type *pointee = static_cast<const PointerType *>(u)
                                    ->getPointeeType()
                                    ->getUnqualifiedType();
          return pointee->getKind() == TypeKind::Builtin &&
                 static_cast<const BuiltinType *>(pointee)->getBuiltinKind() ==
                     BuiltinKind::UInt8;
        };
        auto isStringType = [](const Type *t) {
          const Type *u = t->getUnqualifiedType();
          return (u->getKind() == TypeKind::Struct ||
                  u->getKind() == TypeKind::Class) &&
                 static_cast<const RecordType *>(u)->getName() == "String";
        };

        /* Exact match on both the key and the value. */
        if (mapFrom->getKeyType()->getUnqualifiedType() == viewKeyTy &&
            mapFrom->getValueType()->getUnqualifiedType() == viewValTy) {
          return true;
        }
        /* String literal keys/values promote to String. */
        if (isStringLiteralType(mapFrom->getKeyType()) &&
            isStringType(viewKeyTy) &&
            mapFrom->getValueType()->getUnqualifiedType() == viewValTy) {
          return true;
        }
        if (isStringLiteralType(mapFrom->getValueType()) &&
            isStringType(viewValTy) &&
            mapFrom->getKeyType()->getUnqualifiedType() == viewKeyTy) {
          return true;
        }
        if (isStringLiteralType(mapFrom->getKeyType()) &&
            isStringLiteralType(mapFrom->getValueType()) &&
            isStringType(viewKeyTy) && isStringType(viewValTy)) {
          return true;
        }
        /* Nested map/array literal values convert to the target value type
         * (e.g. '{1: {"a": 2}}' into Map<int, Map<String, int>>). The key
         * must match exactly or be a string literal promoting to String. */
        bool keyMatches = mapFrom->getKeyType()->getUnqualifiedType() ==
                              viewKeyTy ||
                          (isStringLiteralType(mapFrom->getKeyType()) &&
                           isStringType(viewKeyTy));
        if (keyMatches &&
            (llvm::isa<MapLiteralType>(mapFrom->getValueType()) ||
             llvm::isa<ArrayType>(mapFrom->getValueType())) &&
            viewValTy->getUnqualifiedType()->getKind() == TypeKind::Class) {
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
    /* By-value targets copy their argument, so a const-qualified source may
     * pass through (the copy drops const safely, like C++). Only binding to
     * a mutable reference would let the callee mutate the source. */
    bool toIsRef = baseTo->isReferenceType() ||
                   baseTo->getKind() == TypeKind::RValueReference;
    if (!toIsRef || !baseFrom->isConstQualified() || baseTo->isConstQualified())
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
        const Type *elemTy = static_cast<const ArrayType *>(unqualFrom)
                                 ->getElementType();
        const Type *viewTy = nullptr;
        if (recTo->getName().ends_with("ListLiteralView_String") &&
            elemTy->getUnqualifiedType()->isPointerType()) {
          const Type *pointee = static_cast<const PointerType *>(
                                    elemTy->getUnqualifiedType())
                                    ->getPointeeType()
                                    ->getUnqualifiedType();
          if (pointee->getKind() == TypeKind::Builtin &&
              static_cast<const BuiltinType *>(pointee)->getBuiltinKind() ==
                  BuiltinKind::UInt8) {
            viewTy = ctx->astCtx.getRecordType("String");
          }
        } else if (recTo->isTemplateInstantiation() &&
                   recTo->getTemplateArgs().size() == 1) {
          /* Nested literal elements ('[[1, 2], [3, 4]]'): retype each
           * element to the view's template argument, mirroring the
           * nested-value rule of the MapLiteralView conversion. */
          const Type *elemUnqual = elemTy->getUnqualifiedType();
          if (llvm::isa<ArrayType>(elemUnqual) ||
              llvm::isa<MapLiteralType>(elemUnqual)) {
            viewTy = recTo->getTemplateArgs()[0]->getUnqualifiedType();
          }
        }
        if (viewTy) {
          /* Promote string literal elements to String so the literal is
           * laid out as String storage (string literals type as
           * 'const uint8*'). */
          auto *lit = const_cast<ArrayLiteralNode *>(
              static_cast<const ArrayLiteralNode *>(expr));
          std::vector<ExprNode *> promoted;
          for (const auto *elem : lit->elements) {
            promoted.push_back(performImplicitConversion(
                const_cast<ExprNode *>(elem), viewTy));
          }
          lit->elements = ctx->astCtx.copyArray<ExprNode *>(promoted);
          lit->exprType = ctx->astCtx.getArrayType(
              viewTy, lit->elements.size());
        }
        auto *castNode = ctx->astCtx.create<ImplicitCastNode>(
            expr, to, nullptr, expr->line, expr->column, expr->length);
        castNode->exprType = to;
        castNode->isLValue = false;
        return castNode;
      }
    }
  }

  /* Intercept MapLiteral to MapLiteralView intrinsic conversion */
  if (llvm::isa<MapLiteralType>(unqualFrom)) {
    if (auto *recTo = llvm::dyn_cast<RecordType>(unqualTo)) {
      if (recTo->getName().find("MapLiteralView_") != std::string_view::npos) {
        auto *mapLit = static_cast<const MapLiteralType *>(unqualFrom);
        const Type *keyTy = mapLit->getKeyType();
        const Type *valTy = mapLit->getValueType();
        const Type *strTy = ctx->astCtx.getRecordType("String");
        auto isStringLiteralType = [](const Type *t) {
          const Type *u = t->getUnqualifiedType();
          if (!u->isPointerType())
            return false;
          const Type *pointee = static_cast<const PointerType *>(u)
                                    ->getPointeeType()
                                    ->getUnqualifiedType();
          return pointee->getKind() == TypeKind::Builtin &&
                 static_cast<const BuiltinType *>(pointee)->getBuiltinKind() ==
                     BuiltinKind::UInt8;
        };

        const Type *viewKeyTy = nullptr;
        const Type *viewValTy = nullptr;
        if (recTo->isTemplateInstantiation() &&
            recTo->getTemplateArgs().size() == 2) {
          viewKeyTy = recTo->getTemplateArgs()[0]->getUnqualifiedType();
          viewValTy = recTo->getTemplateArgs()[1]->getUnqualifiedType();
        }

        bool retypeKeys = false;
        bool retypeValues = false;
        if (viewKeyTy && viewValTy) {
          if (keyTy->getUnqualifiedType() != viewKeyTy &&
              isStringLiteralType(keyTy) && viewKeyTy == strTy) {
            retypeKeys = true;
          }
          if (valTy->getUnqualifiedType() != viewValTy) {
            if (isStringLiteralType(valTy) && viewValTy == strTy) {
              retypeValues = true;
            } else if (llvm::isa<MapLiteralType>(valTy) ||
                       llvm::isa<ArrayType>(valTy)) {
              /* Nested literal value ('{"a": {"b": 1}}'): convert each
               * value expression to the target value type. */
              retypeValues = true;
            }
          }
        }

        if (retypeKeys || retypeValues) {
          auto *lit = const_cast<MapLiteralNode *>(
              static_cast<const MapLiteralNode *>(expr));
          if (retypeKeys) {
            std::vector<ExprNode *> promotedKeys;
            for (const auto *key : lit->keys) {
              promotedKeys.push_back(performImplicitConversion(
                  const_cast<ExprNode *>(key), viewKeyTy));
            }
            lit->keys = ctx->astCtx.copyArray<ExprNode *>(promotedKeys);
          }
          if (retypeValues) {
            std::vector<ExprNode *> promotedValues;
            for (const auto *value : lit->values) {
              promotedValues.push_back(performImplicitConversion(
                  const_cast<ExprNode *>(value), viewValTy));
            }
            lit->values = ctx->astCtx.copyArray<ExprNode *>(promotedValues);
          }
          lit->exprType = ctx->astCtx.getMapLiteralType(
              retypeKeys ? viewKeyTy : keyTy,
              retypeValues ? viewValTy : valTy, lit->keys.size());
        }
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