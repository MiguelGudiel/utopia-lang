#include "utopia/CodeGen/CodeGen.hpp"
#include <iostream>
#include <llvm/ADT/APSInt.h>
#include <optional>
#include <string>

namespace utopia {

llvm::Type *CodeGen::getLLVMType(const Type *type) {
  if (!type)
    return builder.getVoidTy();

  if (type->getKind() == TypeKind::Const) {
    return getLLVMType(static_cast<const ConstType *>(type)->getBaseType());
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
  } else if (type->isPointerType() || type->isReferenceType()) {
    return builder.getPtrTy();
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
    if (structTy->isOpaque()) {
      std::vector<llvm::Type *> elements;
      for (const auto &f : rec->getFields()) {
        elements.push_back(getLLVMType(f.type));
      }
      structTy->setBody(elements, false);
    }

    return structTy;
  }

  return builder.getVoidTy();
}

llvm::Value *CodeGen::createImplicitCast(llvm::Value *src, llvm::Type *destTy) {
  if (!src)
    return nullptr;
  llvm::Type *srcTy = src->getType();
  if (srcTy == destTy)
    return src;

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
  std::string irName = node->name == "main" ? "main" : node->mangledName;
  llvm::Function *func = mod.getFunction(irName);

  if (func)
    return func;

  std::vector<llvm::Type *> paramTypes;
  for (const auto *p : node->params) {
    paramTypes.push_back(getLLVMType(p->type));
  }

  llvm::FunctionType *funcType =
      llvm::FunctionType::get(getLLVMType(node->returnType), paramTypes, false);

  func = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                irName, mod);

  /* Zero-cost annotation traversal mapping directly to LLVM IR attributes */
  for (const auto *ann : node->annotations) {
    if (ann->name == "inline") {
      func->addFnAttr(llvm::Attribute::AlwaysInline);
    }
  }

  func->addFnAttr(llvm::Attribute::NoUnwind);

