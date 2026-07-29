#include "utopia/CodeGen/Mangler.hpp"

namespace utopia {

/*
 * Itanium ABI mangling subset implementation.
 * 0x5A '_' 0x5A 'Z' -> gateway to the underworld of linkers.
 * Collisions are physically impossible unless the AST is deliberately cloned.
 */
std::string Mangler::mangle(const FunctionDeclNode *node,
                            const std::string &parentRecord) {
  if (node->name == "main")
    return "main";

  std::string res = "_Z";

  if (!parentRecord.empty()) {
    res += "N";
    res += std::to_string(parentRecord.length()) + parentRecord;

    if (node->name == parentRecord) {
      res += "C1";
    } else if (node->name == "~") {
      res += "D1";
    } else {
      res += std::to_string(node->name.length()) + std::string(node->name);
    }
    res += "E";
  } else {
    res += std::to_string(node->name.length()) + std::string(node->name);
  }

  size_t startIdx =
      (node->isMethod && !node->isExtern && !node->isStatic) ? 1 : 0;

  if (startIdx >= node->params.size()) {
    res += "v";
  } else {
    for (size_t i = startIdx; i < node->params.size(); ++i) {
      res += mangleType(node->params[i]->type);
    }
  }

  return res;
}

std::string Mangler::mangle(const VarDeclNode *node,
                            const std::string &parentRecord) {
  std::string res = "_Z";

  if (!parentRecord.empty()) {
    res += "N";
    res += std::to_string(parentRecord.length()) + parentRecord;
    res += std::to_string(node->varName.length()) + std::string(node->varName);
    res += "E";
  } else {
    res += std::to_string(node->varName.length()) + std::string(node->varName);
  }

  return res;
}

std::string Mangler::mangleType(const Type *t) {
  if (t->getKind() == TypeKind::Alias) {
    return mangleType(static_cast<const AliasType *>(t)->getTarget());
  }
  if (t->getKind() == TypeKind::Function) {
    auto fTy = static_cast<const FunctionType *>(t);
    std::string res = "F" + mangleType(fTy->getReturnType());
    for (const auto *p : fTy->getParamTypes()) {
      res += mangleType(p);
    }
    res += "E";
    return res;
  }
  if (t->getKind() == TypeKind::Array) {
    auto arrTy = static_cast<const ArrayType *>(t);
    return "A" + std::to_string(arrTy->getSize()) + "_" +
           mangleType(arrTy->getElementType());
  }
  if (t->isPointerType()) {
    return "P" +
           mangleType(static_cast<const PointerType *>(t)->getPointeeType());
  }
  if (t->isReferenceType()) {
    return "R" +
           mangleType(static_cast<const ReferenceType *>(t)->getPointeeType());
  }
  if (t->isBuiltinType()) {
    auto k = static_cast<const BuiltinType *>(t)->getBuiltinKind();
    switch (k) {
    case BuiltinKind::Int8:
      return "a";
    case BuiltinKind::Int16:
      return "s";
    case BuiltinKind::Int32:
      return "i";
    case BuiltinKind::Int64:
      return "x";
    case BuiltinKind::UInt8:
      return "h";
    case BuiltinKind::UInt16:
      return "t";
    case BuiltinKind::UInt32:
      return "j";
    case BuiltinKind::UInt64:
      return "y";
    case BuiltinKind::Float32:
      return "f";
    case BuiltinKind::Float64:
      return "d";
    case BuiltinKind::Bool:
      return "b";
    case BuiltinKind::Void:
      return "v";
    }
  }
  if (t->getKind() == TypeKind::Struct || t->getKind() == TypeKind::Class) {
    auto name = static_cast<const RecordType *>(t)->getName();
    return std::to_string(name.length()) + std::string(name);
  }
  if (t->getKind() == TypeKind::Enum) {
    auto name = static_cast<const EnumType *>(t)->getName();
    return std::to_string(name.length()) + std::string(name);
  }
  return "u";
}

} // namespace utopia