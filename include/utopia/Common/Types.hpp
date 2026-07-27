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

enum class TypeKind { Builtin, Pointer, Reference, Const, Struct, Class };

enum class BuiltinKind {
  Int8,
  Int16,
  Int32,
  Int64,
  UInt8,
  UInt16,
  UInt32,
  UInt64,
  Float32,
  Float64,
  Bool,
  Void
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

  std::string toString() const;
};

struct FieldInfo {
  std::string_view name;
  const Type *type;
  uint32_t index;
};

class BuiltinType : public Type {
  BuiltinKind bKind;

public:
  explicit BuiltinType(BuiltinKind k) : Type(TypeKind::Builtin), bKind(k) {}
  BuiltinKind getBuiltinKind() const { return bKind; }
};

class PointerType : public Type {
  const Type *pointee;

public:
  explicit PointerType(const Type *p) : Type(TypeKind::Pointer), pointee(p) {}
  const Type *getPointeeType() const { return pointee; }
};

class ReferenceType : public Type {
  const Type *pointee;

public:
  explicit ReferenceType(const Type *p)
      : Type(TypeKind::Reference), pointee(p) {}
  const Type *getPointeeType() const { return pointee; }
};

class ConstType : public Type {
  const Type *base;

public:
  explicit ConstType(const Type *b) : Type(TypeKind::Const), base(b) {}
  const Type *getBaseType() const { return base; }
};

class RecordType : public Type {
protected:
  std::string_view name;
  llvm::ArrayRef<FieldInfo> fields;
  const DeclNode *declaration = nullptr;

  explicit RecordType(TypeKind k, std::string_view n) : Type(k), name(n) {}

public:
  std::string_view getName() const { return name; }
  llvm::ArrayRef<FieldInfo> getFields() const { return fields; }
  void setFields(llvm::ArrayRef<FieldInfo> f) { fields = f; }

  const DeclNode *getDeclaration() const { return declaration; }
  void setDeclaration(const DeclNode *decl) { declaration = decl; }

  const FieldInfo *getField(std::string_view fName) const {
    for (const auto &f : fields) {
      if (f.name == fName)
        return &f;
    }
    return nullptr;
  }
};

class StructType : public RecordType {
public:
  explicit StructType(std::string_view n) : RecordType(TypeKind::Struct, n) {}
};

class ClassType : public RecordType {
public:
  explicit ClassType(std::string_view n) : RecordType(TypeKind::Class, n) {}
};

inline bool Type::isConstQualified() const { return kind == TypeKind::Const; }

inline const Type *Type::getUnqualifiedType() const {
  if (kind == TypeKind::Const)
    return static_cast<const ConstType *>(this)->getBaseType();
  return this;
}

inline bool Type::isInteger() const {
  const Type *unqual = getUnqualifiedType();
  if (!unqual->isBuiltinType())
    return false;
  auto b = static_cast<const BuiltinType *>(unqual)->getBuiltinKind();
  return b >= BuiltinKind::Int8 && b <= BuiltinKind::UInt64;
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
  if (kind == TypeKind::Const) {
    return "const " +
           static_cast<const ConstType *>(this)->getBaseType()->toString();
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
    case BuiltinKind::Float32:
      return "float32";
    case BuiltinKind::Float64:
      return "float64";
    case BuiltinKind::Bool:
      return "bool";
    case BuiltinKind::Void:
      return "void";
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
  } else if (kind == TypeKind::Struct || kind == TypeKind::Class) {
    return std::string(static_cast<const RecordType *>(this)->getName());
  }
  return "unknown";
}

using SemaResult = std::expected<const Type *, ErrorInfo>;

} // namespace utopia