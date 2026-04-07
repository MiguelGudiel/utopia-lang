#include "utopia/Sema/Sema.hpp"
#include <llvm/IR/Function.h>
#include <unordered_set>

namespace utopia {

void Sema::enterScope() { scopeStack.push_back({}); }
void Sema::exitScope() { scopeStack.pop_back(); }

Sema::Symbol *Sema::lookup(const std::string &name) {
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    if (it->count(name))
      return &(*it)[name];
  }
  if (globalSymbols.count(name))
    return &globalSymbols[name];
  return nullptr;
}

bool Sema::analyzeModules(const std::vector<ModuleNode *> &modules) {
  errors.clear();
  scopeStack.clear();
  functionTypes.clear();
  customStructs.clear();
  currentClass.clear();
  structASTs.clear();
  loopDepth = 0;

  // Primera fase: recolectar declaraciones de todos los módulos
  for (ModuleNode *mod : modules) {
    for (auto &var : mod->globalVars) {
      TypeInfo t = parseType(var->typeName, mod);
      globalSymbols[var->name] = {t, var->isConst};
    }

    for (auto &st : mod->structs) {
      structASTs[st->name] = st.get();
      classModuleMap[st->name] = mod->filename;
      StructDef def;
      def.isClass = st->isClass;
      int idx = 0;
      for (auto &f : st->fields) {
        def.fields[f.name] = {parseType(f.typeName, st.get()), f.modifier,
                              f.isStatic ? -1 : idx++, f.isStatic};
      }
      customStructs[st->name] = def;

      for (auto &m : st->methods) {
        std::string baseName = st->name + "_" + m->name;
        std::string mangledName = baseName;
        std::vector<TypeInfo> params;

        if (!m->isStatic && !m->isConstructor) {
          TypeInfo thisType = {st->name, 1, false};
          params.push_back(thisType);
          mangledName += "_" + getMangledType(thisType);
        }

        for (auto &arg : m->args) {
          TypeInfo t;
          if (arg.isThisAssign) {
            t = def.fields.count(arg.name) ? def.fields[arg.name].type
                                           : TypeInfo{"error"};
            // AST mutation in-place. Salva a CodeGen de un escaneo extra.
            arg.type = t.base;
          } else {
            t = parseType(arg.type, m.get());
          }
          params.push_back(t);
          mangledName += "_" + getMangledType(t);
        }

        TypeInfo ret = parseType(m->returnType, m.get());
        registerOverload(baseName, mangledName, params, ret);
        functionTypes[mangledName] = ret;

        if (m->isConstructor && m->args.size() == 1) {
          TypeInfo argType = parseType(m->args[0].type, m.get());
          if (argType.base == st->name && argType.isReference) {
            copyConstructors[st->name] = mangledName;
          }
        }
      }
    }

    for (auto &fn : mod->functions) {
      std::string mangledName = fn->name;
      std::vector<TypeInfo> params;
      for (auto &arg : fn->args) {
        TypeInfo t = parseType(arg.type, fn.get());
        params.push_back(t);
        mangledName += "_" + getMangledType(t);
      }
      TypeInfo ret = parseType(fn->returnType, fn.get());
      functionTypes[mangledName] = ret;
      registerOverload(fn->name, mangledName, params, ret);
    }

    for (auto &ext : mod->extensions) {
      for (auto &m : ext->methods) {
        std::string baseName = "ext_" + ext->targetTypedef + "_" + m->name;
        std::string mangledName = baseName;
        std::vector<TypeInfo> params;
        TypeInfo targetType = parseType(ext->targetTypedef, ext.get());
        params.push_back(targetType);
        mangledName += "_" + getMangledType(targetType);

        for (auto &arg : m->args) {
          TypeInfo t = arg.isThisAssign
                           ? parseType(ext->targetTypedef, ext.get())
                           : parseType(arg.type, m.get());
          params.push_back(t);
          mangledName += "_" + getMangledType(t);
        }
        TypeInfo ret = parseType(m->returnType, m.get());
        registerOverload(baseName, mangledName, params, ret);
        functionTypes[mangledName] = ret;
      }
    }
  }

  // Segunda fase: analizar los cuerpos (funciones y métodos)
  for (ModuleNode *mod : modules) {
    currentModuleFile = mod->filename;

    for (auto &var : mod->globalVars) {
      if (var->initializer) {
        var->initializer->accept(this);
        checkAssignment(globalSymbols[var->name].type, currentExprType,
                        var.get());
      }
    }
    for (auto &st : mod->structs) {
      st->accept(this);

      for (auto &m : st->methods) {
        m->accept(this);
      }
    }
    for (auto &ext : mod->extensions) {
      for (auto &m : ext->methods) {
        m->accept(this);
      }
    }
    for (auto &fn : mod->functions) {
      fn->accept(this);
    }
  }
  return errors.empty();
}

