#include "utopia/CodeGen/CodeGen.hpp"
#include <iostream>
#include <llvm/ADT/APSInt.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/ModRef.h>
#include <optional>
#include <string>

namespace utopia {

llvm::Type *CodeGen::getLLVMType(const Type *type) {
  if (!type)
    return builder.getVoidTy();

  if (type->getKind() == TypeKind::Const) {
    return getLLVMType(static_cast<const ConstType *>(type)->getBaseType());
  }
  if (type->getKind() == TypeKind::Enum) {
    return getLLVMType(
        static_cast<const EnumType *>(type)->getUnderlyingType());
  }
  if (type->getKind() == TypeKind::Alias) {
    return getLLVMType(static_cast<const AliasType *>(type)->getTarget());
  }
  if (type->getKind() == TypeKind::Function) {
    auto *fTy = static_cast<const FunctionType *>(type);
    std::vector<llvm::Type *> paramTys;
    for (const auto *p : fTy->getParamTypes()) {
      paramTys.push_back(getLLVMType(p));
    }
    return llvm::FunctionType::get(getLLVMType(fTy->getReturnType()), paramTys,
                                   false);
  }

  /* Prevent undefined type structures from silently collapsing into LLVM
   * runtime faults */
  if (type->getKind() == TypeKind::TemplateParam) {
    diags.report({DiagLevel::Error, 0, 0, 0,
                  "Uninstantiated template parameter '" + type->toString() +
                      "' reached code generation.",
                  ""});
    return builder.getInt8Ty();
  }

  if (type->isBuiltinType()) {
    auto *bTy = static_cast<const BuiltinType *>(type);
    switch (bTy->getBuiltinKind()) {
    case BuiltinKind::Int8:
    case BuiltinKind::UInt8:
      return builder.getInt8Ty();
    case BuiltinKind::Int16:
    case BuiltinKind::UInt16:
      return builder.getInt16Ty();
    case BuiltinKind::Int32:
    case BuiltinKind::UInt32:
      return builder.getInt32Ty();
    case BuiltinKind::Int64:
    case BuiltinKind::UInt64:
      return builder.getInt64Ty();
    case BuiltinKind::Float32:
      return builder.getFloatTy();
    case BuiltinKind::Float64:
      return builder.getDoubleTy();
    case BuiltinKind::Bool:
      return builder.getInt1Ty();
    case BuiltinKind::Void:
      return builder.getVoidTy();
    }
  } else if (type->isPointerType() || type->isReferenceType() ||
             type->getKind() == TypeKind::RValueReference) {
    return builder.getPtrTy();
  } else if (type->getKind() == TypeKind::Array) {
    auto *arrTy = static_cast<const ArrayType *>(type);
    return llvm::ArrayType::get(getLLVMType(arrTy->getElementType()),
                                arrTy->getSize());
  } else if (type->getKind() == TypeKind::Struct ||
             type->getKind() == TypeKind::Class) {
    auto rec = static_cast<const RecordType *>(type);

    llvm::StructType *structTy =
        llvm::StructType::getTypeByName(ctx, rec->getName());

    /* Register the opaque type first to support self-referential structures
     * and recursive pointers gracefully without infinite loops. */
    if (!structTy) {
      structTy = llvm::StructType::create(ctx, rec->getName());
    }

    /* Materialize the body if it is currently opaque. An empty body is a valid
     * structural configuration in LLVM, so it must be committed even if fields
     * are empty. */
    if (structTy->isOpaque() && !rec->isOpaque()) {
      std::vector<llvm::Type *> elements;
      for (const auto &f : rec->getFields()) {
        elements.push_back(getLLVMType(f.type));
      }

      bool isPacked = false;
      if (const DeclNode *decl = rec->getDeclaration()) {
        for (const auto *ann : decl->annotations) {
          if (ann->name == "packed") {
            isPacked = true;
            break;
          }
        }
      }

      structTy->setBody(elements, isPacked);
    }

    return structTy;
  }

  diags.report({DiagLevel::Error, 0, 0, 0,
                "Unsupported or unresolved type reached code generation: " +
                    type->toString(),
                ""});
  return builder.getInt8Ty();
}

llvm::Value *CodeGen::createImplicitCast(llvm::Value *src, llvm::Type *destTy) {
  if (!src)
    return nullptr;
  llvm::Type *srcTy = src->getType();
  if (srcTy == destTy)
    return src;

  // Pointer cast support (e.g. T* to void* or void* to T*)
  if (srcTy->isPointerTy() && destTy->isPointerTy()) {
    return builder.CreateBitCast(src, destTy);
  }
  // Pointer to integer casts (reinterpret_cast equivalent)
  if (srcTy->isPointerTy() && destTy->isIntegerTy()) {
    return builder.CreatePtrToInt(src, destTy);
  }
  // Integer to pointer casts
  if (srcTy->isIntegerTy() && destTy->isPointerTy()) {
    return builder.CreateIntToPtr(src, destTy);
  }

  if (srcTy->isIntegerTy() && destTy->isIntegerTy()) {
    return builder.CreateIntCast(src, destTy, true);
  }
  if (srcTy->isFloatingPointTy() && destTy->isFloatingPointTy()) {
    return builder.CreateFPCast(src, destTy);
  }
  if (srcTy->isIntegerTy() && destTy->isFloatingPointTy()) {
    return builder.CreateSIToFP(src, destTy);
  }
  if (srcTy->isFloatingPointTy() && destTy->isIntegerTy()) {
    return builder.CreateFPToSI(src, destTy);
  }

  return builder.CreateBitCast(src, destTy);
}

llvm::Function *CodeGen::getOrCreateFunction(const FunctionDeclNode *node) {
  if (node->isTemplate)
    return nullptr;

  std::string irName = node->name == "main" ? "main" : node->mangledName;
  llvm::Function *func = mod.getFunction(irName);

  if (func)
    return func;

  std::vector<llvm::Type *> paramTypes;
  for (const auto *p : node->params) {
    paramTypes.push_back(getLLVMType(p->type));
  }

  llvm::FunctionType *funcType = llvm::FunctionType::get(
      getLLVMType(node->returnType), paramTypes, node->isVariadic);

  func = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                irName, mod);

  func->addFnAttr(llvm::Attribute::NoUnwind);

  for (const auto *ann : node->annotations) {
    if (ann->name == "inline")
      func->addFnAttr(llvm::Attribute::AlwaysInline);
    else if (ann->name == "readnone")
      func->setMemoryEffects(llvm::MemoryEffects::none());
    else if (ann->name == "readonly")
      func->setMemoryEffects(llvm::MemoryEffects::readOnly());
    else if (ann->name == "nosync")
      func->addFnAttr(llvm::Attribute::NoSync);
    else if (ann->name == "nofree")
      func->addFnAttr(llvm::Attribute::NoFree);
    else if (ann->name == "willreturn")
      func->addFnAttr(llvm::Attribute::WillReturn);
    else if (ann->name == "mustprogress")
      func->addFnAttr(llvm::Attribute::MustProgress);
  }

  if (!node->isExtern) {
    if (node->isReadNone)
      func->setMemoryEffects(llvm::MemoryEffects::none());
    else if (node->isReadOnly)
      func->setMemoryEffects(llvm::MemoryEffects::readOnly());

    if (node->isNoFree && !func->hasFnAttribute(llvm::Attribute::NoFree))
      func->addFnAttr(llvm::Attribute::NoFree);
    if (node->isNoSync && !func->hasFnAttribute(llvm::Attribute::NoSync))
      func->addFnAttr(llvm::Attribute::NoSync);
    if (node->isWillReturn &&
        !func->hasFnAttribute(llvm::Attribute::WillReturn))
      func->addFnAttr(llvm::Attribute::WillReturn);
    if (node->isMustProgress &&
        !func->hasFnAttribute(llvm::Attribute::MustProgress))
      func->addFnAttr(llvm::Attribute::MustProgress);
  }

  auto addRefAttributes = [&](const Type *type,
                              std::optional<unsigned> paramIdx,
                              llvm::ArrayRef<AnnotationNode *> annotations) {
    auto addAttrObj = [&](llvm::Attribute attr) {
      if (paramIdx.has_value())
        func->addParamAttr(*paramIdx, attr);
      else
        func->addRetAttr(attr);
    };

    for (const auto *ann : annotations) {
      if (ann->name == "nocapture" && paramIdx.has_value()) {
        addAttrObj(llvm::Attribute::getWithCaptureInfo(
            ctx, llvm::CaptureInfo::none()));
      } else if (ann->name == "nonnull") {
        addAttrObj(llvm::Attribute::get(ctx, llvm::Attribute::NonNull));
      } else if (ann->name == "dereferenceable" && !ann->args.empty()) {
        if (ann->args[0]->kind == NodeKind::Number) {
          auto *num = static_cast<const NumberNode *>(ann->args[0]);
          if (!num->isFloat) {
            uint64_t sz = std::stoull(std::string(num->raw));
            addAttrObj(llvm::Attribute::getWithDereferenceableBytes(ctx, sz));
          }
        }
      }
    }

    if (!type->isReferenceType() && !type->isPointerType() &&
        type->getKind() != TypeKind::RValueReference)
      return;

    if (type->isReferenceType() ||
        type->getKind() == TypeKind::RValueReference) {
      addAttrObj(llvm::Attribute::get(ctx, llvm::Attribute::NonNull));
      addAttrObj(llvm::Attribute::get(ctx, llvm::Attribute::NoUndef));
    }

    const Type *pointee = nullptr;
    if (type->isReferenceType()) {
      pointee = static_cast<const ReferenceType *>(type)->getPointeeType();
    } else if (type->getKind() == TypeKind::RValueReference) {
      pointee =
          static_cast<const RValueReferenceType *>(type)->getPointeeType();
    } else {
      pointee = static_cast<const PointerType *>(type)->getPointeeType();
    }

    if (pointee->isConstQualified() && paramIdx.has_value()) {
      addAttrObj(llvm::Attribute::get(ctx, llvm::Attribute::ReadOnly));
      addAttrObj(
          llvm::Attribute::getWithCaptureInfo(ctx, llvm::CaptureInfo::none()));
    }

    const Type *unqualPointee = pointee->getUnqualifiedType();

    if (unqualPointee->isBuiltinType() &&
        (type->isReferenceType() ||
         type->getKind() == TypeKind::RValueReference)) {
      auto kind =
          static_cast<const BuiltinType *>(unqualPointee)->getBuiltinKind();
      uint64_t size = 0;
      switch (kind) {
      case BuiltinKind::Int8:
      case BuiltinKind::UInt8:
      case BuiltinKind::Bool:
        size = 1;
        break;
      case BuiltinKind::Int16:
      case BuiltinKind::UInt16:
        size = 2;
        break;
      case BuiltinKind::Int32:
      case BuiltinKind::UInt32:
      case BuiltinKind::Float32:
        size = 4;
        break;
      case BuiltinKind::Int64:
      case BuiltinKind::UInt64:
      case BuiltinKind::Float64:
        size = 8;
        break;
      case BuiltinKind::Void:
      default:
        break;
      }

      if (size > 0) {
        addAttrObj(llvm::Attribute::getWithDereferenceableBytes(ctx, size));
        addAttrObj(llvm::Attribute::getWithAlignment(ctx, llvm::Align(size)));
      }
    }
  };

  addRefAttributes(node->returnType, std::nullopt, node->annotations);

  unsigned argIdx = 0;
  for (const auto *param : node->params) {
    addRefAttributes(param->type, argIdx, param->annotations);
    argIdx++;
  }

  return func;
}

llvm::Value *CodeGen::visit(const AnnotationDeclNode *node) {
  if (node->recordType) {
    getLLVMType(node->recordType);
  }
  if (node->constructor) {
    dispatch(node->constructor);
  }
  return nullptr;
}

llvm::Value *CodeGen::visit(const TypedefDeclNode *node) { return nullptr; }

llvm::Value *CodeGen::visit(const AnnotationNode *node) { return nullptr; }

