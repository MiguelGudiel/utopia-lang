#include "utopia/CodeGen/CodeGen.hpp"
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

CodeGen::CodeGen() {
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

llvm::AllocaInst *CodeGen::lookupValue(const std::string &name) {
  for (auto it = valueScopes.rbegin(); it != valueScopes.rend(); ++it) {
    if (it->count(name))
      return (*it)[name];
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
  /*
   * WTF? LLVM will SIGILL if we insert instructions after a terminator.
   * Evil basic block solution: bail out if the block is already sealed.
   */
  if (builder->GetInsertBlock()->getTerminator()) {
    return;
  }

  for (size_t i = valueScopes.size(); i > targetDepth; --i) {
    auto &currentScopeValues = valueScopes[i - 1];
    auto &currentScopeTypes = typeScopes[i - 1];

    for (auto const &[name, allocaInst] : currentScopeValues) {
      TypeInfo type = currentScopeTypes[name];

      if (type.ptrDepth == 0 && !type.isReference &&
          structTypes.count(type.base)) {
        std::string dtorName = type.base + "_~" + type.base;
        llvm::Function *dtorFunc = module->getFunction(dtorName);

        if (dtorFunc) {
          builder->CreateCall(dtorFunc, {allocaInst});
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
    for (auto &method : st->methods) {
      std::string mangledName = st->name + "_" + method->name;
      for (auto &arg : method->args) {
        TypeInfo t = arg.isThisAssign ? structMemberTypes[st->name][arg.name]
                                      : parseTypeString(arg.type);
        mangledName += "_" + getMangledType(t);
      }
      functionTypes[mangledName] = parseTypeString(method->returnType);
    }
  }

  for (auto &st : node->structs) {
    std::vector<llvm::Type *> body;
    int idx = 0;
    for (auto &f : st->fields) {
      TypeInfo t = parseTypeString(f.typeName);
      body.push_back(getLLVMType(t));
      structMemberIndices[st->name][f.name] = idx++;
      structMemberTypes[st->name][f.name] = t;
    }
    structTypes[st->name]->setBody(body);
  }

  for (auto &func : node->functions) {
    std::string mangledName = func->name;
    for (auto &arg : func->args)
      mangledName += "_" + getMangledType(parseTypeString(arg.type));
    functionTypes[mangledName] = parseTypeString(func->returnType);
  }

  for (auto &st : node->structs) {
    for (auto &method : st->methods) {
      std::string originalName = method->name;
      method->name = st->name + "_" + method->name;
      for (auto &arg : method->args) {
        TypeInfo t = arg.isThisAssign ? structMemberTypes[st->name][arg.name]
                                      : parseTypeString(arg.type);
        method->name += "_" + getMangledType(t);
      }
      method->accept(this);
      method->name = originalName;
    }
  }

  for (auto &func : node->functions) {
    std::string originalName = func->name;
    for (auto &arg : func->args)
      func->name += "_" + getMangledType(parseTypeString(arg.type));
    func->accept(this);
    func->name = originalName;
  }
  exitScope();
}

void CodeGen::visit(StructDeclNode *node) {}

void CodeGen::visit(MemberAccessNode *node) {
  bool oldContext = isLValueContext;
  isLValueContext = false;
  node->object->accept(this);
  isLValueContext = oldContext;

  llvm::Value *objPtr = currentVal;
  std::string objTypeName = currentType.base;
  int idx = structMemberIndices[objTypeName][node->field];
  TypeInfo fType = structMemberTypes[objTypeName][node->field];

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

  if (isLValueContext) {
    currentLValue = alloca;
  } else {
    currentVal = builder->CreateLoad(getLLVMType(type), alloca, "this.ptr");
  }
  currentType = type;
}

void CodeGen::visit(FunctionNode *node) {
  enterScope();

  TypeInfo retType = parseTypeString(node->returnType);
  currentReturnType = retType;

  std::vector<llvm::Type *> argTypes;
  if (node->isMethod || node->isConstructor || node->isDestructor) {
    argTypes.push_back(llvm::PointerType::get(*context, 0));
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

  if (node->inlineState == InlineState::Inline) {
    func->addFnAttr(llvm::Attribute::InlineHint);
  } else if (node->inlineState == InlineState::ForceInline) {
    func->addFnAttr(llvm::Attribute::AlwaysInline);
  }

  llvm::BasicBlock *block = llvm::BasicBlock::Create(*context, "entry", func);
  builder->SetInsertPoint(block);

  auto argIt = func->arg_begin();
  if (node->isMethod || node->isConstructor || node->isDestructor) {
    llvm::Argument *thisArg = &(*argIt);
    thisArg->setName("this");
    llvm::AllocaInst *thisAlloca = builder->CreateAlloca(
        llvm::PointerType::get(*context, 0), nullptr, "this.addr");
    builder->CreateStore(thisArg, thisAlloca);
    valueScopes.back()["this"] = thisAlloca;
    typeScopes.back()["this"] = {node->className, 1, false};
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

    valueScopes.back()[argName] = alloca;
    typeScopes.back()[argName] = argType;

    if (node->args[idx].isThisAssign) {
      llvm::Value *thisPtr = builder->CreateLoad(
          llvm::PointerType::get(*context, 0), valueScopes.back()["this"]);
      int fieldIdx = structMemberIndices[node->className][argName];
      llvm::Value *fieldAddr = builder->CreateStructGEP(
          structTypes[node->className], thisPtr, fieldIdx, "this." + argName);
      builder->CreateStore(arg, fieldAddr);
    }
  }

  if (node->isConstructor) {
    StructDeclNode *classAST = structASTs[node->className];
    if (classAST) {
      for (auto &field : classAST->fields) {
        if (field.initializer) {
          field.initializer->accept(this);
          llvm::Value *initVal = currentVal;
          TypeInfo initType = currentType;

          int fieldIdx = structMemberIndices[node->className][field.name];
          TypeInfo targetType = structMemberTypes[node->className][field.name];

          llvm::Value *thisPtr = builder->CreateLoad(
              llvm::PointerType::get(*context, 0), valueScopes.back()["this"]);

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

  exitScope();
}

void CodeGen::visit(ReturnNode *node) {
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
  TypeInfo declType = parseTypeString(node->typeName);

  if (node->arraySize) {
    node->arraySize->accept(this);
    llvm::Value *sizeVal = currentVal;
    llvm::Type *llvmType = getLLVMType(declType);

    llvm::AllocaInst *alloca =
        builder->CreateAlloca(llvmType, sizeVal, node->name);

    emitLifecycleLoop(alloca, sizeVal, node->typeName, false);

    declType.isArray = true;
    valueScopes.back()[node->name] = alloca;
    typeScopes.back()[node->name] = declType;
  } else {
    llvm::Type *llvmType = getLLVMType(declType);
    llvm::AllocaInst *alloca =
        builder->CreateAlloca(llvmType, nullptr, node->name);

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

    valueScopes.back()[node->name] = alloca;
    typeScopes.back()[node->name] = declType;
  }
}

void CodeGen::visit(IfNode *node) {
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
  enterScope();
  for (auto &stmt : node->statements) {
    stmt->accept(this);
    if (builder->GetInsertBlock()->getTerminator())
      break;
  }
  exitScope();
}

void CodeGen::visit(WhileNode *node) {
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
  }

  if (node->op == "&&") {
    currentVal = builder->CreateAnd(L, R, "and_strict");
    currentType = {"bool"};
    return;
  }
  if (node->op == "||") {
    currentVal = builder->CreateOr(L, R, "or_strict");
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

    std::string mangledSig = "";
    for (auto &arg : node->arguments) {
      arg->accept(this);
      callArgs.push_back(currentVal);
      mangledSig += "_" + getMangledType(currentType);
    }

    std::string ctorName = node->typeName + "_" + node->typeName + mangledSig;
    llvm::Function *ctor = module->getFunction(ctorName);
    if (ctor)
      builder->CreateCall(ctor, callArgs);

    currentVal = rawMem;
  }

  currentType = baseType;
  currentType.ptrDepth = 1;
}

void CodeGen::visit(CallNode *node) {
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

    // RVO Intercept: Inject directly into the caller's memory address
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
      mSig += "_" + getMangledType(currentType);
    }
    llvm::Function *ctor =
        module->getFunction(node->callee + "_" + node->callee + mSig);
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
    isLValueContext = false;
    node->object->accept(this);
    isLValueContext = oldCtx;
    thisArg = currentVal;
    baseName = currentType.base + "_" + node->callee;
  }

  llvm::Function *f = module->getFunction(baseName);
  std::string finalMangled = baseName;

  if (!f) {
    for (auto &func : module->getFunctionList()) {
      std::string fnName = func.getName().str();
      if (fnName.find(baseName + "_") == 0) {
        size_t expectedArgs = node->arguments.size() + (node->object ? 1 : 0);
        if (func.arg_size() == expectedArgs) {
          f = &func;
          finalMangled = fnName;
          break;
        }
      }
    }
  }

  if (!f) {
    std::cerr << "[Utopia Fatal] CodeGen Error: Undefined function '"
              << baseName << "'.\n";
    exit(1);
  }

  std::vector<llvm::Value *> args;
  int argOffset = 0;

  if (thisArg) {
    args.push_back(thisArg);
    argOffset = 1;
  }

  for (size_t i = 0; i < node->arguments.size(); ++i) {
    // Get param type safely. getArg() on raw functions is risky business.
    bool expectsRef =
        f->getFunctionType()->getParamType(i + argOffset)->isPointerTy();

    bool oldCtx = isLValueContext;
    isLValueContext = expectsRef;
    currentLValue = nullptr;
    node->arguments[i]->accept(this);
    isLValueContext = oldCtx;

    llvm::Value *argVal = currentVal;

    if (expectsRef && currentLValue) {
      argVal = currentLValue;
    } else if (!expectsRef && currentLValue) {
      if (structTypes.count(currentType.base) && currentType.ptrDepth == 0 &&
          !currentType.isReference) {
        argVal = currentLValue;
      } else {
        argVal = builder->CreateLoad(getLLVMType(currentType), currentLValue);
      }
    }

    /* Isolated temporal memory for pass-by-value structs */
    if (!expectsRef && currentType.ptrDepth == 0 && !currentType.isReference &&
        structTypes.count(currentType.base)) {
      llvm::AllocaInst *temp =
          builder->CreateAlloca(getLLVMType(currentType), nullptr, "arg.temp");

      TypeInfo targetType = currentType;
      targetType.isRValueRef = false;
      targetType.isReference = false;

      emitCopyOrStore(temp, argVal, targetType, currentType);

      /*
       * CORE DUMP PREVENTION:
       * Load raw bytes into SSA struct value before call.
       */
      argVal = builder->CreateLoad(getLLVMType(targetType), temp, "val.load");
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

void CodeGen::generate(ProgramNode *program) { program->accept(this); }

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