void Sema::reportError(ASTNode *node, const std::string &message) {
  errors.push_back(
      {node->line, node->column, node->endLine, node->endColumn, message});
}

TypeInfo Sema::parseType(const std::string &typeName, ASTNode *node) {
  TypeInfo t;
  std::string temp = typeName;
  if (!temp.empty() && temp.back() == '?') {
    t.isNullable = true;
    temp.pop_back();
  }
  if (temp.length() >= 2 && temp.substr(temp.length() - 2) == "&&") {
    t.isRValueRef = true;
    temp.erase(temp.length() - 2);
  } else if (!temp.empty() && temp.back() == '&') {
    t.isReference = true;
    temp.pop_back();
  }
  while (!temp.empty() && temp.back() == '*') {
    t.ptrDepth++;
    temp.pop_back();
  }
  t.base = temp;

  if (!t.base.empty() && t.base[0] == '_' && classModuleMap.count(t.base) &&
      classModuleMap[t.base] != currentModuleFile) {
    if (node) {
      reportError(node,
                  "Semantic Error: Clase '" + t.base +
                      "' es privada a su modulo y no puede ser exportada.");
    } else {
      errors.push_back(
          {0, 0, 0, 0,
           "Semantic Error: Clase '" + t.base + "' es privada a su modulo."});
    }
  }

  // Nullability is a pointer's burden. Values are absolute.
  // Prohibimos estrictamente cosas como int?, float?, o ClassName?
  if (t.isNullable && t.ptrDepth == 0) {
    if (node) {
      reportError(node, "Semantic Error: Value types cannot be nullable. Only "
                        "pointers can be optional in Utopia (e.g., '" +
                            t.base + "*?').");
    } else {
      errors.push_back(
          {0, 0, 0, 0,
           "Semantic Error: Type '" + t.base + "' cannot be nullable."});
    }
    t.isNullable = false;
  }

  return t;
}

std::string typeToString(const TypeInfo &t) {
  std::string s = t.base;
  for (unsigned i = 0; i < t.ptrDepth; ++i)
    s += "*";
  if (t.isReference)
    s += "&";
  if (t.isNullable)
    s += "?";
  return s;
}

bool Sema::checkAssignment(const TypeInfo &target, const TypeInfo &source,
                           ASTNode *node) {
  if (!target.isNullable && source.isNullable) {
    reportError(node, "error: nullable value assigned to non-nullable type '" +
                          typeToString(target) + "'");
    return false;
  }

  if (target.ptrDepth != source.ptrDepth && source.base != "null") {
    reportError(node,
                "error: incompatible pointer indirection levels (expected '" +
                    typeToString(target) + "', found '" + typeToString(source) +
                    "')");
    return false;
  }

  if (target.base != source.base && source.base != "null" &&
      target.ptrDepth == source.ptrDepth) {
    reportError(node, "error: cannot convert '" + typeToString(source) +
                          "' to '" + typeToString(target) + "'");
    return false;
  }

  // Check if we are trying to mix oil and water (pointers and scalars)
  // We use 'null' as a sovereign entity, not a dirty zero from the 70s.
  if (target.base == "int" && target.ptrDepth == 0 && source.base == "null") {
    reportError(node, "Keep your integers away from my null pointers.");
    return false;
  }

  return true;
}