void CodeGen::emitConstructorCall(const FunctionCallNode *node,
                                  llvm::Value *targetAddr) {
  if (!node->resolvedFunc)
    return;

  emitDefaultInitialization(targetAddr, node->exprType);

  llvm::Function *func = getOrCreateFunction(node->resolvedFunc);

  std::vector<llvm::Value *> argsArgs;
  argsArgs.push_back(targetAddr);

  unsigned argIdx = 1;
  for (const auto &arg : node->args) {
    llvm::Value *argVal = nullptr;

    bool isRefParam = false;
    if (node->resolvedFunc && argIdx < node->resolvedFunc->params.size()) {
      isRefParam =
          node->resolvedFunc->params[argIdx]->type->isReferenceType() ||
          node->resolvedFunc->params[argIdx]->type->getKind() ==
              TypeKind::RValueReference;
    }

    if (isRefParam) {
      argVal = getLValue(arg);
      if (!argVal) {
        llvm::Value *val = dispatch(arg);
        argVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
        builder.CreateStore(val, argVal);
      }
    } else {
      argVal = dispatch(arg);
      if (func && argVal && argIdx < func->arg_size()) {
        llvm::Type *paramTy = func->getFunctionType()->getParamType(argIdx);
        argVal = createImplicitCast(argVal, paramTy);
      }
    }

    if (!argVal) {
      diags.report({DiagLevel::Error, arg->line, arg->column, arg->length,
                    "Failed to evaluate argument for constructor call.", ""});
      return;
    }
    argsArgs.push_back(argVal);
    argIdx++;
  }

  builder.CreateCall(func, argsArgs);
}

llvm::Constant *CodeGen::evaluateAsConstant(const ExprNode *node) {
  if (!node)
    return nullptr;

  if (node->kind == NodeKind::Null) {
    return llvm::ConstantPointerNull::get(builder.getPtrTy());
  }

  if (node->kind == NodeKind::Boolean) {
    auto *boolNode = static_cast<const BoolNode *>(node);
    return boolNode->value ? llvm::ConstantInt::getTrue(ctx)
                           : llvm::ConstantInt::getFalse(ctx);
  }

  if (node->kind == NodeKind::Number) {
    auto *num = static_cast<const NumberNode *>(node);
    std::string numStr(num->raw);

    while (!numStr.empty() && !std::isdigit(numStr.back()) &&
           numStr.back() != '.') {
      numStr.pop_back();
    }

    llvm::Type *llvmTy = num->exprType ? getLLVMType(num->exprType) : nullptr;

    if (num->isFloat) {
      if (llvmTy && (llvmTy->isFloatTy() || llvmTy->isDoubleTy())) {
        return llvm::ConstantFP::get(llvmTy, llvm::StringRef(numStr));
      }
      return llvm::ConstantFP::get(builder.getDoubleTy(),
                                   llvm::StringRef(numStr));
    } else {
      if (llvmTy && llvmTy->isIntegerTy()) {
        auto *intTy = llvm::cast<llvm::IntegerType>(llvmTy);
        return llvm::ConstantInt::get(intTy, llvm::StringRef(numStr), 10);
      }
      return llvm::ConstantInt::get(builder.getInt32Ty(),
                                    llvm::StringRef(numStr), 10);
    }
  }

  if (node->kind == NodeKind::Char) {
    auto *charNode = static_cast<const CharNode *>(node);
    return llvm::ConstantInt::get(builder.getInt8Ty(), charNode->value);
  }

  if (node->kind == NodeKind::Rune) {
    auto *runeNode = static_cast<const RuneNode *>(node);
    return llvm::ConstantInt::get(builder.getInt32Ty(), runeNode->value);
  }

  if (node->kind == NodeKind::Variable) {
    auto *varNode = static_cast<const VariableNode *>(node);
    SymbolInfo sym = cgCtx.lookupDetailed(varNode->name);

    if (sym.value) {
      return llvm::dyn_cast<llvm::Constant>(sym.value);
    }

    return nullptr;
  }

  if (node->kind == NodeKind::MemberAccess) {
    auto *maNode = static_cast<const MemberAccessNode *>(node);
    if (maNode->isEnumMember) {
      llvm::Type *llTy = getLLVMType(maNode->exprType);
      return llvm::ConstantInt::get(llTy, maNode->enumMember->evaluatedValue,
                                    true);
    }
    return nullptr;
  }

  if (node->kind == NodeKind::UnaryOp) {
    auto *unNode = static_cast<const UnaryOpNode *>(node);

    if (unNode->op == "&" && unNode->expr->kind == NodeKind::Variable) {
      auto *varNode = static_cast<const VariableNode *>(unNode->expr);
      SymbolInfo sym = cgCtx.lookupDetailed(varNode->name);
      if (sym.value && llvm::isa<llvm::Constant>(sym.value)) {
        return llvm::cast<llvm::Constant>(sym.value);
      }
    } else if (unNode->op == "!") {
      llvm::Constant *innerConst = evaluateAsConstant(unNode->expr);
      if (innerConst && innerConst->getType()->isIntegerTy(1)) {
        if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(innerConst)) {
          return llvm::ConstantInt::get(builder.getInt1Ty(),
                                        !ci->getZExtValue());
        }
      }
    } else if (unNode->op == "~") {
      llvm::Constant *innerConst = evaluateAsConstant(unNode->expr);
      if (innerConst && innerConst->getType()->isIntegerTy()) {
        if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(innerConst)) {
          return llvm::ConstantInt::get(ctx, ~ci->getValue());
        }
      }
    } else if (unNode->op == "-" || unNode->op == "+") {
      llvm::Constant *innerConst = evaluateAsConstant(unNode->expr);
      if (innerConst) {
        if (unNode->op == "+")
          return innerConst;

        if (innerConst->getType()->isFloatingPointTy()) {
          if (auto *cfp = llvm::dyn_cast<llvm::ConstantFP>(innerConst)) {
            llvm::APFloat apf = cfp->getValueAPF();
            apf.changeSign();
            return llvm::ConstantFP::get(ctx, apf);
          }
        } else if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(innerConst)) {
          return llvm::ConstantInt::get(ctx, -ci->getValue());
        }
      }
    }
  }

  if (node->kind == NodeKind::BinaryOp) {
    auto *binNode = static_cast<const BinaryOpNode *>(node);
    llvm::Constant *L = evaluateAsConstant(binNode->left);
    llvm::Constant *R = evaluateAsConstant(binNode->right);

    if (L && R) {
      if (auto *ciL = llvm::dyn_cast<llvm::ConstantInt>(L)) {
        if (auto *ciR = llvm::dyn_cast<llvm::ConstantInt>(R)) {
          llvm::APInt vL = ciL->getValue();
          llvm::APInt vR = ciR->getValue();
          bool isUnsigned = false;

          if (binNode->left->exprType && binNode->left->exprType->isInteger()) {
            auto bKind = static_cast<const BuiltinType *>(
                             binNode->left->exprType->getUnqualifiedType())
                             ->getBuiltinKind();
            isUnsigned =
                (bKind == BuiltinKind::UInt8 || bKind == BuiltinKind::UInt16 ||
                 bKind == BuiltinKind::UInt32 || bKind == BuiltinKind::UInt64);
          }

          if (binNode->op == "+")
            return llvm::ConstantInt::get(ctx, vL + vR);
          if (binNode->op == "-")
            return llvm::ConstantInt::get(ctx, vL - vR);
          if (binNode->op == "*")
            return llvm::ConstantInt::get(ctx, vL * vR);
          if (binNode->op == "/") {
            if (vR.isZero())
              return nullptr;
            return llvm::ConstantInt::get(ctx, isUnsigned ? vL.udiv(vR)
                                                          : vL.sdiv(vR));
          }
          if (binNode->op == "%") {
            if (vR.isZero())
              return nullptr;
            return llvm::ConstantInt::get(ctx, isUnsigned ? vL.urem(vR)
                                                          : vL.srem(vR));
          }
          if (binNode->op == "&")
            return llvm::ConstantInt::get(ctx, vL & vR);
          if (binNode->op == "|")
            return llvm::ConstantInt::get(ctx, vL | vR);
          if (binNode->op == "^")
            return llvm::ConstantInt::get(ctx, vL ^ vR);
          if (binNode->op == "<<")
            return llvm::ConstantInt::get(ctx, vL.shl(vR));
          if (binNode->op == ">>")
            return llvm::ConstantInt::get(ctx, isUnsigned ? vL.lshr(vR)
                                                          : vL.ashr(vR));

          if (binNode->op == "&&")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          (vL != 0) && (vR != 0));
          if (binNode->op == "||")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          (vL != 0) || (vR != 0));
          if (binNode->op == "==")
            return llvm::ConstantInt::get(builder.getInt1Ty(), vL == vR);
          if (binNode->op == "!=")
            return llvm::ConstantInt::get(builder.getInt1Ty(), vL != vR);
          if (binNode->op == "<")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          isUnsigned ? vL.ult(vR) : vL.slt(vR));
          if (binNode->op == "<=")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          isUnsigned ? vL.ule(vR) : vL.sle(vR));
          if (binNode->op == ">")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          isUnsigned ? vL.ugt(vR) : vL.sgt(vR));
          if (binNode->op == ">=")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          isUnsigned ? vL.uge(vR) : vL.sge(vR));
        }
      } else if (auto *cfpL = llvm::dyn_cast<llvm::ConstantFP>(L)) {
        if (auto *cfpR = llvm::dyn_cast<llvm::ConstantFP>(R)) {
          llvm::APFloat vL = cfpL->getValueAPF();
          llvm::APFloat vR = cfpR->getValueAPF();

          if (binNode->op == "+") {
            vL.add(vR, llvm::APFloat::rmNearestTiesToEven);
            return llvm::ConstantFP::get(ctx, vL);
          }
          if (binNode->op == "-") {
            vL.subtract(vR, llvm::APFloat::rmNearestTiesToEven);
            return llvm::ConstantFP::get(ctx, vL);
          }
          if (binNode->op == "*") {
            vL.multiply(vR, llvm::APFloat::rmNearestTiesToEven);
            return llvm::ConstantFP::get(ctx, vL);
          }
          if (binNode->op == "/") {
            vL.divide(vR, llvm::APFloat::rmNearestTiesToEven);
            return llvm::ConstantFP::get(ctx, vL);
          }
          if (binNode->op == "%") {
            vL.mod(vR);
            return llvm::ConstantFP::get(ctx, vL);
          }

          auto cmp = vL.compare(vR);
          if (binNode->op == "==")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          cmp == llvm::APFloat::cmpEqual);
          if (binNode->op == "!=")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          cmp != llvm::APFloat::cmpEqual);
          if (binNode->op == "<")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          cmp == llvm::APFloat::cmpLessThan);
          if (binNode->op == "<=")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          cmp == llvm::APFloat::cmpLessThan ||
                                              cmp == llvm::APFloat::cmpEqual);
          if (binNode->op == ">")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          cmp == llvm::APFloat::cmpGreaterThan);
          if (binNode->op == ">=")
            return llvm::ConstantInt::get(
                builder.getInt1Ty(), cmp == llvm::APFloat::cmpGreaterThan ||
                                         cmp == llvm::APFloat::cmpEqual);
        }
      }
    }
  }

  return nullptr;
}

