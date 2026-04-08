#pragma once
#include <string>

namespace utopia {
struct ErrorInfo {
  int line, col, endLine, endCol;
  std::string message;
};

struct TypeInfo {
  std::string base;
  unsigned ptrDepth = 0;
  bool isNullable = false;
  bool isArray = false;
  bool isReference = false;
  bool isRValueRef = false;

  bool isPointer() const {
    return ptrDepth > 0 || isArray || isReference || isRValueRef ||
           base == "null";
  }

  bool isUnsigned() const {
    return base == "uchar" || base == "ushort" || base == "uint" ||
           base == "ulong" || base == "uint8_t" || base == "uint16_t" ||
           base == "uint32_t" || base == "uint64_t" || base == "size_t" ||
           base == "uintptr_t";
  }

  bool isFloat() const { return base == "float" || base == "double"; }

  bool isInteger() const {
    return base == "char" || base == "uchar" || base == "int8_t" ||
           base == "uint8_t" || base == "short" || base == "ushort" ||
           base == "int16_t" || base == "uint16_t" || base == "int" ||
           base == "uint" || base == "int32_t" || base == "uint32_t" ||
           base == "long" || base == "ulong" || base == "int64_t" ||
           base == "uint64_t" || base == "size_t" || base == "intptr_t" ||
           base == "uintptr_t";
  }

  int getIntegerBitWidth(unsigned targetPtrSize) const {
    if (base == "char" || base == "uchar" || base == "int8_t" ||
        base == "uint8_t")
      return 8;
    if (base == "short" || base == "ushort" || base == "int16_t" ||
        base == "uint16_t")
      return 16;
    if (base == "int" || base == "uint" || base == "int32_t" ||
        base == "uint32_t")
      return 32;
    if (base == "int64_t" || base == "uint64_t")
      return 64;

    if (base == "long" || base == "ulong" || base == "size_t" ||
        base == "intptr_t" || base == "uintptr_t")
      return targetPtrSize;

    return 0;
  }

  bool isPrimitive() const {
    return isInteger() || isFloat() || base == "bool" || base == "void";
  }
};

inline std::string getMangledType(const TypeInfo &t) {
  std::string s = t.base;

  if (t.isNullable)
    s = "opt_" + s;
  if (t.isArray)
    s = "arr_" + s;
  if (t.isReference)
    s = "ref_" + s;
  if (t.isRValueRef)
    s = "rrf_" + s;

  // memory alignment padding simulation for mangling
  for (unsigned i = 0; i < t.ptrDepth; ++i) {
    s = "ptr_" + s;
  }

  return s;
}

} // namespace utopia