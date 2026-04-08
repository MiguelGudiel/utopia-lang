#include "utopia/CodeGen/CodeGen.hpp"
#include <filesystem>
#include <iostream>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <optional>

namespace utopia {

CodeGen::CodeGen(const std::string &sourceFile, bool isDebug)
    : isDebug(isDebug) {
  context = std::make_unique<llvm::LLVMContext>();
  module = std::make_unique<llvm::Module>("UtopiaModule", *context);
  builder = std::make_unique<llvm::IRBuilder<>>(*context);

  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();

  std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
  llvm::Triple targetTriple(targetTripleStr);
  module->setTargetTriple(targetTriple);

  std::string error;
  auto target = llvm::TargetRegistry::lookupTarget(targetTripleStr, error);
  if (!target) {
    std::cerr << "LLVM Error: " << error << "\n";
    exit(1);
  }

  auto cpu = "generic";
  auto features = "";
  llvm::TargetOptions opt;
  std::optional<llvm::Reloc::Model> relocModel = llvm::Reloc::PIC_;

  targetMachine =
      std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
          targetTriple, cpu, features, opt, relocModel));

  module->setDataLayout(targetMachine->createDataLayout());

  if (this->isDebug) {
    // Le decimos a LLVM qué versión de DWARF usar (La 4 es muy compatible con
    // VS Code/LLDB)
    module->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                          llvm::DEBUG_METADATA_VERSION);
    module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);

    dbgBuilder = std::make_unique<llvm::DIBuilder>(*module);

    std::filesystem::path p(sourceFile);
    dbgFile =
        dbgBuilder->createFile(p.filename().string(), p.parent_path().string());

    // Crea la unidad de compilación (DW_LANG_C es un truco seguro para
    // lenguajes custom)
    dbgCU = dbgBuilder->createCompileUnit(llvm::dwarf::DW_LANG_C, dbgFile,
                                          "Utopia Compiler", false, "", 0);
  }
}

void CodeGen::registerModules(const std::vector<ModuleNode *> &allModules) {
  // Construye las estructuras opacas para todo el programa
  for (auto *mod : allModules) {
    for (auto &var : mod->globalVars) {
      globalVarTypes[var->name] = parseTypeString(var->typeName);
    }
    for (auto &st : mod->structs) {
      structASTs[st->name] = st.get();
      structTypes[st->name] = llvm::StructType::create(*context, st->name);
    }
  }

  for (auto *mod : allModules) {
    for (auto &st : mod->structs) {
      std::vector<llvm::Type *> body;
      int idx = 0;

      auto buildMemoryLayout = [&](std::string className, auto &self) -> void {
        StructDeclNode *currentAST = structASTs[className];
        if (!currentAST->baseClass.empty() &&
            structASTs.count(currentAST->baseClass)) {
          self(currentAST->baseClass, self);
        }
        for (auto &f : currentAST->fields) {
          if (f.isStatic)
            continue;

          TypeInfo t = parseTypeString(f.typeName);
          body.push_back(getLLVMType(t));
          structMemberIndices[st->name][f.name] = idx++;
          structMemberTypes[st->name][f.name] = t;
        }
      };

      buildMemoryLayout(st->name, buildMemoryLayout);
      structTypes[st->name]->setBody(body);
    }

    for (auto &func : mod->functions) {
      std::string mangledName = func->name;
      std::vector<TypeInfo> paramTypes;
      for (auto &arg : func->args) {
        paramTypes.push_back(parseTypeString(arg.type));
        mangledName += "_" + getMangledType(paramTypes.back());
      }
      functionParamTypes[mangledName] = paramTypes;
      functionTypes[mangledName] = parseTypeString(func->returnType);
    }

    for (auto &st : mod->structs) {
      for (auto &method : st->methods) {
        std::string mangledName = st->name + "_" + method->name;
        std::vector<TypeInfo> paramTypes;
        if (!method->isStatic && !method->isConstructor) {
          TypeInfo thisType = {st->name, 1, false};
          paramTypes.push_back(thisType);
          mangledName += "_" + getMangledType(thisType);
        }
        if (method->isConstructor) {
          TypeInfo thisType = {st->name, 1, false};
          paramTypes.push_back(thisType);
          mangledName += "_" + getMangledType(thisType);
        }
        for (auto &arg : method->args) {
          TypeInfo t = arg.isThisAssign ? structMemberTypes[st->name][arg.name]
                                        : parseTypeString(arg.type);
          paramTypes.push_back(t);
          mangledName += "_" + getMangledType(t);
        }
        functionParamTypes[mangledName] = paramTypes;
        functionTypes[mangledName] = parseTypeString(method->returnType);
      }
    }

    for (auto &ext : mod->extensions) {
      for (auto &method : ext->methods) {
        std::string mangledName =
            "ext_" + ext->targetTypedef + "_" + method->name;
        std::vector<TypeInfo> paramTypes;

        TypeInfo targetType = parseTypeString(ext->targetTypedef);
        paramTypes.push_back(targetType);
        mangledName += "_" + getMangledType(targetType);

        for (auto &arg : method->args) {
          TypeInfo t = arg.isThisAssign
                           ? structMemberTypes[ext->targetTypedef][arg.name]
                           : parseTypeString(arg.type);
          paramTypes.push_back(t);
          mangledName += "_" + getMangledType(t);
        }
        functionParamTypes[mangledName] = paramTypes;
        functionTypes[mangledName] = parseTypeString(method->returnType);
      }
    }
  }
}

void CodeGen::generate(ModuleNode *astModule, const std::string &outputObjPath,
                       const std::vector<ModuleNode *> &allModules) {
  valueScopes.clear();
  typeScopes.clear();
  stringPool.clear();
  breakTargets.clear();
  continueTargets.clear();
  loopScopeDepths.clear();
  currentVal = nullptr;
  currentType = TypeInfo{};
  currentClass = "";
  currentReturnType = TypeInfo{};
  currentLValue = nullptr;
  isLValueContext = false;
  rvoTarget = nullptr;
  dbgScopes.clear();

  // Recrear el módulo LLVM para este módulo AST
  this->module = std::make_unique<llvm::Module>(astModule->filename, *context);
  this->module->setTargetTriple(targetMachine->getTargetTriple());
  this->module->setDataLayout(targetMachine->createDataLayout());

  if (isDebug) {
    this->module->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                                llvm::DEBUG_METADATA_VERSION);
    this->module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
    // Reconfigurar debug info
    dbgBuilder = std::make_unique<llvm::DIBuilder>(*this->module);
    std::filesystem::path p(astModule->filename);
    dbgFile =
        dbgBuilder->createFile(p.filename().string(), p.parent_path().string());
    dbgCU = dbgBuilder->createCompileUnit(llvm::dwarf::DW_LANG_C, dbgFile,
                                          "Utopia Compiler", false, "", 0);
  }

  registerModules(allModules);

  // Recorrer el AST del módulo
  astModule->accept(this);

  std::cerr << "LLVM module has " << module->getFunctionList().size()
            << " functions\n";

  if (isDebug)
    dbgBuilder->finalize();

  // Emitir el objeto
  emitObjectFile(outputObjPath);
}

// --------------------------------------------------------------------------
// Scope Management
// --------------------------------------------------------------------------

void CodeGen::enterScope() {
  valueScopes.push_back({});
  typeScopes.push_back({});
}

void CodeGen::exitScope() {
  emitScopeCleanup(valueScopes.size() - 1);
  valueScopes.pop_back();
  typeScopes.pop_back();
}

llvm::Value *CodeGen::lookupValue(const std::string &name) {
  for (auto it = valueScopes.rbegin(); it != valueScopes.rend(); ++it) {
    for (auto rit = it->rbegin(); rit != it->rend(); ++rit) {
      if (rit->first == name)
        return rit->second;
    }
  }
  return nullptr;
}

TypeInfo CodeGen::lookupType(const std::string &name) {
  for (auto it = typeScopes.rbegin(); it != typeScopes.rend(); ++it) {
    if (it->count(name))
      return (*it)[name];
  }
  return {"error"};
}

void CodeGen::emitLocation(ASTNode *node) {
  if (!isDebug || !node) {
    builder->SetCurrentDebugLocation(llvm::DebugLoc());
    return;
  }
  llvm::DIScope *scope = dbgScopes.empty() ? dbgFile : dbgScopes.back();
  builder->SetCurrentDebugLocation(
      llvm::DILocation::get(*context, node->line, node->column, scope));
}

llvm::DIType *CodeGen::getDebugType(const TypeInfo &type) {
  if (!isDebug)
    return nullptr;

  if (type.isPointer()) {
    /* * Pointer unwrapping
     * Pelamos un nivel de indirección y referenciamos el tipo base
     * para que LLDB pueda expandir los punteros.
     */
    TypeInfo pointee = type;
    pointee.ptrDepth = 0;
    pointee.arrayDimensions = 0;
    pointee.isReference = false;
    pointee.isRValueRef = false;
    return dbgBuilder->createPointerType(getDebugType(pointee), 64);
  }

  if (type.base == "int" || type.base == "uint")
    return dbgBuilder->createBasicType("int", 32, llvm::dwarf::DW_ATE_signed);
  if (type.base == "float")
    return dbgBuilder->createBasicType("float", 64, llvm::dwarf::DW_ATE_float);
  if (type.base == "bool")
    return dbgBuilder->createBasicType("bool", 8, llvm::dwarf::DW_ATE_boolean);
  if (type.base == "String")
    return dbgBuilder->createPointerType(
        dbgBuilder->createBasicType("char", 8, llvm::dwarf::DW_ATE_signed_char),
        64);

  if (structTypes.count(type.base)) {
    // Retornamos el caché si ya fue construido para evitar recursión infinita
    if (debugTypes.count(type.base))
      return debugTypes[type.base];

    llvm::StructType *llStruct = structTypes[type.base];
    uint64_t sizeInBits =
        module->getDataLayout().getTypeAllocSizeInBits(llStruct);

    /*
     * FORWARD DECLARATION HACK
     * Registramos la estructura opaca de inmediato en el caché.
     * Luego inyectaremos los miembros usando replaceArrays.
     */
    llvm::DICompositeType *structDbgType = dbgBuilder->createStructType(
        dbgFile, type.base, dbgFile, 0, sizeInBits, 8, llvm::DINode::FlagZero,
        nullptr, llvm::DINodeArray());

    debugTypes[type.base] = structDbgType;

    std::vector<llvm::Metadata *> elements;
    auto layout = module->getDataLayout().getStructLayout(llStruct);

    // Mapeo inverso: Garantiza que los campos DWARF se registren en el
    // orden exacto de memoria que definió LLVM.
    std::map<int, std::string> orderedFields;
    for (const auto &[fName, fIdx] : structMemberIndices[type.base]) {
      orderedFields[fIdx] = fName;
    }

    for (const auto &[fIdx, fName] : orderedFields) {
      TypeInfo fType = structMemberTypes[type.base][fName];
      llvm::DIType *fieldDbgType = getDebugType(fType);

      uint64_t fieldSize =
          module->getDataLayout().getTypeAllocSizeInBits(getLLVMType(fType));
      uint64_t fieldOffset = layout->getElementOffsetInBits(fIdx);

      elements.push_back(dbgBuilder->createMemberType(
          structDbgType, fName, dbgFile, 0, fieldSize, 8, fieldOffset,
          llvm::DINode::FlagZero, fieldDbgType));
    }

    dbgBuilder->replaceArrays(structDbgType,
                              dbgBuilder->getOrCreateArray(elements));
    return structDbgType;
  }

  // Fallback seguro
  return dbgBuilder->createBasicType(type.base, 64,
                                     llvm::dwarf::DW_ATE_address);
}

