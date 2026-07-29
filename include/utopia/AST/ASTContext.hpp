#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/Common/Types.hpp"

#include <llvm/Support/Compiler.h>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/Allocator.h>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace utopia {

/*
 * High-performance centralized memory management for the AST and canonical
 * types.
 */
class ASTContext {
public:
  const BuiltinType *VoidTy;
  const BuiltinType *BoolTy;
  const BuiltinType *Int8Ty;
  const BuiltinType *Int16Ty;
  const BuiltinType *Int32Ty;
  const BuiltinType *Int64Ty;
  const BuiltinType *UInt8Ty;
  const BuiltinType *UInt16Ty;
  const BuiltinType *UInt32Ty;
  const BuiltinType *UInt64Ty;
  const BuiltinType *Float32Ty;
  const BuiltinType *Float64Ty;

  ASTContext() {
    VoidTy = create<BuiltinType>(BuiltinKind::Void);
    BoolTy = create<BuiltinType>(BuiltinKind::Bool);
    Int8Ty = create<BuiltinType>(BuiltinKind::Int8);
    Int16Ty = create<BuiltinType>(BuiltinKind::Int16);
    Int32Ty = create<BuiltinType>(BuiltinKind::Int32);
    Int64Ty = create<BuiltinType>(BuiltinKind::Int64);
    UInt8Ty = create<BuiltinType>(BuiltinKind::UInt8);
    UInt16Ty = create<BuiltinType>(BuiltinKind::UInt16);
    UInt32Ty = create<BuiltinType>(BuiltinKind::UInt32);
    UInt64Ty = create<BuiltinType>(BuiltinKind::UInt64);
    Float32Ty = create<BuiltinType>(BuiltinKind::Float32);
    Float64Ty = create<BuiltinType>(BuiltinKind::Float64);
  }
  ~ASTContext() = default;

  ASTContext(const ASTContext &) = delete;
  ASTContext &operator=(const ASTContext &) = delete;

  template <typename T, typename... Args> T *create(Args &&...args) {
    void *mem = allocator.Allocate<T>();
    return new (mem) T(std::forward<Args>(args)...);
  }

  template <typename T> llvm::ArrayRef<T> copyArray(llvm::ArrayRef<T> src) {
    if (src.empty())
      return llvm::ArrayRef<T>();
    T *mem = allocator.Allocate<T>(src.size());
    std::uninitialized_copy(src.begin(), src.end(), mem);
    return llvm::ArrayRef<T>(mem, src.size());
  }

  std::string_view copyString(std::string_view src) {
    if (src.empty())
      return "";
    char *mem = allocator.Allocate<char>(src.size());
    std::copy(src.begin(), src.end(), mem);
    return std::string_view(mem, src.size());
  }

  /* Returns the canonical pointer type for the given pointee. */
  const PointerType *getPointerType(const Type *pointee) {
    auto it = pointerTypes.find(pointee);
    if (it != pointerTypes.end())
      return it->second;

    auto *ptrTy = create<PointerType>(pointee);
    pointerTypes[pointee] = ptrTy;
    return ptrTy;
  }

  /* Returns the canonical reference type for the given pointee. */
  const ReferenceType *getReferenceType(const Type *pointee) {
    auto it = referenceTypes.find(pointee);
    if (it != referenceTypes.end())
      return it->second;

    auto *refTy = create<ReferenceType>(pointee);
    referenceTypes[pointee] = refTy;
    return refTy;
  }

  const ConstType *getConstType(const Type *base) {
    auto it = constTypes.find(base);
    if (it != constTypes.end())
      return it->second;

    auto *constTy = create<ConstType>(base);
    constTypes[base] = constTy;
    return constTy;
  }

  RecordType *createRecordType(TypeKind kind, std::string_view name) {
    auto it = recordTypes.find(name);
    if (it != recordTypes.end()) {
      return it->second;
    }
    RecordType *rec = nullptr;
    if (kind == TypeKind::Struct) {
      rec = create<StructType>(name);
    } else {
      rec = create<ClassType>(name);
    }
    recordTypes[name] = rec;
    return rec;
  }

  const TemplateParamType *getTemplateParamType(std::string_view name) {
    return create<TemplateParamType>(name);
  }

  const TemplateInstType *
  getTemplateInstType(std::string_view name,
                      llvm::ArrayRef<const Type *> args) {
    return create<TemplateInstType>(name, args);
  }

  RecordType *getRecordType(std::string_view name) {
    auto it = recordTypes.find(name);
    return it != recordTypes.end() ? it->second : nullptr;
  }

  const Type *getBuiltinTypeByName(std::string_view name) {
    if (name == "int" || name == "int32")
      return Int32Ty;
    if (name == "int64")
      return Int64Ty;
    if (name == "int16")
      return Int16Ty;
    if (name == "int8")
      return Int8Ty;
    if (name == "uint" || name == "uint32")
      return UInt32Ty;
    if (name == "uint64" || name == "usize_t")
      return UInt64Ty;
    if (name == "uint16")
      return UInt16Ty;
    if (name == "uint8" || name == "char")
      return UInt8Ty;
    if (name == "rune")
      return UInt32Ty;
    if (name == "float" || name == "float32")
      return Float32Ty;
    if (name == "double" || name == "float64")
      return Float64Ty;
    if (name == "bool")
      return BoolTy;
    if (name == "void")
      return VoidTy;
    return nullptr;
  }

  /* Returns the canonical array type for the given element type and size. */
  const ArrayType *getArrayType(const Type *elementType, uint64_t size) {
    for (const auto *arrTy : arrayTypes) {
      if (arrTy->getElementType() == elementType && arrTy->getSize() == size)
        return arrTy;
    }
    auto *arrTy = create<ArrayType>(elementType, size);
    arrayTypes.push_back(arrTy);
    return arrTy;
  }

  const FunctionType *getFunctionType(const Type *ret,
                                      llvm::ArrayRef<const Type *> params) {
    auto *mem = allocator.Allocate<FunctionType>();
    return new (mem) FunctionType(ret, copyArray(params));
  }

  const EnumType *getEnumType(std::string_view name, const Type *underlying) {
    auto it = enumTypes.find(name);
    if (it != enumTypes.end())
      return it->second;

    auto *enumTy = create<EnumType>(name, underlying);
    enumTypes[name] = enumTy;
    return enumTy;
  }

  const EnumType *getEnumTypeByName(std::string_view name) const {
    auto it = enumTypes.find(name);
    return it != enumTypes.end() ? it->second : nullptr;
  }

  void addTypeAlias(std::string_view name, const Type *type) {
    typeAliases[name] = type;
  }

  const Type *getTypeAlias(std::string_view name) const {
    auto it = typeAliases.find(name);
    return it != typeAliases.end() ? it->second : nullptr;
  }

private:
  llvm::BumpPtrAllocator allocator;
  std::vector<const ArrayType *> arrayTypes;
  std::unordered_map<const Type *, const PointerType *> pointerTypes;
  std::unordered_map<const Type *, const ReferenceType *> referenceTypes;
  std::unordered_map<const Type *, const ConstType *> constTypes;
  std::unordered_map<std::string_view, RecordType *> recordTypes;
  std::unordered_map<std::string_view, const Type *> typeAliases;
  std::unordered_map<std::string_view, const EnumType *> enumTypes;
};

} // namespace utopia