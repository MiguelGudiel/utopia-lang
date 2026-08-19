#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/Common/Types.hpp"

#include <llvm/Support/Compiler.h>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/Allocator.h>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
  const BuiltinType *USizeTy;
  const BuiltinType *Float32Ty;
  const BuiltinType *Float64Ty;
  const BuiltinType *TypeValTy;
  const BuiltinType *NamespaceTy;
  const AutoType *AutoTy;

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
    USizeTy = create<BuiltinType>(BuiltinKind::USize);
    Float32Ty = create<BuiltinType>(BuiltinKind::Float32);
    Float64Ty = create<BuiltinType>(BuiltinKind::Float64);
    TypeValTy = create<BuiltinType>(BuiltinKind::TypeVal);
    NamespaceTy = create<BuiltinType>(BuiltinKind::Namespace);
    AutoTy = create<AutoType>();
  }
  ~ASTContext() = default;

  ASTContext(const ASTContext &) = delete;
  ASTContext &operator=(const ASTContext &) = delete;

  template <typename T, typename... Args> T *create(Args &&...args) {
    void *mem = allocator.Allocate<T>();
    return new (mem) T(std::forward<Args>(args)...);
  }

  void registerTemplateName(std::string_view name) {
    registeredTemplates.insert(name);
  }

  bool isTemplateName(std::string_view name) const {
    return registeredTemplates.contains(name);
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

  /* Returns the canonical r-value reference type for the given pointee. */
  const RValueReferenceType *getRValueReferenceType(const Type *pointee) {
    auto it = rvalueReferenceTypes.find(pointee);
    if (it != rvalueReferenceTypes.end())
      return it->second;

    auto *refTy = create<RValueReferenceType>(pointee);
    rvalueReferenceTypes[pointee] = refTy;
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

  NamespaceDeclNode *getNamespace(std::string_view fqName) const {
    auto it = namespaces.find(fqName);
    return it != namespaces.end() ? it->second : nullptr;
  }

  RecordType *createRecordType(TypeKind kind, std::string_view name) {
    auto it = recordTypes.find(name);
    if (it != recordTypes.end()) {
      return it->second;
    }
    RecordType *rec = nullptr;
    if (kind == TypeKind::Struct) {
      rec = create<StructType>(name);
    } else if (kind == TypeKind::Union) {
      rec = create<UnionType>(name);
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
    if (name == "uint64")
      return UInt64Ty;
    if (name == "usize")
      return USizeTy;
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

  /* Returns the canonical vector type for the given element type and lane
   * count, e.g. ('float32', 4) -> float32x4. */
  const VectorType *getVectorType(const Type *elem, uint64_t lanes) {
    auto it = vectorTypes.find(elem);
    if (it != vectorTypes.end()) {
      auto laneIt = it->second.find(lanes);
      if (laneIt != it->second.end())
        return laneIt->second;
    }
    auto *vTy = create<VectorType>(elem, lanes);
    vectorTypes[elem][lanes] = vTy;
    return vTy;
  }

  /* Resolves a vector type by its source spelling ('float32x4'). */
  const Type *getVectorTypeByName(std::string_view name) {
    auto x = name.find('x');
    if (x == std::string_view::npos || x == 0 || x == name.size() - 1)
      return nullptr;
    std::string elemName(name.substr(0, x));
    std::string lanesStr(name.substr(x + 1));
    if (lanesStr.empty() ||
        lanesStr.find_first_not_of("0123456789") != std::string::npos)
      return nullptr;
    uint64_t lanes = 0;
    try {
      lanes = std::stoull(lanesStr);
    } catch (...) {
      return nullptr;
    }
    if (lanes == 0 || lanes > 128)
      return nullptr;
    const Type *elem = getBuiltinTypeByName(elemName);
    if (!elem)
      return nullptr;
    /* Restrict to the canonical element spellings the lexer recognizes. */
    if (elem != Int8Ty && elem != Int16Ty && elem != Int32Ty &&
        elem != Int64Ty && elem != UInt8Ty && elem != UInt16Ty &&
        elem != UInt32Ty && elem != UInt64Ty && elem != Float32Ty &&
        elem != Float64Ty && elem != BoolTy)
      return nullptr;
    return getVectorType(elem, lanes);
  }

  const Type *getPromotedNumericType(const Type *lhs, const Type *rhs) const {
    if (!lhs->isNumeric() || !rhs->isNumeric()) {
      return nullptr;
    }

    auto k1 = static_cast<const BuiltinType *>(lhs->getUnqualifiedType())
                  ->getBuiltinKind();
    auto k2 = static_cast<const BuiltinType *>(rhs->getUnqualifiedType())
                  ->getBuiltinKind();

    if (k1 == k2) {
      return lhs->getUnqualifiedType();
    }

    /* Floating-point type promotion takes absolute precedence */
    if (k1 == BuiltinKind::Float64 || k2 == BuiltinKind::Float64) {
      return Float64Ty;
    }
    if (k1 == BuiltinKind::Float32 || k2 == BuiltinKind::Float32) {
      return Float32Ty;
    }

    /* USize keeps its width unless combined with a 64-bit type, which
     * promotes it to UInt64. */
    if (k1 == BuiltinKind::USize && k2 == BuiltinKind::USize) {
      return USizeTy;
    }
    if (k1 == BuiltinKind::USize) {
      if (k2 == BuiltinKind::Int64 || k2 == BuiltinKind::UInt64)
        return UInt64Ty;
      return USizeTy;
    }
    if (k2 == BuiltinKind::USize) {
      if (k1 == BuiltinKind::Int64 || k1 == BuiltinKind::UInt64)
        return UInt64Ty;
      return USizeTy;
    }

    /*
     * 2D Type Promotion Matrix for Integer Arithmetic.
     * Prevents lossy conversions by safely expanding to the next compatible
     * precision tier when signed and unsigned boundaries collide.
     *
     * Indices correspond directly to BuiltinKind integral definitions:
     * 0: Int8, 1: Int16, 2: Int32, 3: Int64
     * 4: UInt8, 5: UInt16, 6: UInt32, 7: UInt64
     */
    static constexpr BuiltinKind promotionMatrix[8][8] = {
        /*            Int8               Int16                Int32 Int64
                      UInt8              UInt16               UInt32 UInt64 */
        /* Int8   */
        {BuiltinKind::Int8, BuiltinKind::Int16, BuiltinKind::Int32,
         BuiltinKind::Int64, BuiltinKind::Int16, BuiltinKind::Int32,
         BuiltinKind::Int64, BuiltinKind::UInt64},
        /* Int16  */
        {BuiltinKind::Int16, BuiltinKind::Int16, BuiltinKind::Int32,
         BuiltinKind::Int64, BuiltinKind::Int16, BuiltinKind::Int32,
         BuiltinKind::Int64, BuiltinKind::UInt64},
        /* Int32  */
        {BuiltinKind::Int32, BuiltinKind::Int32, BuiltinKind::Int32,
         BuiltinKind::Int64, BuiltinKind::Int32, BuiltinKind::Int32,
         BuiltinKind::Int64, BuiltinKind::UInt64},
        /* Int64  */
        {BuiltinKind::Int64, BuiltinKind::Int64, BuiltinKind::Int64,
         BuiltinKind::Int64, BuiltinKind::Int64, BuiltinKind::Int64,
         BuiltinKind::Int64, BuiltinKind::UInt64},
        /* UInt8  */
        {BuiltinKind::Int16, BuiltinKind::Int16, BuiltinKind::Int32,
         BuiltinKind::Int64, BuiltinKind::UInt8, BuiltinKind::UInt16,
         BuiltinKind::UInt32, BuiltinKind::UInt64},
        /* UInt16 */
        {BuiltinKind::Int32, BuiltinKind::Int32, BuiltinKind::Int32,
         BuiltinKind::Int64, BuiltinKind::UInt16, BuiltinKind::UInt16,
         BuiltinKind::UInt32, BuiltinKind::UInt64},
        /* UInt32 */
        {BuiltinKind::Int64, BuiltinKind::Int64, BuiltinKind::Int64,
         BuiltinKind::Int64, BuiltinKind::UInt32, BuiltinKind::UInt32,
         BuiltinKind::UInt32, BuiltinKind::UInt64},
        /* UInt64 */
        {BuiltinKind::UInt64, BuiltinKind::UInt64, BuiltinKind::UInt64,
         BuiltinKind::UInt64, BuiltinKind::UInt64, BuiltinKind::UInt64,
         BuiltinKind::UInt64, BuiltinKind::UInt64}};

    int idx1 = static_cast<int>(k1);
    int idx2 = static_cast<int>(k2);

    if (idx1 >= 0 && idx1 <= 7 && idx2 >= 0 && idx2 <= 7) {
      switch (promotionMatrix[idx1][idx2]) {
      case BuiltinKind::Int8:
        return Int8Ty;
      case BuiltinKind::Int16:
        return Int16Ty;
      case BuiltinKind::Int32:
        return Int32Ty;
      case BuiltinKind::Int64:
        return Int64Ty;
      case BuiltinKind::UInt8:
        return UInt8Ty;
      case BuiltinKind::UInt16:
        return UInt16Ty;
      case BuiltinKind::UInt32:
        return UInt32Ty;
      case BuiltinKind::UInt64:
        return UInt64Ty;
      default:
        return nullptr;
      }
    }

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

  /* Returns the canonical map-literal type for the given key/value types
   * and entry count. */
  const MapLiteralType *getMapLiteralType(const Type *keyType,
                                          const Type *valueType,
                                          uint64_t size) {
    for (const auto *mapTy : mapLiteralTypes) {
      if (mapTy->getKeyType() == keyType && mapTy->getValueType() == valueType &&
          mapTy->getSize() == size)
        return mapTy;
    }
    auto *mapTy = create<MapLiteralType>(keyType, valueType, size);
    mapLiteralTypes.push_back(mapTy);
    return mapTy;
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

  NamespaceDeclNode *getOrCreateNamespace(std::string_view fqName) {
    auto it = namespaces.find(fqName);
    if (it != namespaces.end())
      return it->second;

    /* Extract the simple name from the fully qualified name to ensure
     * IDE completions and hover tooltips only display the relevant component.
     */
    size_t dot = fqName.find_last_of('.');
    std::string_view simpleName =
        (dot == std::string_view::npos) ? fqName : fqName.substr(dot + 1);

    auto *ns = create<NamespaceDeclNode>(simpleName, 0, 0, 0);
    ns->fqName = fqName;
    namespaces[fqName] = ns;
    return ns;
  }

private:
  llvm::BumpPtrAllocator allocator;
  std::vector<const ArrayType *> arrayTypes;
  std::vector<const MapLiteralType *> mapLiteralTypes;
  std::unordered_map<const Type *, std::unordered_map<uint64_t, const VectorType *>>
      vectorTypes;
  std::unordered_map<const Type *, const PointerType *> pointerTypes;
  std::unordered_map<const Type *, const ReferenceType *> referenceTypes;
  std::unordered_map<const Type *, const RValueReferenceType *>
      rvalueReferenceTypes;
  std::unordered_map<const Type *, const ConstType *> constTypes;
  std::unordered_map<std::string_view, RecordType *> recordTypes;
  std::unordered_map<std::string_view, const Type *> typeAliases;
  std::unordered_map<std::string_view, const EnumType *> enumTypes;

  std::unordered_set<std::string_view> registeredTemplates;

  std::unordered_map<std::string_view, NamespaceDeclNode *> namespaces;
};

} // namespace utopia