// --------------------------------------------------------------------------
// Utils & Intrinsics
// --------------------------------------------------------------------------

llvm::FunctionCallee CodeGen::getMallocPrototype() {
  return module->getOrInsertFunction(
      "malloc",
      llvm::FunctionType::get(llvm::PointerType::get(*context, 0),
                              {llvm::Type::getInt64Ty(*context)}, false));
}

llvm::FunctionCallee CodeGen::getFreePrototype() {
  return module->getOrInsertFunction(
      "free",
      llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
                              {llvm::PointerType::get(*context, 0)}, false));
}

llvm::FunctionCallee CodeGen::getPrintfPrototype() {
  return module->getOrInsertFunction(
      "printf",
      llvm::FunctionType::get(llvm::Type::getInt32Ty(*context),
                              {llvm::PointerType::get(*context, 0)}, true));
}

TypeInfo CodeGen::parseTypeString(const std::string &typeName) const {
  TypeInfo t;
  std::string temp = typeName;
  if (!temp.empty() && temp.back() == '?') {
    t.isNullable = true;
    temp.pop_back();
  }
  if (temp.length() >= 2 && temp.substr(temp.length() - 2) == "&&") {
    t.isRValueRef = true;
    temp.erase(temp.length() - 2);
  } else if (!temp.empty() && temp.back() == '&') {
    t.isReference = true;
    temp.pop_back();
  }
  while (!temp.empty() && temp.back() == '*') {
    t.ptrDepth++;
    temp.pop_back();
  }
  t.base = temp;
  return t;
}

llvm::Type *CodeGen::getLLVMType(const TypeInfo &type) {
  if (type.ptrDepth > 0 || type.arrayDimensions > 0 || type.isReference ||
      type.isRValueRef)
    return llvm::PointerType::get(*context, 0);
  if (structTypes.count(type.base))
    return structTypes[type.base];
  if (type.base == "void")
    return llvm::Type::getVoidTy(*context);

  if (type.base == "char" || type.base == "uchar" || type.base == "int8_t" ||
      type.base == "uint8_t")
    return llvm::Type::getInt8Ty(*context);
  if (type.base == "short" || type.base == "ushort" || type.base == "int16_t" ||
      type.base == "uint16_t")
    return llvm::Type::getInt16Ty(*context);
  if (type.base == "int" || type.base == "uint" || type.base == "int32_t" ||
      type.base == "uint32_t")
    return llvm::Type::getInt32Ty(*context);
  if (type.base == "int64_t" || type.base == "uint64_t")
    return llvm::Type::getInt64Ty(*context);

  if (type.base == "long" || type.base == "ulong" || type.base == "size_t" ||
      type.base == "intptr_t" || type.base == "uintptr_t") {
    /* dynamic bit width resolution. black magic to avoid stack corruption on
     * 32-bit toasters */
    unsigned ptrSize = module->getDataLayout().getPointerSizeInBits();
    return llvm::Type::getIntNTy(*context, ptrSize);
  }

  if (type.base == "float")
    return llvm::Type::getFloatTy(*context);
  if (type.base == "double")
    return llvm::Type::getDoubleTy(*context);
  if (type.base == "bool")
    return llvm::Type::getInt1Ty(*context);

  return llvm::PointerType::get(*context, 0);
}

llvm::Type *CodeGen::getLLVMType(const std::string &typeName) {
  return getLLVMType(parseTypeString(typeName));
}

llvm::Value *CodeGen::getOrCreateString(const std::string &str) {
  if (!stringPool.count(str))
    stringPool[str] = builder->CreateGlobalString(str);
  return stringPool[str];
}

llvm::Value *CodeGen::castValue(llvm::Value *value, const TypeInfo &from,
                                const TypeInfo &to) {
  if (from.base == to.base && from.ptrDepth == to.ptrDepth)
    return value;

  if (from.isPointer() && to.isPointer()) {
    /* blind pointer casting. pray for alignment. */
    return builder->CreateBitCast(value, getLLVMType(to));
  }

  if (!from.isPointer() && !to.isPointer() && structTypes.count(to.base)) {
    return value;
  }

  if (!from.isPointer() && !to.isPointer()) {
    unsigned ptrSize = module->getDataLayout().getPointerSizeInBits();

    if (from.isFloat() && to.isFloat()) {
      if (from.base == "double" && to.base == "float")
        return builder->CreateFPTrunc(value, getLLVMType(to));
      if (from.base == "float" && to.base == "double")
        return builder->CreateFPExt(value, getLLVMType(to));
    }

    if (from.getIntegerBitWidth(ptrSize) > 0 && to.isFloat()) {
      return from.isUnsigned() ? builder->CreateUIToFP(value, getLLVMType(to))
                               : builder->CreateSIToFP(value, getLLVMType(to));
    }

    if (from.isFloat() && to.getIntegerBitWidth(ptrSize) > 0) {
      return to.isUnsigned() ? builder->CreateFPToUI(value, getLLVMType(to))
                             : builder->CreateFPToSI(value, getLLVMType(to));
    }

    if (from.getIntegerBitWidth(ptrSize) > 0 &&
        to.getIntegerBitWidth(ptrSize) > 0) {
      unsigned fromWidth = from.getIntegerBitWidth(ptrSize);
      unsigned toWidth = to.getIntegerBitWidth(ptrSize);

      if (toWidth > fromWidth) {
        return from.isUnsigned() ? builder->CreateZExt(value, getLLVMType(to))
                                 : builder->CreateSExt(value, getLLVMType(to));
      } else if (toWidth < fromWidth) {
        return builder->CreateTrunc(value, getLLVMType(to));
      } else {
        // Same width, diff sign (e.g., int to uint)
        return builder->CreateBitCast(value, getLLVMType(to));
      }
    }

    if (from.base == "bool" && to.getIntegerBitWidth(ptrSize) > 0)
      return builder->CreateZExt(value, getLLVMType(to));
  }

  if (from.isPointer() && !to.isPointer())
    return builder->CreatePtrToInt(value, getLLVMType(to));
  if (!from.isPointer() && to.isPointer())
    return builder->CreateIntToPtr(value, getLLVMType(to));

  return value;
}

void CodeGen::emitCopyOrStore(llvm::Value *destAddr, llvm::Value *srcVal,
                              const TypeInfo &targetType,
                              const TypeInfo &srcType) {
  if (targetType.ptrDepth == 0 && !targetType.isReference &&
      structTypes.count(targetType.base)) {

    std::string mangledCtor = "";

    // --- MOVE SEMANTICS HIJACK ---
    // The source memory is marked for death (r-value). We bypass the deep copy
    // and call the move constructor to steal its pointers.
    // "What is yours is now mine."
    if (srcType.isRValueRef) {
      mangledCtor = targetType.base + "_" + targetType.base + "_" +
                    targetType.base + "rrf";
    }

    llvm::Function *ctor =
        mangledCtor.empty() ? nullptr : module->getFunction(mangledCtor);

    // Fallback a Copy-Ctor si no existe Move-Ctor o no es un r-value
    if (!ctor) {
      mangledCtor = targetType.base + "_" + targetType.base + "_" +
                    targetType.base + "ref";
      ctor = module->getFunction(mangledCtor);
    }

    if (ctor) {
      builder->CreateCall(ctor, {destAddr, srcVal});
    } else {
      llvm::Value *loadedStruct =
          builder->CreateLoad(getLLVMType(targetType), srcVal, "shallow.load");
      builder->CreateStore(loadedStruct, destAddr);
    }
  } else {
    builder->CreateStore(srcVal, destAddr);
  }
}

void CodeGen::emitLifecycleLoop(llvm::Value *basePtr, llvm::Value *size,
                                const std::string &typeName,
                                bool isDestructor) {
  if (!structTypes.count(typeName))
    return;

  std::string funcName =
      isDestructor ? typeName + "_~" + typeName : typeName + "_" + typeName;
  llvm::Function *lifecycleFunc = module->getFunction(funcName);
  if (!lifecycleFunc)
    return;

  llvm::Function *currentFunc = builder->GetInsertBlock()->getParent();

  if (size->getType()->isIntegerTy(32)) {
    size = builder->CreateZExt(size, llvm::Type::getInt64Ty(*context));
  }

  llvm::BasicBlock *condBB =
      llvm::BasicBlock::Create(*context, "loop.cond", currentFunc);
  llvm::BasicBlock *bodyBB =
      llvm::BasicBlock::Create(*context, "loop.body", currentFunc);
  llvm::BasicBlock *endBB =
      llvm::BasicBlock::Create(*context, "loop.end", currentFunc);

  llvm::AllocaInst *indexPtr = builder->CreateAlloca(
      llvm::Type::getInt64Ty(*context), nullptr, "loop.idx");
  if (isDestructor) {
    builder->CreateStore(size, indexPtr);
  } else {
    builder->CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0), indexPtr);
  }
  builder->CreateBr(condBB);

  builder->SetInsertPoint(condBB);
  llvm::Value *currentIndex =
      builder->CreateLoad(llvm::Type::getInt64Ty(*context), indexPtr);

  llvm::Value *cond;
  if (isDestructor) {
    cond = builder->CreateICmpUGT(
        currentIndex,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0), "cmp.gt");
  } else {
    cond = builder->CreateICmpULT(currentIndex, size, "cmp.lt");
  }

  builder->CreateCondBr(cond, bodyBB, endBB);

  builder->SetInsertPoint(bodyBB);

  llvm::Value *actualIndex;
  if (isDestructor) {
    actualIndex = builder->CreateSub(
        currentIndex,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 1), "idx.sub");
  } else {
    actualIndex = currentIndex;
  }

  llvm::Value *elementAddr = builder->CreateInBoundsGEP(
      structTypes[typeName], basePtr, actualIndex, "elem.addr");

  builder->CreateCall(lifecycleFunc, {elementAddr});

  llvm::Value *nextIndex;
  if (isDestructor) {
    nextIndex = builder->CreateSub(
        currentIndex,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 1));
  } else {
    nextIndex = builder->CreateAdd(
        currentIndex,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 1));
  }
  builder->CreateStore(nextIndex, indexPtr);
  builder->CreateBr(condBB);

  builder->SetInsertPoint(endBB);
}

void CodeGen::emitScopeCleanup(size_t targetDepth) {
  if (builder->GetInsertBlock()->getTerminator()) {
    return;
  }

  for (size_t i = valueScopes.size(); i > targetDepth; --i) {
    auto &currentScopeValues = valueScopes[i - 1];
    auto &currentScopeTypes = typeScopes[i - 1];

    // We iterate in reverse (Reverse Declaration Order) to comply with RAII.
    for (auto rit = currentScopeValues.rbegin();
         rit != currentScopeValues.rend(); ++rit) {
      const std::string &name = rit->first;
      llvm::Value *valInst = rit->second;
      TypeInfo type = currentScopeTypes[name];

      if (llvm::isa<llvm::GlobalVariable>(valInst)) {
        continue;
      }

      // We explicitly exclude isRValueRef. We do not own that memory
      if (type.ptrDepth == 0 && !type.isReference && !type.isRValueRef &&
          structTypes.count(type.base)) {

        std::string dtorName = type.base + "_~" + type.base;
        llvm::Function *dtorFunc = module->getFunction(dtorName);

        if (dtorFunc) {
          builder->CreateCall(dtorFunc, {valInst});
        }
      }
    }
  }
}

