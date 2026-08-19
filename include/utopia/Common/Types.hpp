#pragma once
#include <cstdint>
#include <expected>
#include <llvm/ADT/ArrayRef.h>
#include <string>

namespace utopia {

struct DeclNode;

struct ErrorInfo {
  int line;
  int column;
  int length;
  std::string message;
};

enum class TypeKind {
  Builtin,
  Pointer,
  Reference,
  RValueReference,
  Const,
  Struct,
  Class,
  Union,
  Array,
  MapLiteral,
  Function,
  Alias,
  Enum,
  TemplateParam,
  TemplateInst,
  Auto,
  Vector
};

enum class BuiltinKind {
  Int8,
  Int16,
  Int32,
  Int64,
  UInt8,
  UInt16,
  UInt32,
  UInt64,
  USize,
  Float32,
  Float64,
  Bool,
  Void,
  TypeVal,
  Namespace
};

class Type {
protected:
  TypeKind kind;
  explicit Type(TypeKind k) : kind(k) {}

public:
  TypeKind getKind() const { return kind; }

  bool isBuiltinType() const { return kind == TypeKind::Builtin; }
  bool isPointerType() const { return kind == TypeKind::Pointer; }
  bool isReferenceType() const { return kind == TypeKind::Reference; }
  bool isConstType() const { return kind == TypeKind::Const; }

  bool isConstQualified() const;
  const Type *getUnqualifiedType() const;

  bool isInteger() const;
  bool isFloat() const;
  bool isNumeric() const { return isInteger() || isFloat(); }
  bool isVoid() const;
  bool isVectorType() const { return kind == TypeKind::Vector; }

  std::string toString() const;
};

struct FieldInfo {
  std::string_view name;
  const Type *type;
  uint32_t index;
  bool isPublic;
  bool isProtected;
};

class BuiltinType : public Type {
  BuiltinKind bKind;

public:
  explicit BuiltinType(BuiltinKind k) : Type(TypeKind::Builtin), bKind(k) {}
  BuiltinKind getBuiltinKind() const { return bKind; }

  static bool classof(const Type *t) {
    return t->getKind() == TypeKind::Builtin;
  }
};

class PointerType : public Type {
  const Type *pointee;

public:
  explicit PointerType(const Type *p) : Type(TypeKind::Pointer), pointee(p) {}
  const Type *getPointeeType() const { return pointee; }

  static bool classof(const Type *t) {
    return t->getKind() == TypeKind::Pointer;
  }
};

class ReferenceType : public Type {
  const Type *pointee;

public:
  explicit ReferenceType(const Type *p)
      : Type(TypeKind::Reference), pointee(p) {}
  const Type *getPointeeType() const { return pointee; }

  static bool classof(const Type *t) {
    return t->getKind() == TypeKind::Reference;
  }
};

class RValueReferenceType : public Type {
  const Type *pointee;

public:
  explicit RValueReferenceType(const Type *p)
      : Type(TypeKind::RValueReference), pointee(p) {}
  const Type *getPointeeType() const { return pointee; }

  static bool classof(const Type *t) {
    return t->getKind() == TypeKind::RValueReference;
  }
};

class ConstType : public Type {
  const Type *base;

public:
  explicit ConstType(const Type *b) : Type(TypeKind::Const), base(b) {}
  const Type *getBaseType() const { return base; }

  static bool classof(const Type *t) { return t->getKind() == TypeKind::Const; }
};

class AutoType : public Type {
public:
  explicit AutoType() : Type(TypeKind::Auto) {}

  static bool classof(const Type *t) { return t->getKind() == TypeKind::Auto; }
};

class ArrayType : public Type {
  const Type *elementType;
  uint64_t size;

public:
  explicit ArrayType(const Type *elem, uint64_t sz)
      : Type(TypeKind::Array), elementType(elem), size(sz) {}
  const Type *getElementType() const { return elementType; }
  uint64_t getSize() const { return size; }

