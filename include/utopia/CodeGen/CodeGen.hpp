#pragma once
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/CodeGen/BackendContext.hpp"
#include "utopia/CodeGen/CodeGenContext.hpp"
#include "utopia/CodeGen/DebugInfoEmitter.hpp"
#include "utopia/CodeGen/TBAAManager.hpp"
#include "utopia/Common/Diagnostics.hpp"
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
          bool emitDebugInfo, std::string filePath, bool asyncEnabled = true);

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
  llvm::Value *visit(const WhileNode *node);
  llvm::Value *visit(const SwitchNode *node);
  llvm::Value *visit(const CaseNode *node) { return nullptr; }
  llvm::Value *visit(const BreakNode *node);
  llvm::Value *visit(const ContinueNode *node);
  llvm::Value *visit(const ReturnNode *node);
  llvm::Value *visit(const CastNode *node);
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
  llvm::Value *visit(const DestructorCallNode *node);
  llvm::Value *visit(const TypeLiteralNode *node);
  llvm::Value *visit(const ArrayLiteralNode *node);
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

  /* Emits a runtime-library call (get-or-create the declaration). */
  llvm::CallInst *emitRuntimeCall(const std::string &name,
                                  llvm::Type *retTy,
                                  llvm::ArrayRef<llvm::Value *> args);

  /* Cached compiler-generated helpers for the async runtime. */
  llvm::Function *getOrCreateFutureValueDtor(const Type *valueType);
  llvm::Function *getOrCreateThenThunk(const Type *valueType, bool asyncCb,
                                        bool cbTakesValue);
  llvm::Function *getOrCreateThreadThunk(const Type *valueType);
  llvm::Function *getOrCreateAsyncThreadThunk(const Type *valueType);

  /* Creates a pending future state sized for 'valueType'. */
  llvm::Value *createFutureState(const Type *valueType);

private:
  llvm::Function *getOrCreateRuntimeFunction(const std::string &name,
                                             llvm::FunctionType *ty);
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

  llvm::Function *globalInitFunc = nullptr;

  void emitLocation(const ASTNode *node);

  llvm::Type *getLLVMType(const Type *type);
  llvm::Constant *evaluateAsConstant(const ExprNode *node);
  llvm::Function *getOrCreateFunction(const FunctionDeclNode *node);
  llvm::Function *getOrCreateGlobalInitFunc();

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