// --------------------------------------------------------------------------
// Visitor Implementations (Double Dispatch Payload)
// --------------------------------------------------------------------------

void CodeGen::visit(ProgramNode *node) {
  enterScope();

  for (auto &st : node->structs) {
    structASTs[st->name] = st.get();
    structTypes[st->name] = llvm::StructType::create(*context, st->name);
  }

  for (auto &st : node->structs) {
    std::vector<llvm::Type *> body;
    int idx = 0;

    auto buildMemoryLayout = [&](std::string className, auto &self) -> void {
      StructDeclNode *currentAST = structASTs[className];
      if (!currentAST->baseClass.empty() &&
          structASTs.count(currentAST->baseClass)) {
        self(currentAST->baseClass, self);
      }
      for (auto &f : currentAST->fields) {
        TypeInfo t = parseTypeString(f.typeName);
        body.push_back(getLLVMType(t));
        structMemberIndices[st->name][f.name] = idx++;
        structMemberTypes[st->name][f.name] = t;
      }
    };

    buildMemoryLayout(st->name, buildMemoryLayout);
    structTypes[st->name]->setBody(body);

    for (auto &f : st->fields) {
      if (f.isStatic) {
        std::string globalName = st->name + "_" + f.name;
        llvm::Type *llvmType = getLLVMType(structMemberTypes[st->name][f.name]);
        module->getOrInsertGlobal(globalName, llvmType);
        llvm::GlobalVariable *gVar = module->getNamedGlobal(globalName);
        gVar->setLinkage(llvm::GlobalValue::InternalLinkage);
        gVar->setInitializer(llvm::Constant::getNullValue(llvmType));
      }
    }
  }

  currentClass = "";

  for (auto &st : node->structs) {
    for (auto &method : st->methods) {
      std::string originalName = method->name;
      method->name = st->name + "_" + method->name;

      if (!method->isStatic && !method->isConstructor) {
        TypeInfo thisType = {st->name, 1, false};
        method->name += "_" + getMangledType(thisType);
      }
      for (auto &arg : method->args) {
        TypeInfo t = arg.isThisAssign ? structMemberTypes[st->name][arg.name]
                                      : parseTypeString(arg.type);
        method->name += "_" + getMangledType(t);
      }

      method->accept(this);
      method->name = originalName;
    }
  }

  /*
   * Extension methods.
   * We mangle the target type manually because these bastards live outside the
   * struct scope. Forcing the target into the signature so the linker doesn't
   * shit itself.
   */
  for (auto &ext : node->extensions) {
    for (auto &method : ext->methods) {
      std::string originalName = method->name;
      method->name = "ext_" + ext->targetTypedef + "_" + method->name;

      TypeInfo targetType = parseTypeString(ext->targetTypedef);
      method->name += "_" + getMangledType(targetType);

      for (auto &arg : method->args) {
        TypeInfo t = arg.isThisAssign
                         ? structMemberTypes[ext->targetTypedef][arg.name]
                         : parseTypeString(arg.type);
        method->name += "_" + getMangledType(t);
      }

      method->accept(this);
      method->name = originalName;
    }
  }

  for (auto &func : node->functions) {
    std::string mangledName = func->name;
    std::vector<TypeInfo> paramTypes;
    for (auto &arg : func->args) {
      paramTypes.push_back(parseTypeString(arg.type));
      mangledName += "_" + getMangledType(paramTypes.back());
    }
    functionParamTypes[mangledName] = paramTypes;
  }

  for (auto &st : node->structs) {
    for (auto &method : st->methods) {
      std::string mangledName = st->name + "_" + method->name;
      std::vector<TypeInfo> paramTypes;

      // Inject 'this' pointer to match Sema's mangled signature
      if (!method->isStatic && !method->isConstructor) {
        TypeInfo thisType = {st->name, 1, false}; // pointer
        paramTypes.push_back(thisType);
        mangledName += "_" + getMangledType(thisType);
      }

      if (method->isConstructor) {
        TypeInfo thisType = {st->name, 1, false}; // pointer
        paramTypes.push_back(thisType);
        mangledName += "_" + getMangledType(thisType);
      }

      for (auto &arg : method->args) {
        TypeInfo t = arg.isThisAssign ? structMemberTypes[st->name][arg.name]
                                      : parseTypeString(arg.type);
        paramTypes.push_back(t);
        mangledName += "_" + getMangledType(t);
      }
      functionParamTypes[mangledName] = paramTypes;
    }
  }

  /*
   * Extension parameter types registration.
   * If we don't do this, CallNode resolves the arguments as raw ints and burns
   * the stack. WTF.
   */
  for (auto &ext : node->extensions) {
    for (auto &method : ext->methods) {
      std::string mangledName =
          "ext_" + ext->targetTypedef + "_" + method->name;
      std::vector<TypeInfo> paramTypes;

      TypeInfo targetType = parseTypeString(ext->targetTypedef);
      paramTypes.push_back(targetType);
      mangledName += "_" + getMangledType(targetType);

      for (auto &arg : method->args) {
        TypeInfo t = arg.isThisAssign
                         ? structMemberTypes[ext->targetTypedef][arg.name]
                         : parseTypeString(arg.type);
        paramTypes.push_back(t);
        mangledName += "_" + getMangledType(t);
      }
      functionParamTypes[mangledName] = paramTypes;
    }
  }

  // If 'main' is visited first, getOrInsertFunction injects an external
  // declaration. When we try to define it later, LLVM avoids collision by
  // silently renaming ours to '__utopia_global_init.1', breaking the linker.
  // Claim the namespace early.
  llvm::FunctionType *initTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(*context), false);
  llvm::Function *staticInitF =
      llvm::Function::Create(initTy, llvm::Function::InternalLinkage,
                             "__utopia_global_init", module.get());

  for (auto &func : node->functions) {
    std::string originalName = func->name;
    for (auto &arg : func->args)
      func->name += "_" + getMangledType(parseTypeString(arg.type));
    func->accept(this);
    func->name = originalName;
  }

  // Evaluate static initializing expressions
  llvm::BasicBlock *initBB =
      llvm::BasicBlock::Create(*context, "entry", staticInitF);
  builder->SetInsertPoint(initBB);
  builder->CreateRetVoid();

  exitScope();
}

void CodeGen::visit(StructDeclNode *node) {}

void CodeGen::visit(ExtensionNode *node) {}

void CodeGen::visit(MemberAccessNode *node) {
  // Access via the Class Symbol (Counter.globalCount)
  if (auto varNode = dynamic_cast<VariableNode *>(node->object.get())) {
    if (structTypes.count(varNode->name) && !lookupValue(varNode->name)) {
      std::string globalName = varNode->name + "_" + node->field;
      TypeInfo fType = structMemberTypes[varNode->name][node->field];
      llvm::Value *globalVar = module->getNamedGlobal(globalName);

      if (!globalVar) {
        globalVar = module->getOrInsertGlobal(globalName, getLLVMType(fType));
      }
      if (isLValueContext) {
        currentLValue = globalVar;
      } else {
        // Evil bit-level trick. If it's a struct by value, we intercept the
        // load and just pass the raw pointer. Saves register space and prevents
        // LLVM from exploding.
        if (structTypes.count(fType.base) && fType.ptrDepth == 0) {
          currentVal = globalVar;
        } else {
          currentVal =
              builder->CreateLoad(getLLVMType(fType), globalVar, "load_static");
        }
      }
      currentType = fType;
      return;
    }
  }

  bool oldContext = isLValueContext;
  isLValueContext = false;
  node->object->accept(this);
  isLValueContext = oldContext;

  llvm::Value *objPtr = currentVal;
  std::string objTypeName = currentType.base;
  TypeInfo fType = structMemberTypes[objTypeName][node->field];

  // Access to static but through an Instance (c1.globalCount)
  if (structMemberIndices[objTypeName].count(node->field) == 0) {
    std::string globalName = objTypeName + "_" + node->field;
    llvm::Value *globalVar = module->getNamedGlobal(globalName);
    if (isLValueContext) {
      currentLValue = globalVar;
    } else {
      if (structTypes.count(fType.base) && fType.ptrDepth == 0) {
        currentVal = globalVar;
      } else {
        currentVal =
            builder->CreateLoad(getLLVMType(fType), globalVar, "load_static");
      }
    }
    currentType = fType;
    return;
  }

  // Access to normal instance variable (c1.instanceCount)
  int idx = structMemberIndices[objTypeName][node->field];
  llvm::Value *fieldAddr = builder->CreateStructGEP(
      structTypes[objTypeName], objPtr, idx, "member_access");

  if (isLValueContext) {
    currentLValue = fieldAddr;
  } else {
    if (structTypes.count(fType.base) && fType.ptrDepth == 0) {
      currentVal = fieldAddr;
    } else {
      currentVal =
          builder->CreateLoad(getLLVMType(fType), fieldAddr, "load_member");
    }
  }
  currentType = fType;
}

void CodeGen::visit(ThisNode *node) {
  auto alloca = lookupValue("this");
  auto type = lookupType("this");

  if (!alloca) {
    std::cerr << "CodeGen Error: 'this' pointer not found in current scope.\n";
    exit(1);
  }

  if (type.ptrDepth == 0) {
    currentVal =
        builder->CreateLoad(getLLVMType(type.base), alloca, "this.val");
    currentType = type;
  } else {
    llvm::Value *ptr = builder->CreateLoad(llvm::PointerType::get(*context, 0),
                                           alloca, "this.ptr");

    if (isLValueContext) {
      currentLValue = ptr;
    } else {
      // If the object is an int/float, load it. We cannot do math with
      // addresses
      if (currentType.isPrimitive() && type.ptrDepth == 1) {
        currentVal =
            builder->CreateLoad(getLLVMType(type.base), ptr, "this.val");
        currentType = type;
        currentType.ptrDepth = 0;
      } else {
        currentVal = ptr;
        currentType = type;
      }
    }
  }
}