llvm::Value *CodeGen::getLValue(const ExprNode *node) {
  if (!node)
    return nullptr;

  if (node->kind == NodeKind::ArraySubscript) {
    auto *subNode = static_cast<const ArraySubscriptNode *>(node);
    llvm::Value *baseVal = getLValue(subNode->base);
    llvm::Value *idxVal = dispatch(subNode->index);
    if (!baseVal || !idxVal)
      return nullptr;

    const Type *unqualBase = subNode->base->exprType->getUnqualifiedType();

    if (unqualBase->getKind() == TypeKind::Array) {
      llvm::Type *llvmBaseTy = getLLVMType(unqualBase);
      return builder.CreateInBoundsGEP(llvmBaseTy, baseVal,
                                       {builder.getInt32(0), idxVal});
    } else if (unqualBase->isPointerType()) {
      baseVal = dispatch(subNode->base);
      llvm::Type *pointeeTy = getLLVMType(
          static_cast<const PointerType *>(unqualBase)->getPointeeType());
      return builder.CreateInBoundsGEP(pointeeTy, baseVal, idxVal);
    }
    return nullptr;
  }

  if (node->kind == NodeKind::Variable) {
    auto *varNode = static_cast<const VariableNode *>(node);

    if (varNode->isField) {
      SymbolInfo sym = cgCtx.lookupDetailed("this");

      llvm::Value *thisAddr = sym.value;
      llvm::Value *thisPtr =
          builder.CreateLoad(builder.getPtrTy(), thisAddr, "this.val");
      llvm::Type *llvmBaseTy = getLLVMType(varNode->parentType);

      return builder.CreateStructGEP(llvmBaseTy, thisPtr, varNode->fieldIndex,
                                     varNode->name);
    }

    std::string lookupName = std::string(varNode->name);
    if (varNode->resolvedDecl &&
        varNode->resolvedDecl->kind == NodeKind::VarDecl) {
      auto *varDecl = static_cast<const VarDeclNode *>(varNode->resolvedDecl);
      if (varDecl->isStatic && !varDecl->mangledName.empty()) {
        lookupName = varDecl->mangledName;
      }
    }

    SymbolInfo sym = cgCtx.lookupDetailed(lookupName);

    if (!sym.value) {
      std::cerr << "[CodeGen Error] Unbound symbol in l-value resolution: '"
                << lookupName << "'.\n";
      return nullptr;
    }

    if (!sym.isDirectAddress) {
      return builder.CreateLoad(builder.getPtrTy(), sym.value, "indirect.ref");
    }
    return sym.value;
  }

  if (node->kind == NodeKind::MemberAccess) {
    auto *maNode = static_cast<const MemberAccessNode *>(node);
    if (maNode->isMethodRef)
      return nullptr;

    if (maNode->isStaticFieldRef) {
      std::string gName = maNode->resolvedVar->mangledName.empty()
                              ? std::string(maNode->resolvedVar->varName)
                              : maNode->resolvedVar->mangledName;
      SymbolInfo sym = cgCtx.lookupDetailed(gName);
      if (!sym.value) {
        diags.report({DiagLevel::Error, node->line, node->column, node->length,
                      "Unbound static field.", ""});
        return nullptr;
      }
      return sym.value;
    }

    llvm::Value *objPtr = nullptr;
    if (maNode->object->exprType->isPointerType()) {
      objPtr = dispatch(maNode->object);
    } else {
      objPtr = getLValue(maNode->object);
    }

    if (!objPtr)
      return nullptr;

    const Type *baseTy = maNode->object->exprType;
    if (baseTy->isPointerType())
      baseTy = static_cast<const PointerType *>(baseTy)->getPointeeType();
    else if (baseTy->isReferenceType() ||
             baseTy->getKind() == TypeKind::RValueReference)
      baseTy = static_cast<const ReferenceType *>(baseTy)->getPointeeType();

    llvm::Type *llvmBaseTy = getLLVMType(baseTy);
    return builder.CreateStructGEP(llvmBaseTy, objPtr, maNode->fieldIndex,
                                   maNode->memberName);
  }

  if (node->kind == NodeKind::UnaryOp) {
    auto *unNode = static_cast<const UnaryOpNode *>(node);
    if (unNode->op == "*") {
      return dispatch(unNode->expr);
    }
  }

  if (node->exprType &&
      (node->exprType->isReferenceType() ||
       node->exprType->getKind() == TypeKind::RValueReference)) {
    return dispatch(node);
  }

  return nullptr;
}

llvm::Value *CodeGen::visit(const BlockNode *node) {
  CGScopeGuard guard(cgCtx);

  for (const auto *stmt : node->statements) {
    dispatch(stmt);
    if (builder.GetInsertBlock()->getTerminator()) {
      break;
    }
  }

  if (!builder.GetInsertBlock()->getTerminator()) {
    emitScopeCleanups();
  }

  return nullptr;
}

llvm::Value *CodeGen::visit(const NumberNode *node) {
  std::string numStr(node->raw);
  while (!numStr.empty() && !std::isdigit(numStr.back()) &&
         numStr.back() != '.') {
    numStr.pop_back();
  }

  llvm::Type *llvmTy = node->exprType ? getLLVMType(node->exprType) : nullptr;

  if (node->isFloat) {
    if (llvmTy && (llvmTy->isFloatTy() || llvmTy->isDoubleTy())) {
      return llvm::ConstantFP::get(llvmTy, llvm::StringRef(numStr));
    }
    return llvm::ConstantFP::get(ctx, llvm::APFloat(std::stod(numStr)));
  }

  if (llvmTy && llvmTy->isIntegerTy()) {
    auto *intTy = llvm::cast<llvm::IntegerType>(llvmTy);
    return llvm::ConstantInt::get(intTy, llvm::StringRef(numStr), 10);
  }
  return llvm::ConstantInt::get(builder.getInt32Ty(), llvm::StringRef(numStr),
                                10);
}

llvm::Value *CodeGen::visit(const BoolNode *node) {
  if (node->value)
    return llvm::ConstantInt::getTrue(ctx);
  return llvm::ConstantInt::getFalse(ctx);
}

llvm::Value *CodeGen::visit(const CharNode *node) {
  return llvm::ConstantInt::get(builder.getInt8Ty(), node->value);
}

llvm::Value *CodeGen::visit(const RuneNode *node) {
  return llvm::ConstantInt::get(builder.getInt32Ty(), node->value);
}

llvm::Value *CodeGen::visit(const StringNode *node) {
  return builder.CreateGlobalString(
      llvm::StringRef(node->value.data(), node->value.length()), ".str");
}

llvm::Value *CodeGen::visit(const StructDeclNode *node) {
  if (node->isTemplate)
    return nullptr;

  if (node->recordType) {
    getLLVMType(node->recordType);
  }

  if (node->isOpaque)
    return nullptr;

  for (const auto *field : node->fields) {
    if (field->isStatic)
      dispatch(field);
  }

  for (const auto *ctor : node->constructors)
    dispatch(ctor);
  if (node->destructor)
    dispatch(node->destructor);

  for (const auto *method : node->methods) {
    dispatch(method);
  }

  return nullptr;
}

llvm::Value *CodeGen::visit(const ClassDeclNode *node) {
  if (node->isTemplate)
    return nullptr;

  if (node->recordType) {
    getLLVMType(node->recordType);
  }

  if (node->isOpaque)
    return nullptr;

  for (const auto *field : node->fields) {
    if (field->isStatic)
      dispatch(field);
  }

  for (const auto *ctor : node->constructors)
    dispatch(ctor);
  if (node->destructor)
    dispatch(node->destructor);

  for (const auto *method : node->methods) {
    dispatch(method);
  }

  return nullptr;
}

llvm::Value *CodeGen::visit(const EnumDeclNode *node) { return nullptr; }

llvm::Value *CodeGen::visit(const EnumMemberNode *node) { return nullptr; }

llvm::Value *CodeGen::visit(const VariableNode *node) {
  if (node->isField) {
    SymbolInfo sym = cgCtx.lookupDetailed("this");

    llvm::Value *thisAddr = sym.value;
    llvm::Value *thisPtr =
        builder.CreateLoad(builder.getPtrTy(), thisAddr, "this.val");

    llvm::Type *llvmBaseTy = getLLVMType(node->parentType);
    llvm::Value *gep = builder.CreateStructGEP(llvmBaseTy, thisPtr,
                                               node->fieldIndex, node->name);
    return createTBAALoad(getLLVMType(node->exprType), gep,
                          getTBAATagForExpr(node), node->name);
  }

  /* Return direct pointer evaluation if identifier statically targets a
   * function declaration */
  if (node->resolvedDecl &&
      node->resolvedDecl->kind == NodeKind::FunctionDecl) {
    return getOrCreateFunction(
        static_cast<const FunctionDeclNode *>(node->resolvedDecl));
  }

  llvm::Value *lval = getLValue(node);
  if (!lval)
    return nullptr;

  /* Automatic array-to-pointer decay */
  if (node->exprType->getKind() == TypeKind::Array) {
    llvm::Type *arrTy = getLLVMType(node->exprType);
    return builder.CreateInBoundsGEP(
        arrTy, lval, {builder.getInt32(0), builder.getInt32(0)});
  }

  const Type *loadTy = node->exprType;
  if (loadTy->isReferenceType() ||
      loadTy->getKind() == TypeKind::RValueReference) {
    loadTy = static_cast<const ReferenceType *>(loadTy)->getPointeeType();
  }

  llvm::Type *llvmTy = getLLVMType(loadTy);
  return createTBAALoad(llvmTy, lval, loadTy, node->name);
}

llvm::Value *CodeGen::visit(const MemberAccessNode *node) {
  if (node->isEnumMember) {
    llvm::Type *llTy = getLLVMType(node->exprType);
    return llvm::ConstantInt::get(llTy, node->enumMember->evaluatedValue, true);
  }

  if (node->isMethodRef)
    return nullptr;

  if (node->isStaticFieldRef) {
    std::string gName = node->resolvedVar->mangledName.empty()
                            ? std::string(node->resolvedVar->varName)
                            : node->resolvedVar->mangledName;
    SymbolInfo sym = cgCtx.lookupDetailed(gName);
    if (!sym.value)
      return nullptr;
    return createTBAALoad(getLLVMType(node->exprType), sym.value,
                          getTBAATagForExpr(node), node->memberName);
  }

  llvm::Value *objPtr = nullptr;
  if (node->object->exprType->isPointerType()) {
    objPtr = dispatch(node->object);
  } else {
    objPtr = getLValue(node->object);
  }

  if (!objPtr)
    return nullptr;

  const Type *baseTy = node->object->exprType;
  if (baseTy->isPointerType())
    baseTy = static_cast<const PointerType *>(baseTy)->getPointeeType();
  else if (baseTy->isReferenceType() ||
           baseTy->getKind() == TypeKind::RValueReference)
    baseTy = static_cast<const ReferenceType *>(baseTy)->getPointeeType();

  llvm::Type *llvmBaseTy = getLLVMType(baseTy);
  llvm::Value *gep = builder.CreateStructGEP(
      llvmBaseTy, objPtr, node->fieldIndex, node->memberName);

  /* Automatic array-to-pointer decay on record fields */
  if (node->exprType->getKind() == TypeKind::Array) {
    llvm::Type *arrTy = getLLVMType(node->exprType);
    return builder.CreateInBoundsGEP(
        arrTy, gep, {builder.getInt32(0), builder.getInt32(0)});
  }

  return createTBAALoad(getLLVMType(node->exprType), gep,
                        getTBAATagForExpr(node), node->memberName);
}

llvm::Value *CodeGen::visit(const IfNode *node) {
  llvm::Value *condV = dispatch(node->condition);
  if (!condV)
    return nullptr;

  condV = createImplicitCast(condV, builder.getInt1Ty());

  llvm::Function *theFunction = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *thenBB = llvm::BasicBlock::Create(ctx, "then", theFunction);
  llvm::BasicBlock *elseBB = llvm::BasicBlock::Create(ctx, "else");
  llvm::BasicBlock *mergeBB = llvm::BasicBlock::Create(ctx, "ifcont");

  if (node->elseBlock) {
    builder.CreateCondBr(condV, thenBB, elseBB);
  } else {
    builder.CreateCondBr(condV, thenBB, mergeBB);
  }

  builder.SetInsertPoint(thenBB);
  dispatch(node->thenBlock);
  if (!builder.GetInsertBlock()->getTerminator()) {
    builder.CreateBr(mergeBB);
  }

  if (node->elseBlock) {
    theFunction->insert(theFunction->end(), elseBB);
    builder.SetInsertPoint(elseBB);
    dispatch(node->elseBlock);
    if (!builder.GetInsertBlock()->getTerminator()) {
      builder.CreateBr(mergeBB);
    }
  } else {
    /* Cleanup orphaned block explicitly */
    delete elseBB;
  }

  theFunction->insert(theFunction->end(), mergeBB);
  builder.SetInsertPoint(mergeBB);

  return nullptr;
}

