#include "utopia/CodeGen/Intrinsics.hpp"
#include "utopia/CodeGen/CodeGen.hpp"
#include "utopia/Common/Types.hpp"

namespace utopia {

/* Proxy implementations to access CodeGen's private members */

llvm::Type *Intrinsic::getLLVMType(CodeGen &cg, const Type *type) const {
  return cg.getLLVMType(type);
}

llvm::Constant *
Intrinsic::createTypeReflectionConstant(CodeGen &cg, const Type *t,
                                        llvm::StructType *structTy) const {
  return cg.createTypeReflectionConstant(t, structTy);
}

llvm::IRBuilder<> &Intrinsic::getBuilder(CodeGen &cg) const {
  return cg.builder;
}

llvm::Module &Intrinsic::getModule(CodeGen &cg) const { return cg.mod; }

/* Intrinsic Implementations */

class SizeofTypeIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    return evaluateConstant(cg, node);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    if (node->args.size() == 1 && node->args[0]->representedType) {
      llvm::Type *llTy = getLLVMType(cg, node->args[0]->representedType);
      uint64_t size = getModule(cg).getDataLayout().getTypeAllocSize(llTy);
      return getBuilder(cg).getInt64(size);
    }
    return getBuilder(cg).getInt64(0);
  }
};

class SizeofExprIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    return evaluateConstant(cg, node);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    if (node->args.size() == 1 && node->args[0]->exprType) {
      llvm::Type *llTy = getLLVMType(cg, node->args[0]->exprType);
      uint64_t size = getModule(cg).getDataLayout().getTypeAllocSize(llTy);
      return getBuilder(cg).getInt64(size);
    }
    return getBuilder(cg).getInt64(0);
  }
};

class TypeofTypeIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    llvm::Constant *constVal = evaluateConstant(cg, node);
    return constVal ? constVal
                    : llvm::UndefValue::get(getLLVMType(cg, node->exprType));
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    if (node->args.size() == 1 && node->args[0]->representedType) {
      return createTypeReflectionConstant(
          cg, node->args[0]->representedType,
          llvm::cast<llvm::StructType>(getLLVMType(cg, node->exprType)));
    }
    return nullptr;
  }
};

class TypeofExprIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    llvm::Constant *constVal = evaluateConstant(cg, node);
    return constVal ? constVal
                    : llvm::UndefValue::get(getLLVMType(cg, node->exprType));
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    if (node->args.size() == 1 && node->args[0]->exprType) {
      return createTypeReflectionConstant(
          cg, node->args[0]->exprType,
          llvm::cast<llvm::StructType>(getLLVMType(cg, node->exprType)));
    }
    return nullptr;
  }
};

/* Registry Implementation */

namespace {

/* Helper: materialize an expression's value, preferring the l-value. */
struct ValueArg {
  llvm::Value *value = nullptr;
  bool isLValue = false;
};

ValueArg materializeArg(CodeGen &cg, const ExprNode *arg) {
  ValueArg out;
  out.value = cg.getLValue(arg);
  if (out.value) {
    out.isLValue = true;
    return out;
  }
  llvm::Value *val = cg.dispatch(arg);
  if (!val)
    return out;
  /* A temporary created by the dispatch (e.g. a call result) is already an
   * l-value; otherwise fall back to the plain value. */
  out.value = val;
  return out;
}

} // namespace

/* Future.value(T value): creates a completed future holding a copy/move of
 * the value. */
class FutureValueIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    if (node->args.size() != 1 || !node->resolvedFunc)
      return nullptr;

    const Type *valueType = node->args[0]->exprType;
    if (!valueType)
      return nullptr;

    llvm::Value *state = cg.createFutureState(valueType);

    ValueArg arg = materializeArg(cg, node->args[0]);
    if (arg.value) {
      cg.writeFutureValueInto(state, arg.value, valueType, arg.isLValue);
    }
    cg.emitRuntimeCall("utopia_future_complete", getBuilder(cg).getVoidTy(),
                       {state});

    llvm::Value *obj = cg.createFutureObject(
        node->resolvedFunc->returnType, state);
    return cg.materializeFutureValue(node->resolvedFunc->returnType, obj);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    return nullptr;
  }
};

