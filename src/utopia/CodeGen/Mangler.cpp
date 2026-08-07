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

  std::string nameStr =
      std::string(node->fqName.empty() ? node->name : node->fqName);
  std::replace(nameStr.begin(), nameStr.end(), '.', '_');

  std::string parentStr = parentRecord;
  std::replace(parentStr.begin(), parentStr.end(), '.', '_');

  if (!parentStr.empty()) {
    res += "N";
    res += std::to_string(parentStr.length()) + parentStr;

    if (node->name == parentRecord ||
        node->name == parentRecord.substr(parentRecord.find_last_of('.') + 1)) {
      res += "C1";
    } else if (node->name == "~") {
      res += "D1";
    } else {
      std::string simpleName = std::string(node->name);
      std::replace(simpleName.begin(), simpleName.end(), '.', '_');
      res += std::to_string(simpleName.length()) + simpleName;
    }
    res += "E";
  } else {
    res += std::to_string(nameStr.length()) + nameStr;
  }

  if (node->params.empty()) {
    res += "v";
  } else {
    for (const auto *param : node->params) {
      res += mangleType(param->type);
    }
  }

  return res;
}

std::string Mangler::mangle(const VarDeclNode *node,
                            const std::string &parentRecord) {
  std::string res = "_Z";

  std::string nameStr =
      std::string(node->fqName.empty() ? node->varName : node->fqName);
  std::replace(nameStr.begin(), nameStr.end(), '.', '_');

  std::string parentStr = parentRecord;
  std::replace(parentStr.begin(), parentStr.end(), '.', '_');

  if (!parentStr.empty()) {
    res += "N";
    res += std::to_string(parentStr.length()) + parentStr;
    std::string simpleName = std::string(node->varName);
    std::replace(simpleName.begin(), simpleName.end(), '.', '_');
    res += std::to_string(simpleName.length()) + simpleName;
    res += "E";
  } else {
    res += std::to_string(nameStr.length()) + nameStr;
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
  if (t->getKind() == TypeKind::RValueReference) {
    return "O" +
           mangleType(
               static_cast<const RValueReferenceType *>(t)->getPointeeType());
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
    case BuiltinKind::USize:
      return "z";
    case BuiltinKind::Float32:
      return "f";
    case BuiltinKind::Float64:
      return "d";
    case BuiltinKind::Bool:
      return "b";
    case BuiltinKind::Void:
      return "v";
    case BuiltinKind::TypeVal:
      return "T";
    case BuiltinKind::Namespace:
      return "N";
    }
  }
  if (t->getKind() == TypeKind::Struct || t->getKind() == TypeKind::Class ||
      t->getKind() == TypeKind::Union) {
    std::string name =
        std::string(static_cast<const RecordType *>(t)->getName());
    std::replace(name.begin(), name.end(), '.', '_');
    return std::to_string(name.length()) + name;
  }
  if (t->getKind() == TypeKind::Enum) {
    std::string name = std::string(static_cast<const EnumType *>(t)->getName());
    std::replace(name.begin(), name.end(), '.', '_');
    return std::to_string(name.length()) + name;
  }
  return "u";
}

} // namespace utopia