void CodeGen::visit(FunctionNode *node) {
  enterScope();

  TypeInfo retType = parseTypeString(node->returnType);
  currentReturnType = retType;

  std::vector<llvm::Type *> argTypes;
  bool isPrimitiveExtension = false;

  if ((node->isMethod && !node->isStatic) || node->isConstructor ||
      node->isDestructor) {
    if (currentType.isPrimitive()) {
      // If it's an extension of int/float, 'this' is the direct value, not a
      // pointer.
      argTypes.push_back(getLLVMType(node->className));
      isPrimitiveExtension = true;
    } else {
      argTypes.push_back(llvm::PointerType::get(*context, 0));
    }
  }

  for (auto &arg : node->args) {
    TypeInfo t = arg.isThisAssign ? structMemberTypes[node->className][arg.name]
                                  : parseTypeString(arg.type);
    argTypes.push_back(getLLVMType(t));
  }

  llvm::FunctionType *funcType =
      llvm::FunctionType::get(getLLVMType(retType), argTypes, false);

  llvm::Function *func = llvm::Function::Create(
      funcType, llvm::Function::ExternalLinkage, node->name, module.get());

  if (isDebug) {
    func->addFnAttr(llvm::Attribute::NoInline);
    func->addFnAttr(llvm::Attribute::OptimizeNone);
  } else {
    if (node->inlineState == InlineState::Inline) {
      func->addFnAttr(llvm::Attribute::InlineHint);
    } else if (node->inlineState == InlineState::ForceInline) {
      func->addFnAttr(llvm::Attribute::AlwaysInline);
    }
  }

  llvm::BasicBlock *block = llvm::BasicBlock::Create(*context, "entry", func);
  builder->SetInsertPoint(block);

  if (isDebug) {
    llvm::DISubroutineType *dbgFuncType =
        dbgBuilder->createSubroutineType(dbgBuilder->getOrCreateTypeArray({}));

    llvm::DISubprogram *dbgFunc = dbgBuilder->createFunction(
        dbgFile, node->name, func->getName(), dbgFile, node->line, dbgFuncType,
        node->line, llvm::DINode::FlagPrototyped,
        llvm::DISubprogram::SPFlagDefinition);

    func->setSubprogram(dbgFunc);
    dbgScopes.push_back(dbgFunc);
  }

  emitLocation(node);

  if (node->name == "main") {
    llvm::FunctionCallee initF = module->getOrInsertFunction(
        "__utopia_global_init",
        llvm::FunctionType::get(llvm::Type::getVoidTy(*context), false));
    builder->CreateCall(initF);
  }

  int paramOffset = 1;
  auto argIt = func->arg_begin();
  if ((node->isMethod && !node->isStatic) || node->isConstructor ||
      node->isDestructor) {
    llvm::Argument *thisArg = &(*argIt);
    thisArg->setName("this");

    llvm::Type *allocType = isPrimitiveExtension
                                ? getLLVMType(node->className)
                                : llvm::PointerType::get(*context, 0);
    llvm::AllocaInst *thisAlloca = builder->CreateAlloca(
        llvm::PointerType::get(*context, 0), nullptr, "this.addr");
    builder->CreateStore(thisArg, thisAlloca);

    /* Order matters for RAII: push 'this' into the scope stack. */
    valueScopes.back().push_back({"this", thisAlloca});
    typeScopes.back()["this"] = {node->className,
                                 isPrimitiveExtension ? 0u : 1u, false};

    if (isDebug && !dbgScopes.empty()) {
      llvm::DILocalVariable *dbgThis = dbgBuilder->createParameterVariable(
          dbgScopes.back(), "this", paramOffset, dbgFile, node->line,
          getDebugType({node->className, 1, false}), true);

      dbgBuilder->insertDeclare(
          thisAlloca, dbgThis, dbgBuilder->createExpression(),
          llvm::DILocation::get(*context, node->line, node->column,
                                dbgScopes.back()),
          builder->GetInsertBlock());
    }

    paramOffset++;
    argIt++;
  }

  unsigned idx = 0;
  for (; argIt != func->arg_end(); ++argIt, ++idx) {
    std::string argName = node->args[idx].name;
    TypeInfo argType = node->args[idx].isThisAssign
                           ? structMemberTypes[node->className][argName]
                           : parseTypeString(node->args[idx].type);

    llvm::Argument *arg = &(*argIt);
    arg->setName(argName);

    llvm::AllocaInst *alloca =
        builder->CreateAlloca(getLLVMType(argType), nullptr, argName);
    builder->CreateStore(arg, alloca);

    valueScopes.back().push_back({argName, alloca});
    typeScopes.back()[argName] = argType;

    if (isDebug && !dbgScopes.empty()) {
      llvm::DILocalVariable *dbgParam = dbgBuilder->createParameterVariable(
          dbgScopes.back(), argName, paramOffset + idx, dbgFile, node->line,
          getDebugType(argType), true);

      dbgBuilder->insertDeclare(
          alloca, dbgParam, dbgBuilder->createExpression(),
          llvm::DILocation::get(*context, node->line, node->column,
                                dbgScopes.back()),
          builder->GetInsertBlock());
    }

    if (node->args[idx].isThisAssign) {
      /* Extract 'this' from the stack to bind field-assign arguments. */
      llvm::Value *thisPtr = builder->CreateLoad(
          llvm::PointerType::get(*context, 0), lookupValue("this"));
      int fieldIdx = structMemberIndices[node->className][argName];
      llvm::Value *fieldAddr = builder->CreateStructGEP(
          structTypes[node->className], thisPtr, fieldIdx, "this." + argName);
      builder->CreateStore(arg, fieldAddr);
      std::cerr << "[CodeGen] Field " << argName << " index = " << fieldIdx
                << " in class " << node->className << "\n";
    }
  }

  if (node->isConstructor) {
    StructDeclNode *classAST = structASTs[node->className];
    if (classAST) {
      llvm::Value *thisPtr = builder->CreateLoad(
          llvm::PointerType::get(*context, 0), lookupValue("this"));

      for (auto &field : classAST->fields) {
        if (field.isStatic)
          continue;

        if (field.initializer) {
          field.initializer->accept(this);
          llvm::Value *initVal = currentVal;
          TypeInfo initType = currentType;

          int fieldIdx = structMemberIndices[node->className][field.name];
          TypeInfo targetType = structMemberTypes[node->className][field.name];

          llvm::Value *fieldAddr =
              builder->CreateStructGEP(structTypes[node->className], thisPtr,
                                       fieldIdx, "init." + field.name);

          builder->CreateStore(castValue(initVal, initType, targetType),
                               fieldAddr);
        }
      }
    }
  }

  for (const auto &stmt : node->body) {
    stmt->accept(this);
    if (builder->GetInsertBlock()->getTerminator())
      break;
  }

  /* Function epilogue: scope cleanup and default return handling. */
  if (!builder->GetInsertBlock()->getTerminator()) {
    emitScopeCleanup(valueScopes.size() - 1);

    if (retType.base == "void" && retType.ptrDepth == 0) {
      builder->CreateRetVoid();
    } else {
      llvm::Type *t = getLLVMType(retType);
      if (t->isPointerTy()) {
        builder->CreateRet(
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(t)));
      } else if (t->isFloatingPointTy()) {
        builder->CreateRet(llvm::ConstantFP::get(t, 0.0));
      } else {
        builder->CreateRet(llvm::ConstantInt::get(t, 0));
      }
    }
  }

  if (isDebug && !dbgScopes.empty()) {
    dbgScopes.pop_back();
  }

  exitScope();
}

void CodeGen::visit(ReturnNode *node) {
  emitLocation(node);
  llvm::Value *retVal = nullptr;
  if (node->returnValue) {
    node->returnValue->accept(this);
    retVal = castValue(currentVal, currentType, currentReturnType);
  }

  /*
   * Critical path: Burn all local scopes down to the function root
   * (depth 1) before firing the return instruction.
   */
  emitScopeCleanup(1);

  if (retVal) {
    builder->CreateRet(retVal);
  } else {
    builder->CreateRetVoid();
  }
}

void CodeGen::visit(AssignNode *node) {
  emitLocation(node);
  isLValueContext = true;
  node->target->accept(this);
  isLValueContext = false;
  llvm::Value *destAddr = currentLValue;
  TypeInfo targetType = currentType;

  if (!destAddr) {
    std::cerr << "Fatal CodeGen Error: Assign target is not an LValue.\n";
    exit(1);
  }

  node->value->accept(this);
  llvm::Value *val = currentVal;
  TypeInfo valType = currentType;

  val = castValue(val, valType, targetType);

  if (node->op != "=") {
    llvm::Value *curVal =
        builder->CreateLoad(getLLVMType(targetType), destAddr, "cur_val");
    bool isF = (targetType.base == "float");
    if (node->op == "+=")
      val = isF ? builder->CreateFAdd(curVal, val)
                : builder->CreateAdd(curVal, val);
    else if (node->op == "-=")
      val = isF ? builder->CreateFSub(curVal, val)
                : builder->CreateSub(curVal, val);
    else if (node->op == "*=")
      val = isF ? builder->CreateFMul(curVal, val)
                : builder->CreateMul(curVal, val);
    else if (node->op == "/=")
      val = isF ? builder->CreateFDiv(curVal, val)
                : builder->CreateSDiv(curVal, val);
  }

  emitCopyOrStore(destAddr, val, targetType, valType);
}

void CodeGen::visit(SubscriptNode *node) {
  bool oldCtx = isLValueContext;
  isLValueContext = false;
  node->object->accept(this);
  llvm::Value *objPtr = currentVal;
  TypeInfo objType = currentType;

  node->index->accept(this);
  llvm::Value *idx = currentVal;
  isLValueContext = oldCtx;

  TypeInfo elemType = objType;
  elemType.ptrDepth--;

  llvm::Value *elemAddr = builder->CreateInBoundsGEP(getLLVMType(elemType),
                                                     objPtr, idx, "array_idx");

  if (isLValueContext) {
    currentLValue = elemAddr;
  } else {
    if (structTypes.count(elemType.base) && elemType.ptrDepth == 0) {
      currentVal = elemAddr;
    } else {
      currentVal =
          builder->CreateLoad(getLLVMType(elemType), elemAddr, "load_idx");
    }
  }
  currentType = elemType;
}

