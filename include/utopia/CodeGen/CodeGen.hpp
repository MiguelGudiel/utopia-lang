#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/AST/ASTContext.hpp"
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/CodeGen/BackendContext.hpp"
#include "utopia/CodeGen/CodeGenContext.hpp"
#include "utopia/CodeGen/DebugInfoEmitter.hpp"
#include "utopia/CodeGen/TBAAManager.hpp"
#include "utopia/Common/Diagnostics.hpp"
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <memory>
#include <string>
#include <unordered_map>

namespace utopia {
class Intrinsic;

/* Bookkeeping for the async function currently being emitted. */
struct CoroutineInfo {
  llvm::Value *coroId = nullptr;
  llvm::AllocaInst *frameSlot = nullptr;    /* stores the coroutine frame ptr */
  llvm::Value *futureObjSlot = nullptr;     /* stores the returned Future obj */
  llvm::Value *futureStateSlot = nullptr;   /* stores the future state ptr */
  llvm::BasicBlock *suspendBlock = nullptr; /* shared coro.end(false) exit */
  llvm::BasicBlock *cleanupBlock = nullptr; /* shared destroy/free exit */
  const Type *valueType = nullptr;          /* declared (value) return type */
  const Type *futureType = nullptr;         /* effective Future<T> type */
  bool isCoroutine = false;                 /* true when the body has awaits */
  bool isMain = false;
};

class CodeGen : public ASTVisitor<CodeGen, llvm::Value *> {
  friend class Intrinsic;
  friend class DebugInfoEmitter;
  friend class TBAAManager;

public:
  CodeGen(BackendContext &bCtx, llvm::Module &llvmMod, DiagnosticsEngine &diags,
          bool emitDebugInfo, std::string filePath, ASTContext &astCtx,
          bool asyncEnabled = true);

  llvm::Value *dispatch(const ASTNode *node);

  llvm::Value *visit(const NamespaceDeclNode *node);
  llvm::Value *visit(const UsingNode *node);
  llvm::Value *visit(const NumberNode *node);
  llvm::Value *visit(const BoolNode *node);
  llvm::Value *visit(const CharNode *node);
  llvm::Value *visit(const RuneNode *node);
  llvm::Value *visit(const StringNode *node);
  llvm::Value *visit(const VariableNode *node);
  llvm::Value *visit(const UnaryOpNode *node);
  llvm::Value *visit(const BinaryOpNode *node);
  llvm::Value *visit(const TernaryOpNode *node);
  llvm::Value *visit(const VarDeclNode *node);
  llvm::Value *visit(const AssignNode *node);
  llvm::Value *visit(const BlockNode *node);
  llvm::Value *visit(const FunctionDeclNode *node);
  llvm::Value *visit(const FunctionCallNode *node);
  llvm::Value *visit(const IfNode *node);
  llvm::Value *visit(const ForNode *node);
  llvm::Value *visit(const ForInNode *node);
  llvm::Value *visit(const WhileNode *node);
  llvm::Value *visit(const SwitchNode *node);
  llvm::Value *visit(const CaseNode *node) { return nullptr; }
  llvm::Value *visit(const BreakNode *node);
  llvm::Value *visit(const ContinueNode *node);
  llvm::Value *visit(const ReturnNode *node);
  llvm::Value *visit(const CastNode *node);
  llvm::Value *visit(const IsExprNode *node);
  llvm::Value *visit(const TryStmtNode *node);
  llvm::Value *visit(const ThrowStmtNode *node);
  llvm::Value *visit(const AssertStmtNode *node);
  llvm::Value *visit(const ParamDeclNode *node);
  llvm::Value *visit(const ModuleNode *node);
  llvm::Value *visit(const MemberAccessNode *node);
  llvm::Value *visit(const UnionDeclNode *node);
  llvm::Value *visit(const StructDeclNode *node);
  llvm::Value *visit(const ClassDeclNode *node);
  llvm::Value *visit(const AnnotationDeclNode *node);
  llvm::Value *visit(const TypedefDeclNode *node);
  llvm::Value *visit(const AnnotationNode *node);
  llvm::Value *visit(const ArraySubscriptNode *node);
  llvm::Value *visit(const NewExprNode *node);
  llvm::Value *visit(const DeleteExprNode *node);
  llvm::Value *visit(const ConstExprNode *node);
  llvm::Value *visit(const DestructorCallNode *node);
  llvm::Value *visit(const TypeLiteralNode *node);
  llvm::Value *visit(const ArrayLiteralNode *node);
  llvm::Value *visit(const MapLiteralNode *node);
  llvm::Value *visit(const NullNode *node);
  llvm::Value *visit(const LambdaNode *node);
  llvm::Value *visit(const EnumDeclNode *node);
  llvm::Value *visit(const EnumMemberNode *node);
  llvm::Value *visit(const ImplicitCastNode *node);
  llvm::Value *visit(const AwaitExprNode *node);

