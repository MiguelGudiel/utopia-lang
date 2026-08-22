#include "LspCore.hpp"

namespace utopia::lsp {

namespace {

/* Visitor collecting foldable regions from the AST. Blocks, records and
 * declarations with bodies fold between their braces. */
class FoldingCollector : public ASTVisitor<FoldingCollector, void> {
public:
  std::vector<std::pair<int, int>> ranges; /* (startLine, endLine), 1-based */

  void addRange(const ASTNode *n) {
    if (n && n->endLine > n->line)
      ranges.emplace_back(n->line, n->endLine);
  }

  void visit(const ModuleNode *n) {
    for (auto *s : n->statements)
      dispatch(s);
  }
  void visit(const NamespaceDeclNode *n) {
    addRange(n);
    for (auto *s : n->statements)
      dispatch(s);
  }
  void visit(const UsingNode *) {}
  void visit(const FunctionDeclNode *n) {
    if (n->body)
      addRange(n->body);
    for (auto *p : n->params)
      dispatch(p);
    if (n->body)
      dispatch(n->body);
  }
  void visit(const ParamDeclNode *n) {
    if (n->defaultValue)
      dispatch(n->defaultValue);
  }
  void visit(const VarDeclNode *n) {
    if (n->initializer)
      dispatch(n->initializer);
  }
  void visit(const BlockNode *n) {
    addRange(n);
    for (auto *s : n->statements)
      dispatch(s);
  }
  void visit(const IfNode *n) {
    if (n->condition)
      dispatch(n->condition);
    if (n->thenBlock)
      dispatch(n->thenBlock);
    if (n->elseBlock)
      dispatch(n->elseBlock);
  }
  void visit(const ForNode *n) {
    if (n->initStatement)
      dispatch(n->initStatement);
    if (n->condition)
      dispatch(n->condition);
    if (n->increment)
      dispatch(n->increment);
    if (n->body)
      dispatch(n->body);
  }
  void visit(const ForInNode *n) {
    if (n->loopVar)
      dispatch(n->loopVar);
    if (n->iterable)
      dispatch(n->iterable);
    if (n->body)
      dispatch(n->body);
  }
  void visit(const WhileNode *n) {
    if (n->condition)
      dispatch(n->condition);
    if (n->body)
      dispatch(n->body);
  }
  void visit(const SwitchNode *n) {
    addRange(n);
    if (n->condition)
      dispatch(n->condition);
    for (auto *c : n->cases)
      dispatch(c);
  }
  void visit(const CaseNode *n) {
    if (n->value)
      dispatch(n->value);
    for (auto *s : n->statements)
      dispatch(s);
  }
  void visit(const TryStmtNode *n) {
    addRange(n);
    if (n->body)
      dispatch(n->body);
    for (const auto *clause : n->clauses)
      if (clause->body)
        dispatch(clause->body);
  }
  void visit(const ThrowStmtNode *n) {
    if (n->value)
      dispatch(n->value);
  }
  void visit(const AssertStmtNode *n) {
    if (n->condition)
      dispatch(n->condition);
  }
  void visit(const ConstExprNode *n) {
    if (n->expr)
      dispatch(n->expr);
  }
  void visit(const AssignNode *n) {
    dispatch(n->target);
    dispatch(n->value);
  }
  void visit(const ReturnNode *n) {
    if (n->value)
      dispatch(n->value);
  }
  void visit(const UnaryOpNode *n) { dispatch(n->expr); }
  void visit(const AwaitExprNode *n) { dispatch(n->expr); }
  void visit(const LambdaNode *n) {
    for (auto *p : n->params)
      dispatch(p);
    if (n->isExpressionBody && n->exprBody) {
      dispatch(n->exprBody);
    } else if (n->body) {
      addRange(n->body);
      dispatch(n->body);
    }
  }
  void visit(const BinaryOpNode *n) {
    dispatch(n->left);
    dispatch(n->right);
  }
  void visit(const TernaryOpNode *n) {
    dispatch(n->condition);
    dispatch(n->trueExpr);
    dispatch(n->falseExpr);
  }
  void visit(const CastNode *n) { dispatch(n->expr); }
  void visit(const IsExprNode *n) { dispatch(n->expr); }
  void visit(const ImplicitCastNode *n) { dispatch(n->expr); }
  void visit(const ArraySubscriptNode *n) {
    dispatch(n->base);
    dispatch(n->index);
  }
  void visit(const ArrayLiteralNode *n) {
    for (auto *e : n->elements)
      dispatch(e);
  }
  void visit(const MapLiteralNode *n) {
    for (auto *k : n->keys)
      dispatch(k);
    for (auto *v : n->values)
      dispatch(v);
  }
  void visit(const NewExprNode *n) {
    if (n->arraySize)
      dispatch(n->arraySize);
    for (auto *a : n->args)
      dispatch(a);
  }
  void visit(const DeleteExprNode *n) { dispatch(n->ptr); }
  void visit(const DestructorCallNode *n) { dispatch(n->object); }
  void visit(const AnnotationNode *n) {
    for (auto *a : n->args)
      dispatch(a);
  }
  void visit(const StructDeclNode *n) {
    addRange(n);
    for (auto *f : n->fields)
      dispatch(f);
    for (auto *m : n->methods)
      dispatch(m);
    for (auto *c : n->constructors)
      dispatch(c);
    if (n->destructor)
      dispatch(n->destructor);
  }
  void visit(const ClassDeclNode *n) {
    addRange(n);
    for (auto *f : n->fields)
      dispatch(f);
    for (auto *m : n->methods)
      dispatch(m);
    for (auto *c : n->constructors)
      dispatch(c);
    if (n->destructor)
      dispatch(n->destructor);
  }
  void visit(const UnionDeclNode *n) {
    addRange(n);
    for (auto *f : n->fields)
      dispatch(f);
    for (auto *m : n->methods)
      dispatch(m);
    for (auto *c : n->constructors)
      dispatch(c);
    if (n->destructor)
      dispatch(n->destructor);
  }
  void visit(const EnumDeclNode *n) {
    addRange(n);
    for (auto *m : n->members)
      dispatch(m);
  }
  void visit(const EnumMemberNode *n) {
    if (n->initializer)
      dispatch(n->initializer);
  }
  void visit(const TypedefDeclNode *) {}
  void visit(const AnnotationDeclNode *n) {
    addRange(n);
    for (auto *f : n->fields)
      dispatch(f);
    if (n->constructor)
      dispatch(n->constructor);
  }
  void visit(const NumberNode *) {}
  void visit(const BoolNode *) {}
  void visit(const CharNode *) {}
  void visit(const RuneNode *) {}
  void visit(const StringNode *) {}
  void visit(const NullNode *) {}
  void visit(const TypeLiteralNode *) {}
  void visit(const VariableNode *) {}
  void visit(const FunctionCallNode *n) {
    if (n->target)
      dispatch(n->target);
    for (auto *a : n->args)
      dispatch(a);
  }
  void visit(const MemberAccessNode *n) {
    if (n->object)
      dispatch(n->object);
  }
  void visit(const BreakNode *) {}
  void visit(const ContinueNode *) {}
};

} // namespace

void handleFoldingRange(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];

  json res = json::array();

  DocumentState doc;
  if (documents.get(uri, doc) && doc.ast) {
    FoldingCollector collector;
    collector.dispatch(doc.ast);

    /* Deduplicate nested ranges that share a start line; VS Code rejects
     * overlapping regions on the same line. */
    std::map<int, int> bestByStart;
    for (const auto &[startLine, endLine] : collector.ranges) {
      auto it = bestByStart.find(startLine);
      if (it == bestByStart.end() || endLine > it->second) {
        bestByStart[startLine] = endLine;
      }
    }

    for (const auto &[startLine, endLine] : bestByStart) {
      res.push_back({{"startLine", startLine - 1},
                     {"startCharacter", 0},
                     {"endLine", endLine - 1}});
    }
  }

  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
}

} // namespace utopia::lsp