void CodeGen::visit(VarDeclNode *node) {
  emitLocation(node);
  TypeInfo declType = parseTypeString(node->typeName);
  llvm::Type *llvmType = getLLVMType(declType);

  if (node->isStatic) {
    std::string globalName =
        currentClass.empty()
            ? builder->GetInsertBlock()->getParent()->getName().str() + "." +
                  node->name
            : currentClass + "_" + node->name;

    module->getOrInsertGlobal(globalName, llvmType);
    llvm::GlobalVariable *gVar = module->getNamedGlobal(globalName);
    gVar->setLinkage(llvm::GlobalValue::InternalLinkage);
    gVar->setInitializer(llvm::Constant::getNullValue(llvmType));

    if (node->initializer) {
      llvm::Function *currentFunc = builder->GetInsertBlock()->getParent();

      std::string guardName = globalName + ".guard";
      module->getOrInsertGlobal(guardName, builder->getInt1Ty());
      llvm::GlobalVariable *guard = module->getNamedGlobal(guardName);
      guard->setLinkage(llvm::GlobalValue::InternalLinkage);
      guard->setInitializer(llvm::ConstantInt::getFalse(*context));

      llvm::BasicBlock *initBB =
          llvm::BasicBlock::Create(*context, "static.init", currentFunc);
      llvm::BasicBlock *contBB =
          llvm::BasicBlock::Create(*context, "static.cont", currentFunc);

      llvm::Value *isInit =
          builder->CreateLoad(builder->getInt1Ty(), guard, "guard_load");
      builder->CreateCondBr(isInit, contBB, initBB);

      builder->SetInsertPoint(initBB);

      node->initializer->accept(this);
      llvm::Value *initVal = currentVal;
      emitCopyOrStore(gVar, castValue(initVal, currentType, declType), declType,
                      currentType);

      builder->CreateStore(builder->getTrue(), guard);
      builder->CreateBr(contBB);

      builder->SetInsertPoint(contBB);
    }

    valueScopes.back().push_back({node->name, gVar});
    typeScopes.back()[node->name] = declType;
    return;
  }

  if (!node->arraySizes.empty()) {
    std::vector<llvm::Value *> sizeVals;
    for (auto &szNode : node->arraySizes) {
      szNode->accept(this);
      llvm::Value *sz = currentVal;
      if (sz->getType()->isIntegerTy(32))
        sz = builder->CreateZExt(sz, llvm::Type::getInt64Ty(*context));
      sizeVals.push_back(sz);
    }

    // WTF: Single-Allocation Iliffe Vector Stack Allocation.
    // Adiós a los alloca recursivos. Destrozamos la memoria con un bloque
    // gigante y contiguo.
    llvm::Type *i64 = builder->getInt64Ty();
    llvm::Type *i8 = builder->getInt8Ty();
    llvm::Type *ptrTy = llvm::PointerType::get(*context, 0);

    std::vector<llvm::Value *> levelCounts;
    llvm::Value *curCount = builder->getInt64(1);
    for (auto sz : sizeVals) {
      curCount = builder->CreateMul(curCount, sz);
      levelCounts.push_back(curCount);
    }

    llvm::Value *totalData = levelCounts.back();
    llvm::Value *totalPointers = builder->getInt64(0);

    for (size_t i = 0; i < sizeVals.size() - 1; ++i) {
      totalPointers = builder->CreateAdd(totalPointers, levelCounts[i]);
    }

    uint64_t elemSize =
        module->getDataLayout().getTypeAllocSize(getLLVMType(declType));
    llvm::Value *ptrBytes =
        builder->CreateMul(totalPointers, builder->getInt64(8));
    llvm::Value *dataBytes =
        builder->CreateMul(totalData, builder->getInt64(elemSize));
    llvm::Value *totalBytes = builder->CreateAdd(ptrBytes, dataBytes);

    // Un solo Alloca masivo.
    llvm::Value *blockMem =
        builder->CreateAlloca(i8, totalBytes, node->name + "_flat_vla");

    std::vector<llvm::Value *> levelOffsets;
    llvm::Value *currentOffset = builder->getInt64(0);
    for (size_t i = 0; i < sizeVals.size() - 1; ++i) {
      levelOffsets.push_back(currentOffset);
      llvm::Value *levelBytes =
          builder->CreateMul(levelCounts[i], builder->getInt64(8));
      currentOffset = builder->CreateAdd(currentOffset, levelBytes);
    }
    llvm::Value *dataOffset = currentOffset;
    levelOffsets.push_back(dataOffset);

    // Runtime pointer threading magic.
    llvm::Function *func = builder->GetInsertBlock()->getParent();
    for (size_t L = 0; L < sizeVals.size() - 1; ++L) {
      llvm::Value *numPtrs = levelCounts[L];
      llvm::Value *stride = sizeVals[L + 1];
      llvm::Value *curLvlOffset = levelOffsets[L];
      llvm::Value *nextLvlOffset = levelOffsets[L + 1];
      llvm::Value *nextElemSz =
          builder->getInt64((L == sizeVals.size() - 2) ? elemSize : 8);

      llvm::BasicBlock *condBB =
          llvm::BasicBlock::Create(*context, "vla.cond", func);
      llvm::BasicBlock *bodyBB =
          llvm::BasicBlock::Create(*context, "vla.body", func);
      llvm::BasicBlock *endBB =
          llvm::BasicBlock::Create(*context, "vla.end", func);

      llvm::AllocaInst *idxPtr = builder->CreateAlloca(i64, nullptr, "idx");
      builder->CreateStore(builder->getInt64(0), idxPtr);
      builder->CreateBr(condBB);

      builder->SetInsertPoint(condBB);
      llvm::Value *idx = builder->CreateLoad(i64, idxPtr);
      llvm::Value *cmp = builder->CreateICmpULT(idx, numPtrs);
      builder->CreateCondBr(cmp, bodyBB, endBB);

      builder->SetInsertPoint(bodyBB);
      llvm::Value *ptrOffset = builder->CreateAdd(
          curLvlOffset, builder->CreateMul(idx, builder->getInt64(8)));
      llvm::Value *ptrAddr =
          builder->CreateInBoundsGEP(i8, blockMem, ptrOffset);
      ptrAddr = builder->CreateBitCast(ptrAddr, ptrTy);

      llvm::Value *targetOffset = builder->CreateAdd(
          nextLvlOffset,
          builder->CreateMul(builder->CreateMul(idx, stride), nextElemSz));
      llvm::Value *targetAddr =
          builder->CreateInBoundsGEP(i8, blockMem, targetOffset);

      builder->CreateStore(targetAddr, ptrAddr);

      builder->CreateStore(builder->CreateAdd(idx, builder->getInt64(1)),
                           idxPtr);
      builder->CreateBr(condBB);
      builder->SetInsertPoint(endBB);
    }

    llvm::Value *dataStart =
        builder->CreateInBoundsGEP(i8, blockMem, dataOffset);
    emitLifecycleLoop(builder->CreateBitCast(dataStart, ptrTy), totalData,
                      node->typeName, false);

    declType.arrayDimensions = node->arraySizes.size();
    llvm::Value *finalAlloca =
        builder->CreateBitCast(blockMem, getLLVMType(declType));
    valueScopes.back().push_back({node->name, finalAlloca});
    typeScopes.back()[node->name] = declType;

  } else {
    llvm::Type *llvmType = getLLVMType(declType);
    llvm::AllocaInst *alloca =
        builder->CreateAlloca(llvmType, nullptr, node->name);

    if (isDebug && !dbgScopes.empty()) {
      llvm::DILocalVariable *dbgVar =
          dbgBuilder->createAutoVariable(dbgScopes.back(), node->name, dbgFile,
                                         node->line, getDebugType(declType));

      dbgBuilder->insertDeclare(alloca, dbgVar, dbgBuilder->createExpression(),
                                llvm::DILocation::get(*context, node->line,
                                                      node->column,
                                                      dbgScopes.back()),
                                builder->GetInsertBlock());
    }

    if (node->initializer) {
      bool isRef = declType.isReference;
      bool canRVO =
          !isRef && declType.ptrDepth == 0 && structTypes.count(declType.base);

      if (isRef)
        isLValueContext = true;

      // Set the RVO trap.
      if (canRVO) {
        rvoTarget = alloca;
      }

      node->initializer->accept(this);

      // Disarm the trap in case the child node didn't consume it.
      rvoTarget = nullptr;

      if (isRef)
        isLValueContext = false;

      llvm::Value *initVal = currentVal;

      if (isRef) {
        llvm::Value *refTarget = currentLValue ? currentLValue : currentVal;
        builder->CreateStore(refTarget, alloca);
      } else {
        if (initVal != alloca) {
          emitCopyOrStore(alloca, castValue(initVal, currentType, declType),
                          declType, currentType);
        }
      }
    } else {
      builder->CreateStore(llvm::Constant::getNullValue(llvmType), alloca);
    }

    valueScopes.back().push_back({node->name, alloca});
    typeScopes.back()[node->name] = declType;
  }
}

void CodeGen::visit(IfNode *node) {
  emitLocation(node);
  node->condition->accept(this);
  llvm::Value *condV = currentVal;
  if (condV->getType()->isPointerTy())
    condV = builder->CreateIsNotNull(condV, "ptr_val");

  llvm::Function *func = builder->GetInsertBlock()->getParent();
  llvm::BasicBlock *thenBB = llvm::BasicBlock::Create(*context, "then", func);
  llvm::BasicBlock *elseBB = llvm::BasicBlock::Create(*context, "else");
  llvm::BasicBlock *mergeBB = llvm::BasicBlock::Create(*context, "cont");

  bool hasElse = !node->elseBody.empty();
  builder->CreateCondBr(condV, thenBB, hasElse ? elseBB : mergeBB);

  builder->SetInsertPoint(thenBB);
  enterScope();
  for (auto &stmt : node->thenBody) {
    stmt->accept(this);
    if (builder->GetInsertBlock()->getTerminator())
      break;
  }
  exitScope();
  if (!builder->GetInsertBlock()->getTerminator())
    builder->CreateBr(mergeBB);

  if (hasElse) {
    func->insert(func->end(), elseBB);
    builder->SetInsertPoint(elseBB);
    enterScope();
    for (auto &stmt : node->elseBody) {
      stmt->accept(this);
      if (builder->GetInsertBlock()->getTerminator())
        break;
    }
    exitScope();
    if (!builder->GetInsertBlock()->getTerminator())
      builder->CreateBr(mergeBB);
  } else {
    delete elseBB;
  }

  func->insert(func->end(), mergeBB);
  builder->SetInsertPoint(mergeBB);
}

void CodeGen::visit(BlockNode *node) {
  if (isDebug && !dbgScopes.empty()) {
    llvm::DILexicalBlock *dbgBlock = dbgBuilder->createLexicalBlock(
        dbgScopes.back(), dbgFile, node->line, node->column);
    dbgScopes.push_back(dbgBlock);
  }

  enterScope();
  for (auto &stmt : node->statements) {
    stmt->accept(this);
    if (builder->GetInsertBlock()->getTerminator())
      break;
  }
  exitScope();

  if (isDebug && !dbgScopes.empty()) {
    dbgScopes.pop_back();
  }
}

void CodeGen::visit(WhileNode *node) {
  emitLocation(node);
  llvm::Function *func = builder->GetInsertBlock()->getParent();

  llvm::BasicBlock *condBB =
      llvm::BasicBlock::Create(*context, "while.cond", func);
  llvm::BasicBlock *bodyBB = llvm::BasicBlock::Create(*context, "while.body");
  llvm::BasicBlock *mergeBB = llvm::BasicBlock::Create(*context, "while.end");

  builder->CreateBr(condBB);
  builder->SetInsertPoint(condBB);

  node->condition->accept(this);
  llvm::Value *condV = currentVal;
  if (condV->getType()->isPointerTy()) {
    condV = builder->CreateIsNotNull(condV, "ptr_val");
  }

  builder->CreateCondBr(condV, bodyBB, mergeBB);

  continueTargets.push_back(condBB);
  breakTargets.push_back(mergeBB);
  loopScopeDepths.push_back(valueScopes.size());

  func->insert(func->end(), bodyBB);
  builder->SetInsertPoint(bodyBB);
  enterScope();
  for (auto &stmt : node->body) {
    stmt->accept(this);
    if (builder->GetInsertBlock()->getTerminator())
      break;
  }
  exitScope();

  continueTargets.pop_back();
  breakTargets.pop_back();
  loopScopeDepths.pop_back();

  if (!builder->GetInsertBlock()->getTerminator()) {
    builder->CreateBr(condBB);
  }

  func->insert(func->end(), mergeBB);
  builder->SetInsertPoint(mergeBB);
}

void CodeGen::visit(ForNode *node) {
  emitLocation(node);
  llvm::Function *func = builder->GetInsertBlock()->getParent();

  enterScope();

  if (node->init)
    node->init->accept(this);

  llvm::BasicBlock *condBB =
      llvm::BasicBlock::Create(*context, "for.cond", func);
  llvm::BasicBlock *bodyBB = llvm::BasicBlock::Create(*context, "for.body");
  llvm::BasicBlock *incBB = llvm::BasicBlock::Create(*context, "for.inc");
  llvm::BasicBlock *mergeBB = llvm::BasicBlock::Create(*context, "for.end");

  builder->CreateBr(condBB);
  builder->SetInsertPoint(condBB);

  if (node->condition) {
    node->condition->accept(this);
    llvm::Value *condV = currentVal;
    if (condV->getType()->isPointerTy()) {
      condV = builder->CreateIsNotNull(condV, "ptr_val");
    }
    builder->CreateCondBr(condV, bodyBB, mergeBB);
  } else {
    builder->CreateBr(bodyBB);
  }

  continueTargets.push_back(incBB);
  breakTargets.push_back(mergeBB);
  loopScopeDepths.push_back(valueScopes.size());

  func->insert(func->end(), bodyBB);
  builder->SetInsertPoint(bodyBB);
  enterScope();
  for (auto &stmt : node->body) {
    stmt->accept(this);
    if (builder->GetInsertBlock()->getTerminator())
      break;
  }
  exitScope();

  continueTargets.pop_back();
  breakTargets.pop_back();
  loopScopeDepths.pop_back();

  if (!builder->GetInsertBlock()->getTerminator()) {
    builder->CreateBr(incBB);
  }

  func->insert(func->end(), incBB);
  builder->SetInsertPoint(incBB);
  if (node->update)
    node->update->accept(this);
  builder->CreateBr(condBB);

  func->insert(func->end(), mergeBB);
  builder->SetInsertPoint(mergeBB);

  exitScope();
}

