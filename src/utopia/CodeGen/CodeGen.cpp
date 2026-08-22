#include "utopia/CodeGen/CodeGen.hpp"
#include "utopia/CodeGen/Intrinsics.hpp"
#include <llvm/ADT/APSInt.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <functional>
#include <string>

namespace utopia {

/* Forward declarations (defined below with the record helpers). */
static bool isTriviallyCopyable(
    const Type *t, llvm::SmallPtrSetImpl<const RecordType *> &visited);
static const FunctionDeclNode *getCustomDestructor(const Type *t);
static const FunctionDeclNode *findAssignmentOperator(const Type *t);
static const FunctionDeclNode *findCopyOrMoveCtor(const Type *t,
                                                  bool preferMove);


namespace {
enum class BinOpCode {
  Add,
  Sub,
  Mul,
  Div,
  Rem,
  And,
  Or,
  Xor,
  Shl,
  Shr,
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge
};

static const std::unordered_map<std::string_view, BinOpCode> binOpMap = {
    {"+", BinOpCode::Add},  {"-", BinOpCode::Sub}, {"*", BinOpCode::Mul},
    {"/", BinOpCode::Div},  {"%", BinOpCode::Rem}, {"&", BinOpCode::And},
    {"|", BinOpCode::Or},   {"^", BinOpCode::Xor}, {"<<", BinOpCode::Shl},
    {">>", BinOpCode::Shr}, {"==", BinOpCode::Eq}, {"!=", BinOpCode::Ne},
    {"<", BinOpCode::Lt},   {"<=", BinOpCode::Le}, {">", BinOpCode::Gt},
    {">=", BinOpCode::Ge}};

enum class AssignOpCode { Add, Sub, Mul, Div, Rem, And, Or, Xor, Shl, Shr };

static const std::unordered_map<std::string_view, AssignOpCode> assignOpMap = {
    {"+", AssignOpCode::Add},  {"-", AssignOpCode::Sub},
    {"*", AssignOpCode::Mul},  {"/", AssignOpCode::Div},
    {"%", AssignOpCode::Rem},  {"&", AssignOpCode::And},
    {"|", AssignOpCode::Or},   {"^", AssignOpCode::Xor},
    {"<<", AssignOpCode::Shl}, {">>", AssignOpCode::Shr}};
} // namespace

CodeGen::CodeGen(BackendContext &bCtx, llvm::Module &llvmMod,
                 DiagnosticsEngine &diags, bool emitDebugInfo,
                 std::string filePath, ASTContext &astCtx, bool asyncEnabled)
    : backend(bCtx), ctx(bCtx.getLLVMContext()), mod(llvmMod), builder(ctx),
      diags(diags), astCtx(astCtx), currentFilePath(std::move(filePath)),
      asyncEnabled(asyncEnabled), diEmitter(llvmMod, emitDebugInfo),
      tbaaManager(ctx) {

  llvm::FastMathFlags fmf;
  fmf.setFast();
  builder.setFastMathFlags(fmf);
}

llvm::Value *CodeGen::dispatch(const ASTNode *node) {
  if (!node)
    return nullptr;
  emitLocation(node);
  return ASTVisitor<CodeGen, llvm::Value *>::dispatch(node);
}

void CodeGen::emitLocation(const ASTNode *node) {
  diEmitter.emitLocation(builder, node);
}

void CodeGen::reportError(int line, int column, int length,
                          const std::string &message) {
  diags.report({DiagLevel::Error, line, column, length, message,
                currentFilePath});
}

llvm::Type *CodeGen::getLLVMType(const Type *type) {
  if (!type)
    return builder.getVoidTy();

  if (auto *cTy = llvm::dyn_cast<ConstType>(type)) {
    return getLLVMType(cTy->getBaseType());
  }
  if (auto *eTy = llvm::dyn_cast<EnumType>(type)) {
    return getLLVMType(eTy->getUnderlyingType());
  }
  if (auto *aliasTy = llvm::dyn_cast<AliasType>(type)) {
    return getLLVMType(aliasTy->getTarget());
  }
  if (auto *fTy = llvm::dyn_cast<FunctionType>(type)) {
    std::vector<llvm::Type *> paramTys;
    for (const auto *p : fTy->getParamTypes()) {
      if (llvm::isa<ArrayType>(p)) {
        paramTys.push_back(builder.getPtrTy());
      } else {
        paramTys.push_back(getLLVMType(p));
      }
    }
    return llvm::FunctionType::get(getLLVMType(fTy->getReturnType()), paramTys,
                                   false);
  }

  if (llvm::isa<TemplateParamType>(type)) {
    diags.report({DiagLevel::Error, 0, 0, 0,
                  "Uninstantiated template parameter '" + type->toString() +
                      "' reached code generation.",
                  currentFilePath});
    return builder.getInt8Ty();
  }

  if (auto *bTy = llvm::dyn_cast<BuiltinType>(type)) {
    switch (bTy->getBuiltinKind()) {
    case BuiltinKind::TypeVal:
      return builder.getInt8Ty();
    case BuiltinKind::Namespace:
      return builder.getVoidTy();
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
    case BuiltinKind::USize:
      return builder.getIntNTy(mod.getDataLayout().getPointerSizeInBits());
    case BuiltinKind::Float32:
      return builder.getFloatTy();
    case BuiltinKind::Float64:
      return builder.getDoubleTy();
    case BuiltinKind::Bool:
      return builder.getInt1Ty();
    case BuiltinKind::Void:
      return builder.getVoidTy();
    }
  } else if (llvm::isa<PointerType>(type) || llvm::isa<ReferenceType>(type) ||
             llvm::isa<RValueReferenceType>(type)) {
    return builder.getPtrTy();
  } else if (auto *arrTy = llvm::dyn_cast<ArrayType>(type)) {
    /* Empty literals type their element as 'void' ('void[0]'), which LLVM
     * rejects inside an array type; a zero-length array of i8 is used
     * instead (mirrors the MapLiteralType lowering below). */
    llvm::Type *elemLL = getLLVMType(arrTy->getElementType());
    if (elemLL->isVoidTy())
      elemLL = builder.getInt8Ty();
    return llvm::ArrayType::get(elemLL, arrTy->getSize());
  } else if (auto *vTy = llvm::dyn_cast<VectorType>(type)) {
    return llvm::FixedVectorType::get(getLLVMType(vTy->getElementType()),
                                      vTy->getLanes());
  } else if (auto *mapTy = llvm::dyn_cast<MapLiteralType>(type)) {
    /* The literal is lowered to two parallel arrays: keys and values. Empty
     * literals type their key/value as 'void', which LLVM rejects inside an
     * array type; a zero-length array of i8 is used instead. */
    auto elemLL = [&](const Type *t) {
      llvm::Type *lt = getLLVMType(t);
      return lt->isVoidTy() ? builder.getInt8Ty() : lt;
    };
    llvm::Type *keysArr = llvm::ArrayType::get(
        elemLL(mapTy->getKeyType()), mapTy->getSize());
    llvm::Type *valuesArr = llvm::ArrayType::get(
        elemLL(mapTy->getValueType()), mapTy->getSize());
    return llvm::StructType::get(ctx, {keysArr, valuesArr});
  } else if (auto *rec = llvm::dyn_cast<RecordType>(type)) {
    llvm::StructType *structTy =
        llvm::StructType::getTypeByName(ctx, rec->getName());

    /* Register the opaque type first to support self-referential structures
     * and recursive pointers gracefully without infinite loops. */
    if (!structTy) {
      structTy = llvm::StructType::create(ctx, rec->getName());
    }

    if (structTy->isOpaque() && !rec->isOpaque()) {
      if (generatingRecords.contains(rec)) {
        diags.report({DiagLevel::Error, 0, 0, 0,
                      "Infinite size detected due to recursive value type '" +
                          std::string(rec->getName()) +
                          "'. Use a pointer or reference instead.",
                      currentFilePath});
        return structTy;
      }
      generatingRecords.insert(rec);

      std::vector<llvm::Type *> elements;

      if (llvm::isa<UnionType>(type)) {
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
        if (llvm::isa<ClassType>(type) &&
            static_cast<const ClassType *>(type)->getIsPolymorphic()) {
          elements.push_back(builder.getPtrTy()); /* Virtual Pointer */
        }

        for (const auto &f : rec->getFields()) {
          elements.push_back(getLLVMType(f.type));
        }

        bool isPacked = false;
        if (const DeclNode *decl = rec->getDeclaration()) {
          isPacked = decl->isPacked;
        }
        structTy->setBody(elements, isPacked);
      }

      generatingRecords.erase(rec);
    }

    return structTy;
  }

  diags.report({DiagLevel::Error, 0, 0, 0,
                "Unsupported or unresolved type reached code generation: " +
                    type->toString(),
                currentFilePath});
  return builder.getInt8Ty();
}

llvm::Constant *
CodeGen::createTypeReflectionConstant(const Type *t,
                                      llvm::StructType *structTy) {
  if (!t)
    return llvm::UndefValue::get(structTy);

  auto createStr = [&](const std::string &str) -> llvm::Constant * {
    llvm::Constant *strConst =
        llvm::ConstantDataArray::getString(ctx, str, true);
    auto *gv = new llvm::GlobalVariable(mod, strConst->getType(), true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        strConst, ".str.type");
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    llvm::Constant *zero = llvm::ConstantInt::get(builder.getInt32Ty(), 0);
    return llvm::ConstantExpr::getInBoundsGetElementPtr(
        strConst->getType(), gv, llvm::ArrayRef<llvm::Constant *>{zero, zero});
  };

  std::vector<llvm::Constant *> fields;
  /* uint8* name */
  fields.push_back(createStr(t->toString()));

  const Type *unqual = t->getUnqualifiedType();

  bool isClass = llvm::isa<ClassType>(unqual);
  bool isStruct = llvm::isa<StructType>(unqual);
  bool isPrimitive = llvm::isa<BuiltinType>(unqual);
  bool isEnum = llvm::isa<EnumType>(unqual);
  bool isArray = llvm::isa<ArrayType>(unqual);
  bool isPointer = llvm::isa<PointerType>(unqual);

  fields.push_back(
      llvm::ConstantInt::get(builder.getInt1Ty(), isClass ? 1 : 0));
  fields.push_back(
      llvm::ConstantInt::get(builder.getInt1Ty(), isStruct ? 1 : 0));
  fields.push_back(
      llvm::ConstantInt::get(builder.getInt1Ty(), isPrimitive ? 1 : 0));
  fields.push_back(llvm::ConstantInt::get(builder.getInt1Ty(), isEnum ? 1 : 0));
  fields.push_back(
      llvm::ConstantInt::get(builder.getInt1Ty(), isArray ? 1 : 0));
  fields.push_back(
      llvm::ConstantInt::get(builder.getInt1Ty(), isPointer ? 1 : 0));

  return llvm::ConstantStruct::get(structTy, fields);
}

llvm::Value *CodeGen::createImplicitCast(llvm::Value *src, llvm::Type *destTy) {
  return createImplicitCast(src, destTy, nullptr);
}

llvm::Value *CodeGen::lowerVariadicArg(const ExprNode *arg,
                                       llvm::Value *value) {
  if (!value || !arg || !arg->exprType)
    return value;

  const Type *argU = arg->exprType->getUnqualifiedType();
  if (auto *rec = llvm::dyn_cast<RecordType>(argU)) {
    /* A by-value String passed through '...' decays to its data pointer.
     * printf's %s consumes exactly one machine word; forwarding the whole
     * 24-byte struct would shift every later argument into the wrong
     * register slot (e.g. '%s:%d' would print the String length as the
     * integer). */
    if (rec->getName() == "String" && value->getType()->isStructTy()) {
      if (const FieldInfo *dataField = rec->getField("data")) {
        llvm::Value *dataPtr =
            builder.CreateExtractValue(value, dataField->index, "str.data");
        if (dataPtr->getType()->isPointerTy())
          return dataPtr;
      }
    }
    return value;
  }

  /* Widen per the C ABI: sub-32-bit integers are sign/zero-extended to
   * 32 bits and float32 promotes to float64. Passing them raw would make
   * the callee's va_arg read garbage (e.g. %d on an i8 value). */
  if (argU->getKind() == TypeKind::Builtin) {
    auto bKind = static_cast<const BuiltinType *>(argU)->getBuiltinKind();
    if (bKind == BuiltinKind::Float32 || value->getType()->isFloatTy()) {
      return builder.CreateFPExt(value, builder.getDoubleTy());
    }
    if (value->getType()->isIntegerTy() &&
        value->getType()->getIntegerBitWidth() < 32) {
      bool isSigned = (bKind >= BuiltinKind::Int8 && bKind <= BuiltinKind::Int64);
      return builder.CreateIntCast(value, builder.getInt32Ty(), isSigned);
    }
  } else if (value->getType()->isFloatTy()) {
    return builder.CreateFPExt(value, builder.getDoubleTy());
  } else if (value->getType()->isIntegerTy() &&
             value->getType()->getIntegerBitWidth() < 32) {
    /* Non-builtin (e.g. typedef) integer: zero-extend by default. */
    return builder.CreateIntCast(value, builder.getInt32Ty(), false);
  }
  return value;
}

llvm::Value *CodeGen::createImplicitCast(llvm::Value *src, llvm::Type *destTy,
                                         const Type *srcType) {
  if (!src)
    return nullptr;
  llvm::Type *srcTy = src->getType();
  if (srcTy == destTy)
    return src;

  /* Determine the source signedness from the Utopia type when available;
   * defaults to signed for raw LLVM values. Integer widening must respect
   * it (uint8 -> int32 zero-extends, int8 -> int32 sign-extends). */
  bool srcIsSigned = true;
  if (srcType) {
    const Type *u = srcType->getUnqualifiedType();
    if (u->getKind() == TypeKind::Builtin) {
      auto k = static_cast<const BuiltinType *>(u)->getBuiltinKind();
      srcIsSigned = (k >= BuiltinKind::Int8 && k <= BuiltinKind::Int64);
    }
  }

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
    /* Conversion to i1 (bool) must be a zero-test: truncating would turn
     * even values like 42 into false. */
    if (destTy->getIntegerBitWidth() == 1)
      return builder.CreateICmpNE(
          src, llvm::ConstantInt::get(srcTy, 0), "bool.cast");
    /* i1 (bool) must zero-extend: sign-extending a boolean would yield -1
     * for 'true' instead of 1. */
    if (srcTy->getIntegerBitWidth() == 1)
      return builder.CreateIntCast(src, destTy, false);
    return builder.CreateIntCast(src, destTy, srcIsSigned);
  }
  if (srcTy->isFloatingPointTy() && destTy->isFloatingPointTy()) {
    return builder.CreateFPCast(src, destTy);
  }
  if (srcTy->isIntegerTy() && destTy->isFloatingPointTy()) {
    if (srcIsSigned)
      return builder.CreateSIToFP(src, destTy);
    return builder.CreateUIToFP(src, destTy);
  }
  if (srcTy->isFloatingPointTy() && destTy->isIntegerTy()) {
    /* Conversion to i1 (bool): zero-test the float. */
    if (destTy->getIntegerBitWidth() == 1)
      return builder.CreateFCmpONE(
          src, llvm::ConstantFP::get(srcTy, 0.0), "bool.cast");
    return builder.CreateFPToSI(src, destTy);
  }

  return builder.CreateBitCast(src, destTy);
}

llvm::Function *CodeGen::getOrCreateFunction(const FunctionDeclNode *node) {
  if (node->isTemplate)
    return nullptr;

  std::string irName =
      (node->name == "main" && !node->isMethod)
          ? (asyncEnabled ? "utopia_user_main" : "main")
          : node->mangledName;
  llvm::Function *func = mod.getFunction(irName);

  if (func)
    return func;

  std::vector<llvm::Type *> paramTypes;

  if (node->isMethod && !node->isExtern && !node->isStatic &&
      node->parentRecord) {
    paramTypes.push_back(builder.getPtrTy());
  }

  for (const auto *p : node->params) {
    if (llvm::isa<ArrayType>(p->type)) {
      paramTypes.push_back(builder.getPtrTy());
    } else {
      paramTypes.push_back(getLLVMType(p->type));
    }
  }

  llvm::Type *retTy = getLLVMType(node->returnType);

  if (node->name == "main" && !node->isMethod && node->returnType->isVoid()) {
    retTy = builder.getInt32Ty();
  }

  /* Async functions are coroutines: the externally visible signature returns
   * a pointer to the Future object. */
  if (node->isAsync) {
    retTy = builder.getPtrTy();
  }

  llvm::FunctionType *funcType =
      llvm::FunctionType::get(retTy, paramTypes, node->isVariadic);

  /* Apply weak linkage if the function is annotated with @weak */
  llvm::GlobalValue::LinkageTypes linkage =
      node->isWeak ? llvm::GlobalValue::WeakAnyLinkage
                   : llvm::GlobalValue::ExternalLinkage;

  func = llvm::Function::Create(funcType, linkage, irName, mod);

  /* Apply the parsed calling convention before setting other attributes */
  if (node->callingConv == "stdcall") {
    func->setCallingConv(llvm::CallingConv::X86_StdCall);
  } else if (node->callingConv == "fastcall") {
    func->setCallingConv(llvm::CallingConv::X86_FastCall);
  } else {
    func->setCallingConv(llvm::CallingConv::C);
  }

  /* Functions that cannot unwind are marked nounwind so the optimizer can
   * turn invokes into plain calls; functions that may unwind keep the
   * attribute off (see the mayUnwind fixed-point analysis in Sema). */
  if (!node->mayUnwind) {
    func->addFnAttr(llvm::Attribute::NoUnwind);
  }

  /* Async functions compile to LLVM coroutines: mark the function so the
   * CoroSplit pass recognizes it (LLVM 19+ requires the presplit attribute
   * to be set by the frontend). The enum form must be used: the string form
   * is not recognized as the PresplitCoroutine attribute. */
  if (node->isAsync) {
    func->addFnAttr(llvm::Attribute::PresplitCoroutine);
  }

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
        if (auto *num = llvm::dyn_cast<NumberNode>(ann->args[0])) {
          if (!num->isFloat) {
            uint64_t sz = std::stoull(std::string(num->raw), nullptr, 0);
            addAttrObj(llvm::Attribute::getWithDereferenceableBytes(ctx, sz));
          }
        }
      }
    }

    if (!llvm::isa<ReferenceType>(type) && !llvm::isa<PointerType>(type) &&
        !llvm::isa<RValueReferenceType>(type))
      return;

    if (llvm::isa<ReferenceType>(type) ||
        llvm::isa<RValueReferenceType>(type)) {
      addAttrObj(llvm::Attribute::get(ctx, llvm::Attribute::NonNull));
      addAttrObj(llvm::Attribute::get(ctx, llvm::Attribute::NoUndef));
    }

    const Type *pointee = nullptr;
    if (auto *refTy = llvm::dyn_cast<ReferenceType>(type)) {
      pointee = refTy->getPointeeType();
    } else if (auto *rvRefTy = llvm::dyn_cast<RValueReferenceType>(type)) {
      pointee = rvRefTy->getPointeeType();
    } else if (auto *ptrTy = llvm::dyn_cast<PointerType>(type)) {
      pointee = ptrTy->getPointeeType();
    }

    if (pointee->isConstQualified() && paramIdx.has_value()) {
      addAttrObj(llvm::Attribute::get(ctx, llvm::Attribute::ReadOnly));
      addAttrObj(
          llvm::Attribute::getWithCaptureInfo(ctx, llvm::CaptureInfo::none()));
    }

    const Type *unqualPointee = pointee->getUnqualifiedType();

    if (auto *builtinPointee = llvm::dyn_cast<BuiltinType>(unqualPointee)) {
      if (llvm::isa<ReferenceType>(type) ||
          llvm::isa<RValueReferenceType>(type)) {
        auto kind = builtinPointee->getBuiltinKind();
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
      emitCleanupCall(cleanupIt->instancePtr, cleanupIt->destructor,
                      cleanupIt->type, cleanupIt->guard, cleanupIt->runtimeFn);
    }
    for (auto lifeIt = scopeIt->lifetimes.rbegin();
         lifeIt != scopeIt->lifetimes.rend(); ++lifeIt) {
      emitLifetimeEnd(lifeIt->allocaInst, lifeIt->size);
    }
  }
}

llvm::Value *CodeGen::visit(const NamespaceDeclNode *node) {
  for (const auto *stmt : node->statements) {
    dispatch(stmt);
  }
  return nullptr;
}

llvm::Value *CodeGen::visit(const UsingNode *node) { return nullptr; }

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
      const Type *paramDeclTy = nullptr;
      if (node->resolvedFunc &&
          astParamIdx < node->resolvedFunc->params.size()) {
        paramDeclTy = node->resolvedFunc->params[astParamIdx]->type;
      }

      argVal = paramDeclTy ? materializeByValueArg(arg, paramDeclTy)
                           : nullptr;

      if (!argVal) {
        lastTemporaryAlloca = nullptr;
        argVal = dispatch(arg);
        lastTemporaryAlloca = nullptr;
      }

      if (func && argVal && llArgIdx < func->arg_size()) {
        llvm::Type *paramTy = func->getFunctionType()->getParamType(llArgIdx);
        argVal = createImplicitCast(argVal, paramTy, arg->exprType);
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

  /* The constructor body may throw (e.g. it calls a throwing function), so
   * route the call through the active landing pad when one exists. */
  emitCallOrInvoke(func->getFunctionType(), func, argsArgs);
}

llvm::Value *CodeGen::dispatchValueOf(const ExprNode *arg) {
  const Type *argTy = arg->exprType;
  if (argTy && (argTy->isReferenceType() ||
                argTy->getKind() == TypeKind::RValueReference)) {
    llvm::Value *addr = getLValue(arg);
    if (!addr)
      return nullptr;
    const Type *pointee = nullptr;
    if (argTy->isReferenceType())
      pointee = static_cast<const ReferenceType *>(argTy)->getPointeeType();
    else
      pointee =
          static_cast<const RValueReferenceType *>(argTy)->getPointeeType();
    return createTBAALoad(getLLVMType(pointee), addr, pointee);
  }
  return dispatch(arg);
}

llvm::Value *CodeGen::materializeByValueArg(const ExprNode *arg,
                                            const Type *paramDeclTy) {
  if (!arg || !paramDeclTy)
    return nullptr;

  const Type *unqualParam = paramDeclTy->getUnqualifiedType();
  if (unqualParam->getKind() != TypeKind::Class &&
      unqualParam->getKind() != TypeKind::Struct &&
      unqualParam->getKind() != TypeKind::Union) {
    return nullptr;
  }
  auto *recTy = static_cast<const RecordType *>(unqualParam);
  const DeclNode *decl = recTy->getDeclaration();
  if (!decl)
    return nullptr;

  const FunctionDeclNode *dtor = nullptr;
  if (decl->kind == NodeKind::ClassDecl)
    dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
  else if (decl->kind == NodeKind::StructDecl)
    dtor = static_cast<const StructDeclNode *>(decl)->destructor;
  else if (decl->kind == NodeKind::UnionDecl)
    dtor = static_cast<const UnionDeclNode *>(decl)->destructor;

  /* Records without a custom destructor may be passed by value with plain
   * bitwise semantics. */
  if (!dtor || dtor->isImplicit)
    return nullptr;

  llvm::ArrayRef<FunctionDeclNode *> ctors;
  if (decl->kind == NodeKind::ClassDecl)
    ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
  else if (decl->kind == NodeKind::StructDecl)
    ctors = static_cast<const StructDeclNode *>(decl)->constructors;
  else if (decl->kind == NodeKind::UnionDecl)
    ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

  const FunctionDeclNode *copyOrMove = nullptr;
  bool preferMove = !arg->isLValue;
  for (auto *c : ctors) {
    if (c->params.size() != 1)
      continue;
    const Type *p0 = c->params[0]->type;
    const Type *pointee = nullptr;
    if (p0->isReferenceType()) {
      pointee =
          static_cast<const ReferenceType *>(p0)->getPointeeType();
    } else if (p0->getKind() == TypeKind::RValueReference) {
      pointee = static_cast<const RValueReferenceType *>(p0)
                    ->getPointeeType();
    }
    if (!pointee || pointee->getUnqualifiedType() != unqualParam)
      continue;
    if (p0->getKind() == TypeKind::RValueReference) {
      /* Prefer the move constructor for r-values, but keep looking so the
       * copy constructor remains the fallback when no move constructor
       * exists. */
      if (preferMove) {
        copyOrMove = c;
        break;
      }
    } else if (p0->isReferenceType()) {
      if (!preferMove) {
        copyOrMove = c;
        break;
      }
      if (!copyOrMove) {
        copyOrMove = c;
      }
    }
  }

  if (!copyOrMove)
    return nullptr;

  llvm::AllocaInst *tmp =
      createEntryBlockAlloca(getLLVMType(paramDeclTy), "tmp.arg.copy");
  emitDefaultInitialization(tmp, paramDeclTy);

  llvm::Value *src = nullptr;
  lastTemporaryAlloca = nullptr;
  src = getLValue(arg);
  bool srcOwnsRvalue = false;
  if (!src) {
    llvm::Value *val = dispatch(arg);
    if (lastTemporaryAlloca) {
      src = lastTemporaryAlloca;
      lastTemporaryAlloca = nullptr;
    } else if (val) {
      src = createEntryBlockAlloca(val->getType(), "tmp.arg.src");
      createTBAAStore(val, src, arg->exprType);
      /* The dispatched rvalue (e.g. a call result) owns its storage: after
       * the copy/move below its buffers must be released. */
      srcOwnsRvalue = !arg->isLValue;
    }
  }

  if (src) {
    llvm::Function *ctorFunc = getOrCreateFunction(copyOrMove);
    emitCallOrInvoke(ctorFunc->getFunctionType(), ctorFunc, {tmp, src});
    if (srcOwnsRvalue) {
      const Type *srcUnqual = arg->exprType->getUnqualifiedType();
      if (srcUnqual->getKind() == TypeKind::Class ||
          srcUnqual->getKind() == TypeKind::Struct ||
          srcUnqual->getKind() == TypeKind::Union) {
        auto *srcRecTy = static_cast<const RecordType *>(srcUnqual);
        auto *srcDecl = srcRecTy->getDeclaration();
        const FunctionDeclNode *srcDtor = nullptr;
        if (srcDecl) {
          if (srcDecl->kind == NodeKind::ClassDecl)
            srcDtor = static_cast<const ClassDeclNode *>(srcDecl)->destructor;
          else if (srcDecl->kind == NodeKind::StructDecl)
            srcDtor = static_cast<const StructDeclNode *>(srcDecl)->destructor;
          else if (srcDecl->kind == NodeKind::UnionDecl)
            srcDtor = static_cast<const UnionDeclNode *>(srcDecl)->destructor;
        }
        if (srcDtor && !srcDtor->isImplicit) {
          emitCleanupCall(src, srcDtor, arg->exprType);
        }
      }
    }
  }
  /* The by-value argument is transferred to the callee, which owns it and
   * registers the destructor cleanup for the parameter. Do NOT register a
   * cleanup here. */
  lastTemporaryAlloca = nullptr;
  return createTBAALoad(getLLVMType(paramDeclTy), tmp, paramDeclTy);
}

