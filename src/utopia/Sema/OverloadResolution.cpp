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
          if (m->params.size() == args.size()) {
            bool match = true;
            int currentScore = 0;

            for (size_t i = 0; i < args.size(); ++i) {
              const Type *argType = args[i]->exprType;
              const Type *paramType = m->params[i]->type;

              if (paramType->getKind() == TypeKind::Array) {
                paramType = ctx->astCtx.getPointerType(
                    static_cast<const ArrayType *>(paramType)
                        ->getElementType());
              }

              if (!canImplicitlyCast(argType, paramType)) {
                match = false;
                break;
              }

              if (canImplicitlyCast(argType, paramType, false)) {
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
                    static_cast<const ReferenceType *>(paramType)
                        ->getPointeeType();
                if (!pointee->isConstQualified()) {
                  if (!isLValue) {
                    match = false;
                    break;
                  }
                  currentScore += 3;
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
  }

  auto globalDecls = ctx->lookup(opFuncName);
  for (const auto *d : globalDecls) {
    if (d->kind == NodeKind::FunctionDecl) {
      auto *fDecl = static_cast<const FunctionDeclNode *>(d);
      if (fDecl->params.size() == 1 + args.size()) {
        bool match = true;
        int currentScore = 0;

        const Type *lhsParamType = fDecl->params[0]->type;
        if (!canImplicitlyCast(lhsType, lhsParamType)) {
          continue;
        }

        currentScore +=
            canImplicitlyCast(lhsType, lhsParamType, false) ? 10 : 1;

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

          if (canImplicitlyCast(argType, paramType, false)) {
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
                match = false;
                break;
              }
              currentScore += 3;
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

  return bestMatch;
}

} // namespace utopia