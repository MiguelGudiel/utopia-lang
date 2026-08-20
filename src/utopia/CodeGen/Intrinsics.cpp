#include "utopia/CodeGen/Intrinsics.hpp"
#include "utopia/CodeGen/CodeGen.hpp"
#include "utopia/CodeGen/SimdIntrinsics.hpp"
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

void Intrinsic::emitCleanupCall(CodeGen &cg, llvm::Value *ptr,
                                const FunctionDeclNode *dtor) const {
  cg.emitCleanupCall(ptr, dtor);
}

llvm::AllocaInst *Intrinsic::createEntryBlockAlloca(
    CodeGen &cg, llvm::Type *type, const std::string &varName) const {
  return cg.createEntryBlockAlloca(type, varName);
}

void Intrinsic::reportError(CodeGen &cg, int line, int col, int len,
                            const std::string &message) const {
  cg.reportError(line, col, len, message);
}


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

class AlignofTypeIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    return evaluateConstant(cg, node);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    if (node->args.size() == 1 && node->args[0]->representedType) {
      const Type *ty = node->args[0]->representedType;
      llvm::Type *llTy = getLLVMType(cg, ty);
      uint64_t align =
          getModule(cg).getDataLayout().getABITypeAlign(llTy).value();

      /* '@align(N)' raises the alignment of the record; it is applied at
       * allocation sites rather than on the LLVM type, so honor it here. */
      const Type *unqual = ty->getUnqualifiedType();
      if (unqual->getKind() == TypeKind::Class ||
          unqual->getKind() == TypeKind::Struct ||
          unqual->getKind() == TypeKind::Union) {
        const DeclNode *decl =
            static_cast<const RecordType *>(unqual)->getDeclaration();
        if (decl && decl->alignment > align)
          align = decl->alignment;
      }
      return getBuilder(cg).getInt64(align);
    }
    return getBuilder(cg).getInt64(0);
  }
};

class AlignofExprIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    return evaluateConstant(cg, node);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    if (node->args.size() == 1 && node->args[0]->exprType) {
      const Type *ty = node->args[0]->exprType;
      llvm::Type *llTy = getLLVMType(cg, ty);
      uint64_t align =
          getModule(cg).getDataLayout().getABITypeAlign(llTy).value();

      const Type *unqual = ty->getUnqualifiedType();
      if (unqual->getKind() == TypeKind::Class ||
          unqual->getKind() == TypeKind::Struct ||
          unqual->getKind() == TypeKind::Union) {
        const DeclNode *decl =
            static_cast<const RecordType *>(unqual)->getDeclaration();
        if (decl && decl->alignment > align)
          align = decl->alignment;
      }
      return getBuilder(cg).getInt64(align);
    }
    return getBuilder(cg).getInt64(0);
  }
};

/* Memory.alloc(size, align): aligned heap allocation that returns a
 * RawMemory { uint8* ptr } aggregate. The size is rounded up to a multiple
 * of the alignment so the call satisfies C11 'aligned_alloc' requirements,
 * and a null result terminates the program (matching the 'new' OOM policy).
 * With constant size/alignment the rounding folds away at compile time. */
class MemoryAllocIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    if (node->args.size() != 2)
      return nullptr;

    llvm::IRBuilder<> &b = getBuilder(cg);
    llvm::Value *size = cg.dispatch(node->args[0]);
    llvm::Value *align = cg.dispatch(node->args[1]);
    if (!size || !align)
      return nullptr;

    /* Integer literals may arrive as i32; the allocator takes i64. */
    if (size->getType() != b.getInt64Ty())
      size = b.CreateIntCast(size, b.getInt64Ty(), false);
    if (align->getType() != b.getInt64Ty())
      align = b.CreateIntCast(align, b.getInt64Ty(), false);

    /* roundUp(size, align): (size + align - 1) & ~(align - 1) */
    llvm::Value *one = b.getInt64(1);
    llvm::Value *alignMask = b.CreateSub(align, one);
    llvm::Value *rounded =
        b.CreateAnd(b.CreateAdd(size, alignMask),
                    b.CreateNot(alignMask), "alloc.round");

    llvm::FunctionType *alignedAllocTy = llvm::FunctionType::get(
        b.getPtrTy(), {b.getInt64Ty(), b.getInt64Ty()}, false);
    llvm::Function *alignedAllocFunc =
        getModule(cg).getFunction("aligned_alloc");
    if (!alignedAllocFunc) {
      alignedAllocFunc = llvm::Function::Create(
          alignedAllocTy, llvm::Function::ExternalLinkage, "aligned_alloc",
          getModule(cg));
    }

    llvm::Value *mem = b.CreateCall(alignedAllocFunc, {align, rounded},
                                    "alloc.raw");

    /* Out of memory: no exceptions in Utopia, so terminate. */
    llvm::Function *theFunction = b.GetInsertBlock()->getParent();
    llvm::BasicBlock *oomBB =
        llvm::BasicBlock::Create(b.getContext(), "alloc.oom", theFunction);
    llvm::BasicBlock *contBB =
        llvm::BasicBlock::Create(b.getContext(), "alloc.cont");
    b.CreateCondBr(b.CreateIsNull(mem, "alloc.null"), oomBB, contBB);

    b.SetInsertPoint(oomBB);
    llvm::Function *abortFunc = getModule(cg).getFunction("abort");
    if (!abortFunc) {
      abortFunc = llvm::Function::Create(
          llvm::FunctionType::get(b.getVoidTy(), false),
          llvm::Function::ExternalLinkage, "abort", getModule(cg));
      abortFunc->setDoesNotReturn();
    }
    b.CreateCall(abortFunc, {});
    b.CreateUnreachable();

    theFunction->insert(theFunction->end(), contBB);
    b.SetInsertPoint(contBB);

    llvm::StructType *rawMemTy =
        llvm::cast<llvm::StructType>(getLLVMType(cg, node->exprType));
    llvm::Value *rawMem = llvm::PoisonValue::get(rawMemTy);
    return b.CreateInsertValue(rawMem, mem, 0, "alloc.rawmem");
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    return nullptr;
  }
};