llvm::Constant *CodeGen::evaluateAsConstant(const ExprNode *node) {
  if (!node)
    return nullptr;

  /* Dart-style const expressions: the Sema pass validated the expression
   * and stored its serialized constant value; rebuild the LLVM constant
   * from it (this also resolves canonical const objects to their static
   * instances). Array-typed globals hold the constant VALUE; expression
   * contexts get the decayed pointer from buildConstFromSerialized. */
  if (node->isConstExpr && !node->constKey.empty()) {
    if (node->constKey.rfind("A:", 0) == 0) {
      if (llvm::Constant *v = buildConstArrayValue(node))
        return v;
    }
    llvm::Constant *c =
        buildConstFromSerialized(node, node->constKey, node->exprType);
    if (c)
      return c;
  }

  if (auto *ternaryNode = llvm::dyn_cast<TernaryOpNode>(node)) {
    llvm::Constant *condC = evaluateAsConstant(ternaryNode->condition);
    if (!condC)
      return nullptr;

    if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(condC)) {
      if (ci->getZExtValue() != 0) {
        return evaluateAsConstant(ternaryNode->trueExpr);
      } else {
        return evaluateAsConstant(ternaryNode->falseExpr);
      }
    }
    return nullptr;
  }

  if (auto *call = llvm::dyn_cast<FunctionCallNode>(node)) {
    if (call->resolvedFunc && call->resolvedFunc->isIntrinsic) {
      if (const Intrinsic *intrinsic = IntrinsicRegistry::instance().get(
              call->resolvedFunc->intrinsicName)) {
        return intrinsic->evaluateConstant(*this, call);
      }
    }
  }

  if (llvm::isa<NullNode>(node)) {
    return llvm::ConstantPointerNull::get(builder.getPtrTy());
  }

  if (auto *strNode = llvm::dyn_cast<StringNode>(node)) {
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

  if (llvm::isa<ImplicitCastNode>(node) || llvm::isa<CastNode>(node)) {
    const ExprNode *innerExpr = nullptr;
    const Type *targetType = nullptr;
    if (auto *implCastNode = llvm::dyn_cast<ImplicitCastNode>(node)) {
      innerExpr = implCastNode->expr;
      targetType = implCastNode->targetType;
    } else {
      auto *castNode = llvm::cast<CastNode>(node);
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

      /* Only emit the bitcast when LLVM accepts it. The unconditional
       * fallback previously produced invalid IR for record targets
       * ('bitcast ptr @.str to %String'); returning nullptr lets the caller
       * fall back to runtime (dynamic) initialization instead. */
      if (llvm::CastInst::castIsValid(llvm::Instruction::BitCast,
                                      inner->getType(), destTy)) {
        return llvm::ConstantExpr::getBitCast(inner, destTy);
      }
      return nullptr;
    }
  }

  if (auto *arrNode = llvm::dyn_cast<ArrayLiteralNode>(node)) {
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

  if (auto *boolNode = llvm::dyn_cast<BoolNode>(node)) {
    return boolNode->value ? llvm::ConstantInt::getTrue(ctx)
                           : llvm::ConstantInt::getFalse(ctx);
  }

  if (auto *num = llvm::dyn_cast<NumberNode>(node)) {
    std::string numStr(num->raw);

    bool isHex = false;
    uint8_t radix = 10;
    if (numStr.length() > 2 && numStr[0] == '0' &&
        (numStr[1] == 'x' || numStr[1] == 'X')) {
      isHex = true;
      radix = 16;
      numStr = numStr.substr(2);
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

  if (auto *charNode = llvm::dyn_cast<CharNode>(node)) {
    return llvm::ConstantInt::get(builder.getInt8Ty(), charNode->value);
  }

  if (auto *runeNode = llvm::dyn_cast<RuneNode>(node)) {
    return llvm::ConstantInt::get(builder.getInt32Ty(), runeNode->value);
  }

  if (auto *varNode = llvm::dyn_cast<VariableNode>(node)) {
    std::string lookupName = std::string(varNode->name);

    if (varNode->resolvedDecl) {
      if (auto *varDecl = llvm::dyn_cast<VarDeclNode>(varNode->resolvedDecl)) {
        if ((varDecl->isStatic || varDecl->isExtern) &&
            !varDecl->mangledName.empty()) {
          lookupName = varDecl->mangledName;
        }
      }
    }

    SymbolInfo sym = cgCtx.lookupDetailed(lookupName);

    if (sym.value) {
      if (auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(sym.value)) {
        if (gv->isConstant() && gv->hasInitializer()) {
          return gv->getInitializer();
        }
      }
      return llvm::dyn_cast<llvm::Constant>(sym.value);
    }

    return nullptr;
  }

  if (auto *maNode = llvm::dyn_cast<MemberAccessNode>(node)) {
    if (maNode->isEnumMember) {
      llvm::Type *llTy = getLLVMType(maNode->exprType);
      return llvm::ConstantInt::get(llTy, maNode->enumMember->evaluatedValue,
                                    true);
    }
    return nullptr;
  }

  if (auto *unNode = llvm::dyn_cast<UnaryOpNode>(node)) {
    if (unNode->op == "&" && llvm::isa<VariableNode>(unNode->expr)) {
      auto *varNode = llvm::cast<VariableNode>(unNode->expr);
      std::string lookupName = std::string(varNode->name);

      if (varNode->resolvedDecl) {
        if (auto *varDecl =
                llvm::dyn_cast<VarDeclNode>(varNode->resolvedDecl)) {
          if ((varDecl->isStatic || varDecl->isExtern) &&
              !varDecl->mangledName.empty()) {
            lookupName = varDecl->mangledName;
          }
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

  if (auto *binNode = llvm::dyn_cast<BinaryOpNode>(node)) {
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

          if (binNode->op == "&&")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          (vL != 0) && (vR != 0));
          if (binNode->op == "||")
            return llvm::ConstantInt::get(builder.getInt1Ty(),
                                          (vL != 0) || (vR != 0));

          auto opIt = binOpMap.find(binNode->op);
          if (opIt != binOpMap.end()) {
            switch (opIt->second) {
            case BinOpCode::Add:
              return llvm::ConstantInt::get(ctx, vL + vR);
            case BinOpCode::Sub:
              return llvm::ConstantInt::get(ctx, vL - vR);
            case BinOpCode::Mul:
              return llvm::ConstantInt::get(ctx, vL * vR);
            case BinOpCode::Div:
              if (vR.isZero())
                return nullptr;
              return llvm::ConstantInt::get(ctx, isUnsigned ? vL.udiv(vR)
                                                            : vL.sdiv(vR));
            case BinOpCode::Rem:
              if (vR.isZero())
                return nullptr;
              return llvm::ConstantInt::get(ctx, isUnsigned ? vL.urem(vR)
                                                            : vL.srem(vR));
            case BinOpCode::And:
              return llvm::ConstantInt::get(ctx, vL & vR);
            case BinOpCode::Or:
              return llvm::ConstantInt::get(ctx, vL | vR);
            case BinOpCode::Xor:
              return llvm::ConstantInt::get(ctx, vL ^ vR);
            case BinOpCode::Shl:
              return llvm::ConstantInt::get(ctx, vL.shl(vR));
            case BinOpCode::Shr:
              return llvm::ConstantInt::get(ctx, isUnsigned ? vL.lshr(vR)
                                                            : vL.ashr(vR));
            case BinOpCode::Eq:
              return llvm::ConstantInt::get(builder.getInt1Ty(), vL == vR);
            case BinOpCode::Ne:
              return llvm::ConstantInt::get(builder.getInt1Ty(), vL != vR);
            case BinOpCode::Lt:
              return llvm::ConstantInt::get(
                  builder.getInt1Ty(), isUnsigned ? vL.ult(vR) : vL.slt(vR));
            case BinOpCode::Le:
              return llvm::ConstantInt::get(
                  builder.getInt1Ty(), isUnsigned ? vL.ule(vR) : vL.sle(vR));
            case BinOpCode::Gt:
              return llvm::ConstantInt::get(
                  builder.getInt1Ty(), isUnsigned ? vL.ugt(vR) : vL.sgt(vR));
            case BinOpCode::Ge:
              return llvm::ConstantInt::get(
                  builder.getInt1Ty(), isUnsigned ? vL.uge(vR) : vL.sge(vR));
            }
          }
        }
      } else if (auto *cfpL = llvm::dyn_cast<llvm::ConstantFP>(L)) {
        if (auto *cfpR = llvm::dyn_cast<llvm::ConstantFP>(R)) {
          llvm::APFloat vL = cfpL->getValueAPF();
          llvm::APFloat vR = cfpR->getValueAPF();

          auto opIt = binOpMap.find(binNode->op);
          if (opIt != binOpMap.end()) {
            switch (opIt->second) {
            case BinOpCode::Add:
              vL.add(vR, llvm::APFloat::rmNearestTiesToEven);
              return llvm::ConstantFP::get(ctx, vL);
            case BinOpCode::Sub:
              vL.subtract(vR, llvm::APFloat::rmNearestTiesToEven);
              return llvm::ConstantFP::get(ctx, vL);
            case BinOpCode::Mul:
              vL.multiply(vR, llvm::APFloat::rmNearestTiesToEven);
              return llvm::ConstantFP::get(ctx, vL);
            case BinOpCode::Div:
              vL.divide(vR, llvm::APFloat::rmNearestTiesToEven);
              return llvm::ConstantFP::get(ctx, vL);
            case BinOpCode::Rem:
              vL.mod(vR);
              return llvm::ConstantFP::get(ctx, vL);
            case BinOpCode::Eq:
              return llvm::ConstantInt::get(builder.getInt1Ty(),
                                            vL.compare(vR) ==
                                                llvm::APFloat::cmpEqual);
            case BinOpCode::Ne:
              return llvm::ConstantInt::get(builder.getInt1Ty(),
                                            vL.compare(vR) !=
                                                llvm::APFloat::cmpEqual);
            case BinOpCode::Lt:
              return llvm::ConstantInt::get(builder.getInt1Ty(),
                                            vL.compare(vR) ==
                                                llvm::APFloat::cmpLessThan);
            case BinOpCode::Le:
              return llvm::ConstantInt::get(
                  builder.getInt1Ty(),
                  vL.compare(vR) == llvm::APFloat::cmpLessThan ||
                      vL.compare(vR) == llvm::APFloat::cmpEqual);
            case BinOpCode::Gt:
              return llvm::ConstantInt::get(builder.getInt1Ty(),
                                            vL.compare(vR) ==
                                                llvm::APFloat::cmpGreaterThan);
            case BinOpCode::Ge:
              return llvm::ConstantInt::get(
                  builder.getInt1Ty(),
                  vL.compare(vR) == llvm::APFloat::cmpGreaterThan ||
                      vL.compare(vR) == llvm::APFloat::cmpEqual);
            default:
              break;
            }
          }
        }
      }
    }
  }

  return nullptr;
}

llvm::Value *CodeGen::visit(const ImplicitCastNode *node) {
  if (!node->conversionConstructor) {
    const Type *unqualTarget = node->targetType->getUnqualifiedType();
    const Type *unqualExpr = node->expr->exprType->getUnqualifiedType();

    if (unqualTarget->getKind() == TypeKind::Struct &&
        unqualExpr->getKind() == TypeKind::Array) {
      auto *recTy = static_cast<const RecordType *>(unqualTarget);
      if (recTy->getName().find("ListLiteralView_") !=
          std::string_view::npos) {
        llvm::Type *llTy = getLLVMType(node->targetType);
        llvm::AllocaInst *temp = createEntryBlockAlloca(llTy, "llv.tmp");
        emitDefaultInitialization(temp, node->targetType);

        llvm::Value *decayedPtr = dispatch(node->expr);

        uint64_t arrSize =
            static_cast<const ArrayType *>(unqualExpr)->getSize();

        llvm::Value *dataField = builder.CreateStructGEP(llTy, temp, 0);
        builder.CreateStore(decayedPtr, dataField);

        llvm::Value *lenField = builder.CreateStructGEP(llTy, temp, 1);
        builder.CreateStore(builder.getInt64(arrSize), lenField);

        if (!node->exprType->isVoid()) {
          if (node->exprType->isReferenceType() ||
              node->exprType->getKind() == TypeKind::RValueReference) {
            return temp;
          }
          return createTBAALoad(llTy, temp, node->exprType);
        }
        return temp;
      }
    }

    if (unqualTarget->getKind() == TypeKind::Struct &&
        unqualExpr->getKind() == TypeKind::MapLiteral) {
      auto *recTy = static_cast<const RecordType *>(unqualTarget);
      if (recTy->getName().find("MapLiteralView_") !=
          std::string_view::npos) {
        auto *mapExpr = static_cast<const MapLiteralType *>(unqualExpr);
        llvm::Type *llTy = getLLVMType(node->targetType);
        llvm::AllocaInst *temp = createEntryBlockAlloca(llTy, "llv.tmp");
        emitDefaultInitialization(temp, node->targetType);

        /* The map literal was lowered to {[N x K], [N x V]}; decay both
         * arrays to pointers and store them (plus the length) in the view. */
        llvm::Value *mapPtr = dispatch(node->expr);
        uint64_t mapSize = mapExpr->getSize();
        llvm::Type *mapLLTy = getLLVMType(mapExpr);

        auto elemLL = [&](const Type *t) {
          llvm::Type *lt = getLLVMType(t);
          return lt->isVoidTy() ? builder.getInt8Ty() : lt;
        };
        llvm::Type *keysArrTy = llvm::ArrayType::get(
            elemLL(mapExpr->getKeyType()), mapSize);
        llvm::Type *valuesArrTy = llvm::ArrayType::get(
            elemLL(mapExpr->getValueType()), mapSize);

        llvm::Value *keysField =
            builder.CreateStructGEP(mapLLTy, mapPtr, 0);
        llvm::Value *decayedKeys =
            builder.CreateInBoundsGEP(keysArrTy, keysField,
                                      {builder.getInt32(0), builder.getInt32(0)});
        llvm::Value *viewKeys = builder.CreateStructGEP(llTy, temp, 0);
        builder.CreateStore(decayedKeys, viewKeys);

        llvm::Value *valuesField =
            builder.CreateStructGEP(mapLLTy, mapPtr, 1);
        llvm::Value *decayedValues =
            builder.CreateInBoundsGEP(valuesArrTy, valuesField,
                                      {builder.getInt32(0), builder.getInt32(0)});
        llvm::Value *viewValues = builder.CreateStructGEP(llTy, temp, 1);
        builder.CreateStore(decayedValues, viewValues);

        llvm::Value *lenField = builder.CreateStructGEP(llTy, temp, 2);
        builder.CreateStore(builder.getInt64(mapSize), lenField);

        if (!node->exprType->isVoid()) {
          if (node->exprType->isReferenceType() ||
              node->exprType->getKind() == TypeKind::RValueReference) {
            return temp;
          }
          return createTBAALoad(llTy, temp, node->exprType);
        }
        return temp;
      }
    }
  } else {

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
  const Type *unqualForCleanup = unqual;
  while (unqualForCleanup->getKind() == TypeKind::Array) {
    unqualForCleanup = static_cast<const ArrayType *>(unqualForCleanup)
                           ->getElementType()
                           ->getUnqualifiedType();
  }

  if (unqualForCleanup->getKind() == TypeKind::Class ||
      unqualForCleanup->getKind() == TypeKind::Struct ||
      unqualForCleanup->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqualForCleanup);
    if (auto *decl = recTy->getDeclaration()) {
      const FunctionDeclNode *dtor = nullptr;
      if (decl->kind == NodeKind::ClassDecl)
        dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::StructDecl)
        dtor = static_cast<const StructDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::UnionDecl)
        dtor = static_cast<const UnionDeclNode *>(decl)->destructor;

      if (dtor) {
        registerScopeCleanup(temp, dtor, objectType);
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
      argVal = createImplicitCast(argVal, paramTy, node->expr->exprType);
    }
  }

  argsArgs.push_back(argVal);
  emitCallOrInvoke(ctorFunc->getFunctionType(), ctorFunc, argsArgs);

  /* Track this cast's own temporary: 'node->expr' is evaluated above and
   * may leave a stale temporary from a nested expression (e.g. String
   * conversions inside a map literal) in lastTemporaryAlloca. Consumers of
   * getLValue expect it to name the temporary of THE cast itself. */
  lastTemporaryAlloca = temp;

  if (!node->exprType->isVoid()) {
    if (node->exprType->isReferenceType() ||
        node->exprType->getKind() == TypeKind::RValueReference) {
      return temp;
    }
    return createTBAALoad(getLLVMType(node->exprType), temp, node->exprType);
  }

  return temp;
  }
}

llvm::Function *CodeGen::getOrCreateGlobalInitFunc() {
  if (globalInitFunc)
    return globalInitFunc;

  llvm::FunctionType *ft = llvm::FunctionType::get(builder.getVoidTy(), false);
  std::string initName = "_GLOBAL__sub_I_" + currentFilePath;

  /* Strip invalid pathing characters from the global function linkage name */
  std::replace(initName.begin(), initName.end(), '/', '_');
  std::replace(initName.begin(), initName.end(), '.', '_');
  std::replace(initName.begin(), initName.end(), '\\', '_');

  globalInitFunc = llvm::Function::Create(
      ft, llvm::GlobalValue::InternalLinkage, initName, mod);

  llvm::BasicBlock::Create(ctx, "entry", globalInitFunc);

  if (diEmitter.isEnabled()) {
    llvm::DISubroutineType *diFuncTy =
        diEmitter.getBuilder()->createSubroutineType(
            diEmitter.getBuilder()->getOrCreateTypeArray({}));
    llvm::DISubprogram *sp = diEmitter.getBuilder()->createFunction(
        diEmitter.getFile(), initName, initName, diEmitter.getFile(), 0,
        diFuncTy, 0, llvm::DINode::FlagArtificial,
        llvm::DISubprogram::SPFlagDefinition);
    globalInitFunc->setSubprogram(sp);
  }

  /* Bind initialization routine to LLVM global constructors */
  llvm::appendToGlobalCtors(mod, globalInitFunc, 65535);
  return globalInitFunc;
}

llvm::Value *CodeGen::getLValue(const ExprNode *node) {
  if (!node)
    return nullptr;

  if (node->kind == NodeKind::FunctionCall) {
    auto *callNode = static_cast<const FunctionCallNode *>(node);
    if (callNode->exprType &&
        (callNode->exprType->isReferenceType() ||
         callNode->exprType->getKind() == TypeKind::RValueReference)) {
      /* A reference-typed call yields the address of the referenced object:
       * dispatch() normally loads it, so suppress the load here. */
      bool prev = suppressRefResultLoad;
      suppressRefResultLoad = true;
      llvm::Value *addr = dispatch(callNode);
      suppressRefResultLoad = prev;
      return addr;
    }
    if (callNode->exprType &&
        callNode->exprType->getUnqualifiedType()->isPointerType()) {
      /* A pointer-returning call (e.g. a 'T* _elem(i)' accessor) yields the
       * address directly. */
      return dispatch(callNode);
    }
  }

  if (node->kind == NodeKind::MemberAccess) {
    auto *maNode = static_cast<const MemberAccessNode *>(node);
    if (maNode->isMethodRef)
      return nullptr;

    if (maNode->isSuperAccess) {
      llvm::Value *thisAddr = lookupThis(maNode);
      if (!thisAddr)
        return nullptr;
      llvm::Value *thisPtr =
          builder.CreateLoad(builder.getPtrTy(), thisAddr, "this.val");
      const RecordType *currRec =
          currentFunc ? currentFunc->parentRecord : nullptr;
      if (auto *classTy = llvm::dyn_cast_or_null<ClassType>(currRec)) {
        if (classTy->getBaseClass()) {
          const Type *baseTy = classTy->getBaseClass()->getUnqualifiedType();
          llvm::Type *llvmBaseTy = getLLVMType(baseTy);
          return builder.CreateStructGEP(
              llvmBaseTy, thisPtr, maNode->fieldIndex, maNode->memberName);
        }
      }
    }

    if (maNode->isStaticFieldRef) {
      auto *varDecl = static_cast<const VarDeclNode *>(maNode->resolvedDecl);
      std::string gName = varDecl->mangledName.empty()
                              ? std::string(varDecl->varName)
                              : varDecl->mangledName;
      SymbolInfo sym = cgCtx.lookupDetailed(gName);
      if (!sym.value) {
        diags.report({DiagLevel::Error, node->line, node->column, node->length,
                      "Unbound static field.", currentFilePath});
        return nullptr;
      }
      return sym.value;
    }

    llvm::Value *objPtr = nullptr;
    if (maNode->object->exprType->getUnqualifiedType()->isPointerType()) {
      objPtr = dispatch(maNode->object);
    } else {
      objPtr = getLValue(maNode->object);
    }

    if (!objPtr)
      return nullptr;

    const Type *baseTy = maNode->object->exprType->getUnqualifiedType();
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

    return gep;
  }

  if (node->kind == NodeKind::TernaryOp) {
    auto *ternary = static_cast<const TernaryOpNode *>(node);
    llvm::Value *condV = dispatch(ternary->condition);
    condV = createImplicitCast(condV, builder.getInt1Ty());

    llvm::Function *theFunction = builder.GetInsertBlock()->getParent();

    const Type *lvalTy = node->exprType;
    const Type *unqualLval = lvalTy->getUnqualifiedType();
    bool lvalNeedsOwn =
        (unqualLval->getKind() == TypeKind::Struct ||
         unqualLval->getKind() == TypeKind::Class ||
         unqualLval->getKind() == TypeKind::Union);
    llvm::Value *trueGuard = nullptr;
    llvm::Value *falseGuard = nullptr;
    if (lvalNeedsOwn) {
      llvm::SmallPtrSet<const RecordType *, 8> visited;
      lvalNeedsOwn = !isTriviallyCopyable(unqualLval, visited);
      /* Initialized in the entry (which dominates both branches): an
       * uninitialized flag would be UNDEF on the other path and the
       * optimizer would assume the destructor always runs. */
      llvm::AllocaInst *tg =
          createEntryBlockAlloca(builder.getInt1Ty(), "ternary.lval.tguard");
      llvm::AllocaInst *fg =
          createEntryBlockAlloca(builder.getInt1Ty(), "ternary.lval.fguard");
      builder.CreateStore(builder.getInt1(false), tg);
      builder.CreateStore(builder.getInt1(false), fg);
      trueGuard = tg;
      falseGuard = fg;
    }

    llvm::BasicBlock *trueBB =
        llvm::BasicBlock::Create(ctx, "ternary.lval.true", theFunction);
    llvm::BasicBlock *falseBB =
        llvm::BasicBlock::Create(ctx, "ternary.lval.false");
    llvm::BasicBlock *mergeBB =
        llvm::BasicBlock::Create(ctx, "ternary.lval.merge");

    builder.CreateCondBr(condV, trueBB, falseBB);

    builder.SetInsertPoint(trueBB);
    size_t trueCleanups = cgCtx.getCleanupCount();
    llvm::Value *trueLVal = getLValue(ternary->trueExpr);
    if (!trueLVal) {
      /* Fall back to materializing the branch value in a temporary:
       * aborting here would leave the condition branch dangling. */
      llvm::Value *val = dispatch(ternary->trueExpr);
      if (!val)
        return nullptr;
      trueLVal = materializeTernaryBranchValue(val, ternary->trueExpr);
    }
    if (lvalNeedsOwn) {
      /* Own a deep copy: the branch temporaries die below, so the
       * merged lvalue cannot point into them. The copy must run while
       * the branch temporary is still alive; its cleanup is registered
       * after the branch cleanups are flushed. */
      llvm::AllocaInst *owned = createEntryBlockAlloca(
          getLLVMType(lvalTy), "ternary.lval.owned");
      emitDefaultInitialization(owned, lvalTy);
      emitMemberWiseCopy(owned, trueLVal, lvalTy, false);
      builder.CreateStore(builder.getInt1(true), trueGuard);
      trueLVal = owned;
    }
    trueLVal = createImplicitCast(trueLVal, builder.getPtrTy());
    emitBranchCleanups(trueCleanups);
    if (lvalNeedsOwn) {
      if (const FunctionDeclNode *dtor = getCustomDestructor(unqualLval)) {
        registerScopeCleanup(trueLVal, dtor, lvalTy, trueGuard);
      }
    }
    trueBB = builder.GetInsertBlock();
    builder.CreateBr(mergeBB);

    theFunction->insert(theFunction->end(), falseBB);
    builder.SetInsertPoint(falseBB);
    size_t falseCleanups = cgCtx.getCleanupCount();
    llvm::Value *falseLVal = getLValue(ternary->falseExpr);
    if (!falseLVal) {
      llvm::Value *val = dispatch(ternary->falseExpr);
      if (!val)
        return nullptr;
      falseLVal = materializeTernaryBranchValue(val, ternary->falseExpr);
    }
    if (lvalNeedsOwn) {
      llvm::AllocaInst *owned = createEntryBlockAlloca(
          getLLVMType(lvalTy), "ternary.lval.owned");
      emitDefaultInitialization(owned, lvalTy);
      emitMemberWiseCopy(owned, falseLVal, lvalTy, false);
      builder.CreateStore(builder.getInt1(true), falseGuard);
      falseLVal = owned;
    }
    falseLVal = createImplicitCast(falseLVal, builder.getPtrTy());
    emitBranchCleanups(falseCleanups);
    if (lvalNeedsOwn) {
      if (const FunctionDeclNode *dtor = getCustomDestructor(unqualLval)) {
        registerScopeCleanup(falseLVal, dtor, lvalTy, falseGuard);
      }
    }
    falseBB = builder.GetInsertBlock();
    builder.CreateBr(mergeBB);

    theFunction->insert(theFunction->end(), mergeBB);
    builder.SetInsertPoint(mergeBB);

    llvm::PHINode *phi =
        builder.CreatePHI(builder.getPtrTy(), 2, "ternary.lval.phi");
    phi->addIncoming(trueLVal, trueBB);
    phi->addIncoming(falseLVal, falseBB);
    return phi;
  }

  if (node->kind == NodeKind::ArrayLiteral) {
    /* Array literal evaluation intrinsically constructs a local temporary
     * allocation and yields the decayed pointer. Opaque pointers map this
     * gracefully as a valid l-value. */
    return dispatch(node);
  }

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

      /* By-value struct arguments must be materialized (copy/move-constructed)
       * like ordinary call arguments: passing the raw value shares the
       * temporary's storage with the callee's parameter and both destruct it. */
      bool isRefParam = false;
      const Type *paramDeclTy = nullptr;
      if (!subNode->overloadedOperator->params.empty()) {
        paramDeclTy = subNode->overloadedOperator->params[0]->type;
        isRefParam = paramDeclTy->isReferenceType() ||
                     paramDeclTy->getKind() == TypeKind::RValueReference;
      }
      llvm::Value *idxVal = nullptr;
      if (isRefParam) {
        idxVal = getLValue(subNode->index);
        if (!idxVal) {
          llvm::Value *val = dispatch(subNode->index);
          idxVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
          builder.CreateStore(val, idxVal);
        }
      } else {
        idxVal = paramDeclTy ? materializeByValueArg(subNode->index,
                                                     paramDeclTy)
                             : nullptr;
        if (!idxVal) {
          lastTemporaryAlloca = nullptr;
          idxVal = dispatch(subNode->index);
          lastTemporaryAlloca = nullptr;
        }
      }

      llvm::Type *paramTy =
          getLLVMType(subNode->overloadedOperator->params[0]->type);
      idxVal = createImplicitCast(idxVal, paramTy);
      argsArgs.push_back(idxVal);

      return emitCallOrInvoke(func->getFunctionType(), func, argsArgs);
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

    if (varNode->name == "super") {
      llvm::Value *thisAddr = lookupThis(varNode);
      if (!thisAddr)
        return nullptr;
      return builder.CreateLoad(builder.getPtrTy(), thisAddr, "super.this");
    }

    if (varNode->isField) {
      llvm::Value *thisAddr = lookupThis(varNode);
      if (!thisAddr)
        return nullptr;

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
      /* Namespace-scoped globals carry a mangled symbol (namespace-mangled,
       * like C++); look them up under that name. */
      if (!varDecl->mangledName.empty()) {
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

  if (node->kind == NodeKind::UnaryOp) {
    auto *unNode = static_cast<const UnaryOpNode *>(node);
    if (unNode->op == "*") {
      return dispatch(unNode->expr);
    }
  }

  if (llvm::isa<ImplicitCastNode>(node) || llvm::isa<CastNode>(node)) {
    /* A cast to a reference or rvalue-reference type yields the referenced
     * object's address directly; materializing it into a temporary would
     * introduce an extra indirection. */
    if (node->exprType &&
        (node->exprType->isReferenceType() ||
         node->exprType->getKind() == TypeKind::RValueReference)) {
      return dispatch(node);
    }

    /* Cast results are not naturally addressable: the cast visit builds
     * an owned temporary (tracked via lastTemporaryAlloca) — return its
     * address (e.g. for ternary lvalues or reference arguments). */
    lastTemporaryAlloca = nullptr;
    llvm::Value *val = dispatch(node);
    if (lastTemporaryAlloca) {
      llvm::Value *tmp = lastTemporaryAlloca;
      lastTemporaryAlloca = nullptr;
      return tmp;
    }
    if (!val)
      return nullptr;
    return materializeTernaryBranchValue(val, node);
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

  diEmitter.pushLexicalBlock(node, ctx);

  for (const auto *stmt : node->statements) {
    dispatch(stmt);
    if (builder.GetInsertBlock()->getTerminator()) {
      break;
    }
  }

  if (!builder.GetInsertBlock()->getTerminator()) {
    emitScopeCleanups();
  }

  diEmitter.popLexicalBlock();

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
    numStr = numStr.substr(2);
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

llvm::Value *CodeGen::visit(const TypeLiteralNode *node) {
  /* A type name is not a runtime value: sizeof/typeof/alignof read the
   * literal's representedType without dispatching it, so reaching this
   * visit means the user used a type where a value was expected. Emitting
   * 'undef' would produce a silently garbage program. */
  reportError(node->line, node->column, node->length,
              "A type name cannot be used as a runtime value (types are "
              "compile-time only; use typeof/sizeof/alignof to inspect them).");
  return nullptr;
}

llvm::Value *CodeGen::visit(const VariableNode *node) {
  if (node->name == "super") {
    llvm::Value *thisAddr = lookupThis(node);
    if (!thisAddr)
      return nullptr;
    return builder.CreateLoad(builder.getPtrTy(), thisAddr, "super.this");
  }

  if (node->isField) {
    llvm::Value *thisAddr = lookupThis(node);
    if (!thisAddr)
      return nullptr;

    llvm::Value *thisPtr =
        builder.CreateLoad(builder.getPtrTy(), thisAddr, "this.val");
    llvm::Type *llvmBaseTy = getLLVMType(node->parentType);

    if (node->parentType->getKind() == TypeKind::Union) {
      return thisPtr;
    }

    llvm::Value *gep = builder.CreateStructGEP(llvmBaseTy, thisPtr,
                                               node->fieldIndex, node->name);
    return createTBAALoad(getLLVMType(node->exprType), gep,
                          tbaaManager.getTBAATagForExpr(*this, node),
                          node->name);
  }

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
    auto *varDecl = static_cast<const VarDeclNode *>(node->resolvedDecl);
    std::string gName = varDecl->mangledName.empty()
                            ? std::string(varDecl->varName)
                            : varDecl->mangledName;
    SymbolInfo sym = cgCtx.lookupDetailed(gName);
    if (!sym.value) {
      /* The l-value path reports the same condition; a missing global here
       * is a Sema/codegen inconsistency that must not vanish into a null
       * propagated up the expression tree. */
      reportError(node->line, node->column, node->length,
                  "Static field '" + std::string(node->memberName) +
                      "' has no backing storage (declaration not emitted).");
      return nullptr;
    }
    return createTBAALoad(getLLVMType(node->exprType), sym.value,
                          tbaaManager.getTBAATagForExpr(*this, node),
                          node->memberName);
  }

  llvm::Value *objPtr = nullptr;
  if (node->isSuperAccess) {
    llvm::Value *thisAddr = lookupThis(node);
    if (!thisAddr)
      return nullptr;
    objPtr = builder.CreateLoad(builder.getPtrTy(), thisAddr, "this.val");
  } else if (node->object->exprType &&
             node->object->exprType->getUnqualifiedType()->isPointerType()) {
    objPtr = dispatch(node->object);
  } else {
    objPtr = getLValue(node->object);
  }

  if (!objPtr && !node->isSuperAccess) {
    lastTemporaryAlloca = nullptr;
    llvm::Value *val = dispatch(node->object);
    if (lastTemporaryAlloca) {
      objPtr = lastTemporaryAlloca;
      lastTemporaryAlloca = nullptr;
    } else if (val) {
      objPtr = createEntryBlockAlloca(val->getType(), "tmp.ma.recv");
      createTBAAStore(val, objPtr, node->object->exprType);
    }
  }

  if (!objPtr)
    return nullptr;

  const Type *baseTy = nullptr;
  if (node->isSuperAccess && currentFunc && currentFunc->parentRecord) {
    if (auto *classTy = llvm::dyn_cast<ClassType>(currentFunc->parentRecord)) {
      baseTy = classTy->getBaseClass()->getUnqualifiedType();
    }
  } else {
    baseTy = node->object->exprType->getUnqualifiedType();
    if (baseTy->isPointerType())
      baseTy = static_cast<const PointerType *>(baseTy)->getPointeeType();
    else if (baseTy->isReferenceType() ||
             baseTy->getKind() == TypeKind::RValueReference)
      baseTy = static_cast<const ReferenceType *>(baseTy)->getPointeeType();
  }

  llvm::Type *llvmBaseTy = getLLVMType(baseTy);
  llvm::Value *gep = objPtr;

  if (baseTy && baseTy->getKind() != TypeKind::Union) {
    gep = builder.CreateStructGEP(llvmBaseTy, objPtr, node->fieldIndex,
                                  node->memberName);
  }

  if (node->exprType->getKind() == TypeKind::Array) {
    llvm::Type *arrTy = getLLVMType(node->exprType);
    return builder.CreateInBoundsGEP(
        arrTy, gep, {builder.getInt32(0), builder.getInt32(0)});
  }

  const Type *loadTy = node->exprType;
  if (loadTy->isReferenceType() ||
      loadTy->getKind() == TypeKind::RValueReference) {
    loadTy = static_cast<const ReferenceType *>(loadTy)->getPointeeType();
  }

  llvm::Type *llvmTy = getLLVMType(loadTy);
  return createTBAALoad(llvmTy, gep, tbaaManager.getTBAATagForExpr(*this, node),
                        node->memberName);
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
      lastTemporaryAlloca = nullptr;
      llvm::Value *val = dispatch(node->expr);
      if (lastTemporaryAlloca) {
        objPtr = lastTemporaryAlloca;
        lastTemporaryAlloca = nullptr;
      } else if (val) {
        objPtr = createEntryBlockAlloca(val->getType(), "tmp.op.recv");
        createTBAAStore(val, objPtr, node->expr->exprType);
      }
    }

    llvm::Value *oldVal = nullptr;
    if (node->isPostfix && (node->op == "++" || node->op == "--")) {
      llvm::Type *valTy = getLLVMType(node->expr->exprType);
      oldVal =
          createTBAALoad(valTy, objPtr, node->expr->exprType, "postfix.old");
    }

    llvm::Function *func = getOrCreateFunction(node->overloadedOperator);
    llvm::Value *res =
        emitCallOrInvoke(func->getFunctionType(), func, {objPtr});

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

    createTBAAStore(newVal, lval,
                    tbaaManager.getTBAATagForExpr(*this, node->expr));

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
        } else if (val) {
          objPtr = createEntryBlockAlloca(val->getType(), "tmp.op.recv");
          createTBAAStore(val, objPtr, node->left->exprType);
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
          } else if (val) {
            rhsVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
            createTBAAStore(val, rhsVal, node->right->exprType);
          }
        }
      } else {
        const Type *paramDeclTy = nullptr;
        if (opFunc->params.size() > 0) {
          paramDeclTy = opFunc->params[0]->type;
        }
        lastTemporaryAlloca = nullptr;
        rhsVal = paramDeclTy ? materializeByValueArg(node->right, paramDeclTy)
                             : nullptr;
        if (!rhsVal) {
          rhsVal = dispatch(node->right);
        }
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
          } else if (val) {
            lhsVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg.l");
            createTBAAStore(val, lhsVal, node->left->exprType);
          }
        }
      } else {
        const Type *paramDeclTy = nullptr;
        if (opFunc->params.size() > 0) {
          paramDeclTy = opFunc->params[0]->type;
        }
        lastTemporaryAlloca = nullptr;
        lhsVal = paramDeclTy ? materializeByValueArg(node->left, paramDeclTy)
                             : nullptr;
        if (!lhsVal) {
          lhsVal = dispatch(node->left);
        }
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
          } else if (val) {
            rhsVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg.r");
            createTBAAStore(val, rhsVal, node->right->exprType);
          }
        }
      } else {
        const Type *paramDeclTy = nullptr;
        if (opFunc->params.size() > 1) {
          paramDeclTy = opFunc->params[1]->type;
        }
        lastTemporaryAlloca = nullptr;
        rhsVal = paramDeclTy ? materializeByValueArg(node->right, paramDeclTy)
                             : nullptr;
        if (!rhsVal) {
          rhsVal = dispatch(node->right);
        }
        lastTemporaryAlloca = nullptr;

        llvm::Type *paramTyR = func->getFunctionType()->getParamType(1);
        rhsVal = createImplicitCast(rhsVal, paramTyR);
      }
      argsArgs.push_back(rhsVal);
    }

    auto res = emitCallOrInvoke(func->getFunctionType(), func, argsArgs);
    lastTemporaryAlloca = nullptr;
    return res;
  }

  /* '&&' and '||' must short-circuit: the right operand is evaluated only
   * when the left operand does not decide the outcome. Evaluating both
   * operands eagerly keeps side effects (and, for 'j >= 0 && a[j] < key',
   * out-of-range dereferences) from firing on the skipped branch. */
  if (node->op == "&&" || node->op == "||") {
    const bool isAnd = node->op == "&&";
    llvm::Value *lhsVal = dispatch(node->left);
    if (!lhsVal) {
      diags.report({DiagLevel::Error, node->line, node->column, node->length,
                    "Failed to generate logical operand.", currentFilePath});
      return nullptr;
    }
    lhsVal = createImplicitCast(lhsVal, builder.getInt1Ty());

    llvm::Function *theFunction = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock *lhsBB = builder.GetInsertBlock();
    llvm::BasicBlock *rhsBB =
        llvm::BasicBlock::Create(ctx, "logical.rhs", theFunction);
    llvm::BasicBlock *mergeBB =
        llvm::BasicBlock::Create(ctx, "logical.merge");

    builder.CreateCondBr(lhsVal, isAnd ? rhsBB : mergeBB,
                         isAnd ? mergeBB : rhsBB);

    builder.SetInsertPoint(rhsBB);
    size_t rhsCleanups = cgCtx.getCleanupCount();
    llvm::Value *rhsVal = dispatch(node->right);
    if (!rhsVal) {
      diags.report({DiagLevel::Error, node->line, node->column, node->length,
                    "Failed to generate logical operand.", currentFilePath});
      return nullptr;
    }
    rhsVal = createImplicitCast(rhsVal, builder.getInt1Ty());
    /* The right operand's dispatch may have redirected the insert point
     * into freshly created blocks (e.g. the continuation of an invoke for
     * a call that can unwind, or a nested short-circuit). The branch to
     * merge lands in that final block, which is therefore the PHI's real
     * predecessor — recording 'rhsBB' instead would leave a PHI whose
     * predecessors do not match (LLVM IR verification failure). */
    llvm::BasicBlock *rhsExitBB = builder.GetInsertBlock();
    /* Temporaries created while evaluating the right operand (e.g. the
     * String in `s == ""`) must die at the end of this branch: when the
     * left operand short-circuits, they are never constructed, so the
     * scope-end cleanup would destroy an uninitialized object. */
    emitBranchCleanups(rhsCleanups);
    builder.CreateBr(mergeBB);

    theFunction->insert(theFunction->end(), mergeBB);
    builder.SetInsertPoint(mergeBB);
    llvm::PHINode *phi = builder.CreatePHI(builder.getInt1Ty(), 2);
    phi->addIncoming(builder.getInt1(isAnd ? 0 : 1), lhsBB);
    phi->addIncoming(rhsVal, rhsExitBB);
    lastTemporaryAlloca = nullptr;
    return phi;
  }

  llvm::Value *L = dispatch(node->left);
  llvm::Value *R = dispatch(node->right);
  lastTemporaryAlloca = nullptr;

  if (!L || !R) {
    diags.report({DiagLevel::Error, node->line, node->column, node->length,
                  "Failed to generate binary operands.", currentFilePath});
    return nullptr;
  }

  /* Pointer arithmetic: 'p + n', 'n + p', 'p - n' — the GEP element type is
   * the pointee type, so typed pointers scale by element size while byte
   * pointers (RawMemory.ptr) step by byte. */
  if (node->exprType && node->exprType->isPointerType() &&
      (node->op == "+" || node->op == "-")) {
    llvm::Value *ptrV = nullptr;
    llvm::Value *idxV = nullptr;
    if (L->getType()->isPointerTy() && R->getType()->isIntegerTy()) {
      ptrV = L;
      idxV = R;
    } else if (R->getType()->isPointerTy() && L->getType()->isIntegerTy()) {
      ptrV = R;
      idxV = L;
    }
    if (ptrV && idxV) {
      llvm::Value *idx64 =
          builder.CreateIntCast(idxV, builder.getInt64Ty(), false);
      if (node->op == "-")
        idx64 = builder.CreateNeg(idx64, "ptr.offset.neg");
      const Type *pointee =
          static_cast<const PointerType *>(
              node->exprType->getUnqualifiedType())
              ->getPointeeType()
              ->getUnqualifiedType();
      return builder.CreateInBoundsGEP(getLLVMType(pointee), ptrV, idx64,
                                       "ptr.offset");
    }
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

  auto opIt = binOpMap.find(node->op);
  if (opIt != binOpMap.end()) {
    switch (opIt->second) {
    case BinOpCode::Add:
      return isFloat ? builder.CreateFAdd(L, R) : builder.CreateAdd(L, R);
    case BinOpCode::Sub:
      return isFloat ? builder.CreateFSub(L, R) : builder.CreateSub(L, R);
    case BinOpCode::Mul:
      return isFloat ? builder.CreateFMul(L, R) : builder.CreateMul(L, R);
    case BinOpCode::Div:
      return isFloat ? builder.CreateFDiv(L, R)
                     : (isUnsigned ? builder.CreateUDiv(L, R)
                                   : builder.CreateSDiv(L, R));
    case BinOpCode::Rem:
      return isFloat ? builder.CreateFRem(L, R)
                     : (isUnsigned ? builder.CreateURem(L, R)
                                   : builder.CreateSRem(L, R));
    case BinOpCode::And:
      return builder.CreateAnd(L, R);
    case BinOpCode::Or:
      return builder.CreateOr(L, R);
    case BinOpCode::Xor:
      return builder.CreateXor(L, R);
    case BinOpCode::Shl:
      return builder.CreateShl(L, R);
    case BinOpCode::Shr:
      return isUnsigned ? builder.CreateLShr(L, R) : builder.CreateAShr(L, R);
    case BinOpCode::Eq:
      return isFloat ? builder.CreateFCmpOEQ(L, R) : builder.CreateICmpEQ(L, R);
    case BinOpCode::Ne:
      return isFloat ? builder.CreateFCmpONE(L, R) : builder.CreateICmpNE(L, R);
    case BinOpCode::Lt:
      return isFloat ? builder.CreateFCmpOLT(L, R)
                     : (isUnsigned ? builder.CreateICmpULT(L, R)
                                   : builder.CreateICmpSLT(L, R));
    case BinOpCode::Le:
      return isFloat ? builder.CreateFCmpOLE(L, R)
                     : (isUnsigned ? builder.CreateICmpULE(L, R)
                                   : builder.CreateICmpSLE(L, R));
    case BinOpCode::Gt:
      return isFloat ? builder.CreateFCmpOGT(L, R)
                     : (isUnsigned ? builder.CreateICmpUGT(L, R)
                                   : builder.CreateICmpSGT(L, R));
    case BinOpCode::Ge:
      return isFloat ? builder.CreateFCmpOGE(L, R)
                     : (isUnsigned ? builder.CreateICmpUGE(L, R)
                                   : builder.CreateICmpSGE(L, R));
    }
  }

  return nullptr;
}

