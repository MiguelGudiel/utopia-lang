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
  valueScopes.pop_back();
  typeScopes.pop_back();
}

// Fast reverse lookup. The deepest scope intercepts the resolution.
// Shadows outer variables flawlessly.
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
  while (!temp.empty() && temp.back() == '*') {
    t.ptrDepth++;
    temp.pop_back();
  }
  t.base = temp;
  return t;
}

llvm::Type *CodeGen::getLLVMType(const TypeInfo &type) {
  if (type.ptrDepth > 0)
    return llvm::PointerType::get(*context, 0);
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

// --------------------------------------------------------------------------
// Visitor Implementations (Double Dispatch Payload)
// --------------------------------------------------------------------------

void CodeGen::visit(ProgramNode *node) {
  enterScope();

  // Forward Declaration Pre-pass
  for (auto &func : node->functions) {
    functionTypes[func->name] = parseTypeString(func->returnType);
  }

  for (auto &func : node->functions)
    func->accept(this);
  exitScope();
}

void CodeGen::visit(FunctionNode *node) {
  enterScope(); // Function argument scope

  TypeInfo retType = parseTypeString(node->returnType);
  currentReturnType = retType;

  std::vector<llvm::Type *> argTypes;
  for (auto &arg : node->args)
    argTypes.push_back(getLLVMType(parseTypeString(arg.first)));

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

  unsigned idx = 0;
  for (auto &arg : func->args()) {
    std::string argName = node->args[idx].second;
    TypeInfo argType = parseTypeString(node->args[idx].first);
    llvm::AllocaInst *alloca =
        builder->CreateAlloca(getLLVMType(argType), nullptr, argName);
    builder->CreateStore(&arg, alloca);

    valueScopes.back()[argName] = alloca;
    typeScopes.back()[argName] = argType;
    idx++;
  }

  for (const auto &stmt : node->body) {
    stmt->accept(this);
    if (builder->GetInsertBlock()->getTerminator())
      break;
  }

  // Prevent crash for LLVM (Implicit Return)
  if (!builder->GetInsertBlock()->getTerminator()) {
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
  if (node->returnValue) {
    node->returnValue->accept(this);
    builder->CreateRet(castValue(currentVal, currentType, currentReturnType));
  } else {
    builder->CreateRetVoid();
  }
}

void CodeGen::visit(AssignNode *node) {
  // We evaluate the expression on the right-hand side (RValue)
  node->value->accept(this);
  llvm::Value *val = currentVal;
  TypeInfo valType = currentType;

  // We evaluate the expression on the left side (LValue)
  // We explicitly activate LValue mode so that VariableNode and DerefNode
  // return memory addresses, not loaded values.
  isLValueContext = true;
  node->target->accept(this);
  isLValueContext = false;

  llvm::Value *destAddr = currentLValue;
  TypeInfo targetType = currentType;

  if (!destAddr) {
    std::cerr << "Fatal CodeGen Error: Assign target is not an LValue.\n";
    exit(1);
  }

  builder->CreateStore(castValue(val, valType, targetType), destAddr);
}

void CodeGen::visit(VarDeclNode *node) {
  TypeInfo declType = parseTypeString(node->typeName);
  node->initializer->accept(this);

  llvm::AllocaInst *alloca =
      builder->CreateAlloca(getLLVMType(declType), nullptr, node->name);
  builder->CreateStore(castValue(currentVal, currentType, declType), alloca);

  // Bind to current local scope
  valueScopes.back()[node->name] = alloca;
  typeScopes.back()[node->name] = declType;
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

  // Process THEN block
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

  // Process ELSE block
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
    delete elseBB; // Purge unused block
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
  // Branching: If True, jump to body. If False, jump to merge (end)
  builder->CreateCondBr(condV, bodyBB, mergeBB);

  continueTargets.push_back(condBB); // Continue re-evaluating the condition
  breakTargets.push_back(mergeBB);   // Break escapes at the end

  // Body
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
    // If not condition (ej: for(;;))
    builder->CreateBr(bodyBB);
  }

  // A "continue" in a "For" loop does NOT go to the condition, it goes to the
  // increment phase.
  continueTargets.push_back(incBB);
  breakTargets.push_back(mergeBB);

  // Body
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

  if (!builder->GetInsertBlock()->getTerminator()) {
    builder->CreateBr(incBB);
  }

  // Update (Increment)
  func->insert(func->end(), incBB);
  builder->SetInsertPoint(incBB);
  if (node->update)
    node->update->accept(this);
  builder->CreateBr(condBB); // Return the condition

  // End for
  func->insert(func->end(), mergeBB);
  builder->SetInsertPoint(mergeBB);

  exitScope();
}

// Fast branch generation. The block terminator check in BlockNode
// natively purges dead code following these instructions.
void CodeGen::visit(BreakNode *node) {
  if (!breakTargets.empty()) {
    builder->CreateBr(breakTargets.back());
  }
}

void CodeGen::visit(ContinueNode *node) {
  if (!continueTargets.empty()) {
    builder->CreateBr(continueTargets.back());
  }
}

void CodeGen::visit(DeleteNode *node) {
  node->pointerExpr->accept(this);
  builder->CreateCall(getFreePrototype(), {currentVal});
}

// --------------------------------------------------------------------------
// Expression Visitors
// --------------------------------------------------------------------------

void CodeGen::visit(VariableNode *node) {
  auto alloca = lookupValue(node->name);
  auto type = lookupType(node->name);

  if (isLValueContext) {
    currentLValue = alloca;
  } else {
    currentVal = builder->CreateLoad(getLLVMType(type), alloca, node->name);
  }
  currentType = type;
}

void CodeGen::visit(BinaryOpNode *node) {
  node->left->accept(this);
  llvm::Value *L = currentVal;
  TypeInfo LT = currentType;

  node->right->accept(this);
  llvm::Value *R = currentVal;
  TypeInfo RT = currentType;

  bool isF = LT.base == "float" || RT.base == "float";
  if (isF && (LT.base == "int" || RT.base == "int")) {
    if (LT.base == "int")
      L = builder->CreateSIToFP(L, llvm::Type::getDoubleTy(*context));
    if (RT.base == "int")
      R = builder->CreateSIToFP(R, llvm::Type::getDoubleTy(*context));
  }

  // --- Strict Boolean Evaluation ---
  // Branchless logical evaluation. Fast as hell.
  // Warning: Does not short-circuit. If you rely on 'if (p != null && *p ==
  // 1)', you will segfault. Fix your code architecture. Deal with it.
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

  // Relational Operators
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

  // Arithmetic
  if (node->op == "+")
    currentVal = isF ? builder->CreateFAdd(L, R) : builder->CreateAdd(L, R);
  else if (node->op == "-")
    currentVal = isF ? builder->CreateFSub(L, R) : builder->CreateSub(L, R);
  else if (node->op == "*")
    currentVal = isF ? builder->CreateFMul(L, R) : builder->CreateMul(L, R);
  else if (node->op == "/")
    currentVal = isF ? builder->CreateFDiv(L, R) : builder->CreateSDiv(L, R);

  currentType = isF ? TypeInfo{"float"} : TypeInfo{"int"};
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
  // To dereference, we need to know WHICH direction the pointer points to.
  // Therefore, the operand of Deref (the pointer itself) always evaluates to
  // RValue.
  bool oldContext = isLValueContext;
  isLValueContext = false;
  node->operand->accept(this);
  isLValueContext = oldContext;

  currentType.ptrDepth--;

  if (isLValueContext) {
    // If we are being asked for an LValue (e.g., *ptr = 10), the value we just
    // extracted is the memory address where we must write.
    currentLValue = currentVal;
  } else {
    // If we are asked for an RValue (e.g., int x = *ptr), we load the value
    // from memory
    currentVal =
        builder->CreateLoad(getLLVMType(currentType), currentVal, "deref");
  }
}

void CodeGen::visit(NewNode *node) {
  TypeInfo baseType = parseTypeString(node->typeName);
  uint64_t size = (baseType.base == "float" || baseType.base == "int") ? 8 : 4;
  llvm::Value *sz = llvm::ConstantInt::get(*context, llvm::APInt(64, size));
  llvm::Value *mem = builder->CreateCall(getMallocPrototype(), {sz}, "mem");

  if (node->initializer) {
    node->initializer->accept(this);
    builder->CreateStore(castValue(currentVal, currentType, baseType), mem);
  }
  currentVal = mem;
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
    builder->CreateStore(castValue(currentVal, currentType, targetT), addr);
    return;
  }
  if (node->callee == "print") {
    std::string fmt = "";
    std::vector<llvm::Value *> args = {nullptr};
    for (auto &arg : node->arguments) {
      arg->accept(this);
      if (currentType.base == "int") {
        fmt += "%d";
        args.push_back(currentVal);
      } else if (currentType.base == "float") {
        fmt += "%f";
        args.push_back(currentVal);
      } else {
        fmt += "%s";
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

  llvm::Function *f = module->getFunction(node->callee);

  if (!f) {
    std::cerr << "[Utopia Fatal] CodeGen Error: Call to undefined function '"
              << node->callee << "'.\n"
              << "Make sure the function is defined before it is called.\n";
    exit(1);
  }

  std::vector<llvm::Value *> args;
  for (auto &a : node->arguments) {
    a->accept(this);
    args.push_back(currentVal);
  }
  currentVal = builder->CreateCall(f, args);

  if (functionTypes.count(node->callee)) {
    currentType = functionTypes[node->callee];
  } else {
    currentType = {"int"};
  }
}

void CodeGen::visit(NullAssertNode *node) {
  node->operand->accept(this);
  currentType.isNullable = false;
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