/* Memory.free(mem): releases a block returned by Memory.alloc(). */
class MemoryFreeIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    if (node->args.size() != 1)
      return nullptr;

    llvm::IRBuilder<> &b = getBuilder(cg);
    llvm::Value *rawMem = cg.dispatch(node->args[0]);
    if (!rawMem)
      return nullptr;

    llvm::Value *ptr = b.CreateExtractValue(rawMem, 0, "free.ptr");

    llvm::Function *freeFunc = getModule(cg).getFunction("free");
    if (!freeFunc) {
      freeFunc = llvm::Function::Create(
          llvm::FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false),
          llvm::Function::ExternalLinkage, "free", getModule(cg));
    }
    b.CreateCall(freeFunc, {ptr});
    return nullptr;
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    return nullptr;
  }
};

/* Memory.destruct(ptr): runs the destructor of the pointee type, if it has
 * one. The memory itself is not released. */
class MemoryDestructIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    if (node->args.size() != 1)
      return nullptr;

    llvm::Value *ptr = cg.dispatch(node->args[0]);
    if (!ptr)
      return nullptr;

    const Type *argTy = node->args[0]->exprType;
    if (!argTy)
      return nullptr;
    const Type *unqual = argTy->getUnqualifiedType();
    if (!unqual->isPointerType())
      return nullptr;
    const Type *pointee =
        static_cast<const PointerType *>(unqual)->getPointeeType()
            ->getUnqualifiedType();

    if (pointee->getKind() != TypeKind::Class &&
        pointee->getKind() != TypeKind::Struct &&
        pointee->getKind() != TypeKind::Union)
      return nullptr;

    const DeclNode *decl =
        static_cast<const RecordType *>(pointee)->getDeclaration();
    if (!decl)
      return nullptr;

    const FunctionDeclNode *dtor = nullptr;
    if (decl->kind == NodeKind::ClassDecl)
      dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
    else if (decl->kind == NodeKind::StructDecl)
      dtor = static_cast<const StructDeclNode *>(decl)->destructor;
    else if (decl->kind == NodeKind::UnionDecl)
      dtor = static_cast<const UnionDeclNode *>(decl)->destructor;

    if (dtor && !dtor->isImplicit)
      emitCleanupCall(cg, ptr, dtor);
    return nullptr;
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    return nullptr;
  }
};

/* 'Memory.isConst(ptr)': whether the pointer addresses a canonical const
 * object (static read-only storage). Lowers to the runtime registry walker
 * '__utopia_is_const_ptr', which CodeGen emits once per module. */
class MemoryIsConstIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    if (node->args.size() != 1)
      return nullptr;

    llvm::Value *ptr = cg.dispatch(node->args[0]);
    if (!ptr)
      return nullptr;
    if (ptr->getType() != getBuilder(cg).getPtrTy())
      ptr = getBuilder(cg).CreateBitCast(ptr, getBuilder(cg).getPtrTy());

    llvm::Module &mod = getModule(cg);
    llvm::FunctionType *fty =
        llvm::FunctionType::get(getBuilder(cg).getInt1Ty(),
                                {getBuilder(cg).getPtrTy()}, false);
    llvm::Function *walker = mod.getFunction("__utopia_is_const_ptr");
    if (!walker) {
      walker = llvm::Function::Create(
          fty, llvm::GlobalValue::LinkOnceODRLinkage,
          "__utopia_is_const_ptr", mod);
    }
    return getBuilder(cg).CreateCall(walker, {ptr});
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    /* Not a compile-time constant in general. */
    return nullptr;
  }
};

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

