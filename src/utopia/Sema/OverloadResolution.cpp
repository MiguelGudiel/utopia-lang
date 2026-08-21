#include "utopia/Sema/Sema.hpp"
#include <limits>

namespace utopia {

const FunctionDeclNode *
TypeCheckPass::resolveOverloadedOperator(const Type *lhsType,
                                         std::string_view opName,
                                         const std::vector<ExprNode *> &args) {
  if (!lhsType)
    return nullptr;

  auto isUserDefined = [](const Type *t) {
    if (!t)
      return false;
    const Type *unqual = t->getUnqualifiedType();
    if (unqual->isPointerType()) {
      unqual = static_cast<const PointerType *>(unqual)
                   ->getPointeeType()
                   ->getUnqualifiedType();
    } else if (unqual->isReferenceType()) {
      unqual = static_cast<const ReferenceType *>(unqual)
                   ->getPointeeType()
                   ->getUnqualifiedType();
    } else if (unqual->getKind() == TypeKind::RValueReference) {
      unqual = static_cast<const RValueReferenceType *>(unqual)
                   ->getPointeeType()
                   ->getUnqualifiedType();
    }
    return unqual->getKind() == TypeKind::Struct ||
           unqual->getKind() == TypeKind::Class ||
           unqual->getKind() == TypeKind::Union ||
           unqual->getKind() == TypeKind::Enum;
  };

  bool hasUserDefinedOperand = isUserDefined(lhsType);
  if (!hasUserDefinedOperand) {
    for (const auto *arg : args) {
      if (arg && isUserDefined(arg->exprType)) {
        hasUserDefinedOperand = true;
        break;
      }
    }
  }

  if (!hasUserDefinedOperand) {
    return nullptr;
  }

  std::string opFuncName = "operator" + std::string(opName);
  const FunctionDeclNode *bestMatch = nullptr;
  int bestScore = std::numeric_limits<int>::min();

  const Type *unqual = lhsType->getUnqualifiedType();
  if (unqual->isPointerType()) {
    unqual = static_cast<const PointerType *>(unqual)
                 ->getPointeeType()
                 ->getUnqualifiedType();
  } else if (unqual->isReferenceType()) {
    unqual = static_cast<const ReferenceType *>(unqual)
                 ->getPointeeType()
                 ->getUnqualifiedType();
  } else if (unqual->getKind() == TypeKind::RValueReference) {
    unqual = static_cast<const RValueReferenceType *>(unqual)
                 ->getPointeeType()
                 ->getUnqualifiedType();
  }

  if (unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqual);
    auto *decl = recTy->getDeclaration();
    if (decl) {
      llvm::ArrayRef<FunctionDeclNode *> methods;
      if (decl->kind == NodeKind::ClassDecl) {
        methods = static_cast<const ClassDeclNode *>(decl)->methods;
      } else if (decl->kind == NodeKind::StructDecl) {
        methods = static_cast<const StructDeclNode *>(decl)->methods;
      } else if (decl->kind == NodeKind::UnionDecl) {
        methods = static_cast<const UnionDeclNode *>(decl)->methods;
      }

      for (auto *m : methods) {
        if (m->name == opFuncName) {
          /* Instance methods receive 'this' implicitly: they declare only
           * the operand parameters. Static methods (like global operators)
           * declare the left operand as their first parameter. Matching
           * both under 'params.size() == args.size()' made a static
           * operator with too few parameters win and then index past the
           * end of 'params' at the call site. */
          size_t paramBase = m->isStatic ? 1 : 0;
          if (m->params.size() != paramBase + args.size()) {
            continue;
          }

          if (m->isStatic &&
              !canImplicitlyCast(lhsType, m->params[0]->type)) {
            continue;
          }

          bool match = true;
          int currentScore = m->isStatic
                                 ? (typesMatchExactly(lhsType, m->params[0]->type)
                                        ? 10
                                        : 1)
                                 : 0;

          for (size_t i = 0; i < args.size(); ++i) {
            const Type *argType = args[i]->exprType;
            const Type *paramType = m->params[paramBase + i]->type;

            if (paramType->getKind() == TypeKind::Array) {
              paramType = ctx->astCtx.getPointerType(
                  static_cast<const ArrayType *>(paramType)->getElementType());
            }

            if (!canImplicitlyCast(argType, paramType)) {
              match = false;
              break;
            }

            if (typesMatchExactly(argType, paramType)) {
              currentScore += 10;
            }

            bool isLValue = args[i]->isLValue;
            if (paramType->getKind() == TypeKind::RValueReference) {
              if (isLValue) {
                match = false;
                break;
              }
              currentScore += 3;
            } else if (paramType->isReferenceType()) {
              const Type *pointee =
                  static_cast<const ReferenceType *>(paramType)->getPointeeType();
              if (!pointee->isConstQualified()) {
                if (!isLValue) {
                  currentScore += 1;
                } else {
                  currentScore += 3;
                }
              } else {
                currentScore += 2;
              }
            } else {
              currentScore += 1;
            }
          }

          if (match && currentScore > bestScore) {
            bestScore = currentScore;
            bestMatch = m;
          }
        }
      }
    }
  }

