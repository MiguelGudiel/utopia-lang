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
  unsigned arrayDimensions = 0;
  bool isReference = false;
  bool isRValueRef = false;
  bool isNullable = false;
  bool isConst = false;

  bool isPointer() const {
    return ptrDepth > 0 || arrayDimensions > 0 || isReference || isRValueRef ||
           base == "null";
  }

  bool isUnsigned() const {
    return base == "uint8" || base == "uint16" || base == "uint" ||
           base == "uint32" || base == "uint64" || base == "usize";
  }

  bool isFloat() const {
    if (ptrDepth > 0 || arrayDimensions > 0 || isReference || isRValueRef)
      return false;
    return base == "float" || base == "double" || base == "float8" ||
           base == "float16" || base == "float32" || base == "float64";
  }

  bool isInteger() const {
    if (ptrDepth > 0 || arrayDimensions > 0 || isReference || isRValueRef)
      return false;

    return base == "char" || base == "int8" || base == "uint8" ||
           base == "int16" || base == "uint16" || base == "int" ||
           base == "uint" || base == "int32" || base == "uint32" ||
           base == "int64" || base == "uint64" || base == "usize" ||
           base == "intptr";
  }

  int getIntegerBitWidth(unsigned targetPtrSize) const {
    if (base == "int8" || base == "uint8")
      return 8;
    if (base == "int16" || base == "uint16")
      return 16;
    if (base == "int32" || base == "uint32")
      return 32;
    if (base == "int" || base == "uint")
      return 32; // Default 32b
    if (base == "int64" || base == "uint64")
      return 64;

    // Architecture pointer size (usually 64 or 32)
    if (base == "usize" || base == "intptr")
      return targetPtrSize;

    return 0;
  }

  bool isTextual() const {
    return (base == "char" && ptrDepth == 1) || (base == "String");
  }

  bool isPrimitive() const {
    return isInteger() || isFloat() || base == "bool" || base == "void";
  }
};

inline std::string getMangledType(const TypeInfo &t) {
  std::string s = t.base;

  if (t.isConst)
    s = "c_" + s;
  if (t.isNullable)
    s = "opt_" + s;
  for (unsigned i = 0; i < t.arrayDimensions; ++i)
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