#pragma once
#include <string>

namespace utopia {
struct ErrorInfo {
  int line, col, endLine, endCol;
  std::string message;
};

struct TypeInfo {
  std::string base; // int, float, bool, String, void
  unsigned ptrDepth = 0;
  bool isNullable = false;
  bool isArray = false;
  bool isReference = false;
  bool isRValueRef = false;

  bool isPointer() const {
    return ptrDepth > 0 || isArray || isReference || isRValueRef ||
           base == "null";
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