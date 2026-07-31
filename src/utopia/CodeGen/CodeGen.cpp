#include "utopia/CodeGen/CodeGen.hpp"
#include <filesystem>
#include <iostream>
#include <llvm/ADT/APSInt.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/ModRef.h>
#include <optional>
#include <string>
namespace fs = std::filesystem;

namespace utopia {

CodeGen::CodeGen(BackendContext &bCtx, llvm::Module &llvmMod,
                 DiagnosticsEngine &diags, bool emitDebugInfo,
                 std::string filePath)
    : backend(bCtx), ctx(bCtx.getLLVMContext()), mod(llvmMod), builder(ctx),
      diags(diags), mdBuilder(ctx), emitDebugInfo(emitDebugInfo),
      currentFilePath(std::move(filePath)) {
  tbaaRoot = mdBuilder.createTBAARoot("Utopia TBAA");

  llvm::FastMathFlags fmf;
  fmf.setFast();
  builder.setFastMathFlags(fmf);

  if (emitDebugInfo) {
    dBuilder = std::make_unique<llvm::DIBuilder>(mod);
  }
}

llvm::Value *CodeGen::dispatch(const ASTNode *node) {
  if (!node)
    return nullptr;
  emitLocation(node);
  return ASTVisitor<CodeGen, llvm::Value *>::dispatch(node);
}

void CodeGen::emitLocation(const ASTNode *node) {
  if (!emitDebugInfo || !node)
    return;
  if (node->line == 0)
    return;
  llvm::DIScope *scope = lexicalBlocks.empty() ? diCU : lexicalBlocks.back();
  builder.SetCurrentDebugLocation(
      llvm::DILocation::get(ctx, node->line, node->column, scope));
}

llvm::DIType *CodeGen::getDIType(const Type *type) {
  if (!type)
    return nullptr;
  if (debugTypes.contains(type))
    return debugTypes[type];

  llvm::DIType *diTy = nullptr;
  if (type->isBuiltinType()) {
    auto *bTy = static_cast<const BuiltinType *>(type);
    switch (bTy->getBuiltinKind()) {
    case BuiltinKind::Int8:
      diTy = dBuilder->createBasicType("int8", 8, llvm::dwarf::DW_ATE_signed);
      break;
    case BuiltinKind::UInt8:
      diTy =
          dBuilder->createBasicType("uint8", 8, llvm::dwarf::DW_ATE_unsigned);
      break;
    case BuiltinKind::Int16:
      diTy = dBuilder->createBasicType("int16", 16, llvm::dwarf::DW_ATE_signed);
      break;
    case BuiltinKind::UInt16:
      diTy =
          dBuilder->createBasicType("uint16", 16, llvm::dwarf::DW_ATE_unsigned);
      break;
    case BuiltinKind::Int32:
      diTy = dBuilder->createBasicType("int32", 32, llvm::dwarf::DW_ATE_signed);
      break;
    case BuiltinKind::UInt32:
      diTy =
          dBuilder->createBasicType("uint32", 32, llvm::dwarf::DW_ATE_unsigned);
      break;
    case BuiltinKind::Int64:
      diTy = dBuilder->createBasicType("int64", 64, llvm::dwarf::DW_ATE_signed);
      break;
    case BuiltinKind::UInt64:
      diTy =
          dBuilder->createBasicType("uint64", 64, llvm::dwarf::DW_ATE_unsigned);
      break;
    case BuiltinKind::Float32:
      diTy =
          dBuilder->createBasicType("float32", 32, llvm::dwarf::DW_ATE_float);
      break;
    case BuiltinKind::Float64:
      diTy =
          dBuilder->createBasicType("float64", 64, llvm::dwarf::DW_ATE_float);
      break;
    case BuiltinKind::Bool:
      diTy = dBuilder->createBasicType("bool", 8, llvm::dwarf::DW_ATE_boolean);
      break;
    case BuiltinKind::Void:
      diTy = nullptr;
      break;
    }
  } else if (type->isPointerType() || type->isReferenceType() ||
             type->getKind() == TypeKind::RValueReference) {
    const Type *pointee = nullptr;
    if (type->isPointerType())
      pointee = static_cast<const PointerType *>(type)->getPointeeType();
    else if (type->isReferenceType())
      pointee = static_cast<const ReferenceType *>(type)->getPointeeType();
    else
      pointee =
          static_cast<const RValueReferenceType *>(type)->getPointeeType();

    diTy = dBuilder->createPointerType(getDIType(pointee), 64);
  } else if (type->getKind() == TypeKind::Array) {
    auto *arrTy = static_cast<const ArrayType *>(type);
    llvm::SmallVector<llvm::Metadata *, 1> subscripts;
    subscripts.push_back(dBuilder->getOrCreateSubrange(0, arrTy->getSize()));
    diTy = dBuilder->createArrayType(
        arrTy->getSize() *
            mod.getDataLayout().getTypeAllocSize(
                getLLVMType(arrTy->getElementType())) *
            8,
        mod.getDataLayout()
                .getABITypeAlign(getLLVMType(arrTy->getElementType()))
                .value() *
            8,
        getDIType(arrTy->getElementType()),
        dBuilder->getOrCreateArray(subscripts));
  } else if (type->getKind() == TypeKind::Struct ||
             type->getKind() == TypeKind::Class ||
             type->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(type);
    unsigned tag = llvm::dwarf::DW_TAG_structure_type;
    if (type->getKind() == TypeKind::Union)
      tag = llvm::dwarf::DW_TAG_union_type;
    auto *fwdDecl = dBuilder->createReplaceableCompositeType(
        tag, recTy->getName(), diCU, diFile, 0);
    debugTypes[type] = fwdDecl;

    std::vector<llvm::Metadata *> elements;
    llvm::StructType *llStruct =
        llvm::cast<llvm::StructType>(getLLVMType(type));
    const llvm::StructLayout *layout = nullptr;
    if (!llStruct->isOpaque()) {
      layout = mod.getDataLayout().getStructLayout(llStruct);
    }

    for (const auto &f : recTy->getFields()) {
      uint64_t sizeInBits =
          mod.getDataLayout().getTypeAllocSizeInBits(getLLVMType(f.type));
      uint32_t alignInBits =
          mod.getDataLayout().getABITypeAlign(getLLVMType(f.type)).value() * 8;
      uint64_t offsetInBits = (layout && type->getKind() != TypeKind::Union)
                                  ? layout->getElementOffsetInBits(f.index)
                                  : 0;
      elements.push_back(dBuilder->createMemberType(
          fwdDecl, f.name, diFile, 0, sizeInBits, alignInBits, offsetInBits,
          llvm::DINode::FlagZero, getDIType(f.type)));
    }
    diTy = dBuilder->createStructType(
        diCU, recTy->getName(), diFile, 0, layout ? layout->getSizeInBits() : 0,
        layout ? layout->getAlignment().value() * 8 : 0, llvm::DINode::FlagZero,
        nullptr, dBuilder->getOrCreateArray(elements));
    dBuilder->replaceTemporary(llvm::TempDINode(fwdDecl), diTy);
  } else if (type->getKind() == TypeKind::Alias) {
    auto *alias = static_cast<const AliasType *>(type);
    diTy = dBuilder->createTypedef(getDIType(alias->getTarget()),
                                   alias->getName(), diFile, 0, diCU);
  } else if (type->getKind() == TypeKind::Enum) {
    diTy = dBuilder->createBasicType(
        static_cast<const EnumType *>(type)->getName(), 32,
        llvm::dwarf::DW_ATE_signed);
  } else if (type->getKind() == TypeKind::Function) {
    auto *fTy = static_cast<const FunctionType *>(type);
    std::vector<llvm::Metadata *> types;
    types.push_back(getDIType(fTy->getReturnType()));
    for (auto *p : fTy->getParamTypes())
      types.push_back(getDIType(p));
    diTy =
        dBuilder->createSubroutineType(dBuilder->getOrCreateTypeArray(types));
  } else if (type->getKind() == TypeKind::Const) {
    auto *constTy = static_cast<const ConstType *>(type);
    diTy = dBuilder->createQualifiedType(llvm::dwarf::DW_TAG_const_type,
                                         getDIType(constTy->getBaseType()));
  }

  debugTypes[type] = diTy;
  return diTy;
}

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
      if (p->getKind() == TypeKind::Array) {
        paramTys.push_back(builder.getPtrTy());
      } else {
        paramTys.push_back(getLLVMType(p));
      }
    }
    return llvm::FunctionType::get(getLLVMType(fTy->getReturnType()), paramTys,
                                   false);
  }

  if (type->getKind() == TypeKind::TemplateParam) {
    diags.report({DiagLevel::Error, 0, 0, 0,
                  "Uninstantiated template parameter '" + type->toString() +
                      "' reached code generation.",
                  currentFilePath});
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
             type->getKind() == TypeKind::Class ||
             type->getKind() == TypeKind::Union) {
    auto rec = static_cast<const RecordType *>(type);

    llvm::StructType *structTy =
        llvm::StructType::getTypeByName(ctx, rec->getName());

    /* Register the opaque type first to support self-referential structures
     * and recursive pointers gracefully without infinite loops. */
    if (!structTy) {
      structTy = llvm::StructType::create(ctx, rec->getName());
    }

    if (structTy->isOpaque() && !rec->isOpaque()) {
      std::vector<llvm::Type *> elements;

      if (type->getKind() == TypeKind::Union) {
        uint64_t maxSize = 0;
        llvm::Type *maxAlignType = builder.getInt8Ty();
        uint64_t maxAlign = 1;
        for (const auto &f : rec->getFields()) {
          llvm::Type *fTy = getLLVMType(f.type);
          uint64_t sz = mod.getDataLayout().getTypeAllocSize(fTy);
          uint64_t al = mod.getDataLayout().getABITypeAlign(fTy).value();
          if (sz > maxSize)
            maxSize = sz;
          if (al > maxAlign) {
            maxAlign = al;
            maxAlignType = fTy;
          }
        }

        if (!rec->getFields().empty()) {
          elements.push_back(maxAlignType);
          uint64_t paddedSize = (maxSize + maxAlign - 1) & ~(maxAlign - 1);
          uint64_t alignSize =
              mod.getDataLayout().getTypeAllocSize(maxAlignType);
          if (paddedSize > alignSize) {
            elements.push_back(llvm::ArrayType::get(builder.getInt8Ty(),
                                                    paddedSize - alignSize));
          }
        }
        structTy->setBody(elements, false);
      } else {
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
    }

    return structTy;
  }

  diags.report({DiagLevel::Error, 0, 0, 0,
                "Unsupported or unresolved type reached code generation: " +
                    type->toString(),
                currentFilePath});
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

  if (node->isMethod && !node->isExtern && !node->isStatic &&
      node->parentRecord) {
    paramTypes.push_back(builder.getPtrTy());
  }

  for (const auto *p : node->params) {
    if (p->type->getKind() == TypeKind::Array) {
      paramTypes.push_back(builder.getPtrTy());
    } else {
      paramTypes.push_back(getLLVMType(p->type));
    }
  }

  llvm::FunctionType *funcType = llvm::FunctionType::get(
      getLLVMType(node->returnType), paramTypes, node->isVariadic);

  func = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                irName, mod);

  func->addFnAttr(llvm::Attribute::NoUnwind);

  /* Map standard inline to an LLVM hint, and forceInline to AlwaysInline */
  for (const auto *ann : node->annotations) {
    if (ann->name == "inline")
      func->addFnAttr(llvm::Attribute::InlineHint);
    else if (ann->name == "forceInline")
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
            uint64_t sz = std::stoull(std::string(num->raw), nullptr, 0);
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

  unsigned argIdx = (node->isMethod && !node->isExtern && !node->isStatic &&
                     node->parentRecord)
                        ? 1
                        : 0;
  for (const auto *param : node->params) {
    addRefAttributes(param->type, argIdx, param->annotations);
    argIdx++;
  }

  return func;
}