bool Sema::analyze(ProgramNode *program) {
  errors.clear();
  scopeStack.clear();
  functionTypes.clear();
  customStructs.clear();
  currentClass.clear();
  structASTs.clear();
  loopDepth = 0;

  for (auto &func : program->functions) {
    std::string mangledName = func->name;
    std::vector<TypeInfo> params;

    for (auto &arg : func->args) {
      TypeInfo t = parseType(arg.type, program);
      params.push_back(t);
      mangledName += "_" + getMangledType(t);
    }

    TypeInfo ret = parseType(func->returnType, program);
    functionTypes[mangledName] = ret;

    registerOverload(func->name, mangledName, params, ret);
  }

  for (auto &st : program->structs) {
    structASTs[st->name] = st.get();

    StructDef def;
    def.isClass = st->isClass;
    int idx = 0;
    for (auto &f : st->fields) {
      def.fields[f.name] = {parseType(f.typeName, program), f.modifier,
                            f.isStatic ? -1 : idx++, f.isStatic};
    }
    customStructs[st->name] = def;

    // register methods into global scope
    for (auto &m : st->methods) {
      std::string baseName = st->name + "_" + m->name;
      std::string mangledName = baseName;
      std::vector<TypeInfo> params;

      if (!m->isStatic && !m->isConstructor) {
        TypeInfo thisType = {st->name, 1, false};
        params.push_back(thisType);
        mangledName += "_" + getMangledType(thisType);
      }

      for (auto &arg : m->args) {
        TypeInfo t;
        if (arg.isThisAssign) {
          t = def.fields.count(arg.name) ? def.fields[arg.name].type
                                         : TypeInfo{"error"};
          arg.type = t.base;
        } else {
          t = parseType(arg.type, program);
        }
        params.push_back(t);
        mangledName += "_" + getMangledType(t);
      }
      TypeInfo ret = parseType(m->returnType, program);
      registerOverload(baseName, mangledName, params, ret);

      functionTypes[mangledName] = parseType(m->returnType, program);

      if (m->isConstructor && m->args.size() == 1) {
        TypeInfo argType = parseType(m->args[0].type, program);
        // Copy Constructor: ClassName(ClassName& other)
        if (argType.base == st->name && argType.isReference) {
          copyConstructors[st->name] = mangledName;
        }
      }
    }
  }

  for (auto &ext : program->extensions) {
    for (auto &m : ext->methods) {
      std::string baseName = "ext_" + ext->targetTypedef + "_" + m->name;
      std::string mangledName = baseName;
      std::vector<TypeInfo> params;

      // The first parameter of an extension is always the target (this)
      TypeInfo targetType = parseType(ext->targetTypedef, program);
      params.push_back(targetType);
      mangledName += "_" + getMangledType(targetType);

      for (auto &arg : m->args) {
        TypeInfo t = arg.isThisAssign ? parseType(ext->targetTypedef, program)
                                      : parseType(arg.type, program);
        params.push_back(t);
        mangledName += "_" + getMangledType(t);
      }

      // We record it in the table so that resolveOverload can work its magic
      registerOverload(baseName, mangledName, params,
                       parseType(m->returnType, program));
      functionTypes[mangledName] = parseType(m->returnType, program);
    }
  }

  program->accept(this);
  return errors.empty();
}

bool Sema::methodExistsInClass(const std::string &className,
                               const std::string &methodName) {
  std::string prefix = className + "_" + methodName;
  for (const auto &pair : functionTypes) {
    if (pair.first.find(prefix) == 0)
      return true;
  }
  return false;
}

void Sema::validateInterfaceCompliance(StructDeclNode *node,
                                       const std::string &interfaceName) {
  if (!structASTs.count(interfaceName)) {
    reportError(node, "Semantic Error: Interface '" + interfaceName +
                          "' is not defined.");
    return;
  }

  StructDeclNode *ifaceAST = structASTs[interfaceName];

  for (const auto &f : ifaceAST->fields) {
    if (f.isStatic)
      continue;
    if (!hasClassField(node, f.name, f.typeName)) {
      reportError(node, "Semantic Error: Class '" + node->name +
                            "' fails to implement interface '" + interfaceName +
                            "'. Missing field '" + f.name + "' of type '" +
                            f.typeName + "'.");
    }
  }

  for (const auto &m : ifaceAST->methods) {
    if (m->isStatic || m->isConstructor || m->isDestructor)
      continue;
    if (!hasClassMethod(node, m.get())) {
      reportError(node, "Semantic Error: Class '" + node->name +
                            "' fails to implement interface '" + interfaceName +
                            "'. Missing method '" + m->name + "'.");
    }
  }
}

std::string Sema::resolveParamType(StructDeclNode *node,
                                   const FunctionParam &param) {
  if (!param.isThisAssign)
    return param.type;

  StructDeclNode *curr = node;
  while (curr) {
    for (const auto &f : curr->fields) {
      if (f.name == param.name)
        return f.typeName;
    }
    if (!curr->baseClass.empty() && structASTs.count(curr->baseClass)) {
      curr = structASTs[curr->baseClass];
    } else {
      curr = nullptr;
    }
  }
  return "error";
}

bool Sema::hasClassField(StructDeclNode *node, const std::string &name,
                         const std::string &type) {
  for (const auto &f : node->fields) {
    if (!f.isStatic && f.name == name && f.typeName == type)
      return true;
  }
  // If we don't find it, traverse the bloodline. Pray there are no circular
  // inheritance chains here.
  if (!node->baseClass.empty() && structASTs.count(node->baseClass)) {
    return hasClassField(structASTs[node->baseClass], name, type);
  }
  return false;
}