  llvm::Value *createImplicitCast(llvm::Value *src, llvm::Type *destTy);
  llvm::Value *createImplicitCast(llvm::Value *src, llvm::Type *destTy,
                                  const Type *srcType);
  void emitLoopCleanups(size_t targetDepth);

  /* Emits a call, or an invoke to a landing pad when the function may
   * unwind and either a try is active or destructor-bearing objects are
   * live: the pad captures the cleanups active at the invoke site (plus
   * the innermost try's catch clauses) so no chained pads are needed. */
  llvm::Value *emitCallOrInvoke(llvm::FunctionType *fty, llvm::Value *callee,
                                llvm::ArrayRef<llvm::Value *> args,
                                const llvm::Twine &name = "");

  /* Shared EH slots / resume block of the current function (created on
   * demand, reset per function). */
  llvm::AllocaInst *getOrCreateEHExnSlot();
  llvm::AllocaInst *getOrCreateEHSelSlot();
  /* Emits the per-function exception resume block: reconstructs the landing
   * pad value from the exception slots and resumes unwinding. In async
   * functions it is the implicit error boundary (see CodeGen.cpp). */
  llvm::BasicBlock *getOrCreateEHResumeBlock();

  /* Function-scoped EH state: whether the current function's own scopes
   * are inside a try, or hold pending destructor cleanups. The global
   * cgCtx variants also see the enclosing function's scopes when a nested
   * function (lambda/thunk) is emitted mid-body. */
  bool funcScopeHasTry() const;
  bool funcScopeHasCleanups() const;
  /* Runs every live scope's cleanups (innermost first) into the current
   * block; used by landing pads. Only scopes from 'scopeStart' (inclusive)
   * run: a try's pad must not destroy objects declared before the try
   * (their destructor runs once on the normal path; destroying them in the
   * pad would run it twice on the caught path), and a nested function's
   * pad must not destroy the enclosing function's locals. */
  void emitScopeCleanupsInPad(size_t scopeStart);

  /* Async support (shared with the Future intrinsics). */

  /* Returns true when 't' (possibly behind a pointer/reference) is a
   * Future<T>; 'outValue' receives T when non-null. */
  static bool unwrapFutureType(const Type *t, const Type **outValue);

  /* Allocates a Future<T> object wrapping 'state' and returns a pointer to
   * it. The state's initial reference is transferred to the object. */
  llvm::Value *createFutureObject(const Type *futureType,
                                  llvm::Value *state);

  /* Wraps a Future object pointer into the Future<T> value type. */
  llvm::Value *materializeFutureValue(const Type *futureType,
                                      llvm::Value *objPtr);

  /* Pointer to the Future object for an expression of Future type (handles
   * both by-value and pointer operands). */
  llvm::Value *getFutureObjectPointer(const ExprNode *expr);

  /* Loads the '_state' field of a Future object pointer. 'operandType'
   * is the expression's type, used to distinguish Future values from
   * Future object pointers. */
  llvm::Value *getFutureState(llvm::Value *futureValueOrObjPtr,
                              const Type *operandType);

  /* Reads the value out of a future state (load for trivial types, a
   * move/copy-constructed temporary otherwise). */
  llvm::Value *readFutureValue(llvm::Value *state, const Type *valueType);

  /* Moves/copies 'src' (an expression or SSA value) into a future state's
   * value slot. */
  void writeFutureValueInto(llvm::Value *state, llvm::Value *src,
                            const Type *valueType, bool srcIsLValue = false);

  llvm::Value *getLValue(const ExprNode *node);

  /* Returns the 'this' slot address of the current method. Reports an error
   * and returns nullptr when 'this' is not bound (top-level code, static
   * functions): callers must check before dereferencing, since a null
   * 'this' slot would otherwise produce a silent segfault in codegen. */
  llvm::Value *lookupThis(const ASTNode *errSite);

  /* Reports an error diagnostic against the file currently being compiled. */
  void reportError(int line, int column, int length,
                   const std::string &message);

  /* Emits a runtime-library call (get-or-create the declaration). */
  llvm::CallInst *emitRuntimeCall(const std::string &name,
                                  llvm::Type *retTy,
                                  llvm::ArrayRef<llvm::Value *> args);