void CodeGen::visit(BreakNode *node) {
  if (!breakTargets.empty()) {
    emitScopeCleanup(loopScopeDepths.back());
    builder->CreateBr(breakTargets.back());
  }
}

void CodeGen::visit(ContinueNode *node) {
  if (!continueTargets.empty()) {
    emitScopeCleanup(loopScopeDepths.back());
    builder->CreateBr(continueTargets.back());
  }
}

void CodeGen::visit(DeleteNode *node) {
  node->pointerExpr->accept(this);
  llvm::Value *ptrVal = currentVal;
  TypeInfo ptrType = currentType;

  if (node->isArray) {
    std::string dtorName = ptrType.base + "_~" + ptrType.base;
    bool hasDtor = (module->getFunction(dtorName) != nullptr);

    if (hasDtor) {
      llvm::Value *rawMem = builder->CreateInBoundsGEP(
          llvm::Type::getInt8Ty(*context), ptrVal,
          llvm::ConstantInt::get(*context, llvm::APInt(64, -8)), "raw_mem");

      llvm::Value *sizeVal = builder->CreateLoad(
          llvm::Type::getInt64Ty(*context), rawMem, "array_size");

      emitLifecycleLoop(ptrVal, sizeVal, ptrType.base, true);
      builder->CreateCall(getFreePrototype(), {rawMem});
    } else {
      builder->CreateCall(getFreePrototype(), {ptrVal});
    }
  } else {
    if (ptrType.ptrDepth > 0 && structTypes.count(ptrType.base)) {
      std::string dtorName = ptrType.base + "_~" + ptrType.base;
      llvm::Function *dtor = module->getFunction(dtorName);
      if (dtor)
        builder->CreateCall(dtor, {ptrVal});
    }
    builder->CreateCall(getFreePrototype(), {ptrVal});
  }
}

void CodeGen::visit(MoveNode *node) {
  // Evil pointer manipulation: Force LValue evaluation to extract the raw
  // memory address, but tag it as an RValue reference so the caller
  // knows it's safe to gut it.
  bool oldCtx = isLValueContext;
  isLValueContext = true;
  node->operand->accept(this);
  isLValueContext = oldCtx;

  currentVal = currentLValue;

  // Burn the evidence. We own this address now.
  currentLValue = nullptr;

  currentType.isRValueRef = true;
  currentType.isReference = false;
}

// --------------------------------------------------------------------------
// Expression Visitors
// --------------------------------------------------------------------------

void CodeGen::visit(VariableNode *node) {
  auto alloca = lookupValue(node->name);
  auto type = lookupType(node->name);

  if (!alloca) {
    if (globalVarTypes.count(node->name)) {
      llvm::GlobalVariable *gVar = module->getNamedGlobal(node->name);
      if (!gVar) {
        // Declaración en vuelo. El linker resolverá esto luego.
        llvm::Type *llTy = getLLVMType(globalVarTypes[node->name]);
        gVar = static_cast<llvm::GlobalVariable *>(
            module->getOrInsertGlobal(node->name, llTy));
      }
      alloca = gVar;
      type = globalVarTypes[node->name];
    } else {
      if (currentType.isPrimitive()) {
        currentVal = nullptr;
        currentType = {node->name, 0, false};
        return;
      }

      auto thisAddr = lookupValue("this");
      if (thisAddr) {
        TypeInfo thisType = lookupType("this");
        std::string className = thisType.base;
        if (structMemberIndices[className].count(node->name)) {
          llvm::Value *thisPtr = builder->CreateLoad(
              llvm::PointerType::get(*context, 0), thisAddr, "this.ptr");
          int idx = structMemberIndices[className][node->name];
          TypeInfo fType = structMemberTypes[className][node->name];
          llvm::Value *fieldAddr =
              builder->CreateStructGEP(structTypes[className], thisPtr, idx,
                                       className + "." + node->name);

          if (isLValueContext) {
            currentLValue = fieldAddr;
          } else {
            currentVal = builder->CreateLoad(getLLVMType(fType), fieldAddr,
                                             "load_member");
          }
          currentType = fType;
          return;
        }
      }
      // We hit a naked symbol that isn't mapped to an address.
      // If it's a known struct, the AST is likely resolving a static method
      // call (e.g., Entity.create()) and evaluated the class symbol as an
      // object. Yield a null pointer and the type identity, CallNode will take
      // it from here.
      if (structTypes.count(node->name)) {
        currentVal = nullptr;
        currentType = {node->name, 0, false};
        return;
      }

      std::cerr << "CodeGen Error: Undeclared variable '" << node->name
                << "'\n";
      exit(1);
    }
  }

  llvm::Value *baseAddr = alloca;
  if (type.isReference || type.isRValueRef) {
    baseAddr = builder->CreateLoad(llvm::PointerType::get(*context, 0), alloca,
                                   node->name + ".ref");
  }

  if (isLValueContext) {
    currentLValue = baseAddr;
  } else {
    if (structTypes.count(type.base) && type.ptrDepth == 0) {
      currentVal = baseAddr;
    } else if (type.arrayDimensions > 0) {
      currentVal = baseAddr;
    } else {
      // strip reference flag. if we don't, getLLVMType returns ptr
      // and we double-load garbage directly into the stack.
      TypeInfo valType = type;
      valType.isReference = false;
      currentVal =
          builder->CreateLoad(getLLVMType(valType), baseAddr, node->name);
    }
  }

  currentType = type;
  if (currentType.arrayDimensions > 0) {
    // Decay full dimensions into pointer indirection.
    currentType.ptrDepth += currentType.arrayDimensions;
    currentType.arrayDimensions = 0;
  }
}

void CodeGen::visit(BinaryOpNode *node) {
  node->left->accept(this);
  llvm::Value *L = currentVal;
  TypeInfo LT = currentType;

  node->right->accept(this);
  llvm::Value *R = currentVal;
  TypeInfo RT = currentType;

  bool isF = LT.isFloat() || RT.isFloat();

  if (isF) {
    if (LT.base == "int" && !LT.isPointer())
      L = builder->CreateSIToFP(L, llvm::Type::getDoubleTy(*context));
    if (RT.base == "int" && !RT.isPointer())
      R = builder->CreateSIToFP(R, llvm::Type::getDoubleTy(*context));

    // LLVM panics if we feed it mixed precision floats for FCmp.
    // Promote the 32-bit weakling to 64-bit before the backend loses its mind.
    if (L->getType()->isFloatTy() && R->getType()->isDoubleTy()) {
      L = builder->CreateFPExt(L, llvm::Type::getDoubleTy(*context));
    } else if (L->getType()->isDoubleTy() && R->getType()->isFloatTy()) {
      R = builder->CreateFPExt(R, llvm::Type::getDoubleTy(*context));
    }
  } else {
    if (L->getType() != R->getType()) {
      if (L->getType()->isPointerTy() && !R->getType()->isPointerTy()) {
        R = builder->CreateIntToPtr(R, L->getType());
      } else if (!L->getType()->isPointerTy() && R->getType()->isPointerTy()) {
        L = builder->CreateIntToPtr(L, R->getType());
      } else {
        uint64_t LBits =
            module->getDataLayout().getTypeSizeInBits(L->getType());
        uint64_t RBits =
            module->getDataLayout().getTypeSizeInBits(R->getType());
        if (LBits < RBits)
          L = builder->CreateZExt(L, R->getType());
        else if (RBits < LBits)
          R = builder->CreateZExt(R, L->getType());
      }
    }
  }

  if (node->op == "&&") {
    currentVal = builder->CreateAnd(L, R, "and_tmp");
    currentType = {"bool"};
    return;
  }
  if (node->op == "||") {
    currentVal = builder->CreateOr(L, R, "or_tmp");
    currentType = {"bool"};
    return;
  }

  if (node->op == "==") {
    currentVal =
        isF ? builder->CreateFCmpOEQ(L, R) : builder->CreateICmpEQ(L, R);
    currentType = {"bool", 0, false};
    return;
  }
  if (node->op == "!=") {
    currentVal =
        isF ? builder->CreateFCmpONE(L, R) : builder->CreateICmpNE(L, R);
    currentType = {"bool", 0, false};
    return;
  }
  if (node->op == "<") {
    currentVal = isF ? builder->CreateFCmpOLT(L, R)
                     : (LT.isUnsigned() ? builder->CreateICmpULT(L, R)
                                        : builder->CreateICmpSLT(L, R));
    currentType = {"bool", 0, false};
    return;
  }
  if (node->op == ">") {
    currentVal = isF ? builder->CreateFCmpOGT(L, R)
                     : (LT.isUnsigned() ? builder->CreateICmpUGT(L, R)
                                        : builder->CreateICmpSGT(L, R));
    currentType = {"bool", 0, false};
    return;
  }
  if (node->op == "<=") {
    currentVal =
        isF ? builder->CreateFCmpOLE(L, R) : builder->CreateICmpSLE(L, R);
    currentType = {"bool"};
    return;
  }
  if (node->op == ">=") {
    currentVal =
        isF ? builder->CreateFCmpOGE(L, R) : builder->CreateICmpSGE(L, R);
    currentType = {"bool"};
    return;
  }

  if (node->op == "+")
    currentVal = isF ? builder->CreateFAdd(L, R) : builder->CreateAdd(L, R);
  else if (node->op == "-")
    currentVal = isF ? builder->CreateFSub(L, R) : builder->CreateSub(L, R);
  else if (node->op == "*")
    currentVal = isF ? builder->CreateFMul(L, R) : builder->CreateMul(L, R);
  else if (node->op == "/")
    currentVal = isF ? builder->CreateFDiv(L, R)
                     : (LT.isUnsigned() ? builder->CreateUDiv(L, R)
                                        : builder->CreateSDiv(L, R));
  else if (node->op == "%") {
    currentVal = isF ? builder->CreateFRem(L, R)
                     : (LT.isUnsigned() ? builder->CreateURem(L, R)
                                        : builder->CreateSRem(L, R));
  }

  currentType = isF ? TypeInfo{"float", 0, false} : LT;
}

void CodeGen::visit(NumberNode *node) {
  currentVal =
      llvm::ConstantInt::get(*context, llvm::APInt(32, node->value, true));
  currentType = {"int", 0, false};
}

void CodeGen::visit(FloatNode *node) {
  if (node->isDouble) {
    currentVal = llvm::ConstantFP::get(*context, llvm::APFloat(node->value));
    currentType = {"double", 0, false};
  } else {
    currentVal =
        llvm::ConstantFP::get(*context, llvm::APFloat((float)node->value));
    currentType = {"float", 0, false};
  }
}

void CodeGen::visit(BoolNode *node) {
  currentVal =
      llvm::ConstantInt::get(*context, llvm::APInt(1, node->value, false));
  currentType = {"bool", 0, false};
}

void CodeGen::visit(StringNode *node) {
  currentVal = getOrCreateString(node->value);
  currentType = {"char", 1, false};
}

