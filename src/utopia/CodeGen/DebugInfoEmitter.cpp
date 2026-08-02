#include "utopia/CodeGen/DebugInfoEmitter.hpp"
#include "utopia/CodeGen/CodeGen.hpp"
#include <filesystem>

namespace fs = std::filesystem;

namespace utopia {

DebugInfoEmitter::DebugInfoEmitter(llvm::Module &mod, bool emitDebugInfo)
    : emitDebugInfo(emitDebugInfo), mod(mod) {
  if (emitDebugInfo) {
    dBuilder = std::make_unique<llvm::DIBuilder>(mod);
  }
}

void DebugInfoEmitter::initializeModule(const ModuleNode *node) {
  if (!emitDebugInfo)
    return;
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

void DebugInfoEmitter::finalize() {
  if (emitDebugInfo) {
    dBuilder->finalize();
    lexicalBlocks.pop_back();
  }
}

void DebugInfoEmitter::emitLocation(llvm::IRBuilder<> &builder,
                                    const ASTNode *node) {
  if (!emitDebugInfo || !node || node->line == 0)
    return;
  llvm::DIScope *scope = lexicalBlocks.empty() ? diCU : lexicalBlocks.back();
  builder.SetCurrentDebugLocation(llvm::DILocation::get(
      builder.getContext(), node->line, node->column, scope));
}

llvm::DIType *DebugInfoEmitter::getDIType(CodeGen &cg, const Type *type) {
  if (!type)
    return nullptr;
  if (debugTypes.contains(type))
    return debugTypes[type];

  llvm::DIType *diTy = nullptr;
  if (type->isBuiltinType()) {
    auto *bTy = static_cast<const BuiltinType *>(type);
    switch (bTy->getBuiltinKind()) {
    case BuiltinKind::TypeVal:
      diTy = dBuilder->createBasicType("Type", 8,
                                       llvm::dwarf::DW_ATE_unsigned_char);
      break;
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

    diTy = dBuilder->createPointerType(getDIType(cg, pointee), 64);
  } else if (type->getKind() == TypeKind::Array) {
    auto *arrTy = static_cast<const ArrayType *>(type);
    llvm::SmallVector<llvm::Metadata *, 1> subscripts;
    subscripts.push_back(dBuilder->getOrCreateSubrange(0, arrTy->getSize()));
    diTy = dBuilder->createArrayType(
        arrTy->getSize() *
            mod.getDataLayout().getTypeAllocSize(
                cg.getLLVMType(arrTy->getElementType())) *
            8,
        mod.getDataLayout()
                .getABITypeAlign(cg.getLLVMType(arrTy->getElementType()))
                .value() *
            8,
        getDIType(cg, arrTy->getElementType()),
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
        llvm::cast<llvm::StructType>(cg.getLLVMType(type));
    const llvm::StructLayout *layout = nullptr;
    if (!llStruct->isOpaque()) {
      layout = mod.getDataLayout().getStructLayout(llStruct);
    }

    for (const auto &f : recTy->getFields()) {
      uint64_t sizeInBits =
          mod.getDataLayout().getTypeAllocSizeInBits(cg.getLLVMType(f.type));
      uint32_t alignInBits =
          mod.getDataLayout().getABITypeAlign(cg.getLLVMType(f.type)).value() *
          8;
      uint64_t offsetInBits = (layout && type->getKind() != TypeKind::Union)
                                  ? layout->getElementOffsetInBits(f.index)
                                  : 0;
      elements.push_back(dBuilder->createMemberType(
          fwdDecl, f.name, diFile, 0, sizeInBits, alignInBits, offsetInBits,
          llvm::DINode::FlagZero, getDIType(cg, f.type)));
    }
    diTy = dBuilder->createStructType(
        diCU, recTy->getName(), diFile, 0, layout ? layout->getSizeInBits() : 0,
        layout ? layout->getAlignment().value() * 8 : 0, llvm::DINode::FlagZero,
        nullptr, dBuilder->getOrCreateArray(elements));
    dBuilder->replaceTemporary(llvm::TempDINode(fwdDecl), diTy);
  } else if (type->getKind() == TypeKind::Alias) {
    auto *alias = static_cast<const AliasType *>(type);
    diTy = dBuilder->createTypedef(getDIType(cg, alias->getTarget()),
                                   alias->getName(), diFile, 0, diCU);
  } else if (type->getKind() == TypeKind::Enum) {
    diTy = dBuilder->createBasicType(
        static_cast<const EnumType *>(type)->getName(), 32,
        llvm::dwarf::DW_ATE_signed);
  } else if (type->getKind() == TypeKind::Function) {
    auto *fTy = static_cast<const FunctionType *>(type);
    std::vector<llvm::Metadata *> types;
    types.push_back(getDIType(cg, fTy->getReturnType()));
    for (auto *p : fTy->getParamTypes())
      types.push_back(getDIType(cg, p));
    diTy =
        dBuilder->createSubroutineType(dBuilder->getOrCreateTypeArray(types));
  } else if (type->getKind() == TypeKind::Const) {
    auto *constTy = static_cast<const ConstType *>(type);
    diTy = dBuilder->createQualifiedType(llvm::dwarf::DW_TAG_const_type,
                                         getDIType(cg, constTy->getBaseType()));
  }

  debugTypes[type] = diTy;
  return diTy;
}

void DebugInfoEmitter::pushLexicalBlock(const ASTNode *node,
                                        llvm::LLVMContext &ctx) {
  if (emitDebugInfo && !lexicalBlocks.empty()) {
    llvm::DILexicalBlock *block = dBuilder->createLexicalBlock(
        lexicalBlocks.back(), diFile, node->line, node->column);
    lexicalBlocks.push_back(block);
  }
}

void DebugInfoEmitter::popLexicalBlock() {
  if (emitDebugInfo) {
    lexicalBlocks.pop_back();
  }
}

void DebugInfoEmitter::emitFunctionStart(CodeGen &cg, llvm::Function *func,
                                         const FunctionDeclNode *node) {
  if (!emitDebugInfo || !diFile)
    return;

  std::vector<llvm::Metadata *> paramTys;
  paramTys.push_back(getDIType(cg, node->returnType));
  if (node->isMethod && !node->isExtern && !node->isStatic &&
      node->parentRecord) {
    paramTys.push_back(
        dBuilder->createPointerType(getDIType(cg, node->parentRecord), 64));
  }
  for (const auto *p : node->params)
    paramTys.push_back(getDIType(cg, p->type));

  llvm::DISubroutineType *diFuncTy =
      dBuilder->createSubroutineType(dBuilder->getOrCreateTypeArray(paramTys));

  llvm::DISubprogram *sp = dBuilder->createFunction(
      diFile, node->name, func->getName(), diFile, node->line, diFuncTy,
      node->line, llvm::DINode::FlagPrototyped,
      llvm::DISubprogram::SPFlagDefinition);
  func->setSubprogram(sp);
  lexicalBlocks.push_back(sp);

  emitLocation(cg.builder, node);
}

void DebugInfoEmitter::emitFunctionEnd() {
  if (emitDebugInfo) {
    lexicalBlocks.pop_back();
  }
}

void DebugInfoEmitter::emitLocalVariable(CodeGen &cg,
                                         llvm::IRBuilder<> &builder,
                                         llvm::AllocaInst *alloca,
                                         const VarDeclNode *node) {
  if (!emitDebugInfo || lexicalBlocks.empty())
    return;
  auto *diTy = getDIType(cg, node->type);
  llvm::DILocalVariable *dVar = dBuilder->createAutoVariable(
      lexicalBlocks.back(), std::string(node->varName), diFile, node->line,
      diTy);
  dBuilder->insertDeclare(alloca, dVar, dBuilder->createExpression(),
                          llvm::DILocation::get(builder.getContext(),
                                                node->line, node->column,
                                                lexicalBlocks.back()),
                          builder.GetInsertBlock());
}

void DebugInfoEmitter::emitParameterVariable(CodeGen &cg,
                                             llvm::IRBuilder<> &builder,
                                             llvm::AllocaInst *alloca,
                                             const ParamDeclNode *node,
                                             unsigned argNo) {
  if (!emitDebugInfo || lexicalBlocks.empty())
    return;
  llvm::DILocalVariable *dVar = dBuilder->createParameterVariable(
      lexicalBlocks.back(), std::string(node->name), argNo, diFile, node->line,
      getDIType(cg, node->type));
  dBuilder->insertDeclare(alloca, dVar, dBuilder->createExpression(),
                          llvm::DILocation::get(builder.getContext(),
                                                node->line, node->column,
                                                lexicalBlocks.back()),
                          builder.GetInsertBlock());
}

void DebugInfoEmitter::emitGlobalVariable(CodeGen &cg,
                                          llvm::GlobalVariable *gvar,
                                          const VarDeclNode *node,
                                          const std::string &bindName) {
  if (!emitDebugInfo)
    return;
  auto *diTy = getDIType(cg, node->type);
  auto *gve = dBuilder->createGlobalVariableExpression(
      diCU, bindName, bindName, diFile, node->line, diTy, false);
  gvar->addDebugInfo(gve);
}

} // namespace utopia