llvm::Value *CodeGen::visit(const ForNode *node) {
  CGScopeGuard guard(cgCtx);

  if (node->initStatement) {
    dispatch(node->initStatement);
  }

  llvm::Function *theFunction = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *condBB =
      llvm::BasicBlock::Create(ctx, "for.cond", theFunction);
  llvm::BasicBlock *bodyBB = llvm::BasicBlock::Create(ctx, "for.body");
  llvm::BasicBlock *incBB = llvm::BasicBlock::Create(ctx, "for.inc");
  llvm::BasicBlock *endBB = llvm::BasicBlock::Create(ctx, "for.end");

  builder.CreateBr(condBB);
  builder.SetInsertPoint(condBB);

  if (node->condition) {
    llvm::Value *condV = dispatch(node->condition);
    if (!condV)
      return nullptr;
    condV = createImplicitCast(condV, builder.getInt1Ty());
    builder.CreateCondBr(condV, bodyBB, endBB);
  } else {
    builder.CreateBr(bodyBB);
  }

  theFunction->insert(theFunction->end(), bodyBB);
  builder.SetInsertPoint(bodyBB);
  dispatch(node->body);
  if (!builder.GetInsertBlock()->getTerminator()) {
    builder.CreateBr(incBB);
  }

  theFunction->insert(theFunction->end(), incBB);
  builder.SetInsertPoint(incBB);
  if (node->increment) {
    dispatch(node->increment);
  }
  builder.CreateBr(condBB);

  theFunction->insert(theFunction->end(), endBB);
  builder.SetInsertPoint(endBB);

  return nullptr;
}

llvm::Value *CodeGen::visit(const WhileNode *node) {
  llvm::Function *theFunction = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *condBB =
      llvm::BasicBlock::Create(ctx, "while.cond", theFunction);
  llvm::BasicBlock *bodyBB = llvm::BasicBlock::Create(ctx, "while.body");
  llvm::BasicBlock *endBB = llvm::BasicBlock::Create(ctx, "while.end");

  builder.CreateBr(condBB);
  builder.SetInsertPoint(condBB);

  llvm::Value *condV = dispatch(node->condition);
  if (!condV)
    return nullptr;
  condV = createImplicitCast(condV, builder.getInt1Ty());
  builder.CreateCondBr(condV, bodyBB, endBB);

  theFunction->insert(theFunction->end(), bodyBB);
  builder.SetInsertPoint(bodyBB);
  dispatch(node->body);
  if (!builder.GetInsertBlock()->getTerminator()) {
    builder.CreateBr(condBB);
  }

  theFunction->insert(theFunction->end(), endBB);
  builder.SetInsertPoint(endBB);

  return nullptr;
}

llvm::Value *CodeGen::visit(const UnaryOpNode *node) {
  if (node->overloadedOperator) {
    llvm::Value *objPtr = nullptr;
    if (node->expr->exprType->isPointerType()) {
      objPtr = dispatch(node->expr);
    } else {
      objPtr = getLValue(node->expr);
    }

    if (!objPtr) {
      llvm::Value *val = dispatch(node->expr);
      objPtr = createEntryBlockAlloca(val->getType(), "tmp.op.recv");
      builder.CreateStore(val, objPtr);
    }

    llvm::Value *oldVal = nullptr;
    if (node->isPostfix && (node->op == "++" || node->op == "--")) {
      llvm::Type *valTy = getLLVMType(node->expr->exprType);
      oldVal =
          createTBAALoad(valTy, objPtr, node->expr->exprType, "postfix.old");
    }

    llvm::Function *func = getOrCreateFunction(node->overloadedOperator);
    llvm::Value *res = builder.CreateCall(func, {objPtr});

    if (node->isPostfix && oldVal) {
      return oldVal;
    }
    return res;
  }

  if (node->op == "!") {
    llvm::Value *val = dispatch(node->expr);
    if (!val)
      return nullptr;
    return builder.CreateNot(val);
  }
  if (node->op == "~") {
    llvm::Value *val = dispatch(node->expr);
    if (!val)
      return nullptr;
    return builder.CreateNot(val);
  }
  if (node->op == "&") {
    return getLValue(node->expr);
  }
  if (node->op == "*") {
    llvm::Value *ptr = dispatch(node->expr);
    if (!ptr) {
      diags.report({DiagLevel::Error, node->line, node->column, node->length,
                    "Invalid operand for dereference.", ""});
      return nullptr;
    }
    llvm::Type *loadTy = getLLVMType(node->exprType);
    return createTBAALoad(loadTy, ptr, node->exprType);
  }
  if (node->op == "-") {
    llvm::Value *val = dispatch(node->expr);
    if (!val) {
      diags.report({DiagLevel::Error, node->line, node->column, node->length,
                    "Invalid operand for unary minus.", ""});
      return nullptr;
    }
    if (val->getType()->isFloatingPointTy()) {
      return builder.CreateFNeg(val);
    } else {
      return builder.CreateNeg(val);
    }
  }
  if (node->op == "+") {
    return dispatch(node->expr);
  }
  if (node->op == "++" || node->op == "--") {
    llvm::Value *lval = getLValue(node->expr);
    if (!lval) {
      diags.report(
          {DiagLevel::Error, node->line, node->column, node->length,
           "Invalid operand for " + std::string(node->op) + " operator.", ""});
      return nullptr;
    }

    llvm::Type *valTy = getLLVMType(node->exprType);
    llvm::Value *oldVal = createTBAALoad(valTy, lval, node->exprType);
    llvm::Value *newVal = nullptr;

    bool isFloat = valTy->isFloatingPointTy();
    if (node->op == "++") {
      if (isFloat)
        newVal = builder.CreateFAdd(oldVal, llvm::ConstantFP::get(valTy, 1.0));
      else
        newVal = builder.CreateAdd(oldVal, llvm::ConstantInt::get(valTy, 1));
    } else {
      if (isFloat)
        newVal = builder.CreateFSub(oldVal, llvm::ConstantFP::get(valTy, 1.0));
      else
        newVal = builder.CreateSub(oldVal, llvm::ConstantInt::get(valTy, 1));
    }

    createTBAAStore(newVal, lval, getTBAATagForExpr(node->expr));

    return node->isPostfix ? oldVal : newVal;
  }

  return nullptr;
}

llvm::Value *CodeGen::visit(const BinaryOpNode *node) {
  if (node->overloadedOperator) {
    llvm::Value *objPtr = nullptr;
    if (node->left->exprType->isPointerType()) {
      objPtr = dispatch(node->left);
    } else {
      objPtr = getLValue(node->left);
    }

    if (!objPtr) {
      llvm::Value *val = dispatch(node->left);
      objPtr = createEntryBlockAlloca(val->getType(), "tmp.op.recv");
      builder.CreateStore(val, objPtr);
    }

    llvm::Function *func = getOrCreateFunction(node->overloadedOperator);
    std::vector<llvm::Value *> argsArgs;
    argsArgs.push_back(objPtr);

    llvm::Value *rhsVal = nullptr;
    bool isRefParam = false;
    if (node->overloadedOperator->params.size() > 1) {
      isRefParam =
          node->overloadedOperator->params[1]->type->isReferenceType() ||
          node->overloadedOperator->params[1]->type->getKind() ==
              TypeKind::RValueReference;
    }

    if (isRefParam) {
      rhsVal = getLValue(node->right);
      if (!rhsVal) {
        llvm::Value *val = dispatch(node->right);
        rhsVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
        builder.CreateStore(val, rhsVal);
      }
    } else {
      rhsVal = dispatch(node->right);
      llvm::Type *paramTy =
          getLLVMType(node->overloadedOperator->params[1]->type);
      rhsVal = createImplicitCast(rhsVal, paramTy);
    }
    argsArgs.push_back(rhsVal);

    return builder.CreateCall(func, argsArgs);
  }

  llvm::Value *L = dispatch(node->left);
  llvm::Value *R = dispatch(node->right);

  if (!L || !R) {
    diags.report({DiagLevel::Error, node->line, node->column, node->length,
                  "Failed to generate binary operands.", ""});
    return nullptr;
  }

  if (node->op == "&&") {
    L = createImplicitCast(L, builder.getInt1Ty());
    R = createImplicitCast(R, builder.getInt1Ty());
    return builder.CreateLogicalAnd(L, R);
  }
  if (node->op == "||") {
    L = createImplicitCast(L, builder.getInt1Ty());
    R = createImplicitCast(R, builder.getInt1Ty());
    return builder.CreateLogicalOr(L, R);
  }

  if (node->promotedType) {
    llvm::Type *promotedLLVMTy = getLLVMType(node->promotedType);
    if (L->getType() != promotedLLVMTy)
      L = createImplicitCast(L, promotedLLVMTy);
    if (R->getType() != promotedLLVMTy)
      R = createImplicitCast(R, promotedLLVMTy);
  } else if (L->getType() != R->getType()) {
    if (L->getType()->isPointerTy())
      R = builder.CreateBitCast(R, L->getType());
    else if (R->getType()->isPointerTy())
      L = builder.CreateBitCast(L, R->getType());
  }

  bool isFloat = L->getType()->isFloatingPointTy();
  bool isUnsigned = false;

  const Type *evalType =
      node->promotedType ? node->promotedType : node->left->exprType;
  if (evalType && evalType->isInteger()) {
    auto bKind =
        static_cast<const BuiltinType *>(evalType->getUnqualifiedType())
            ->getBuiltinKind();
    isUnsigned = (bKind == BuiltinKind::UInt8 || bKind == BuiltinKind::UInt16 ||
                  bKind == BuiltinKind::UInt32 || bKind == BuiltinKind::UInt64);
  }

  if (node->op == "+")
    return isFloat ? builder.CreateFAdd(L, R) : builder.CreateAdd(L, R);
  if (node->op == "-")
    return isFloat ? builder.CreateFSub(L, R) : builder.CreateSub(L, R);
  if (node->op == "*")
    return isFloat ? builder.CreateFMul(L, R) : builder.CreateMul(L, R);
  if (node->op == "/")
    return isFloat ? builder.CreateFDiv(L, R)
                   : (isUnsigned ? builder.CreateUDiv(L, R)
                                 : builder.CreateSDiv(L, R));
  if (node->op == "%")
    return isFloat ? builder.CreateFRem(L, R)
                   : (isUnsigned ? builder.CreateURem(L, R)
                                 : builder.CreateSRem(L, R));
  if (node->op == "&")
    return builder.CreateAnd(L, R);
  if (node->op == "|")
    return builder.CreateOr(L, R);
  if (node->op == "^")
    return builder.CreateXor(L, R);
  if (node->op == "<<")
    return builder.CreateShl(L, R);
  if (node->op == ">>")
    return isUnsigned ? builder.CreateLShr(L, R) : builder.CreateAShr(L, R);

  if (node->op == "==")
    return isFloat ? builder.CreateFCmpOEQ(L, R) : builder.CreateICmpEQ(L, R);
  if (node->op == "!=")
    return isFloat ? builder.CreateFCmpONE(L, R) : builder.CreateICmpNE(L, R);
  if (node->op == "<")
    return isFloat ? builder.CreateFCmpOLT(L, R)
                   : (isUnsigned ? builder.CreateICmpULT(L, R)
                                 : builder.CreateICmpSLT(L, R));
  if (node->op == "<=")
    return isFloat ? builder.CreateFCmpOLE(L, R)
                   : (isUnsigned ? builder.CreateICmpULE(L, R)
                                 : builder.CreateICmpSLE(L, R));
  if (node->op == ">")
    return isFloat ? builder.CreateFCmpOGT(L, R)
                   : (isUnsigned ? builder.CreateICmpUGT(L, R)
                                 : builder.CreateICmpSGT(L, R));
  if (node->op == ">=")
    return isFloat ? builder.CreateFCmpOGE(L, R)
                   : (isUnsigned ? builder.CreateICmpUGE(L, R)
                                 : builder.CreateICmpSGE(L, R));

  return nullptr;
}