bool Sema::hasClassMethod(StructDeclNode *node, FunctionNode *m) {
  for (const auto &nm : node->methods) {
    if (!nm->isStatic && nm->name == m->name &&
        nm->args.size() == m->args.size()) {
      bool argsMatch = true;
      for (size_t i = 0; i < m->args.size(); ++i) {
        std::string nmType = resolveParamType(node, nm->args[i]);
        std::string mType = resolveParamType(node, m->args[i]);
        if (nmType != mType) {
          argsMatch = false;
          break;
        }
      }
      if (argsMatch && nm->returnType == m->returnType) {
        return true;
      }
    }
  }

  if (!node->baseClass.empty() && structASTs.count(node->baseClass)) {
    return hasClassMethod(structASTs[node->baseClass], m);
  }
  return false;
}

void Sema::registerOverload(const std::string &baseName,
                            const std::string &mangledName,
                            const std::vector<TypeInfo> &params,
                            const TypeInfo &ret) {
  // Capture the candidate in the overload table
  overloadTable[baseName].push_back({mangledName, params, ret});
}

int Sema::getConversionCost(const TypeInfo &target, const TypeInfo &source) {
  if (target.base == source.base && target.ptrDepth == source.ptrDepth &&
      target.isReference == source.isReference &&
      target.isRValueRef == source.isRValueRef)
    return 0;

  // Descent into the hell of L-value
  // If the variable is of the same type but the destination requires a
  // reference, we let it pass without penalty. The compiler will do the rest.
  if (target.isReference && !source.isReference && !source.isRValueRef &&
      target.base == source.base && target.ptrDepth == source.ptrDepth)
    return 0;

  // Mismatch in pointer levels. We are not attempting to fix this
  if (target.ptrDepth != source.ptrDepth)
    return 100;

  // Implicit promotion: int to float is an acceptable compromise.
  if (target.base == "float" && source.base == "int" && !target.isReference)
    return 1;

  // The null constant is a universal donor for any direction
  if (target.ptrDepth > 0 && source.base == "null")
    return 1;

  return 1000; // The type system says no
}

std::string Sema::resolveOverload(const std::string &baseName,
                                  const std::vector<TypeInfo> &argTypes,
                                  ASTNode *node, TypeInfo &outReturnType) {
  if (!overloadTable.count(baseName))
    return "";

  std::string bestMangled = "";
  int minCost = 1000;

  for (const auto &cand : overloadTable[baseName]) {
    if (cand.paramTypes.size() != argTypes.size())
      continue;

    int currentCost = 0;
    bool possible = true;
    for (size_t i = 0; i < argTypes.size(); ++i) {
      int cost = getConversionCost(cand.paramTypes[i], argTypes[i]);
      if (cost >= 1000) {
        possible = false;
        break;
      }
      currentCost += cost;
    }

    // Select the candidate who requires the least amount of butchery.
    if (possible && currentCost < minCost) {
      minCost = currentCost;
      bestMangled = cand.mangledName;
      outReturnType = cand.returnType;
    }
  }

  if (bestMangled.empty() && node) {
    reportError(node,
                "error: no matching function for call to '" + baseName + "'");
  }

  return bestMangled;
}

void Sema::visit(StructDeclNode *node) {
  for (const auto &interfaceName : node->interfaces) {
    validateInterfaceCompliance(node, interfaceName);
  }
}

void Sema::visit(ExtensionNode *node) {
  bool oldState = isProcessingExtension;
  isProcessingExtension = true;
  for (auto &method : node->methods) {
    method->accept(this);
  }
  isProcessingExtension = oldState;
}