void CodeGen::visit(UnaryMinusNode *node) {
  node->operand->accept(this);
  if (currentType.base == "int") {
    currentVal = builder->CreateNeg(currentVal, "neg_int");
  } else if (currentType.base == "float") {
    currentVal = builder->CreateFNeg(currentVal, "neg_float");
  } else {
    std::cerr << "CodeGen Error: Unary minus on non-numeric type.\n";
    exit(1);
  }
}

void CodeGen::visit(NullLiteralNode *node) {
  currentVal =
      llvm::ConstantPointerNull::get(llvm::PointerType::get(*context, 0));
  currentType = {"null", 0, true};
}

void CodeGen::visit(AddressOfNode *node) {
  bool old = isLValueContext;
  isLValueContext = true;
  node->operand->accept(this);
  isLValueContext = old;
  currentVal = currentLValue;

  // Wipe the L-value tag. If left alive, CallNode assumes a memory
  // location and executes a 64-bit pointer load over adjacent 32-bit
  // array elements. 0x0000000A00000000
  currentLValue = nullptr;

  currentType.ptrDepth++;
}

void CodeGen::visit(DerefNode *node) {
  bool oldContext = isLValueContext;
  isLValueContext = false;
  node->operand->accept(this);
  isLValueContext = oldContext;

  currentType.ptrDepth--;

  if (isLValueContext) {
    currentLValue = currentVal;
  } else {
    currentVal =
        builder->CreateLoad(getLLVMType(currentType), currentVal, "deref");
  }
}

void CodeGen::visit(NewNode *node) {
  TypeInfo baseType = parseTypeString(node->typeName);
  llvm::Type *llvmType = getLLVMType(baseType);

  std::vector<llvm::Value *> sizeVals;
  for (auto &szNode : node->arraySizes) {
    szNode->accept(this);
    llvm::Value *sz = currentVal;
    if (sz->getType()->isIntegerTy(32))
      sz = builder->CreateZExt(sz, llvm::Type::getInt64Ty(*context));
    sizeVals.push_back(sz);
  }

  if (!sizeVals.empty()) {
    // WTF: Single-Allocation Iliffe Vector Heap Allocation.
    // Adiós al laberinto de fragmentación. Un solo malloc y que la caché fluya.
    llvm::Type *i64 = builder->getInt64Ty();
    llvm::Type *i8 = builder->getInt8Ty();
    llvm::Type *ptrTy = llvm::PointerType::get(*context, 0);

    std::vector<llvm::Value *> levelCounts;
    llvm::Value *curCount = builder->getInt64(1);
    for (auto sz : sizeVals) {
      curCount = builder->CreateMul(curCount, sz);
      levelCounts.push_back(curCount);
    }

    llvm::Value *totalData = levelCounts.back();
    llvm::Value *totalPointers = builder->getInt64(0);

    for (size_t i = 0; i < sizeVals.size() - 1; ++i) {
      totalPointers = builder->CreateAdd(totalPointers, levelCounts[i]);
    }

    uint64_t elemSize =
        module->getDataLayout().getTypeAllocSize(getLLVMType(baseType));
    llvm::Value *ptrBytes =
        builder->CreateMul(totalPointers, builder->getInt64(8));
    llvm::Value *dataBytes =
        builder->CreateMul(totalData, builder->getInt64(elemSize));
    llvm::Value *totalBytes = builder->CreateAdd(ptrBytes, dataBytes);

    // Aseguramos la supervivencia agregando 8 bytes extra para que DeleteNode
    // no reviente.
    llvm::Value *allocBytes =
        builder->CreateAdd(totalBytes, builder->getInt64(8));
    llvm::Value *rawMem = builder->CreateCall(getMallocPrototype(),
                                              {allocBytes}, "flat_matrix_heap");

    // Anclamos el tamaño en el offset -8 para DeleteNode.
    builder->CreateStore(
        totalData,
        builder->CreateBitCast(rawMem, llvm::PointerType::get(i64, 0)));
    llvm::Value *blockMem =
        builder->CreateInBoundsGEP(i8, rawMem, builder->getInt64(8));

    std::vector<llvm::Value *> levelOffsets;
    llvm::Value *currentOffset = builder->getInt64(0);
    for (size_t i = 0; i < sizeVals.size() - 1; ++i) {
      levelOffsets.push_back(currentOffset);
      llvm::Value *levelBytes =
          builder->CreateMul(levelCounts[i], builder->getInt64(8));
      currentOffset = builder->CreateAdd(currentOffset, levelBytes);
    }
    llvm::Value *dataOffset = currentOffset;
    levelOffsets.push_back(dataOffset);

    // The Pointer Threader
    llvm::Function *func = builder->GetInsertBlock()->getParent();
    for (size_t L = 0; L < sizeVals.size() - 1; ++L) {
      llvm::Value *numPtrs = levelCounts[L];
      llvm::Value *stride = sizeVals[L + 1];
      llvm::Value *curLvlOffset = levelOffsets[L];
      llvm::Value *nextLvlOffset = levelOffsets[L + 1];
      llvm::Value *nextElemSz =
          builder->getInt64((L == sizeVals.size() - 2) ? elemSize : 8);

      llvm::BasicBlock *condBB =
          llvm::BasicBlock::Create(*context, "arr.cond", func);
      llvm::BasicBlock *bodyBB =
          llvm::BasicBlock::Create(*context, "arr.body", func);
      llvm::BasicBlock *endBB =
          llvm::BasicBlock::Create(*context, "arr.end", func);

      llvm::AllocaInst *idxPtr = builder->CreateAlloca(i64, nullptr, "idx");
      builder->CreateStore(builder->getInt64(0), idxPtr);
      builder->CreateBr(condBB);

      builder->SetInsertPoint(condBB);
      llvm::Value *idx = builder->CreateLoad(i64, idxPtr);
      llvm::Value *cmp = builder->CreateICmpULT(idx, numPtrs);
      builder->CreateCondBr(cmp, bodyBB, endBB);

      builder->SetInsertPoint(bodyBB);
      llvm::Value *ptrOffset = builder->CreateAdd(
          curLvlOffset, builder->CreateMul(idx, builder->getInt64(8)));
      llvm::Value *ptrAddr =
          builder->CreateInBoundsGEP(i8, blockMem, ptrOffset);
      ptrAddr = builder->CreateBitCast(ptrAddr, ptrTy);

      llvm::Value *targetOffset = builder->CreateAdd(
          nextLvlOffset,
          builder->CreateMul(builder->CreateMul(idx, stride), nextElemSz));
      llvm::Value *targetAddr =
          builder->CreateInBoundsGEP(i8, blockMem, targetOffset);

      builder->CreateStore(targetAddr, ptrAddr);

      builder->CreateStore(builder->CreateAdd(idx, builder->getInt64(1)),
                           idxPtr);
      builder->CreateBr(condBB);
      builder->SetInsertPoint(endBB);
    }

    llvm::Value *dataStart =
        builder->CreateInBoundsGEP(i8, blockMem, dataOffset);
    emitLifecycleLoop(builder->CreateBitCast(dataStart, ptrTy), totalData,
                      node->typeName, false);

    currentVal = builder->CreateBitCast(blockMem, ptrTy);
  } else {
    uint64_t typeBytes = module->getDataLayout().getTypeAllocSize(llvmType);
    llvm::Value *typeSz =
        llvm::ConstantInt::get(*context, llvm::APInt(64, typeBytes));
    llvm::Value *rawMem =
        builder->CreateCall(getMallocPrototype(), {typeSz}, "raw_mem");
    std::vector<TypeInfo> expectedParams;
    if (functionParamTypes.count(node->resolvedMangledName)) {
      expectedParams = functionParamTypes[node->resolvedMangledName];
    }

    std::vector<llvm::Value *> callArgs;
    callArgs.push_back(rawMem);

    int pIdx = 1;
    for (auto &arg : node->arguments) {
      arg->accept(this);
      llvm::Value *argVal = currentVal;

      /* Force alignment for overloaded constructors. */
      if (pIdx < expectedParams.size()) {
        argVal = castValue(argVal, currentType, expectedParams[pIdx]);
      }

      callArgs.push_back(argVal);
      pIdx++;
    }

    llvm::Function *ctor = module->getFunction(node->resolvedMangledName);
    if (ctor) {
      builder->CreateCall(ctor, callArgs);
    } else if (!node->arguments.empty()) {
      node->arguments[0]->accept(this);

      /* Raw memory injection for scalars. Bypass the constructor lookup. */
      llvm::Value *initVal = castValue(currentVal, currentType, baseType);
      builder->CreateStore(initVal, rawMem);
    }
    currentVal = rawMem;
  }

  currentType = baseType;
  currentType.ptrDepth +=
      node->arraySizes.empty() ? 1 : node->arraySizes.size();
}