llvm::Value *CodeGen::visit(const VarDeclNode *node) {
  bool isGlobal = !builder.GetInsertBlock();

  uint64_t customAlign = 0;

  const Type *baseUnqualTy = node->type->getUnqualifiedType();
  const Type *elemTyForAlign = baseUnqualTy;
  while (elemTyForAlign->getKind() == TypeKind::Array) {
    elemTyForAlign = static_cast<const ArrayType *>(elemTyForAlign)
                         ->getElementType()
                         ->getUnqualifiedType();
  }

  if (elemTyForAlign->getKind() == TypeKind::Struct ||
      elemTyForAlign->getKind() == TypeKind::Class) {
    if (const DeclNode *decl =
            static_cast<const RecordType *>(elemTyForAlign)->getDeclaration()) {
      for (const auto *ann : decl->annotations) {
        if (ann->name == "align" && !ann->args.empty() &&
            ann->args[0]->kind == NodeKind::Number) {
          uint64_t typeAlign = std::stoull(
              std::string(static_cast<const NumberNode *>(ann->args[0])->raw));
          if (typeAlign > customAlign) {
            customAlign = typeAlign;
          }
        }
      }
    }
  }

  for (const auto *ann : node->annotations) {
    if (ann->name == "align" && !ann->args.empty() &&
        ann->args[0]->kind == NodeKind::Number) {
      uint64_t varAlign = std::stoull(
          std::string(static_cast<const NumberNode *>(ann->args[0])->raw));
      if (varAlign > customAlign) {
        customAlign = varAlign;
      }
    }
  }

  if (node->type->isReferenceType() ||
      node->type->getKind() == TypeKind::RValueReference) {
    if (!node->initializer) {
      diags.report({DiagLevel::Error, node->line, node->column, node->length,
                    "Reference '" + std::string(node->varName) +
                        "' lacks an initializer.",
                    ""});
      return nullptr;
    }

    llvm::Value *initAddr = getLValue(node->initializer);
    if (!initAddr) {
      llvm::Value *val = dispatch(node->initializer);
      initAddr = createEntryBlockAlloca(val->getType(), "tmp.rval.ref");
      builder.CreateStore(val, initAddr);
    }

    if (isGlobal) {
      diags.report({DiagLevel::Error, node->line, node->column, node->length,
                    "Dynamic global references are not supported.", ""});
      return nullptr;
    }

    llvm::Type *ptrTy = builder.getPtrTy();
    llvm::AllocaInst *alloca =
        createEntryBlockAlloca(ptrTy, std::string(node->varName));

    if (customAlign > 0) {
      alloca->setAlignment(llvm::Align(customAlign));
    }

    uint64_t allocSize = mod.getDataLayout().getTypeAllocSize(ptrTy);
    emitLifetimeStart(alloca, allocSize);
    cgCtx.addLifetime(alloca, allocSize);

    createTBAAStore(initAddr, alloca, node->type);
    cgCtx.bind(node->varName, alloca, false);

    return alloca;
  }

  llvm::Type *ty = getLLVMType(node->type);

  if (isGlobal) {
    llvm::Constant *initConst = nullptr;
    if (node->initializer) {
      initConst = evaluateAsConstant(node->initializer);
      if (initConst && initConst->getType() != ty) {
        if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(initConst)) {
          if (ty->isIntegerTy()) {
            initConst = llvm::ConstantInt::get(
                ctx, ci->getValue().sextOrTrunc(ty->getIntegerBitWidth()));
          } else if (ty->isFloatingPointTy()) {
            llvm::APFloat apf(ty->getFltSemantics());
            apf.convertFromAPInt(ci->getValue(), true,
                                 llvm::APFloat::rmNearestTiesToEven);
            initConst = llvm::ConstantFP::get(ctx, apf);
          }
        } else if (auto *cfp = llvm::dyn_cast<llvm::ConstantFP>(initConst)) {
          if (ty->isFloatingPointTy()) {
            llvm::APFloat apf = cfp->getValueAPF();
            bool losesInfo;
            apf.convert(ty->getFltSemantics(),
                        llvm::APFloat::rmNearestTiesToEven, &losesInfo);
            initConst = llvm::ConstantFP::get(ctx, apf);
          } else if (ty->isIntegerTy()) {
            llvm::APSInt api(ty->getIntegerBitWidth(), false);
            bool isExact;
            cfp->getValueAPF().convertToInteger(
                api, llvm::APFloat::rmTowardZero, &isExact);
            initConst = llvm::ConstantInt::get(ctx, api);
          }
        }
      }

      if (!initConst) {
        diags.report(
            {DiagLevel::Error, node->initializer->line,
             node->initializer->column, node->initializer->length,
             "Global variable requires a compile-time constant initializer.",
             ""});
      }
    }

    if (!initConst) {
      initConst = llvm::Constant::getNullValue(ty);
    }

    bool isConstant = node->type->isConstQualified();
    std::string bindName = node->mangledName.empty()
                               ? std::string(node->varName)
                               : node->mangledName;
    auto *gvar = new llvm::GlobalVariable(mod, ty, isConstant,
                                          llvm::GlobalValue::ExternalLinkage,
                                          initConst, bindName);

    if (customAlign > 0) {
      gvar->setAlignment(llvm::Align(customAlign));
    }

    cgCtx.bind(bindName, gvar, true);
    return gvar;
  }

  llvm::AllocaInst *alloca =
      createEntryBlockAlloca(ty, std::string(node->varName));

  if (customAlign > 0) {
    alloca->setAlignment(llvm::Align(customAlign));
  }

  cgCtx.bind(node->varName, alloca, true);

  uint64_t allocSize = mod.getDataLayout().getTypeAllocSize(ty);
  emitLifetimeStart(alloca, allocSize);
  cgCtx.addLifetime(alloca, allocSize);

  const auto *unqualTy = node->type->getUnqualifiedType();
  if (unqualTy->getKind() == TypeKind::Class ||
      unqualTy->getKind() == TypeKind::Struct) {
    auto *recTy = static_cast<const RecordType *>(unqualTy);
    auto *decl = recTy->getDeclaration();
    const FunctionDeclNode *dtor = nullptr;
    if (decl) {
      if (decl->kind == NodeKind::ClassDecl)
        dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::StructDecl)
        dtor = static_cast<const StructDeclNode *>(decl)->destructor;
    }
    if (dtor) {
      cgCtx.addCleanup(alloca, dtor);
    }
  }

  if (node->initializer) {
    if (unqualTy->getKind() == TypeKind::Array) {
      emitArrayLiteralInit(alloca, node->type, node->initializer);
    } else {
      bool isRVO = false;

      if (node->initializer->kind == NodeKind::FunctionCall) {
        auto *callNode =
            static_cast<const FunctionCallNode *>(node->initializer);
        if (callNode->target->kind == NodeKind::Variable) {
          if (callNode->resolvedFunc && callNode->resolvedFunc->isMethod &&
              callNode->resolvedFunc->returnType->isVoid()) {
            isRVO = true;
            emitConstructorCall(callNode, alloca);
          }
        }
      }

      if (!isRVO) {
        bool isAggregate = (unqualTy->getKind() == TypeKind::Struct ||
                            unqualTy->getKind() == TypeKind::Class);

        if (isAggregate && node->copyCtor) {
          llvm::Value *rvalAddr = getLValue(node->initializer);
          if (!rvalAddr) {
            /* Materialize transient r-value to memory if there's no inherent
             * l-value */
            llvm::Value *initVal = dispatch(node->initializer);
            if (initVal) {
              rvalAddr = createEntryBlockAlloca(ty, "tmp.copy.src");
              createTBAAStore(initVal, rvalAddr, node->type);
            }
          }

          if (rvalAddr) {
            llvm::Function *ctorFunc = getOrCreateFunction(node->copyCtor);
            builder.CreateCall(ctorFunc, {alloca, rvalAddr});
          } else {
            diags.report(
                {DiagLevel::Error, node->line, node->column, node->length,
                 "Failed to resolve source for copy constructor.", ""});
          }
        } else if (isAggregate) {
          /* Fallback generic memcpy for records lacking a custom destructor */
          llvm::Value *rvalAddr = getLValue(node->initializer);
          if (rvalAddr) {
            llvm::Align align = mod.getDataLayout().getABITypeAlign(ty);
            builder.CreateMemCpy(alloca, align, rvalAddr, align, allocSize);
          } else {
            llvm::Value *initVal = dispatch(node->initializer);
            if (initVal) {
              createTBAAStore(initVal, alloca, node->type);
            } else {
              diags.report({DiagLevel::Error, node->line, node->column,
                            node->length, "Initialization failed for variable.",
                            ""});
            }
          }
        } else {
          llvm::Value *initVal = dispatch(node->initializer);
          if (initVal) {
            initVal = createImplicitCast(initVal, ty);
            createTBAAStore(initVal, alloca, node->type);
          } else {
            diags.report({DiagLevel::Error, node->line, node->column,
                          node->length, "Initialization failed for variable.",
                          ""});
          }
        }
      }
    }
  } else {
    /* Leave arrays strictly uninitialized when declared without initializers */
    if (node->type->getKind() != TypeKind::Array) {
      emitDefaultInitialization(alloca, node->type);

      if (unqualTy->getKind() == TypeKind::Class ||
          unqualTy->getKind() == TypeKind::Struct) {
        auto *recTy = static_cast<const RecordType *>(unqualTy);
        auto *decl = recTy->getDeclaration();
        if (decl) {
          const FunctionDeclNode *emptyCtor = nullptr;
          llvm::ArrayRef<FunctionDeclNode *> ctors;
          if (decl->kind == NodeKind::ClassDecl)
            ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
          else if (decl->kind == NodeKind::StructDecl)
            ctors = static_cast<const StructDeclNode *>(decl)->constructors;

          for (auto *ctor : ctors) {
            if (ctor->params.size() == 1) {
              emptyCtor = ctor;
              break;
            }
          }
          if (emptyCtor) {
            llvm::Function *ctorFunc = getOrCreateFunction(emptyCtor);
            builder.CreateCall(ctorFunc, {alloca});
          }
        }
      }
    }
  }
  return alloca;
}