void Sema::visit(MemberAccessNode *node) {
  static const std::unordered_set<std::string> primitiveTypes = {
      "int", "float", "bool", "uint", "char", "String"};

  node->object->accept(this);

  bool isPrimitive = primitiveTypes.count(currentExprType.base) > 0;

  if (isPrimitive) {
    // We do nothing else, currentExprType is already the type of the object
    return;
  }

  // Auto-deref: LLVM takes care of making both (values ​​and pointers)
  // accessible via CreateStructGEP, so we only validate the base

  if (!customStructs.count(currentExprType.base)) {
    reportError(node, "error: type '" + currentExprType.base +
                          "' is not a class or struct");
    currentExprType = {"error", 0, false};
    return;
  }

  bool isClassSymbol = false;
  if (auto varNode = dynamic_cast<VariableNode *>(node->object.get())) {
    if (customStructs.count(varNode->name) && !lookup(varNode->name)) {
      isClassSymbol = true;
    }
  }

  std::string currObjName = currentExprType.base;
  bool fieldFound = false;
  StructDef::Field foundField;
  std::string classDefiningField;

  while (!currObjName.empty() && customStructs.count(currObjName)) {
    auto &def = customStructs[currObjName];
    if (def.fields.count(node->field)) {
      foundField = def.fields[node->field];
      classDefiningField = currObjName;
      fieldFound = true;
      break;
    }

    // Ascend to the ancestor if one exists
    if (structASTs.count(currObjName) &&
        !structASTs[currObjName]->baseClass.empty()) {
      currObjName = structASTs[currObjName]->baseClass;
    } else {
      break;
    }
  }

  if (fieldFound) {
    // Access and static validations
    if (isClassSymbol && !foundField.isStatic) {
      reportError(node, "error: invalid use of non-static member '" +
                            node->field + "' via class name");
      currentExprType = {"error", 0, false};
      return;
    }

    bool isPub = (foundField.mod == AccessModifier::Public) ||
                 (foundField.mod == AccessModifier::Implicit &&
                  (node->field.empty() || node->field[0] != '_'));

    if (!isPub && currentClass != classDefiningField) {
      reportError(node, "error: '" + node->field +
                            "' is a private member of '" + classDefiningField +
                            "'");
    }

    currentExprType = foundField.type;
  } else {
    // No field was found. We do nothing; we let CallNode attempt to resolve it
    // as a method. currentExprType is already the object's type
  }
}

void Sema::visit(ProgramNode *node) {
  enterScope();
  for (auto &st : node->structs) {
    st->accept(this);

    currentClass = st->name; // Setup this context for initializers

    // Typecheck initializers against field definitions
    for (auto &field : st->fields) {
      if (field.initializer) {
        field.initializer->accept(this);
        TypeInfo fieldType = parseType(field.typeName);
        checkAssignment(fieldType, currentExprType, field.initializer.get());
      }
    }

    currentClass = "";
    for (auto &method : st->methods) {
      method->accept(this);
    }
  }
  for (auto &ext : node->extensions) {
    ext->accept(this);
  }
  for (auto &func : node->functions)
    func->accept(this);
  exitScope();
}

void Sema::visit(ThisNode *node) {
  if (currentClass.empty() || inStaticMethod) {
    reportError(
        node,
        "error: invalid use of 'this' outside of a non-static member function");
    currentExprType = {"error", 0, false};
    return;
  }
  currentExprType = {currentClass, 1, false};
}

void Sema::visit(FunctionNode *node) {
  enterScope();
  std::string previousClassContext = currentClass;
  bool previousStaticContext = inStaticMethod;

  if (node->isMethod || node->isConstructor || node->isDestructor) {
    currentClass = node->className;
    inStaticMethod = node->isStatic;
  }

  // Recover mangled identity. Name mangling is unforgiving.
  // We must manually inject the implicit 'this' parameter back into the
  // signature. If we skip this, the lookup fails, we fallback to an 'error'
  // return type, and the semantic pass bleeds out on valid returns.
  std::string lookupName = node->name;
  if (node->isMethod || node->isConstructor || node->isDestructor) {
    lookupName = (isProcessingExtension ? "ext_" : "") + node->className + "_" +
                 node->name;

    if (isProcessingExtension) {
      TypeInfo targetType = parseType(node->className, node);
      lookupName += "_" + getMangledType(targetType);
    } else if (!node->isStatic && !node->isConstructor) {
      TypeInfo thisType = {node->className, 1, false};
      lookupName += "_" + getMangledType(thisType);
    }
  }

  for (auto &arg : node->args) {
    TypeInfo t = arg.isThisAssign
                     ? customStructs[node->className].fields[arg.name].type
                     : parseType(arg.type, node);
    lookupName += "_" + getMangledType(t);
  }

  if (functionTypes.count(lookupName)) {
    currentReturnType = functionTypes[lookupName];
  } else {
    // If we got here, something went seriously wrong in the previous
    // registration
    currentReturnType = {"error", 0, false};
  }

  for (auto &arg : node->args) {
    if (arg.isThisAssign) {
      if (node->isStatic) {
        reportError(
            node,
            "error: static methods cannot use 'this.' field assignment syntax");
      }

      TypeInfo fieldType = {"error", 0, false};
      if (!currentClass.empty() && customStructs.count(currentClass)) {
        if (customStructs[currentClass].fields.count(arg.name)) {
          fieldType = customStructs[currentClass].fields[arg.name].type;
        } else {
          reportError(node, "Field '" + arg.name + "' not found in class '" +
                                currentClass + "'.");
        }
      }
      scopeStack.back()[arg.name] = {fieldType, false};
    } else {
      scopeStack.back()[arg.name] = {parseType(arg.type, node), false};
    }
  }

  for (auto &stmt : node->body)
    stmt->accept(this);

  currentClass = previousClassContext;
  inStaticMethod = previousStaticContext;
  exitScope();
}

