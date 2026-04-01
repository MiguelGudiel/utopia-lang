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

  bool isPointer() const { return ptrDepth > 0; }
};
} // namespace utopia