llvm::Value *CodeGen::visit(const AssignNode *node) {
  if (node->overloadedOperator) {
    llvm::Value *objPtr = nullptr;
    if (node->target->exprType->isPointerType()) {
      objPtr = dispatch(node->target);
    } else {
      objPtr = getLValue(node->target);
    }

    if (!objPtr) {
      diags.report({DiagLevel::Error, node->target->line, node->target->column,
                    node->target->length, "LHS is not an l-value", ""});
      return nullptr;
    }

    llvm::Function *func = getOrCreateFunction(node->overloadedOperator);
    std::vector<llvm::Value *> argsArgs;
    argsArgs.push_back(objPtr);

    llvm::Value *rhsVal = nullptr;
    bool isRefParam = false;
    if (node->overloadedOperator->params.size() > 1) {
      isRefParam =
          node->overloadedOperator->params[1]->type->isReferenceType() ||
          node->overloadedOperator->params[1]->type->getKind() ==
              TypeKind::RValueReference;
    }

    if (isRefParam) {
      rhsVal = getLValue(node->value);
      if (!rhsVal) {
        llvm::Value *val = dispatch(node->value);
        rhsVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
        builder.CreateStore(val, rhsVal);
      }
    } else {
      rhsVal = dispatch(node->value);
      llvm::Type *paramTy =
          getLLVMType(node->overloadedOperator->params[1]->type);
      rhsVal = createImplicitCast(rhsVal, paramTy);
    }
    argsArgs.push_back(rhsVal);

    return builder.CreateCall(func, argsArgs);
  }

  llvm::Value *lval = getLValue(node->target);
  if (!lval) {
    diags.report({DiagLevel::Error, node->target->line, node->target->column,
                  node->target->length, "Failed to evaluate LHS of assignment.",
                  ""});
    return nullptr;
  }

  if (node->value->kind == NodeKind::FunctionCall) {
    auto *callNode = static_cast<const FunctionCallNode *>(node->value);
    if (callNode->target->kind == NodeKind::Variable) {
      if (callNode->resolvedFunc && callNode->resolvedFunc->isMethod &&
          callNode->resolvedFunc->returnType->isVoid()) {
        emitConstructorCall(callNode, lval);
        return createTBAALoad(getLLVMType(node->exprType), lval,
                              getTBAATagForExpr(node->target));
      }
    }
  }

  const Type *unqualTargetTy = node->target->exprType->getUnqualifiedType();
  bool isAggregate = (unqualTargetTy->getKind() == TypeKind::Struct ||
                      unqualTargetTy->getKind() == TypeKind::Class ||
                      unqualTargetTy->getKind() == TypeKind::Array);

  /* Map assignment to deep memory copy for aggregate types to preserve locality
   * and prevent SSA explosion. evaluates L-values to support references */
  if (isAggregate) {
    llvm::Value *rvalAddr = getLValue(node->value);
    if (rvalAddr) {
      llvm::Type *llvmDestTy = getLLVMType(unqualTargetTy);
      uint64_t size = mod.getDataLayout().getTypeAllocSize(llvmDestTy);
      llvm::Align align = mod.getDataLayout().getABITypeAlign(llvmDestTy);

      builder.CreateMemCpy(lval, align, rvalAddr, align, size);
      return nullptr;
    }
  }

  llvm::Value *rval = dispatch(node->value);
  if (!rval) {
    diags.report({DiagLevel::Error, node->value->line, node->value->column,
                  node->value->length, "Failed to evaluate RHS of assignment.",
                  ""});
    return nullptr;
  }

  if (node->op != "=") {
    llvm::Type *valTy = getLLVMType(node->target->exprType);
    llvm::Value *oldVal = createTBAALoad(valTy, lval, node->target->exprType);
    llvm::Value *castedRval = createImplicitCast(rval, valTy);
    std::string_view binOp = node->op.substr(0, node->op.length() - 1);

    bool isFloat = valTy->isFloatingPointTy();
    bool isUnsigned = false;
    if (node->target->exprType && node->target->exprType->isInteger()) {
      auto bKind = static_cast<const BuiltinType *>(
                       node->target->exprType->getUnqualifiedType())
                       ->getBuiltinKind();
      isUnsigned =
          (bKind == BuiltinKind::UInt8 || bKind == BuiltinKind::UInt16 ||
           bKind == BuiltinKind::UInt32 || bKind == BuiltinKind::UInt64);
    }

    if (binOp == "+")
      rval = isFloat ? builder.CreateFAdd(oldVal, castedRval)
                     : builder.CreateAdd(oldVal, castedRval);
    else if (binOp == "-")
      rval = isFloat ? builder.CreateFSub(oldVal, castedRval)
                     : builder.CreateSub(oldVal, castedRval);
    else if (binOp == "*")
      rval = isFloat ? builder.CreateFMul(oldVal, castedRval)
                     : builder.CreateMul(oldVal, castedRval);
    else if (binOp == "/")
      rval = isFloat ? builder.CreateFDiv(oldVal, castedRval)
                     : (isUnsigned ? builder.CreateUDiv(oldVal, castedRval)
                                   : builder.CreateSDiv(oldVal, castedRval));
    else if (binOp == "%")
      rval = isFloat ? builder.CreateFRem(oldVal, castedRval)
                     : (isUnsigned ? builder.CreateURem(oldVal, castedRval)
                                   : builder.CreateSRem(oldVal, castedRval));
    else if (binOp == "&")
      rval = builder.CreateAnd(oldVal, castedRval);
    else if (binOp == "|")
      rval = builder.CreateOr(oldVal, castedRval);
    else if (binOp == "^")
      rval = builder.CreateXor(oldVal, castedRval);
    else if (binOp == "<<")
      rval = builder.CreateShl(oldVal, castedRval);
    else if (binOp == ">>")
      rval = isUnsigned ? builder.CreateLShr(oldVal, castedRval)
                        : builder.CreateAShr(oldVal, castedRval);
  } else {
    llvm::Type *destTy = getLLVMType(node->target->exprType);
    rval = createImplicitCast(rval, destTy);
  }

  createTBAAStore(rval, lval, getTBAATagForExpr(node->target));

  return rval;
}

llvm::Value *CodeGen::visit(const ArrayLiteralNode *node) {
  llvm::Type *allocTy = getLLVMType(node->exprType);
  llvm::AllocaInst *tempArr = createEntryBlockAlloca(allocTy, "array.literal");
  emitArrayLiteralInit(tempArr, node->exprType, node);
  return builder.CreateInBoundsGEP(allocTy, tempArr,
                                   {builder.getInt32(0), builder.getInt32(0)});
}

llvm::Value *CodeGen::visit(const NullNode *node) {
  return llvm::ConstantPointerNull::get(builder.getPtrTy());
}

llvm::Value *CodeGen::visit(const ParamDeclNode *node) { return nullptr; }

llvm::Value *CodeGen::visit(const FunctionDeclNode *node) {
  if (node->isTemplate)
    return nullptr;

  llvm::Function *func = getOrCreateFunction(node);

  if (!node->body || !func->empty()) {
    return func;
  }

  const FunctionDeclNode *prevFunc = currentFunc;
  currentFunc = node;

  llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", func);
  builder.SetInsertPoint(entry);

  CGScopeGuard guard(cgCtx);

  unsigned idx = 0;
  for (auto &arg : func->args()) {
    const ParamDeclNode *paramDecl = node->params[idx];
    std::string_view pName = paramDecl->name;
    arg.setName(pName);

    llvm::Type *argType = arg.getType();
    llvm::AllocaInst *alloca =
        createEntryBlockAlloca(argType, std::string(pName) + ".addr");
    builder.CreateStore(&arg, alloca);

    bool isRef = paramDecl->type->isReferenceType() ||
                 paramDecl->type->getKind() == TypeKind::RValueReference;
    cgCtx.bind(pName, alloca, !isRef);
    idx++;
  }

  dispatch(node->body);

  for (auto &bb : *func) {
    if (!bb.getTerminator()) {
      builder.SetInsertPoint(&bb);

      emitScopeCleanups();

      if (func->getReturnType()->isVoidTy()) {
        builder.CreateRetVoid();
      } else {
        builder.CreateRet(llvm::UndefValue::get(func->getReturnType()));
      }
    }
  }

  builder.ClearInsertionPoint();
  currentFunc = prevFunc;
  return func;
}

llvm::Value *CodeGen::visit(const FunctionCallNode *node) {
  llvm::Function *func = nullptr;
  std::vector<llvm::Value *> argsArgs;

  if (node->resolvedFunc) {
    func = getOrCreateFunction(node->resolvedFunc);

    if (node->target->kind == NodeKind::MemberAccess) {
      auto ma = static_cast<const MemberAccessNode *>(node->target);
      if (!node->resolvedFunc->isExtern && !node->resolvedFunc->isStatic) {
        llvm::Value *objPtr = nullptr;

        /*
         * Retrieve the object address for the 'this' argument.
         * If the object is already a pointer, dispatch it directly to get its
         * value. Otherwise, fetch its l-value (memory address).
         */
        if (ma->object->exprType->isPointerType()) {
          objPtr = dispatch(ma->object);
        } else {
          objPtr = getLValue(ma->object);
        }

        argsArgs.push_back(objPtr);
      }
    } else if (node->resolvedFunc->isMethod && !node->resolvedFunc->isExtern &&
               !node->resolvedFunc->isStatic) {
      llvm::Type *allocTy = getLLVMType(node->exprType);
      llvm::AllocaInst *instance = createEntryBlockAlloca(allocTy, "instance");

      emitDefaultInitialization(instance, node->exprType);

      argsArgs.push_back(instance);

      const auto *unqual = node->exprType->getUnqualifiedType();
      if (unqual->getKind() == TypeKind::Class ||
          unqual->getKind() == TypeKind::Struct) {
        auto *recTy = static_cast<const RecordType *>(unqual);
        auto *decl = recTy->getDeclaration();
        if (decl) {
          const FunctionDeclNode *dtor = nullptr;
          if (decl->kind == NodeKind::ClassDecl)
            dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
          else if (decl->kind == NodeKind::StructDecl)
            dtor = static_cast<const StructDeclNode *>(decl)->destructor;

          if (dtor) {
            cgCtx.addCleanup(instance, dtor);
          }
        }
      }
    }

    unsigned argIdx = argsArgs.empty() ? 0 : argsArgs.size();
    for (const auto &arg : node->args) {
      llvm::Value *argVal = nullptr;

      bool isRefParam = false;
      size_t paramOffset =
          (node->resolvedFunc->isMethod && !node->resolvedFunc->isExtern &&
           !node->resolvedFunc->isStatic)
              ? 1
              : 0;

      if (argIdx >= paramOffset &&
          (argIdx - paramOffset) < node->resolvedFunc->params.size()) {
        isRefParam =
            node->resolvedFunc->params[argIdx - paramOffset]
                ->type->isReferenceType() ||
            node->resolvedFunc->params[argIdx - paramOffset]->type->getKind() ==
                TypeKind::RValueReference;
      }

      if (isRefParam) {
        argVal = getLValue(arg);
        if (!argVal) {
          llvm::Value *val = dispatch(arg);
          argVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
          builder.CreateStore(val, argVal);
        }
      } else {
        argVal = dispatch(arg);

        if (func && argVal) {
          if (argIdx < func->arg_size()) {
            llvm::Type *paramTy = func->getFunctionType()->getParamType(argIdx);
            argVal = createImplicitCast(argVal, paramTy);
          } else if (func->isVarArg()) {
            if (argVal->getType()->isFloatTy()) {
              argVal = builder.CreateFPExt(argVal, builder.getDoubleTy());
            }
          }
        }
      }

      if (!argVal) {
        diags.report({DiagLevel::Error, arg->line, arg->column, arg->length,
                      "Failed to evaluate argument for function call.", ""});
        return nullptr;
      }
      argsArgs.push_back(argVal);
      argIdx++;
    }

    auto callRes = builder.CreateCall(func, argsArgs);

    if (node->resolvedFunc->isMethod &&
        node->resolvedFunc->returnType->isVoid()) {
      if (!node->exprType->isVoid()) {
        return createTBAALoad(getLLVMType(node->exprType), argsArgs[0],
                              node->exprType);
      }
    }

    return callRes;

  } else {
    /* Evaluate invocation through a dynamic function pointer */
    llvm::Value *dynamicFuncPtr = dispatch(node->target);
    if (!dynamicFuncPtr)
      return nullptr;

    const Type *targetTy = node->target->exprType->getUnqualifiedType();
    const FunctionType *fTy = static_cast<const FunctionType *>(
        static_cast<const PointerType *>(targetTy)->getPointeeType());
    llvm::FunctionType *llvmFTy =
        static_cast<llvm::FunctionType *>(getLLVMType(fTy));

    unsigned argIdx = 0;
    for (const auto &arg : node->args) {
      llvm::Value *argVal = dispatch(arg);
      if (argIdx < llvmFTy->getNumParams()) {
        argVal = createImplicitCast(argVal, llvmFTy->getParamType(argIdx));
      }
      argsArgs.push_back(argVal);
      argIdx++;
    }

    return builder.CreateCall(llvmFTy, dynamicFuncPtr, argsArgs);
  }
}

llvm::Value *CodeGen::visit(const CastNode *node) {
  llvm::Value *src = nullptr;

  if (node->targetType->isReferenceType() ||
      node->targetType->getKind() == TypeKind::RValueReference) {
    src = getLValue(node->expr);
    if (!src) {
      // Allocate temporary storage for materialized xvalues or prvalues
      llvm::Value *val = dispatch(node->expr);
      if (val) {
        src = createEntryBlockAlloca(val->getType(), "tmp.cast.rval");
        createTBAAStore(val, src, node->expr->exprType);
      }
    }
  } else {
    src = dispatch(node->expr);
  }

  if (!src)
    return nullptr;

  llvm::Type *destTy = getLLVMType(node->targetType);
  return createImplicitCast(src, destTy);
}