/* Future.then(cb): registers the callback to run when the future completes
 * and returns a Future<void> that completes after the callback ran. */
class FutureThenIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    auto *ma = llvm::dyn_cast<MemberAccessNode>(node->target);
    if (!ma || node->args.size() != 1 || !node->resolvedFunc)
      return nullptr;

    const Type *valueTy = nullptr;
    if (!CodeGen::unwrapFutureType(ma->object->exprType, &valueTy))
      return nullptr;

    /* The callback may return void or a Future<void> (async lambda), and
     * may or may not take the value as a parameter. */
    bool asyncCb = false;
    bool cbTakesValue = true;
    const Type *cbType = node->args[0]->exprType;
    if (cbType) {
      const Type *u = cbType->getUnqualifiedType();
      if (u->isPointerType()) {
        const Type *pointee = static_cast<const PointerType *>(u)
                                  ->getPointeeType()
                                  ->getUnqualifiedType();
        if (auto *fnTy = llvm::dyn_cast<FunctionType>(pointee)) {
          const Type *inner = nullptr;
          asyncCb = CodeGen::unwrapFutureType(fnTy->getReturnType(), &inner);
          cbTakesValue = !fnTy->getParamTypes().empty();
        }
      }
    }

    llvm::Value *cb = cg.dispatch(node->args[0]);
    if (!cb)
      return nullptr;

    llvm::Value *futObj = cg.getFutureObjectPointer(ma->object);
    if (!futObj)
      return nullptr;
    llvm::Value *state = cg.getFutureState(futObj, ma->object->exprType);
    if (!state)
      return nullptr;

    llvm::Value *resultState = cg.createFutureState(nullptr);
    llvm::Function *thunk =
        cg.getOrCreateThenThunk(valueTy, asyncCb, cbTakesValue);
    cg.emitRuntimeCall("utopia_future_then_cb", getBuilder(cg).getVoidTy(),
                       {state, cb, thunk, resultState});

    const Type *resultTy = node->resolvedFunc->returnType;
    llvm::Value *obj = cg.createFutureObject(resultTy, resultState);
    return cg.materializeFutureValue(resultTy, obj);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    return nullptr;
  }
};

/* Future.runOnThread(fn): runs fn on a worker thread and completes the
 * future with its return value. */
class FutureRunOnThreadIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    if (node->args.size() != 1 || !node->resolvedFunc)
      return nullptr;

    const Type *fnType = node->args[0]->exprType;
    if (!fnType)
      return nullptr;
    const Type *u = fnType->getUnqualifiedType();
    const FunctionType *fTy = nullptr;
    if (u->isPointerType()) {
      const Type *pointee =
          static_cast<const PointerType *>(u)->getPointeeType()
              ->getUnqualifiedType();
      fTy = llvm::dyn_cast<FunctionType>(pointee);
    }
    if (!fTy)
      return nullptr;

    /* An async lambda returns a Future<X>: the worker runs it with its own
     * event loop and the outer future completes with X. */
    const Type *innerTy = nullptr;
    bool isAsyncFn = CodeGen::unwrapFutureType(fTy->getReturnType(),
                                               &innerTy);
    const Type *valueTy = isAsyncFn ? innerTy : fTy->getReturnType();

    llvm::Value *fn = cg.dispatch(node->args[0]);
    if (!fn)
      return nullptr;

    llvm::Value *state = cg.createFutureState(valueTy);
    llvm::Function *thunk = isAsyncFn
                                ? cg.getOrCreateAsyncThreadThunk(valueTy)
                                : cg.getOrCreateThreadThunk(valueTy);
    cg.emitRuntimeCall("utopia_thread_spawn", getBuilder(cg).getVoidTy(),
                       {state, fn, thunk});

    const Type *resultTy = node->resolvedFunc->returnType;
    llvm::Value *obj = cg.createFutureObject(resultTy, state);
    return cg.materializeFutureValue(resultTy, obj);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    return nullptr;
  }
};

/* Future.sync(fn): runs fn immediately and completes the future with its
 * result. */
class FutureSyncIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    if (node->args.size() != 1 || !node->resolvedFunc)
      return nullptr;

    const Type *fnType = node->args[0]->exprType;
    if (!fnType)
      return nullptr;
    const Type *u = fnType->getUnqualifiedType();
    const FunctionType *fTy = nullptr;
    if (u->isPointerType()) {
      const Type *pointee =
          static_cast<const PointerType *>(u)->getPointeeType()
              ->getUnqualifiedType();
      fTy = llvm::dyn_cast<FunctionType>(pointee);
    }
    if (!fTy)
      return nullptr;
    const Type *valueTy = fTy->getReturnType();

    llvm::Value *fn = cg.dispatch(node->args[0]);
    if (!fn)
      return nullptr;

    llvm::Value *state = cg.createFutureState(valueTy);

    bool isVoidValue = !valueTy || valueTy->isVoid();
    llvm::Value *retVal = nullptr;
    if (!isVoidValue) {
      llvm::Type *retTy = getLLVMType(cg, valueTy);
      llvm::FunctionType *fnTy = llvm::FunctionType::get(retTy, {}, false);
      llvm::Value *callee = getBuilder(cg).CreateBitCast(
          fn, llvm::PointerType::getUnqual(fnTy));
      retVal = getBuilder(cg).CreateCall(fnTy, callee, {});
      cg.writeFutureValueInto(state, retVal, valueTy, false);
    } else {
      llvm::FunctionType *fnTy =
          llvm::FunctionType::get(getBuilder(cg).getVoidTy(), {}, false);
      llvm::Value *callee = getBuilder(cg).CreateBitCast(
          fn, llvm::PointerType::getUnqual(fnTy));
      getBuilder(cg).CreateCall(fnTy, callee, {});
    }

    cg.emitRuntimeCall("utopia_future_complete", getBuilder(cg).getVoidTy(),
                       {state});

    const Type *resultTy = node->resolvedFunc->returnType;
    llvm::Value *obj = cg.createFutureObject(resultTy, state);
    return cg.materializeFutureValue(resultTy, obj);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    return nullptr;
  }
};

/* Future._delay(ms): a Future<void> that completes after 'ms'
 * milliseconds. */
class FutureDelayIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    if (node->args.size() != 1 || !node->resolvedFunc)
      return nullptr;

    llvm::Value *ms = cg.dispatch(node->args[0]);
    if (!ms)
      return nullptr;

    llvm::Value *delayState = cg.emitRuntimeCall(
        "utopia_future_delay", getBuilder(cg).getPtrTy(), {ms});

    const Type *resultTy = node->resolvedFunc->returnType;
    llvm::Value *obj = cg.createFutureObject(resultTy, delayState);
    return cg.materializeFutureValue(resultTy, obj);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    return nullptr;
  }
};

IntrinsicRegistry::IntrinsicRegistry() {
  registerIntrinsic("sizeof_type", std::make_unique<SizeofTypeIntrinsic>());
  registerIntrinsic("sizeof_expr", std::make_unique<SizeofExprIntrinsic>());
  registerIntrinsic("typeof_type", std::make_unique<TypeofTypeIntrinsic>());
  registerIntrinsic("typeof_expr", std::make_unique<TypeofExprIntrinsic>());
  registerIntrinsic("future_value", std::make_unique<FutureValueIntrinsic>());
  registerIntrinsic("future_then", std::make_unique<FutureThenIntrinsic>());
  registerIntrinsic("future_runOnThread",
                    std::make_unique<FutureRunOnThreadIntrinsic>());
  registerIntrinsic("future_sync", std::make_unique<FutureSyncIntrinsic>());
  registerIntrinsic("future_delay", std::make_unique<FutureDelayIntrinsic>());
}

const IntrinsicRegistry &IntrinsicRegistry::instance() {
  static IntrinsicRegistry registry;
  return registry;
}

void IntrinsicRegistry::registerIntrinsic(
    std::string_view name, std::unique_ptr<Intrinsic> intrinsic) {
  registry[name] = std::move(intrinsic);
}

const Intrinsic *IntrinsicRegistry::get(std::string_view name) const {
  auto it = registry.find(name);
  if (it != registry.end()) {
    return it->second.get();
  }
  return nullptr;
}

} // namespace utopia