/* Future._delayUs(us): a Future<void> that completes after 'us'
 * microseconds (used by the Duration overload of Future.delayed). */
class FutureDelayUsIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    if (node->args.size() != 1 || !node->resolvedFunc)
      return nullptr;

    llvm::Value *us = cg.dispatch(node->args[0]);
    if (!us)
      return nullptr;

    llvm::Value *delayState = cg.emitRuntimeCall(
        "utopia_future_delay_us", getBuilder(cg).getPtrTy(), {us});

    const Type *resultTy = node->resolvedFunc->returnType;
    llvm::Value *obj = cg.createFutureObject(resultTy, delayState);
    return cg.materializeFutureValue(resultTy, obj);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    return nullptr;
  }
};

/* hash<T>(value): deterministic content hash used by HashMap/Map (like
 * Dart's Object.hashCode). String hashes its buffer bytes; 'const uint8*'
 * (string literal) hashes the null-terminated content; every other type
 * hashes its raw in-memory bytes (sizeof(T)). The value is copied into a
 * temporary and hashed through a byte pointer, so no type-specific
 * lowering is needed. FNV-1a 64-bit. */
class HashExprIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    if (node->args.size() != 1)
      return nullptr;

    llvm::IRBuilder<> &b = getBuilder(cg);
    llvm::Module &mod = getModule(cg);

    const Type *argTy = node->args[0]->exprType;
    if (!argTy)
      return nullptr;
    const Type *unqual = argTy->getUnqualifiedType();
    if (unqual->isReferenceType())
      unqual = static_cast<const ReferenceType *>(unqual)
                   ->getPointeeType()
                   ->getUnqualifiedType();
    else if (unqual->getKind() == TypeKind::RValueReference)
      unqual = static_cast<const RValueReferenceType *>(unqual)
                   ->getPointeeType()
                   ->getUnqualifiedType();

    llvm::Value *val = cg.dispatch(node->args[0]);
    if (!val)
      return nullptr;

    llvm::Value *dataPtr = nullptr;
    llvm::Value *len = nullptr;

    const Type *pointeeTy = nullptr;
    if (unqual->isPointerType()) {
      const Type *p = static_cast<const PointerType *>(unqual)
                          ->getPointeeType()
                          ->getUnqualifiedType();
      if (p->getKind() == TypeKind::Builtin &&
          static_cast<const BuiltinType *>(p)->getBuiltinKind() ==
              BuiltinKind::UInt8)
        pointeeTy = p;
    }

    bool isString = (unqual->getKind() == TypeKind::Class ||
                     unqual->getKind() == TypeKind::Struct) &&
                    static_cast<const RecordType *>(unqual)->getName() ==
                        "String";

    if (isString) {
      /* String { uint8* data; usize len; usize cap }: hash data[0..len). */
      llvm::Type *llTy = getLLVMType(cg, unqual);
      llvm::AllocaInst *tmp = createEntryBlockAlloca(cg, llTy, "hash.str.tmp");
      b.CreateStore(val, tmp);
      llvm::Value *dataField = b.CreateStructGEP(llTy, tmp, 0);
      llvm::Value *lenField = b.CreateStructGEP(llTy, tmp, 1);
      dataPtr = b.CreateLoad(b.getPtrTy(), dataField, "hash.str.data");
      len = b.CreateLoad(b.getInt64Ty(), lenField, "hash.str.len");
    } else if (pointeeTy) {
      /* C string: hash until the terminating null. */
      llvm::AllocaInst *tmp = createEntryBlockAlloca(cg, b.getPtrTy(),
                                                        "hash.cstr.tmp");
      b.CreateStore(val, tmp);
      dataPtr = b.CreateLoad(b.getPtrTy(), tmp, "hash.cstr.ptr");
      llvm::Function *fn = b.GetInsertBlock()->getParent();
      llvm::BasicBlock *entryBB = b.GetInsertBlock();
      llvm::BasicBlock *lenLoop =
          llvm::BasicBlock::Create(mod.getContext(), "hash.lenloop", fn);
      llvm::BasicBlock *lenExit =
          llvm::BasicBlock::Create(mod.getContext(), "hash.lenexit", fn);
      llvm::Value *i0 = b.getInt64(0);
      b.CreateBr(lenLoop);
      b.SetInsertPoint(lenLoop);
      llvm::PHINode *phiI = b.CreatePHI(b.getInt64Ty(), 2, "i");
      phiI->addIncoming(i0, entryBB);
      llvm::Value *bytePtr = b.CreateInBoundsGEP(b.getInt8Ty(), dataPtr, phiI);
      llvm::Value *byte = b.CreateLoad(b.getInt8Ty(), bytePtr, "c");
      llvm::Value *nextI = b.CreateAdd(phiI, b.getInt64(1));
      llvm::Value *done = b.CreateICmpEQ(byte, b.getInt8(0));
      b.CreateCondBr(done, lenExit, lenLoop);
      phiI->addIncoming(nextI, lenLoop);
      b.SetInsertPoint(lenExit);
      len = phiI;
    } else {
      /* Raw bytes: hash sizeof(T) bytes of the value's representation. */
      llvm::Type *llTy = getLLVMType(cg, unqual);
      llvm::AllocaInst *tmp = createEntryBlockAlloca(cg, llTy, "hash.tmp");
      b.CreateStore(val, tmp);
      dataPtr = b.CreateBitCast(tmp, b.getPtrTy());
      uint64_t size = mod.getDataLayout().getTypeAllocSize(llTy);
      len = b.getInt64(size);
    }

    /* FNV-1a 64: h = offset_basis; for each byte: h = (h ^ byte) * prime. */
    constexpr uint64_t fnvOffsetBasis = 14695981039346656037ull;
    constexpr uint64_t fnvPrime = 1099511628211ull;

    llvm::Function *fn = b.GetInsertBlock()->getParent();
    llvm::BasicBlock *entryBB = b.GetInsertBlock();
    llvm::BasicBlock *loopBB =
        llvm::BasicBlock::Create(mod.getContext(), "hash.loop", fn);
    llvm::BasicBlock *exitBB =
        llvm::BasicBlock::Create(mod.getContext(), "hash.exit", fn);

    llvm::Value *h0 = b.getInt64(fnvOffsetBasis);
    llvm::Value *i0 = b.getInt64(0);
    b.CreateBr(loopBB);
    b.SetInsertPoint(loopBB);
    llvm::PHINode *phiH = b.CreatePHI(b.getInt64Ty(), 2, "h");
    phiH->addIncoming(h0, entryBB);
    llvm::PHINode *phiI = b.CreatePHI(b.getInt64Ty(), 2, "i");
    phiI->addIncoming(i0, entryBB);
    llvm::Value *bytePtr = b.CreateInBoundsGEP(b.getInt8Ty(), dataPtr, phiI);
    llvm::Value *byte = b.CreateLoad(b.getInt8Ty(), bytePtr, "hash.byte");
    llvm::Value *byte64 = b.CreateZExt(byte, b.getInt64Ty());
    llvm::Value *mixed = b.CreateXor(phiH, byte64);
    llvm::Value *nextH = b.CreateMul(mixed, b.getInt64(fnvPrime));
    llvm::Value *nextI = b.CreateAdd(phiI, b.getInt64(1));
    llvm::Value *more = b.CreateICmpULT(nextI, len);
    b.CreateCondBr(more, loopBB, exitBB);
    phiH->addIncoming(nextH, loopBB);
    phiI->addIncoming(nextI, loopBB);
    b.SetInsertPoint(exitBB);
    llvm::PHINode *result = b.CreatePHI(b.getInt64Ty(), 1, "hash");
    result->addIncoming(nextH, loopBB);
    return result;
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    return nullptr;
  }
};