llvm::Value *CodeGen::visit(const ReturnNode *node) {
  llvm::Value *retVal = nullptr;
  if (node->value) {
    bool isRefReturn =
        currentFunc &&
        (currentFunc->returnType->isReferenceType() ||
         currentFunc->returnType->getKind() == TypeKind::RValueReference);

    if (isRefReturn) {
      retVal = getLValue(node->value);
      if (!retVal) {
        diags.report({DiagLevel::Error, node->line, node->column, node->length,
                      "Unresolved l-value in reference return.", ""});
        return nullptr;
      }
    } else {
      retVal = dispatch(node->value);
      if (!retVal) {
        diags.report({DiagLevel::Error, node->line, node->column, node->length,
                      "Failed to evaluate return expression.", ""});
        return nullptr;
      }
      if (currentFunc) {
        llvm::Type *destTy = getLLVMType(currentFunc->returnType);
        retVal = createImplicitCast(retVal, destTy);
      }
    }
  }

  auto allScopes = cgCtx.getAllScopes();
  for (auto scopeIt = allScopes.rbegin(); scopeIt != allScopes.rend();
       ++scopeIt) {
    for (auto cleanupIt = scopeIt->cleanups.rbegin();
         cleanupIt != scopeIt->cleanups.rend(); ++cleanupIt) {
      emitCleanupCall(cleanupIt->instancePtr, cleanupIt->destructor);
    }

    /* Ensure deterministic lifetime closure upon early returns */
    for (auto lifeIt = scopeIt->lifetimes.rbegin();
         lifeIt != scopeIt->lifetimes.rend(); ++lifeIt) {
      emitLifetimeEnd(lifeIt->allocaInst, lifeIt->size);
    }
  }

  if (retVal) {
    return builder.CreateRet(retVal);
  }
  return builder.CreateRetVoid();
}

llvm::Value *CodeGen::visit(const ModuleNode *node) {
  for (const auto *imp : node->importedModules) {
    for (const auto &stmt : imp->statements) {
      if (stmt->kind == NodeKind::FunctionDecl) {
        getOrCreateFunction(static_cast<const FunctionDeclNode *>(stmt));
      }
    }
  }

  /* Pass 1: Forward declare and materialize all record types in the translation
   * unit before processing function bodies or variables that rely on their size
   * and layout. */
  for (const auto &stmt : node->statements) {
    if (stmt->kind == NodeKind::StructDecl) {
      getLLVMType(static_cast<const StructDeclNode *>(stmt)->recordType);
    } else if (stmt->kind == NodeKind::ClassDecl) {
      getLLVMType(static_cast<const ClassDeclNode *>(stmt)->recordType);
    } else if (stmt->kind == NodeKind::AnnotationDecl) {
      getLLVMType(static_cast<const AnnotationDeclNode *>(stmt)->recordType);
    }
  }

  for (const auto &stmt : node->instantiatedTemplates) {
    if (stmt->kind == NodeKind::StructDecl) {
      getLLVMType(static_cast<const StructDeclNode *>(stmt)->recordType);
    } else if (stmt->kind == NodeKind::ClassDecl) {
      getLLVMType(static_cast<const ClassDeclNode *>(stmt)->recordType);
    }
  }

  /* Pass 2: Traverse and generate instructions for expressions, methods, and
   * functions. */
  for (const auto &stmt : node->statements) {
    dispatch(stmt);
  }

  for (const auto &stmt : node->instantiatedTemplates) {
    dispatch(stmt);
  }

  return nullptr;
}

llvm::Value *CodeGen::visit(const ArraySubscriptNode *node) {
  llvm::Value *lval = getLValue(node);
  if (!lval)
    return nullptr;

  if (node->exprType->getKind() == TypeKind::Array) {
    llvm::Type *arrTy = getLLVMType(node->exprType);
    return builder.CreateInBoundsGEP(
        arrTy, lval, {builder.getInt32(0), builder.getInt32(0)});
  }

  return createTBAALoad(getLLVMType(node->exprType), lval, node->exprType);
}

llvm::Value *CodeGen::visit(const NewExprNode *node) {
  llvm::Type *allocTy = getLLVMType(node->allocatedType);
  llvm::Value *sizeVal = nullptr;
  llvm::Value *arrSize64 = nullptr;

  if (node->arraySize) {
    llvm::Value *arrSize = dispatch(node->arraySize);
    llvm::Value *elemSize =
        builder.getInt64(mod.getDataLayout().getTypeAllocSize(allocTy));
    arrSize64 = builder.CreateIntCast(arrSize, builder.getInt64Ty(), false);
    llvm::Value *totalElemSize = builder.CreateMul(arrSize64, elemSize);

    // Over-allocate by 8 bytes to store the array length prefix
    sizeVal = builder.CreateAdd(totalElemSize, builder.getInt64(8));
  } else {
    sizeVal = builder.getInt64(mod.getDataLayout().getTypeAllocSize(allocTy));
  }

  llvm::Function *mallocFunc = mod.getFunction("malloc");
  if (!mallocFunc) {
    llvm::FunctionType *mallocTy = llvm::FunctionType::get(
        builder.getPtrTy(), {builder.getInt64Ty()}, false);
    mallocFunc = llvm::Function::Create(
        mallocTy, llvm::Function::ExternalLinkage, "malloc", mod);
  }

  llvm::Value *allocatedMem = builder.CreateCall(mallocFunc, {sizeVal});
  llvm::Value *userMem = allocatedMem;

  if (node->arraySize) {
    // Store the element count at the base of the allocated block
    builder.CreateStore(arrSize64, allocatedMem);
    // Offset the pointer by 8 bytes to return to the user context
    userMem = builder.CreateInBoundsGEP(builder.getInt8Ty(), allocatedMem,
                                        builder.getInt64(8));
  }

  if (node->hasParens) {
    llvm::Value *memsetSize = sizeVal;
    if (node->arraySize) {
      memsetSize = builder.CreateSub(sizeVal, builder.getInt64(8));
    }
    builder.CreateMemSet(userMem, builder.getInt8(0), memsetSize,
                         llvm::Align(1));

    if (!node->arraySize) {
      if (node->resolvedConstructor) {
        llvm::Function *ctorFunc =
            getOrCreateFunction(node->resolvedConstructor);
        llvm::Value *typedMem =
            builder.CreateBitCast(userMem, getLLVMType(node->exprType));

        std::vector<llvm::Value *> argsArgs;
        argsArgs.push_back(typedMem);

        unsigned argIdx = 1;
        for (const auto &arg : node->args) {
          llvm::Value *argVal = nullptr;

          bool isRefParam = false;
          if (node->resolvedConstructor &&
              argIdx < node->resolvedConstructor->params.size()) {
            isRefParam =
                node->resolvedConstructor->params[argIdx]
                    ->type->isReferenceType() ||
                node->resolvedConstructor->params[argIdx]->type->getKind() ==
                    TypeKind::RValueReference;
          }

          if (isRefParam) {
            argVal = getLValue(arg);
            if (!argVal) {
              llvm::Value *val = dispatch(arg);
              argVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
              builder.CreateStore(val, argVal);
            }
          } else {
            argVal = dispatch(arg);
            if (ctorFunc && argVal && argIdx < ctorFunc->arg_size()) {
              llvm::Type *paramTy =
                  ctorFunc->getFunctionType()->getParamType(argIdx);
              argVal = createImplicitCast(argVal, paramTy);
            }
          }

          if (!argVal) {
            diags.report({DiagLevel::Error, arg->line, arg->column, arg->length,
                          "Failed to evaluate argument for constructor call.",
                          ""});
            return nullptr;
          }
          argsArgs.push_back(argVal);
          argIdx++;
        }

        builder.CreateCall(ctorFunc, argsArgs);
      }
    }
  }

  return builder.CreateBitCast(userMem, getLLVMType(node->exprType));
}

llvm::Value *CodeGen::visit(const DeleteExprNode *node) {
  llvm::Value *ptr = dispatch(node->ptr);
  if (!ptr)
    return nullptr;

  llvm::Function *theFunction = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *deleteBB =
      llvm::BasicBlock::Create(ctx, "delete.notnull", theFunction);
  llvm::BasicBlock *mergeBB = llvm::BasicBlock::Create(ctx, "delete.cont");

  // Prevent SIGSEGV by ensuring the pointer is valid before dereferencing
  llvm::Value *isNotNull = builder.CreateIsNotNull(ptr, "ptr.notnull");
  builder.CreateCondBr(isNotNull, deleteBB, mergeBB);

  builder.SetInsertPoint(deleteBB);

  llvm::Function *freeFunc = mod.getFunction("free");
  if (!freeFunc) {
    llvm::FunctionType *freeTy = llvm::FunctionType::get(
        builder.getVoidTy(), {builder.getPtrTy()}, false);
    freeFunc = llvm::Function::Create(freeTy, llvm::Function::ExternalLinkage,
                                      "free", mod);
  }

  if (node->isArray) {
    // Shift pointer back by 8 bytes to get the raw allocation start and the
    // count
    llvm::Value *rawPtr = builder.CreateInBoundsGEP(
        builder.getInt8Ty(), ptr, builder.getInt64(-8), "raw.ptr");
    llvm::Value *count =
        builder.CreateLoad(builder.getInt64Ty(), rawPtr, "array.count");

    const Type *pointeeTy =
        static_cast<const PointerType *>(node->ptr->exprType)->getPointeeType();
    const Type *unqual = pointeeTy->getUnqualifiedType();

    const FunctionDeclNode *dtor = nullptr;
    if (unqual->getKind() == TypeKind::Class ||
        unqual->getKind() == TypeKind::Struct) {
      auto *recTy = static_cast<const RecordType *>(unqual);

      if (auto *decl = recTy->getDeclaration()) {
        if (decl->kind == NodeKind::ClassDecl) {
          dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
        } else if (decl->kind == NodeKind::StructDecl) {
          dtor = static_cast<const StructDeclNode *>(decl)->destructor;
        }
      }
    }

    if (dtor) {
      llvm::BasicBlock *condBB =
          llvm::BasicBlock::Create(ctx, "delete.array.cond", theFunction);
      llvm::BasicBlock *bodyBB =
          llvm::BasicBlock::Create(ctx, "delete.array.body", theFunction);
      llvm::BasicBlock *endBB =
          llvm::BasicBlock::Create(ctx, "delete.array.end", theFunction);

      llvm::IRBuilder<> TmpB(&theFunction->getEntryBlock(),
                             theFunction->getEntryBlock().begin());
      llvm::AllocaInst *idxAlloca =
          TmpB.CreateAlloca(builder.getInt64Ty(), nullptr, "delete.idx");

      // Initialize loop counter with the array count to traverse backwards
      builder.CreateStore(count, idxAlloca);
      builder.CreateBr(condBB);

      builder.SetInsertPoint(condBB);
      llvm::Value *idxVal = builder.CreateLoad(builder.getInt64Ty(), idxAlloca);
      llvm::Value *cmp = builder.CreateICmpSGT(idxVal, builder.getInt64(0));
      builder.CreateCondBr(cmp, bodyBB, endBB);

      builder.SetInsertPoint(bodyBB);
      llvm::Value *nextIdx = builder.CreateSub(idxVal, builder.getInt64(1));
      builder.CreateStore(nextIdx, idxAlloca);

      llvm::Type *llvmElemTy = getLLVMType(unqual);
      llvm::Value *elemPtr =
          builder.CreateInBoundsGEP(llvmElemTy, ptr, nextIdx);

      emitCleanupCall(elemPtr, dtor);
      builder.CreateBr(condBB);

      builder.SetInsertPoint(endBB);
    }

    // Free the original un-offset memory block
    builder.CreateCall(freeFunc, {rawPtr});
  } else {
    if (node->ptr->exprType && node->ptr->exprType->isPointerType()) {
      const Type *pointeeTy =
          static_cast<const PointerType *>(node->ptr->exprType)
              ->getPointeeType();
      const Type *unqual = pointeeTy->getUnqualifiedType();

      if (unqual->getKind() == TypeKind::Class ||
          unqual->getKind() == TypeKind::Struct) {
        auto *recTy = static_cast<const RecordType *>(unqual);

        if (auto *decl = recTy->getDeclaration()) {
          const FunctionDeclNode *dtor = nullptr;
          if (decl->kind == NodeKind::ClassDecl) {
            dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
          } else if (decl->kind == NodeKind::StructDecl) {
            dtor = static_cast<const StructDeclNode *>(decl)->destructor;
          }

          if (dtor) {
            emitCleanupCall(ptr, dtor);
          }
        }
      }
    }
    builder.CreateCall(freeFunc, {ptr});
  }

  builder.CreateBr(mergeBB);

  theFunction->insert(theFunction->end(), mergeBB);
  builder.SetInsertPoint(mergeBB);

  return nullptr;
}