void Sema::visit(IfNode *node) {
  node->condition->accept(this);

  enterScope();
  for (auto &s : node->thenBody)
    s->accept(this);
  exitScope();

  if (!node->elseBody.empty()) {
    enterScope();
    for (auto &s : node->elseBody)
      s->accept(this);
    exitScope();
  }
}

void Sema::visit(BlockNode *node) {
  enterScope();
  for (auto &stmt : node->statements) {
    stmt->accept(this);
  }
  exitScope();
}

void Sema::visit(WhileNode *node) {
  node->condition->accept(this);
  if (currentExprType.base != "bool") {
    reportError(node,
                "Type Mismatch: 'while' condition must evaluate to 'bool'.");
  }

  loopDepth++;
  enterScope();
  for (auto &stmt : node->body)
    stmt->accept(this);
  exitScope();
  loopDepth--;
}

void Sema::visit(ForNode *node) {
  enterScope();

  if (node->init)
    node->init->accept(this);

  if (node->condition) {
    node->condition->accept(this);
    if (currentExprType.base != "bool") {
      reportError(node,
                  "Type Mismatch: 'for' condition must evaluate to 'bool'.");
    }
  }

  if (node->update)
    node->update->accept(this);

  loopDepth++;
  enterScope();
  for (auto &stmt : node->body)
    stmt->accept(this);
  exitScope();
  loopDepth--;
  exitScope();
}

void Sema::visit(BreakNode *node) {
  if (loopDepth == 0)
    reportError(node, "'break' is only valid inside a loop.");
}

void Sema::visit(ContinueNode *node) {
  if (loopDepth == 0)
    reportError(node, "'continue' is only valid inside a loop.");
}

void Sema::visit(AssignNode *node) {
  node->target->accept(this);
  TypeInfo targetType = currentExprType;

  // R-Value validation: We enforce strict L-Value semantics.
  // The target must be an entity capable of holding memory (variables, fields,
  // pointers, array indices).
  bool isValidLValue = dynamic_cast<VariableNode *>(node->target.get()) ||
                       dynamic_cast<MemberAccessNode *>(node->target.get()) ||
                       dynamic_cast<SubscriptNode *>(node->target.get()) ||
                       dynamic_cast<DerefNode *>(node->target.get());

  if (!isValidLValue) {
    reportError(
        node,
        "error: expression is not assignable (requires a valid l-value).");
  }

  node->value->accept(this);
  TypeInfo valueType = currentExprType;

  checkAssignment(targetType, valueType, node);
}

void Sema::visit(VarDeclNode *node) {
  if (scopeStack.back().count(node->name)) {
    reportError(node, "Redefinition of variable '" + node->name +
                          "' in the same scope.");
  }

  TypeInfo declType = parseType(node->typeName, node);

  if (node->arraySize) {
    node->arraySize->accept(this);
    if (currentExprType.base != "int") {
      reportError(node, "Array size must evaluate to an integer.");
    }
    declType.isArray = true;
  }

  if (node->initializer) {
    node->initializer->accept(this);
    checkAssignment(declType, currentExprType, node);
  }

  scopeStack.back()[node->name] = {declType, node->isConst};
}

void Sema::visit(SubscriptNode *node) {
  node->object->accept(this);
  TypeInfo objType = currentExprType;

  if (objType.isNullable) {
    reportError(node,
                "error: cannot access array element of a nullable pointer. Use "
                "'!' to assert non-nullity (e.g., 'ptr![index]').");
  }

  // Since VariableNode already decayed, objType is already a pointer (ptrDepth
  // > 0)
  if (objType.ptrDepth == 0) {
    reportError(node, "Subscript operator [] requires a pointer or array.");
    currentExprType = {"error", 0, false};
    return;
  }

  node->index->accept(this);
  if (currentExprType.base != "int") {
    reportError(node, "Array index must be an integer.");
  }

  currentExprType = objType;
  currentExprType.ptrDepth--; // The resulting type is the extracted base type

  currentExprType.isNullable = false;
}