llvm::Value *CodeGen::visit(const TernaryOpNode *node) {
  llvm::Value *condV = dispatch(node->condition);
  if (!condV)
    return nullptr;

  condV = createImplicitCast(condV, builder.getInt1Ty());

  llvm::Function *theFunction = builder.GetInsertBlock()->getParent();

  /* The ternary value is loaded into the phi, but the branch temporaries
   * die at their branch ends: records with destructor-bearing members
   * need an owned deep copy so the merged value does not dangle. */
  const Type *valTy = node->exprType;
  const Type *unqualVal = valTy->getUnqualifiedType();
  bool valueNeedsOwn =
      (unqualVal->getKind() == TypeKind::Struct ||
       unqualVal->getKind() == TypeKind::Class ||
       unqualVal->getKind() == TypeKind::Union);
  if (valueNeedsOwn) {
    llvm::SmallPtrSet<const RecordType *, 8> visited;
    valueNeedsOwn = !isTriviallyCopyable(unqualVal, visited);
  }

  /* Guards for the owned branch copies: initialized false so the
   * scope-end cleanups only run for the branch that actually executed. */
  llvm::AllocaInst *trueGuard =
      createEntryBlockAlloca(builder.getInt1Ty(), "ternary.true.guard");
  llvm::AllocaInst *falseGuard =
      createEntryBlockAlloca(builder.getInt1Ty(), "ternary.false.guard");
  builder.CreateStore(builder.getInt1(false), trueGuard);
  builder.CreateStore(builder.getInt1(false), falseGuard);

  llvm::BasicBlock *trueBB =
      llvm::BasicBlock::Create(ctx, "ternary.true", theFunction);
  llvm::BasicBlock *falseBB = llvm::BasicBlock::Create(ctx, "ternary.false");
  llvm::BasicBlock *mergeBB = llvm::BasicBlock::Create(ctx, "ternary.merge");

  builder.CreateCondBr(condV, trueBB, falseBB);

  builder.SetInsertPoint(trueBB);
  size_t trueCleanups = cgCtx.getCleanupCount();
  llvm::Value *trueV = dispatch(node->trueExpr);
  if (!trueV && !node->exprType->isVoid())
    return nullptr;
  if (!node->exprType->isVoid() && trueV) {
    if (valueNeedsOwn) {
      /* Deep-copy inside the branch, BEFORE the branch temporaries are
       * destroyed: the loaded value shares their buffers. The owned copy
       * is registered for destruction after the flush so it survives. */
      llvm::Value *owned = materializeTernaryBranchValue(trueV,
                                                         node->trueExpr);
      trueV = builder.CreateLoad(getLLVMType(node->exprType), owned);
      emitBranchCleanups(trueCleanups);
      if (const FunctionDeclNode *dtor =
              getCustomDestructor(unqualVal)) {
        builder.CreateStore(builder.getInt1(true), trueGuard);
        registerScopeCleanup(owned, dtor, valTy, trueGuard);
      }
    } else {
      trueV = createImplicitCast(trueV, getLLVMType(node->exprType));
      emitBranchCleanups(trueCleanups);
    }
  } else {
    emitBranchCleanups(trueCleanups);
  }
  trueBB = builder.GetInsertBlock();
  builder.CreateBr(mergeBB);

  theFunction->insert(theFunction->end(), falseBB);
  builder.SetInsertPoint(falseBB);
  size_t falseCleanups = cgCtx.getCleanupCount();
  llvm::Value *falseV = dispatch(node->falseExpr);
  if (!falseV && !node->exprType->isVoid())
    return nullptr;
  if (!node->exprType->isVoid() && falseV) {
    if (valueNeedsOwn) {
      llvm::Value *owned = materializeTernaryBranchValue(falseV,
                                                         node->falseExpr);
      falseV = builder.CreateLoad(getLLVMType(node->exprType), owned);
      emitBranchCleanups(falseCleanups);
      if (const FunctionDeclNode *dtor =
              getCustomDestructor(unqualVal)) {
        builder.CreateStore(builder.getInt1(true), falseGuard);
        registerScopeCleanup(owned, dtor, valTy, falseGuard);
      }
    } else {
      falseV = createImplicitCast(falseV, getLLVMType(node->exprType));
      emitBranchCleanups(falseCleanups);
    }
  } else {
    emitBranchCleanups(falseCleanups);
  }
  falseBB = builder.GetInsertBlock();
  builder.CreateBr(mergeBB);

  theFunction->insert(theFunction->end(), mergeBB);
  builder.SetInsertPoint(mergeBB);

  if (node->exprType->isVoid()) {
    return nullptr;
  }

  llvm::Type *resTy = getLLVMType(node->exprType);
  llvm::PHINode *phi = builder.CreatePHI(resTy, 2, "ternary.phi");
  phi->addIncoming(trueV, trueBB);
  phi->addIncoming(falseV, falseBB);

  return phi;
}