llvm::MDNode *CodeGen::getTBAATypeNode(const Type *type) {
  if (!type || type->isVoid())
    return nullptr;

  const Type *unqual = type->getUnqualifiedType();
  if (tbaaTypes.contains(unqual))
    return tbaaTypes[unqual];

  /* Enforce hierarchical scalar derivations to enable aggressive pointer
   * disjointing */
  llvm::MDNode *charNode =
      mdBuilder.createTBAAScalarTypeNode("omnipotent char", tbaaRoot);
  llvm::MDNode *node = nullptr;

  if (unqual->isPointerType() || unqual->isReferenceType() ||
      unqual->getKind() == TypeKind::RValueReference) {
    node = mdBuilder.createTBAAScalarTypeNode("any pointer", charNode);
  } else if (unqual->isBuiltinType()) {
    node = mdBuilder.createTBAAScalarTypeNode(unqual->toString(), charNode);
  } else if (unqual->getKind() == TypeKind::Class ||
             unqual->getKind() == TypeKind::Struct) {
    /* Struct-Path TBAA: Build precise topological map of the record type */
    auto *recTy = static_cast<const RecordType *>(unqual);
    llvm::StructType *structTy =
        llvm::cast<llvm::StructType>(getLLVMType(recTy));
    const llvm::StructLayout *layout =
        mod.getDataLayout().getStructLayout(structTy);

    std::vector<std::pair<llvm::MDNode *, uint64_t>> fields;
    for (const auto &f : recTy->getFields()) {
      uint64_t offset = layout->getElementOffset(f.index);
      llvm::MDNode *fieldTypeNode = getTBAATypeNode(f.type);
      fields.push_back({fieldTypeNode, offset});
    }
    node = mdBuilder.createTBAAStructTypeNode(recTy->getName(), fields);
  } else {
    node = charNode;
  }

  tbaaTypes[unqual] = node;
  return node;
}

llvm::MDNode *CodeGen::getTBAAAccessTag(const Type *type) {
  if (!type || type->isVoid())
    return nullptr;

  const Type *unqual = type->getUnqualifiedType();
  if (unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Array) {
    return nullptr;
  }

  llvm::MDNode *typeNode = getTBAATypeNode(type);
  return mdBuilder.createTBAAStructTagNode(typeNode, typeNode, 0);
}

llvm::MDNode *CodeGen::getTBAAStructAccessTag(const Type *baseType,
                                              const Type *accessType,
                                              uint64_t offset) {
  if (!baseType || !accessType)
    return nullptr;

  const Type *unqualAccess = accessType->getUnqualifiedType();
  if (unqualAccess->getKind() == TypeKind::Struct ||
      unqualAccess->getKind() == TypeKind::Class ||
      unqualAccess->getKind() == TypeKind::Array) {
    return nullptr;
  }

  llvm::MDNode *baseNode = getTBAATypeNode(baseType);
  llvm::MDNode *accessNode = getTBAATypeNode(accessType);

  return mdBuilder.createTBAAStructTagNode(baseNode, accessNode, offset);
}

llvm::MDNode *CodeGen::getTBAATagForExpr(const ExprNode *node) {
  if (!node || !node->exprType)
    return nullptr;

  const Type *unqual = node->exprType->getUnqualifiedType();
  if (unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Array) {
    return nullptr;
  }

  if (node->kind == NodeKind::MemberAccess) {
    auto *ma = static_cast<const MemberAccessNode *>(node);
    if (ma->isMethodRef)
      return nullptr;
    if (ma->isStaticFieldRef)
      return getTBAAAccessTag(node->exprType);

    const Type *baseTy = ma->object->exprType;
    if (baseTy->isPointerType()) {
      baseTy = static_cast<const PointerType *>(baseTy)->getPointeeType();
    } else if (baseTy->isReferenceType() ||
               baseTy->getKind() == TypeKind::RValueReference) {
      baseTy = static_cast<const ReferenceType *>(baseTy)->getPointeeType();
    }

    llvm::StructType *llBaseTy =
        llvm::cast<llvm::StructType>(getLLVMType(baseTy));
    uint64_t offset =
        mod.getDataLayout().getStructLayout(llBaseTy)->getElementOffset(
            ma->fieldIndex);

    return getTBAAStructAccessTag(baseTy, ma->exprType, offset);
  }

  if (node->kind == NodeKind::Variable) {
    auto *varNode = static_cast<const VariableNode *>(node);
    if (varNode->isField) {
      llvm::StructType *llBaseTy =
          llvm::cast<llvm::StructType>(getLLVMType(varNode->parentType));
      uint64_t offset =
          mod.getDataLayout().getStructLayout(llBaseTy)->getElementOffset(
              varNode->fieldIndex);
      return getTBAAStructAccessTag(varNode->parentType, varNode->exprType,
                                    offset);
    }
  }

  /* Fallback to strict scalar TBAA for arrays, pointers, and direct variables
   */
  return getTBAAAccessTag(node->exprType);
}

llvm::LoadInst *CodeGen::createTBAALoad(llvm::Type *llTy, llvm::Value *ptr,
                                        llvm::MDNode *tbaaTag,
                                        const llvm::Twine &name) {
  auto *load = builder.CreateLoad(llTy, ptr, name);
  if (tbaaTag) {
    load->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaTag);
  }
  return load;
}

llvm::LoadInst *CodeGen::createTBAALoad(llvm::Type *llTy, llvm::Value *ptr,
                                        const Type *utopiaTy,
                                        const llvm::Twine &name) {
  return createTBAALoad(llTy, ptr, getTBAAAccessTag(utopiaTy), name);
}

llvm::StoreInst *CodeGen::createTBAAStore(llvm::Value *val, llvm::Value *ptr,
                                          llvm::MDNode *tbaaTag) {
  auto *store = builder.CreateStore(val, ptr);
  if (tbaaTag) {
    store->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaTag);
  }
  return store;
}

llvm::StoreInst *CodeGen::createTBAAStore(llvm::Value *val, llvm::Value *ptr,
                                          const Type *utopiaTy) {
  return createTBAAStore(val, ptr, getTBAAAccessTag(utopiaTy));
}

void CodeGen::emitLifetimeStart(llvm::AllocaInst *allocaInst, uint64_t size) {
  if (size == 0 || !allocaInst)
    return;

  llvm::Function *lifetimeStart = llvm::Intrinsic::getOrInsertDeclaration(
      &mod, llvm::Intrinsic::lifetime_start, {builder.getPtrTy()});

  builder.CreateCall(lifetimeStart, {builder.getInt64(size), allocaInst});
}

void CodeGen::emitLifetimeEnd(llvm::AllocaInst *allocaInst, uint64_t size) {
  if (size == 0 || !allocaInst)
    return;

  llvm::Function *lifetimeEnd = llvm::Intrinsic::getOrInsertDeclaration(
      &mod, llvm::Intrinsic::lifetime_end, {builder.getPtrTy()});

  builder.CreateCall(lifetimeEnd, {builder.getInt64(size), allocaInst});
}

llvm::AllocaInst *CodeGen::createEntryBlockAlloca(llvm::Type *type,
                                                  const std::string &varName) {
  llvm::Function *TheFunction = builder.GetInsertBlock()->getParent();
  llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                         TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(type, nullptr, varName);
}

void CodeGen::emitDefaultInitialization(llvm::Value *ptr, const Type *type) {
  llvm::Type *llTy = getLLVMType(type);
  uint64_t size = mod.getDataLayout().getTypeAllocSize(llTy);

  /* Warn if the stack allocation exceeds typical OS stack limits (e.g., 8MB) */
  if (size >= 8388608) {
    diags.report({DiagLevel::Warning, 0, 0, 0,
                  "Massive memory allocation detected (" +
                      std::to_string(size) +
                      " bytes). This may cause a stack overflow at runtime.",
                  ""});
  }

  if (size > 0) {
    llvm::Align align = mod.getDataLayout().getABITypeAlign(llTy);
    builder.CreateMemSet(ptr, builder.getInt8(0), size, align);
  }

  const auto *unqual = type->getUnqualifiedType();
  if (unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Class) {
    auto *recTy = static_cast<const RecordType *>(unqual);
    auto *decl = recTy->getDeclaration();
    if (!decl)
      return;

    llvm::ArrayRef<VarDeclNode *> fields;
    if (decl->kind == NodeKind::StructDecl) {
      fields = static_cast<const StructDeclNode *>(decl)->fields;
    } else if (decl->kind == NodeKind::ClassDecl) {
      fields = static_cast<const ClassDeclNode *>(decl)->fields;
    }

    llvm::StructType *llRecTy = llvm::cast<llvm::StructType>(llTy);
    const llvm::StructLayout *layout =
        mod.getDataLayout().getStructLayout(llRecTy);

    for (size_t i = 0; i < fields.size(); ++i) {
      auto *fieldDecl = fields[i];
      if (fieldDecl->initializer) {
        llvm::Value *initVal = dispatch(fieldDecl->initializer);
        if (initVal) {
          llvm::Type *destTy = getLLVMType(fieldDecl->type);
          initVal = createImplicitCast(initVal, destTy);

          llvm::Value *gep = builder.CreateStructGEP(
              llTy, ptr, i, std::string(fieldDecl->varName));

          uint64_t offset = layout->getElementOffset(i);
          llvm::MDNode *tbaaTag =
              getTBAAStructAccessTag(type, fieldDecl->type, offset);

          createTBAAStore(initVal, gep, tbaaTag);
        }
      }
    }
  }
}

void CodeGen::emitArrayLiteralInit(llvm::Value *targetAddr,
                                   const Type *targetType,
                                   const ExprNode *initExpr) {
  if (!initExpr || !targetAddr)
    return;

  const Type *unqualTarget = targetType->getUnqualifiedType();

  if (initExpr->kind == NodeKind::ArrayLiteral) {
    auto *lit = static_cast<const ArrayLiteralNode *>(initExpr);
    llvm::Type *llTargetTy = getLLVMType(unqualTarget);

    /* Zero-initialize whole array via llvm.memset when given an empty literal
     * [] */
    if (lit->elements.empty()) {
      uint64_t size = mod.getDataLayout().getTypeAllocSize(llTargetTy);
      if (size > 0) {
        llvm::Align align = mod.getDataLayout().getABITypeAlign(llTargetTy);
        builder.CreateMemSet(targetAddr, builder.getInt8(0), size, align);
      }
      return;
    }

    if (unqualTarget->getKind() == TypeKind::Array) {
      const auto *arrTy = static_cast<const ArrayType *>(unqualTarget);
      const Type *elemTy = arrTy->getElementType();

      for (size_t i = 0; i < lit->elements.size(); ++i) {
        llvm::Value *elemPtr = builder.CreateInBoundsGEP(
            llTargetTy, targetAddr, {builder.getInt32(0), builder.getInt32(i)});

        if (elemTy->getKind() == TypeKind::Array &&
            lit->elements[i]->kind == NodeKind::ArrayLiteral) {
          emitArrayLiteralInit(elemPtr, elemTy, lit->elements[i]);
        } else {
          llvm::Value *val = dispatch(lit->elements[i]);
          if (val) {
            val = createImplicitCast(val, getLLVMType(elemTy));
            createTBAAStore(val, elemPtr, elemTy);
          }
        }
      }
    }
  } else {
    llvm::Value *srcVal = dispatch(initExpr);
    if (srcVal) {
      llvm::Type *destTy = getLLVMType(unqualTarget);
      srcVal = createImplicitCast(srcVal, destTy);
      createTBAAStore(srcVal, targetAddr, targetType);
    }
  }
}

void CodeGen::emitCleanupCall(llvm::Value *ptr, const FunctionDeclNode *dtor) {
  if (!ptr || !dtor)
    return;
  llvm::Function *dtorFunc = getOrCreateFunction(dtor);
  builder.CreateCall(dtorFunc, {ptr});
}

void CodeGen::emitScopeCleanups() {
  const auto &scope = cgCtx.getCurrentScope();
  for (auto it = scope.cleanups.rbegin(); it != scope.cleanups.rend(); ++it) {
    emitCleanupCall(it->instancePtr, it->destructor);
  }

  /* Flush lifetimes back to the execution environment upon natural closure */
  for (auto it = scope.lifetimes.rbegin(); it != scope.lifetimes.rend(); ++it) {
    emitLifetimeEnd(it->allocaInst, it->size);
  }
}

} // namespace utopia