  auto addRefAttributes = [&](const Type *type,
                              std::optional<unsigned> paramIdx) {
    if (!type->isReferenceType())
      return;

    auto addAttrObj = [&](llvm::Attribute attr) {
      if (paramIdx.has_value())
        func->addParamAttr(*paramIdx, attr);
      else
        func->addRetAttr(attr);
    };

    addAttrObj(llvm::Attribute::get(ctx, llvm::Attribute::NonNull));
    addAttrObj(llvm::Attribute::get(ctx, llvm::Attribute::NoUndef));

    const Type *pointee =
        static_cast<const ReferenceType *>(type)->getPointeeType();

    if (pointee->isConstQualified() && paramIdx.has_value()) {
      addAttrObj(llvm::Attribute::get(ctx, llvm::Attribute::ReadOnly));
    }

    const Type *unqualPointee = pointee->getUnqualifiedType();

    if (unqualPointee->isBuiltinType()) {
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

  addRefAttributes(node->returnType, std::nullopt);

  unsigned argIdx = 0;
  for (const auto *param : node->params) {
    addRefAttributes(param->type, argIdx);
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

llvm::Value *CodeGen::visit(const AnnotationNode *node) {
  /* Annotations act as static descriptors; dynamic execution cost is strictly 0
   */
  return nullptr;
}

void CodeGen::emitConstructorCall(const FunctionCallNode *node,
                                  llvm::Value *targetAddr) {
  if (!node->resolvedFunc)
    return;

  // Evalúa valores por defecto y ceros previos a la ejecución del constructor
  emitDefaultInitialization(targetAddr, node->exprType);

  llvm::Function *func = getOrCreateFunction(node->resolvedFunc);

  std::vector<llvm::Value *> argsArgs;
  argsArgs.push_back(targetAddr);

  unsigned argIdx = 1;
  for (const auto &arg : node->args) {
    llvm::Value *argVal = nullptr;

    if (arg->kind == NodeKind::Variable && arg->exprType->isReferenceType()) {
      argVal = getLValue(arg);
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

  return nullptr;
}

llvm::Value *CodeGen::getLValue(const ExprNode *node) {
  if (!node)
    return nullptr;

  if (node->kind == NodeKind::Variable) {
    auto *varNode = static_cast<const VariableNode *>(node);

    if (varNode->isField) {
      SymbolInfo sym = cgCtx.lookupDetailed("this");

      llvm::Value *thisPtr = sym.value;
      llvm::Type *llvmBaseTy = getLLVMType(varNode->parentType);

      return builder.CreateStructGEP(llvmBaseTy, thisPtr, varNode->fieldIndex,
                                     varNode->name);
    }

    SymbolInfo sym = cgCtx.lookupDetailed(varNode->name);

    if (!sym.value) {
      std::cerr << "[CodeGen Error] Unbound symbol in l-value resolution: '"
                << varNode->name << "'.\n";
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
    else if (baseTy->isReferenceType())
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

  if (node->exprType && node->exprType->isReferenceType()) {
    return dispatch(node);
  }

  std::cerr << "[CodeGen Error] Expression does not yield a valid l-value.\n";
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
  if (node->recordType) {
    getLLVMType(node->recordType);
  }
  return nullptr;
}

llvm::Value *CodeGen::visit(const ClassDeclNode *node) {
  if (node->recordType) {
    getLLVMType(node->recordType);
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

llvm::Value *CodeGen::visit(const VariableNode *node) {
  if (node->isField) {
    SymbolInfo sym = cgCtx.lookupDetailed("this");

    llvm::Value *thisPtr = sym.value;
    llvm::Type *llvmBaseTy = getLLVMType(node->parentType);

    llvm::Value *gep = builder.CreateStructGEP(llvmBaseTy, thisPtr,
                                               node->fieldIndex, node->name);
    return builder.CreateLoad(getLLVMType(node->exprType), gep);
  }

  llvm::Value *lval = getLValue(node);
  if (!lval)
    return nullptr;

  if (llvm::isa<llvm::Argument>(lval)) {
    return lval;
  }

  const Type *loadTy = node->exprType;
  if (loadTy->isReferenceType()) {
    loadTy = static_cast<const ReferenceType *>(loadTy)->getPointeeType();
  }

  llvm::Type *llvmTy = getLLVMType(loadTy);
  return builder.CreateLoad(llvmTy, lval, node->name);
}

llvm::Value *CodeGen::visit(const MemberAccessNode *node) {
  if (node->isMethodRef)
    return nullptr;

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
  else if (baseTy->isReferenceType())
    baseTy = static_cast<const ReferenceType *>(baseTy)->getPointeeType();

  llvm::Type *llvmBaseTy = getLLVMType(baseTy);

  llvm::Value *gep = builder.CreateStructGEP(
      llvmBaseTy, objPtr, node->fieldIndex, node->memberName);
  return builder.CreateLoad(getLLVMType(node->exprType), gep);
}

llvm::Value *CodeGen::visit(const UnaryOpNode *node) {
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
    return builder.CreateLoad(loadTy, ptr);
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

  return nullptr;
}

llvm::Value *CodeGen::visit(const BinaryOpNode *node) {
  llvm::Value *L = dispatch(node->left);
  llvm::Value *R = dispatch(node->right);

  if (!L || !R) {
    diags.report({DiagLevel::Error, node->line, node->column, node->length,
                  "Failed to generate binary operands.", ""});
    return nullptr;
  }

  bool isFloat = L->getType()->isFloatingPointTy();

  if (node->op == "+")
    return isFloat ? builder.CreateFAdd(L, R) : builder.CreateAdd(L, R);
  if (node->op == "-")
    return isFloat ? builder.CreateFSub(L, R) : builder.CreateSub(L, R);
  if (node->op == "*")
    return isFloat ? builder.CreateFMul(L, R) : builder.CreateMul(L, R);
  if (node->op == "/")
    return isFloat ? builder.CreateFDiv(L, R) : builder.CreateSDiv(L, R);

  return nullptr;
}

llvm::Value *CodeGen::visit(const VarDeclNode *node) {
  bool isGlobal = !builder.GetInsertBlock();

  if (node->type->isReferenceType()) {
    if (!node->initializer) {
      diags.report({DiagLevel::Error, node->line, node->column, node->length,
                    "Reference '" + std::string(node->varName) +
                        "' lacks an initializer.",
                    ""});
      return nullptr;
    }

    llvm::Value *initAddr = getLValue(node->initializer);
    if (!initAddr) {
      diags.report({DiagLevel::Error, node->line, node->column, node->length,
                    "Failed to resolve target address for reference.", ""});
      return nullptr;
    }

    if (isGlobal) {
      diags.report({DiagLevel::Error, node->line, node->column, node->length,
                    "Dynamic global references are not supported.", ""});
      return nullptr;
    }

    llvm::Type *ptrTy = builder.getPtrTy();
    llvm::AllocaInst *alloca =
        createEntryBlockAlloca(ptrTy, std::string(node->varName));
    builder.CreateStore(initAddr, alloca);
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
    auto *gvar = new llvm::GlobalVariable(mod, ty, isConstant,
                                          llvm::GlobalValue::ExternalLinkage,
                                          initConst, node->varName);
    cgCtx.bind(node->varName, gvar, true);
    return gvar;
  }

  // SROA Optimization: Move the allocas to the beginning of the function
  llvm::AllocaInst *alloca =
      createEntryBlockAlloca(ty, std::string(node->varName));
  cgCtx.bind(node->varName, alloca, true);

  // Uso de 'auto *' para evitar shadowing (colisión) con llvm::Type
  const auto *unqualTy = node->type->getUnqualifiedType();
  if (unqualTy->getKind() == TypeKind::Class) {
    auto *classTy = static_cast<const ClassType *>(unqualTy);
    auto *classDecl =
        static_cast<const ClassDeclNode *>(classTy->getDeclaration());
    if (classDecl && classDecl->destructor) {
      cgCtx.addCleanup(alloca, classDecl->destructor);
    }
  }

  if (node->initializer) {
    bool isRVO = false;

    if (node->initializer->kind == NodeKind::FunctionCall) {
      auto *callNode = static_cast<const FunctionCallNode *>(node->initializer);
      if (callNode->target->kind == NodeKind::Variable) {
        if (callNode->resolvedFunc && callNode->resolvedFunc->isMethod &&
            callNode->resolvedFunc->returnType->isVoid()) {
          isRVO = true;
          emitConstructorCall(callNode, alloca);
        }
      }
    }

    if (!isRVO) {
      llvm::Value *initVal = dispatch(node->initializer);
      if (initVal) {
        initVal = createImplicitCast(initVal, ty);
        builder.CreateStore(initVal, alloca);
      } else {
        diags.report({DiagLevel::Error, node->line, node->column, node->length,
                      "Initialization failed for variable.", ""});
      }
    }
  } else {
    // Implicit Default Initialization (Zero + Default Member Initializers)
    emitDefaultInitialization(alloca, node->type);

    // Llama al constructor vacío automáticamente (si está definido)
    if (unqualTy->getKind() == TypeKind::Class) {
      auto *classTy = static_cast<const ClassType *>(unqualTy);
      auto *classDecl =
          static_cast<const ClassDeclNode *>(classTy->getDeclaration());
      if (classDecl) {
        const FunctionDeclNode *emptyCtor = nullptr;
        for (auto *ctor : classDecl->constructors) {
          // Si el tamaño de parámetros es 1, asume que solo tiene el implícito
          // 'this'
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
  return alloca;
}

llvm::Value *CodeGen::visit(const AssignNode *node) {
  llvm::Value *lval = getLValue(node->target);
  if (!lval) {
    diags.report({DiagLevel::Error, node->target->line, node->target->column,
                  node->target->length, "Failed to evaluate LHS of assignment.",
                  ""});
    return nullptr;
  }

  if (node->target->kind == NodeKind::Variable) {
    std::string_view varName =
        static_cast<const VariableNode *>(node->target)->name;
    SymbolInfo sym = cgCtx.lookupDetailed(varName);

    if (sym.isDirectAddress && llvm::isa<llvm::Argument>(lval)) {
      llvm::Argument *arg = llvm::cast<llvm::Argument>(lval);
      llvm::AllocaInst *alloca = createEntryBlockAlloca(
          arg->getType(), std::string(arg->getName()) + ".addr");
      builder.CreateStore(arg, alloca);

      cgCtx.bind(varName, alloca, true);
      lval = alloca;
    }
  }

  if (node->value->kind == NodeKind::FunctionCall) {
    auto *callNode = static_cast<const FunctionCallNode *>(node->value);
    if (callNode->target->kind == NodeKind::Variable) {
      if (callNode->resolvedFunc && callNode->resolvedFunc->isMethod &&
          callNode->resolvedFunc->returnType->isVoid()) {
        emitConstructorCall(callNode, lval);
        return builder.CreateLoad(getLLVMType(node->exprType), lval);
      }
    }
  }

  llvm::Value *rval = dispatch(node->value);
  if (!rval) {
    diags.report({DiagLevel::Error, node->value->line, node->value->column,
                  node->value->length, "Failed to evaluate RHS of assignment.",
                  ""});
    return nullptr;
  }

  llvm::Type *destTy = getLLVMType(node->target->exprType);
  rval = createImplicitCast(rval, destTy);
  builder.CreateStore(rval, lval);

  return rval;
}

llvm::Value *CodeGen::visit(const ParamDeclNode *node) { return nullptr; }

llvm::Value *CodeGen::visit(const FunctionDeclNode *node) {
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
    std::string_view pName = node->params[idx++]->name;
    arg.setName(pName);
    cgCtx.bind(pName, &arg, true);
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
      llvm::Value *objPtr = getLValue(ma->object);
      argsArgs.push_back(objPtr);
    } else if (node->resolvedFunc->isMethod) {
      llvm::Type *allocTy = getLLVMType(node->exprType);
      llvm::AllocaInst *instance = createEntryBlockAlloca(allocTy, "instance");

      emitDefaultInitialization(instance, node->exprType);

      argsArgs.push_back(instance);

      const auto *unqual = node->exprType->getUnqualifiedType();
      if (unqual->getKind() == TypeKind::Class) {
        auto *classTy = static_cast<const ClassType *>(unqual);
        auto *classDecl =
            static_cast<const ClassDeclNode *>(classTy->getDeclaration());
        if (classDecl && classDecl->destructor) {
          cgCtx.addCleanup(instance, classDecl->destructor);
        }
      }
    }
  } else {
    if (node->target->kind == NodeKind::Variable) {
      func = mod.getFunction(
          std::string(static_cast<const VariableNode *>(node->target)->name));
    }
    if (!func) {
      std::cerr
          << "[CodeGen Warning] Implicitly defining unlinked function call\n";
    }
  }

  unsigned argIdx = argsArgs.empty() ? 0 : 1;
  for (const auto &arg : node->args) {
    llvm::Value *argVal = nullptr;

    if (arg->kind == NodeKind::Variable && arg->exprType->isReferenceType()) {
      argVal = getLValue(arg);
    } else {
      argVal = dispatch(arg);
      if (func && argVal && argIdx < func->arg_size()) {
        llvm::Type *paramTy = func->getFunctionType()->getParamType(argIdx);
        argVal = createImplicitCast(argVal, paramTy);
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

  if (node->resolvedFunc && node->resolvedFunc->isMethod &&
      node->resolvedFunc->returnType->isVoid()) {
    if (!node->exprType->isVoid()) {
      return builder.CreateLoad(getLLVMType(node->exprType), argsArgs[0]);
    }
  }

  return callRes;
}

llvm::Value *CodeGen::visit(const CastNode *node) {
  llvm::Value *src = dispatch(node->expr);
  if (!src)
    return nullptr;

  llvm::Type *destTy = getLLVMType(node->targetType);
  return createImplicitCast(src, destTy);
}

llvm::Value *CodeGen::visit(const ReturnNode *node) {
  llvm::Value *retVal = nullptr;
  if (node->value) {
    bool isRefReturn =
        currentFunc && currentFunc->returnType->isReferenceType();

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

  /* Pass 2: Traverse and generate instructions for expressions, methods, and
   * functions. */
  for (const auto &stmt : node->statements) {
    dispatch(stmt);
  }

  return nullptr;
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
  builder.CreateStore(llvm::Constant::getNullValue(llTy), ptr);

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

    for (size_t i = 0; i < fields.size(); ++i) {
      auto *fieldDecl = fields[i];
      if (fieldDecl->initializer) {
        llvm::Value *initVal = dispatch(fieldDecl->initializer);
        if (initVal) {
          llvm::Type *destTy = getLLVMType(fieldDecl->type);
          initVal = createImplicitCast(initVal, destTy);
          llvm::Value *gep = builder.CreateStructGEP(
              llTy, ptr, i, std::string(fieldDecl->varName));
          builder.CreateStore(initVal, gep);
        }
      }
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
}

} // namespace utopia