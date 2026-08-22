#include "LspCore.hpp"

namespace utopia::lsp {

void LocalVarCollector::visit(const ModuleNode *n) {
  for (auto *s : n->statements) {
    if (s->line <= targetLine)
      dispatch(s);
  }
}

void LocalVarCollector::visit(const NamespaceDeclNode *n) {
  if (n->line <= targetLine) {
    std::string oldNs = currentNamespace;
    currentNamespace = currentNamespace.empty()
                           ? std::string(n->name)
                           : currentNamespace + "." + std::string(n->name);
    for (auto *s : n->statements) {
      if (s->line <= targetLine)
        dispatch(s);
    }
    if (targetLine > n->endLine && !n->isFileScoped) {
      currentNamespace = oldNs;
    }
  }
}

void LocalVarCollector::visit(const UsingNode *n) {
  if (n->line <= targetLine)
    activeUsings.push_back(std::string(n->name));
}

void LocalVarCollector::visit(const FunctionDeclNode *n) {
  if (n->line <= targetLine && n->endLine >= targetLine) {
    closestFunc = n;
    for (auto *p : n->params)
      dispatch(p);
    if (n->body)
      dispatch(n->body);
  }
}

void LocalVarCollector::visit(const VarDeclNode *n) {
  if (n->line <= targetLine)
    locals.push_back(n);
  if (n->initializer)
    dispatch(n->initializer);
}

void LocalVarCollector::visit(const BlockNode *n) {
  for (auto *s : n->statements) {
    if (s->line <= targetLine)
      dispatch(s);
  }
}

void LocalVarCollector::visit(const IfNode *n) {
  if (n->condition)
    dispatch(n->condition);
  if (n->thenBlock && n->thenBlock->line <= targetLine)
    dispatch(n->thenBlock);
  if (n->elseBlock && n->elseBlock->line <= targetLine)
    dispatch(n->elseBlock);
}

void LocalVarCollector::visit(const ForNode *n) {
  if (n->initStatement && n->initStatement->line <= targetLine)
    dispatch(n->initStatement);
  if (n->condition)
    dispatch(n->condition);
  if (n->increment)
    dispatch(n->increment);
  if (n->body && n->body->line <= targetLine)
    dispatch(n->body);
}

void LocalVarCollector::visit(const ForInNode *n) {
  if (n->loopVar && n->loopVar->line <= targetLine)
    dispatch(n->loopVar);
  if (n->iterable)
    dispatch(n->iterable);
  if (n->body && n->body->line <= targetLine)
    dispatch(n->body);
}

void LocalVarCollector::visit(const WhileNode *n) {
  if (n->condition)
    dispatch(n->condition);
  if (n->body && n->body->line <= targetLine)
    dispatch(n->body);
}

void LocalVarCollector::visit(const SwitchNode *n) {
  if (n->condition)
    dispatch(n->condition);
  for (auto *c : n->cases) {
    if (c->line <= targetLine)
      dispatch(c);
  }
}

void LocalVarCollector::visit(const CaseNode *n) {
  if (n->value)
    dispatch(n->value);
  for (auto *s : n->statements) {
    if (s->line <= targetLine)
      dispatch(s);
  }
}

void LocalVarCollector::visit(const TryStmtNode *n) {
  if (n->body && n->body->line <= targetLine)
    dispatch(n->body);
  for (const auto *clause : n->clauses) {
    if (clause->line <= targetLine)
      dispatch(clause->body);
  }
}

void LocalVarCollector::visit(const ThrowStmtNode *n) {
  if (n->value)
    dispatch(n->value);
}

void LocalVarCollector::visit(const AssertStmtNode *n) {
  if (n->condition)
    dispatch(n->condition);
}

void LocalVarCollector::visit(const ConstExprNode *n) {
  if (n->expr && n->expr->line <= targetLine)
    dispatch(n->expr);
}

void LocalVarCollector::visit(const AssignNode *n) {
  if (n->target)
    dispatch(n->target);
  if (n->value)
    dispatch(n->value);
}

void LocalVarCollector::visit(const LambdaNode *n) {
  for (auto *p : n->params)
    dispatch(p);
  if (n->isExpressionBody && n->exprBody) {
    dispatch(n->exprBody);
  } else if (n->body) {
    dispatch(n->body);
  }
}

/* Records are descended into so that method bodies expose their parameters
 * and locals (completion inside a method must see 'this' and the enclosing
 * record's context). */
void LocalVarCollector::visit(const ClassDeclNode *n) {
  if (n->line > targetLine)
    return;
  for (auto *c : n->constructors)
    dispatch(c);
  for (auto *m : n->methods)
    dispatch(m);
  if (n->destructor)
    dispatch(n->destructor);
}

void LocalVarCollector::visit(const StructDeclNode *n) {
  if (n->line > targetLine)
    return;
  for (auto *c : n->constructors)
    dispatch(c);
  for (auto *m : n->methods)
    dispatch(m);
  if (n->destructor)
    dispatch(n->destructor);
}

void LocalVarCollector::visit(const UnionDeclNode *n) {
  if (n->line > targetLine)
    return;
  for (auto *c : n->constructors)
    dispatch(c);
  for (auto *m : n->methods)
    dispatch(m);
  if (n->destructor)
    dispatch(n->destructor);
}

} // namespace utopia::lsp