  /* Cached compiler-generated helpers for the async runtime. */
  llvm::Function *getOrCreateFutureValueDtor(const Type *valueType);
  llvm::Function *getOrCreateThenThunk(const Type *valueType, bool asyncCb,
                                        bool cbTakesValue, bool cbIsClosure);
  llvm::Function *getOrCreateThreadThunk(const Type *valueType,
                                         bool cbIsClosure);
  llvm::Function *getOrCreateAsyncThreadThunk(const Type *valueType,
                                              bool cbIsClosure);

  /* Closures: allocates the environment of a capturing lambda (a
   * reference-counted heap block holding {refcount, fn, dtor, captures})
   * and returns its pointer; the creating scope's exit releases it. */
  llvm::Value *createClosureEnvironment(const LambdaNode *node);
  llvm::Function *getOrCreateClosureEnvDtor(const LambdaNode *node);
  llvm::StructType *getClosureEnvType(const LambdaNode *node);
  void emitCaptureCopy(llvm::Value *dst, llvm::Value *src, const Type *type);

  /* The closure environment layout: {i32 refcount, fn pointer, dtor
   * pointer} followed by the captured values. */
  llvm::StructType *getClosureEnvHeaderTy();

  /* True when the callback argument of an async intrinsic is a closure
   * (a capturing lambda or a variable holding one), so its value is an
   * environment pointer instead of a function address. */
  static bool argIsClosure(const ExprNode *arg);

  /* Resolves the record type of a class declared in the prelude by its
   * simple name (used by the timeout helper to type its TimeoutException).
   * Returns null when the class is not visible. */
  const Type *getPreludeRecordType(std::string_view simpleName);

  /* 'void errThunk(ptr errValuePtr, ptr cb, ptr resultState)': runs an
   * error handler (Future.catchError, the onError parameter of then) and
   * completes resultState with its result, or chains it when the handler
   * is async. 'cbIsClosure' selects the closure invocation convention. */
  llvm::Function *getOrCreateErrorThunk(const Type *valueType, bool asyncCb,
                                        bool cbIsClosure);

  /* 'void timeoutThunk(ptr cb, ptr resultState)': runs the onTimeout
   * callback of Future.timeout (sync or async) and completes resultState
   * with its result. */
  llvm::Function *getOrCreateTimeoutThunk(const Type *valueType,
                                          bool asyncCb, bool cbIsClosure);

  /* 'void timeoutThunk(ptr cb, ptr resultState)': completes resultState
   * with a freshly constructed TimeoutException (the no-onTimeout case of
   * Future.timeout). */
  llvm::Function *getOrCreateTimeoutExceptionThunk();

  /* Invokes a user callback inside a compiler-generated helper thunk,
   * routing an exception raised by the callback into a completed-with-
   * error future instead of unwinding into the event loop. Returns the
   * callback's result. When 'closureEnv' is non-null the callback is a
   * closure: its environment reference is released both on the normal path
   * and when the callback throws (the intrinsic retained it before
   * registering). */
  llvm::Value *emitThunkCallbackCall(llvm::FunctionType *fty,
                                     llvm::Value *callee,
                                     llvm::ArrayRef<llvm::Value *> args,
                                     llvm::Value *stateOnError,
                                     llvm::Value *closureEnv = nullptr);

  /* Lowers a callback invocation inside a generated thunk: for a closure
   * 'cb' is the environment pointer, so the callee becomes env->fn and the
   * environment is passed as the first argument ('closureEnv' is set for
   * the caller to release afterwards). Returns the invoke result. */
  llvm::Value *emitThunkClosureCall(llvm::Value *cb, bool cbIsClosure,
                                    llvm::FunctionType *plainFnTy,
                                    llvm::ArrayRef<llvm::Value *> args,
                                    llvm::Value *stateOnError,
                                    llvm::Value *&closureEnv);

  /* Creates a pending future state sized for 'valueType'. */
  llvm::Value *createFutureState(const Type *valueType);