  static bool classof(const Type *t) { return t->getKind() == TypeKind::Array; }
};

/* A fixed-size SIMD vector, e.g. float32x4 or int8x16. It maps to an LLVM
 * fixed-length vector type ('<4 x float>') and lowers to the best SIMD
 * instructions available on the target (SSE/AVX/AVX-512, NEON, SVE, RVV...). */
class VectorType : public Type {
  const Type *elementType;
  uint64_t lanes;

public:
  explicit VectorType(const Type *elem, uint64_t laneCount)
      : Type(TypeKind::Vector), elementType(elem), lanes(laneCount) {}
  const Type *getElementType() const { return elementType; }
  uint64_t getLanes() const { return lanes; }
  bool isBoolVector() const {
    return elementType->isBuiltinType() &&
           static_cast<const BuiltinType *>(elementType)->getBuiltinKind() ==
               BuiltinKind::Bool;
  }

  static bool classof(const Type *t) { return t->getKind() == TypeKind::Vector; }
};

/* The compiler-internal type of a '{k: v, ...}' literal. It carries the
 * unified key/value types and the entry count so the literal can be lowered
 * to two parallel arrays (keys and values) that back a MapLiteralView<K, V>.
 * Like ArrayType, it only exists at expression level: there is no storage of
 * MapLiteral type. */
class MapLiteralType : public Type {
  const Type *keyType;
  const Type *valueType;
  uint64_t size;

public:
  explicit MapLiteralType(const Type *k, const Type *v, uint64_t sz)
      : Type(TypeKind::MapLiteral), keyType(k), valueType(v), size(sz) {}
  const Type *getKeyType() const { return keyType; }
  const Type *getValueType() const { return valueType; }
  uint64_t getSize() const { return size; }

  static bool classof(const Type *t) {
    return t->getKind() == TypeKind::MapLiteral;
  }
};

class RecordType : public Type {
protected:
  std::string_view name;
  llvm::ArrayRef<FieldInfo> fields;
  const DeclNode *declaration = nullptr;
  bool opaque = true;

  /* For template instantiations: the base template's fully-qualified name
   * and the resolved template arguments. */
  std::string_view templateBaseName;
  llvm::ArrayRef<const Type *> templateArgs;

  explicit RecordType(TypeKind k, std::string_view n) : Type(k), name(n) {}

public:
  std::string_view getName() const { return name; }
  llvm::ArrayRef<FieldInfo> getFields() const { return fields; }
  void setFields(llvm::ArrayRef<FieldInfo> f) { fields = f; }

  const DeclNode *getDeclaration() const { return declaration; }
  void setDeclaration(const DeclNode *decl) { declaration = decl; }

  std::string_view getTemplateBaseName() const { return templateBaseName; }
  void setTemplateBaseName(std::string_view n) { templateBaseName = n; }

  llvm::ArrayRef<const Type *> getTemplateArgs() const { return templateArgs; }
  void setTemplateArgs(llvm::ArrayRef<const Type *> a) { templateArgs = a; }
  bool isTemplateInstantiation() const { return !templateBaseName.empty(); }

  bool isOpaque() const { return opaque; }
  void setOpaque(bool op) { opaque = op; }

  const FieldInfo *getField(std::string_view fName) const {
    for (const auto &f : fields) {
      if (f.name == fName)
        return &f;
    }
    return nullptr;
  }

  static bool classof(const Type *t) {
    return t->getKind() == TypeKind::Struct ||
           t->getKind() == TypeKind::Class || t->getKind() == TypeKind::Union;
  }
};

class UnionType : public RecordType {
public:
  explicit UnionType(std::string_view n) : RecordType(TypeKind::Union, n) {}

  static bool classof(const Type *t) { return t->getKind() == TypeKind::Union; }
};

class StructType : public RecordType {
public:
  explicit StructType(std::string_view n) : RecordType(TypeKind::Struct, n) {}

  static bool classof(const Type *t) {
    return t->getKind() == TypeKind::Struct;
  }
};

class ClassType : public RecordType {
  const Type *baseClass = nullptr;
  llvm::ArrayRef<const Type *> interfaces;
  bool isPolymorphic = false;
  bool isAbstract = false;

public:
  explicit ClassType(std::string_view n) : RecordType(TypeKind::Class, n) {}

  const Type *getBaseClass() const { return baseClass; }
  void setBaseClass(const Type *b) { baseClass = b; }

  llvm::ArrayRef<const Type *> getInterfaces() const { return interfaces; }
  void setInterfaces(llvm::ArrayRef<const Type *> i) { interfaces = i; }

  bool getIsPolymorphic() const { return isPolymorphic; }
  void setIsPolymorphic(bool p) { isPolymorphic = p; }

