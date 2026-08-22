#include "LspCore.hpp"

namespace utopia::lsp {

namespace {

/* The declaration a reference node points at. */
const DeclNode *referencedDecl(const ASTNode *node);

/* Field layout rebuilds during Sema can duplicate a record's field nodes,
 * so pointer equality alone misses references; identity is defined by
 * (kind, file, line, name) for source-level comparisons. */
bool sameDecl(const DeclNode *a, const DeclNode *b) {
  if (a == b)
    return true;
  if (!a || !b || a->kind != b->kind || a->line != b->line)
    return false;
  if (!a->declFilePath.empty() && !b->declFilePath.empty() &&
      a->declFilePath != b->declFilePath)
    return false;
  std::string_view an = a->fqName;
  std::string_view bn = b->fqName;
  if (auto *v = llvm::dyn_cast<VarDeclNode>(a))
    an = v->varName;
  else if (auto *f = llvm::dyn_cast<FunctionDeclNode>(a))
    an = f->name;
  if (auto *v = llvm::dyn_cast<VarDeclNode>(b))
    bn = v->varName;
  else if (auto *f = llvm::dyn_cast<FunctionDeclNode>(b))
    bn = f->name;
  return an == bn;
}

const DeclNode *referencedDecl(const ASTNode *node) {
  if (!node)
    return nullptr;
  if (auto *var = llvm::dyn_cast_or_null<VariableNode>(node))
    return var->resolvedDecl;
  if (auto *call = llvm::dyn_cast_or_null<FunctionCallNode>(node))
    return call->resolvedFunc;
  if (auto *ma = llvm::dyn_cast_or_null<MemberAccessNode>(node)) {
    if (ma->resolvedMethod)
      return ma->resolvedMethod;
    if (ma->resolvedDecl)
      return ma->resolvedDecl;
    if (ma->enumMember)
      return ma->enumMember;

    /* Plain field access on the same record resolves through 'fieldIndex'
     * without attaching the declaration: fall back to a name lookup on the
     * object's record type so references and highlights still match. */
    const Type *baseTy = ma->object ? ma->object->exprType : nullptr;
    if (baseTy) {
      if (baseTy->isPointerType())
        baseTy = static_cast<const PointerType *>(baseTy)->getPointeeType();
      else if (baseTy->isReferenceType() ||
               baseTy->getKind() == TypeKind::RValueReference)
        baseTy = static_cast<const ReferenceType *>(baseTy)->getPointeeType();

      const Type *unqual = baseTy->getUnqualifiedType();
      if (unqual->getKind() == TypeKind::Class ||
          unqual->getKind() == TypeKind::Struct ||
          unqual->getKind() == TypeKind::Union) {
        auto *decl = static_cast<const RecordType *>(unqual)->getDeclaration();
        if (decl) {
          llvm::ArrayRef<VarDeclNode *> fields;
          if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(decl))
            fields = cDecl->fields;
          else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(decl))
            fields = sDecl->fields;
          else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(decl))
            fields = uDecl->fields;
          for (auto *f : fields)
            if (f->varName == ma->memberName)
              return f;
        }
      }
    }
  }
  if (auto *newNode = llvm::dyn_cast_or_null<NewExprNode>(node))
    return newNode->resolvedConstructor;
  return nullptr;
}

/* Visitor collecting every reference to a given declaration within a
 * document. */
class ReferenceCollector : public ASTVisitor<ReferenceCollector, void> {
public:
  const DeclNode *target;
  std::vector<const ASTNode *> hits;

  explicit ReferenceCollector(const DeclNode *t) : target(t) {}

  void visit(const ModuleNode *n) {
    for (auto *s : n->statements)
      dispatch(s);
  }
  void visit(const NamespaceDeclNode *n) {
    for (auto *s : n->statements)
      dispatch(s);
  }
  void visit(const UsingNode *) {}
  void visit(const FunctionDeclNode *n) {
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
    if (n->body)
      dispatch(n->body);
    for (const auto *clause : n->clauses) {
      if (clause->isCatchAll)
        continue;
      if (clause->catchType) {
        /* Catch variables are binds, not references. */
      }
      if (clause->body)
        dispatch(clause->body);
    }
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
    if (n->isExpressionBody && n->exprBody)
      dispatch(n->exprBody);
    else if (n->body)
      dispatch(n->body);
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
    for (auto *m : n->members)
      dispatch(m);
  }
  void visit(const EnumMemberNode *n) {
    if (n->initializer)
      dispatch(n->initializer);
  }
  void visit(const TypedefDeclNode *) {}
  void visit(const AnnotationDeclNode *n) {
    for (auto *f : n->fields)
      dispatch(f);
    if (n->constructor)
      dispatch(n->constructor);
  }
  void visit(const VariableNode *n) {
    if (sameDecl(referencedDecl(n), target))
      hits.push_back(n);
  }
  void visit(const FunctionCallNode *n) {
    if (sameDecl(referencedDecl(n), target))
      hits.push_back(n);
    if (n->target)
      dispatch(n->target);
    for (auto *a : n->args)
      dispatch(a);
  }
  void visit(const MemberAccessNode *n) {
    if (sameDecl(referencedDecl(n), target))
      hits.push_back(n);
    if (n->object)
      dispatch(n->object);
  }
  void visit(const NumberNode *) {}
  void visit(const BoolNode *) {}
  void visit(const CharNode *) {}
  void visit(const RuneNode *) {}
  void visit(const StringNode *) {}
  void visit(const NullNode *) {}
  void visit(const TypeLiteralNode *) {}
  void visit(const BreakNode *) {}
  void visit(const ContinueNode *) {}
};

} // namespace