void CodeGen::emitLoopCleanups(size_t targetDepth) {
  auto allScopes = cgCtx.getAllScopes();
  size_t currentDepth = allScopes.size();
  for (auto scopeIt = allScopes.rbegin();
       scopeIt != allScopes.rend() && currentDepth > targetDepth;
       ++scopeIt, --currentDepth) {
    for (auto cleanupIt = scopeIt->cleanups.rbegin();
         cleanupIt != scopeIt->cleanups.rend(); ++cleanupIt) {
      emitCleanupCall(cleanupIt->instancePtr, cleanupIt->destructor);
    }
    for (auto lifeIt = scopeIt->lifetimes.rbegin();
         lifeIt != scopeIt->lifetimes.rend(); ++lifeIt) {
      emitLifetimeEnd(lifeIt->allocaInst, lifeIt->size);
    }
  }
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

  unsigned llArgIdx = 1;
  unsigned astParamIdx = 0;

  for (const auto &arg : node->args) {
    llvm::Value *argVal = nullptr;

    bool isRefParam = false;
    if (node->resolvedFunc && astParamIdx < node->resolvedFunc->params.size()) {
      isRefParam =
          node->resolvedFunc->params[astParamIdx]->type->isReferenceType() ||
          node->resolvedFunc->params[astParamIdx]->type->getKind() ==
              TypeKind::RValueReference;
    }

    if (isRefParam) {
      argVal = getLValue(arg);
      if (!argVal) {
        lastTemporaryAlloca = nullptr;
        llvm::Value *val = dispatch(arg);
        if (lastTemporaryAlloca) {
          argVal = lastTemporaryAlloca;
          lastTemporaryAlloca = nullptr;
        } else {
          argVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
          builder.CreateStore(val, argVal);
        }
      }
    } else {
      lastTemporaryAlloca = nullptr;
      argVal = dispatch(arg);
      lastTemporaryAlloca = nullptr;

      if (func && argVal && llArgIdx < func->arg_size()) {
        llvm::Type *paramTy = func->getFunctionType()->getParamType(llArgIdx);
        argVal = createImplicitCast(argVal, paramTy);
      }
    }

    if (!argVal) {
      diags.report({DiagLevel::Error, arg->line, arg->column, arg->length,
                    "Failed to evaluate argument for constructor call.",
                    currentFilePath});
      return;
    }
    argsArgs.push_back(argVal);
    llArgIdx++;
    astParamIdx++;
  }

  builder.CreateCall(func, argsArgs);
}