  bool getIsAbstract() const { return isAbstract; }
  void setIsAbstract(bool a) { isAbstract = a; }

  static bool classof(const Type *t) { return t->getKind() == TypeKind::Class; }
};

class TemplateParamType : public Type {
  std::string_view name;

public:
  explicit TemplateParamType(std::string_view n)
      : Type(TypeKind::TemplateParam), name(n) {}
  std::string_view getName() const { return name; }

  static bool classof(const Type *t) {
    return t->getKind() == TypeKind::TemplateParam;
  }
};

class TemplateInstType : public Type {
  std::string_view baseName;
  llvm::ArrayRef<const Type *> templateArgs;
  mutable const Type *resolvedType = nullptr;

public:
  explicit TemplateInstType(std::string_view n,
                            llvm::ArrayRef<const Type *> args)
      : Type(TypeKind::TemplateInst), baseName(n), templateArgs(args) {}

  std::string_view getBaseName() const { return baseName; }
  llvm::ArrayRef<const Type *> getTemplateArgs() const { return templateArgs; }
  const Type *getResolvedType() const { return resolvedType; }
  void setResolvedType(const Type *t) const { resolvedType = t; }

  static bool classof(const Type *t) {
    return t->getKind() == TypeKind::TemplateInst;
  }
};

class FunctionType : public Type {
  const Type *returnType;
  llvm::ArrayRef<const Type *> paramTypes;

public:
  explicit FunctionType(const Type *ret, llvm::ArrayRef<const Type *> params)
      : Type(TypeKind::Function), returnType(ret), paramTypes(params) {}
  const Type *getReturnType() const { return returnType; }
  llvm::ArrayRef<const Type *> getParamTypes() const { return paramTypes; }

  static bool classof(const Type *t) {
    return t->getKind() == TypeKind::Function;
  }
};

class AliasType : public Type {
  std::string_view aliasName;
  mutable const Type *target;
  mutable const DeclNode *declaration = nullptr;

public:
  explicit AliasType(std::string_view n)
      : Type(TypeKind::Alias), aliasName(n), target(nullptr) {}
  std::string_view getName() const { return aliasName; }
  const Type *getTarget() const { return target; }
  void setTarget(const Type *t) const { target = t; }
  const DeclNode *getDeclaration() const { return declaration; }
  void setDeclaration(const DeclNode *decl) const { declaration = decl; }

  static bool classof(const Type *t) { return t->getKind() == TypeKind::Alias; }
};

class EnumType : public Type {
  std::string_view name;
  const Type *underlyingType;
  mutable const DeclNode *declaration;

public:
  explicit EnumType(std::string_view n, const Type *u)
      : Type(TypeKind::Enum), name(n), underlyingType(u), declaration(nullptr) {
  }

  std::string_view getName() const { return name; }
  const Type *getUnderlyingType() const { return underlyingType; }
  const DeclNode *getDeclaration() const { return declaration; }
  void setDeclaration(const DeclNode *decl) const { declaration = decl; }