llvm::Value *CodeGen::visit(const VarDeclNode *node) {
  bool isGlobal = !builder.GetInsertBlock();
  bool isStaticLocal = !isGlobal && node->isStatic && !node->isExtern &&
                       node->mangledName.empty();

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
      if (decl->alignment > customAlign) {
        customAlign = decl->alignment;
      }
    }
  }

  if (node->alignment > customAlign) {
    customAlign = node->alignment;
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

    if (isGlobal || isStaticLocal) {
      diags.report({DiagLevel::Error, node->line, node->column, node->length,
                    "Dynamic global or static references are not supported.",
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

    diEmitter.emitLocalVariable(*this, builder, alloca, node);

    createTBAAStore(initAddr, alloca, node->type);
    cgCtx.bind(node->varName, alloca, false);

    return alloca;
  }

  llvm::Type *ty = getLLVMType(node->type);

  if (isGlobal || node->isExtern || node->isStatic) {
    llvm::Constant *initConst = nullptr;
    bool requiresDynamicInit = false;

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

      /* The constant still does not fit the global's type: the initializer
       * requires runtime construction (e.g. 'const String s = "..."', where
       * the literal decays to a pointer but the global is a record). Falling
       * back to dynamic initialization keeps the module verifier happy —
       * previously this produced an invalid 'bitcast ptr to %String'. */
      if (initConst && initConst->getType() != ty) {
        initConst = nullptr;
      }

      if (!initConst) {
        /* Local statics natively permit runtime initialization via guards.
         * Globals are mapped to module startup ctors. */
        if (isStaticLocal || isGlobal) {
          requiresDynamicInit = true;
        } else {
          diags.report({DiagLevel::Error, node->initializer->line,
                        node->initializer->column, node->initializer->length,
                        "Variable requires a compile-time "
                        "constant initializer.",
                        currentFilePath});
        }
      }
    }

    if (!initConst && !node->isExtern) {
      initConst = llvm::Constant::getNullValue(ty);
    }

    /* Const-qualified globals may only be marked immutable in LLVM IR when
     * they carry a compile-time constant initializer. Globals requiring
     * dynamic initialization are written by module startup ctors, so they
     * must remain writable (and would otherwise land in read-only sections
     * under both AOT and JIT, causing crashes). */
    bool isConstant = node->type->isConstQualified() && !requiresDynamicInit;
    std::string bindName = node->mangledName.empty()
                               ? std::string(node->varName)
                               : node->mangledName;

    /* Generate a unique collision-free symbol name for local statics */
    if (isStaticLocal) {
      bindName = currentFunc ? (std::string(currentFunc->name) + "." +
                                std::string(node->varName))
                             : bindName;
    }

    /* Keep static locals strictly private to the translation unit */
    llvm::GlobalValue::LinkageTypes linkage =
        isStaticLocal ? llvm::GlobalValue::PrivateLinkage
                      : (node->isWeak ? llvm::GlobalValue::WeakAnyLinkage
                                      : llvm::GlobalValue::ExternalLinkage);

    llvm::GlobalVariable *gvar = mod.getGlobalVariable(bindName);
    if (!gvar) {
      gvar = new llvm::GlobalVariable(mod, ty, isConstant, linkage, initConst,
                                      bindName);
    }

    if (customAlign > 0) {
      gvar->setAlignment(llvm::Align(customAlign));
    }

    diEmitter.emitGlobalVariable(*this, gvar, node, bindName);

    if (isGlobal) {
      cgCtx.bind(bindName, gvar, true);
    } else {
      /* Static class fields are looked up through their mangled name (see
       * MemberAccessNode), so bind both spellings: the unqualified name
       * covers local statics and the mangled name covers member access. */
      cgCtx.bind(node->varName, gvar, true);
      if (bindName != node->varName) {
        cgCtx.bind(bindName, gvar, true);
      }
    }

    /* Implements dynamic initialization block for local static variables.
     * Thread-safety is deferred to standard ABI guard acquisition in the
     * future. */
    if (requiresDynamicInit) {
      llvm::BasicBlock *savedInsertBlock = builder.GetInsertBlock();
      auto savedDebugLoc = builder.getCurrentDebugLocation();
      llvm::BasicBlock *contBB = nullptr;
      llvm::GlobalVariable *guardVar = nullptr;

      if (isStaticLocal) {
        std::string guardName = bindName + ".guard";

        // Guard status: 0 = Not initialized, 1 = Initializing (Lock), 2 =
        // Initialized
        guardVar = new llvm::GlobalVariable(
            mod, builder.getInt8Ty(), false, llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantInt::get(builder.getInt8Ty(), 0), guardName);

        llvm::Function *theFunction = savedInsertBlock->getParent();

        llvm::BasicBlock *checkBB =
            llvm::BasicBlock::Create(ctx, "static.check", theFunction);
        llvm::BasicBlock *spinBB =
            llvm::BasicBlock::Create(ctx, "static.spin", theFunction);
        llvm::BasicBlock *initBB =
            llvm::BasicBlock::Create(ctx, "static.init", theFunction);
        contBB = llvm::BasicBlock::Create(ctx, "static.cont", theFunction);

        builder.CreateBr(checkBB);

        builder.SetInsertPoint(checkBB);
        llvm::LoadInst *guardLoad =
            builder.CreateLoad(builder.getInt8Ty(), guardVar);
        guardLoad->setAtomic(llvm::AtomicOrdering::Acquire);

        llvm::Value *isInit =
            builder.CreateICmpEQ(guardLoad, builder.getInt8(2));
        builder.CreateCondBr(isInit, contBB, spinBB);

        builder.SetInsertPoint(spinBB);
        llvm::Value *cmpXchg = builder.CreateAtomicCmpXchg(
            guardVar, builder.getInt8(0), builder.getInt8(1),
            llvm::MaybeAlign(1), llvm::AtomicOrdering::AcquireRelease,
            llvm::AtomicOrdering::Acquire);

        llvm::Value *success = builder.CreateExtractValue(cmpXchg, 1);

        builder.CreateCondBr(success, initBB, checkBB);

        builder.SetInsertPoint(initBB);
      } else {
        llvm::Function *initF = getOrCreateGlobalInitFunc();
        builder.SetInsertPoint(&initF->back());
        diEmitter.pushScope(initF->getSubprogram());
      }

      {
        CGScopeGuard initGuard(cgCtx);

        if (baseUnqualTy->getKind() == TypeKind::Array) {
          emitArrayLiteralInit(gvar, node->type, node->initializer);
        } else {
          bool isRVO = false;

          if (node->initializer->kind == NodeKind::FunctionCall) {
            auto *callNode =
                static_cast<const FunctionCallNode *>(node->initializer);
            if (callNode->target->kind == NodeKind::Variable) {
              if (callNode->resolvedFunc && callNode->resolvedFunc->isMethod &&
                  callNode->resolvedFunc->returnType->isVoid()) {
                isRVO = true;
                emitConstructorCall(callNode, gvar);
              }
            }
          }

          if (!isRVO) {
            bool isAggregate = (baseUnqualTy->getKind() == TypeKind::Struct ||
                                baseUnqualTy->getKind() == TypeKind::Class ||
                                baseUnqualTy->getKind() == TypeKind::Union);

            if (isAggregate && node->copyCtor) {
              llvm::Value *rvalAddr = getLValue(node->initializer);
              if (!rvalAddr) {
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
                /* Copy constructors construct the object: zero the
                 * destination first so member-wise copies that route through
                 * operator= (which may free the previous buffer) are safe on
                 * the freshly allocated, otherwise uninitialized storage. */
                llvm::Align align = mod.getDataLayout().getABITypeAlign(ty);
                uint64_t allocSize = mod.getDataLayout().getTypeAllocSize(ty);
                builder.CreateMemSet(gvar, builder.getInt8(0), allocSize,
                                     align);
                llvm::Function *ctorFunc = getOrCreateFunction(node->copyCtor);
                emitCallOrInvoke(ctorFunc->getFunctionType(), ctorFunc,
                                  {gvar, rvalAddr});
              } else {
                diags.report({DiagLevel::Error, node->line, node->column,
                              node->length,
                              "Failed to resolve source for copy constructor.",
                              currentFilePath});
              }
            } else if (isAggregate) {
              llvm::Value *rvalAddr = getLValue(node->initializer);
              if (rvalAddr) {
                llvm::SmallPtrSet<const RecordType *, 8> copyVisited;
                bool trivialCopyable = isTriviallyCopyable(node->type,
                                                           copyVisited);
                if (!trivialCopyable) {
                  /* Records with destructor-bearing members must be copied
                   * member-wise: a plain memcpy would share String buffers
                   * and double-free. */
                  llvm::Align align = mod.getDataLayout().getABITypeAlign(ty);
                  uint64_t allocSize = mod.getDataLayout().getTypeAllocSize(ty);
                  builder.CreateMemSet(gvar, builder.getInt8(0), allocSize,
                                       align);
                  emitMemberWiseCopy(gvar, rvalAddr, node->type, false);
                } else {
                  llvm::Align align = mod.getDataLayout().getABITypeAlign(ty);
                  uint64_t allocSize =
                      mod.getDataLayout().getTypeAllocSize(ty);
                  builder.CreateMemCpy(gvar, align, rvalAddr, align,
                                       allocSize);
                }
              } else {
                lastTemporaryAlloca = nullptr;
                llvm::Value *initVal = dispatch(node->initializer);
                lastTemporaryAlloca = nullptr;
                if (initVal) {
                  createTBAAStore(initVal, gvar, node->type);
                } else {
                  diags.report(
                      {DiagLevel::Error, node->line, node->column, node->length,
                       "Initialization failed for variable.", currentFilePath});
                }
              }
            } else {
              lastTemporaryAlloca = nullptr;
              llvm::Value *initVal = dispatchValueOf(node->initializer);
              lastTemporaryAlloca = nullptr;
              if (initVal) {
                initVal = createImplicitCast(initVal, ty);
                createTBAAStore(initVal, gvar, node->type);
              } else {
                diags.report(
                    {DiagLevel::Error, node->line, node->column, node->length,
                     "Initialization failed for variable.", currentFilePath});
              }
            }
          }
        }

        emitScopeCleanups();
      }

      if (isStaticLocal) {
        llvm::StoreInst *guardStore =
            builder.CreateStore(builder.getInt8(2), guardVar);
        guardStore->setAtomic(llvm::AtomicOrdering::Release);

        builder.CreateBr(contBB);

        builder.SetInsertPoint(contBB);
        builder.SetCurrentDebugLocation(savedDebugLoc);
      } else {
        diEmitter.popScope();
        /* Restore the caller's debug location so instructions emitted after
         * the dynamic init block are not annotated with the init function's
         * scope. */
        builder.SetCurrentDebugLocation(savedDebugLoc);
        if (savedInsertBlock) {
          builder.SetInsertPoint(savedInsertBlock);
        } else {
          builder.ClearInsertionPoint();
        }
      }
    }

    return gvar;
  }

  llvm::AllocaInst *alloca =
      createEntryBlockAlloca(ty, std::string(node->varName));

  if (customAlign > 0) {
    alloca->setAlignment(llvm::Align(customAlign));
  }

  diEmitter.emitLocalVariable(*this, builder, alloca, node);

  cgCtx.bind(node->varName, alloca, true);

  uint64_t allocSize = mod.getDataLayout().getTypeAllocSize(ty);
  emitLifetimeStart(alloca, allocSize);
  cgCtx.addLifetime(alloca, allocSize);

  const auto *unqualTyForCleanup = node->type->getUnqualifiedType();
  while (unqualTyForCleanup->getKind() == TypeKind::Array) {
    unqualTyForCleanup = static_cast<const ArrayType *>(unqualTyForCleanup)
                             ->getElementType()
                             ->getUnqualifiedType();
  }

  if (unqualTyForCleanup->getKind() == TypeKind::Class ||
      unqualTyForCleanup->getKind() == TypeKind::Struct ||
      unqualTyForCleanup->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqualTyForCleanup);
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
      registerScopeCleanup(alloca, dtor, node->type);
    }
  }

  if (node->initializer) {
    if (baseUnqualTy->getKind() == TypeKind::Array) {
      if (node->initializer->isConstExpr &&
          node->initializer->constKey.rfind("A:", 0) == 0) {
        /* Const array literal: the variable holds the constant VALUE
         * (the canonical static backing is shared via the decayed
         * expression form). */
        if (llvm::Constant *arrConst = buildConstArrayValue(node->initializer)) {
          llvm::Align align = mod.getDataLayout().getABITypeAlign(ty);
          llvm::Value *dst = alloca;
          if (arrConst->getType() != ty) {
            /* Empty arrays use a placeholder backing; zero the slot. */
            builder.CreateMemSet(dst, builder.getInt8(0),
                                 mod.getDataLayout().getTypeAllocSize(ty),
                                 align);
          } else {
            createTBAAStore(arrConst, dst, node->type);
          }
        } else {
          emitArrayLiteralInit(alloca, node->type, node->initializer);
        }
      } else {
        emitArrayLiteralInit(alloca, node->type, node->initializer);
      }
    } else {
      bool isRVO = false;

      if (node->initializer->kind == NodeKind::FunctionCall) {
        auto *callNode =
            static_cast<const FunctionCallNode *>(node->initializer);
        if (callNode->target->kind == NodeKind::Variable) {
          if (callNode->resolvedFunc && callNode->resolvedFunc->isMethod &&
              callNode->resolvedFunc->returnType->isVoid() &&
              !callNode->isConstExpr) {
            isRVO = true;
            emitConstructorCall(callNode, alloca);
          }
        }
      }

      if (!isRVO) {
        bool isAggregate = (baseUnqualTy->getKind() == TypeKind::Struct ||
                            baseUnqualTy->getKind() == TypeKind::Class ||
                            baseUnqualTy->getKind() == TypeKind::Union);

        if (isAggregate && node->copyCtor) {
          llvm::Value *rvalAddr = getLValue(node->initializer);
          bool srcOwnsRvalue = false;
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
              /* A dispatched rvalue (e.g. a call result) owns its storage:
               * release it after the copy below. */
              srcOwnsRvalue = !node->initializer->isLValue;
            }
          }

          if (rvalAddr) {
            /* See the comment in the module-level copy-constructor path:
             * zero the destination so member-wise operator= copies are safe
             * on uninitialized storage. */
            llvm::Align align = mod.getDataLayout().getABITypeAlign(ty);
            uint64_t allocSize = mod.getDataLayout().getTypeAllocSize(ty);
            builder.CreateMemSet(alloca, builder.getInt8(0), allocSize,
                                 align);
            llvm::Function *ctorFunc = getOrCreateFunction(node->copyCtor);
            emitCallOrInvoke(ctorFunc->getFunctionType(), ctorFunc,
                            {alloca, rvalAddr});
            if (srcOwnsRvalue) {
              auto *srcRecTy =
                  static_cast<const RecordType *>(baseUnqualTy);
              auto *srcDecl = srcRecTy->getDeclaration();
              const FunctionDeclNode *srcDtor = nullptr;
              if (srcDecl) {
                if (srcDecl->kind == NodeKind::ClassDecl)
                  srcDtor =
                      static_cast<const ClassDeclNode *>(srcDecl)->destructor;
                else if (srcDecl->kind == NodeKind::StructDecl)
                  srcDtor =
                      static_cast<const StructDeclNode *>(srcDecl)->destructor;
                else if (srcDecl->kind == NodeKind::UnionDecl)
                  srcDtor =
                      static_cast<const UnionDeclNode *>(srcDecl)->destructor;
              }
              if (srcDtor && !srcDtor->isImplicit) {
                emitCleanupCall(rvalAddr, srcDtor, node->type);
              }
            }
          } else {
            diags.report({DiagLevel::Error, node->line, node->column,
                          node->length,
                          "Failed to resolve source for copy constructor.",
                          currentFilePath});
          }
        } else if (isAggregate) {
          llvm::Value *rvalAddr = getLValue(node->initializer);
          if (rvalAddr) {
            llvm::SmallPtrSet<const RecordType *, 8> copyVisited;
            bool trivialCopyable = isTriviallyCopyable(node->type,
                                                       copyVisited);
            if (!trivialCopyable) {
              /* Member-wise copy for records with destructor-bearing
               * members (deep-copies String fields instead of sharing
               * their buffers). */
              llvm::Align align = mod.getDataLayout().getABITypeAlign(ty);
              builder.CreateMemSet(alloca, builder.getInt8(0), allocSize,
                                   align);
              emitMemberWiseCopy(alloca, rvalAddr, node->type, false);
            } else {
              llvm::Align align = mod.getDataLayout().getABITypeAlign(ty);
              builder.CreateMemCpy(alloca, align, rvalAddr, align,
                                   allocSize);
            }
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
          llvm::Value *initVal = dispatchValueOf(node->initializer);
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
    /* Scalar arrays keep C-style default-init; record arrays have their
     * elements default-constructed so invariants hold before use and before
     * the scope-end destructor loop runs. */
    if (node->type->getKind() != TypeKind::Array) {
      emitDefaultInitialization(alloca, node->type);

      if (baseUnqualTy->getKind() == TypeKind::Class ||
          baseUnqualTy->getKind() == TypeKind::Struct ||
          baseUnqualTy->getKind() == TypeKind::Union) {
        auto *recTy = static_cast<const RecordType *>(baseUnqualTy);
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
            emitCallOrInvoke(ctorFunc->getFunctionType(), ctorFunc, {alloca});
          }
        }
      }
    } else {
      const Type *elemUnqual = baseUnqualTy;
      while (elemUnqual->getKind() == TypeKind::Array) {
        elemUnqual = static_cast<const ArrayType *>(elemUnqual)
                         ->getElementType()
                         ->getUnqualifiedType();
      }

      if (elemUnqual->getKind() == TypeKind::Class ||
          elemUnqual->getKind() == TypeKind::Struct ||
          elemUnqual->getKind() == TypeKind::Union) {
        auto *recTy = static_cast<const RecordType *>(elemUnqual);
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
            emitArrayDefaultConstruct(alloca, node->type, emptyCtor);
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
      const Type *paramDeclTy = nullptr;
      if (node->overloadedOperator->params.size() > 0) {
        paramDeclTy = node->overloadedOperator->params[0]->type;
      }
      lastTemporaryAlloca = nullptr;
      rhsVal =
          paramDeclTy ? materializeByValueArg(node->value, paramDeclTy)
                      : nullptr;
      if (!rhsVal) {
        rhsVal = dispatch(node->value);
      }
      lastTemporaryAlloca = nullptr;

      llvm::Type *paramTy = func->getFunctionType()->getParamType(1);
      rhsVal = createImplicitCast(rhsVal, paramTy);
    }
    argsArgs.push_back(rhsVal);

    auto res = emitCallOrInvoke(func->getFunctionType(), func, argsArgs);
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
        return createTBAALoad(
            getLLVMType(node->exprType), lval,
            tbaaManager.getTBAATagForExpr(*this, node->target));
      }
    }
  }

  const Type *unqualTargetTy = node->target->exprType->getUnqualifiedType();
  bool isAggregate = (unqualTargetTy->getKind() == TypeKind::Struct ||
                      unqualTargetTy->getKind() == TypeKind::Class ||
                      unqualTargetTy->getKind() == TypeKind::Union ||
                      unqualTargetTy->getKind() == TypeKind::Array);

  /* Aggregate assignment lowers to a memory copy (deep, member-wise, when
   * the record holds destructor-bearing members: a bit-copy would share
   * String buffers and double-free). */
  if (isAggregate) {
    llvm::Value *rvalAddr = getLValue(node->value);
    if (rvalAddr) {
      llvm::Type *llvmDestTy = getLLVMType(unqualTargetTy);
      uint64_t size = mod.getDataLayout().getTypeAllocSize(llvmDestTy);
      llvm::Align align = mod.getDataLayout().getABITypeAlign(llvmDestTy);

      llvm::SmallPtrSet<const RecordType *, 8> copyVisited;
      bool trivialCopyable = isTriviallyCopyable(unqualTargetTy, copyVisited);
      if (!trivialCopyable) {
        emitMemberWiseCopy(lval, rvalAddr, unqualTargetTy, true);
      } else {
        builder.CreateMemCpy(lval, align, rvalAddr, align, size);
      }
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

  /* Reference-typed l-values store through the pointee type: writing
   * with the reference's own (pointer) type would store 8 bytes into a
   * 4-byte slot and corrupt the stack. */
  const Type *assignTargetTy = node->target->exprType;
  if (assignTargetTy->isReferenceType()) {
    assignTargetTy = static_cast<const ReferenceType *>(assignTargetTy)
                         ->getPointeeType();
  } else if (assignTargetTy->getKind() == TypeKind::RValueReference) {
    assignTargetTy = static_cast<const RValueReferenceType *>(assignTargetTy)
                         ->getPointeeType();
  }

  if (node->op != "=") {
    llvm::Type *valTy = getLLVMType(assignTargetTy);
    llvm::Value *oldVal = createTBAALoad(valTy, lval, assignTargetTy);
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

    auto opIt = assignOpMap.find(binOp);
    if (opIt != assignOpMap.end()) {
      switch (opIt->second) {
      case AssignOpCode::Add:
        rval = isFloat ? builder.CreateFAdd(oldVal, castedRval)
                       : builder.CreateAdd(oldVal, castedRval);
        break;
      case AssignOpCode::Sub:
        rval = isFloat ? builder.CreateFSub(oldVal, castedRval)
                       : builder.CreateSub(oldVal, castedRval);
        break;
      case AssignOpCode::Mul:
        rval = isFloat ? builder.CreateFMul(oldVal, castedRval)
                       : builder.CreateMul(oldVal, castedRval);
        break;
      case AssignOpCode::Div:
        rval = isFloat ? builder.CreateFDiv(oldVal, castedRval)
                       : (isUnsigned ? builder.CreateUDiv(oldVal, castedRval)
                                     : builder.CreateSDiv(oldVal, castedRval));
        break;
      case AssignOpCode::Rem:
        rval = isFloat ? builder.CreateFRem(oldVal, castedRval)
                       : (isUnsigned ? builder.CreateURem(oldVal, castedRval)
                                     : builder.CreateSRem(oldVal, castedRval));
        break;
      case AssignOpCode::And:
        rval = builder.CreateAnd(oldVal, castedRval);
        break;
      case AssignOpCode::Or:
        rval = builder.CreateOr(oldVal, castedRval);
        break;
      case AssignOpCode::Xor:
        rval = builder.CreateXor(oldVal, castedRval);
        break;
      case AssignOpCode::Shl:
        rval = builder.CreateShl(oldVal, castedRval);
        break;
      case AssignOpCode::Shr:
        rval = isUnsigned ? builder.CreateLShr(oldVal, castedRval)
                          : builder.CreateAShr(oldVal, castedRval);
        break;
      }
    }
  } else {
    llvm::Type *destTy = getLLVMType(assignTargetTy);
    rval = createImplicitCast(rval, destTy);
  }

  createTBAAStore(rval, lval,
                  tbaaManager.getTBAATagForExpr(*this, node->target));

  return rval;
}

llvm::Value *CodeGen::visit(const ArrayLiteralNode *node) {
  llvm::Type *allocTy = getLLVMType(node->exprType);
  llvm::AllocaInst *tempArr = createEntryBlockAlloca(allocTy, "array.literal");
  emitArrayLiteralInit(tempArr, node->exprType, node);
  return builder.CreateInBoundsGEP(allocTy, tempArr,
                                   {builder.getInt32(0), builder.getInt32(0)});
}

llvm::Value *CodeGen::visit(const MapLiteralNode *node) {
  const auto *mapTy = static_cast<const MapLiteralType *>(node->exprType);
  llvm::Type *allocTy = getLLVMType(node->exprType);
  llvm::AllocaInst *tempMap = createEntryBlockAlloca(allocTy, "map.literal");

  const Type *keyTy = mapTy->getKeyType();
  const Type *valTy = mapTy->getValueType();
  auto elemLL = [&](const Type *t) {
    llvm::Type *lt = getLLVMType(t);
    return lt->isVoidTy() ? builder.getInt8Ty() : lt;
  };
  llvm::Type *keysArrTy =
      llvm::ArrayType::get(elemLL(keyTy), mapTy->getSize());
  llvm::Type *valuesArrTy =
      llvm::ArrayType::get(elemLL(valTy), mapTy->getSize());

  llvm::Value *keysField = builder.CreateStructGEP(allocTy, tempMap, 0);
  llvm::Value *valuesField = builder.CreateStructGEP(allocTy, tempMap, 1);

  for (size_t i = 0; i < node->keys.size(); i++) {
    llvm::Value *keyPtr = builder.CreateInBoundsGEP(
        keysArrTy, keysField, {builder.getInt32(0), builder.getInt32(i)});
    llvm::Value *keyVal = dispatch(node->keys[i]);
    if (!keyVal) {
      reportError(node->keys[i]->line, node->keys[i]->column,
                  node->keys[i]->length,
                  "Failed to evaluate map literal key.");
      return nullptr;
    }
    keyVal = createImplicitCast(keyVal, getLLVMType(keyTy));
    createTBAAStore(keyVal, keyPtr, keyTy);
    llvm::Value *valPtr = builder.CreateInBoundsGEP(
        valuesArrTy, valuesField, {builder.getInt32(0), builder.getInt32(i)});
    llvm::Value *valVal = dispatch(node->values[i]);
    if (!valVal) {
      reportError(node->values[i]->line, node->values[i]->column,
                  node->values[i]->length,
                  "Failed to evaluate map literal value.");
      return nullptr;
    }
    valVal = createImplicitCast(valVal, getLLVMType(valTy));
    createTBAAStore(valVal, valPtr, valTy);
  }
  return tempMap;
}

llvm::Value *CodeGen::visit(const NullNode *node) {
  return llvm::ConstantPointerNull::get(builder.getPtrTy());
}

llvm::Value *CodeGen::visit(const LambdaNode *node) {
  if (!node->synthesizedFunc || !node->synthesizedFunc->body)
    return nullptr;

  /* Emit the synthesized function once, then yield its address. */
  llvm::Function *func = getOrCreateFunction(node->synthesizedFunc);
  if (func->empty()) {
    /* Emitting a function body clears the builder's insertion point, so the
     * enclosing function's position must be restored afterwards. Likewise
     * the debug location is restored: instructions emitted by the lambda
     * would otherwise carry a location scoped to the lambda's subprogram,
     * which fails IR verification. */
    auto savedIP = builder.saveIP();
    auto savedDebugLoc = builder.getCurrentDebugLocation();
    dispatch(node->synthesizedFunc);
    builder.restoreIP(savedIP);
    builder.SetCurrentDebugLocation(savedDebugLoc);
  }
  return func;
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
  auto prevCoroInfo = std::move(coroInfo);

  /* Per-function exception-handling state. */
  ehExnSlot = nullptr;
  ehSelSlot = nullptr;
  ehResumeBlock = nullptr;
  tryDispatchStack.clear();
  tryTypeInfoStack.clear();

  funcScopeStarts.push_back(cgCtx.getAllScopes().size());

  diEmitter.emitFunctionStart(*this, func, node);

  llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", func);
  builder.SetInsertPoint(entry);

  /* Functions that may unwind get the personality routine and exception
   * tables; without it the unwinder would terminate the program when an
   * exception crosses the frame. */
  if (node->mayUnwind) {
    func->setPersonalityFn(getOrCreatePersonalityFunction());
  }

  CGScopeGuard guard(cgCtx);

  /* Async functions are lowered as coroutines: set up the frame, the future
   * state and the Future object that the ramp returns to callers. This runs
   * first so the coroutine intrinsics sit at the top of the entry block. */
  if (node->isAsync) {
    setupAsyncFunction(node, func);
  }

  unsigned astParamIdx = 0;
  auto argIt = func->arg_begin();

  if (node->isMethod && !node->isExtern && !node->isStatic &&
      node->parentRecord) {
    argIt->setName("this");
    llvm::AllocaInst *alloca =
        createEntryBlockAlloca(argIt->getType(), "this.addr");
    builder.CreateStore(&*argIt, alloca);
    cgCtx.bind("this", alloca, true);

    if (diEmitter.isEnabled()) {
      auto *diTy = diEmitter.getDIType(*this, node->parentRecord);
      auto *ptrTy = diEmitter.getBuilder()->createPointerType(diTy, 64);
      llvm::DILocalVariable *dVar =
          diEmitter.getBuilder()->createParameterVariable(
              diEmitter.getCurrentScope(), "this", 1, diEmitter.getFile(),
              node->line, ptrTy,
              llvm::DINode::FlagArtificial | llvm::DINode::FlagObjectPointer);
      diEmitter.getBuilder()->insertDeclare(
          alloca, dVar, diEmitter.getBuilder()->createExpression(),
          llvm::DILocation::get(ctx, node->line, node->column,
                                diEmitter.getCurrentScope()),
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

    unsigned argNo = astParamIdx + (node->isMethod && !node->isStatic ? 2 : 1);
    diEmitter.emitParameterVariable(*this, builder, alloca, paramDecl, argNo);

    bool isRef = paramDecl->type->isReferenceType() ||
                 paramDecl->type->getKind() == TypeKind::RValueReference;
    cgCtx.bind(pName, alloca, !isRef);

    /* By-value records with a custom destructor are owned by the callee;
     * register the destructor cleanup so RAII semantics hold for smart
     * pointers and other destructor-based types passed by value. */
    if (!isRef) {
      const Type *unqualP = paramDecl->type->getUnqualifiedType();
      if (unqualP->getKind() == TypeKind::Class ||
          unqualP->getKind() == TypeKind::Struct ||
          unqualP->getKind() == TypeKind::Union) {
        auto *recTy = static_cast<const RecordType *>(unqualP);
        if (const DeclNode *decl = recTy->getDeclaration()) {
          const FunctionDeclNode *dtor = nullptr;
          if (decl->kind == NodeKind::ClassDecl)
            dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
          else if (decl->kind == NodeKind::StructDecl)
            dtor = static_cast<const StructDeclNode *>(decl)->destructor;
          else if (decl->kind == NodeKind::UnionDecl)
            dtor = static_cast<const UnionDeclNode *>(decl)->destructor;
          if (dtor && !dtor->isImplicit) {
            registerScopeCleanup(alloca, dtor, paramDecl->type);
          }
        }
      }
    }

    astParamIdx++;
  }

  if (node->superCall) {
    dispatch(node->superCall);
  }

  /* Field initializers ('int32 _base = 1000;') run after super
   * construction and before the constructor body, which may overwrite
   * them. Without this the parser's implicit default constructor (empty
   * body) would never apply them. */
  {
    const DeclNode *decl =
        node->parentRecord ? node->parentRecord->getDeclaration() : nullptr;
    llvm::ArrayRef<VarDeclNode *> fields;
    bool isCtor = false;
    if (decl) {
      if (decl->kind == NodeKind::ClassDecl) {
        const auto *c = static_cast<const ClassDeclNode *>(decl);
        for (const auto *ctor : c->constructors) {
          if (ctor == node) {
            isCtor = true;
            break;
          }
        }
        fields = c->fields;
      } else if (decl->kind == NodeKind::StructDecl) {
        const auto *s = static_cast<const StructDeclNode *>(decl);
        for (const auto *ctor : s->constructors) {
          if (ctor == node) {
            isCtor = true;
            break;
          }
        }
        fields = s->fields;
      } else if (decl->kind == NodeKind::UnionDecl) {
        const auto *u = static_cast<const UnionDeclNode *>(decl);
        for (const auto *ctor : u->constructors) {
          if (ctor == node) {
            isCtor = true;
            break;
          }
        }
        fields = u->fields;
      }
    }

    if (isCtor) {
      SymbolInfo thisSym = cgCtx.lookupDetailed("this");
      if (thisSym.value) {
        llvm::Value *thisPtr =
            builder.CreateLoad(builder.getPtrTy(), thisSym.value, "this.val");
        llvm::Type *llRecTy = getLLVMType(node->parentRecord);
        for (auto *f : fields) {
          if (!f->initializer)
            continue;
          const FieldInfo *fInfo = node->parentRecord->getField(f->varName);
          if (!fInfo)
            continue;
          llvm::Value *gep = builder.CreateStructGEP(
              llRecTy, thisPtr, fInfo->index, f->varName);

          const Type *fUnqual = f->type->getUnqualifiedType();
          bool isAggregate = fUnqual->getKind() == TypeKind::Struct ||
                             fUnqual->getKind() == TypeKind::Class ||
                             fUnqual->getKind() == TypeKind::Union;
          if (isAggregate) {
            /* A shallow struct store would alias the initializer's
             * temporary and double-free when its cleanup runs; records
             * with destructor-bearing members are copied member-wise
             * (deep copy), like local variable initialization. */
            llvm::Value *rvalAddr = getLValue(f->initializer);
            if (!rvalAddr) {
              lastTemporaryAlloca = nullptr;
              llvm::Value *initVal = dispatch(f->initializer);
              if (lastTemporaryAlloca) {
                rvalAddr = lastTemporaryAlloca;
                lastTemporaryAlloca = nullptr;
              } else if (initVal) {
                rvalAddr = createEntryBlockAlloca(getLLVMType(f->type),
                                                  "tmp.field.init");
                createTBAAStore(initVal, rvalAddr, f->type);
              }
            }
            if (rvalAddr) {
              llvm::Align align = mod.getDataLayout().getABITypeAlign(
                  getLLVMType(f->type));
              uint64_t allocSize =
                  mod.getDataLayout().getTypeAllocSize(getLLVMType(f->type));
              builder.CreateMemSet(gep, builder.getInt8(0), allocSize, align);
              llvm::SmallPtrSet<const RecordType *, 8> copyVisited;
              if (isTriviallyCopyable(f->type, copyVisited)) {
                builder.CreateMemCpy(gep, align, rvalAddr, align, allocSize);
              } else {
                emitMemberWiseCopy(gep, rvalAddr, f->type, false);
              }
            }
          } else {
            llvm::Value *initVal = dispatch(f->initializer);
            if (!initVal)
              continue;
            initVal = createImplicitCast(initVal, getLLVMType(f->type));
            createTBAAStore(initVal, gep, f->type);
          }
        }
      }
    }

    /* 'Class(this.x)' parameters: each parameter is copied into its field
     * after the declaration initializers, matching Dart where initializing
     * formals override field initializers. Scalar and aggregate stores
     * follow the same deep-copy rules as regular assignment. */
    {
      SymbolInfo thisSym = cgCtx.lookupDetailed("this");
      if (thisSym.value) {
        llvm::Value *thisPtr =
            builder.CreateLoad(builder.getPtrTy(), thisSym.value, "this.val");
        llvm::Type *llRecTy = getLLVMType(node->parentRecord);
        for (const auto *param : node->params) {
          if (!param->isThisParam)
            continue;
          const FieldInfo *fInfo = node->parentRecord->getField(param->name);
          if (!fInfo)
            continue;
          llvm::Value *gep = builder.CreateStructGEP(
              llRecTy, thisPtr, fInfo->index, param->name);

          const Type *unqualFieldTy = fInfo->type->getUnqualifiedType();
          if (unqualFieldTy->getKind() == TypeKind::Struct ||
              unqualFieldTy->getKind() == TypeKind::Class ||
              unqualFieldTy->getKind() == TypeKind::Union) {
            SymbolInfo paramSym = cgCtx.lookupDetailed(param->name);
            if (!paramSym.value)
              continue;
            llvm::Value *rvalAddr = paramSym.value;
            if (!paramSym.isDirectAddress) {
              rvalAddr = builder.CreateLoad(builder.getPtrTy(), rvalAddr,
                                            "indirect.ref");
            }
            llvm::Align align = mod.getDataLayout().getABITypeAlign(
                getLLVMType(fInfo->type));
            uint64_t allocSize = mod.getDataLayout().getTypeAllocSize(
                getLLVMType(fInfo->type));
            builder.CreateMemSet(gep, builder.getInt8(0), allocSize, align);
            llvm::SmallPtrSet<const RecordType *, 8> copyVisited;
            if (isTriviallyCopyable(fInfo->type, copyVisited)) {
              builder.CreateMemCpy(gep, align, rvalAddr, align, allocSize);
            } else {
              emitMemberWiseCopy(gep, rvalAddr, fInfo->type, false);
            }
          } else {
            SymbolInfo paramSym = cgCtx.lookupDetailed(param->name);
            if (!paramSym.value)
              continue;
            llvm::Value *paramAddr = paramSym.value;
            if (!paramSym.isDirectAddress) {
              paramAddr = builder.CreateLoad(builder.getPtrTy(), paramAddr,
                                             "indirect.ref");
            }
            const Type *loadTy = fInfo->type;
            if (loadTy->isReferenceType() ||
                loadTy->getKind() == TypeKind::RValueReference) {
              loadTy = static_cast<const ReferenceType *>(loadTy)
                           ->getPointeeType();
            }
            llvm::Value *paramVal = createTBAALoad(
                getLLVMType(loadTy), paramAddr, fInfo->type, param->name);
            createTBAAStore(paramVal, gep, fInfo->type);
          }
        }
      }
    }

    /* Constructor initializer-list entries (': this.x = expr') run after
     * the this-parameters, in source order, still before the body. */
    for (const auto *init : node->fieldInitializers) {
      dispatch(init);
    }
  }

  /* Automatically invoke destructors for aggregate fields inside destructors */
  if (node->name == "~" && node->parentRecord) {
    /* Explicitly exclude Unions from auto-destruction logic as their active
     * state cannot be definitively resolved by the compiler. */
    if (node->parentRecord->getKind() != TypeKind::Union) {
      SymbolInfo thisSym = cgCtx.lookupDetailed("this");
      if (thisSym.value) {
        llvm::Value *thisAddr = thisSym.value;
        llvm::Value *thisPtr =
            builder.CreateLoad(builder.getPtrTy(), thisAddr, "this.val");
        llvm::Type *llvmBaseTy = getLLVMType(node->parentRecord);

        /* Because cleanups are executed in REVERSE order during
         * `emitScopeCleanups`, adding fields sequentially causes them to be
         * destructed top-down in code, but correctly executed bottom-up at
         * runtime. */
        for (const auto &f : node->parentRecord->getFields()) {
          const Type *fBaseUnqual = f.type->getUnqualifiedType();
          while (fBaseUnqual->getKind() == TypeKind::Array) {
            fBaseUnqual = static_cast<const ArrayType *>(fBaseUnqual)
                              ->getElementType()
                              ->getUnqualifiedType();
          }

          if (fBaseUnqual->getKind() == TypeKind::Class ||
              fBaseUnqual->getKind() == TypeKind::Struct) {
            auto *recTy = static_cast<const RecordType *>(fBaseUnqual);
            auto *decl = recTy->getDeclaration();
            const FunctionDeclNode *fDtor = nullptr;
            if (decl) {
              if (decl->kind == NodeKind::ClassDecl)
                fDtor = static_cast<const ClassDeclNode *>(decl)->destructor;
              else if (decl->kind == NodeKind::StructDecl)
                fDtor = static_cast<const StructDeclNode *>(decl)->destructor;
            }

            if (fDtor) {
              llvm::Value *fieldGep =
                  builder.CreateStructGEP(llvmBaseTy, thisPtr, f.index, f.name);
              registerScopeCleanup(fieldGep, fDtor, f.type);
            }
          }
        }
      }
    }
  }

  /* Async functions are lowered as coroutines: set up the frame, the future
   * state and the Future object that the ramp returns to callers. */
  dispatch(node->body);
  for (auto &bb : *func) {
    if (!bb.getTerminator()) {
      builder.SetInsertPoint(&bb);

      emitScopeCleanups();

      if (node->isAsync) {
        emitAsyncFallthroughFinish(node);
      } else if (node->name == "main" && !node->isMethod &&
                 node->returnType->isVoid()) {
        builder.CreateRet(llvm::ConstantInt::get(builder.getInt32Ty(), 0));
      } else if (func->getReturnType()->isVoidTy()) {
        builder.CreateRetVoid();
      } else {
        builder.CreateRet(llvm::UndefValue::get(func->getReturnType()));
      }
    }
  }

  /* The real C entry point: wraps the user's main so the async runtime can
   * drive the event loop after the user's code runs. */
  if (node->name == "main" && !node->isMethod && asyncEnabled) {
    emitMainWrapper(func, node);
  }

  builder.ClearInsertionPoint();
  currentFunc = prevFunc;
  funcScopeStarts.pop_back();
  coroInfo = std::move(prevCoroInfo);

  diEmitter.emitFunctionEnd();

  return func;
}

llvm::Value *CodeGen::visit(const FunctionCallNode *node) {

  /* Memory.construct<T>(ptr, args...) is lowered by the type checker into a
   * placement NewExprNode; codegen visits that instead of the call. */
  if (node->loweredNew) {
    return visit(node->loweredNew);
  }

  /* Const constructor invocation -> canonical static instance. */
  if (node->isConstExpr && !node->constKey.empty()) {
    if (llvm::Constant *c = buildConstFromSerialized(node, node->constKey,
                                                     node->exprType))
      return c;
  }

  /* Top-level (module-scope) calls have no IR builder insert point;
   * previously they crashed inside CreateGlobalString while lowering
   * their arguments. */
  if (!builder.GetInsertBlock()) {
    diags.report({DiagLevel::Error, node->line, node->column, node->length,
                  "Top-level function calls are not supported. Wrap the "
                  "expression in a function body.",
                  currentFilePath});
    return nullptr;
  }

  if (node->isSuperCall) {
    if (!node->resolvedFunc)
      return nullptr;

    llvm::Value *thisAddr = lookupThis(node);
    if (!thisAddr)
      return nullptr;
    llvm::Value *thisPtr =
        builder.CreateLoad(builder.getPtrTy(), thisAddr, "this.val");

    std::vector<llvm::Value *> argsArgs;
    argsArgs.push_back(thisPtr);

    llvm::Function *func = getOrCreateFunction(node->resolvedFunc);
    llvm::FunctionType *calleeTy = func->getFunctionType();

    unsigned llArgIdx = 1;
    unsigned astParamIdx = 0;
    for (const auto &arg : node->args) {
      llvm::Value *argVal = nullptr;

      const Type *paramDeclTy = nullptr;
      if (astParamIdx < node->resolvedFunc->params.size()) {
        paramDeclTy = node->resolvedFunc->params[astParamIdx]->type;
      }

      bool isRefParam = paramDeclTy &&
                        (paramDeclTy->isReferenceType() ||
                         paramDeclTy->getKind() == TypeKind::RValueReference);

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
        argVal = paramDeclTy ? materializeByValueArg(arg, paramDeclTy)
                             : nullptr;

        if (!argVal) {
          lastTemporaryAlloca = nullptr;
          argVal = dispatch(arg);
          lastTemporaryAlloca = nullptr;
        }
      }

      if (llArgIdx < calleeTy->getNumParams()) {
        llvm::Type *paramTy = calleeTy->getParamType(llArgIdx);
        argVal = createImplicitCast(argVal, paramTy, arg->exprType);
      }
      argsArgs.push_back(argVal);
      llArgIdx++;
      astParamIdx++;
    }

    return emitCallOrInvoke(calleeTy, func, argsArgs);
  }

  llvm::Function *func = nullptr;
  std::vector<llvm::Value *> argsArgs;

  if (node->resolvedFunc) {
    if (node->resolvedFunc->isIntrinsic) {
      if (const Intrinsic *intrinsic = IntrinsicRegistry::instance().get(
              node->resolvedFunc->intrinsicName)) {
        return intrinsic->evaluateRuntime(*this, node);
      }
    }

    func = getOrCreateFunction(node->resolvedFunc);
    llvm::Value *callee = func;
    llvm::FunctionType *calleeTy = func->getFunctionType();

    bool isConstructorViaMA = false;
    if (node->target->kind == NodeKind::MemberAccess) {
      auto ma = static_cast<const MemberAccessNode *>(node->target);
      if (ma->resolvedDecl && (ma->resolvedDecl->kind == NodeKind::ClassDecl ||
                               ma->resolvedDecl->kind == NodeKind::StructDecl ||
                               ma->resolvedDecl->kind == NodeKind::UnionDecl)) {
        isConstructorViaMA = true;
      }
    }

    if (node->target->kind == NodeKind::MemberAccess && !isConstructorViaMA) {
      auto ma = static_cast<const MemberAccessNode *>(node->target);
      if (node->resolvedFunc->isMethod && !node->resolvedFunc->isExtern &&
          !node->resolvedFunc->isStatic && node->resolvedFunc->parentRecord) {
        llvm::Value *objPtr = nullptr;
        if (ma->isSuperAccess) {
          llvm::Value *thisAddr = lookupThis(ma);
          if (!thisAddr)
            return nullptr;
          objPtr = builder.CreateLoad(builder.getPtrTy(), thisAddr,
                                      "this.val");
        } else if (ma->object->exprType->getUnqualifiedType()->isPointerType()) {
          objPtr = dispatch(ma->object);
        } else {
          objPtr = getLValue(ma->object);
        }

        if (!objPtr) {
          lastTemporaryAlloca = nullptr;
          llvm::Value *val = dispatch(ma->object);
          if (lastTemporaryAlloca) {
            objPtr = lastTemporaryAlloca;
            lastTemporaryAlloca = nullptr;
          } else if (val) {
            objPtr = createEntryBlockAlloca(val->getType(), "tmp.method.recv");
            createTBAAStore(val, objPtr, ma->object->exprType);
          }
        }

        argsArgs.push_back(objPtr);

        if (!ma->isSuperAccess &&
            (node->resolvedFunc->isVirtual || node->resolvedFunc->isOverride)) {
          const Type *baseTy = ma->object->exprType;
          if (baseTy->isPointerType()) {
            baseTy = static_cast<const PointerType *>(baseTy)->getPointeeType();
          } else if (baseTy->isReferenceType() ||
                     baseTy->getKind() == TypeKind::RValueReference) {
            if (baseTy->isReferenceType()) {
              baseTy =
                  static_cast<const ReferenceType *>(baseTy)->getPointeeType();
            } else {
              baseTy = static_cast<const RValueReferenceType *>(baseTy)
                           ->getPointeeType();
            }
          }

          llvm::Type *structLlTy = getLLVMType(baseTy);
          llvm::Value *vptrGep =
              builder.CreateStructGEP(structLlTy, objPtr, 0, "vptr.gep");
          llvm::Value *vptr =
              builder.CreateLoad(builder.getPtrTy(), vptrGep, "vptr");
          /* Slot 0 of the vtable holds the RTTI type descriptor; methods
           * start at index 1. */
          llvm::Value *methodGep = builder.CreateInBoundsGEP(
              builder.getPtrTy(), vptr,
              builder.getInt32(node->resolvedFunc->vtableIndex + 1),
              "method.gep");
          callee =
              builder.CreateLoad(builder.getPtrTy(), methodGep, "method.ptr");
        }
      }
    } else if (node->resolvedFunc->isMethod && !node->resolvedFunc->isExtern &&
               !node->resolvedFunc->isStatic &&
               node->resolvedFunc->parentRecord) {
      llvm::Type *allocTy = getLLVMType(node->exprType);
      llvm::AllocaInst *instance = createEntryBlockAlloca(allocTy, "instance");

      lastTemporaryAlloca = instance;

      emitDefaultInitialization(instance, node->exprType);

      argsArgs.push_back(instance);

      const auto *unqual = node->exprType->getUnqualifiedType();
      const Type *unqualForCleanup = unqual;
      while (unqualForCleanup->getKind() == TypeKind::Array) {
        unqualForCleanup = static_cast<const ArrayType *>(unqualForCleanup)
                               ->getElementType()
                               ->getUnqualifiedType();
      }

      if (unqualForCleanup->getKind() == TypeKind::Class ||
          unqualForCleanup->getKind() == TypeKind::Struct ||
          unqualForCleanup->getKind() == TypeKind::Union) {
        auto *recTy = static_cast<const RecordType *>(unqualForCleanup);
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
            registerScopeCleanup(instance, dtor, node->exprType);
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
        /* Passing a record by value requires materializing a proper copy (or
         * move) so destructor-based types (smart pointers, String, ...) keep
         * correct ownership semantics instead of a raw bitwise copy. */
        const Type *paramDeclTy = nullptr;
        if (node->resolvedFunc &&
            astParamIdx < node->resolvedFunc->params.size()) {
          paramDeclTy = node->resolvedFunc->params[astParamIdx]->type;
        }

        argVal = paramDeclTy ? materializeByValueArg(arg, paramDeclTy)
                             : nullptr;

        if (!argVal) {
          lastTemporaryAlloca = nullptr;
          argVal = dispatch(arg);
          lastTemporaryAlloca = nullptr;
        }

        if (func && argVal) {
          if (llArgIdx < calleeTy->getNumParams()) {
            llvm::Type *paramTy = calleeTy->getParamType(llArgIdx);
            argVal = createImplicitCast(argVal, paramTy, arg->exprType);
          } else if (func->isVarArg()) {
            argVal = lowerVariadicArg(arg, argVal);
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

    llvm::Value *callRes =
        emitCallOrInvoke(calleeTy, callee, argsArgs);

    /* Reference-typed call results yield an address; load it so dispatch
     * returns the value (matching the operator[]/subscript convention).
     * getLValue() keeps providing the raw address for l-value uses. */
    if (callRes && node->exprType &&
        (node->exprType->isReferenceType() ||
         node->exprType->getKind() == TypeKind::RValueReference) &&
        !suppressRefResultLoad) {
      const Type *refPointee = node->exprType;
      if (refPointee->isReferenceType()) {
        refPointee =
            static_cast<const ReferenceType *>(refPointee)->getPointeeType();
      } else {
        refPointee = static_cast<const RValueReferenceType *>(refPointee)
                         ->getPointeeType();
      }
      callRes = createTBAALoad(getLLVMType(refPointee), callRes, refPointee);
    }

    /* Async calls return a pointer to the Future object; wrap it into the
     * Future value type expected by the caller. The materialized temporary
     * owns the future state and is exposed as the last temporary so a
     * surrounding copy-initialization consumes it directly (without an
     * extra raw byte-copy that would share the state without retaining it). */
    if (callRes && node->resolvedFunc && node->resolvedFunc->isAsync) {
      const Type *futTy = node->resolvedFunc->effectiveReturnType;
      if (futTy) {
        lastTemporaryAlloca = nullptr;
        callRes = materializeFutureValue(futTy, callRes);
      }
    }

    if (node->resolvedFunc->isMethod &&
        node->resolvedFunc->returnType->isVoid()) {
      if (!node->exprType->isVoid()) {
        /* Constructor call: the instance alloca is the resulting object.
         * Re-point lastTemporaryAlloca at it (argument evaluation nulls it)
         * so RVO/return-escape logic can drop its cleanup when the object is
         * returned by value. */
        lastTemporaryAlloca = llvm::cast<llvm::AllocaInst>(argsArgs[0]);
        return createTBAALoad(getLLVMType(node->exprType), argsArgs[0],
                              node->exprType);
      }
    }

    /* Non-async calls: no owned temporary is produced, so clear any stale
     * flag leaked by the argument evaluation. Async calls already set the
     * flag to the materialized Future temporary above; keep it so the
     * caller's copy-initialization copies/moves from the owned object. */
    if (!node->resolvedFunc->isAsync) {
      lastTemporaryAlloca = nullptr;
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
      llvm::Value *argVal = nullptr;

      if (argIdx < llvmFTy->getNumParams()) {
        const Type *paramDeclTy = fTy->getParamTypes()[argIdx];
        bool isRefParam = paramDeclTy->isReferenceType() ||
                          paramDeclTy->getKind() == TypeKind::RValueReference;

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
          /* By-value records with a custom destructor must be copied into a
           * fresh temporary so the callee owns an independent instance;
           * bitwise transfer would make both sides free the same buffer. */
          argVal = materializeByValueArg(arg, paramDeclTy);
          if (!argVal) {
            lastTemporaryAlloca = nullptr;
            argVal = dispatch(arg);
            lastTemporaryAlloca = nullptr;
          }
        }
      }

      if (!argVal) {
        argVal = dispatch(arg);
      }

      if (argIdx < llvmFTy->getNumParams()) {
        argVal = createImplicitCast(argVal, llvmFTy->getParamType(argIdx),
                                    arg->exprType);
      } else if (llvmFTy->isVarArg()) {
        argVal = lowerVariadicArg(arg, argVal);
      }
      argsArgs.push_back(argVal);
      argIdx++;
    }

    llvm::Value *dynRes =
        emitCallOrInvoke(llvmFTy, dynamicFuncPtr, argsArgs);

    /* Reference-typed function-pointer results yield an address; load it
     * (see the direct-call path above). */
    if (dynRes && node->exprType &&
        (node->exprType->isReferenceType() ||
         node->exprType->getKind() == TypeKind::RValueReference) &&
        !suppressRefResultLoad) {
      const Type *refPointee = node->exprType;
      if (refPointee->isReferenceType()) {
        refPointee =
            static_cast<const ReferenceType *>(refPointee)->getPointeeType();
      } else {
        refPointee = static_cast<const RValueReferenceType *>(refPointee)
                         ->getPointeeType();
      }
      dynRes = createTBAALoad(getLLVMType(refPointee), dynRes, refPointee);
    }
    return dynRes;
  }
}

llvm::Value *CodeGen::visit(const CastNode *node) {
  llvm::Value *src = nullptr;

  if (node->conversionConstructor) {
    llvm::Type *llTy = getLLVMType(node->targetType);
    llvm::AllocaInst *temp = createEntryBlockAlloca(llTy, "explicit.cast.tmp");
    emitDefaultInitialization(temp, node->targetType);

    const Type *unqual = node->targetType->getUnqualifiedType();
    const Type *unqualForCleanup = unqual;
    while (unqualForCleanup->getKind() == TypeKind::Array) {
      unqualForCleanup = static_cast<const ArrayType *>(unqualForCleanup)
                             ->getElementType()
                             ->getUnqualifiedType();
    }

    if (unqualForCleanup->getKind() == TypeKind::Class ||
        unqualForCleanup->getKind() == TypeKind::Struct ||
        unqualForCleanup->getKind() == TypeKind::Union) {
      auto *recTy = static_cast<const RecordType *>(unqualForCleanup);
      if (auto *decl = recTy->getDeclaration()) {
        const FunctionDeclNode *dtor = nullptr;
        if (decl->kind == NodeKind::ClassDecl)
          dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
        else if (decl->kind == NodeKind::StructDecl)
          dtor = static_cast<const StructDeclNode *>(decl)->destructor;
        else if (decl->kind == NodeKind::UnionDecl)
          dtor = static_cast<const UnionDeclNode *>(decl)->destructor;

        if (dtor) {
          registerScopeCleanup(temp, dtor, node->targetType);
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
        argVal = createEntryBlockAlloca(val->getType(), "tmp.cast.arg");
        builder.CreateStore(val, argVal);
      }
    } else {
      argVal = dispatch(node->expr);
      if (ctorFunc && argVal && ctorFunc->arg_size() > 1) {
        llvm::Type *paramTy = ctorFunc->getFunctionType()->getParamType(1);
        argVal = createImplicitCast(argVal, paramTy, node->expr->exprType);
      }
    }

    argsArgs.push_back(argVal);
    emitCallOrInvoke(ctorFunc->getFunctionType(), ctorFunc, argsArgs);

    if (!node->exprType->isVoid()) {
      if (node->exprType->isReferenceType() ||
          node->exprType->getKind() == TypeKind::RValueReference) {
        return temp;
      }
      return createTBAALoad(getLLVMType(node->exprType), temp, node->exprType);
    }

    return temp;
  }

  if (node->targetType->isReferenceType() ||
      node->targetType->getKind() == TypeKind::RValueReference) {
    src = getLValue(node->expr);
    if (!src) {
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
  return createImplicitCast(src, destTy, node->expr->exprType);
}

llvm::Value *CodeGen::visit(const IsExprNode *node) {
  /* Statically-decided tests emit a constant. */
  if (node->staticResult >= 0) {
    return builder.getInt1(node->staticResult != 0);
  }

  llvm::Value *objPtr = nullptr;
  const Type *operandTy = node->expr->exprType->getUnqualifiedType();
  if (operandTy->isPointerType()) {
    objPtr = dispatch(node->expr);
  } else {
    /* Reference-typed operands: the lvalue is the referenced object. */
    objPtr = getLValue(node->expr);
  }
  if (!objPtr)
    return nullptr;

  llvm::Constant *targetTD = getOrCreateTypeInfo(
      llvm::cast<ClassType>(node->targetType->getUnqualifiedType()));

  llvm::Function *F = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *nullBlock = llvm::BasicBlock::Create(ctx, "is.null", F);
  llvm::BasicBlock *loopCondBlock = llvm::BasicBlock::Create(ctx, "is.cond", F);
  llvm::BasicBlock *loopBodyBlock = llvm::BasicBlock::Create(ctx, "is.body", F);
  llvm::BasicBlock *ifaceCondBlock =
      llvm::BasicBlock::Create(ctx, "is.iface.cond", F);
  llvm::BasicBlock *ifaceBodyBlock =
      llvm::BasicBlock::Create(ctx, "is.iface.body", F);
  llvm::BasicBlock *ifaceNextBlock =
      llvm::BasicBlock::Create(ctx, "is.iface.next", F);
  llvm::BasicBlock *parentBlock = llvm::BasicBlock::Create(ctx, "is.parent", F);
  llvm::BasicBlock *foundBlock = llvm::BasicBlock::Create(ctx, "is.found", F);
  llvm::BasicBlock *failBlock = llvm::BasicBlock::Create(ctx, "is.fail", F);
  llvm::BasicBlock *contBlock = llvm::BasicBlock::Create(ctx, "is.cont", F);

  /* 'null is T' is false. */
  builder.CreateCondBr(builder.CreateIsNull(objPtr), failBlock, nullBlock);

  builder.SetInsertPoint(nullBlock);
  const ClassType *operandClass = node->operandClassType
                                      ? node->operandClassType
                                      : llvm::cast<ClassType>(
                                            node->targetType->getUnqualifiedType());
  llvm::Value *vptr = builder.CreateLoad(
      builder.getPtrTy(),
      builder.CreateStructGEP(getLLVMType(operandClass), objPtr, 0,
                              "is.vptr.gep"),
      "is.vptr");
  llvm::Value *td = builder.CreateLoad(builder.getPtrTy(), vptr, "is.td");
  builder.CreateBr(loopCondBlock);

  /* Walk the dynamic type chain: descriptor, then its interfaces, then the
   * parent descriptor, until the chain ends. */
  builder.SetInsertPoint(loopCondBlock);
  llvm::PHINode *tdPhi = builder.CreatePHI(builder.getPtrTy(), 2, "is.td.phi");
  tdPhi->addIncoming(td, nullBlock);
  builder.CreateCondBr(builder.CreateIsNull(tdPhi), failBlock, loopBodyBlock);

  builder.SetInsertPoint(loopBodyBlock);
  builder.CreateCondBr(builder.CreateICmpEQ(tdPhi, targetTD), foundBlock,
                       ifaceCondBlock);

  builder.SetInsertPoint(ifaceCondBlock);
  llvm::AllocaInst *iPtr =
      createEntryBlockAlloca(builder.getInt32Ty(), "is.iface.idx");
  builder.CreateStore(builder.getInt32(1), iPtr);
  builder.CreateBr(ifaceBodyBlock);

  builder.SetInsertPoint(ifaceBodyBlock);
  llvm::Value *i = builder.CreateLoad(builder.getInt32Ty(), iPtr, "is.i");
  llvm::Value *ifaceSlot = builder.CreateInBoundsGEP(
      builder.getPtrTy(), tdPhi, i, "is.iface.gep");
  llvm::Value *ifaceTD = builder.CreateLoad(builder.getPtrTy(), ifaceSlot,
                                            "is.iface.td");
  builder.CreateCondBr(builder.CreateIsNull(ifaceTD), parentBlock,
                       ifaceNextBlock);

  builder.SetInsertPoint(ifaceNextBlock);
  llvm::Value *nextI =
      builder.CreateAdd(i, builder.getInt32(1), "is.iface.idx.next");
  builder.CreateStore(nextI, iPtr);
  builder.CreateCondBr(builder.CreateICmpEQ(ifaceTD, targetTD), foundBlock,
                       ifaceBodyBlock);

  builder.SetInsertPoint(parentBlock);
  llvm::Value *parentTd =
      builder.CreateLoad(builder.getPtrTy(),
                         builder.CreateInBoundsGEP(builder.getPtrTy(), tdPhi,
                                                   builder.getInt32(0),
                                                   "is.parent.gep"),
                         "is.parent.td");
  tdPhi->addIncoming(parentTd, parentBlock);
  builder.CreateBr(loopCondBlock);

  builder.SetInsertPoint(foundBlock);
  builder.CreateBr(contBlock);

  builder.SetInsertPoint(failBlock);
  builder.CreateBr(contBlock);

  builder.SetInsertPoint(contBlock);
  llvm::Value *result = builder.CreatePHI(builder.getInt1Ty(), 2, "is.result");
  llvm::cast<llvm::PHINode>(result)->addIncoming(builder.getTrue(), foundBlock);
  llvm::cast<llvm::PHINode>(result)->addIncoming(builder.getFalse(), failBlock);

  if (node->isNegated) {
    result = builder.CreateXor(result, builder.getTrue(), "is.negated");
  }
  return result;
}

/* Exception handling: try / catch / throw */

llvm::Value *CodeGen::visit(const TryStmtNode *node) {
  llvm::Function *fn = builder.GetInsertBlock()->getParent();

  llvm::BasicBlock *mergeBB = llvm::BasicBlock::Create(ctx, "try.merge", fn);
  llvm::BasicBlock *dispatchBB =
      llvm::BasicBlock::Create(ctx, "try.dispatch", fn);

  std::vector<llvm::BasicBlock *> handlerBBs;
  handlerBBs.reserve(node->clauses.size());
  for (size_t i = 0; i < node->clauses.size(); ++i) {
    handlerBBs.push_back(
        llvm::BasicBlock::Create(ctx, "try.catch." + std::to_string(i), fn));
  }

  /* Catch clauses of this try, shared by every per-invoke landing pad of
   * the try body. */
  std::vector<llvm::Constant *> typeInfos;
  typeInfos.reserve(node->clauses.size());
  for (const auto *clause : node->clauses) {
    llvm::Constant *ti = clause->isCatchAll
                             ? llvm::ConstantPointerNull::get(
                                   builder.getPtrTy())
                             : getOrCreateTypeInfoForType(clause->catchType);
    typeInfos.push_back(ti);
  }

  /* The try body runs with the try marked active. A bare 'throw;' inside a
   * handler reloads the current exception from the shared EH slot. */
  cgCtx.pushScope();
  cgCtx.setCatchPad(dispatchBB);
  cgCtx.bind("$catch_exn", getOrCreateEHExnSlot());
  tryDispatchStack.push_back(dispatchBB);
  tryTypeInfoStack.push_back(typeInfos);

  dispatch(node->body);

  if (!builder.GetInsertBlock()->getTerminator()) {
    builder.CreateBr(mergeBB);
  }

  /* The handlers are not covered by the try's own region: exceptions
   * raised inside them (a rethrow, a new throw, or a throwing call)
   * propagate to the enclosing trys, so the innermost dispatch is popped
   * before the handlers run. */
  cgCtx.clearCatchPad();
  tryDispatchStack.pop_back();
  tryTypeInfoStack.pop_back();

  if (dispatchBB->hasNPredecessorsOrMore(1)) {
    /* Dispatch on the selector: it equals llvm.eh.typeid.for(@T) when the
     * T clause matched, and 0 when a catch-all matched. */
    builder.SetInsertPoint(dispatchBB);
    llvm::Value *sel =
        builder.CreateLoad(builder.getInt32Ty(), getOrCreateEHSelSlot(),
                           "try.disp.sel");
    llvm::Function *typeidFn = llvm::Intrinsic::getDeclaration(
        &mod, llvm::Intrinsic::eh_typeid_for, {builder.getPtrTy()});
    int catchAllHandler = -1;
    for (size_t i = 0; i < node->clauses.size(); ++i) {
      if (node->clauses[i]->isCatchAll) {
        catchAllHandler = static_cast<int>(i);
        break;
      }
    }

    /* No clause of this try matched: an enclosing try may still catch the
     * exception, so the chain continues into its dispatch (or resumes when
     * this is the outermost try). The try's own entry was popped before
     * its dispatch is emitted, so the top of the stack is the enclosing
     * dispatch. */
    auto outerTarget = [&]() -> llvm::BasicBlock * {
      if (!tryDispatchStack.empty())
        return tryDispatchStack.back();
      return getOrCreateEHResumeBlock();
    };

    for (size_t i = 0; i < node->clauses.size(); ++i) {
      if (node->clauses[i]->isCatchAll)
        continue;

      llvm::Value *tid =
          builder.CreateCall(typeidFn, {typeInfos[i]}, "try.tid");
      llvm::Value *match = builder.CreateICmpEQ(sel, tid, "try.match");

      /* Whether a later (non-catch-all) clause still needs comparing. */
      bool isLastTypeClause = true;
      for (size_t j = i + 1; j < node->clauses.size(); ++j) {
        if (!node->clauses[j]->isCatchAll) {
          isLastTypeClause = false;
          break;
        }
      }

      if (isLastTypeClause) {
        llvm::BasicBlock *elseTarget =
            catchAllHandler >= 0 ? handlerBBs[catchAllHandler] : outerTarget();
        builder.CreateCondBr(match, handlerBBs[i], elseTarget);
      } else {
        llvm::BasicBlock *nextBB =
            llvm::BasicBlock::Create(ctx, "try.sel.next", fn);
        builder.CreateCondBr(match, handlerBBs[i], nextBB);
        builder.SetInsertPoint(nextBB);
      }
    }

    /* No type clause matched: run this try's catch-all handler when it has
     * one, otherwise continue into the enclosing try's dispatch. */
    if (!builder.GetInsertBlock()->getTerminator()) {
      builder.CreateBr(catchAllHandler >= 0 ? handlerBBs[catchAllHandler]
                                            : outerTarget());
    }

    /* Each handler: enter the catch, initialize the binding variable from
     * the thrown value, run the body and leave the catch. */
    llvm::Function *beginCatch = getOrCreateRuntimeFunction(
        "utopia_begin_catch",
        llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy()},
                                false));
    llvm::Function *endCatch = getOrCreateRuntimeFunction(
        "utopia_end_catch",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false));

    for (size_t i = 0; i < node->clauses.size(); ++i) {
      const auto *clause = node->clauses[i];
      builder.SetInsertPoint(handlerBBs[i]);

      llvm::Value *exn = builder.CreateLoad(builder.getPtrTy(),
                                            getOrCreateEHExnSlot(),
                                            "try.handler.exn");
      llvm::Value *valuePtr =
          builder.CreateCall(beginCatch, {exn}, "catch.value");

      /* The handler scope holds the binding variable and the end_catch
       * cleanup, so both run however the body exits (normally, return,
       * break, or a nested throw). */
      cgCtx.pushScope();

      if (!clause->isCatchAll && !clause->varName.empty()) {
        const Type *catchTy = clause->catchType;
        if (catchTy->isReferenceType() ||
            catchTy->getKind() == TypeKind::RValueReference) {
          /* A reference catch binds directly to the thrown object. */
          cgCtx.bind(clause->varName, valuePtr);
        } else {
          llvm::Type *llTy = getLLVMType(catchTy);
          llvm::AllocaInst *var =
              createEntryBlockAlloca(llTy, std::string(clause->varName));
          if (clause->copyCtor) {
            /* Zero the destination first: copy constructors assign their
             * members (e.g. String operator= frees the previous buffer),
             * which is unsafe on uninitialized storage. */
            emitDefaultInitialization(var, catchTy);
            llvm::Function *cc = getOrCreateFunction(clause->copyCtor);
            emitCallOrInvoke(cc->getFunctionType(), cc, {var, valuePtr});
          } else {
            llvm::Value *v = createTBAALoad(llTy, valuePtr, catchTy,
                                            "catch.val");
            createTBAAStore(v, var, catchTy);
          }
          cgCtx.bind(clause->varName, var);
          if (const FunctionDeclNode *dtor =
                  getCustomDestructor(catchTy)) {
            registerScopeCleanup(var, dtor, catchTy);
          }
        }
      }

      cgCtx.addCleanup(exn, nullptr, nullptr, nullptr, endCatch);

      dispatch(clause->body);

      if (!builder.GetInsertBlock()->getTerminator()) {
        emitScopeCleanups();
        builder.CreateBr(mergeBB);
      }
      cgCtx.popScope();
    }
  } else {
    /* Nothing in the try body can throw: the dispatch and handlers are
     * unreachable. */
    dispatchBB->eraseFromParent();
    for (llvm::BasicBlock *bb : handlerBBs)
      bb->eraseFromParent();
  }

  cgCtx.popScope();

  builder.SetInsertPoint(mergeBB);
  return nullptr;
}

llvm::Value *CodeGen::visit(const ThrowStmtNode *node) {
  if (node->isRethrow) {
    llvm::Value *slot = cgCtx.lookup("$catch_exn");
    llvm::Value *exn = builder.CreateLoad(builder.getPtrTy(), slot,
                                          "rethrow.exn");
    llvm::Function *rethrowFn = getOrCreateRuntimeFunction(
        "utopia_rethrow",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false));
    emitCallOrInvoke(rethrowFn->getFunctionType(), rethrowFn, {exn});
    builder.CreateUnreachable();
    return nullptr;
  }

  /* The thrown object is a copy of the expression's value; records with a
   * custom destructor are copy-constructed into the exception storage, all
   * other types are bit-copied. Arrays decay to pointers (C++ rules). */
  const Type *thrownTy = node->value->exprType;
  if (thrownTy->getUnqualifiedType()->getKind() == TypeKind::Array) {
    thrownTy = astCtx.getPointerType(
        static_cast<const ArrayType *>(thrownTy->getUnqualifiedType())
            ->getElementType());
  } else if (thrownTy->isReferenceType()) {
    thrownTy = static_cast<const ReferenceType *>(thrownTy)->getPointeeType();
  } else if (thrownTy->getKind() == TypeKind::RValueReference) {
    thrownTy = static_cast<const RValueReferenceType *>(thrownTy)
                   ->getPointeeType();
  }
  const Type *unqualTy = thrownTy->getUnqualifiedType();

  llvm::Type *llTy = getLLVMType(thrownTy);
  uint64_t size = mod.getDataLayout().getTypeAllocSize(llTy);

  /* Locate the source value (l-value in place, r-values through the
   * temporary machinery). */
  llvm::Value *src = getLValue(node->value);
  if (!src) {
    lastTemporaryAlloca = nullptr;
    llvm::Value *val = dispatch(node->value);
    if (lastTemporaryAlloca) {
      src = lastTemporaryAlloca;
      lastTemporaryAlloca = nullptr;
    } else if (val) {
      src = createEntryBlockAlloca(llTy, "throw.tmp");
      createTBAAStore(val, src, thrownTy);
    }
  }
  if (!src)
    return nullptr;

  llvm::Function *allocFn = getOrCreateRuntimeFunction(
      "utopia_allocate_exception",
      llvm::FunctionType::get(builder.getPtrTy(), {builder.getInt64Ty()},
                              false));
  llvm::Value *slot = builder.CreateCall(
      allocFn, {builder.getInt64(size)}, "throw.slot");

  if (node->copyCtor) {
    /* Zero the destination first: copy constructors assign their members
     * (e.g. String operator= frees the previous buffer), which is unsafe
     * on the uninitialized exception storage. */
    llvm::Align align = mod.getDataLayout().getABITypeAlign(llTy);
    builder.CreateMemSet(slot, builder.getInt8(0), size, align);
    llvm::Function *cc = getOrCreateFunction(node->copyCtor);
    emitCallOrInvoke(cc->getFunctionType(), cc, {slot, src});
  } else {
    llvm::Align align = mod.getDataLayout().getABITypeAlign(llTy);
    builder.CreateMemCpy(slot, align, src, align,
                         llvm::ConstantInt::get(builder.getInt64Ty(), size));
  }

  const FunctionDeclNode *dtor = getCustomDestructor(unqualTy);
  llvm::Function *throwFn = getOrCreateRuntimeFunction(
      "utopia_throw",
      llvm::FunctionType::get(builder.getVoidTy(),
                              {builder.getPtrTy(), builder.getPtrTy(),
                               builder.getPtrTy()},
                              false));
  llvm::Constant *typeInfo = getOrCreateTypeInfoForType(thrownTy);
  llvm::Value *dtorFn = dtor
                            ? static_cast<llvm::Value *>(getOrCreateFunction(dtor))
                            : static_cast<llvm::Value *>(
                                  llvm::ConstantPointerNull::get(
                                      builder.getPtrTy()));
  emitCallOrInvoke(throwFn->getFunctionType(), throwFn,
                   {slot, typeInfo, dtorFn});
  builder.CreateUnreachable();
  return nullptr;
}

llvm::Value *CodeGen::visit(const AssertStmtNode *node) {
  if (node->isNoOp)
    return nullptr;

  llvm::Value *cond = dispatch(node->condition);
  if (!cond)
    return nullptr;

  llvm::Function *fn = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *okBB = llvm::BasicBlock::Create(ctx, "assert.ok", fn);
  llvm::BasicBlock *failBB = llvm::BasicBlock::Create(ctx, "assert.fail", fn);
  builder.CreateCondBr(cond, okBB, failBB);

  builder.SetInsertPoint(failBB);
  llvm::Value *fileStr =
      builder.CreateGlobalStringPtr(currentFilePath, ".assert.file");
  llvm::Function *failFn = getOrCreateRuntimeFunction(
      "utopia_assert_failed",
      llvm::FunctionType::get(builder.getVoidTy(),
                              {builder.getPtrTy(), builder.getInt32Ty(),
                               builder.getPtrTy()},
                              false));
  builder.CreateCall(failFn,
                     {fileStr, builder.getInt32(node->line),
                      llvm::ConstantPointerNull::get(builder.getPtrTy())});
  builder.CreateUnreachable();

  builder.SetInsertPoint(okBB);
  return nullptr;
}

llvm::Value *CodeGen::visit(const ReturnNode *node) {
  /* Async functions store the value into the future state and complete the
   * future instead of returning directly. */
  if (coroInfo && currentFunc && currentFunc->isAsync) {
    /* 'return fut;': the returned expression is a Future<T> of the value
     * type; await it first and complete the enclosing future afterwards. */
    if (node->implicitAwait) {
      const Type *futValueTy = nullptr;
      if (!unwrapFutureType(node->value->exprType, &futValueTy)) {
        diags.report({DiagLevel::Error, node->line, node->column, node->length,
                      "Failed to evaluate implicit await return.",
                      currentFilePath});
        return nullptr;
      }

      llvm::Value *futObj = getFutureObjectPointer(node->value);
      if (!futObj) {
        diags.report({DiagLevel::Error, node->line, node->column, node->length,
                      "Failed to evaluate return expression.",
                      currentFilePath});
        return nullptr;
      }
      llvm::Value *srcState =
          getFutureState(futObj, node->value->exprType);

      llvm::Function *func = builder.GetInsertBlock()->getParent();
      llvm::BasicBlock *doneBB =
          llvm::BasicBlock::Create(ctx, "ret.done", func);
      llvm::BasicBlock *regBB =
          llvm::BasicBlock::Create(ctx, "ret.register", func);
      llvm::BasicBlock *resumedBB =
          llvm::BasicBlock::Create(ctx, "ret.resumed", func);
      llvm::BasicBlock *afterBB =
          llvm::BasicBlock::Create(ctx, "ret.after", func);

      llvm::Value *done32 = emitRuntimeCall("utopia_future_is_completed",
                                            builder.getInt32Ty(), {srcState});
      llvm::Value *done =
          builder.CreateICmpNE(done32, builder.getInt32(0), "ret.done");
      builder.CreateCondBr(done, doneBB, regBB);

      builder.SetInsertPoint(doneBB);
      builder.CreateBr(afterBB);

      builder.SetInsertPoint(regBB);
      llvm::Value *hdl =
          builder.CreateLoad(builder.getPtrTy(), coroInfo->frameSlot,
                             "coro.frame");
      llvm::Value *resumeFn =
          builder.CreateLoad(builder.getPtrTy(), hdl, "coro.resume.fn");
      emitRuntimeCall("utopia_future_then", builder.getVoidTy(),
                      {srcState, resumeFn, hdl});

      llvm::Function *coroSuspendFn = llvm::Intrinsic::getDeclaration(
          &mod, llvm::Intrinsic::coro_suspend);
      llvm::Value *s = builder.CreateCall(
          coroSuspendFn, {coroInfo->coroId, builder.getInt1(false)});
      llvm::SwitchInst *sw =
          builder.CreateSwitch(s, coroInfo->suspendBlock, 2);
      sw->addCase(builder.getInt8(0), resumedBB);
      sw->addCase(builder.getInt8(1), coroInfo->cleanupBlock);

      builder.SetInsertPoint(resumedBB);
      builder.CreateBr(afterBB);

      builder.SetInsertPoint(afterBB);
      llvm::Value *state = builder.CreateLoad(
          builder.getPtrTy(), coroInfo->futureStateSlot, "future.state");
      llvm::Value *retVal = readFutureValue(srcState, futValueTy);
      writeFutureValueInto(state, retVal, coroInfo->valueType, false);

      size_t scopeStart = funcScopeStarts.empty() ? 0 : funcScopeStarts.back();
      auto allScopes = cgCtx.getAllScopes();
      for (size_t si = scopeStart; si < allScopes.size(); ++si) {
        const auto &scope = allScopes[si];
        for (auto cleanupIt = scope.cleanups.rbegin();
             cleanupIt != scope.cleanups.rend(); ++cleanupIt) {
          emitCleanupCall(cleanupIt->instancePtr, cleanupIt->destructor,
                          cleanupIt->type, cleanupIt->guard,
                          cleanupIt->runtimeFn);
        }
        for (auto lifeIt = scope.lifetimes.rbegin();
             lifeIt != scope.lifetimes.rend(); ++lifeIt) {
          emitLifetimeEnd(lifeIt->allocaInst, lifeIt->size);
        }
      }

      emitAsyncReturn(currentFunc, nullptr, false);
      return nullptr;
    }

    llvm::Value *retVal = nullptr;
    bool isLValue = false;
    if (node->value) {
      bool isRefReturn =
          (coroInfo->valueType->isReferenceType() ||
           coroInfo->valueType->getKind() == TypeKind::RValueReference);

      if (isRefReturn) {
        retVal = getLValue(node->value);
        if (!retVal) {
          diags.report({DiagLevel::Error, node->line, node->column,
                        node->length,
                        "Unresolved l-value in reference return.",
                        currentFilePath});
          return nullptr;
        }
        isLValue = true;
      } else {
        /* Prefer the l-value so the value can be moved out of the source
         * object with proper ownership semantics. */
        retVal = getLValue(node->value);
        if (retVal) {
          isLValue = true;
        } else {
          lastTemporaryAlloca = nullptr;
          retVal = dispatch(node->value);
          if (!retVal) {
            diags.report({DiagLevel::Error, node->line, node->column,
                          node->length,
                          "Failed to evaluate return expression.",
                          currentFilePath});
            return nullptr;
          }
          if (lastTemporaryAlloca) {
            retVal = lastTemporaryAlloca;
            lastTemporaryAlloca = nullptr;
            isLValue = true;
          }
        }
      }
    }

    /* The value is stored into the future BEFORE the scope cleanups run,
     * otherwise returning a local would copy from already-destroyed
     * storage. */
    llvm::Value *state = builder.CreateLoad(
        builder.getPtrTy(), coroInfo->futureStateSlot, "future.state");
    if (retVal) {
      writeFutureValueInto(state, retVal, coroInfo->valueType, isLValue);
    }

    size_t scopeStart = funcScopeStarts.empty() ? 0 : funcScopeStarts.back();
    auto allScopes = cgCtx.getAllScopes();
    for (size_t si = scopeStart; si < allScopes.size(); ++si) {
      const auto &scope = allScopes[si];
      for (auto cleanupIt = scope.cleanups.rbegin();
           cleanupIt != scope.cleanups.rend(); ++cleanupIt) {
        emitCleanupCall(cleanupIt->instancePtr, cleanupIt->destructor,
                        cleanupIt->type, cleanupIt->guard,
                        cleanupIt->runtimeFn);
      }
      for (auto lifeIt = scope.lifetimes.rbegin();
           lifeIt != scope.lifetimes.rend(); ++lifeIt) {
        emitLifetimeEnd(lifeIt->allocaInst, lifeIt->size);
      }
    }

    emitAsyncReturn(currentFunc, nullptr, false);
    return nullptr;
  }

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

      /* RVO / Return Escape: a returned local (or temporary) is moved into
       * an owned return value so ownership transfers without a deep copy,
       * while the source keeps its cleanup and destructs as a moved-from
       * object (freeing nothing). Removing the cleanup instead would orphan
       * the buffers whenever the returned value is deep-copied by an
       * intermediate caller (e.g. 'return a() + b();'). */
      bool returnEscaped = false;
      if (node->value->kind == NodeKind::Variable) {
        if (llvm::Value *lval = getLValue(node->value)) {
          const Type *retUnqual =
              currentFunc ? currentFunc->returnType->getUnqualifiedType()
                          : nullptr;
          if (retUnqual &&
              (retUnqual->getKind() == TypeKind::Struct ||
               retUnqual->getKind() == TypeKind::Class ||
               retUnqual->getKind() == TypeKind::Union)) {
            if (const FunctionDeclNode *mv =
                    findCopyOrMoveCtor(retUnqual, true)) {
              llvm::AllocaInst *owned = createEntryBlockAlloca(
                  getLLVMType(retUnqual), "ret.owned");
              emitDefaultInitialization(owned, retUnqual);
              llvm::Function *mvFunc = getOrCreateFunction(mv);
              builder.CreateCall(mvFunc, {owned, lval});
              retVal = builder.CreateLoad(getLLVMType(retUnqual), owned);
              returnEscaped = true;
            }
          }
        }
      } else if (lastTemporaryAlloca) {
        /* A temporary (e.g. a cast result): move out of it so ownership
         * transfers, and keep its cleanup (the moved-from temporary frees
         * nothing). */
        const Type *retUnqual =
            currentFunc ? currentFunc->returnType->getUnqualifiedType()
                        : nullptr;
        if (retUnqual &&
            (retUnqual->getKind() == TypeKind::Struct ||
             retUnqual->getKind() == TypeKind::Class ||
             retUnqual->getKind() == TypeKind::Union)) {
          if (const FunctionDeclNode *mv =
                  findCopyOrMoveCtor(retUnqual, true)) {
            llvm::AllocaInst *owned = createEntryBlockAlloca(
                getLLVMType(retUnqual), "ret.owned");
            emitDefaultInitialization(owned, retUnqual);
            llvm::Function *mvFunc = getOrCreateFunction(mv);
            builder.CreateCall(mvFunc, {owned, lastTemporaryAlloca});
            retVal = builder.CreateLoad(getLLVMType(retUnqual), owned);
            returnEscaped = true;
          }
        }
        if (!returnEscaped) {
          cgCtx.removeCleanup(lastTemporaryAlloca);
        }
        lastTemporaryAlloca = nullptr;
      }

      /* Any other expression (ternaries, calls, ...) yields a value that
       * may share storage with an object destroyed by the scope cleanups
       * below. Rvalues (e.g. call results) OWN their storage: move them so
       * the buffers are not orphaned. Lvalues (e.g. 'obj.field') may share
       * storage with a scope object: deep-copy into an unowned temporary so
       * the returned value owns its buffers and the scope object stays
       * intact (the caller's copy frees them). */
      if (!returnEscaped) {
        const Type *retUnqual =
            currentFunc ? currentFunc->returnType->getUnqualifiedType()
                        : nullptr;
        if (retUnqual && (retUnqual->getKind() == TypeKind::Struct ||
                          retUnqual->getKind() == TypeKind::Class ||
                          retUnqual->getKind() == TypeKind::Union)) {
          llvm::SmallPtrSet<const RecordType *, 8> visited;
          if (!isTriviallyCopyable(retUnqual, visited)) {
            llvm::AllocaInst *owned = createEntryBlockAlloca(
                getLLVMType(retUnqual), "ret.owned");
            if (!node->value->isLValue) {
              if (const FunctionDeclNode *mv =
                      findCopyOrMoveCtor(retUnqual, true)) {
                llvm::AllocaInst *srcTmp = createEntryBlockAlloca(
                    getLLVMType(retUnqual), "ret.owned.src");
                createTBAAStore(retVal, srcTmp, retUnqual);
                emitDefaultInitialization(owned, retUnqual);
                llvm::Function *mvFunc = getOrCreateFunction(mv);
                builder.CreateCall(mvFunc, {owned, srcTmp});
                retVal = builder.CreateLoad(getLLVMType(retUnqual), owned);
                returnEscaped = true;
              }
            }
            if (!returnEscaped) {
              llvm::AllocaInst *srcTmp = createEntryBlockAlloca(
                  getLLVMType(retUnqual), "ret.owned.src");
              createTBAAStore(retVal, srcTmp, retUnqual);
              emitMemberWiseCopy(owned, srcTmp, retUnqual, false);
              retVal = builder.CreateLoad(getLLVMType(retUnqual), owned);
            }
          }
        }
      }
    }
  }

  /* Emit cleanups and lifetime ends only for scopes belonging to the current
   * function; enclosing functions' scopes (e.g. when emitting a lambda's
   * synthesized function) must not be closed here. */
  size_t scopeStart = funcScopeStarts.empty() ? 0 : funcScopeStarts.back();
  auto allScopes = cgCtx.getAllScopes();
  for (size_t si = scopeStart; si < allScopes.size(); ++si) {
    const auto &scope = allScopes[si];
    for (auto cleanupIt = scope.cleanups.rbegin();
         cleanupIt != scope.cleanups.rend(); ++cleanupIt) {
      emitCleanupCall(cleanupIt->instancePtr, cleanupIt->destructor,
                      cleanupIt->type, cleanupIt->guard, cleanupIt->runtimeFn);
    }

    for (auto lifeIt = scope.lifetimes.rbegin();
         lifeIt != scope.lifetimes.rend(); ++lifeIt) {
      emitLifetimeEnd(lifeIt->allocaInst, lifeIt->size);
    }
  }

  if (retVal) {
    return builder.CreateRet(retVal);
  }

  if (currentFunc && currentFunc->name == "main" && !currentFunc->isMethod &&
      currentFunc->returnType->isVoid()) {
    return builder.CreateRet(llvm::ConstantInt::get(builder.getInt32Ty(), 0));
  }

  return builder.CreateRetVoid();
}

llvm::Value *CodeGen::visit(const ModuleNode *node) {
  diEmitter.initializeModule(node);

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
          if (varDecl->isGlobal || varDecl->isExtern) {
            llvm::Type *ty = getLLVMType(varDecl->type);
            std::string bindName = varDecl->mangledName.empty()
                                       ? std::string(varDecl->varName)
                                       : varDecl->mangledName;
            llvm::GlobalVariable *gvar = mod.getGlobalVariable(bindName);
            if (!gvar) {
              /* Apply weak linkage if the dependency variable is annotated with
               * @weak */
              llvm::GlobalValue::LinkageTypes linkage =
                  varDecl->isWeak ? llvm::GlobalValue::WeakAnyLinkage
                                  : llvm::GlobalValue::ExternalLinkage;

              llvm::Constant *init = nullptr;

              /* Record-typed globals may be dynamically initialized on the
               * defining side (const String = "..."), in which case the
               * definition is writable; mirror that so both modules agree on
               * the global's immutability. */
              const Type *unqualTy = varDecl->type->getUnqualifiedType();
              bool recordTy = unqualTy->getKind() == TypeKind::Struct ||
                              unqualTy->getKind() == TypeKind::Class ||
                              unqualTy->getKind() == TypeKind::Union;

              gvar = new llvm::GlobalVariable(mod, ty,
                                              varDecl->type->isConstQualified() &&
                                                  !recordTy,
                                              linkage, init, bindName);
            }
            cgCtx.bind(bindName, gvar, true);
          }
        } else if (stmt->kind == NodeKind::ClassDecl ||
                   stmt->kind == NodeKind::StructDecl ||
                   stmt->kind == NodeKind::UnionDecl ||
                   stmt->kind == NodeKind::NamespaceDecl) {
          /* Declare static class/struct/union fields of dependency modules
           * so member access (which resolves through the mangled name) can
           * reference them across modules. Namespace blocks are unwrapped
           * recursively. */
          std::vector<const VarDeclNode *> staticFields;
          std::function<void(const ASTNode *)> collectStatics =
              [&](const ASTNode *n) {
                if (auto *c = llvm::dyn_cast<ClassDeclNode>(n)) {
                  for (const auto *f : c->fields)
                    if (f->isStatic)
                      staticFields.push_back(f);
                } else if (auto *s = llvm::dyn_cast<StructDeclNode>(n)) {
                  for (const auto *f : s->fields)
                    if (f->isStatic)
                      staticFields.push_back(f);
                } else if (auto *u = llvm::dyn_cast<UnionDeclNode>(n)) {
                  for (const auto *f : u->fields)
                    if (f->isStatic)
                      staticFields.push_back(f);
                } else if (auto *ns = llvm::dyn_cast<NamespaceDeclNode>(n)) {
                  for (const auto *inner : ns->statements) {
                    /* Namespace-level variables/constants are globals from
                     * the consumer's perspective; declare them externally so
                     * qualified member access ('NS.CONST') resolves. */
                    if (auto *vd = llvm::dyn_cast<VarDeclNode>(inner)) {
                      if (vd->isGlobal || vd->isExtern)
                        staticFields.push_back(vd);
                    }
                    collectStatics(inner);
                  }
                }
              };
          collectStatics(stmt);

          for (const auto *field : staticFields) {
            std::string bindName = field->mangledName.empty()
                                       ? std::string(field->varName)
                                       : field->mangledName;
            if (cgCtx.lookupDetailed(bindName).value)
              continue;
            llvm::Type *ty = getLLVMType(field->type);
            llvm::GlobalVariable *gvar = mod.getGlobalVariable(bindName);
            if (!gvar) {
              gvar = new llvm::GlobalVariable(
                  mod, ty, field->type->isConstQualified(),
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
      const auto *sDecl = static_cast<const StructDeclNode *>(stmt);
      if (!sDecl->isTemplate)
        getLLVMType(sDecl->recordType);
    } else if (stmt->kind == NodeKind::ClassDecl) {
      const auto *cDecl = static_cast<const ClassDeclNode *>(stmt);
      if (!cDecl->isTemplate)
        getLLVMType(cDecl->recordType);
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

  for (const auto &stmt : node->statements) {
    dispatch(stmt);
  }

  for (const auto &stmt : node->instantiatedTemplates) {
    dispatch(stmt);
  }

  /* Runtime registry for Memory.isConst(): every canonical const object of
   * this module is registered before main runs. */
  this->emitConstRegistry();

  /* Finalize any generated dynamic module initializations safely */
  if (globalInitFunc) {
    llvm::BasicBlock *savedBB = builder.GetInsertBlock();
    builder.SetInsertPoint(&globalInitFunc->back());

    if (diEmitter.isEnabled()) {
      builder.SetCurrentDebugLocation(
          llvm::DILocation::get(ctx, 0, 0, globalInitFunc->getSubprogram()));
    }

    builder.CreateRetVoid();

    if (savedBB) {
      builder.SetInsertPoint(savedBB);
    } else {
      builder.ClearInsertionPoint();
    }
  }

  diEmitter.finalize();

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
      lastTemporaryAlloca = nullptr;
      llvm::Value *val = dispatch(node->base);
      if (lastTemporaryAlloca) {
        objPtr = lastTemporaryAlloca;
        lastTemporaryAlloca = nullptr;
      } else if (val) {
        objPtr = createEntryBlockAlloca(val->getType(), "tmp.op.recv");
        createTBAAStore(val, objPtr, node->base->exprType);
      }
    }

    llvm::Function *func = getOrCreateFunction(node->overloadedOperator);
    std::vector<llvm::Value *> argsArgs;
    argsArgs.push_back(objPtr);

    /* By-value struct arguments must be materialized (copy/move-constructed)
     * like ordinary call arguments: passing the raw value shares the
     * temporary's storage with the callee's parameter and both destruct it. */
    bool isRefParam = false;
    const Type *paramDeclTy = nullptr;
    if (!node->overloadedOperator->params.empty()) {
      paramDeclTy = node->overloadedOperator->params[0]->type;
      isRefParam = paramDeclTy->isReferenceType() ||
                   paramDeclTy->getKind() == TypeKind::RValueReference;
    }
    llvm::Value *idxVal = nullptr;
    if (isRefParam) {
      idxVal = getLValue(node->index);
      if (!idxVal) {
        lastTemporaryAlloca = nullptr;
        llvm::Value *val = dispatch(node->index);
        if (lastTemporaryAlloca) {
          idxVal = lastTemporaryAlloca;
          lastTemporaryAlloca = nullptr;
        } else if (val) {
          idxVal = createEntryBlockAlloca(val->getType(), "tmp.op.arg");
          builder.CreateStore(val, idxVal);
        }
      }
    } else {
      idxVal = paramDeclTy ? materializeByValueArg(node->index, paramDeclTy)
                           : nullptr;
      if (!idxVal) {
        lastTemporaryAlloca = nullptr;
        idxVal = dispatch(node->index);
        lastTemporaryAlloca = nullptr;
      }
    }

    llvm::Type *paramTy = func->getFunctionType()->getParamType(1);
    idxVal = createImplicitCast(idxVal, paramTy);
    argsArgs.push_back(idxVal);

    llvm::Value *res =
        emitCallOrInvoke(func->getFunctionType(), func, argsArgs);

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

/* Dart-style const objects */

namespace {
/* Mirrors Sema's resolver: the constructor behind an object creation. */
const FunctionDeclNode *cgResolveConstCtorCall(const ExprNode *expr) {
  if (auto *n = llvm::dyn_cast<NewExprNode>(expr))
    return n->resolvedConstructor;
  if (auto *call = llvm::dyn_cast<FunctionCallNode>(expr)) {
    const FunctionDeclNode *f = call->resolvedFunc;
    if (!f || !f->parentRecord)
      return nullptr;
    if (f->isNamedCtor) {
      if (auto *ma = llvm::dyn_cast<MemberAccessNode>(call->target))
        return ma->memberName == f->name ? f : nullptr;
      return nullptr;
    }
    std::string_view recName = f->parentRecord->getName();
    size_t dot = recName.find_last_of('.');
    std::string_view simple =
        (dot != std::string_view::npos) ? recName.substr(dot + 1) : recName;
    if (f->name == simple)
      return f;
  }
  return nullptr;
}

uint64_t fnv1a64(llvm::StringRef s) {
  uint64_t h = 1469598103934665603ULL;
  for (char c : s) {
    h ^= (uint8_t)c;
    h *= 1099511628211ULL;
  }
  return h;
}
} // namespace

/* Finds the array literal behind an "A:" const value (the node may be an
 * implicit cast or the const-expression wrapper). */
static const ArrayLiteralNode *cgUnwrapConstArray(const ExprNode *node) {
  if (auto *arr = llvm::dyn_cast<ArrayLiteralNode>(node))
    return arr;
  if (auto *ic = llvm::dyn_cast<ImplicitCastNode>(node))
    return cgUnwrapConstArray(ic->expr);
  if (auto *ce = llvm::dyn_cast<ConstExprNode>(node))
    return cgUnwrapConstArray(ce->expr);
  return nullptr;
}

/* One const-array element: nested const arrays are embedded as values. */
llvm::Constant *CodeGen::buildConstArrayElement(const ExprNode *node) {
  const Type *eu =
      node->exprType ? node->exprType->getUnqualifiedType() : nullptr;
  if (eu && eu->getKind() == TypeKind::Array && node->isConstExpr &&
      node->constKey.rfind("A:", 0) == 0)
    return buildConstArrayValue(node);
  return buildConstFromSerialized(node, node->constKey, node->exprType);
}

llvm::Constant *CodeGen::buildConstArray(const ExprNode *node,
                                         const std::string &key) {
  const ArrayLiteralNode *arr = cgUnwrapConstArray(node);
  if (!arr)
    return nullptr;
  const Type *arrTy = arr->exprType->getUnqualifiedType();
  auto *at = llvm::dyn_cast<ArrayType>(arrTy);
  if (!at)
    return nullptr;
  const Type *elemTy = at->getElementType();
  llvm::Type *elemLL = getLLVMType(elemTy);

  std::vector<llvm::Constant *> elems;
  for (const auto *e : arr->elements) {
    llvm::Constant *c = buildConstArrayElement(e);
    if (!c)
      return nullptr;
    elems.push_back(c);
  }

  llvm::ArrayType *arrLL = llvm::ArrayType::get(elemLL, elems.size());
  llvm::Constant *init;
  if (elems.empty()) {
    /* LLVM rejects zero-sized globals: a placeholder byte still yields a
     * valid (immutable) base pointer for the decayed empty array. */
    arrLL = llvm::ArrayType::get(builder.getInt8Ty(), 1);
    init = llvm::ConstantAggregateZero::get(arrLL);
  } else {
    init = llvm::ConstantArray::get(arrLL, elems);
  }

  /* Canonical static backing: deterministic name + mergeable linkage so
   * identical const arrays share one read-only array across modules. */
  std::string name = "__const_";
  for (char c : key) {
    if (isalnum((unsigned char)c) || c == '_' || c == '.')
      name += c;
    else
      name += '_';
  }
  if (name.size() > 160)
    name.resize(160);
  char buf[24];
  snprintf(buf, sizeof(buf), "_%016llx", (unsigned long long)fnv1a64(key));
  name += buf;

  auto *gv = new llvm::GlobalVariable(mod, arrLL, /*isConstant */ true,
                                      llvm::GlobalValue::LinkOnceODRLinkage,
                                      init, name);
  gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  llvm::Constant *zero = llvm::ConstantInt::get(builder.getInt32Ty(), 0);
  return llvm::ConstantExpr::getInBoundsGetElementPtr(
      arrLL, gv, llvm::ArrayRef<llvm::Constant *>{zero, zero});
}

/* The ConstantArray VALUE for a const array literal (for array-typed
 * variables and globals, which hold the array, not a decayed pointer). */
llvm::Constant *CodeGen::buildConstArrayValue(const ExprNode *node) {
  const ArrayLiteralNode *arr = cgUnwrapConstArray(node);
  if (!arr)
    return nullptr;
  const Type *arrTy = arr->exprType->getUnqualifiedType();
  auto *at = llvm::dyn_cast<ArrayType>(arrTy);
  if (!at)
    return nullptr;
  const Type *elemTy = at->getElementType();
  llvm::Type *elemLL = getLLVMType(elemTy);

  std::vector<llvm::Constant *> elems;
  for (const auto *e : arr->elements) {
    llvm::Constant *c = buildConstArrayElement(e);
    if (!c)
      return nullptr;
    elems.push_back(c);
  }
  return llvm::ConstantArray::get(
      llvm::ArrayType::get(elemLL, elems.size()), elems);
}

llvm::Constant *CodeGen::buildConstStringGlobal(llvm::StringRef value) {
  llvm::Constant *strConst =
      llvm::ConstantDataArray::getString(ctx, value, true);
  /* Deterministic name + mergeable linkage: identical const strings in
   * different modules share one backing buffer. */
  llvm::SmallString<64> name;
  name.append("__const_str_");
  char buf[24];
  snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)fnv1a64(value));
  name.append(buf);
  auto *gv = new llvm::GlobalVariable(mod, strConst->getType(),
                                      /*isConstant */ true,
                                      llvm::GlobalValue::LinkOnceODRLinkage,
                                      strConst, name.str());
  gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  llvm::Constant *zero = llvm::ConstantInt::get(builder.getInt32Ty(), 0);
  return llvm::ConstantExpr::getInBoundsGetElementPtr(
      strConst->getType(), gv, llvm::ArrayRef<llvm::Constant *>{zero, zero});
}

/* String struct { data, len, cap } with data pointing at a static buffer.
 * cap == len so any mutation reallocates before touching read-only memory;
 * the struct is embedded in an immortal const object, so its destructor
 * never runs. */
llvm::Constant *CodeGen::buildConstString(llvm::StringRef value) {
  llvm::Constant *dataPtr = buildConstStringGlobal(value);
  std::vector<llvm::Constant *> elems;
  elems.push_back(dataPtr);
  elems.push_back(llvm::ConstantInt::get(builder.getInt64Ty(), value.size()));
  elems.push_back(llvm::ConstantInt::get(builder.getInt64Ty(), value.size()));
  return llvm::ConstantStruct::get(
      llvm::StructType::get(ctx, {builder.getPtrTy(), builder.getInt64Ty(),
                                  builder.getInt64Ty()},
                            /*isPacked */ false),
      elems);
}

llvm::Constant *CodeGen::buildConstFromSerialized(const ExprNode *node,
                                                  const std::string &key,
                                                  const Type *expected) {
  if (key.rfind("o:", 0) == 0) {
    return getOrCreateCanonicalConst(node);
  }
  if (key.rfind("A:", 0) == 0) {
    return buildConstArray(node, key);
  }
  if (key.rfind("s:", 0) == 0) {
    /* Unescape '\:' and '\\'. */
    std::string content;
    content.reserve(key.size() - 2);
    for (size_t i = 2; i < key.size(); ++i) {
      if (key[i] == '\\' && i + 1 < key.size())
        ++i;
      content += key[i];
    }
    if (expected) {
      const Type *u = expected->getUnqualifiedType();
      if (u->getKind() == TypeKind::Class || u->getKind() == TypeKind::Struct) {
        /* String value (or struct field) -> String struct constant. */
        if (u->isPointerType()) {
          /* handled below */
        } else {
          return buildConstString(content);
        }
      }
      if (u->isPointerType()) {
        return buildConstStringGlobal(content);
      }
    }
    return buildConstString(content);
  }
  if (key.rfind("i:", 0) == 0 || key.rfind("u:", 0) == 0) {
    bool isSigned = key[1] == 'i';
    unsigned long long raw =
        strtoull(key.c_str() + 2, nullptr, 10);
    llvm::Type *ty = nullptr;
    if (expected) {
      const Type *u = expected->getUnqualifiedType();
      ty = getLLVMType(u);
    }
    if (ty && ty->isFloatingPointTy()) {
      llvm::APFloat apf(ty->getFltSemantics());
      apf.convertFromAPInt(
          llvm::APInt(64, raw, isSigned), isSigned,
          llvm::APFloat::rmNearestTiesToEven);
      return llvm::ConstantFP::get(ctx, apf);
    }
    if (ty && ty->isIntegerTy()) {
      unsigned bits = ty->getIntegerBitWidth();
      return llvm::ConstantInt::get(ctx,
                                    llvm::APInt(bits, raw, isSigned));
    }
    return llvm::ConstantInt::get(builder.getInt64Ty(), raw, isSigned);
  }
  if (key.rfind("f:", 0) == 0) {
    double d = strtod(key.c_str() + 2, nullptr);
    llvm::Type *ty = nullptr;
    if (expected) {
      const Type *u = expected->getUnqualifiedType();
      ty = getLLVMType(u);
    }
    if (ty && ty->isFloatingPointTy()) {
      llvm::APFloat apf(ty->getFltSemantics());
      apf.convertFromString(key.c_str() + 2, llvm::APFloat::rmNearestTiesToEven);
      return llvm::ConstantFP::get(ctx, apf);
    }
    llvm::APFloat apd(d);
    if (ty && ty->isIntegerTy()) {
      llvm::APSInt api(ty->getIntegerBitWidth(), false);
      bool isExact = false;
      apd.convertToInteger(api, llvm::APFloat::rmTowardZero, &isExact);
      return llvm::ConstantInt::get(ctx, api);
    }
    return llvm::ConstantFP::get(ctx, apd);
  }
  if (key == "b:0" || key == "b:1") {
    if (expected) {
      llvm::Type *ty = getLLVMType(expected->getUnqualifiedType());
      if (ty->isIntegerTy())
        return llvm::ConstantInt::get(ty, key == "b:1" ? 1 : 0);
    }
    return llvm::ConstantInt::get(builder.getInt1Ty(), key == "b:1" ? 1 : 0);
  }
  if (key == "n") {
    return llvm::ConstantPointerNull::get(builder.getPtrTy());
  }
  if (key.rfind("c:", 0) == 0 || key.rfind("r:", 0) == 0) {
    unsigned long long raw = strtoull(key.c_str() + 2, nullptr, 10);
    if (expected) {
      llvm::Type *ty = getLLVMType(expected->getUnqualifiedType());
      if (ty->isIntegerTy())
        return llvm::ConstantInt::get(ty, raw);
    }
    return llvm::ConstantInt::get(builder.getInt8Ty(), raw);
  }
  return nullptr;
}

void CodeGen::emitConstRegistry() {
  if (canonicalConsts.empty())
    return;

  llvm::Type *ptrTy = builder.getPtrTy();
  llvm::Type *i64Ty = builder.getInt64Ty();
  llvm::StructType *entryTy =
      llvm::StructType::get(ctx, {ptrTy, i64Ty});
  llvm::StructType *nodeTy =
      llvm::StructType::get(ctx, {ptrTy, i64Ty, ptrTy});

  /* Per-module table of (address, byte size) of this module's consts. */
  std::vector<llvm::Constant *> entries;
  for (auto &kv : canonicalConsts) {
    llvm::GlobalVariable *gv = kv.second;
    uint64_t size = mod.getDataLayout().getTypeAllocSize(gv->getValueType());
    entries.push_back(llvm::ConstantStruct::get(
        entryTy, {gv, llvm::ConstantInt::get(i64Ty, size)}));
  }
  llvm::ArrayType *tblTy = llvm::ArrayType::get(entryTy, entries.size());
  std::string modHash = std::to_string(fnv1a64(currentFilePath));
  auto *tblGV = new llvm::GlobalVariable(
      mod, tblTy, /*isConstant */ true,
      llvm::GlobalValue::LinkOnceODRLinkage,
      llvm::ConstantArray::get(tblTy, entries),
      "__utopia_const_tbl_" + modHash);
  tblGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  /* Registry node { table, count, next }: writable, chained at startup. */
  llvm::Constant *nullPtr = llvm::ConstantPointerNull::get(
      llvm::cast<llvm::PointerType>(ptrTy));
  auto *nodeGV = new llvm::GlobalVariable(
      mod, nodeTy, /*isConstant */ false,
      llvm::GlobalValue::LinkOnceODRLinkage,
      llvm::ConstantStruct::get(
          nodeTy, {tblGV,
                   llvm::ConstantInt::get(i64Ty, entries.size()), nullPtr}),
      "__utopia_const_node_" + modHash);

  /* Shared list head (linkonce_odr folds all modules into one). */
  llvm::GlobalVariable *headGV = mod.getGlobalVariable("__utopia_const_head");
  if (!headGV) {
    headGV = new llvm::GlobalVariable(mod, ptrTy, /*isConstant */ false,
                                      llvm::GlobalValue::LinkOnceODRLinkage,
                                      nullPtr, "__utopia_const_head");
  }

  /* Append this module's node during global init (runs before main in
   * both AOT and JIT). */
  llvm::Function *initF =
      globalInitFunc ? globalInitFunc : getOrCreateGlobalInitFunc();
  llvm::IRBuilder<> tmpB(&initF->getEntryBlock(),
                         initF->getEntryBlock().begin());
  llvm::Value *head = tmpB.CreateLoad(ptrTy, headGV, "const.head");
  llvm::Value *nextGep = tmpB.CreateStructGEP(nodeTy, nodeGV, 2);
  tmpB.CreateStore(head, nextGep);
  tmpB.CreateStore(nodeGV, headGV);

  /* The walker: linear scan over the chained (address, size) ranges. The
   * intrinsic may have already declared it; give the declaration a body. */
  llvm::Function *walker = mod.getFunction("__utopia_is_const_ptr");
  if (!walker) {
    llvm::FunctionType *fty = llvm::FunctionType::get(
        builder.getInt1Ty(), {ptrTy}, false);
    walker = llvm::Function::Create(
        fty, llvm::GlobalValue::LinkOnceODRLinkage, "__utopia_is_const_ptr",
        mod);
    walker->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  }
  if (walker->empty()) {

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", walker);
    llvm::BasicBlock *loop = llvm::BasicBlock::Create(ctx, "node.loop", walker);
    llvm::BasicBlock *check =
        llvm::BasicBlock::Create(ctx, "node.check", walker);
    llvm::BasicBlock *inner = llvm::BasicBlock::Create(ctx, "inner", walker);
    llvm::BasicBlock *innerBody =
        llvm::BasicBlock::Create(ctx, "inner.body", walker);
    llvm::BasicBlock *innerNext =
        llvm::BasicBlock::Create(ctx, "inner.next", walker);
    llvm::BasicBlock *nextNode =
        llvm::BasicBlock::Create(ctx, "next.node", walker);
    llvm::BasicBlock *retTrue =
        llvm::BasicBlock::Create(ctx, "ret.true", walker);
    llvm::BasicBlock *retFalse =
        llvm::BasicBlock::Create(ctx, "ret.false", walker);

    llvm::IRBuilder<> wb(entry);
    llvm::Value *p = walker->arg_begin();
    llvm::Value *headV = wb.CreateLoad(ptrTy, headGV, "head");
    wb.CreateBr(loop);

    wb.SetInsertPoint(loop);
    llvm::PHINode *nodePhi = wb.CreatePHI(ptrTy, 2, "node");
    nodePhi->addIncoming(headV, entry);
    llvm::Value *isNull = wb.CreateICmpEQ(nodePhi, nullPtr);
    wb.CreateCondBr(isNull, retFalse, check);

    wb.SetInsertPoint(check);
    llvm::Value *tblV =
        wb.CreateLoad(ptrTy, wb.CreateStructGEP(nodeTy, nodePhi, 0), "tbl");
    llvm::Value *cntV =
        wb.CreateLoad(i64Ty, wb.CreateStructGEP(nodeTy, nodePhi, 1), "cnt");
    wb.CreateBr(inner);

    wb.SetInsertPoint(inner);
    llvm::PHINode *iPhi = wb.CreatePHI(i64Ty, 2, "i");
    iPhi->addIncoming(wb.getInt64(0), check);
    llvm::Value *done = wb.CreateICmpUGE(iPhi, cntV);
    wb.CreateCondBr(done, nextNode, innerBody);

    wb.SetInsertPoint(innerBody);
    llvm::Value *entGep =
        wb.CreateInBoundsGEP(entryTy, tblV, iPhi, "entry");
    llvm::Value *startV =
        wb.CreateLoad(ptrTy, wb.CreateStructGEP(entryTy, entGep, 0), "start");
    llvm::Value *sizeV =
        wb.CreateLoad(i64Ty, wb.CreateStructGEP(entryTy, entGep, 1), "size");
    llvm::Value *endV = wb.CreateGEP(wb.getInt8Ty(), startV, sizeV, "end");
    llvm::Value *lo = wb.CreateICmpUGE(p, startV);
    llvm::Value *hi = wb.CreateICmpULT(p, endV);
    llvm::Value *inside = wb.CreateAnd(lo, hi);
    wb.CreateCondBr(inside, retTrue, innerNext);

    wb.SetInsertPoint(innerNext);
    llvm::Value *iNext = wb.CreateAdd(iPhi, wb.getInt64(1));
    iPhi->addIncoming(iNext, innerNext);
    wb.CreateBr(inner);

    wb.SetInsertPoint(nextNode);
    llvm::Value *nextV =
        wb.CreateLoad(ptrTy, wb.CreateStructGEP(nodeTy, nodePhi, 2), "next");
    nodePhi->addIncoming(nextV, nextNode);
    wb.CreateBr(loop);

    wb.SetInsertPoint(retTrue);
    wb.CreateRet(wb.getTrue());

    wb.SetInsertPoint(retFalse);
    wb.CreateRet(wb.getFalse());
  }
}

llvm::GlobalVariable *CodeGen::getOrCreateCanonicalConst(const ExprNode *node) {
  if (!node || !node->isConstExpr || node->constKey.rfind("o:", 0) != 0)
    return nullptr;
  const std::string &key = node->constKey;

  auto it = canonicalConsts.find(key);
  if (it != canonicalConsts.end())
    return it->second;

  /* The creation may be wrapped in a const-expression or implicit cast. */
  while (auto *ce = llvm::dyn_cast<ConstExprNode>(node))
    node = ce->expr;
  while (auto *ic = llvm::dyn_cast<ImplicitCastNode>(node))
    node = ic->expr;

  const FunctionDeclNode *ctor = cgResolveConstCtorCall(node);
  if (!ctor || !ctor->parentRecord)
    return nullptr;

  /* The creation AST node is shared through the AST context so modules
   * that only see the const object by key can still rebuild the constant
   * initializer from the original expression. */
  const ExprNode *creation = node;
  auto cit = astCtx.constObjectCreations.find(key);
  if (cit != astCtx.constObjectCreations.end())
    creation = cit->second;

  std::vector<llvm::Constant *> fields;
  llvm::Constant *init = buildConstObjectInitializer(creation, fields);
  if (!init) {
    return nullptr;
  }

  /* Deterministic symbol: identical constructions in any module produce
   * the same name, and 'linkonce_odr' lets the linker fold them into a
   * single address (canonicalization). */
  std::string name = "__const_";
  for (char c : key) {
    if (isalnum((unsigned char)c) || c == '_' || c == '.')
      name += c;
    else
      name += '_';
  }
  if (name.size() > 160)
    name.resize(160);
  char buf[24];
  snprintf(buf, sizeof(buf), "_%016llx", (unsigned long long)fnv1a64(key));
  name += buf;

  llvm::StructType *structTy =
      llvm::cast<llvm::StructType>(getLLVMType(ctor->parentRecord));
  auto *gv = new llvm::GlobalVariable(mod, structTy,
                                      /*isConstant */ true,
                                      llvm::GlobalValue::LinkOnceODRLinkage,
                                      init, name);
  gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  canonicalConsts[key] = gv;
  return gv;
}

llvm::Constant *CodeGen::buildConstObjectInitializer(
    const ExprNode *creation, std::vector<llvm::Constant *> &fields) {
  return buildConstObjectInitializerImpl(creation, fields, nullptr, nullptr);
}

llvm::Constant *CodeGen::buildConstObjectInitializerImpl(
    const ExprNode *creation, std::vector<llvm::Constant *> &fields,
    const FunctionDeclNode *parentCtor,
    const std::vector<const ExprNode *> *parentParamArgs) {
  const FunctionDeclNode *ctor = cgResolveConstCtorCall(creation);
  if (!ctor || !ctor->parentRecord)
    return nullptr;
  const ClassType *classTy = llvm::cast<ClassType>(ctor->parentRecord);

  const auto *call = llvm::dyn_cast<FunctionCallNode>(creation);
  const auto *newNode = llvm::dyn_cast<NewExprNode>(creation);
  llvm::ArrayRef<ExprNode *> args =
      call ? call->args
           : (newNode ? newNode->args : llvm::ArrayRef<ExprNode *>());
  llvm::ArrayRef<std::string_view> argNames =
      call ? call->argNames
           : (newNode ? newNode->argNames
                      : llvm::ArrayRef<std::string_view>());

  /* Map params to args (positional in order, named by name). */
  std::vector<const ExprNode *> paramArgs(ctor->params.size(), nullptr);
  size_t positional = 0;
  for (size_t i = 0; i < args.size(); ++i) {
    if (!argNames.empty() && !argNames[i].empty()) {
      for (size_t p = 0; p < ctor->params.size(); ++p)
        if (ctor->params[p]->name == argNames[i])
          paramArgs[p] = args[i];
    } else {
      if (positional < ctor->params.size())
        paramArgs[positional] = args[i];
      ++positional;
    }
  }

  /* A super call forwards the enclosing constructor's parameters; resolve
   * them against the caller's arguments (mirrors Sema's constParamEnv). */
  if (parentCtor && parentParamArgs) {
    for (size_t p = 0; p < paramArgs.size(); ++p) {
      const ExprNode *a = paramArgs[p];
      if (!a)
        continue;
      if (auto *v = llvm::dyn_cast<VariableNode>(a)) {
        if (v->resolvedDecl &&
            v->resolvedDecl->kind == NodeKind::ParamDecl) {
          for (size_t pp = 0; pp < parentCtor->params.size(); ++pp) {
            if (parentCtor->params[pp] == v->resolvedDecl) {
              paramArgs[p] = (*parentParamArgs)[pp];
              break;
            }
          }
        }
      }
    }
  }

  /* vptr first for polymorphic classes (getLLVMType layout). */
  if (classTy->getIsPolymorphic()) {
    fields.push_back(getOrCreateVTable(classTy));
  }

  /* Base-class fields, in layout order, from the const super call. */
  if (ctor->superCall) {
    if (!cgResolveConstCtorCall(ctor->superCall))
      return nullptr;
    if (!buildConstObjectInitializerImpl(ctor->superCall, fields, ctor,
                                         &paramArgs))
      return nullptr;
  }

  const ClassDeclNode *cls = nullptr;
  if (const DeclNode *recDecl = classTy->getDeclaration())
    cls = llvm::dyn_cast<ClassDeclNode>(recDecl);
  if (!cls)
    return nullptr;

  for (const auto *f : cls->fields) {
    if (f->isStatic)
      continue;
    llvm::Constant *val = nullptr;

    /* Array fields embed the constant VALUE (not the decayed pointer). */
    auto fieldValue = [&](const ExprNode *valueNode) -> llvm::Constant * {
      /* ': this.field = param' entries reference the ctor parameters. */
      if (auto *v = llvm::dyn_cast<VariableNode>(valueNode)) {
        if (v->resolvedDecl &&
            v->resolvedDecl->kind == NodeKind::ParamDecl) {
          for (size_t pp = 0; pp < ctor->params.size(); ++pp) {
            if (ctor->params[pp] == v->resolvedDecl) {
              valueNode = paramArgs[pp];
              break;
            }
          }
        }
      }
      if (!valueNode)
        return nullptr;
      if (valueNode->isConstExpr && valueNode->constKey.rfind("A:", 0) == 0) {
        const Type *fu = f->type->getUnqualifiedType();
        if (fu->getKind() == TypeKind::Array)
          return buildConstArrayValue(valueNode);
      }
      return buildConstFromSerialized(valueNode, valueNode->constKey, f->type);
    };

    /* this-parameter argument. */
    for (size_t p = 0; p < ctor->params.size(); ++p) {
      if (ctor->params[p]->isThisParam &&
          ctor->params[p]->name == f->varName && paramArgs[p]) {
        val = fieldValue(paramArgs[p]);
        break;
      }
    }
    /* ': this.field = expr' initializer-list entry. */
    if (!val) {
      for (const auto *init : ctor->fieldInitializers) {
        if (auto *target = llvm::dyn_cast<MemberAccessNode>(init->target)) {
          if (target->memberName == f->varName) {
            val = fieldValue(init->value);
            break;
          }
        }
      }
    }
    /* Declaration initializer. */
    if (!val && f->initializer) {
      val = fieldValue(f->initializer);
    }
    if (!val) {
      return nullptr;
    }
    fields.push_back(val);
  }

  return llvm::ConstantStruct::get(
      llvm::cast<llvm::StructType>(getLLVMType(classTy)), fields);
}

llvm::Value *CodeGen::visit(const ConstExprNode *node) {
  if (node->isConstExpr) {
    if (llvm::Constant *c = buildConstFromSerialized(node, node->constKey,
                                                     node->exprType))
      return c;
  }
  return dispatch(node->expr);
}

llvm::Value *CodeGen::visit(const NewExprNode *node) {
  if (node->isConstExpr) {
    if (llvm::Constant *c = buildConstFromSerialized(node, node->constKey,
                                                     node->exprType))
      return c;
  }
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

  llvm::Value *allocatedMem = nullptr;
  llvm::Value *userMem = nullptr;
  llvm::Value *typedMem = nullptr;

  if (node->placementExpr) {
    /* Placement new: construct in existing memory; no allocation, no
     * ownership. */
    llvm::Value *placePtr = dispatch(node->placementExpr);
    if (!placePtr)
      return nullptr;
    typedMem = builder.CreateBitCast(placePtr, getLLVMType(node->exprType),
                                     "placement.ptr");
  } else {
    /* Custom allocator ('operator new') or the default malloc. */
    llvm::Function *allocFunc = nullptr;
    if (node->allocator) {
      allocFunc = getOrCreateFunction(node->allocator);
    } else {
      allocFunc = mod.getFunction("malloc");
      if (!allocFunc) {
        llvm::FunctionType *mallocTy = llvm::FunctionType::get(
            builder.getPtrTy(), {builder.getInt64Ty()}, false);
        allocFunc = llvm::Function::Create(
            mallocTy, llvm::Function::ExternalLinkage, "malloc", mod);
      }
    }

    allocatedMem = builder.CreateCall(allocFunc, {sizeVal});

    /* Out of memory: no exceptions in Utopia, so terminate (the C++
     * analogue of an allocation failure is std::bad_alloc). */
    {
      llvm::Function *theFunction = builder.GetInsertBlock()->getParent();
      llvm::BasicBlock *oomBB =
          llvm::BasicBlock::Create(ctx, "new.oom", theFunction);
      llvm::BasicBlock *contBB =
          llvm::BasicBlock::Create(ctx, "new.oom.cont");
      llvm::Value *isNull = builder.CreateIsNull(allocatedMem, "alloc.null");
      builder.CreateCondBr(isNull, oomBB, contBB);

      builder.SetInsertPoint(oomBB);
      llvm::Function *abortFunc = mod.getFunction("abort");
      if (!abortFunc) {
        abortFunc = llvm::Function::Create(
            llvm::FunctionType::get(builder.getVoidTy(), false),
            llvm::Function::ExternalLinkage, "abort", mod);
        abortFunc->setDoesNotReturn();
      }
      builder.CreateCall(abortFunc, {});
      builder.CreateUnreachable();

      theFunction->insert(theFunction->end(), contBB);
      builder.SetInsertPoint(contBB);
    }

    userMem = allocatedMem;
    if (node->arraySize) {
      /* Store the element count at the base of the allocated block */
      builder.CreateStore(arrSize64, allocatedMem);
      /* Offset the pointer by 8 bytes to return to the user context */
      userMem = builder.CreateInBoundsGEP(builder.getInt8Ty(), allocatedMem,
                                          builder.getInt64(8));
    }

    typedMem = builder.CreateBitCast(userMem, getLLVMType(node->exprType));
  }

  const Type *elemUnqual = node->allocatedType->getUnqualifiedType();
  bool elemIsRecord = elemUnqual->getKind() == TypeKind::Class ||
                      elemUnqual->getKind() == TypeKind::Struct ||
                      elemUnqual->getKind() == TypeKind::Union;

  /* Constructors never store the vtable themselves (and may even zero the
   * object), so it must be written after construction completes. Storing it
   * last also guarantees the most-derived vtable survives base-constructor
   * delegation. */
  auto storeVTableIfPolymorphic = [&](llvm::Value *objPtr, const Type *ty) {
    const Type *u = ty->getUnqualifiedType();
    if (u->getKind() != TypeKind::Class)
      return;
    auto *classTy = static_cast<const ClassType *>(u);
    if (!classTy->getIsPolymorphic())
      return;
    llvm::Constant *vtable = getOrCreateVTable(classTy);
    llvm::Value *vptrGep = builder.CreateStructGEP(getLLVMType(u), objPtr, 0,
                                                   "vptr");
    builder.CreateStore(vtable, vptrGep);
  };

  if (node->arraySize) {
    /* Records must be zero-initialized (at minimum) so member-wise copies
     * and destructors see valid state. This is unconditional: the
     * optimizer may legally remove a default-constructor loop whose
     * allocations are provably freed again, but a memset of the block
     * cannot be dropped. */
    if (node->hasParens || elemIsRecord) {
      llvm::Value *memsetSize = builder.CreateSub(sizeVal, builder.getInt64(8));
      builder.CreateMemSet(userMem, builder.getInt8(0), memsetSize,
                           llvm::Align(1));
    }

    /* Default-construct every element of a record array so its state is
     * valid before use and before 'delete[]' runs the element destructors. */
    const FunctionDeclNode *elemCtor = nullptr;
    if (elemUnqual->getKind() == TypeKind::Class ||
        elemUnqual->getKind() == TypeKind::Struct ||
        elemUnqual->getKind() == TypeKind::Union) {
      auto *recTy = static_cast<const RecordType *>(elemUnqual);
      if (auto *decl = recTy->getDeclaration()) {
        llvm::ArrayRef<FunctionDeclNode *> ctors;
        if (decl->kind == NodeKind::ClassDecl)
          ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
        else if (decl->kind == NodeKind::StructDecl)
          ctors = static_cast<const StructDeclNode *>(decl)->constructors;
        else if (decl->kind == NodeKind::UnionDecl)
          ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

        for (const auto *ctor : ctors) {
          if (ctor->params.empty()) {
            elemCtor = ctor;
            break;
          }
        }
      }
    }

    if (elemCtor ||
        (elemUnqual->getKind() == TypeKind::Class ||
         elemUnqual->getKind() == TypeKind::Struct ||
         elemUnqual->getKind() == TypeKind::Union)) {
      llvm::Function *ctorFunc =
          elemCtor ? getOrCreateFunction(elemCtor) : nullptr;
      llvm::Function *theFunction = builder.GetInsertBlock()->getParent();

      llvm::BasicBlock *condBB =
          llvm::BasicBlock::Create(ctx, "new.array.cond", theFunction);
      llvm::BasicBlock *bodyBB =
          llvm::BasicBlock::Create(ctx, "new.array.body", theFunction);
      llvm::BasicBlock *endBB =
          llvm::BasicBlock::Create(ctx, "new.array.end", theFunction);

      llvm::IRBuilder<> tmpB(&theFunction->getEntryBlock(),
                             theFunction->getEntryBlock().begin());
      llvm::AllocaInst *idxAlloca =
          tmpB.CreateAlloca(builder.getInt64Ty(), nullptr, "new.idx");

      builder.CreateStore(builder.getInt64(0), idxAlloca);
      builder.CreateBr(condBB);

      builder.SetInsertPoint(condBB);
      llvm::Value *idxVal =
          builder.CreateLoad(builder.getInt64Ty(), idxAlloca);
      llvm::Value *cmp = builder.CreateICmpULT(idxVal, arrSize64);
      builder.CreateCondBr(cmp, bodyBB, endBB);

      builder.SetInsertPoint(bodyBB);
      llvm::Value *elemPtr = builder.CreateInBoundsGEP(
          getLLVMType(elemUnqual), typedMem, idxVal);
      if (ctorFunc) {
        emitCallOrInvoke(ctorFunc->getFunctionType(), ctorFunc, {elemPtr});
        storeVTableIfPolymorphic(elemPtr, elemUnqual);
      } else {
        /* Records without an explicit default constructor still need
         * their members constructed (e.g. String): a bare malloc leaves
         * garbage that crashes on assignment/destruction. */
        emitDefaultInitialization(elemPtr, elemUnqual);
      }
      llvm::Value *nextIdx = builder.CreateAdd(idxVal, builder.getInt64(1));
      builder.CreateStore(nextIdx, idxAlloca);
      builder.CreateBr(condBB);

      builder.SetInsertPoint(endBB);
    }
  } else if (!node->args.empty() &&
             (elemUnqual->getKind() != TypeKind::Class &&
              elemUnqual->getKind() != TypeKind::Struct &&
              elemUnqual->getKind() != TypeKind::Union)) {
    /* Scalar initialization: 'new T(value)' writes the argument into the
     * allocated object (e.g. 'new int(42)'). */
    llvm::Value *initVal = dispatchValueOf(node->args[0]);
    if (initVal) {
      initVal =
          createImplicitCast(initVal, getLLVMType(node->allocatedType));
      createTBAAStore(initVal, typedMem, node->allocatedType);
    }
  } else if (node->hasParens || node->resolvedConstructor) {
    /* Memory.construct zeroes the destination first so constructor
     * assignments ('this.field = value') and destructor-bearing members
     * always see a valid object state; without a constructor,
     * value-initialize (zero) as well. */
    if (node->placementExpr || !node->resolvedConstructor) {
      emitDefaultInitialization(typedMem, node->allocatedType);
    }

    /* The parser-synthesized implicit constructor has an empty body and
     * exists only to satisfy call syntax: constructing through it must
     * value-initialize (zero + field initializers + vptr) instead, since a
     * bare malloc would leave every field undefined. */
    if (node->resolvedConstructor && node->resolvedConstructor->isImplicit) {
      emitDefaultInitialization(typedMem, node->allocatedType);
    } else if (node->resolvedConstructor && !node->placementExpr) {
      /* Explicit constructors may assign through operators that free the
       * previous buffer (e.g. String::operator=): the object came straight
       * from malloc and holds garbage, so zero it before the body runs.
       * Placement new is excluded: Memory.construct zeroes there. */
      llvm::Align allocAlign = mod.getDataLayout().getABITypeAlign(
          getLLVMType(node->allocatedType));
      uint64_t allocSize =
          mod.getDataLayout().getTypeAllocSize(getLLVMType(node->allocatedType));
      builder.CreateMemSet(typedMem, builder.getInt8(0), allocSize,
                           allocAlign);
    }

    if (node->implicitCopyInit) {
      /* Trivially copyable record with a single value argument: bitwise
       * copy instead of a constructor call. */
      llvm::Value *initVal = dispatchValueOf(node->args[0]);
      if (initVal) {
        initVal =
            createImplicitCast(initVal, getLLVMType(node->allocatedType));
        createTBAAStore(initVal, typedMem, node->allocatedType);
      }
    }

    if (node->resolvedConstructor) {
      llvm::Function *ctorFunc =
          getOrCreateFunction(node->resolvedConstructor);
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
          const Type *paramDeclTy = nullptr;
          if (node->resolvedConstructor &&
              astParamIdx < node->resolvedConstructor->params.size()) {
            paramDeclTy =
                node->resolvedConstructor->params[astParamIdx]->type;
          }

          argVal = paramDeclTy ? materializeByValueArg(arg, paramDeclTy)
                               : nullptr;

          if (!argVal) {
            lastTemporaryAlloca = nullptr;
            argVal = dispatch(arg);
            lastTemporaryAlloca = nullptr;
          }

          if (ctorFunc && argVal && llArgIdx < ctorFunc->arg_size()) {
            llvm::Type *paramTy =
                ctorFunc->getFunctionType()->getParamType(llArgIdx);
            argVal = createImplicitCast(argVal, paramTy, arg->exprType);
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

      emitCallOrInvoke(ctorFunc->getFunctionType(), ctorFunc, argsArgs);
      storeVTableIfPolymorphic(typedMem, elemUnqual);
    }
  }

  return typedMem;
}

llvm::Value *CodeGen::visit(const DeleteExprNode *node) {
  llvm::Value *ptr = dispatch(node->ptr);
  if (!ptr)
    return nullptr;

  llvm::Function *theFunction = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *deleteBB =
      llvm::BasicBlock::Create(ctx, "delete.notnull", theFunction);
  llvm::BasicBlock *mergeBB = llvm::BasicBlock::Create(ctx, "delete.cont");

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

    if (node->deallocator) {
      llvm::Function *deallocFunc = getOrCreateFunction(node->deallocator);
      builder.CreateCall(deallocFunc, {rawPtr});
    } else {
      builder.CreateCall(freeFunc, {rawPtr});
    }
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
    if (node->deallocator) {
      llvm::Function *deallocFunc = getOrCreateFunction(node->deallocator);
      builder.CreateCall(deallocFunc, {ptr});
    } else {
      builder.CreateCall(freeFunc, {ptr});
    }
  }

  builder.CreateBr(mergeBB);

  theFunction->insert(theFunction->end(), mergeBB);
  builder.SetInsertPoint(mergeBB);

  return nullptr;
}

llvm::Value *CodeGen::visit(const DestructorCallNode *node) {
  llvm::Value *obj = nullptr;
  if (node->object->exprType &&
      node->object->exprType->getUnqualifiedType()->isPointerType()) {
    obj = dispatch(node->object);
  } else {
    obj = getLValue(node->object);
  }
  if (!obj || !node->destructor)
    return nullptr;
  emitCleanupCall(obj, node->destructor);
  return nullptr;
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
  return createTBAALoad(llTy, ptr,
                        tbaaManager.getTBAAAccessTag(*this, utopiaTy), name);
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
  return createTBAAStore(val, ptr,
                         tbaaManager.getTBAAAccessTag(*this, utopiaTy));
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

llvm::Constant *CodeGen::getOrCreateVTable(const ClassType *classTy) {
  std::string vtableName = "_ZTV" + std::string(classTy->getName());
  std::replace(vtableName.begin(), vtableName.end(), '.', '_');

  if (llvm::GlobalVariable *gv = mod.getGlobalVariable(vtableName)) {
    return gv;
  }

  auto *cDecl = llvm::cast<ClassDeclNode>(classTy->getDeclaration());
  std::vector<const FunctionDeclNode *> vtableMethods;

  /* Walks the base chain and every interface so inherited implementations
   * land in the correct (globally shared) slots. Own methods are processed
   * last, so the most-derived implementation overwrites any abstract slot
   * from an interface. */
  auto collectMethods = [&](const ClassDeclNode *node, auto &self) -> void {
    if (node->baseClass) {
      if (auto *pType = llvm::dyn_cast<ClassType>(
              node->baseClass->getUnqualifiedType())) {
        if (auto *pDecl = llvm::dyn_cast_or_null<ClassDeclNode>(
                pType->getDeclaration())) {
          self(pDecl, self);
        }
      }
    }
    for (const auto *iface : node->interfaces) {
      if (auto *iType =
              llvm::dyn_cast<ClassType>(iface->getUnqualifiedType())) {
        if (auto *iDecl = llvm::dyn_cast_or_null<ClassDeclNode>(
                iType->getDeclaration())) {
          self(iDecl, self);
        }
      }
    }
    for (auto *m : node->methods) {
      if (m->isVirtual || m->isOverride) {
        if (m->vtableIndex >= vtableMethods.size()) {
          vtableMethods.resize(m->vtableIndex + 1, nullptr);
        }
        vtableMethods[m->vtableIndex] = m;
      }
    }
  };

  collectMethods(cDecl, collectMethods);

  /* Slot 0 is reserved for the RTTI type descriptor (see
   * getOrCreateTypeInfo); virtual dispatch indexes methods from slot 1. */
  std::vector<llvm::Constant *> vtablePointers;
  vtablePointers.push_back(getOrCreateTypeInfo(classTy));
  for (auto *m : vtableMethods) {
    if (m) {
      if (m->isAbstract) {
        vtablePointers.push_back(
            llvm::ConstantPointerNull::get(builder.getPtrTy()));
      } else {
        llvm::Function *func = getOrCreateFunction(m);
        vtablePointers.push_back(func);
      }
    } else {
      vtablePointers.push_back(
          llvm::ConstantPointerNull::get(builder.getPtrTy()));
    }
  }

  llvm::ArrayType *vtableTy =
      llvm::ArrayType::get(builder.getPtrTy(), vtablePointers.size());
  llvm::Constant *vtableInit =
      llvm::ConstantArray::get(vtableTy, vtablePointers);

  auto *gv = new llvm::GlobalVariable(mod, vtableTy, true,
                                      llvm::GlobalValue::LinkOnceODRLinkage,
                                      vtableInit, vtableName);
  return gv;
}

llvm::Constant *CodeGen::getOrCreateTypeInfo(const ClassType *classTy) {
  std::string infoName = "_ZTI" + std::string(classTy->getName());
  std::replace(infoName.begin(), infoName.end(), '.', '_');

  if (llvm::GlobalVariable *gv = mod.getGlobalVariable(infoName)) {
    return gv;
  }

  auto *cDecl = llvm::cast<ClassDeclNode>(classTy->getDeclaration());

  /* Slot 0: the parent's descriptor (null at the root of the hierarchy).
   * The chain is linked for every class (polymorphic or not): catch
   * matching walks it to accept a thrown derived type in a base clause. */
  llvm::Constant *parentTD = llvm::ConstantPointerNull::get(builder.getPtrTy());
  if (cDecl->baseClass) {
    if (auto *pType = llvm::dyn_cast<ClassType>(
            cDecl->baseClass->getUnqualifiedType())) {
      parentTD = getOrCreateTypeInfo(pType);
    }
  }

  /* Remaining slots: every (transitively) implemented interface descriptor,
   * terminated by null. The closure is computed at compile time so the
   * runtime walk only needs a flat list per class. */
  std::vector<llvm::Constant *> ifaceTDs;
  std::function<void(const ClassDeclNode *)> collectIfaces =
      [&](const ClassDeclNode *decl) {
        if (!decl)
          return;
        for (const Type *iface : decl->interfaces) {
          if (auto *iType =
                  llvm::dyn_cast<ClassType>(iface->getUnqualifiedType())) {
            llvm::Constant *iTD = getOrCreateTypeInfo(iType);
            if (std::find(ifaceTDs.begin(), ifaceTDs.end(), iTD) ==
                ifaceTDs.end()) {
              ifaceTDs.push_back(iTD);
            }
            collectIfaces(
                llvm::dyn_cast_or_null<ClassDeclNode>(iType->getDeclaration()));
          }
        }
      };
  collectIfaces(cDecl);

  std::vector<llvm::Constant *> slots;
  slots.push_back(parentTD);
  slots.insert(slots.end(), ifaceTDs.begin(), ifaceTDs.end());
  slots.push_back(llvm::ConstantPointerNull::get(builder.getPtrTy()));

  llvm::ArrayType *infoTy = llvm::ArrayType::get(builder.getPtrTy(), slots.size());
  llvm::Constant *init = llvm::ConstantArray::get(infoTy, slots);

  return new llvm::GlobalVariable(mod, infoTy, true,
                                  llvm::GlobalValue::LinkOnceODRLinkage, init,
                                  infoName);
}

/* Descriptor for any throwable/catchable type. Class types reuse the
 * hierarchy descriptor (with the parent chain always linked); every other
 * type gets a single-null descriptor whose address identifies it, so exact
 * pointer comparison implements exact type matching. */
llvm::Constant *CodeGen::getOrCreateTypeInfoForType(const Type *type) {
  const Type *u = type->getUnqualifiedType();
  if (auto *classTy = llvm::dyn_cast<ClassType>(u)) {
    return getOrCreateTypeInfo(classTy);
  }

  std::string key = "t:" + u->toString();
  if (auto it = ehTypeInfoCache.find(key); it != ehTypeInfoCache.end()) {
    return it->second;
  }

  std::string infoName = "_ZTI";
  for (char c : key) {
    if (c == '.' || c == ' ' || c == '(' || c == ')' || c == ',' || c == '[' ||
        c == ']' || c == ':' || c == '*' || c == '&')
      infoName += '_';
    else
      infoName += c;
  }

  /* The descriptor layout mirrors the class form: slot 0 is the (absent)
   * parent and slot 1 terminates the interface list, so the runtime walk
   * reads exactly two slots. */
  llvm::ArrayType *infoTy = llvm::ArrayType::get(builder.getPtrTy(), 2);
  llvm::Constant *init = llvm::ConstantArray::get(
      infoTy,
      {llvm::ConstantPointerNull::get(builder.getPtrTy()),
       llvm::ConstantPointerNull::get(builder.getPtrTy())});

  auto *gv = new llvm::GlobalVariable(mod, infoTy, true,
                                      llvm::GlobalValue::LinkOnceODRLinkage,
                                      init, infoName);
  ehTypeInfoCache[key] = gv;
  return gv;
}

void CodeGen::emitDefaultInitialization(llvm::Value *ptr, const Type *type) {
  llvm::Type *llTy = getLLVMType(type);
  uint64_t size = mod.getDataLayout().getTypeAllocSize(llTy);

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

  if (unqual->getKind() == TypeKind::Class) {
    auto *classTy = static_cast<const ClassType *>(unqual);
    if (classTy->getIsPolymorphic()) {
      llvm::Constant *vtable = getOrCreateVTable(classTy);
      llvm::Value *vptrGep = builder.CreateStructGEP(llTy, ptr, 0, "vptr");
      builder.CreateStore(vtable, vptrGep);
    }
  }

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

    for (size_t i = 0; i < fields.size(); ++i) {
      auto *fieldDecl = fields[i];

      if (fieldDecl->isStatic) {
        continue;
      }

      if (fieldDecl->initializer) {
        const Type *fTy = fieldDecl->type;
        const Type *fUnqual = fTy->getUnqualifiedType();
        bool fIsAggregate = fUnqual->getKind() == TypeKind::Struct ||
                            fUnqual->getKind() == TypeKind::Class ||
                            fUnqual->getKind() == TypeKind::Union;

        llvm::Value *gep = ptr;
        uint64_t offset = 0;

        if (type->getKind() != TypeKind::Union) {
          auto *fInfo = recTy->getField(fieldDecl->varName);
          uint32_t fIdx = fInfo ? fInfo->index : 0;
          gep = builder.CreateStructGEP(llTy, ptr, fIdx,
                                        std::string(fieldDecl->varName));
          offset = layout->getElementOffset(fIdx);
        }

        llvm::MDNode *tbaaTag = tbaaManager.getTBAAStructAccessTag(
            *this, type, fTy, offset);

        if (fIsAggregate) {
          /* Records with destructor-bearing members need a deep copy: a
           * shallow struct store would alias the initializer temporary and
           * double-free when its cleanup runs. The destination field is
           * zeroed first (String::operator= frees the previous buffer). */
          llvm::Value *rvalAddr = getLValue(fieldDecl->initializer);
          if (!rvalAddr) {
            lastTemporaryAlloca = nullptr;
            llvm::Value *val = dispatch(fieldDecl->initializer);
            if (lastTemporaryAlloca) {
              rvalAddr = lastTemporaryAlloca;
              lastTemporaryAlloca = nullptr;
            } else if (val) {
              rvalAddr = createEntryBlockAlloca(getLLVMType(fTy),
                                                "tmp.field.init");
              createTBAAStore(val, rvalAddr, fTy);
            }
          }
          if (rvalAddr) {
            llvm::Align fAlign = mod.getDataLayout().getABITypeAlign(
                getLLVMType(fTy));
            uint64_t fSize =
                mod.getDataLayout().getTypeAllocSize(getLLVMType(fTy));
            builder.CreateMemSet(gep, builder.getInt8(0), fSize, fAlign);
            llvm::SmallPtrSet<const RecordType *, 8> copyVisited;
            if (isTriviallyCopyable(fTy, copyVisited)) {
              builder.CreateMemCpy(gep, fAlign, rvalAddr, fAlign, fSize);
            } else {
              emitMemberWiseCopy(gep, rvalAddr, fTy, false);
            }
          }
        } else {
          llvm::Value *initVal = dispatch(fieldDecl->initializer);
          if (initVal) {
            llvm::Type *destTy = getLLVMType(fTy);
            initVal = createImplicitCast(initVal, destTy);
            createTBAAStore(initVal, gep, tbaaTag);
          }
        }
      }
    }
  }
}

void CodeGen::emitArrayDefaultConstruct(llvm::Value *ptr, const Type *arrayType,
                                        const FunctionDeclNode *ctor) {
  if (!ptr || !ctor)
    return;

  const Type *unqual = arrayType->getUnqualifiedType();
  if (unqual->getKind() != TypeKind::Array) {
    llvm::Function *ctorFunc = getOrCreateFunction(ctor);
    emitCallOrInvoke(ctorFunc->getFunctionType(), ctorFunc, {ptr});
    return;
  }

  const ArrayType *arrTy = static_cast<const ArrayType *>(unqual);
  uint64_t count = arrTy->getSize();
  if (count == 0)
    return;

  llvm::Function *theFunction = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *condBB =
      llvm::BasicBlock::Create(ctx, "ctor.array.cond", theFunction);
  llvm::BasicBlock *bodyBB =
      llvm::BasicBlock::Create(ctx, "ctor.array.body", theFunction);
  llvm::BasicBlock *endBB =
      llvm::BasicBlock::Create(ctx, "ctor.array.end", theFunction);

  llvm::AllocaInst *idxAlloca =
      createEntryBlockAlloca(builder.getInt64Ty(), "ctor.idx");
  builder.CreateStore(builder.getInt64(0), idxAlloca);
  builder.CreateBr(condBB);

  builder.SetInsertPoint(condBB);
  llvm::Value *idxVal = builder.CreateLoad(builder.getInt64Ty(), idxAlloca);
  llvm::Value *cmp = builder.CreateICmpULT(idxVal, builder.getInt64(count));
  builder.CreateCondBr(cmp, bodyBB, endBB);

  builder.SetInsertPoint(bodyBB);
  llvm::Type *llvmArrTy = getLLVMType(arrTy);
  llvm::Value *elemPtr = builder.CreateInBoundsGEP(
      llvmArrTy, ptr, {builder.getInt32(0), idxVal});
  emitArrayDefaultConstruct(elemPtr, arrTy->getElementType(), ctor);
  llvm::Value *nextIdx = builder.CreateAdd(idxVal, builder.getInt64(1));
  builder.CreateStore(nextIdx, idxAlloca);
  builder.CreateBr(condBB);

  builder.SetInsertPoint(endBB);
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
          if (!val) {
            reportError(lit->elements[i]->line, lit->elements[i]->column,
                        lit->elements[i]->length,
                        "Failed to evaluate array literal element.");
            return;
          }
          val = createImplicitCast(val, getLLVMType(elemTy));
          createTBAAStore(val, elemPtr, elemTy);
        }
      }
    }
  } else {
    llvm::Value *srcVal = dispatch(initExpr);
    if (!srcVal) {
      reportError(initExpr->line, initExpr->column, initExpr->length,
                  "Failed to evaluate array initializer.");
      return;
    }
    llvm::Type *destTy = getLLVMType(unqualTarget);
    srcVal = createImplicitCast(srcVal, destTy);
    createTBAAStore(srcVal, targetAddr, targetType);
  }
}