void Sema::visit(VariableNode *node) {
  Symbol *sym = lookup(node->name);
  if (sym) {
    currentExprType = sym->type;
    // Native Array-to-Pointer Decay
    if (currentExprType.isArray) {
      currentExprType.isArray = false;
      currentExprType.ptrDepth++;
    }
    return;
  }

  static const std::unordered_set<std::string> primitives = {
      "int", "float", "bool", "uint", "char", "String", "void"};

  if (primitives.count(node->name)) {
    currentExprType = {node->name, 0, false};
    return;
  }

  // Fallback to implicit 'this' member lookup
  if (!currentClass.empty() && customStructs.count(currentClass)) {
    std::string curr = currentClass;
    while (!curr.empty() && customStructs.count(curr)) {
      if (customStructs[curr].fields.count(node->name)) {
        currentExprType = customStructs[curr].fields[node->name].type;
        return;
      }
      if (structASTs.count(curr) && !structASTs[curr]->baseClass.empty()) {
        curr = structASTs[curr]->baseClass;
      } else {
        break;
      }
    }
  }

  if (customStructs.count(node->name)) {
    currentExprType = {node->name, 0, false};
    return;
  }

  reportError(node, "error: use of undeclared identifier '" + node->name + "'");
  currentExprType = {"error", 0, false};
}

void Sema::visit(BinaryOpNode *node) {
  node->left->accept(this);
  TypeInfo leftT = currentExprType;
  node->right->accept(this);
  TypeInfo rightT = currentExprType;

  if (node->op == "&&" || node->op == "||") {
    if (leftT.base != "bool" || rightT.base != "bool") {
      reportError(node, "Logical operators require boolean operands.");
    }
    currentExprType = {"bool", 0, false};
    return;
  }

  if (node->op == "<" || node->op == ">" || node->op == "<=" ||
      node->op == ">=") {
    currentExprType = {"bool", 0, false};
    return;
  }

  if (node->op == "==" || node->op == "!=") {
    currentExprType = {"bool", 0, false};
    return;
  }

  if (leftT.base == "String" || rightT.base == "String") {
    if (node->op != "+")
      reportError(node, "Invalid operator for String type");
    currentExprType = {"String", 0, false};
  } else {
    currentExprType = (leftT.base == "float" || rightT.base == "float")
                          ? TypeInfo{"float", 0}
                          : TypeInfo{"int", 0};
  }
}

// Stubs
void Sema::visit(NumberNode *node) { currentExprType = {"int", 0}; }
void Sema::visit(FloatNode *node) { currentExprType = {"float", 0}; }
void Sema::visit(BoolNode *node) { currentExprType = {"bool", 0}; }
void Sema::visit(StringNode *node) { currentExprType = {"String", 0}; }
void Sema::visit(NullLiteralNode *node) { currentExprType = {"null", 0, true}; }

void Sema::visit(UnaryMinusNode *node) {
  node->operand->accept(this);
  if (currentExprType.base == "int" || currentExprType.base == "float") {
    // The type remains
  } else {
    reportError(node, "Unary minus requires numeric operand.");
    currentExprType = {"error", 0, false};
  }
}

void Sema::visit(ReturnNode *node) {
  if (node->returnValue) {
    if (currentReturnType.base == "void" && currentReturnType.ptrDepth == 0) {
      reportError(node, "error: void function should not return a value");
    } else {
      node->returnValue->accept(this);
      checkAssignment(currentReturnType, currentExprType, node);
    }
  } else if (currentReturnType.base != "void" ||
             currentReturnType.ptrDepth > 0) {
    reportError(node,
                "error: non-void function must return a value (expected '" +
                    typeToString(currentReturnType) + "')");
  }
}