  /* Lowers an argument passed through a C variadic tail: sub-32-bit values
   * are widened per the C ABI and by-value Strings decay to their data
   * pointer (the prelude's own printf wrappers call c_str() explicitly). */
  llvm::Value *lowerVariadicArg(const ExprNode *arg, llvm::Value *value);

private:
  llvm::Function *getOrCreateRuntimeFunction(const std::string &name,
                                             llvm::FunctionType *ty);
  /* The exception-handling personality routine (from libutopia_runtime). */
  llvm::Function *getOrCreatePersonalityFunction();
  void setupAsyncFunction(const FunctionDeclNode *node, llvm::Function *func);
  void emitAsyncReturn(const FunctionDeclNode *node, llvm::Value *value,
                       bool valueIsLValue);
  void emitAsyncFallthroughFinish(const FunctionDeclNode *node);
  void emitMainWrapper(llvm::Function *userMain,
                       const FunctionDeclNode *node);

private:
  BackendContext &backend;
  llvm::LLVMContext &ctx;
  llvm::Module &mod;
  llvm::IRBuilder<> builder;
  CodeGenContext cgCtx;
  DiagnosticsEngine &diags;
  ASTContext &astCtx;
  const FunctionDeclNode *currentFunc = nullptr;
  llvm::AllocaInst *lastTemporaryAlloca = nullptr;
  std::string currentFilePath;
  bool asyncEnabled = true;

  /* When set by getLValue, reference-typed function calls return their raw
   * address instead of the loaded value. */
  bool suppressRefResultLoad = false;

  /* CodeGen-context scope depth at function entry. Return/cleanup emission
   * must never touch scopes that belong to an enclosing function (e.g. when a
   * lambda's synthesized function is emitted while another function is being
   * generated). */
  std::vector<size_t> funcScopeStarts;

  std::unordered_set<const RecordType *> generatingRecords;

  DebugInfoEmitter diEmitter;
  TBAAManager tbaaManager;

  /* Active coroutine bookkeeping while an async function is emitted. */
  std::unique_ptr<CoroutineInfo> coroInfo;

  /* Cache of compiler-generated async helper functions. */
  std::unordered_map<std::string, llvm::Function *> asyncHelpers;

  /* Cache of per-type EH descriptors created by getOrCreateTypeInfoForType
   * for non-class types. */
  std::unordered_map<std::string, llvm::Constant *> ehTypeInfoCache;

  /* Per-function exception handling state (reset per function). */
  llvm::AllocaInst *ehExnSlot = nullptr;
  llvm::AllocaInst *ehSelSlot = nullptr;
  llvm::BasicBlock *ehResumeBlock = nullptr;
  /* Innermost try's catch dispatch + clauses, used by per-invoke pads. */
  std::vector<llvm::BasicBlock *> tryDispatchStack;
  std::vector<std::vector<llvm::Constant *>> tryTypeInfoStack;
  /* Scope index of each enclosing try (parallel to tryDispatchStack): the
   * try's landing pads destroy only the objects declared inside it, so the
   * enclosing scope's destructors still run exactly once on the normal
   * path. */
  std::vector<size_t> tryScopeStack;

  llvm::Function *globalInitFunc = nullptr;

  void emitLocation(const ASTNode *node);

  llvm::Type *getLLVMType(const Type *type);
  llvm::Constant *evaluateAsConstant(const ExprNode *node);
  llvm::Function *getOrCreateFunction(const FunctionDeclNode *node);
  llvm::Function *getOrCreateGlobalInitFunc();

  /* Dart-style const objects */

  /* Canonical const objects: one read-only global per (class, ctor, args)
   * key. 'linkonce_odr' + a deterministic name make identical constructions
   * (in the same or different modules) merge into a single address. */
  std::unordered_map<std::string, llvm::GlobalVariable *> canonicalConsts;

  llvm::GlobalVariable *getOrCreateCanonicalConst(const ExprNode *creation);
  /* Builds the constant initializer for a const object creation call. */
  llvm::Constant *buildConstObjectInitializer(const ExprNode *creation,
                                              std::vector<llvm::Constant *> &out);
  /* Recursive initializer builder; the parent ctor's param->arg mapping is
   * threaded through so 'super(param)' forwards resolve to constants. */
  llvm::Constant *buildConstObjectInitializerImpl(
      const ExprNode *creation, std::vector<llvm::Constant *> &out,
      const FunctionDeclNode *parentCtor,
      const std::vector<const ExprNode *> *parentParamArgs);
  /* Constant String struct {data, len, cap} backed by a static buffer. */
  llvm::Constant *buildConstString(llvm::StringRef value);
  llvm::Constant *buildConstStringGlobal(llvm::StringRef value);
  /* Rebuilds an LLVM constant from a serialized const value ("i:5",
   * "s:hi", "o:key", ...) converted to 'expected'. */
  llvm::Constant *buildConstFromSerialized(const ExprNode *node,
                                           const std::string &key,
                                           const Type *expected);
  /* Emits the runtime const registry (per-module table + linked list head,
   * populated by the module ctor) and the '__utopia_is_const_ptr' walker
   * that backs Memory.isConst(). */
  void emitConstRegistry();
  /* Canonical static array for 'const [1, 2, 3]' (key "A:..."). */
  llvm::Constant *buildConstArray(const ExprNode *node,
                                  const std::string &key);
  /* The ConstantArray value (for array-typed variables/globals). */
  llvm::Constant *buildConstArrayValue(const ExprNode *node);
  /* One const-array element: nested arrays are embedded as values. */
  llvm::Constant *buildConstArrayElement(const ExprNode *node);