void CodeGen::emitCleanupCall(llvm::Value *ptr, const FunctionDeclNode *dtor,
                              const Type *type, llvm::Value *guard) {
  emitCleanupCall(ptr, dtor, type, guard, nullptr);
}

void CodeGen::emitCleanupCall(llvm::Value *ptr, const FunctionDeclNode *dtor,
                              const Type *type, llvm::Value *guard,
                              llvm::Function *runtimeFn) {
  if (!ptr || (!dtor && !runtimeFn))
    return;

  if (guard) {
    /* Conditional cleanup: only run the destructor when the flag is
     * set (the object may never have been constructed). */
    llvm::Function *theFn = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock *doDtorBB =
        llvm::BasicBlock::Create(ctx, "dtor.guard.run", theFn);
    llvm::BasicBlock *skipBB =
        llvm::BasicBlock::Create(ctx, "dtor.guard.skip", theFn);
    llvm::Value *flag = builder.CreateLoad(builder.getInt1Ty(), guard,
                                           "dtor.guard");
    builder.CreateCondBr(flag, doDtorBB, skipBB);
    builder.SetInsertPoint(doDtorBB);
    emitCleanupCall(ptr, dtor, type, nullptr, runtimeFn);
    builder.CreateBr(skipBB);
    builder.SetInsertPoint(skipBB);
    return;
  }

  /* Dynamically unroll and destruct static arrays in reverse order */
  if (type && type->getUnqualifiedType()->getKind() == TypeKind::Array) {
    const ArrayType *arrTy =
        static_cast<const ArrayType *>(type->getUnqualifiedType());
    uint64_t count = arrTy->getSize();
    if (count == 0)
      return;

    llvm::Function *theFunction = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock *condBB =
        llvm::BasicBlock::Create(ctx, "dtor.array.cond", theFunction);
    llvm::BasicBlock *bodyBB =
        llvm::BasicBlock::Create(ctx, "dtor.array.body", theFunction);
    llvm::BasicBlock *endBB =
        llvm::BasicBlock::Create(ctx, "dtor.array.end", theFunction);

    llvm::AllocaInst *idxAlloca =
        createEntryBlockAlloca(builder.getInt64Ty(), "dtor.idx");
    builder.CreateStore(builder.getInt64(count), idxAlloca);
    builder.CreateBr(condBB);

    builder.SetInsertPoint(condBB);
    llvm::Value *idxVal = builder.CreateLoad(builder.getInt64Ty(), idxAlloca);
    llvm::Value *cmp = builder.CreateICmpSGT(idxVal, builder.getInt64(0));
    builder.CreateCondBr(cmp, bodyBB, endBB);

    builder.SetInsertPoint(bodyBB);
    llvm::Value *nextIdx = builder.CreateSub(idxVal, builder.getInt64(1));
    builder.CreateStore(nextIdx, idxAlloca);

    llvm::Type *llvmArrTy = getLLVMType(arrTy);
    llvm::Value *elemPtr = builder.CreateInBoundsGEP(
        llvmArrTy, ptr, {builder.getInt32(0), nextIdx});

    emitCleanupCall(elemPtr, dtor, arrTy->getElementType());
    builder.CreateBr(condBB);

    builder.SetInsertPoint(endBB);
    return;
  }

  llvm::Function *dtorFunc =
      runtimeFn ? runtimeFn : getOrCreateFunction(dtor);
  builder.CreateCall(dtorFunc, {ptr});
}