llvm::Constant *CodeGen::evaluateAsConstant(const ExprNode *node) {
  if (!node)
    return nullptr;

  if (node->kind == NodeKind::Null) {
    return llvm::ConstantPointerNull::get(builder.getPtrTy());
  }

  if (node->kind == NodeKind::String) {
    auto *strNode = static_cast<const StringNode *>(node);
    llvm::Constant *strConst = llvm::ConstantDataArray::getString(
        ctx, llvm::StringRef(strNode->value.data(), strNode->value.length()),
        true);
    auto *gv = new llvm::GlobalVariable(mod, strConst->getType(), true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        strConst, ".str");
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    llvm::Constant *zero = llvm::ConstantInt::get(builder.getInt32Ty(), 0);
    return llvm::ConstantExpr::getInBoundsGetElementPtr(
        strConst->getType(), gv, llvm::ArrayRef<llvm::Constant *>{zero, zero});
  }

  if (node->kind == NodeKind::ImplicitCast || node->kind == NodeKind::Cast) {
    const ExprNode *innerExpr = nullptr;
    const Type *targetType = nullptr;
    if (node->kind == NodeKind::ImplicitCast) {
      auto *castNode = static_cast<const ImplicitCastNode *>(node);
      innerExpr = castNode->expr;
      targetType = castNode->targetType;
    } else {
      auto *castNode = static_cast<const CastNode *>(node);
      innerExpr = castNode->expr;
      targetType = castNode->targetType;
    }

    llvm::Constant *inner = evaluateAsConstant(innerExpr);
    if (inner) {
      llvm::Type *destTy = getLLVMType(targetType);
      if (inner->getType() == destTy)
        return inner;

      // Pointer-based constant expressions are still supported in LLVM 15+
      if (inner->getType()->isPointerTy() && destTy->isPointerTy())
        return llvm::ConstantExpr::getBitCast(inner, destTy);
      if (inner->getType()->isPointerTy() && destTy->isIntegerTy())
        return llvm::ConstantExpr::getPtrToInt(inner, destTy);
      if (inner->getType()->isIntegerTy() && destTy->isPointerTy())
        return llvm::ConstantExpr::getIntToPtr(inner, destTy);

      if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(inner)) {
        if (destTy->isIntegerTy()) {
          return llvm::ConstantInt::get(
              ctx, ci->getValue().sextOrTrunc(destTy->getIntegerBitWidth()));
        } else if (destTy->isFloatingPointTy()) {
          llvm::APFloat apf(destTy->getFltSemantics());
          apf.convertFromAPInt(ci->getValue(), true,
                               llvm::APFloat::rmNearestTiesToEven);
          return llvm::ConstantFP::get(ctx, apf);
        }
      } else if (auto *cfp = llvm::dyn_cast<llvm::ConstantFP>(inner)) {
        if (destTy->isFloatingPointTy()) {
          llvm::APFloat apf = cfp->getValueAPF();
          bool losesInfo;
          apf.convert(destTy->getFltSemantics(),
                      llvm::APFloat::rmNearestTiesToEven, &losesInfo);
          return llvm::ConstantFP::get(ctx, apf);
        } else if (destTy->isIntegerTy()) {
          llvm::APSInt api(destTy->getIntegerBitWidth(), false);
          bool isExact;
          cfp->getValueAPF().convertToInteger(api, llvm::APFloat::rmTowardZero,
                                              &isExact);
          return llvm::ConstantInt::get(ctx, api);
        }
      }

      return llvm::ConstantExpr::getBitCast(inner, destTy);
    }
  }

  if (node->kind == NodeKind::ArrayLiteral) {
    auto *arrNode = static_cast<const ArrayLiteralNode *>(node);
    std::vector<llvm::Constant *> elems;
    for (const auto *elem : arrNode->elements) {
      auto *c = evaluateAsConstant(elem);
      if (!c)
        return nullptr;
      elems.push_back(c);
    }
    llvm::Type *destTy = getLLVMType(arrNode->exprType);
    if (!destTy->isArrayTy())
      return nullptr;
    return llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(destTy), elems);
  }

  if (node->kind == NodeKind::Boolean) {
    auto *boolNode = static_cast<const BoolNode *>(node);
    return boolNode->value ? llvm::ConstantInt::getTrue(ctx)
                           : llvm::ConstantInt::getFalse(ctx);
  }

  if (node->kind == NodeKind::Number) {
    auto *num = static_cast<const NumberNode *>(node);
    std::string numStr(num->raw);

    bool isHex = false;
    uint8_t radix = 10;
    if (numStr.length() > 2 && numStr[0] == '0' &&
        (numStr[1] == 'x' || numStr[1] == 'X')) {
      isHex = true;
      radix = 16;
      numStr = numStr.substr(2); // Remove "0x"
    }

    while (!numStr.empty()) {
      char back = numStr.back();
      if (back == 'u' || back == 'U' || back == 'l' || back == 'L') {
        numStr.pop_back();
      } else if (!isHex && (back == 'f' || back == 'F')) {
        numStr.pop_back();
      } else {
        break;
      }
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
        return llvm::ConstantInt::get(intTy, llvm::StringRef(numStr), radix);
      }
      return llvm::ConstantInt::get(builder.getInt32Ty(),
                                    llvm::StringRef(numStr), radix);
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
    std::string lookupName = std::string(varNode->name);

    if (varNode->resolvedDecl &&
        varNode->resolvedDecl->kind == NodeKind::VarDecl) {
      auto *varDecl = static_cast<const VarDeclNode *>(varNode->resolvedDecl);
      if (varDecl->isStatic && !varDecl->mangledName.empty()) {
        lookupName = varDecl->mangledName;
      }
    }

    SymbolInfo sym = cgCtx.lookupDetailed(lookupName);

    if (sym.value) {
      /* Resolve scalar values directly from global variable initializers */
      if (auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(sym.value)) {
        if (gv->isConstant() && gv->hasInitializer()) {
          return gv->getInitializer();
        }
      }
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
      std::string lookupName = std::string(varNode->name);

      if (varNode->resolvedDecl &&
          varNode->resolvedDecl->kind == NodeKind::VarDecl) {
        auto *varDecl = static_cast<const VarDeclNode *>(varNode->resolvedDecl);
        if (varDecl->isStatic && !varDecl->mangledName.empty()) {
          lookupName = varDecl->mangledName;
        }
      }

      SymbolInfo sym = cgCtx.lookupDetailed(lookupName);
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

llvm::Value *CodeGen::visit(const ImplicitCastNode *node) {
  const Type *objectType = node->targetType;
  if (objectType->isReferenceType()) {
    objectType =
        static_cast<const ReferenceType *>(objectType)->getPointeeType();
  } else if (objectType->getKind() == TypeKind::RValueReference) {
    objectType =
        static_cast<const RValueReferenceType *>(objectType)->getPointeeType();
  }

  llvm::Type *llTy = getLLVMType(objectType);

  llvm::AllocaInst *temp = createEntryBlockAlloca(llTy, "implicit.cast.tmp");
  emitDefaultInitialization(temp, objectType);

  const Type *unqual = objectType->getUnqualifiedType();
  if (unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqual);
    if (auto *decl = recTy->getDeclaration()) {
      const FunctionDeclNode *dtor = nullptr;
      if (decl->kind == NodeKind::ClassDecl)
        dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::StructDecl)
        dtor = static_cast<const StructDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::UnionDecl)
        dtor = static_cast<const UnionDeclNode *>(decl)->destructor;

      if (dtor) {
        cgCtx.addCleanup(temp, dtor);
        lastTemporaryAlloca = temp;
      }
    }
  }

  llvm::Function *ctorFunc = getOrCreateFunction(node->conversionConstructor);
  std::vector<llvm::Value *> argsArgs;
  argsArgs.push_back(temp);

  llvm::Value *argVal = nullptr;
  bool isRefParam = false;
  if (node->conversionConstructor->params.size() > 0) {
    const Type *pType = node->conversionConstructor->params[0]->type;
    isRefParam = pType->isReferenceType() ||
                 pType->getKind() == TypeKind::RValueReference;
  }

  if (isRefParam) {
    argVal = getLValue(node->expr);
    if (!argVal) {
      llvm::Value *val = dispatch(node->expr);
      argVal = createEntryBlockAlloca(val->getType(), "tmp.implicit.arg");
      builder.CreateStore(val, argVal);
    }
  } else {
    argVal = dispatch(node->expr);
    if (ctorFunc && argVal && ctorFunc->arg_size() > 1) {
      llvm::Type *paramTy = ctorFunc->getFunctionType()->getParamType(1);
      argVal = createImplicitCast(argVal, paramTy);
    }
  }

  argsArgs.push_back(argVal);
  builder.CreateCall(ctorFunc, argsArgs);

  if (!node->exprType->isVoid()) {
    if (node->exprType->isReferenceType() ||
        node->exprType->getKind() == TypeKind::RValueReference) {
      return temp;
    }
    return createTBAALoad(getLLVMType(node->exprType), temp, node->exprType);
  }

  return temp;
}