  auto globalDecls = ctx->lookup(opFuncName);
  for (const auto *d : globalDecls) {
    if (d->kind == NodeKind::FunctionDecl) {
      auto *fDecl = static_cast<const FunctionDeclNode *>(d);
      if (fDecl->params.size() == 1 + args.size()) {
        bool match = true;
        int currentScore = 0;

        const Type *lhsParamType = fDecl->params[0]->type;
        /* The left operand must convert DIRECTLY (no conversion
         * constructors): otherwise implicit enum->underlying conversions
         * would make e.g. 'String(int32)' reachable from an enum and
         * hijack built-in arithmetic like 'enumValue + 1'. */
        if (!canImplicitlyCast(lhsType, lhsParamType, false)) {
          continue;
        }

        currentScore += typesMatchExactly(lhsType, lhsParamType) ? 10 : 1;

        for (size_t i = 0; i < args.size(); ++i) {
          const Type *argType = args[i]->exprType;
          const Type *paramType = fDecl->params[1 + i]->type;

          if (paramType->getKind() == TypeKind::Array) {
            paramType = ctx->astCtx.getPointerType(
                static_cast<const ArrayType *>(paramType)->getElementType());
          }

          if (!canImplicitlyCast(argType, paramType)) {
            match = false;
            break;
          }

          if (typesMatchExactly(argType, paramType)) {
            currentScore += 10;
          }

          bool isLValue = args[i]->isLValue;
          if (paramType->getKind() == TypeKind::RValueReference) {
            if (isLValue) {
              match = false;
              break;
            }
            currentScore += 3;
          } else if (paramType->isReferenceType()) {
            const Type *pointee =
                static_cast<const ReferenceType *>(paramType)->getPointeeType();
            if (!pointee->isConstQualified()) {
              if (!isLValue) {
                currentScore += 1;
              } else {
                currentScore += 3;
              }
            } else {
              currentScore += 2;
            }
          } else {
            currentScore += 1;
          }
        }

        if (match && currentScore > bestScore) {
          bestScore = currentScore;
          bestMatch = fDecl;
        }
      }
    }
  }

  if (bestMatch) {
    size_t argsStartIdx = bestMatch->params.size() - args.size();
    for (size_t i = 0; i < args.size(); ++i) {
      const Type *paramType = bestMatch->params[argsStartIdx + i]->type;
      if (paramType->isReferenceType() && !args[i]->isLValue) {
        const Type *pointee =
            static_cast<const ReferenceType *>(paramType)->getPointeeType();
        if (!pointee->isConstQualified()) {
          (void)ctx->diags.report(
              {DiagLevel::Warning, args[i]->line, args[i]->column,
               args[i]->length,
               "Binding an r-value to non-const reference parameter '" +
                   std::string(bestMatch->params[argsStartIdx + i]->name) +
                   "' will implicitly create a stack-allocated temporary.",
               std::string(ctx->currentFile), args[i]->endLine});
        }
      }
    }
  }

  return bestMatch;
}

} // namespace utopia