/* Destroys and unregisters the cleanups registered since 'cleanupCount'
 * (used by the ternary: branch temporaries must die at the end of their
 * branch, not unconditionally at scope exit — only one branch runs).
 * Guarded cleanups (owned copies of nested ternaries) are left in the
 * scope: they own their object until scope end. */
void CodeGen::emitBranchCleanups(size_t cleanupCount) {
  while (cgCtx.getCleanupCount() > cleanupCount) {
    const auto &cleanups = cgCtx.getCurrentScope().cleanups;
    if (cleanups.back().guard)
      break;
    emitCleanupCall(cleanups.back().instancePtr, cleanups.back().destructor,
                    cleanups.back().type, nullptr, cleanups.back().runtimeFn);
    cgCtx.popCleanup();
  }
}

void CodeGen::emitScopeCleanups() {
  const auto &scope = cgCtx.getCurrentScope();
  for (auto it = scope.cleanups.rbegin(); it != scope.cleanups.rend(); ++it) {
    emitCleanupCall(it->instancePtr, it->destructor, it->type, it->guard,
                    it->runtimeFn);
  }

  /* Flush lifetimes back to the execution environment upon natural closure */
  for (auto it = scope.lifetimes.rbegin(); it != scope.lifetimes.rend(); ++it) {
    emitLifetimeEnd(it->allocaInst, it->size);
  }
}