void Sema::visit(CallNode *node) {
  std::vector<TypeInfo> argTypes;
  for (auto &a : node->arguments) {
    a->accept(this);
    argTypes.push_back(currentExprType);
  }

  if (node->object) {
    node->object->accept(this);
    TypeInfo objType = currentExprType;

    bool isStaticCall = false;
    if (auto varNode = dynamic_cast<VariableNode *>(node->object.get())) {
      if (customStructs.count(varNode->name) && !lookup(varNode->name)) {
        isStaticCall = true;
      }
    }

    /* * Primitive type safety bypass.
     * Extensions for primitives are compiled to receive the direct value, not a
     * pointer. If we blindly increase ptrDepth, Sema resolves int* instead of
     * int and fails the overload lookup.
     */
    static const std::unordered_set<std::string> primitives = {
        "int", "float", "bool", "uint", "char", "String"};
    bool isPrimitive = primitives.count(objType.base) > 0;

    if (!isStaticCall && !isPrimitive && objType.ptrDepth == 0 &&
        !objType.isReference) {
      objType.ptrDepth = 1;
    }

    std::string baseName = objType.base + "_" + node->callee;
    TypeInfo outRet;

    std::vector<TypeInfo> resolutionArgs = argTypes;
    if (!isStaticCall) {
      resolutionArgs.insert(resolutionArgs.begin(), objType);
    }

    // Bruteforcing the vtable because name mangling is a bitch
    std::string resolved =
        resolveOverload(baseName, resolutionArgs, nullptr, outRet);
    if (resolved.empty()) {
      std::string extensionName = "ext_" + objType.base + "_" + node->callee;
      std::vector<TypeInfo> extArgs = argTypes;
      extArgs.insert(extArgs.begin(), objType);
      resolved = resolveOverload(extensionName, extArgs, node, outRet);
    }

    if (!resolved.empty()) {
      currentExprType = outRet;
      node->resolvedMangledName = resolved;
    } else {
      reportError(node, "error: no member function matching '" + node->callee +
                            "' in '" + objType.base + "'");
      currentExprType = {"error", 0, false};
    }
    return;
  }

  // Stack allocation bypass (RVO simulation)
  if (customStructs.count(node->callee)) {
    std::string baseName = node->callee + "_" + node->callee;
    TypeInfo outRet;
    std::string resolved = resolveOverload(baseName, argTypes, node, outRet);

    if (!resolved.empty()) {
      node->resolvedMangledName = resolved;
    }
    currentExprType = {node->callee, 0, false};
    return;
  }

  if (node->callee == "print") {
    currentExprType = {"void", 0, false};
    return;
  }
  if (node->callee == "int" || node->callee == "float" ||
      node->callee == "bool") {
    currentExprType = {node->callee, 0, false};
    return;
  }

  TypeInfo outRet;
  std::string resolved =
      resolveOverload(node->callee, argTypes, nullptr, outRet);
  if (!resolved.empty()) {
    currentExprType = outRet;
    node->resolvedMangledName = resolved;
  } else {
    reportError(node,
                "error: call to undeclared function or unresolved overload '" +
                    node->callee + "'");
    currentExprType = {"error", 0, false};
  }
}

void Sema::visit(NullAssertNode *node) {
  node->operand->accept(this);
  currentExprType.isNullable = false;
}

void Sema::visit(LogicalNotNode *node) {
  node->operand->accept(this);

  // No negotiatons with non-booleans.
  if (currentExprType.base != "bool" || currentExprType.ptrDepth > 0) {
    reportError(
        node,
        "error: logical NOT operator '!' requires a 'bool' operand (found '" +
            typeToString(currentExprType) + "')");
  }

  currentExprType = {"bool", 0, false};
}

void Sema::visit(AddressOfNode *node) {
  node->operand->accept(this);
  currentExprType.ptrDepth++;
}

void Sema::visit(DerefNode *node) {
  node->operand->accept(this);
  if (currentExprType.isNullable) {
    reportError(node, "error: cannot dereference a nullable pointer. You must "
                      "assert it first with '!'.");
  }
  if (currentExprType.ptrDepth > 0)
    currentExprType.ptrDepth--;
}

void Sema::visit(NewNode *node) {
  TypeInfo resultType = parseType(node->typeName, node);
  resultType.ptrDepth = 1; // Heap allocation always returns a pointer

  std::vector<TypeInfo> argTypes;
  for (auto &a : node->arguments) {
    a->accept(this);
    argTypes.push_back(currentExprType);
  }

  if (customStructs.count(node->typeName)) {
    if (!node->arraySize) {
      std::string baseName = node->typeName + "_" + node->typeName;
      TypeInfo outRet;
      std::string resolved = resolveOverload(baseName, argTypes, node, outRet);

      if (resolved.empty()) {
        reportError(node, "error: no matching constructor found for '" +
                              node->typeName + "'");
      } else {
        node->resolvedMangledName = resolved;
      }
    }
  }

  currentExprType = resultType;
}

void Sema::visit(DeleteNode *node) { node->pointerExpr->accept(this); }

void Sema::visit(MoveNode *node) {
  node->operand->accept(this);
  currentExprType.isRValueRef = true;
  currentExprType.isReference = false;
}

void Sema::visit(ModuleNode *node) {
  for (auto &st : node->structs)
    st->accept(this);
  for (auto &ext : node->extensions)
    ext->accept(this);
  for (auto &fn : node->functions)
    fn->accept(this);
}

} // namespace utopia