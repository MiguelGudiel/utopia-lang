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

bool Sema::checkAssignment(const TypeInfo &target, const TypeInfo &source,
                           ASTNode *node) {
  if (!target.isNullable && source.isNullable) {
    reportError(node,
                "Type Mismatch: Cannot assign nullable type to non-nullable '" +
                    target.base + "'");
    return false;
  }
  if (target.ptrDepth != source.ptrDepth && source.base != "null") {
    reportError(node,
                "Type Mismatch: Pointer indirection level does not match");
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
    functionTypes[func->name] = parseType(func->returnType);
  }

  for (auto &st : program->structs) {
    StructDef def;
    def.isClass = st->isClass;
    int idx = 0;
    for (auto &f : st->fields) {
      def.fields[f.name] = {parseType(f.typeName), f.modifier, idx++};
    }
    customStructs[st->name] = def;

    // register methods into global scope so the symbol table doesn't shit
    // itself on 'this' dispatches
    for (auto &m : st->methods) {
      std::string mangledName = st->name + "_" + m->name;
      functionTypes[mangledName] = parseType(m->returnType);
    }
  }

  program->accept(this);
  return errors.empty();
}

void Sema::visit(StructDeclNode *node) {}

void Sema::visit(MemberAccessNode *node) {
  node->object->accept(this);

  if (currentExprType.ptrDepth > 0) {
    reportError(
        node,
        "The '.' operator requires an object by value. Use '(*obj).field'.");
    return;
  }

  if (!customStructs.count(currentExprType.base)) {
    reportError(node, "Type '" + currentExprType.base +
                          "' has no fields or does not exist.");
    currentExprType = {"error", 0, false};
    return;
  }

  auto &def = customStructs[currentExprType.base];
  if (!def.fields.count(node->field)) {
    reportError(node, "Field '" + node->field + "' does not exist.");
    currentExprType = {"error", 0, false};
    return;
  }

  auto &field = def.fields[node->field];

  bool isPub = false;
  if (field.mod == AccessModifier::Public) {
    isPub = true;
  } else if (field.mod == AccessModifier::Private) {
    isPub = false;
  } else {
    if (!node->field.empty() && node->field[0] == '_') {
      isPub = false;
    } else {
      isPub = !def.isClass;
    }
  }

  if (!isPub) {
    reportError(node,
                "Access Violation: Field '" + node->field + "' is private.");
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
    reportError(
        node, "Context Error: 'this' can only be used inside a class method.");
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

  // /* Recover mangled identity to prevent return type mismatches */
  std::string lookupName = node->name;
  if (node->isMethod || node->isConstructor || node->isDestructor) {
    lookupName = node->className + "_" + node->name;
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
  node->initializer->accept(this);

  checkAssignment(declType, currentExprType, node);
  scopeStack.back()[node->name] = {declType, node->isConst};
}

void Sema::visit(VariableNode *node) {
  Symbol *sym = lookup(node->name);
  if (sym) {
    currentExprType = sym->type;
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

  reportError(node, "Use of undeclared identifier '" + node->name + "'");
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
      reportError(
          node,
          "Void return mismatch: Cannot return a value from a void function.");
    } else {
      node->returnValue->accept(this);
      checkAssignment(currentReturnType, currentExprType, node);
    }
  } else {
    if (currentReturnType.base != "void" || currentReturnType.ptrDepth > 0) {
      reportError(node, "Missing return value: Expected '" +
                            currentReturnType.base + "'.");
    }
  }
}

void Sema::visit(CallNode *node) {
  for (auto &a : node->arguments)
    a->accept(this);

  if (node->object) {
    node->object->accept(this);
    std::string objType = currentExprType.base;
    std::string mangledName = objType + "_" + node->callee;

    if (functionTypes.count(mangledName)) {
      currentExprType = functionTypes[mangledName];
    } else {
      reportError(node, "Method '" + node->callee + "' not found in class '" +
                            objType + "'.");
      currentExprType = {"error", 0, false};
    }
    return;
  }

  if (node->callee == "print") {
    currentExprType = {"void", 0};
    return;
  }

  if (functionTypes.count(node->callee)) {
    currentExprType = functionTypes[node->callee];
  } else {
    currentExprType = {"int", 0};
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
  currentExprType = parseType(node->typeName);
  currentExprType.ptrDepth = 1;

  for (auto &a : node->arguments)
    a->accept(this);

  std::string ctorName = node->typeName + "_" + node->typeName;
  if (!functionTypes.count(ctorName) && !node->arguments.empty()) {
    reportError(node,
                "No matching constructor found for '" + node->typeName + "'.");
  }
}

void Sema::visit(DeleteNode *node) { node->pointerExpr->accept(this); }

} // namespace utopia