  llvm::LoadInst *createTBAALoad(llvm::Type *llTy, llvm::Value *ptr,
                                 const Type *utopiaTy,
                                 const llvm::Twine &name = "");
  llvm::LoadInst *createTBAALoad(llvm::Type *llTy, llvm::Value *ptr,
                                 llvm::MDNode *tbaaTag,
                                 const llvm::Twine &name = "");

  llvm::StoreInst *createTBAAStore(llvm::Value *val, llvm::Value *ptr,
                                   const Type *utopiaTy);
  llvm::StoreInst *createTBAAStore(llvm::Value *val, llvm::Value *ptr,
                                   llvm::MDNode *tbaaTag);

  llvm::Constant *createTypeReflectionConstant(const Type *t,
                                               llvm::StructType *structTy);

  llvm::Constant *getOrCreateVTable(const ClassType *classTy);

  /* RTTI descriptor for a polymorphic class: a constant array of pointers
   * laid out as [parent descriptor or null, interface descriptors..., null].
   * Stored at the front of the class's vtable so 'expr is T' can walk the
   * dynamic type chain at runtime. */
  llvm::Constant *getOrCreateTypeInfo(const ClassType *classTy);

  /* RTTI descriptor for any throwable type: classes get the hierarchy
   * descriptor above (with the parent chain always linked so derived-to-base
   * catch matching works for non-polymorphic classes too); every other type
   * gets a single-null descriptor whose address identifies the type. */
  llvm::Constant *getOrCreateTypeInfoForType(const Type *type);

  void emitLifetimeStart(llvm::AllocaInst *allocaInst, uint64_t size);
  void emitLifetimeEnd(llvm::AllocaInst *allocaInst, uint64_t size);

  void emitConstructorCall(const FunctionCallNode *node,
                           llvm::Value *targetAddr);
  llvm::Value *materializeByValueArg(const ExprNode *arg,
                                     const Type *paramDeclTy);

  /* Evaluates an expression to its value: reference/rvalue-reference typed
   * expressions ('x as T&&') yield the referenced object's address, so the
   * value is loaded through it. */
  llvm::Value *dispatchValueOf(const ExprNode *arg);
  void emitArrayLiteralInit(llvm::Value *targetAddr, const Type *targetType,
                            const ExprNode *initExpr);
  llvm::AllocaInst *createEntryBlockAlloca(llvm::Type *type,
                                           const std::string &varName);
  void emitDefaultInitialization(llvm::Value *ptr, const Type *type);
  void emitArrayDefaultConstruct(llvm::Value *ptr, const Type *arrayType,
                                 const FunctionDeclNode *ctor);
  void emitCleanupCall(llvm::Value *ptr, const FunctionDeclNode *dtor,
                       const Type *type = nullptr,
                       llvm::Value *guard = nullptr);
  /* Overload accepting a direct runtime function (e.g. utopia_end_catch)
   * instead of an AST destructor. */
  void emitCleanupCall(llvm::Value *ptr, const FunctionDeclNode *dtor,
                       const Type *type, llvm::Value *guard,
                       llvm::Function *runtimeFn);
  /* Registers a scope cleanup and, for destructor-bearing locals of
   * functions that may unwind, the matching exception cleanup landing
   * pad. */
  void registerScopeCleanup(llvm::Value *ptr, const FunctionDeclNode *dtor,
                            const Type *type, llvm::Value *guard = nullptr,
                            llvm::Function *runtimeFn = nullptr);
  void emitScopeCleanups();
  void emitBranchCleanups(size_t cleanupCount);
  /* Emits a per-member copy (construction or assignment) for records
   * that are not trivially copyable, so members with destructors
   * (e.g. String) are deep-copied instead of bit-copied. */
  void emitMemberWiseCopy(llvm::Value *dst, llvm::Value *src,
                          const Type *type, bool isAssignment);
  llvm::Value *materializeTernaryBranchValue(llvm::Value *val,
                                             const ExprNode *expr);
};

} // namespace utopia