/* Registers a scope cleanup that runs when the scope exits normally. The
 * unwinding counterpart is emitted per invoke site by emitCallOrInvoke,
 * which captures the cleanups live at that point. */
void CodeGen::registerScopeCleanup(llvm::Value *ptr,
                                   const FunctionDeclNode *dtor,
                                   const Type *type, llvm::Value *guard,
                                   llvm::Function *runtimeFn) {
  cgCtx.addCleanup(ptr, dtor, type, guard, runtimeFn);
}


bool CodeGen::unwrapFutureType(const Type *t, const Type **outValue) {
  if (!t)
    return false;
  const Type *u = t->getUnqualifiedType();
  while (u->isPointerType() || u->isReferenceType() ||
         u->getKind() == TypeKind::RValueReference) {
    if (u->isPointerType())
      u = static_cast<const PointerType *>(u)->getPointeeType()
              ->getUnqualifiedType();
    else if (u->isReferenceType())
      u = static_cast<const ReferenceType *>(u)->getPointeeType()
              ->getUnqualifiedType();
    else
      u = static_cast<const RValueReferenceType *>(u)->getPointeeType()
              ->getUnqualifiedType();
  }

  std::string_view baseName;
  llvm::ArrayRef<const Type *> args;
  if (auto *inst = llvm::dyn_cast<TemplateInstType>(u)) {
    baseName = inst->getBaseName();
    args = inst->getTemplateArgs();
  } else if (auto *rec = llvm::dyn_cast<RecordType>(u)) {
    baseName = rec->getTemplateBaseName();
    args = rec->getTemplateArgs();
  } else {
    return false;
  }

  if (baseName.empty() || args.size() != 1)
    return false;
  if (baseName != "Future" && baseName != "Future.Future" &&
      !baseName.ends_with(".Future"))
    return false;

  if (outValue)
    *outValue = args[0];
  return true;
}

llvm::Function *CodeGen::getOrCreateRuntimeFunction(const std::string &name,
                                                    llvm::FunctionType *ty) {
  if (llvm::Function *f = mod.getFunction(name))
    return f;
  return llvm::Function::Create(ty, llvm::Function::ExternalLinkage, name,
                                &mod);
}

llvm::CallInst *CodeGen::emitRuntimeCall(const std::string &name,
                                         llvm::Type *retTy,
                                         llvm::ArrayRef<llvm::Value *> args) {
  std::vector<llvm::Type *> paramTys;
  for (auto *a : args) {
    paramTys.push_back(a->getType());
  }
  llvm::FunctionType *ty =
      llvm::FunctionType::get(retTy, paramTys, false);
  llvm::Function *fn = getOrCreateRuntimeFunction(name, ty);
  return builder.CreateCall(fn, args);
}

/* The personality routine installed on every function that may unwind. */
llvm::Function *CodeGen::getOrCreatePersonalityFunction() {
  if (llvm::Function *f = mod.getFunction("utopia_personality"))
    return f;

  llvm::Type *i32Ty = builder.getInt32Ty();
  llvm::Type *i64Ty = builder.getInt64Ty();
  llvm::FunctionType *fty =
      llvm::FunctionType::get(i32Ty,
                              {i32Ty, i32Ty, i64Ty, builder.getPtrTy(),
                               builder.getPtrTy()},
                              false);
  return getOrCreateRuntimeFunction("utopia_personality", fty);
}

/* The shared exception-pointer / selector slots of the current function,
 * written by every landing pad and read by the dispatch, handlers and
 * resumes. */
llvm::AllocaInst *CodeGen::getOrCreateEHExnSlot() {
  if (!ehExnSlot) {
    ehExnSlot =
        createEntryBlockAlloca(builder.getPtrTy(), "eh.exn.slot");
  }
  return ehExnSlot;
}

llvm::AllocaInst *CodeGen::getOrCreateEHSelSlot() {
  if (!ehSelSlot) {
    ehSelSlot =
        createEntryBlockAlloca(builder.getInt32Ty(), "eh.sel.slot");
  }
  return ehSelSlot;
}

/* Emits the per-function exception resume block: reconstructs the landing
 * pad value from the exception slots and resumes unwinding. */
llvm::BasicBlock *CodeGen::getOrCreateEHResumeBlock() {
  if (ehResumeBlock)
    return ehResumeBlock;

  llvm::Function *fn = builder.GetInsertBlock()->getParent();
  ehResumeBlock = llvm::BasicBlock::Create(ctx, "eh.resume", fn);
  llvm::BasicBlock *saved = builder.GetInsertBlock();
  builder.SetInsertPoint(ehResumeBlock);
  llvm::Value *exn = builder.CreateLoad(builder.getPtrTy(),
                                        getOrCreateEHExnSlot(), "eh.r.exn");
  llvm::Value *sel = builder.CreateLoad(builder.getInt32Ty(),
                                        getOrCreateEHSelSlot(), "eh.r.sel");
  llvm::Value *v = llvm::PoisonValue::get(
      llvm::StructType::get(ctx, {builder.getPtrTy(), builder.getInt32Ty()}));
  v = builder.CreateInsertValue(v, exn, 0);
  v = builder.CreateInsertValue(v, sel, 1);
  builder.CreateResume(v);
  builder.SetInsertPoint(saved);
  return ehResumeBlock;
}

/* Runs every pending scope cleanup (innermost first) into the current
 * block; used by landing pads to destroy the objects alive at an invoke
 * site before the exception continues. */
void CodeGen::emitScopeCleanupsInPad() {
  auto allScopes = cgCtx.getAllScopes();
  for (auto it = allScopes.rbegin(); it != allScopes.rend(); ++it) {
    for (auto cIt = it->cleanups.rbegin(); cIt != it->cleanups.rend(); ++cIt) {
      emitCleanupCall(cIt->instancePtr, cIt->destructor, cIt->type,
                      cIt->guard, cIt->runtimeFn);
    }
  }
}

llvm::Value *CodeGen::emitCallOrInvoke(llvm::FunctionType *fty,
                                       llvm::Value *callee,
                                       llvm::ArrayRef<llvm::Value *> args,
                                       const llvm::Twine &name) {
  if (!currentFunc || !currentFunc->mayUnwind) {
    return builder.CreateCall(fty, callee, args);
  }

  llvm::Function *fn = builder.GetInsertBlock()->getParent();
  llvm::Value *result = nullptr;

  /* Inside a try: the invoke routes to a fresh landing pad that captures the
   * cleanups live at this site plus the catch clauses of every enclosing
   * try (innermost first), then falls into the innermost catch dispatch.
   * The dispatch chains outward, so an exception that matches an enclosing
   * try's clause is routed there without a second search. */
  if (cgCtx.isTryActive()) {
    llvm::BasicBlock *pad = llvm::BasicBlock::Create(ctx, name + ".pad", fn);
    llvm::BasicBlock *cont =
        llvm::BasicBlock::Create(ctx, name + ".cont", fn);

    std::vector<llvm::Constant *> clauses;
    for (const auto &tryTypes : tryTypeInfoStack) {
      clauses.insert(clauses.end(), tryTypes.begin(), tryTypes.end());
    }
    llvm::BasicBlock *saved = builder.GetInsertBlock();
    builder.SetInsertPoint(pad);
    llvm::StructType *lpTy =
        llvm::StructType::get(ctx, {builder.getPtrTy(), builder.getInt32Ty()});
    bool hasCleanups = cgCtx.hasActiveCleanups();
    llvm::LandingPadInst *lp = builder.CreateLandingPad(
        lpTy, (hasCleanups ? 1 : 0) + clauses.size(), name + ".lp");
    if (hasCleanups)
      lp->setCleanup(true);
    for (llvm::Constant *ti : clauses)
      lp->addClause(ti);
    builder.CreateStore(builder.CreateExtractValue(lp, 0, name + ".exn"),
                        getOrCreateEHExnSlot());
    builder.CreateStore(builder.CreateExtractValue(lp, 1, name + ".sel"),
                        getOrCreateEHSelSlot());
    if (hasCleanups)
      emitScopeCleanupsInPad();
    builder.CreateBr(tryDispatchStack.back());
    builder.SetInsertPoint(saved);

    result = builder.CreateInvoke(fty, callee, cont, pad, args);
    builder.SetInsertPoint(cont);
    return result;
  }

  /* Outside any try but with destructor-bearing objects live: the invoke
   * routes to a landing pad that runs them and resumes unwinding. */
  if (cgCtx.hasActiveCleanups()) {
    llvm::BasicBlock *pad = llvm::BasicBlock::Create(ctx, name + ".pad", fn);
    llvm::BasicBlock *cont =
        llvm::BasicBlock::Create(ctx, name + ".cont", fn);

    llvm::BasicBlock *saved = builder.GetInsertBlock();
    builder.SetInsertPoint(pad);
    llvm::StructType *lpTy =
        llvm::StructType::get(ctx, {builder.getPtrTy(), builder.getInt32Ty()});
    llvm::LandingPadInst *lp =
        builder.CreateLandingPad(lpTy, 1, name + ".lp");
    lp->setCleanup(true);
    builder.CreateStore(builder.CreateExtractValue(lp, 0, name + ".exn"),
                        getOrCreateEHExnSlot());
    builder.CreateStore(builder.CreateExtractValue(lp, 1, name + ".sel"),
                        getOrCreateEHSelSlot());
    emitScopeCleanupsInPad();
    builder.CreateBr(getOrCreateEHResumeBlock());
    builder.SetInsertPoint(saved);

    result = builder.CreateInvoke(fty, callee, cont, pad, args);
    builder.SetInsertPoint(cont);
    return result;
  }

  return builder.CreateCall(fty, callee, args);
}

/* Returns the destructor for a record type, or null when there is none (or
 * it is implicit). */
static const FunctionDeclNode *getCustomDestructor(const Type *t) {
  const Type *u = t->getUnqualifiedType();
  if (u->getKind() != TypeKind::Class && u->getKind() != TypeKind::Struct &&
      u->getKind() != TypeKind::Union)
    return nullptr;
  auto *recTy = static_cast<const RecordType *>(u);
  const DeclNode *decl = recTy->getDeclaration();
  if (!decl)
    return nullptr;
  const FunctionDeclNode *dtor = nullptr;
  if (decl->kind == NodeKind::ClassDecl)
    dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
  else if (decl->kind == NodeKind::StructDecl)
    dtor = static_cast<const StructDeclNode *>(decl)->destructor;
  else if (decl->kind == NodeKind::UnionDecl)
    dtor = static_cast<const UnionDeclNode *>(decl)->destructor;
  if (!dtor || dtor->isImplicit)
    return nullptr;
  return dtor;
}


/* True when the record can be bit-copied safely: no custom destructor
 * anywhere in its member graph. */
static bool isTriviallyCopyable(
    const Type *t,
    llvm::SmallPtrSetImpl<const RecordType *> &visited) {
  const Type *u = t->getUnqualifiedType();
  if (u->getKind() == TypeKind::Array) {
    return isTriviallyCopyable(
        static_cast<const ArrayType *>(u)->getElementType(), visited);
  }
  if (u->getKind() != TypeKind::Class && u->getKind() != TypeKind::Struct &&
      u->getKind() != TypeKind::Union)
    return true;
  auto *recTy = static_cast<const RecordType *>(u);
  if (getCustomDestructor(recTy))
    return false;
  if (!visited.insert(recTy).second)
    return true; /* already on the walk: assume copyable to break cycles */
  for (const auto &f : recTy->getFields()) {
    if (!isTriviallyCopyable(f.type, visited))
      return false;
  }
  return true;
}

/* Finds 'operator=' on a record (single parameter of the record type). */
static const FunctionDeclNode *findAssignmentOperator(const Type *t) {
  const Type *u = t->getUnqualifiedType();
  if (u->getKind() != TypeKind::Class && u->getKind() != TypeKind::Struct &&
      u->getKind() != TypeKind::Union)
    return nullptr;
  auto *recTy = static_cast<const RecordType *>(u);
  const DeclNode *decl = recTy->getDeclaration();
  if (!decl)
    return nullptr;
  llvm::ArrayRef<FunctionDeclNode *> methods;
  if (decl->kind == NodeKind::ClassDecl)
    methods = static_cast<const ClassDeclNode *>(decl)->methods;
  else if (decl->kind == NodeKind::StructDecl)
    methods = static_cast<const StructDeclNode *>(decl)->methods;
  else if (decl->kind == NodeKind::UnionDecl)
    methods = static_cast<const UnionDeclNode *>(decl)->methods;
  for (auto *m : methods) {
    if (m->name != "operator=" || m->params.size() != 1)
      continue;
    const Type *p0 = m->params[0]->type;
    const Type *pointee = nullptr;
    if (p0->isReferenceType()) {
      pointee = static_cast<const ReferenceType *>(p0)->getPointeeType();
    } else if (p0->getKind() == TypeKind::RValueReference) {
      pointee = static_cast<const RValueReferenceType *>(p0)->getPointeeType();
    } else {
      pointee = p0;
    }
    if (pointee->getUnqualifiedType() == u)
      return m;
  }
  return nullptr;
}


/* Materializes a ternary branch value into an owned temporary, deep-copying
 * records with destructor-bearing members (a plain value store would share
 * String buffers with the branch temporaries, which die immediately). */
llvm::Value *
CodeGen::materializeTernaryBranchValue(llvm::Value *val, const ExprNode *expr) {
  llvm::AllocaInst *tmp = createEntryBlockAlloca(val->getType(),
                                                 "tmp.ternary.branch");
  const Type *valTy = expr->exprType;
  const Type *unqual = valTy->getUnqualifiedType();
  bool isAggregate = unqual->getKind() == TypeKind::Struct ||
                     unqual->getKind() == TypeKind::Class ||
                     unqual->getKind() == TypeKind::Union;
  if (isAggregate) {
    llvm::SmallPtrSet<const RecordType *, 8> visited;
    if (!isTriviallyCopyable(unqual, visited)) {
      llvm::AllocaInst *srcTmp = createEntryBlockAlloca(val->getType(),
                                                        "tmp.ternary.branch.src");
      createTBAAStore(val, srcTmp, valTy);
      emitMemberWiseCopy(tmp, srcTmp, unqual, false);
      return tmp;
    }
  }
  createTBAAStore(val, tmp, valTy);
  return tmp;
}

void CodeGen::emitMemberWiseCopy(llvm::Value *dst, llvm::Value *src,
                                 const Type *type, bool isAssignment) {
  const Type *unqual = type->getUnqualifiedType();
  const Type *elem = unqual;
  while (elem->getKind() == TypeKind::Array)
    elem = static_cast<const ArrayType *>(elem)->getElementType();
  bool isAggregate = elem->getKind() == TypeKind::Class ||
                     elem->getKind() == TypeKind::Struct ||
                     elem->getKind() == TypeKind::Union;

  /* Arrays: copy every element (elements with destructors route through
   * their own copy ctor / operator=). */
  if (unqual->getKind() == TypeKind::Array) {
    const auto *arrTy = static_cast<const ArrayType *>(unqual);
    llvm::Type *llArrTy = getLLVMType(unqual);
    uint64_t count = arrTy->getSize();
    for (uint64_t i = 0; i < count; i++) {
      /* Two indices: the first GEPs the array itself, the second scales
       * by the element size (a single index would scale by the whole
       * array type). */
      llvm::Value *zero = builder.getInt64(0);
      llvm::Value *idx = builder.getInt64(i);
      llvm::Value *dstE = builder.CreateInBoundsGEP(llArrTy, dst, {zero, idx},
                                                    "cpy.arr.dst");
      llvm::Value *srcE = builder.CreateInBoundsGEP(llArrTy, src, {zero, idx},
                                                    "cpy.arr.src");
      emitMemberWiseCopy(dstE, srcE, arrTy->getElementType(), isAssignment);
    }
    return;
  }

  if (!isAggregate) {
    /* Primitive (and pointer) members: plain value copy. */
    llvm::Value *v = builder.CreateLoad(getLLVMType(type), src, "cpy.prim");
    builder.CreateStore(v, dst);
    return;
  }

  /* Records with a copy constructor (or assignment operator) are copied
   * through it; records that are trivially copyable are bit-copied. */
  {
    llvm::SmallPtrSet<const RecordType *, 8> visited;
    if (isTriviallyCopyable(type, visited)) {
      llvm::Value *v = builder.CreateLoad(getLLVMType(type), src, "cpy.whole");
      builder.CreateStore(v, dst);
      return;
    }
    if (isAssignment) {
      if (const FunctionDeclNode *op = findAssignmentOperator(elem)) {
        llvm::Function *fn = getOrCreateFunction(op);
        emitCallOrInvoke(fn->getFunctionType(), fn, {dst, src});
        return;
      }
    } else {
      if (const FunctionDeclNode *cc = findCopyOrMoveCtor(type, false)) {
        llvm::Function *fn = getOrCreateFunction(cc);
        emitCallOrInvoke(fn->getFunctionType(), fn, {dst, src});
        return;
      }
    }
  }

  auto *recTy = static_cast<const RecordType *>(unqual);
  llvm::Type *llTy = getLLVMType(unqual);
  for (const auto &f : recTy->getFields()) {
    llvm::Value *dstF = builder.CreateStructGEP(llTy, dst, f.index, "cpy.dst");
    llvm::Value *srcF = builder.CreateStructGEP(llTy, src, f.index, "cpy.src");
    emitMemberWiseCopy(dstF, srcF, f.type, isAssignment);
  }
}

