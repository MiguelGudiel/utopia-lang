#pragma once
#include "utopia/AST/AST.hpp"
#include <string>

namespace utopia {

class Mangler {
public:
  static std::string mangle(const FunctionDeclNode *node,
                            const std::string &parentRecord = "");
  static std::string mangle(const VarDeclNode *node,
                            const std::string &parentRecord = "");
  static std::string mangleType(const Type *t);
};

} // namespace utopia