void CodeGen::visit(CallNode *node) {
  emitLocation(node);

  if (node->callee == "@assign_deref") {
    isLValueContext = true;
    node->arguments[0]->accept(this);
    isLValueContext = false;
    llvm::Value *addr = currentLValue;
    TypeInfo targetT = currentType;
    node->arguments[1]->accept(this);
    emitCopyOrStore(addr, castValue(currentVal, currentType, targetT), targetT,
                    currentType);
    return;
  }

  if (node->callee == "print") {
    std::string fmt = "";
    std::vector<llvm::Value *> args = {nullptr};
    for (auto &arg : node->arguments) {
      arg->accept(this);
      if (currentType.base == "int") {
        fmt += "%d ";
        args.push_back(currentVal);
      } else if (currentType.base == "float") {
        fmt += "%f ";
        llvm::Value *doubleVal =
            builder->CreateFPExt(currentVal, llvm::Type::getDoubleTy(*context));
        args.push_back(doubleVal);
      } else {
        fmt += "%s ";
        args.push_back(currentVal);
      }
    }
    fmt += "\n";
    args[0] = getOrCreateString(fmt);
    builder->CreateCall(getPrintfPrototype(), args);
    return;
  }

  if (node->callee == "int" || node->callee == "float" ||
      node->callee == "bool") {
    node->arguments[0]->accept(this);
    TypeInfo targetType{node->callee, 0, false};
    currentVal = castValue(currentVal, currentType, targetType);
    currentType = targetType;
    return;
  }

  if (!node->object && structTypes.count(node->callee)) {
    llvm::Value *target = nullptr;

    if (rvoTarget) {
      target = rvoTarget;
      rvoTarget = nullptr;
    } else {
      target = builder->CreateAlloca(structTypes[node->callee], nullptr,
                                     "stack.obj");
    }

    std::vector<llvm::Value *> ctorArgs;
    ctorArgs.push_back(target);

    std::vector<TypeInfo> expectedParams;
    if (functionParamTypes.count(node->resolvedMangledName)) {
      expectedParams = functionParamTypes[node->resolvedMangledName];
    }

    int pIdx = 1;
    for (auto &a : node->arguments) {
      a->accept(this);
      llvm::Value *argVal = currentVal;

      if (pIdx < expectedParams.size()) {
        argVal = castValue(argVal, currentType, expectedParams[pIdx]);
      }
      ctorArgs.push_back(argVal);
      pIdx++;
    }

    llvm::Function *ctor = module->getFunction(node->resolvedMangledName);
    if (ctor)
      builder->CreateCall(ctor, ctorArgs);

    currentVal = target;
    currentType = {node->callee, 0, false};

    return;
  }

  llvm::Value *thisArg = nullptr;
  std::string baseName = node->callee;

  if (node->object) {
    bool oldCtx = isLValueContext;

    // What the fuck? isLValueContext = true was leaving currentVal stale,
    // feeding a void CallInst hallucination as the 'this' pointer.
    // We need the actual evaluated pointer, not an L-value ghost.
    isLValueContext = false;
    node->object->accept(this);
    isLValueContext = oldCtx;

    thisArg = currentVal;
    baseName = currentType.base + "_" + node->callee;
  }

  std::string finalMangled = node->resolvedMangledName;
  llvm::Function *f = module->getFunction(finalMangled);

  if (!f) {
    if (functionTypes.count(finalMangled)) {
      std::vector<llvm::Type *> pTypes;
      for (const auto &pt : functionParamTypes[finalMangled]) {
        pTypes.push_back(getLLVMType(pt));
      }
      llvm::FunctionType *fTy = llvm::FunctionType::get(
          getLLVMType(functionTypes[finalMangled]), pTypes, false);
      f = llvm::Function::Create(fTy, llvm::Function::ExternalLinkage,
                                 finalMangled, module.get());
    } else {
      std::cerr << "[Utopia Fatal] CodeGen Error: Undefined function or "
                   "unresolved overload '"
                << finalMangled << "'.\n";
      exit(1);
    }
  }

  if (thisArg && f->arg_size() == node->arguments.size()) {
    thisArg = nullptr;
  }

  if (!thisArg && node->object && f->arg_size() > node->arguments.size()) {
    thisArg = llvm::Constant::getNullValue(getLLVMType(currentType));
  }

  std::vector<llvm::Value *> args;
  int argOffset = 0;

  if (thisArg) {
    args.push_back(thisArg);
    argOffset = 1;
  }

  std::vector<TypeInfo> paramTypes;
  auto it = functionParamTypes.find(finalMangled);
  if (it != functionParamTypes.end()) {
    paramTypes = it->second;
  } else {
    for (size_t j = 0; j < node->arguments.size() + argOffset; ++j) {
      paramTypes.push_back(TypeInfo{"int", 0, false});
    }
  }

  for (size_t i = 0; i < node->arguments.size(); ++i) {
    size_t paramIdx = i + argOffset;
    bool isRefParam = (paramIdx < paramTypes.size())
                          ? paramTypes[paramIdx].isReference
                          : false;

    bool expectsLValue = isRefParam;
    bool oldCtx = isLValueContext;
    isLValueContext = expectsLValue;
    currentLValue = nullptr;
    node->arguments[i]->accept(this);
    isLValueContext = oldCtx;

    llvm::Value *argVal = currentVal;

    if (expectsLValue) {
      if (currentLValue) {
        argVal = currentLValue;
      } else {
        argVal = currentVal;
      }
    }

    if (!expectsLValue && currentType.ptrDepth == 0 &&
        !currentType.isReference && structTypes.count(currentType.base)) {
      llvm::AllocaInst *temp =
          builder->CreateAlloca(getLLVMType(currentType), nullptr, "arg.temp");
      TypeInfo targetType = currentType;
      targetType.isRValueRef = false;
      targetType.isReference = false;
      emitCopyOrStore(temp, argVal, targetType, currentType);
      argVal = temp;
    } else if (!expectsLValue && paramIdx < paramTypes.size()) {
      argVal = castValue(argVal, currentType, paramTypes[paramIdx]);
    }

    args.push_back(argVal);
  }

  currentVal = builder->CreateCall(f, args);
  currentType = functionTypes.count(finalMangled) ? functionTypes[finalMangled]
                                                  : TypeInfo{"int"};
}

void CodeGen::visit(NullAssertNode *node) {
  // Se evalua el operando. Preservamos isLValueContext por si estamos afirmando
  // un l-value (ej. un puntero en una estructura).
  node->operand->accept(this);
  llvm::Value *ptrVal = currentVal;
  llvm::Value *preservedLValue = currentLValue;

  /*
   * Fast-path hardware validation.
   * Branch prediction assumes the valid block. Cost is effectively 0 cycles
   * on modern superscalar CPUs due to speculative execution.
   */
  llvm::Function *func = builder->GetInsertBlock()->getParent();
  llvm::BasicBlock *validBB =
      llvm::BasicBlock::Create(*context, "assert.valid", func);
  llvm::BasicBlock *panicBB =
      llvm::BasicBlock::Create(*context, "assert.panic", func);

  llvm::Value *isNull = builder->CreateIsNull(ptrVal, "is_null_check");
  builder->CreateCondBr(isNull, panicBB, validBB);

  // --- PANIC BLOCK ---
  builder->SetInsertPoint(panicBB);
  llvm::FunctionCallee printfFunc = getPrintfPrototype();
  llvm::Value *panicMsg =
      getOrCreateString("Utopia Runtime Panic: Attempted to dereference a null "
                        "pointer via '!' assertion.\n");
  builder->CreateCall(printfFunc, {panicMsg});

  llvm::FunctionCallee exitFunc = module->getOrInsertFunction(
      "exit",
      llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
                              {llvm::Type::getInt32Ty(*context)}, false));
  builder->CreateCall(
      exitFunc, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1)});

  // Optimizer hint: Let LLVM know execution terminates here to avoid CFG
  // corruption.
  builder->CreateUnreachable();

  // --- VALID BLOCK ---
  builder->SetInsertPoint(validBB);

  currentType.isNullable = false;
  currentVal = ptrVal;
  currentLValue = preservedLValue;
}

void CodeGen::visit(LogicalNotNode *node) {
  node->operand->accept(this);

  // Flip the bit.
  currentVal = builder->CreateNot(currentVal, "log_not");
  currentType = {"bool", 0, false};
}

// --------------------------------------------------------------------------
// External Entrypoints (generate, optimize, emit)
// --------------------------------------------------------------------------

void CodeGen::generate(ProgramNode *program) {
  program->accept(this);
  if (isDebug) {
    dbgBuilder->finalize();
  }
}

void CodeGen::optimize(int level) {
  if (level == 0)
    return;

  llvm::OptimizationLevel optLevel;
  switch (level) {
  case 1:
    optLevel = llvm::OptimizationLevel::O1;
    break;
  case 2:
    optLevel = llvm::OptimizationLevel::O2;
    break;
  case 3:
    optLevel = llvm::OptimizationLevel::O3;
    break;
  default:
    optLevel = llvm::OptimizationLevel::O3;
    break;
  }

  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;

  llvm::PassBuilder pb(targetMachine.get());
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(optLevel);
  mpm.run(*module, mam);

  std::cout << "[CodeGen] Optimizations applied: O" << level << "\n";
}

void CodeGen::saveToFile(const std::string &filename) {
  std::error_code EC;
  llvm::raw_fd_ostream dest(filename, EC, llvm::sys::fs::OF_None);

  if (EC) {
    std::cerr << "[Fatal] CodeGen IO Error: " << EC.message() << " ("
              << filename << ")\n";
    exit(1);
  }

  module->print(dest, nullptr);
  std::cout << "[CodeGen] LLVM IR en: " << filename << "\n";
}

void CodeGen::emitObjectFile(const std::string &filename) {
  std::error_code EC;
  llvm::raw_fd_ostream dest(filename, EC, llvm::sys::fs::OF_None);

  if (EC) {
    std::cerr << "Could not open object file: " << EC.message() << "\n";
    return;
  }

  llvm::legacy::PassManager pass;
  auto fileType = llvm::CodeGenFileType::ObjectFile;

  if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
    std::cerr << "TargetMachine can't emit a file of this type\n";
    return;
  }

  pass.run(*module);
  dest.flush();
  std::cout << "[CodeGen] Object generated: " << filename << "\n";
}

void CodeGen::emitAssemblyFile(const std::string &filename) {
  std::error_code EC;
  llvm::raw_fd_ostream dest(filename, EC, llvm::sys::fs::OF_None);
  if (EC) {
    std::cerr << "Could not open assembly file: " << EC.message() << "\n";
    return;
  }

  llvm::legacy::PassManager pass;
  auto fileType = llvm::CodeGenFileType::AssemblyFile;

  if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
    std::cerr << "TargetMachine can't emit assembly file\n";
    return;
  }

  pass.run(*module);
  dest.flush();
  std::cout << "[CodeGen] Assembly generated: " << filename << "\n";
}

void CodeGen::visit(ModuleNode *node) {
  enterScope();

  for (auto &st : node->structs) {
    for (auto &f : st->fields) {
      if (f.isStatic) {
        std::string globalName = st->name + "_" + f.name;
        llvm::Type *llvmType = getLLVMType(structMemberTypes[st->name][f.name]);
        module->getOrInsertGlobal(globalName, llvmType);
        llvm::GlobalVariable *gVar = module->getNamedGlobal(globalName);

        gVar->setLinkage(llvm::GlobalValue::ExternalLinkage);
        gVar->setInitializer(llvm::Constant::getNullValue(llvmType));
      }
    }
  }

  currentClass = "";

  for (auto &st : node->structs) {
    for (auto &method : st->methods) {
      std::string originalName = method->name;
      method->name = st->name + "_" + method->name;

      // Inject 'this'. Constructors are instance methods in disguise.
      // If we skip this, the linker bleeds out searching for a phantom
      // signature.
      if (!method->isStatic) {
        TypeInfo thisType = {st->name, 1, false};
        method->name += "_" + getMangledType(thisType);
      }

      for (auto &arg : method->args) {
        TypeInfo t = arg.isThisAssign ? structMemberTypes[st->name][arg.name]
                                      : parseTypeString(arg.type);
        method->name += "_" + getMangledType(t);
      }
      method->accept(this);
      method->name = originalName;
    }
  }

  for (auto &ext : node->extensions) {
    for (auto &method : ext->methods) {
      std::string originalName = method->name;
      method->name = "ext_" + ext->targetTypedef + "_" + method->name;
      TypeInfo targetType = parseTypeString(ext->targetTypedef);
      method->name += "_" + getMangledType(targetType);
      for (auto &arg : method->args) {
        TypeInfo t = arg.isThisAssign
                         ? structMemberTypes[ext->targetTypedef][arg.name]
                         : parseTypeString(arg.type);
        method->name += "_" + getMangledType(t);
      }
      method->accept(this);
      method->name = originalName;
    }
  }

  llvm::FunctionType *initTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(*context), false);
  llvm::Function *staticInitF =
      llvm::Function::Create(initTy, llvm::Function::InternalLinkage,
                             "__utopia_global_init", module.get());

  for (auto &func : node->functions) {
    std::string originalName = func->name;
    for (auto &arg : func->args)
      func->name += "_" + getMangledType(parseTypeString(arg.type));
    func->accept(this);
    func->name = originalName;
  }

  llvm::BasicBlock *initBB =
      llvm::BasicBlock::Create(*context, "entry", staticInitF);
  builder->SetInsertPoint(initBB);

  builder->SetCurrentDebugLocation(llvm::DebugLoc());

  for (auto &var : node->globalVars) {
    TypeInfo declType = globalVarTypes[var->name];
    llvm::Type *llvmType = getLLVMType(declType);

    module->getOrInsertGlobal(var->name, llvmType);
    llvm::GlobalVariable *gVar = module->getNamedGlobal(var->name);
    gVar->setLinkage(llvm::GlobalValue::ExternalLinkage);
    gVar->setInitializer(llvm::Constant::getNullValue(llvmType));

    if (var->initializer) {
      var->initializer->accept(this);
      emitCopyOrStore(gVar, castValue(currentVal, currentType, declType),
                      declType, currentType);
    }
  }

  builder->CreateRetVoid();

  exitScope();
}

} // namespace utopia