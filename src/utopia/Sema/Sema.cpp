// File: src/utopia/Sema/Sema.cpp
#include "utopia/Sema/Sema.hpp"

namespace utopia {

void Sema::enterScope() { scopeStack.push_back({}); }
void Sema::exitScope() { scopeStack.pop_back(); }

Sema::Symbol *Sema::lookup(const std::string &name) {
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    if (it->count(name))
      return &(*it)[name];
  }
  return nullptr;
}

void Sema::reportError(ASTNode *node, const std::string &message) {
  errors.push_back(
      {node->line, node->column, node->endLine, node->endColumn, message});
}

TypeInfo Sema::parseType(const std::string &typeName) {
  TypeInfo t;
  std::string temp = typeName;
  if (!temp.empty() && temp.back() == '?') {
    t.isNullable = true;
    temp.pop_back();
  }
  while (!temp.empty() && temp.back() == '*') {
    t.ptrDepth++;
    temp.pop_back();
  }
  t.base = temp;
  return t;
}

std::string typeToString(const TypeInfo &t) {
  std::string s = t.base;
  for (unsigned i = 0; i < t.ptrDepth; ++i)
    s += "*";
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

  return true;
}

bool Sema::analyze(ProgramNode *program) {
  errors.clear();
  scopeStack.clear();
  functionTypes.clear();
  customStructs.clear();
  currentClass.clear();
  loopDepth = 0;

  for (auto &func : program->functions) {
    std::string mangledName = func->name;
    for (auto &arg : func->args)
      mangledName += "_" + getMangledType(parseType(arg.type));
    functionTypes[mangledName] = parseType(func->returnType);
  }

  for (auto &st : program->structs) {
    StructDef def;
    def.isClass = st->isClass;
    int idx = 0;
    for (auto &f : st->fields) {
      def.fields[f.name] = {parseType(f.typeName), f.modifier, idx++};
    }
    customStructs[st->name] = def;

    // register methods into global scope
    for (auto &m : st->methods) {
      std::string mangledName = st->name + "_" + m->name;
      for (auto &arg : m->args) {
        TypeInfo t =
            arg.isThisAssign ? def.fields[arg.name].type : parseType(arg.type);
        mangledName += "_" + getMangledType(t);
      }
      functionTypes[mangledName] = parseType(m->returnType);
    }
  }

  program->accept(this);
  return errors.empty();
}

void Sema::visit(StructDeclNode *node) {}

void Sema::visit(MemberAccessNode *node) {
  node->object->accept(this);

  // Auto-deref: LLVM se encarga de que ambos (valores y punteros)
  // sean accesibles vía CreateStructGEP, así que solo validamos la base.

  if (!customStructs.count(currentExprType.base)) {
    reportError(node, "error: type '" + currentExprType.base +
                          "' is not a class or struct");
    currentExprType = {"error", 0, false};
    return;
  }

  auto &def = customStructs[currentExprType.base];
  if (!def.fields.count(node->field)) {
    reportError(node, "error: no member named '" + node->field + "' in '" +
                          currentExprType.base + "'");
    currentExprType = {"error", 0, false};
    return;
  }

  auto &field = def.fields[node->field];

  // Implicit access resolution: Public by default unless it starts with '_'
  bool isPub = (field.mod == AccessModifier::Public) ||
               (field.mod == AccessModifier::Implicit &&
                (node->field.empty() || node->field[0] != '_'));

  if (!isPub && currentClass != currentExprType.base) {
    reportError(node, "error: '" + node->field + "' is a private member of '" +
                          currentExprType.base + "'");
  }

  currentExprType = field.type;
}

