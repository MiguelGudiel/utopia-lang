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

  bool isPointer() const { return ptrDepth > 0 || isArray; }
};

inline std::string getMangledType(const TypeInfo &t) {
  std::string s = t.base;
  for (unsigned i = 0; i < t.ptrDepth; ++i)
    s += "ptr";
  if (t.isArray)
    s += "arr";
  if (t.isNullable)
    s += "opt";
  return s;
}
} // namespace utopia