/* Finds the copy or move constructor for a record type; 'preferMove' picks
 * the rvalue-reference constructor first. Mirrors the by-value argument
 * materialization logic. */
static const FunctionDeclNode *findCopyOrMoveCtor(const Type *t,
                                                  bool preferMove) {
  const Type *u = t->getUnqualifiedType();
  if (u->getKind() != TypeKind::Class && u->getKind() != TypeKind::Struct &&
      u->getKind() != TypeKind::Union)
    return nullptr;
  auto *recTy = static_cast<const RecordType *>(u);
  const DeclNode *decl = recTy->getDeclaration();
  if (!decl)
    return nullptr;

  llvm::ArrayRef<FunctionDeclNode *> ctors;
  if (decl->kind == NodeKind::ClassDecl)
    ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
  else if (decl->kind == NodeKind::StructDecl)
    ctors = static_cast<const StructDeclNode *>(decl)->constructors;
  else if (decl->kind == NodeKind::UnionDecl)
    ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

  const Type *unqualParam = recTy;
  const FunctionDeclNode *moveCtor = nullptr;
  const FunctionDeclNode *copyCtor = nullptr;
  for (auto *c : ctors) {
    if (c->params.size() != 1)
      continue;
    const Type *p0 = c->params[0]->type;
    const Type *pointee = nullptr;
    if (p0->isReferenceType()) {
      pointee = static_cast<const ReferenceType *>(p0)->getPointeeType();
    } else if (p0->getKind() == TypeKind::RValueReference) {
      pointee = static_cast<const RValueReferenceType *>(p0)
                    ->getPointeeType();
    }
    if (!pointee || pointee->getUnqualifiedType() != unqualParam)
      continue;
    if (p0->getKind() == TypeKind::RValueReference)
      moveCtor = c;
    else if (p0->isReferenceType())
      copyCtor = c;
  }
  if (preferMove)
    return moveCtor ? moveCtor : copyCtor;
  return copyCtor ? copyCtor : moveCtor;
}

llvm::Function *
CodeGen::getOrCreateFutureValueDtor(const Type *valueType) {
  if (!valueType || valueType->isVoid())
    return nullptr;
  const FunctionDeclNode *dtor = getCustomDestructor(valueType);
  if (!dtor)
    return nullptr;

  std::string key =
      "future_dtor_" +
      std::string(valueType ? valueType->toString() : "void");
  auto it = asyncHelpers.find(key);
  if (it != asyncHelpers.end())
    return it->second;

  llvm::FunctionType *ty = llvm::FunctionType::get(
      builder.getVoidTy(), {builder.getPtrTy()}, false);
  auto *fn = llvm::Function::Create(ty, llvm::Function::InternalLinkage, key,
                                    &mod);

  auto savedIP = builder.saveIP();
  auto savedLoc = builder.getCurrentDebugLocation();
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
  builder.SetInsertPoint(entry);

  llvm::Type *objTy = getLLVMType(valueType);
  llvm::Value *objPtr = builder.CreateBitCast(
      fn->getArg(0), llvm::PointerType::getUnqual(objTy));
  emitCleanupCall(objPtr, dtor, valueType);
  builder.CreateRetVoid();

  builder.restoreIP(savedIP);
  builder.SetCurrentDebugLocation(savedLoc);

  asyncHelpers[key] = fn;
  return fn;
}

/* 'void thunk(ptr valuePtr, ptr cb, ptr resultState)' — used by
 * Future.then. When 'asyncCb' is set, the callback returns a Future<void>
 * which is chained into resultState; otherwise the callback returns void
 * and resultState is completed immediately after the call. */
llvm::Function *CodeGen::getOrCreateThenThunk(const Type *valueType,
                                              bool asyncCb,
                                              bool cbTakesValue) {
  std::string key = std::string("then_thunk_") + (asyncCb ? "a_" : "s_") +
                    (cbTakesValue ? "v_" : "n_") +
                    std::string(valueType ? valueType->toString() : "void");
  auto it = asyncHelpers.find(key);
  if (it != asyncHelpers.end())
    return it->second;

  llvm::FunctionType *ty = llvm::FunctionType::get(
      builder.getVoidTy(),
      {builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy()}, false);
  auto *fn = llvm::Function::Create(ty, llvm::Function::InternalLinkage, key,
                                    &mod);

  auto savedIP = builder.saveIP();
  auto savedLoc = builder.getCurrentDebugLocation();
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
  builder.SetInsertPoint(entry);

  llvm::Value *valuePtr = fn->getArg(0);
  llvm::Value *cb = fn->getArg(1);
  llvm::Value *resultState = fn->getArg(2);

  bool isVoidValue = !valueType || valueType->isVoid();
  std::vector<llvm::Type *> cbParamTys;
  llvm::Value *val = nullptr;
  if (!isVoidValue && cbTakesValue) {
    llvm::Type *llTy = getLLVMType(valueType);
    llvm::Value *vp = builder.CreateBitCast(
        valuePtr, llvm::PointerType::getUnqual(llTy));
    const FunctionDeclNode *dtor = getCustomDestructor(valueType);
    if (dtor) {
      llvm::AllocaInst *tmp = createEntryBlockAlloca(llTy, "then.value.tmp");
      emitDefaultInitialization(tmp, valueType);
      const FunctionDeclNode *ctor =
          findCopyOrMoveCtor(valueType, /*preferMove=*/false);
      if (ctor) {
        llvm::Function *ctorFunc = getOrCreateFunction(ctor);
        builder.CreateCall(ctorFunc, {tmp, vp});
      } else {
        llvm::Align align = mod.getDataLayout().getABITypeAlign(llTy);
        uint64_t size = mod.getDataLayout().getTypeAllocSize(llTy);
        builder.CreateMemCpy(tmp, align, vp, align, size);
      }
      /* The callback owns the by-value parameter (its destructor runs on
       * the callee side), mirroring the by-value argument convention. */
      val = createTBAALoad(llTy, tmp, valueType);
    } else {
      val = createTBAALoad(llTy, vp, valueType);
    }
    cbParamTys.push_back(llTy);
  }

  /* Call the callback through its pointer. */
  llvm::Type *cbRetTy = asyncCb
                              ? static_cast<llvm::Type *>(builder.getPtrTy())
                              : builder.getVoidTy();
  llvm::FunctionType *cbFnTy =
      llvm::FunctionType::get(cbRetTy, cbParamTys, false);
  llvm::Value *cbPtr = builder.CreateBitCast(
      cb, llvm::PointerType::getUnqual(cbFnTy));

  llvm::Value *ret = nullptr;
  if (cbParamTys.empty()) {
    ret = builder.CreateCall(cbFnTy, cbPtr, {});
  } else {
    ret = builder.CreateCall(cbFnTy, cbPtr, {val});
  }
  if (asyncCb) {
    emitRuntimeCall("utopia_future_chain", builder.getVoidTy(),
                    {ret, resultState});
  } else {
    emitRuntimeCall("utopia_future_complete", builder.getVoidTy(),
                    {resultState});
  }
  builder.CreateRetVoid();

  builder.restoreIP(savedIP);
  builder.SetCurrentDebugLocation(savedLoc);

  asyncHelpers[key] = fn;
  return fn;
}

/* 'void thunk(ptr state, ptr fn)' — worker-thread entry used by
 * Future.runOnThread: calls fn, stores the result into the state and
 * completes it. */
llvm::Function *CodeGen::getOrCreateThreadThunk(const Type *valueType) {
  std::string key =
      "thread_thunk_" +
      std::string(valueType ? valueType->toString() : "void");
  auto it = asyncHelpers.find(key);
  if (it != asyncHelpers.end())
    return it->second;

  llvm::FunctionType *ty = llvm::FunctionType::get(
      builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
  auto *fn = llvm::Function::Create(ty, llvm::Function::InternalLinkage, key,
                                    &mod);

  auto savedIP = builder.saveIP();
  auto savedLoc = builder.getCurrentDebugLocation();
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
  builder.SetInsertPoint(entry);

  llvm::Value *state = fn->getArg(0);
  llvm::Value *fnPtr = fn->getArg(1);

  bool isVoidValue = !valueType || valueType->isVoid();
  llvm::Value *retVal = nullptr;
  if (!isVoidValue) {
    llvm::Type *retTy = getLLVMType(valueType);
    llvm::FunctionType *fnTy = llvm::FunctionType::get(retTy, {}, false);
    llvm::Value *callee = builder.CreateBitCast(
        fnPtr, llvm::PointerType::getUnqual(fnTy));
    retVal = builder.CreateCall(fnTy, callee, {});
    writeFutureValueInto(state, retVal, valueType, false);
  } else {
    llvm::FunctionType *fnTy =
        llvm::FunctionType::get(builder.getVoidTy(), {}, false);
    llvm::Value *callee = builder.CreateBitCast(
        fnPtr, llvm::PointerType::getUnqual(fnTy));
    builder.CreateCall(fnTy, callee, {});
  }

  emitRuntimeCall("utopia_future_complete", builder.getVoidTy(), {state});
  builder.CreateRetVoid();

  builder.restoreIP(savedIP);
  builder.SetCurrentDebugLocation(savedLoc);

  asyncHelpers[key] = fn;
  return fn;
}

/* 'void thunk(ptr state, ptr fn)' — worker-thread entry for an async
 * function passed to Future.runOnThread. The worker calls fn (which runs
 * the async function's coroutine up to its first await), then pumps its own
 * event loop until the returned future completes and stores the resulting
 * value into the outer state. This gives every worker thread its own
 * runtime copy, so awaits keep working off the main thread. */
llvm::Function *CodeGen::getOrCreateAsyncThreadThunk(const Type *valueType) {
  std::string key =
      "async_thread_thunk_" +
      std::string(valueType ? valueType->toString() : "void");
  auto it = asyncHelpers.find(key);
  if (it != asyncHelpers.end())
    return it->second;

  llvm::FunctionType *ty = llvm::FunctionType::get(
      builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
  auto *fn = llvm::Function::Create(ty, llvm::Function::InternalLinkage, key,
                                    &mod);

  auto savedIP = builder.saveIP();
  auto savedLoc = builder.getCurrentDebugLocation();
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
  builder.SetInsertPoint(entry);

  llvm::Value *state = fn->getArg(0);
  llvm::Value *fnPtr = fn->getArg(1);

  /* The future struct is {ptr state}; call fn and pull the state pointer out
   * of the returned struct. */
  llvm::Type *llFutTy = llvm::StructType::get(
      ctx, {builder.getPtrTy()}, /*isPacked=*/false);
  llvm::FunctionType *fnTy = llvm::FunctionType::get(llFutTy, {}, false);
  llvm::Value *callee = builder.CreateBitCast(
      fnPtr, llvm::PointerType::getUnqual(fnTy));
  llvm::Value *fut = builder.CreateCall(fnTy, callee, {});
  llvm::Value *innerState =
      builder.CreateExtractValue(fut, 0, "async.fn.state");

  /* Pump this thread's loop until the async function completes. */
  emitRuntimeCall("utopia_loop_run", builder.getInt32Ty(), {innerState});

  bool isVoidValue = !valueType || valueType->isVoid();
  if (!isVoidValue) {
    llvm::Value *retVal = readFutureValue(innerState, valueType);
    writeFutureValueInto(state, retVal, valueType, false);
  }

  emitRuntimeCall("utopia_future_complete", builder.getVoidTy(), {state});

  /* The by-value future returned from the call does not run a destructor;
   * release the reference it carried now that the value was copied out. */
  emitRuntimeCall("utopia_future_release", builder.getVoidTy(), {innerState});

  builder.CreateRetVoid();

  builder.restoreIP(savedIP);
  builder.SetCurrentDebugLocation(savedLoc);

  asyncHelpers[key] = fn;
  return fn;
}

llvm::Value *CodeGen::getFutureObjectPointer(const ExprNode *expr) {
  if (!expr)
    return nullptr;

  const Type *t = expr->exprType;
  const Type *u = t ? t->getUnqualifiedType() : nullptr;
  bool isIndirect = u && (u->isPointerType() || u->isReferenceType() ||
                          u->getKind() == TypeKind::RValueReference);

  llvm::Value *objPtr = nullptr;
  if (isIndirect) {
    /* Pointer operands (e.g. 'this') carry the object address in the
     * value; reference operands (e.g. an operator[] returning T&) need the
     * l-value resolution. */
    bool isPointerOperand = u && u->isPointerType();
    if (isPointerOperand) {
      objPtr = dispatch(expr);
      if (!objPtr)
        objPtr = getLValue(expr);
    } else {
      objPtr = getLValue(expr);
      if (!objPtr)
        objPtr = dispatch(expr);
    }
  } else {
    objPtr = getLValue(expr);
    if (!objPtr) {
      lastTemporaryAlloca = nullptr;
      llvm::Value *val = dispatch(expr);
      if (lastTemporaryAlloca) {
        objPtr = lastTemporaryAlloca;
        lastTemporaryAlloca = nullptr;
      } else if (val) {
        objPtr = createEntryBlockAlloca(val->getType(), "tmp.future.recv");
        createTBAAStore(val, objPtr, expr->exprType);
      }
    }
  }
  return objPtr;
}

llvm::Value *CodeGen::getFutureState(llvm::Value *futureValueOrObjPtr,
                                       const Type *operandType) {
  if (!futureValueOrObjPtr)
    return nullptr;

  /* A Future is a struct whose single field is the runtime state pointer,
   * whether it is used by value or heap-allocated: one dereference from
   * the operand's address reaches the state. */
  const Type *u = operandType ? operandType->getUnqualifiedType() : nullptr;
  const Type *recordTy = u;
  while (recordTy &&
         (recordTy->isPointerType() || recordTy->isReferenceType() ||
          recordTy->getKind() == TypeKind::RValueReference)) {
    if (recordTy->isPointerType())
      recordTy = static_cast<const PointerType *>(recordTy)
                     ->getPointeeType()
                     ->getUnqualifiedType();
    else if (recordTy->isReferenceType())
      recordTy = static_cast<const ReferenceType *>(recordTy)
                     ->getPointeeType()
                     ->getUnqualifiedType();
    else
      recordTy = static_cast<const RValueReferenceType *>(recordTy)
                     ->getPointeeType()
                     ->getUnqualifiedType();
  }

  llvm::Type *llTy = nullptr;
  uint32_t stateFieldIndex = 0;
  if (auto *rec = llvm::dyn_cast<RecordType>(recordTy)) {
    if (const FieldInfo *f = rec->getField("_state")) {
      stateFieldIndex = f->index;
    }
    llTy = getLLVMType(rec);
  }
  if (!llTy || !llTy->isStructTy()) {
    llTy = builder.getPtrTy();
  }

  llvm::Value *stateGep = builder.CreateStructGEP(
      llTy, futureValueOrObjPtr, stateFieldIndex, "future.state.gep");
  return builder.CreateLoad(builder.getPtrTy(), stateGep, "future.state");
}
llvm::Value *CodeGen::readFutureValue(llvm::Value *state,
                                      const Type *valueType) {
  if (!state || !valueType || valueType->isVoid())
    return nullptr;

  llvm::Type *llTy = getLLVMType(valueType);
  llvm::Value *vp =
      emitRuntimeCall("utopia_future_value_ptr", builder.getPtrTy(), {state});
  vp = builder.CreateBitCast(vp, llvm::PointerType::getUnqual(llTy),
                             "future.value.ptr");

  const FunctionDeclNode *dtor = getCustomDestructor(valueType);
  if (!dtor) {
    return createTBAALoad(llTy, vp, valueType);
  }

  /* Non-trivial value: move (or copy) it out of the state into a temporary
   * owned by the current function. */
  llvm::AllocaInst *tmp = createEntryBlockAlloca(llTy, "await.value.tmp");
  emitDefaultInitialization(tmp, valueType);
  const FunctionDeclNode *ctor =
      findCopyOrMoveCtor(valueType, /*preferMove=*/true);
  if (ctor) {
    llvm::Function *ctorFunc = getOrCreateFunction(ctor);
    builder.CreateCall(ctorFunc, {tmp, vp});
  } else {
    llvm::Align align = mod.getDataLayout().getABITypeAlign(llTy);
    uint64_t size = mod.getDataLayout().getTypeAllocSize(llTy);
    builder.CreateMemCpy(tmp, align, vp, align, size);
  }
  cgCtx.addCleanup(tmp, dtor, valueType);
  lastTemporaryAlloca = tmp;
  return tmp;
}

void CodeGen::writeFutureValueInto(llvm::Value *state, llvm::Value *src,
                                   const Type *valueType, bool srcIsLValue) {
  if (!state || !src || !valueType || valueType->isVoid())
    return;

  llvm::Type *llTy = getLLVMType(valueType);
  llvm::Value *vp =
      emitRuntimeCall("utopia_future_value_ptr", builder.getPtrTy(), {state});
  vp = builder.CreateBitCast(vp, llvm::PointerType::getUnqual(llTy),
                             "future.value.ptr");

  const FunctionDeclNode *dtor = getCustomDestructor(valueType);
  if (!dtor) {
    if (srcIsLValue) {
      src = builder.CreateLoad(llTy, src, "future.value.load");
    }
    src = createImplicitCast(src, llTy);
    createTBAAStore(src, vp, valueType);
    return;
  }

  llvm::Value *srcPtr = src;
  llvm::AllocaInst *internalTemp = nullptr;
  if (!srcIsLValue) {
    internalTemp = createEntryBlockAlloca(llTy, "future.value.src");
    createTBAAStore(src, internalTemp, valueType);
    srcPtr = internalTemp;
  }

  emitDefaultInitialization(vp, valueType);
  const FunctionDeclNode *ctor =
      findCopyOrMoveCtor(valueType, /*preferMove=*/!srcIsLValue);
  if (ctor) {
    llvm::Function *ctorFunc = getOrCreateFunction(ctor);
    builder.CreateCall(ctorFunc, {vp, srcPtr});
  } else {
    llvm::Align align = mod.getDataLayout().getABITypeAlign(llTy);
    uint64_t size = mod.getDataLayout().getTypeAllocSize(llTy);
    builder.CreateMemCpy(vp, align, srcPtr, align, size);
  }

  /* The internally materialized temporary owns the original storage; release
   * it now that the state owns its own copy. Real l-values keep their own
   * lifetime management. */
  if (internalTemp) {
    emitCleanupCall(srcPtr, dtor, valueType);
  }
}

/* Wraps a future state pointer into the Future value type expected by the
 * caller of an async function or intrinsic. */
llvm::Value *CodeGen::materializeFutureValue(const Type *futureType,
                                             llvm::Value *statePtr) {
  const Type *u = futureType->getUnqualifiedType();
  llvm::Type *llFutTy = getLLVMType(u);
  llvm::AllocaInst *tmp = createEntryBlockAlloca(llFutTy, "future.value");
  emitDefaultInitialization(tmp, futureType);
  llvm::Value *gep = builder.CreateStructGEP(llFutTy, tmp, 0, "future.ptr.gep");
  builder.CreateStore(statePtr, gep);
  if (const FunctionDeclNode *dtor = getCustomDestructor(futureType)) {
    cgCtx.addCleanup(tmp, dtor, futureType);
  }
  /* The temporary owns the state (its destructor releases it). Expose it as
   * the last temporary so copy-initializations ('Future<int> f = value()'),
   * member calls ('value().then(...)') and by-value arguments copy/move from
   * it directly. Without this, the loaded struct value is stored into a raw
   * byte-copy temporary that never retains the state: the future then has
   * more owners than references and its destructor double-releases. */
  lastTemporaryAlloca = tmp;
  return createTBAALoad(llFutTy, tmp, futureType);
}

llvm::Value *CodeGen::createFutureObject(const Type *futureType,
                                         llvm::Value *state) {
  (void)futureType;
  /* A Future is a value type whose single field is the runtime state
   * pointer; no separate heap object is needed. */
  return state;
}

llvm::Value *CodeGen::createFutureState(const Type *valueType) {
  uint64_t size = 0;
  uint32_t align = 1;
  if (valueType && !valueType->isVoid()) {
    llvm::Type *llTy = getLLVMType(valueType);
    size = mod.getDataLayout().getTypeAllocSize(llTy);
    align = mod.getDataLayout().getABITypeAlign(llTy).value();
  }
  llvm::Function *dtor = getOrCreateFutureValueDtor(valueType);
  llvm::Value *dtorVal = dtor
                              ? static_cast<llvm::Value *>(dtor)
                              : static_cast<llvm::Value *>(
                                    llvm::ConstantPointerNull::get(
                                        builder.getPtrTy()));
  return emitRuntimeCall("utopia_future_create", builder.getPtrTy(),
                         {builder.getInt64(size), builder.getInt32(align),
                          dtorVal});
}

void CodeGen::setupAsyncFunction(const FunctionDeclNode *node,
                                 llvm::Function *func) {
  coroInfo = std::make_unique<CoroutineInfo>();

  const Type *inner = nullptr;
  coroInfo->valueType = unwrapFutureType(node->returnType, &inner)
                            ? inner
                            : node->returnType;
  coroInfo->futureType = node->effectiveReturnType;
  if (!coroInfo->futureType) {
    coroInfo->futureType = node->returnType;
  }
  coroInfo->isMain = (node->name == "main" && !node->isMethod);
  coroInfo->isCoroutine = node->hasAwait;

  llvm::Value *state = createFutureState(coroInfo->valueType);
  coroInfo->futureStateSlot =
      createEntryBlockAlloca(builder.getPtrTy(), "future.state.slot");
  builder.CreateStore(state, coroInfo->futureStateSlot);

  /* The coroutine's frame holds a reference to the state so it survives the
   * caller dropping the returned Future before the coroutine completes. */
  emitRuntimeCall("utopia_future_retain", builder.getVoidTy(), {state});

  llvm::Value *futObj = createFutureObject(coroInfo->futureType, state);
  (void)futObj;
  /* The Future value and the state are the same pointer; the ramp returns
   * the state, which is stored in a frame-resident slot so the suspend
   * block can reload it after a resume. */

  if (!coroInfo->isCoroutine) {
    /* No suspension points: the body runs to completion synchronously and
     * completes the future on the way out (see visit(ReturnNode)). */
    return;
  }

  llvm::Function *coroIdFn =
      llvm::Intrinsic::getDeclaration(&mod, llvm::Intrinsic::coro_id);
  llvm::Function *coroSizeFn = llvm::Intrinsic::getDeclaration(
      &mod, llvm::Intrinsic::coro_size, {builder.getInt64Ty()});
  llvm::Function *coroBeginFn =
      llvm::Intrinsic::getDeclaration(&mod, llvm::Intrinsic::coro_begin);

  llvm::Value *id = builder.CreateCall(
      coroIdFn,
      {builder.getInt32(0), llvm::ConstantPointerNull::get(builder.getPtrTy()),
       llvm::ConstantPointerNull::get(builder.getPtrTy()),
       llvm::ConstantPointerNull::get(builder.getPtrTy())});
  coroInfo->coroId = id;

  llvm::Value *frameSize = builder.CreateCall(coroSizeFn, {});
  llvm::Function *mallocFn = mod.getFunction("malloc");
  if (!mallocFn) {
    llvm::FunctionType *mallocTy = llvm::FunctionType::get(
        builder.getPtrTy(), {builder.getInt64Ty()}, false);
    mallocFn = llvm::Function::Create(
        mallocTy, llvm::Function::ExternalLinkage, "malloc", mod);
  }
  llvm::Value *frameMem = builder.CreateCall(mallocFn, {frameSize});
  llvm::Value *hdl = builder.CreateCall(coroBeginFn, {id, frameMem});

  coroInfo->frameSlot =
      createEntryBlockAlloca(builder.getPtrTy(), "coro.frame.slot");
  builder.CreateStore(hdl, coroInfo->frameSlot);

  /* Shared suspend / cleanup exits for every await site. */
  coroInfo->suspendBlock =
      llvm::BasicBlock::Create(ctx, "coro.suspend", func);
  coroInfo->cleanupBlock =
      llvm::BasicBlock::Create(ctx, "coro.cleanup", func);

  /* Fill the suspend exit: this is where every await lands on its first
   * suspension. The ramp returns the future object here; when the block is
   * reached from the resume function, CoroSplit rewrites the return. */
  {
    auto savedIP = builder.saveIP();
    auto savedLoc = builder.getCurrentDebugLocation();
    builder.SetInsertPoint(coroInfo->suspendBlock);

    llvm::Value *susHdl = builder.CreateLoad(builder.getPtrTy(),
                                             coroInfo->frameSlot,
                                             "coro.frame");
    llvm::Function *coroEndFn = llvm::Intrinsic::getDeclaration(
        &mod, llvm::Intrinsic::coro_end);
    builder.CreateCall(coroEndFn,
                       {susHdl, builder.getInt1(false), coroInfo->coroId});
    llvm::Value *susFut = builder.CreateLoad(
        builder.getPtrTy(), coroInfo->futureStateSlot, "future.obj");
    builder.CreateRet(susFut);

    /* Fill the destroy exit: the coroutine was destroyed while suspended.
     * Release the frame's future-state reference, then the coro.free + free
     * pair releases the frame. */
    builder.SetInsertPoint(coroInfo->cleanupBlock);
    llvm::Value *clnHdl = builder.CreateLoad(builder.getPtrTy(),
                                             coroInfo->frameSlot,
                                             "coro.frame");
    llvm::Value *clnState = builder.CreateLoad(
        builder.getPtrTy(), coroInfo->futureStateSlot, "future.state");
    emitRuntimeCall("utopia_future_release", builder.getVoidTy(), {clnState});
    llvm::Function *coroFreeFn = llvm::Intrinsic::getDeclaration(
        &mod, llvm::Intrinsic::coro_free);
    llvm::Value *toFree =
        builder.CreateCall(coroFreeFn, {coroInfo->coroId, clnHdl});
    llvm::Function *freeFn = mod.getFunction("free");
    if (!freeFn) {
      llvm::FunctionType *freeTy = llvm::FunctionType::get(
          builder.getVoidTy(), {builder.getPtrTy()}, false);
      freeFn = llvm::Function::Create(
          freeTy, llvm::Function::ExternalLinkage, "free", mod);
    }
    builder.CreateCall(freeFn, {toFree});
    builder.CreateBr(coroInfo->suspendBlock);

    builder.restoreIP(savedIP);
    builder.SetCurrentDebugLocation(savedLoc);
  }
}

/* Emits the final suspend path: complete the future, mark the coroutine as
 * done, self-destroy (frees the frame) and return the future object. */
void CodeGen::emitAsyncReturn(const FunctionDeclNode *node,
                              llvm::Value *value, bool valueIsLValue) {
  llvm::Value *state = builder.CreateLoad(
      builder.getPtrTy(), coroInfo->futureStateSlot, "future.state");
  if (value) {
    writeFutureValueInto(state, value, coroInfo->valueType, valueIsLValue);
  }
  emitRuntimeCall("utopia_future_complete", builder.getVoidTy(), {state});

  /* The coroutine's own frame reference: the caller may drop the returned
   * Future (fire-and-forget), so the state must stay alive until the
   * coroutine is done. For real coroutines the release happens in the
   * destroy function (which coro.destroy below invokes on the completion
   * path); releasing here as well would double-release. Plain async
   * functions (no awaits) have no destroy function, so they release here. */
  if (!coroInfo->isCoroutine) {
    emitRuntimeCall("utopia_future_release", builder.getVoidTy(), {state});
  }

  /* Load the future (the state pointer) before the frame is destroyed. */
  llvm::Value *fut = builder.CreateLoad(
      builder.getPtrTy(), coroInfo->futureStateSlot, "future.obj");

  if (coroInfo->isCoroutine) {
    llvm::Function *coroEndFn = llvm::Intrinsic::getDeclaration(
        &mod, llvm::Intrinsic::coro_end);
    llvm::Function *coroDestroyFn = llvm::Intrinsic::getDeclaration(
        &mod, llvm::Intrinsic::coro_destroy);
    llvm::Value *hdl = builder.CreateLoad(builder.getPtrTy(),
                                          coroInfo->frameSlot, "coro.frame");

    /* Mark the coroutine done, then self-destroy (freeing the frame). */
    builder.CreateCall(coroEndFn,
                       {hdl, builder.getInt1(true), coroInfo->coroId});
    builder.CreateCall(coroDestroyFn, {hdl});
    builder.CreateRet(fut);
    return;
  }

  builder.CreateRet(fut);
}

void CodeGen::emitAsyncFallthroughFinish(const FunctionDeclNode *node) {
  /* The value type is non-void only when every path returned (Sema);
   * fall-through with a non-void value is a compile-time error already. */
  emitAsyncReturn(node, nullptr, false);
}

void CodeGen::emitMainWrapper(llvm::Function *userMain,
                              const FunctionDeclNode *node) {
  if (mod.getFunction("main"))
    return;

  std::vector<llvm::Type *> paramTys;
  for (const auto *p : node->params) {
    if (llvm::isa<ArrayType>(p->type)) {
      paramTys.push_back(builder.getPtrTy());
    } else {
      paramTys.push_back(getLLVMType(p->type));
    }
  }
  llvm::FunctionType *ft =
      llvm::FunctionType::get(builder.getInt32Ty(), paramTys, false);
  auto *mainFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                        "main", &mod);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", mainFn);
  builder.SetInsertPoint(entry);

  std::vector<llvm::Value *> callArgs;
  for (auto &arg : mainFn->args()) {
    callArgs.push_back(&arg);
  }
  llvm::Value *fut = builder.CreateCall(userMain, callArgs);

  if (node->isAsync) {
    emitRuntimeCall("utopia_loop_run", builder.getInt32Ty(), {fut});
    /* Like Dart, stay alive while fire-and-forget work (timers, worker
     * threads) is still pending after main completed. */
    emitRuntimeCall("utopia_loop_run_all", builder.getVoidTy(), {});
    builder.CreateRet(builder.getInt32(0));
  } else {
    /* Synchronous main: drive the event loop until every future scheduled
     * during main has settled, mirroring the Dart event loop semantics.
     * The user's exit code must reach the OS: scripts and CI depend on it,
     * and 'utopia run' forwards it as the command's own exit code. */
    emitRuntimeCall("utopia_loop_run_all", builder.getVoidTy(), {});
    if (fut->getType()->isIntegerTy(32)) {
      builder.CreateRet(fut);
    } else {
      builder.CreateRet(
          llvm::ConstantInt::get(builder.getInt32Ty(), 0));
    }
  }
}

llvm::Value *CodeGen::visit(const AwaitExprNode *node) {
  if (!coroInfo || !coroInfo->isCoroutine) {
    return dispatch(node->expr);
  }

  /* 'Future<T> a = await fut;': the await is consumed as the future
   * itself, so the operand passes through unwrapped. */
  if (node->keepFuture) {
    node->exprType = node->expr->exprType;
    return dispatch(node->expr);
  }

  const Type *valueTy = nullptr;
  if (!unwrapFutureType(node->expr->exprType, &valueTy)) {
    return dispatch(node->expr);
  }

  llvm::Value *futObj = getFutureObjectPointer(node->expr);
  if (!futObj) {
    diags.report({DiagLevel::Error, node->line, node->column, node->length,
                  "Failed to evaluate await operand.", currentFilePath});
    return nullptr;
  }
  llvm::Value *state = getFutureState(futObj, node->expr->exprType);

  llvm::Function *func = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *doneBB = llvm::BasicBlock::Create(ctx, "await.done", func);
  llvm::BasicBlock *regBB =
      llvm::BasicBlock::Create(ctx, "await.register", func);
  llvm::BasicBlock *resumedBB =
      llvm::BasicBlock::Create(ctx, "await.resumed", func);
  llvm::BasicBlock *afterBB =
      llvm::BasicBlock::Create(ctx, "await.after", func);

  /* The runtime returns an int (C ABI): normalize to i1. */
  llvm::Value *done32 = emitRuntimeCall("utopia_future_is_completed",
                                        builder.getInt32Ty(), {state});
  llvm::Value *done =
      builder.CreateICmpNE(done32, builder.getInt32(0), "await.done");
  builder.CreateCondBr(done, doneBB, regBB);

  /* Already completed: take the value inline. */
  builder.SetInsertPoint(doneBB);
  builder.CreateBr(afterBB);

  /* Pending: register our resume function and suspend. */
  builder.SetInsertPoint(regBB);
  llvm::Value *hdl =
      builder.CreateLoad(builder.getPtrTy(), coroInfo->frameSlot,
                         "coro.frame");
  llvm::Value *resumeFn =
      builder.CreateLoad(builder.getPtrTy(), hdl, "coro.resume.fn");
  emitRuntimeCall("utopia_future_then", builder.getVoidTy(),
                  {state, resumeFn, hdl});

  llvm::Function *coroSuspendFn = llvm::Intrinsic::getDeclaration(
      &mod, llvm::Intrinsic::coro_suspend);
  llvm::Value *s =
      builder.CreateCall(coroSuspendFn, {coroInfo->coroId, builder.getInt1(false)});
  llvm::SwitchInst *sw =
      builder.CreateSwitch(s, coroInfo->suspendBlock, 2);
  sw->addCase(builder.getInt8(0), resumedBB);
  sw->addCase(builder.getInt8(1), coroInfo->cleanupBlock);

  builder.SetInsertPoint(resumedBB);
  builder.CreateBr(afterBB);

  /* The value is read once, after either path converges. */
  builder.SetInsertPoint(afterBB);
  llvm::Value *val = readFutureValue(state, valueTy);
  node->exprType = valueTy;
  return val;
}

llvm::Value *CodeGen::lookupThis(const ASTNode *errSite) {
  SymbolInfo sym = cgCtx.lookupDetailed("this");
  if (sym.value)
    return sym.value;
  reportError(errSite->line, errSite->column, errSite->length,
              "'this' is not bound in this context (super/field access "
              "outside an instance method).");
  return nullptr;
}

} // namespace utopia