  static bool classof(const Type *t) { return t->getKind() == TypeKind::Enum; }
};

inline bool Type::isConstQualified() const { return kind == TypeKind::Const; }

inline const Type *Type::getUnqualifiedType() const {
  if (kind == TypeKind::Const)
    return static_cast<const ConstType *>(this)
        ->getBaseType()
        ->getUnqualifiedType();
  if (kind == TypeKind::Alias) {
    const Type *tgt = static_cast<const AliasType *>(this)->getTarget();
    if (tgt)
      return tgt->getUnqualifiedType();
  }
  if (kind == TypeKind::TemplateInst) {
    const Type *res =
        static_cast<const TemplateInstType *>(this)->getResolvedType();
    if (res)
      return res->getUnqualifiedType();
  }
  return this;
}

inline bool Type::isInteger() const {
  const Type *unqual = getUnqualifiedType();
  if (!unqual->isBuiltinType())
    return false;
  auto b = static_cast<const BuiltinType *>(unqual)->getBuiltinKind();
  return (b >= BuiltinKind::Int8 && b <= BuiltinKind::UInt64) ||
         b == BuiltinKind::USize;
}

inline bool Type::isFloat() const {
  const Type *unqual = getUnqualifiedType();
  if (!unqual->isBuiltinType())
    return false;
  auto b = static_cast<const BuiltinType *>(unqual)->getBuiltinKind();
  return b == BuiltinKind::Float32 || b == BuiltinKind::Float64;
}

inline bool Type::isVoid() const {
  const Type *unqual = getUnqualifiedType();
  if (!unqual->isBuiltinType())
    return false;
  return static_cast<const BuiltinType *>(unqual)->getBuiltinKind() ==
         BuiltinKind::Void;
}

inline std::string Type::toString() const {
  if (kind == TypeKind::Auto) {
    return "auto";
  }
  if (kind == TypeKind::Alias) {
    return std::string(static_cast<const AliasType *>(this)->getName());
  }
  if (kind == TypeKind::TemplateParam) {
    return std::string(static_cast<const TemplateParamType *>(this)->getName());
  }
  if (kind == TypeKind::TemplateInst) {
    auto tInst = static_cast<const TemplateInstType *>(this);
    std::string res = std::string(tInst->getBaseName()) + "<";
    auto args = tInst->getTemplateArgs();
    for (size_t i = 0; i < args.size(); ++i) {
      res += args[i]->toString();
      if (i < args.size() - 1)
        res += ", ";
    }
    res += ">";
    return res;
  }
  if (kind == TypeKind::Function) {
    auto fTy = static_cast<const FunctionType *>(this);
    std::string res = fTy->getReturnType()->toString() + "(";
    auto params = fTy->getParamTypes();
    for (size_t i = 0; i < params.size(); ++i) {
      res += params[i]->toString();
      if (i < params.size() - 1)
        res += ", ";
    }
    res += ")";
    return res;
  }
  if (kind == TypeKind::Const) {
    return "const " +
           static_cast<const ConstType *>(this)->getBaseType()->toString();
  }
  if (kind == TypeKind::Array) {
    return static_cast<const ArrayType *>(this)->getElementType()->toString() +
           "[" +
           std::to_string(static_cast<const ArrayType *>(this)->getSize()) +
           "]";
  }
  if (kind == TypeKind::Vector) {
    auto *vTy = static_cast<const VectorType *>(this);
    return vTy->getElementType()->toString() + "x" +
           std::to_string(vTy->getLanes());
  }
  if (kind == TypeKind::MapLiteral) {
    auto *m = static_cast<const MapLiteralType *>(this);
    return m->getKeyType()->toString() + " -> " +
           m->getValueType()->toString() + "[" +
           std::to_string(m->getSize()) + "]";
  }
  if (kind == TypeKind::Enum) {
    return std::string(static_cast<const EnumType *>(this)->getName());
  }
  if (isBuiltinType()) {
    switch (static_cast<const BuiltinType *>(this)->getBuiltinKind()) {
    case BuiltinKind::Int8:
      return "int8";
    case BuiltinKind::Int16:
      return "int16";
    case BuiltinKind::Int32:
      return "int32";
    case BuiltinKind::Int64:
      return "int64";
    case BuiltinKind::UInt8:
      return "uint8";
    case BuiltinKind::UInt16:
      return "uint16";
    case BuiltinKind::UInt32:
      return "uint32";
    case BuiltinKind::UInt64:
      return "uint64";
    case BuiltinKind::USize:
      return "usize";
    case BuiltinKind::Float32:
      return "float32";
    case BuiltinKind::Float64:
      return "float64";
    case BuiltinKind::Bool:
      return "bool";
    case BuiltinKind::Void:
      return "void";
    case BuiltinKind::TypeVal:
      return "Type";
    }
  } else if (isPointerType()) {
    return static_cast<const PointerType *>(this)
               ->getPointeeType()
               ->toString() +
           "*";
  } else if (isReferenceType()) {
    return static_cast<const ReferenceType *>(this)
               ->getPointeeType()
               ->toString() +
           "&";
  } else if (kind == TypeKind::RValueReference) {
    return static_cast<const RValueReferenceType *>(this)
               ->getPointeeType()
               ->toString() +
           "&&";
  } else if (kind == TypeKind::Struct || kind == TypeKind::Class ||
             kind == TypeKind::Union) {
    return std::string(static_cast<const RecordType *>(this)->getName());
  }
  return "unknown";
}

using SemaResult = std::expected<const Type *, ErrorInfo>;

} // namespace utopia