IntrinsicRegistry::IntrinsicRegistry() {
  registerIntrinsic("sizeof_type", std::make_unique<SizeofTypeIntrinsic>());
  registerIntrinsic("sizeof_expr", std::make_unique<SizeofExprIntrinsic>());
  registerIntrinsic("alignof_type", std::make_unique<AlignofTypeIntrinsic>());
  registerIntrinsic("alignof_expr", std::make_unique<AlignofExprIntrinsic>());
  registerIntrinsic("typeof_type", std::make_unique<TypeofTypeIntrinsic>());
  registerIntrinsic("typeof_expr", std::make_unique<TypeofExprIntrinsic>());
  registerIntrinsic("memory_alloc", std::make_unique<MemoryAllocIntrinsic>());
  registerIntrinsic("memory_free", std::make_unique<MemoryFreeIntrinsic>());
  registerIntrinsic("memory_destruct",
                    std::make_unique<MemoryDestructIntrinsic>());
  registerIntrinsic("memory_is_const",
                    std::make_unique<MemoryIsConstIntrinsic>());
  registerIntrinsic("hash_expr", std::make_unique<HashExprIntrinsic>());
  registerIntrinsic("future_value", std::make_unique<FutureValueIntrinsic>());
  registerIntrinsic("future_then", std::make_unique<FutureThenIntrinsic>());
  registerIntrinsic("future_runOnThread",
                    std::make_unique<FutureRunOnThreadIntrinsic>());
  registerIntrinsic("future_sync", std::make_unique<FutureSyncIntrinsic>());
  registerIntrinsic("future_delay", std::make_unique<FutureDelayIntrinsic>());
  registerIntrinsic("future_delay_us",
                    std::make_unique<FutureDelayUsIntrinsic>());
  registerSimdIntrinsics(*this);
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