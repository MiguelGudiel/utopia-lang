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
#include <unordered_set>

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
    pointee.isArray = false;
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
  if (type.ptrDepth > 0 || type.isReference || type.isRValueRef)
    return llvm::PointerType::get(*context, 0);
  if (structTypes.count(type.base))
    return structTypes[type.base];
  if (type.base == "void")
    return llvm::Type::getVoidTy(*context);
  if (type.base == "int" || type.base == "uint")
    return llvm::Type::getInt32Ty(*context);
  if (type.base == "float")
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
  if (from.isPointer() && to.isPointer())
    return value;

  if (!from.isPointer() && !to.isPointer() && structTypes.count(to.base)) {
    return value;
  }

  if (!from.isPointer() && !to.isPointer()) {
    if (from.base == "int" && to.base == "float")
      return builder->CreateSIToFP(value, llvm::Type::getDoubleTy(*context));
    if (from.base == "float" && to.base == "int")
      return builder->CreateFPToSI(value, llvm::Type::getInt32Ty(*context));
    if (from.base == "bool" && to.base == "int")
      return builder->CreateZExt(value, llvm::Type::getInt32Ty(*context));
  }
  if (from.isPointer() && !to.isPointer())
    return builder->CreatePtrToInt(value, llvm::Type::getInt64Ty(*context));
  if (!from.isPointer() && to.isPointer())
    return builder->CreateIntToPtr(value, llvm::PointerType::get(*context, 0));
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
        std::cerr << "CodeGen Error: Static field " << globalName
                  << " not found.\n";
        exit(1);
      }
      if (isLValueContext) {
        currentLValue = globalVar;
      } else {
        currentVal =
            builder->CreateLoad(getLLVMType(fType), globalVar, "load_static");
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
      currentVal =
          builder->CreateLoad(getLLVMType(fType), globalVar, "load_static");
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
    currentVal =
        builder->CreateLoad(getLLVMType(fType), fieldAddr, "load_member");
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
      static const std::unordered_set<std::string> primitives = {
          "int", "float", "bool", "uint", "char"};
      if (primitives.count(type.base) && type.ptrDepth == 1) {
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

  static const std::unordered_set<std::string> primitives = {
      "int", "float", "bool", "uint", "char"};

  if ((node->isMethod && !node->isStatic) || node->isConstructor ||
      node->isDestructor) {
    if (primitives.count(node->className)) {
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

  if (node->name == "main") {
    llvm::FunctionCallee initF = module->getOrInsertFunction(
        "__utopia_global_init",
        llvm::FunctionType::get(llvm::Type::getVoidTy(*context), false));
    builder->CreateCall(initF);
  }

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
    }
  }

  if (node->isConstructor) {
    StructDeclNode *classAST = structASTs[node->className];
    if (classAST) {
      /* Cache 'this' for the field initialization massacre. */
      llvm::Value *thisPtr = builder->CreateLoad(
          llvm::PointerType::get(*context, 0), lookupValue("this"));

      for (auto &field : classAST->fields) {
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
      } else if (t->isDoubleTy()) {
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

  if (node->arraySize) {
    node->arraySize->accept(this);
    llvm::Value *sizeVal = currentVal;

    llvm::AllocaInst *alloca =
        builder->CreateAlloca(llvmType, sizeVal, node->name);

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

    emitLifecycleLoop(alloca, sizeVal, node->typeName, false);

    declType.isArray = true;
    valueScopes.back().push_back({node->name, alloca});
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

      // Set the RVO trap. We tell the child expression exactly where to build.
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
        // RVO Verification: If the returned value IS our allocated address,
        // the constructor executed in-place. We bypass the copy constructor.
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
    static const std::unordered_set<std::string> primitives = {
        "int", "float", "bool", "uint", "char", "String", "void"};

    if (primitives.count(node->name)) {
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
        llvm::Value *fieldAddr = builder->CreateStructGEP(
            structTypes[className], thisPtr, idx, className + "." + node->name);

        if (isLValueContext) {
          currentLValue = fieldAddr;
        } else {
          currentVal =
              builder->CreateLoad(getLLVMType(fType), fieldAddr, "load_member");
        }
        currentType = fType;
        return;
      }
    }
    // We hit a naked symbol that isn't mapped to an address.
    // If it's a known struct, the AST is likely resolving a static method call
    // (e.g., Entity.create()) and evaluated the class symbol as an object.
    // Yield a null pointer and the type identity, CallNode will take it from
    // here.
    if (structTypes.count(node->name)) {
      currentVal = nullptr;
      currentType = {node->name, 0, false};
      return;
    }

    std::cerr << "CodeGen Error: Undeclared variable '" << node->name << "'\n";
    exit(1);
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
    } else if (type.isArray) {
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
  if (currentType.isArray) {
    currentType.isArray = false;
    currentType.ptrDepth++;
  }
}

void CodeGen::visit(BinaryOpNode *node) {
  node->left->accept(this);
  llvm::Value *L = currentVal;
  TypeInfo LT = currentType;

  node->right->accept(this);
  llvm::Value *R = currentVal;
  TypeInfo RT = currentType;

  bool isF = (LT.base == "float" && !LT.isPointer()) ||
             (RT.base == "float" && !RT.isPointer());

  if (isF) {
    if (LT.base == "int" && !LT.isPointer())
      L = builder->CreateSIToFP(L, llvm::Type::getDoubleTy(*context));
    if (RT.base == "int" && !RT.isPointer())
      R = builder->CreateSIToFP(R, llvm::Type::getDoubleTy(*context));
  } else {
    // If we are comparing a pointer to an integer (common in null checks), or
    // if the widths of the integers do not match, we force equality.
    if (L->getType() != R->getType()) {
      if (L->getType()->isPointerTy() && !R->getType()->isPointerTy()) {
        R = builder->CreateIntToPtr(R, L->getType());
      } else if (!L->getType()->isPointerTy() && R->getType()->isPointerTy()) {
        L = builder->CreateIntToPtr(L, R->getType());
      } else {
        // If we arrive here with integers of different sizes, we scale the
        // smallest to the size of the largest.
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
    currentType = {"bool"};
    return;
  }
  if (node->op == "!=") {
    currentVal =
        isF ? builder->CreateFCmpONE(L, R) : builder->CreateICmpNE(L, R);
    currentType = {"bool"};
    return;
  }
  if (node->op == "<") {
    currentVal =
        isF ? builder->CreateFCmpOLT(L, R) : builder->CreateICmpSLT(L, R);
    currentType = {"bool"};
    return;
  }
  if (node->op == ">") {
    currentVal =
        isF ? builder->CreateFCmpOGT(L, R) : builder->CreateICmpSGT(L, R);
    currentType = {"bool"};
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
    currentVal = isF ? builder->CreateFDiv(L, R) : builder->CreateSDiv(L, R);
  else if (node->op == "%") {
    // LLVM handles remainder strictly according to the underlying architecture
    // type: FRem for IEEE-754 floating-point numbers and SRem for two's
    // complement signed integers.
    currentVal = isF ? builder->CreateFRem(L, R) : builder->CreateSRem(L, R);
  }

  currentType = isF ? TypeInfo{"float", 0, false} : TypeInfo{"int", 0, false};
}

void CodeGen::visit(NumberNode *node) {
  currentVal =
      llvm::ConstantInt::get(*context, llvm::APInt(32, node->value, true));
  currentType = {"int", 0, false};
}

void CodeGen::visit(FloatNode *node) {
  currentVal = llvm::ConstantFP::get(*context, llvm::APFloat(node->value));
  currentType = {"float", 0, false};
}

void CodeGen::visit(BoolNode *node) {
  currentVal =
      llvm::ConstantInt::get(*context, llvm::APInt(1, node->value, false));
  currentType = {"bool", 0, false};
}

void CodeGen::visit(StringNode *node) {
  currentVal = getOrCreateString(node->value);
  currentType = {"String", 0, false};
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

  llvm::Value *sizeVal = nullptr;
  if (node->arraySize) {
    node->arraySize->accept(this);
    sizeVal = currentVal;
    if (sizeVal->getType()->isIntegerTy(32)) {
      sizeVal = builder->CreateZExt(sizeVal, llvm::Type::getInt64Ty(*context));
    }
  }

  uint64_t typeBytes = module->getDataLayout().getTypeAllocSize(llvmType);
  llvm::Value *typeSz =
      llvm::ConstantInt::get(*context, llvm::APInt(64, typeBytes));

  llvm::Value *totalSz = typeSz;
  bool needsCookie = false;

  if (sizeVal) {
    std::string dtorName = node->typeName + "_~" + node->typeName;
    needsCookie = (module->getFunction(dtorName) != nullptr);

    llvm::Value *arrayBytes =
        builder->CreateMul(typeSz, sizeVal, "array_bytes");

    if (needsCookie) {
      totalSz = builder->CreateAdd(
          arrayBytes, llvm::ConstantInt::get(*context, llvm::APInt(64, 8)),
          "total_sz_with_cookie");
    } else {
      totalSz = arrayBytes;
    }
  }

  llvm::Value *rawMem =
      builder->CreateCall(getMallocPrototype(), {totalSz}, "raw_mem");

  if (sizeVal) {
    if (needsCookie) {
      builder->CreateStore(sizeVal, rawMem);
      llvm::Value *arrayBase = builder->CreateInBoundsGEP(
          llvm::Type::getInt8Ty(*context), rawMem,
          llvm::ConstantInt::get(*context, llvm::APInt(64, 8)), "array_base");

      emitLifecycleLoop(arrayBase, sizeVal, node->typeName, false);
      currentVal = arrayBase;
    } else {
      emitLifecycleLoop(rawMem, sizeVal, node->typeName, false);
      currentVal = rawMem;
    }
  } else {
    std::vector<llvm::Value *> callArgs;
    callArgs.push_back(rawMem);

    for (auto &arg : node->arguments) {
      arg->accept(this);
      callArgs.push_back(currentVal);
    }

    // Retrieve the pre-computed constructor overload.
    llvm::Function *ctor = module->getFunction(node->resolvedMangledName);
    if (ctor) {
      builder->CreateCall(ctor, callArgs);
    } else if (!node->arguments.empty()) {
      // If there is no constructor but there are arguments (like 'new
      // float(50.5)') We take the first argument and store it directly in
      // memory.
      node->arguments[0]->accept(this);
      builder->CreateStore(currentVal, rawMem);
    }
    currentVal = rawMem;
  }

  currentType = baseType;
  currentType.ptrDepth = 1;
}

void CodeGen::visit(CallNode *node) {
  emitLocation(node);

  // Intrinsic: @assign_deref
  // Manual pointer surgery for cases where the high-level syntax is too polite.
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

  // Intrinsic: print
  // Because calling printf manually every time is a crime against humanity.
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
        args.push_back(currentVal);
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

  // Built-in type casts
  if (node->callee == "int" || node->callee == "float" ||
      node->callee == "bool") {
    node->arguments[0]->accept(this);
    TypeInfo targetType{node->callee, 0, false};
    currentVal = castValue(currentVal, currentType, targetType);
    currentType = targetType;
    return;
  }

  // Stack Allocation / RVO Simulation
  if (!node->object && structTypes.count(node->callee)) {
    llvm::Value *target = nullptr;

    // RVO Intercept: Inject directly into the caller's memory address.
    // Bypasses the 'stack.obj' temporary alloca and eliminates a copy cycle.
    if (rvoTarget) {
      target = rvoTarget;
      rvoTarget = nullptr; // Payload consumed.
    } else {
      target = builder->CreateAlloca(structTypes[node->callee], nullptr,
                                     "stack.obj");
    }

    std::vector<llvm::Value *> ctorArgs;
    ctorArgs.push_back(target);
    std::string mSig = "";

    for (auto &a : node->arguments) {
      a->accept(this);
      ctorArgs.push_back(currentVal);
    }

    // Trust the Sema's resolved constructor overload. We already did the math.
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
    isLValueContext = true;
    node->object->accept(this);
    isLValueContext = oldCtx;
    thisArg = currentVal;
    baseName = currentType.base + "_" + node->callee;
  }

  // Use the exact mangled name resolved by Sema.
  // We were ignoring Sema's resolution and guessing the symbol via prefix
  // matching.
  std::string finalMangled = node->resolvedMangledName;
  llvm::Function *f = module->getFunction(finalMangled);

  if (!f) {
    std::cerr << "[Utopia Fatal] CodeGen Error: Undefined function or "
                 "unresolved overload '"
              << finalMangled << "'.\n";
    exit(1);
  }

  // Discard 'this' if the resolved function is static (doesn't expect it).
  // Burn the pointer. It's a static call routed through an instance.
  if (thisArg && f->arg_size() == node->arguments.size()) {
    thisArg = nullptr;
  }

  /*
   * We invoked a pseudo-static method on a primitive type (like int.max()).
   * VariableNode("int") evaluates to a null pointer, so thisArg is empty.
   * But Sema registers ALL extension methods as instance methods, meaning LLVM
   * expects a 'this' parameter. Feed it a zeroed artifact to keep CreateCall
   * from panicking.
   */
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
    // Fallback: assume all parameters are pointers, not references (old
    // behavior, but shouldn't happen)
    for (size_t j = 0; j < node->arguments.size() + argOffset; ++j) {
      paramTypes.push_back(TypeInfo{"int", 0, false}); // dummy
    }
  }

  for (size_t i = 0; i < node->arguments.size(); ++i) {
    size_t paramIdx = i + argOffset;
    bool isRefParam = (paramIdx < paramTypes.size())
                          ? paramTypes[paramIdx].isReference
                          : false;
    bool isPointerParam = (paramIdx < paramTypes.size())
                              ? (paramTypes[paramIdx].ptrDepth > 0)
                              : false;

    // Decide on the evaluation method:
    // - If the parameter is a reference (isReference == true) -> LValue mode
    // (address)
    // - If the parameter is a normal pointer (ptrDepth > 0 && !isReference) ->
    // value mode (load the pointer)
    // - If the parameter is a value type (ptrDepth == 0) -> normal value mode
    bool expectsLValue = isRefParam;
    bool oldCtx = isLValueContext;
    isLValueContext = expectsLValue;
    currentLValue = nullptr;
    node->arguments[i]->accept(this);
    isLValueContext = oldCtx;

    llvm::Value *argVal = currentVal;

    if (expectsLValue) {
      // An address is expected: use currentLValue if available, otherwise use
      // currentVal (e.g., &expr)
      if (currentLValue) {
        argVal = currentLValue;
      } else {
        // If there is no LValue, we assume that currentVal is already the
        // address (e.g., the result of AddressOfNode).
        argVal = currentVal;
      }
    } else {
      // A value is expected: load if we have LValue and it is not a struct
      if (currentLValue) {
        if (structTypes.count(currentType.base) && currentType.ptrDepth == 0 &&
            !currentType.isReference) {
          // For structs passed by value, we pass the address (it will be copied
          // to the temp below)
          argVal = currentLValue;
        } else {
          argVal = builder->CreateLoad(getLLVMType(currentType), currentLValue);
        }
      }
      // If we already have currentVal (because accept loaded the value), we use
      // it directly
    }

    // Special treatment for structs passed by value (create a temporary copy)
    if (!expectsLValue && currentType.ptrDepth == 0 &&
        !currentType.isReference && structTypes.count(currentType.base)) {
      // Create a temporary copy on the stack
      llvm::AllocaInst *temp =
          builder->CreateAlloca(getLLVMType(currentType), nullptr, "arg.temp");
      TypeInfo targetType = currentType;
      targetType.isRValueRef = false;
      targetType.isReference = false;
      emitCopyOrStore(temp, argVal, targetType, currentType);
      // Pass the copy address (the function expects a pointer to a struct)
      argVal = temp;
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

  llvm::PassBuilder pb;
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

} // namespace utopia