llvm::Value *CodeGen::getLValue(const ExprNode *node) {
  if (!node)
    return nullptr;

  if (node->kind == NodeKind::ArraySubscript) {
    auto *subNode = static_cast<const ArraySubscriptNode *>(node);

    if (subNode->overloadedOperator) {
      llvm::Value *objPtr = nullptr;
      if (subNode->base->exprType->isPointerType()) {
        objPtr = dispatch(subNode->base);
      } else {
        objPtr = getLValue(subNode->base);
      }

      if (!objPtr) {
        llvm::Value *val = dispatch(subNode->base);
        objPtr = createEntryBlockAlloca(val->getType(), "tmp.op.recv");
        builder.CreateStore(val, objPtr);
      }

      llvm::Function *func = getOrCreateFunction(subNode->overloadedOperator);
      std::vector<llvm::Value *> argsArgs;
      argsArgs.push_back(objPtr);

      llvm::Value *idxVal = dispatch(subNode->index);
      llvm::Type *paramTy =
          getLLVMType(subNode->overloadedOperator->params[0]->type);
      idxVal = createImplicitCast(idxVal, paramTy);
      argsArgs.push_back(idxVal);

      return builder.CreateCall(func, argsArgs);
    }

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

      if (varNode->parentType->getKind() == TypeKind::Union) {
        return thisPtr;
      }

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
      diags.report(
          {DiagLevel::Error, varNode->line, varNode->column, varNode->length,
           "Unbound symbol in l-value resolution: '" + lookupName + "'.",
           currentFilePath});
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
                      "Unbound static field.", currentFilePath});
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
    llvm::Value *gep = objPtr;

    if (baseTy->getKind() != TypeKind::Union) {
      gep = builder.CreateStructGEP(llvmBaseTy, objPtr, maNode->fieldIndex,
                                    maNode->memberName);
    }

    if (node->exprType->getKind() == TypeKind::Array) {
      llvm::Type *arrTy = getLLVMType(node->exprType);
      return builder.CreateInBoundsGEP(
          arrTy, gep, {builder.getInt32(0), builder.getInt32(0)});
    }

    /* Return the memory address (pointer) directly for valid L-Value
     * assignments */
    return gep;
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

  if (emitDebugInfo && !lexicalBlocks.empty()) {
    llvm::DILexicalBlock *block = dBuilder->createLexicalBlock(
        lexicalBlocks.back(), diFile, node->line, node->column);
    lexicalBlocks.push_back(block);
  }

  for (const auto *stmt : node->statements) {
    dispatch(stmt);
    if (builder.GetInsertBlock()->getTerminator()) {
      break;
    }
  }

  if (!builder.GetInsertBlock()->getTerminator()) {
    emitScopeCleanups();
  }

  if (emitDebugInfo) {
    lexicalBlocks.pop_back();
  }

  return nullptr;
}

llvm::Value *CodeGen::visit(const NumberNode *node) {
  std::string numStr(node->raw);

  bool isHex = false;
  uint8_t radix = 10;
  if (numStr.length() > 2 && numStr[0] == '0' &&
      (numStr[1] == 'x' || numStr[1] == 'X')) {
    isHex = true;
    radix = 16;
    numStr = numStr.substr(2); // Remove "0x"
  }

  while (!numStr.empty()) {
    char back = numStr.back();
    if (back == 'u' || back == 'U' || back == 'l' || back == 'L') {
      numStr.pop_back();
    } else if (!isHex && (back == 'f' || back == 'F')) {
      numStr.pop_back();
    } else {
      break;
    }
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
    return llvm::ConstantInt::get(intTy, llvm::StringRef(numStr), radix);
  }
  return llvm::ConstantInt::get(builder.getInt32Ty(), llvm::StringRef(numStr),
                                radix);
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

llvm::Value *CodeGen::visit(const UnionDeclNode *node) {
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
    llvm::Value *gep = thisPtr;

    if (node->parentType->getKind() != TypeKind::Union) {
      gep = builder.CreateStructGEP(llvmBaseTy, thisPtr, node->fieldIndex,
                                    node->name);
    }
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
  llvm::Value *gep = objPtr;

  if (baseTy->getKind() != TypeKind::Union) {
    gep = builder.CreateStructGEP(llvmBaseTy, objPtr, node->fieldIndex,
                                  node->memberName);
  }

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

  cgCtx.pushLoop(incBB, endBB);
  dispatch(node->body);
  cgCtx.popLoop();

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

  /* Ensures cleanups are executed for the loop initializer scope */
  emitScopeCleanups();

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

  cgCtx.pushLoop(condBB, endBB);
  dispatch(node->body);
  cgCtx.popLoop();

  if (!builder.GetInsertBlock()->getTerminator()) {
    builder.CreateBr(condBB);
  }

  theFunction->insert(theFunction->end(), endBB);
  builder.SetInsertPoint(endBB);

  return nullptr;
}

llvm::Value *CodeGen::visit(const SwitchNode *node) {
  llvm::Value *condV = dispatch(node->condition);
  if (!condV)
    return nullptr;

  llvm::Type *condTy = getLLVMType(node->condition->exprType);
  condV = createImplicitCast(condV, condTy);

  llvm::Function *theFunction = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *endBB =
      llvm::BasicBlock::Create(ctx, "switch.end", theFunction);
  llvm::BasicBlock *defaultBB = endBB;

  /* Phase 1: Pre-allocate basic blocks to map the switch table targets */
  std::vector<llvm::BasicBlock *> caseBlocks;
  for (size_t i = 0; i < node->cases.size(); ++i) {
    llvm::BasicBlock *bb =
        llvm::BasicBlock::Create(ctx, "switch.case", theFunction);
    caseBlocks.push_back(bb);
    if (!node->cases[i]->value) {
      defaultBB = bb;
    }
  }

  llvm::SwitchInst *switchInst =
      builder.CreateSwitch(condV, defaultBB, node->cases.size());

  CGScopeGuard guard(cgCtx);
  /* Push break-target context. Nullptr distinguishes it from loops for
   * 'continue' commands */
  cgCtx.pushLoop(nullptr, endBB);

  /* Phase 2: Emit block internals and configure explicit fallthrough links */
  for (size_t i = 0; i < node->cases.size(); ++i) {
    const auto *c = node->cases[i];

    if (c->value) {
      llvm::Constant *caseConst = evaluateAsConstant(c->value);
      if (!caseConst) {
        diags.report({DiagLevel::Error, c->value->line, c->value->column,
                      c->value->length,
                      "Case value must evaluate to a compile-time constant",
                      currentFilePath});
        return nullptr;
      }

      if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(caseConst)) {
        switchInst->addCase(ci, caseBlocks[i]);
      } else {
        diags.report({DiagLevel::Error, c->value->line, c->value->column,
                      c->value->length,
                      "Switch cases only support integer and enum constants",
                      currentFilePath});
        return nullptr;
      }
    }

    /* Emulate C-ABI Implicit Fallthrough constraint */
    if (i > 0 && !builder.GetInsertBlock()->getTerminator()) {
      builder.CreateBr(caseBlocks[i]);
    }

    builder.SetInsertPoint(caseBlocks[i]);

    for (const auto *stmt : c->statements) {
      dispatch(stmt);
      if (builder.GetInsertBlock()->getTerminator()) {
        break;
      }
    }
  }

  /* Secure block termination if the final case lacks a break/return */
  if (!builder.GetInsertBlock()->getTerminator()) {
    builder.CreateBr(endBB);
  }

  cgCtx.popLoop();

  endBB->moveAfter(builder.GetInsertBlock());
  builder.SetInsertPoint(endBB);

  return nullptr;
}