void Sema::visit(ProgramNode *node) {
  enterScope();
  for (auto &st : node->structs) {
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
  for (auto &func : node->functions)
    func->accept(this);
  exitScope();
}

void Sema::visit(ThisNode *node) {
  if (currentClass.empty()) {
    reportError(node, "error: 'this' used outside of class context");
    currentExprType = {"error", 0, false};
    return;
  }
  currentExprType = {currentClass, 1, false};
}

void Sema::visit(FunctionNode *node) {
  enterScope();
  std::string previousClassContext = currentClass;

  if (node->isMethod || node->isConstructor || node->isDestructor) {
    currentClass = node->className;
  }

  // Recover mangled identity to prevent return type mismatches
  std::string lookupName = node->name;
  if (node->isMethod || node->isConstructor || node->isDestructor) {
    lookupName = node->className + "_" + node->name;
  }
  for (auto &arg : node->args) {
    TypeInfo t = arg.isThisAssign
                     ? customStructs[node->className].fields[arg.name].type
                     : parseType(arg.type);
    lookupName += "_" + getMangledType(t);
  }
  currentReturnType = functionTypes[lookupName];

  for (auto &arg : node->args) {
    if (arg.isThisAssign) {
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
      scopeStack.back()[arg.name] = {parseType(arg.type), false};
    }
  }

  for (auto &stmt : node->body)
    stmt->accept(this);

  currentClass = previousClassContext;
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

  if (targetType.base == "error" ||
      (targetType.base != "String" && targetType.base != "int" &&
       targetType.base != "float" && targetType.base != "bool" &&
       targetType.ptrDepth == 0)) {
  }

  // R-Value validation: Check if we are trying to assign to something that
  // isn't an identifier or member This is a simple version, in the future we'll
  // need a proper isLValue flag

  node->value->accept(this);
  TypeInfo valueType = currentExprType;

  checkAssignment(targetType, valueType, node);
}

void Sema::visit(VarDeclNode *node) {
  if (scopeStack.back().count(node->name)) {
    reportError(node, "Redefinition of variable '" + node->name +
                          "' in the same scope.");
  }

  TypeInfo declType = parseType(node->typeName);

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

  // Como VariableNode ya hizo el decay, objType ya es un puntero (ptrDepth > 0)
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
  currentExprType.ptrDepth--; // El tipo resultante es el base extraído
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

  // Fallback to implicit 'this' member lookup
  if (!currentClass.empty() && customStructs.count(currentClass)) {
    auto &def = customStructs[currentClass];
    if (def.fields.count(node->name)) {
      currentExprType = def.fields[node->name].type;
      return;
    }
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
  std::string mangledSig = "";
  for (auto &a : node->arguments) {
    a->accept(this);
    mangledSig += "_" + getMangledType(currentExprType);
  }

  if (node->object) {
    node->object->accept(this);
    TypeInfo objType = currentExprType;

    std::string mangledName = objType.base + "_" + node->callee + mangledSig;
    if (functionTypes.count(mangledName)) {
      currentExprType = functionTypes[mangledName];
    } else {
      reportError(node, "error: no member function matching '" + node->callee +
                            "' in '" + objType.base + "'");
      currentExprType = {"error", 0, false};
    }
    return;
  }

  // Global scope calls
  if (node->callee == "print") {
    currentExprType = {"void", 0, false};
  } else if (node->callee == "int" || node->callee == "float" ||
             node->callee == "bool") {
    currentExprType = {node->callee, 0, false};
  } else {
    std::string mangledName = node->callee + mangledSig;
    if (functionTypes.count(mangledName)) {
      currentExprType = functionTypes[mangledName];
    } else if (functionTypes.count(node->callee)) {
      currentExprType = functionTypes[node->callee]; // Fallback externals
    } else {
      reportError(node,
                  "error: call to undeclared function '" + node->callee + "'");
      currentExprType = {"error", 0, false};
    }
  }
}

void Sema::visit(NullAssertNode *node) {
  node->operand->accept(this);
  currentExprType.isNullable = false;
}

void Sema::visit(AddressOfNode *node) {
  node->operand->accept(this);
  currentExprType.ptrDepth++;
}

void Sema::visit(DerefNode *node) {
  node->operand->accept(this);
  if (currentExprType.ptrDepth > 0)
    currentExprType.ptrDepth--;
}

void Sema::visit(NewNode *node) {
  TypeInfo resultType = parseType(node->typeName);
  resultType.ptrDepth = 1; // Heap allocation always returns a pointer

  // Check constructor arguments
  for (auto &a : node->arguments)
    a->accept(this);

  // Check constructor arguments and mutate signature
  std::string mangledSig = "";
  for (auto &a : node->arguments) {
    a->accept(this);
    mangledSig += "_" + getMangledType(currentExprType);
  }

  if (customStructs.count(node->typeName)) {
    std::string expectedCtor = node->typeName + "_" + node->typeName +
                               (node->arraySize ? "" : mangledSig);
    if (!functionTypes.count(expectedCtor)) {
      reportError(node, "error: no matching constructor found for '" +
                            node->typeName + "'");
    }
  }

  currentExprType = resultType;
}

void Sema::visit(DeleteNode *node) { node->pointerExpr->accept(this); }

} // namespace utopia