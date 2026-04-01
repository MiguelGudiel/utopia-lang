#include "utopia/Sema/Sema.hpp"

namespace utopia {

void Sema::enterScope() { scopeStack.push_back({}); }
void Sema::exitScope() { scopeStack.pop_back(); }

Sema::Symbol *Sema::lookup(const std::string &name) {
  // LIFO lookup to support local shadowing
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
  loopDepth = 0;

  // Forward Declaration Pre-pass
  for (auto &func : program->functions) {
    functionTypes[func->name] = parseType(func->returnType);
  }

  program->accept(this);
  return errors.empty();
}

void Sema::visit(ProgramNode *node) {
  enterScope(); // Global scope
  for (auto &func : node->functions)
    func->accept(this);
  exitScope();
}

void Sema::visit(FunctionNode *node) {
  enterScope();
  currentReturnType = functionTypes[node->name];

  for (auto &arg : node->args) {
    scopeStack.back()[arg.second] = {parseType(arg.first), false};
  }
  for (auto &stmt : node->body)
    stmt->accept(this);
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
    reportError(
        node, "Type Mismatch: 'while' condition must evaluate to 'bool'.");
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
      reportError(
          node, "Type Mismatch: 'for' condition must evaluate to 'bool'.");
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

  // Basic verification: you cannot assign to a literal (e.g., 5 = x)
  if (targetType.base == "error" ||
      (targetType.base != "String" && targetType.base != "int" &&
       targetType.base != "float" && targetType.base != "bool" &&
       targetType.ptrDepth == 0)) {
    // If the ASTTarget is not a valid variable or dereference, it will
    // logically fail. This can be improved later by checking the node type
    // directly.
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
  if (!sym) {
    reportError(node, "Use of undeclared identifier '" + node->name + "'");
    currentExprType = {"error", 0, false};
    return;
  }
  currentExprType = sym->type;
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

  if (node->callee == "print") {
    currentExprType = {"void", 0};
    return;
  }

  if (functionTypes.count(node->callee)) {
    currentExprType = functionTypes[node->callee];
  } else {
    currentExprType = {
        "int",
        0}; // Fallback for intrinsic factors (if they exist in the future)
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
}

void Sema::visit(DeleteNode *node) { node->pointerExpr->accept(this); }

} // namespace utopia