llvm::Value *CodeGen::visit(const BreakNode *node) {
  const auto &loop = cgCtx.getCurrentLoop();
  emitLoopCleanups(loop.scopeDepth);
  builder.CreateBr(loop.breakBlock);
  return nullptr;
}

llvm::Value *CodeGen::visit(const ContinueNode *node) {
  const auto &loop = cgCtx.getCurrentLoopForContinue();
  emitLoopCleanups(loop.scopeDepth);
  builder.CreateBr(loop.continueBlock);
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
                    "Invalid operand for dereference.", currentFilePath});
      return nullptr;
    }
    llvm::Type *loadTy = getLLVMType(node->exprType);
    return createTBAALoad(loadTy, ptr, node->exprType);
  }
  if (node->op == "-") {
    llvm::Value *val = dispatch(node->expr);
    if (!val) {
      diags.report({DiagLevel::Error, node->line, node->column, node->length,
                    "Invalid operand for unary minus.", currentFilePath});
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
           "Invalid operand for " + std::string(node->op) + " operator.",
           currentFilePath});
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
    const FunctionDeclNode *opFunc = node->overloadedOperator;
    llvm::Function *func = getOrCreateFunction(opFunc);
    std::vector<llvm::Value *> argsArgs;

    if (opFunc->isMethod && !opFunc->isExtern && !opFunc->isStatic) {
      llvm::Value *objPtr = nullptr;
      if (node->left->exprType->isPointerType()) {
        objPtr = dispatch(node->left);
      } else {
        objPtr = getLValue(node->left);
      }

      if (!objPtr) {
        lastTemporaryAlloca = nullptr;
        llvm::Value *val = dispatch(node->left);
        if (lastTemporaryAlloca) {
          objPtr = lastTemporaryAlloca;
          lastTemporaryAlloca = nullptr;
        } else {
          objPtr = createEntryBlockAlloca(val->getType(), "tmp.op.recv");
          builder.CreateStore(val, objPtr);
        }
      }
      argsArgs.push_back(objPtr);

      llvm::Value *rhsVal = nullptr;
      bool isRefParam = false;
      if (opFunc->params.size() > 0) {
        isRefParam =
            opFunc->params[0]->type->isReferenceType() ||
            opFunc->params[0]->type->getKind() == TypeKind::RValueReference;
      }

      if (isRefParam) {
        rhsVal = getLValue(node->right);
        if (!rhsVal) {
          lastTemporaryAlloca = nullptr;
          llvm::Value *val = dispatch(node->right);
          if (lastTemporaryAlloca) {
            rhsVal = lastTemporaryAlloca;
            lastTemporaryAlloca = nullptr;
          } else {
            rhsVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
            builder.CreateStore(val, rhsVal);
          }
        }
      } else {
        lastTemporaryAlloca = nullptr;
        rhsVal = dispatch(node->right);
        lastTemporaryAlloca = nullptr;

        llvm::Type *paramTy = func->getFunctionType()->getParamType(1);
        rhsVal = createImplicitCast(rhsVal, paramTy);
      }
      argsArgs.push_back(rhsVal);
    } else {
      llvm::Value *lhsVal = nullptr;
      bool isRefParamL = false;
      if (opFunc->params.size() > 0) {
        isRefParamL =
            opFunc->params[0]->type->isReferenceType() ||
            opFunc->params[0]->type->getKind() == TypeKind::RValueReference;
      }

      if (isRefParamL) {
        lhsVal = getLValue(node->left);
        if (!lhsVal) {
          lastTemporaryAlloca = nullptr;
          llvm::Value *val = dispatch(node->left);
          if (lastTemporaryAlloca) {
            lhsVal = lastTemporaryAlloca;
            lastTemporaryAlloca = nullptr;
          } else {
            lhsVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg.l");
            builder.CreateStore(val, lhsVal);
          }
        }
      } else {
        lastTemporaryAlloca = nullptr;
        lhsVal = dispatch(node->left);
        lastTemporaryAlloca = nullptr;

        llvm::Type *paramTyL = func->getFunctionType()->getParamType(0);
        lhsVal = createImplicitCast(lhsVal, paramTyL);
      }
      argsArgs.push_back(lhsVal);

      llvm::Value *rhsVal = nullptr;
      bool isRefParamR = false;
      if (opFunc->params.size() > 1) {
        isRefParamR =
            opFunc->params[1]->type->isReferenceType() ||
            opFunc->params[1]->type->getKind() == TypeKind::RValueReference;
      }

      if (isRefParamR) {
        rhsVal = getLValue(node->right);
        if (!rhsVal) {
          lastTemporaryAlloca = nullptr;
          llvm::Value *val = dispatch(node->right);
          if (lastTemporaryAlloca) {
            rhsVal = lastTemporaryAlloca;
            lastTemporaryAlloca = nullptr;
          } else {
            rhsVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg.r");
            builder.CreateStore(val, rhsVal);
          }
        }
      } else {
        lastTemporaryAlloca = nullptr;
        rhsVal = dispatch(node->right);
        lastTemporaryAlloca = nullptr;

        llvm::Type *paramTyR = func->getFunctionType()->getParamType(1);
        rhsVal = createImplicitCast(rhsVal, paramTyR);
      }
      argsArgs.push_back(rhsVal);
    }

    auto res = builder.CreateCall(func, argsArgs);
    lastTemporaryAlloca = nullptr;
    return res;
  }

  llvm::Value *L = dispatch(node->left);
  llvm::Value *R = dispatch(node->right);
  lastTemporaryAlloca = nullptr;

  if (!L || !R) {
    diags.report({DiagLevel::Error, node->line, node->column, node->length,
                  "Failed to generate binary operands.", currentFilePath});
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
      elemTyForAlign->getKind() == TypeKind::Class ||
      elemTyForAlign->getKind() == TypeKind::Union) {
    if (const DeclNode *decl =
            static_cast<const RecordType *>(elemTyForAlign)->getDeclaration()) {
      for (const auto *ann : decl->annotations) {
        if (ann->name == "align" && !ann->args.empty() &&
            ann->args[0]->kind == NodeKind::Number) {
          uint64_t typeAlign = std::stoull(
              std::string(static_cast<const NumberNode *>(ann->args[0])->raw),
              nullptr, 0);
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
          std::string(static_cast<const NumberNode *>(ann->args[0])->raw),
          nullptr, 0);
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
                    currentFilePath});
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
                    "Dynamic global references are not supported.",
                    currentFilePath});
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

    if (emitDebugInfo && !lexicalBlocks.empty()) {
      auto *diTy = getDIType(node->type);
      llvm::DILocalVariable *dVar = dBuilder->createAutoVariable(
          lexicalBlocks.back(), std::string(node->varName), diFile, node->line,
          diTy);
      dBuilder->insertDeclare(alloca, dVar, dBuilder->createExpression(),
                              llvm::DILocation::get(ctx, node->line,
                                                    node->column,
                                                    lexicalBlocks.back()),
                              builder.GetInsertBlock());
    }

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
             currentFilePath});
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

    if (emitDebugInfo) {
      auto *diTy = getDIType(node->type);
      auto *gve = dBuilder->createGlobalVariableExpression(
          diCU, bindName, bindName, diFile, node->line, diTy, false);
      gvar->addDebugInfo(gve);
    }

    cgCtx.bind(bindName, gvar, true);
    return gvar;
  }

  llvm::AllocaInst *alloca =
      createEntryBlockAlloca(ty, std::string(node->varName));

  if (customAlign > 0) {
    alloca->setAlignment(llvm::Align(customAlign));
  }

  if (emitDebugInfo && !lexicalBlocks.empty()) {
    auto *diTy = getDIType(node->type);
    llvm::DILocalVariable *dVar = dBuilder->createAutoVariable(
        lexicalBlocks.back(), std::string(node->varName), diFile, node->line,
        diTy);
    dBuilder->insertDeclare(alloca, dVar, dBuilder->createExpression(),
                            llvm::DILocation::get(ctx, node->line, node->column,
                                                  lexicalBlocks.back()),
                            builder.GetInsertBlock());
  }

  cgCtx.bind(node->varName, alloca, true);

  uint64_t allocSize = mod.getDataLayout().getTypeAllocSize(ty);
  emitLifetimeStart(alloca, allocSize);
  cgCtx.addLifetime(alloca, allocSize);

  const auto *unqualTy = node->type->getUnqualifiedType();
  if (unqualTy->getKind() == TypeKind::Class ||
      unqualTy->getKind() == TypeKind::Struct ||
      unqualTy->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqualTy);
    auto *decl = recTy->getDeclaration();
    const FunctionDeclNode *dtor = nullptr;
    if (decl) {
      if (decl->kind == NodeKind::ClassDecl)
        dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::StructDecl)
        dtor = static_cast<const StructDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::UnionDecl)
        dtor = static_cast<const UnionDeclNode *>(decl)->destructor;
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
                            unqualTy->getKind() == TypeKind::Class ||
                            unqualTy->getKind() == TypeKind::Union);

        if (isAggregate && node->copyCtor) {
          llvm::Value *rvalAddr = getLValue(node->initializer);
          if (!rvalAddr) {
            /* Map to the underlying temporary object to enable safe moves */
            lastTemporaryAlloca = nullptr;
            llvm::Value *initVal = dispatch(node->initializer);
            if (lastTemporaryAlloca) {
              rvalAddr = lastTemporaryAlloca;
              lastTemporaryAlloca = nullptr;
            } else if (initVal) {
              rvalAddr = createEntryBlockAlloca(ty, "tmp.copy.src");
              createTBAAStore(initVal, rvalAddr, node->type);
            }
          }

          if (rvalAddr) {
            llvm::Function *ctorFunc = getOrCreateFunction(node->copyCtor);
            builder.CreateCall(ctorFunc, {alloca, rvalAddr});
          } else {
            diags.report({DiagLevel::Error, node->line, node->column,
                          node->length,
                          "Failed to resolve source for copy constructor.",
                          currentFilePath});
          }
        } else if (isAggregate) {
          /* Fallback generic memcpy for records lacking a custom destructor */
          llvm::Value *rvalAddr = getLValue(node->initializer);
          if (rvalAddr) {
            llvm::Align align = mod.getDataLayout().getABITypeAlign(ty);
            builder.CreateMemCpy(alloca, align, rvalAddr, align, allocSize);
          } else {
            lastTemporaryAlloca = nullptr;
            llvm::Value *initVal = dispatch(node->initializer);
            lastTemporaryAlloca = nullptr;
            if (initVal) {
              createTBAAStore(initVal, alloca, node->type);
            } else {
              diags.report({DiagLevel::Error, node->line, node->column,
                            node->length, "Initialization failed for variable.",
                            currentFilePath});
            }
          }
        } else {
          lastTemporaryAlloca = nullptr;
          llvm::Value *initVal = dispatch(node->initializer);
          lastTemporaryAlloca = nullptr;
          if (initVal) {
            initVal = createImplicitCast(initVal, ty);
            createTBAAStore(initVal, alloca, node->type);
          } else {
            diags.report({DiagLevel::Error, node->line, node->column,
                          node->length, "Initialization failed for variable.",
                          currentFilePath});
          }
        }
      }
    }
  } else {
    /* Leave arrays strictly uninitialized when declared without initializers */
    if (node->type->getKind() != TypeKind::Array) {
      emitDefaultInitialization(alloca, node->type);

      if (unqualTy->getKind() == TypeKind::Class ||
          unqualTy->getKind() == TypeKind::Struct ||
          unqualTy->getKind() == TypeKind::Union) {
        auto *recTy = static_cast<const RecordType *>(unqualTy);
        auto *decl = recTy->getDeclaration();
        if (decl) {
          const FunctionDeclNode *emptyCtor = nullptr;
          llvm::ArrayRef<FunctionDeclNode *> ctors;
          if (decl->kind == NodeKind::ClassDecl)
            ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
          else if (decl->kind == NodeKind::StructDecl)
            ctors = static_cast<const StructDeclNode *>(decl)->constructors;
          else if (decl->kind == NodeKind::UnionDecl)
            ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

          for (auto *ctor : ctors) {
            if (ctor->params.empty()) {
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
                    node->target->length, "LHS is not an l-value",
                    currentFilePath});
      return nullptr;
    }

    llvm::Function *func = getOrCreateFunction(node->overloadedOperator);
    std::vector<llvm::Value *> argsArgs;
    argsArgs.push_back(objPtr);

    llvm::Value *rhsVal = nullptr;
    bool isRefParam = false;
    if (node->overloadedOperator->params.size() > 0) {
      isRefParam =
          node->overloadedOperator->params[0]->type->isReferenceType() ||
          node->overloadedOperator->params[0]->type->getKind() ==
              TypeKind::RValueReference;
    }

    if (isRefParam) {
      rhsVal = getLValue(node->value);
      if (!rhsVal) {
        lastTemporaryAlloca = nullptr;
        llvm::Value *val = dispatch(node->value);
        if (lastTemporaryAlloca) {
          rhsVal = lastTemporaryAlloca;
          lastTemporaryAlloca = nullptr;
        } else {
          rhsVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
          builder.CreateStore(val, rhsVal);
        }
      }
    } else {
      lastTemporaryAlloca = nullptr;
      rhsVal = dispatch(node->value);
      lastTemporaryAlloca = nullptr;

      llvm::Type *paramTy = func->getFunctionType()->getParamType(1);
      rhsVal = createImplicitCast(rhsVal, paramTy);
    }
    argsArgs.push_back(rhsVal);

    auto res = builder.CreateCall(func, argsArgs);
    lastTemporaryAlloca = nullptr;
    return res;
  }

  llvm::Value *lval = getLValue(node->target);
  if (!lval) {
    diags.report({DiagLevel::Error, node->target->line, node->target->column,
                  node->target->length, "Failed to evaluate LHS of assignment.",
                  currentFilePath});
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
                      unqualTargetTy->getKind() == TypeKind::Union ||
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
  lastTemporaryAlloca = nullptr;

  if (!rval) {
    diags.report({DiagLevel::Error, node->value->line, node->value->column,
                  node->value->length, "Failed to evaluate RHS of assignment.",
                  currentFilePath});
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

  if (emitDebugInfo && diFile) {
    std::vector<llvm::Metadata *> paramTys;
    paramTys.push_back(getDIType(node->returnType));
    if (node->isMethod && !node->isExtern && !node->isStatic &&
        node->parentRecord) {
      paramTys.push_back(
          dBuilder->createPointerType(getDIType(node->parentRecord), 64));
    }
    for (const auto *p : node->params)
      paramTys.push_back(getDIType(p->type));

    llvm::DISubroutineType *diFuncTy = dBuilder->createSubroutineType(
        dBuilder->getOrCreateTypeArray(paramTys));

    llvm::DISubprogram *sp = dBuilder->createFunction(
        diFile, node->name, func->getName(), diFile, node->line, diFuncTy,
        node->line, llvm::DINode::FlagPrototyped,
        llvm::DISubprogram::SPFlagDefinition);
    func->setSubprogram(sp);
    lexicalBlocks.push_back(sp);
    emitLocation(node);
  }

  llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", func);
  builder.SetInsertPoint(entry);

  CGScopeGuard guard(cgCtx);

  unsigned astParamIdx = 0;
  auto argIt = func->arg_begin();

  if (node->isMethod && !node->isExtern && !node->isStatic &&
      node->parentRecord) {
    argIt->setName("this");
    llvm::AllocaInst *alloca =
        createEntryBlockAlloca(argIt->getType(), "this.addr");
    builder.CreateStore(&*argIt, alloca);
    cgCtx.bind("this", alloca, true);

    if (emitDebugInfo) {
      auto *diTy =
          dBuilder->createPointerType(getDIType(node->parentRecord), 64);
      llvm::DILocalVariable *dVar = dBuilder->createParameterVariable(
          lexicalBlocks.back(), "this", 1, diFile, node->line, diTy,
          llvm::DINode::FlagArtificial | llvm::DINode::FlagObjectPointer);
      dBuilder->insertDeclare(alloca, dVar, dBuilder->createExpression(),
                              llvm::DILocation::get(ctx, node->line,
                                                    node->column,
                                                    lexicalBlocks.back()),
                              builder.GetInsertBlock());
    }

    ++argIt;
  }

  for (; argIt != func->arg_end(); ++argIt) {
    const ParamDeclNode *paramDecl = node->params[astParamIdx];
    std::string_view pName = paramDecl->name;
    argIt->setName(pName);

    llvm::Type *argType = argIt->getType();
    llvm::AllocaInst *alloca =
        createEntryBlockAlloca(argType, std::string(pName) + ".addr");
    builder.CreateStore(&*argIt, alloca);

    if (emitDebugInfo) {
      unsigned argNo =
          astParamIdx + (node->isMethod && !node->isStatic ? 2 : 1);
      llvm::DILocalVariable *dVar = dBuilder->createParameterVariable(
          lexicalBlocks.back(), std::string(pName), argNo, diFile,
          paramDecl->line, getDIType(paramDecl->type));
      dBuilder->insertDeclare(alloca, dVar, dBuilder->createExpression(),
                              llvm::DILocation::get(ctx, paramDecl->line,
                                                    paramDecl->column,
                                                    lexicalBlocks.back()),
                              builder.GetInsertBlock());
    }

    bool isRef = paramDecl->type->isReferenceType() ||
                 paramDecl->type->getKind() == TypeKind::RValueReference;
    cgCtx.bind(pName, alloca, !isRef);
    astParamIdx++;
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

  if (emitDebugInfo) {
    lexicalBlocks.pop_back();
  }

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

      lastTemporaryAlloca = instance;

      emitDefaultInitialization(instance, node->exprType);

      argsArgs.push_back(instance);

      const auto *unqual = node->exprType->getUnqualifiedType();
      if (unqual->getKind() == TypeKind::Class ||
          unqual->getKind() == TypeKind::Struct ||
          unqual->getKind() == TypeKind::Union) {
        auto *recTy = static_cast<const RecordType *>(unqual);
        auto *decl = recTy->getDeclaration();
        if (decl) {
          const FunctionDeclNode *dtor = nullptr;
          if (decl->kind == NodeKind::ClassDecl)
            dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
          else if (decl->kind == NodeKind::StructDecl)
            dtor = static_cast<const StructDeclNode *>(decl)->destructor;
          else if (decl->kind == NodeKind::UnionDecl)
            dtor = static_cast<const UnionDeclNode *>(decl)->destructor;

          if (dtor) {
            cgCtx.addCleanup(instance, dtor);
          }
        }
      }
    }

    unsigned llArgIdx = argsArgs.empty() ? 0 : argsArgs.size();
    unsigned astParamIdx = 0;

    for (const auto &arg : node->args) {
      llvm::Value *argVal = nullptr;

      bool isRefParam = false;
      if (node->resolvedFunc &&
          astParamIdx < node->resolvedFunc->params.size()) {
        isRefParam =
            node->resolvedFunc->params[astParamIdx]->type->isReferenceType() ||
            node->resolvedFunc->params[astParamIdx]->type->getKind() ==
                TypeKind::RValueReference;
      }

      if (isRefParam) {
        argVal = getLValue(arg);
        if (!argVal) {
          lastTemporaryAlloca = nullptr;
          llvm::Value *val = dispatch(arg);
          if (lastTemporaryAlloca) {
            argVal = lastTemporaryAlloca;
            lastTemporaryAlloca = nullptr;
          } else {
            argVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
            builder.CreateStore(val, argVal);
          }
        }
      } else {
        lastTemporaryAlloca = nullptr;
        argVal = dispatch(arg);
        lastTemporaryAlloca = nullptr;

        if (func && argVal) {
          if (llArgIdx < func->arg_size()) {
            llvm::Type *paramTy =
                func->getFunctionType()->getParamType(llArgIdx);
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
                      "Failed to evaluate argument for function call.",
                      currentFilePath});
        return nullptr;
      }
      argsArgs.push_back(argVal);
      llArgIdx++;
      astParamIdx++;
    }

    auto callRes = builder.CreateCall(func, argsArgs);
    lastTemporaryAlloca = nullptr;

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
                      "Unresolved l-value in reference return.",
                      currentFilePath});
        return nullptr;
      }
    } else {
      lastTemporaryAlloca = nullptr; // Reset before dispatch
      retVal = dispatch(node->value);
      if (!retVal) {
        diags.report({DiagLevel::Error, node->line, node->column, node->length,
                      "Failed to evaluate return expression.",
                      currentFilePath});
        return nullptr;
      }
      if (currentFunc) {
        llvm::Type *destTy = getLLVMType(currentFunc->returnType);
        retVal = createImplicitCast(retVal, destTy);
      }

      /* RVO / Return Escape: Prevent local variables and constructed
       * temporaries from being destroyed if they are being returned by value */
      if (node->value->kind == NodeKind::Variable) {
        if (llvm::Value *lval = getLValue(node->value)) {
          cgCtx.removeCleanup(lval);
        }
      } else if (lastTemporaryAlloca) {
        cgCtx.removeCleanup(lastTemporaryAlloca);
        lastTemporaryAlloca = nullptr;
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
  if (emitDebugInfo) {
    mod.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                      llvm::DEBUG_METADATA_VERSION);
    mod.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
    fs::path p(node->filePath);
    diFile =
        dBuilder->createFile(p.filename().string(), p.parent_path().string());
    diCU = dBuilder->createCompileUnit(llvm::dwarf::DW_LANG_C, diFile,
                                       "Utopia Compiler", false, "", 0);
    lexicalBlocks.push_back(diCU);
  }

  std::unordered_set<const ModuleNode *> visitedDeps;

  /* Recursively traverses the full module hierarchy to establish external
   * linkages for global functions and variables, respecting export visibility
   */
  auto declareGlobals = [&](const ModuleNode *m, auto &self) -> void {
    if (!m || visitedDeps.contains(m))
      return;
    visitedDeps.insert(m);

    if (m != node) {
      for (const auto &stmt : m->statements) {
        if (stmt->kind == NodeKind::FunctionDecl) {
          getOrCreateFunction(static_cast<const FunctionDeclNode *>(stmt));
        } else if (stmt->kind == NodeKind::VarDecl) {
          auto *varDecl = static_cast<const VarDeclNode *>(stmt);
          if (varDecl->isGlobal) {
            llvm::Type *ty = getLLVMType(varDecl->type);
            std::string bindName = varDecl->mangledName.empty()
                                       ? std::string(varDecl->varName)
                                       : varDecl->mangledName;
            llvm::GlobalVariable *gvar = mod.getGlobalVariable(bindName);
            if (!gvar) {
              gvar = new llvm::GlobalVariable(
                  mod, ty, varDecl->type->isConstQualified(),
                  llvm::GlobalValue::ExternalLinkage, nullptr, bindName);
            }
            cgCtx.bind(bindName, gvar, true);
          }
        }
      }
    }

    for (const auto *imp : m->importedModules) {
      self(imp, self);
    }
    for (const auto *exp : m->exportedModules) {
      self(exp, self);
    }
  };

  declareGlobals(node, declareGlobals);

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

  if (emitDebugInfo) {
    dBuilder->finalize();
    lexicalBlocks.pop_back();
  }

  return nullptr;
}