void handleReferences(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = json::array();

  DocumentState doc;
  if (documents.get(uri, doc) && doc.ast) {
    SearchVisitor searcher(line, col, &doc.text);
    const ASTNode *node = searcher.find(doc.ast);
    const DeclNode *target = referencedDecl(node);
    if (!target)
      target = llvm::dyn_cast_or_null<DeclNode>(node);

    if (target) {
      /* The declaration itself counts as a reference. */
      std::string declUri = uri;
      std::string declText = doc.text;
      if (!target->declFilePath.empty()) {
        declUri = pathToUri(target->declFilePath);
        if (declUri != uri)
          declText = documents.textFor(declUri);
      }
      auto declLoc = getExactNameLocation(declText, target);
      res.push_back(
          {{"uri", declUri},
           {"range",
            {{"start", {{"line", declLoc.line}, {"character", declLoc.col}}},
             {"end", {{"line", declLoc.line},
                      {"character", declLoc.col + declLoc.length}}}}}});

      /* References across every analyzed document. */
      ReferenceCollector collector(target);
      for (const auto &[docUri, state] : documents.snapshot()) {
        if (!state.ast)
          continue;
        collector.hits.clear();
        collector.dispatch(state.ast);
        for (const auto *hit : collector.hits) {
          int hitLine = hit->line > 0 ? hit->line - 1 : 0;
          int hitCol = hit->column > 0 ? hit->column - 1 : 0;
          int hitLen = hit->length > 0 ? hit->length : 1;
          res.push_back(
              {{"uri", docUri},
               {"range",
                {{"start", {{"line", hitLine}, {"character", hitCol}}},
                 {"end", {{"line", hitLine}, {"character", hitCol + hitLen}}}}}});
        }
      }
    }
  }

  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
}

void handleDocumentHighlight(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = json::array();

  DocumentState doc;
  if (documents.get(uri, doc) && doc.ast) {
    SearchVisitor searcher(line, col, &doc.text);
    const ASTNode *node = searcher.find(doc.ast);
    const DeclNode *target = referencedDecl(node);
    if (!target)
      target = llvm::dyn_cast_or_null<DeclNode>(node);

    if (target) {
      /* Highlight only the document's own references; the declaration
       * itself uses the 'declaration' kind. */
      ReferenceCollector collector(target);
      collector.dispatch(doc.ast);

      auto addRange = [&](const ASTNode *n, int kind) {
        int hitLine = n->line > 0 ? n->line - 1 : 0;
        int hitCol = n->column > 0 ? n->column - 1 : 0;
        int hitLen = n->length > 0 ? n->length : 1;
        res.push_back({{"range",
                        {{"start", {{"line", hitLine}, {"character", hitCol}}},
                         {"end", {{"line", hitLine},
                                  {"character", hitCol + hitLen}}}}},
                       {"kind", kind}});
      };

      std::string declText = doc.text;
      if (!target->declFilePath.empty() &&
          pathToUri(target->declFilePath) != uri) {
        declText = documents.textFor(pathToUri(target->declFilePath));
      }
      auto declLoc = getExactNameLocation(declText, target);
      if (declLoc.line >= 0) {
        res.push_back(
            {{"range",
              {{"start", {{"line", declLoc.line}, {"character", declLoc.col}}},
               {"end", {{"line", declLoc.line},
                        {"character", declLoc.col + declLoc.length}}}}},
             {"kind", 1}}); /* declaration */
      }

      for (const auto *hit : collector.hits) {
        if (hit == node)
          continue;
        addRange(hit, 3); /* write vs read are not distinguished; use 'text' */
      }
    }
  }

  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
}

} // namespace utopia::lsp