llvm::Value *CodeGen::visit(const ArraySubscriptNode *node) {
  if (node->overloadedOperator) {
    llvm::Value *objPtr = nullptr;
    if (node->base->exprType->isPointerType()) {
      objPtr = dispatch(node->base);
    } else {
      objPtr = getLValue(node->base);
    }

    if (!objPtr) {
      llvm::Value *val = dispatch(node->base);
      objPtr = createEntryBlockAlloca(val->getType(), "tmp.op.recv");
      builder.CreateStore(val, objPtr);
    }

    llvm::Function *func = getOrCreateFunction(node->overloadedOperator);
    std::vector<llvm::Value *> argsArgs;
    argsArgs.push_back(objPtr);

    llvm::Value *idxVal = dispatch(node->index);
    llvm::Type *paramTy = func->getFunctionType()->getParamType(1);
    idxVal = createImplicitCast(idxVal, paramTy);
    argsArgs.push_back(idxVal);

    llvm::Value *res = builder.CreateCall(func, argsArgs);

    if (node->exprType->isReferenceType() ||
        node->exprType->getKind() == TypeKind::RValueReference) {
      const Type *loadTy =
          static_cast<const ReferenceType *>(node->exprType)->getPointeeType();
      return createTBAALoad(getLLVMType(loadTy), res, loadTy);
    }
    return res;
  }

  llvm::Value *lval = getLValue(node);
  if (!lval)
    return nullptr;

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

  return createTBAALoad(getLLVMType(loadTy), lval, loadTy);
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

        unsigned llArgIdx = 1;
        unsigned astParamIdx = 0;

        for (const auto &arg : node->args) {
          llvm::Value *argVal = nullptr;

          bool isRefParam = false;
          if (node->resolvedConstructor &&
              astParamIdx < node->resolvedConstructor->params.size()) {
            isRefParam = node->resolvedConstructor->params[astParamIdx]
                             ->type->isReferenceType() ||
                         node->resolvedConstructor->params[astParamIdx]
                                 ->type->getKind() == TypeKind::RValueReference;
          }

          if (isRefParam) {
            argVal = getLValue(arg);
            if (!argVal) {
              lastTemporaryAlloca = nullptr;
              llvm::Value *val = dispatch(arg);
              if (lastTemporaryAlloca) {
                argVal = lastTemporaryAlloca;
                lastTemporaryAlloca = nullptr;
              } else {
                argVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
                builder.CreateStore(val, argVal);
              }
            }
          } else {
            lastTemporaryAlloca = nullptr;
            argVal = dispatch(arg);
            lastTemporaryAlloca = nullptr;

            if (ctorFunc && argVal && llArgIdx < ctorFunc->arg_size()) {
              llvm::Type *paramTy =
                  ctorFunc->getFunctionType()->getParamType(llArgIdx);
              argVal = createImplicitCast(argVal, paramTy);
            }
          }

          if (!argVal) {
            diags.report({DiagLevel::Error, arg->line, arg->column, arg->length,
                          "Failed to evaluate argument for constructor call.",
                          currentFilePath});
            return nullptr;
          }
          argsArgs.push_back(argVal);
          llArgIdx++;
          astParamIdx++;
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
        unqual->getKind() == TypeKind::Struct ||
        unqual->getKind() == TypeKind::Union) {
      auto *recTy = static_cast<const RecordType *>(unqual);

      if (auto *decl = recTy->getDeclaration()) {
        if (decl->kind == NodeKind::ClassDecl) {
          dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
        } else if (decl->kind == NodeKind::StructDecl) {
          dtor = static_cast<const StructDeclNode *>(decl)->destructor;
        } else if (decl->kind == NodeKind::UnionDecl) {
          dtor = static_cast<const UnionDeclNode *>(decl)->destructor;
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
          unqual->getKind() == TypeKind::Struct ||
          unqual->getKind() == TypeKind::Union) {
        auto *recTy = static_cast<const RecordType *>(unqual);

        if (auto *decl = recTy->getDeclaration()) {
          const FunctionDeclNode *dtor = nullptr;
          if (decl->kind == NodeKind::ClassDecl) {
            dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
          } else if (decl->kind == NodeKind::StructDecl) {
            dtor = static_cast<const StructDeclNode *>(decl)->destructor;
          } else if (decl->kind == NodeKind::UnionDecl) {
            dtor = static_cast<const UnionDeclNode *>(decl)->destructor;
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
             unqual->getKind() == TypeKind::Struct ||
             unqual->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqual);

    if (unqual->getKind() == TypeKind::Union) {
      node = mdBuilder.createTBAAScalarTypeNode(recTy->getName(), charNode);
    } else {
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
    }
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
      unqual->getKind() == TypeKind::Union ||
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

    if (baseTy->getKind() == TypeKind::Union) {
      return getTBAAAccessTag(node->exprType);
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
      if (varNode->parentType->getKind() == TypeKind::Union) {
        return getTBAAAccessTag(node->exprType);
      }

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
                  currentFilePath});
  }

  if (size > 0) {
    llvm::Align align = mod.getDataLayout().getABITypeAlign(llTy);
    builder.CreateMemSet(ptr, builder.getInt8(0), size, align);
  }

  const auto *unqual = type->getUnqualifiedType();
  if (unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqual);
    auto *decl = recTy->getDeclaration();
    if (!decl)
      return;

    llvm::ArrayRef<VarDeclNode *> fields;
    if (decl->kind == NodeKind::StructDecl) {
      fields = static_cast<const StructDeclNode *>(decl)->fields;
    } else if (decl->kind == NodeKind::ClassDecl) {
      fields = static_cast<const ClassDeclNode *>(decl)->fields;
    } else if (decl->kind == NodeKind::UnionDecl) {
      fields = static_cast<const UnionDeclNode *>(decl)->fields;
    }

    llvm::StructType *llRecTy = llvm::cast<llvm::StructType>(llTy);
    const llvm::StructLayout *layout = nullptr;
    if (type->getKind() != TypeKind::Union) {
      layout = mod.getDataLayout().getStructLayout(llRecTy);
    }

    unsigned instanceIdx = 0;
    for (size_t i = 0; i < fields.size(); ++i) {
      auto *fieldDecl = fields[i];

      if (fieldDecl->isStatic) {
        continue;
      }

      if (fieldDecl->initializer) {
        llvm::Value *initVal = dispatch(fieldDecl->initializer);
        if (initVal) {
          llvm::Type *destTy = getLLVMType(fieldDecl->type);
          initVal = createImplicitCast(initVal, destTy);

          llvm::Value *gep = ptr;
          uint64_t offset = 0;

          if (type->getKind() != TypeKind::Union) {
            gep = builder.CreateStructGEP(llTy, ptr, instanceIdx,
                                          std::string(fieldDecl->varName));
            offset = layout->getElementOffset(instanceIdx);
          }

          llvm::MDNode *tbaaTag =
              getTBAAStructAccessTag(type, fieldDecl->type, offset);

          createTBAAStore(initVal, gep, tbaaTag);
        }
      }

      instanceIdx++;
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