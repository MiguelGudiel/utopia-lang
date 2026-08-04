#include "utopia/Format/PieceFactory.hpp"
#include <algorithm>
#include <string>

namespace utopia {

static bool isExpressionStatement(NodeKind kind) {
  return kind == NodeKind::FunctionCall || kind == NodeKind::Assign ||
         kind == NodeKind::UnaryOp || kind == NodeKind::BinaryOp ||
         kind == NodeKind::TernaryOp || kind == NodeKind::Variable ||
         kind == NodeKind::Number || kind == NodeKind::String ||
         kind == NodeKind::Delete || kind == NodeKind::New;
}

static int getActualStartLine(const ASTNode *node) {
  if (!node)
    return 0;
  int startLine = node->line;

  if (auto *decl = llvm::dyn_cast<DeclNode>(node)) {
    if (!decl->annotations.empty()) {
      startLine = decl->annotations.front()->line;
    }
  }

  /* Discount lines occupied by multi-line docstrings to calculate the true
     starting point of the entity. */
  if (!node->docString.empty()) {
    int docLines = 1;
    for (char c : node->docString) {
      if (c == '\n')
        docLines++;
    }
    startLine -= docLines;
  }
  return startLine;
}

Piece *PieceFactory::dispatchStmt(const ASTNode *node) {
  if (!node)
    return nullptr;

  Piece *p = nullptr;
  if (auto *expr = llvm::dyn_cast<ExprNode>(node)) {
    p = dispatchExpr(expr);
  } else {
    p = dispatch(node);
  }

  if (!node->trailingComment.empty()) {
    std::string tComment = std::string(node->trailingComment);
    /* Inject a single space fallback if the user left zero whitespace. */
    if (tComment.empty() || (tComment[0] != ' ' && tComment[0] != '\t')) {
      tComment = " " + tComment;
    }
    p = create<ConcatPiece>(
        std::vector<Piece *>{p, create<StringPiece>(tComment)});
  }

  if (!node->docString.empty()) {
    p = create<SequencePiece>(std::vector<Piece *>{
        create<StringPiece>(formatCommentString(std::string(node->docString))),
        p});
  }
  return p;
}

Piece *PieceFactory::extractChain(const ExprNode *node) {
  std::vector<const ExprNode *> links;
  const ExprNode *current = node;

  while (current && !current->hasParens) {
    if (auto *call = llvm::dyn_cast<FunctionCallNode>(current)) {
      if (auto *ma = llvm::dyn_cast<MemberAccessNode>(call->target)) {
        links.push_back(current);
        current = ma->object;
      } else {
        break;
      }
    } else if (auto *ma = llvm::dyn_cast<MemberAccessNode>(current)) {
      links.push_back(current);
      current = ma->object;
    } else {
      break;
    }
  }

  if (links.size() <= 1)
    return nullptr;

  std::reverse(links.begin(), links.end());
  Piece *targetPiece = dispatchStmt(current);

  std::vector<ChainLink> chainLinks;
  bool hasProperties = false;
  bool hasBlockFormat = false;

  for (size_t i = 0; i < links.size(); ++i) {
    const ExprNode *linkNode = links[i];
    ChainLinkKind kind;
    Piece *linkPiece = nullptr;

    if (auto *ma = llvm::dyn_cast<MemberAccessNode>(linkNode)) {
      kind = ChainLinkKind::Property;
      hasProperties = true;

      std::string memberStr = "." + std::string(ma->memberName);
      if (!ma->templateArgs.empty()) {
        memberStr += "<";
        for (size_t j = 0; j < ma->templateArgs.size(); ++j) {
          memberStr += ma->templateArgs[j]->toString();
          if (j < ma->templateArgs.size() - 1)
            memberStr += ", ";
        }
        memberStr += ">";
      }
      linkPiece = create<TextPiece>(memberStr);
    } else if (auto *call = llvm::dyn_cast<FunctionCallNode>(linkNode)) {
      auto *ma = llvm::cast<MemberAccessNode>(call->target);

      auto actualArgs = call->hasRawArgs ? call->rawArgs : call->args;
      bool isBlock = false;
      if (!actualArgs.empty()) {
        if (llvm::isa<BlockNode>(actualArgs.back()) ||
            llvm::isa<ArrayLiteralNode>(actualArgs.back())) {
          isBlock = true;
        }
      }

      if (isBlock && i == links.size() - 1) {
        kind = ChainLinkKind::BlockFormatCall;
        hasBlockFormat = true;
      } else if (actualArgs.empty()) {
        kind = ChainLinkKind::UnsplittableCall;
      } else {
        kind = ChainLinkKind::SplittableCall;
      }

      std::string memberStr = "." + std::string(ma->memberName);
      if (!ma->templateArgs.empty()) {
        memberStr += "<";
        for (size_t j = 0; j < ma->templateArgs.size(); ++j) {
          memberStr += ma->templateArgs[j]->toString();
          if (j < ma->templateArgs.size() - 1)
            memberStr += ", ";
        }
        memberStr += ">";
      }

      std::vector<Piece *> args;
      auto actualNames = call->hasRawArgs ? call->rawArgNames : call->argNames;
      for (size_t j = 0; j < actualArgs.size(); ++j) {
        Piece *argPiece = dispatchExpr(actualArgs[j]);
        if (!actualNames.empty() && !actualNames[j].empty()) {
          argPiece = create<ConcatPiece>(std::vector<Piece *>{
              create<TextPiece>(std::string(actualNames[j]) + ": "), argPiece});
        }
        args.push_back(argPiece);
      }

      Piece *listPiece = create<ListPiece>(
          create<TextPiece>("("), std::move(args), create<TextPiece>(")"));
      linkPiece = create<ConcatPiece>(
          std::vector<Piece *>{create<TextPiece>(memberStr), listPiece});
    }

    chainLinks.push_back({linkPiece, kind});
  }

  return create<ChainPiece>(targetPiece, std::move(chainLinks), hasBlockFormat,
                            hasProperties);
}

Piece *PieceFactory::dispatchExpr(const ExprNode *node) {
  if (!node)
    return nullptr;

  if (!node->hasParens && (node->kind == NodeKind::FunctionCall ||
                           node->kind == NodeKind::MemberAccess)) {
    if (Piece *chain = extractChain(node)) {
      return chain;
    }
  }

  Piece *p = dispatch(node);
  if (node->hasParens) {
    return create<ConcatPiece>(std::vector<Piece *>{create<TextPiece>("("), p,
                                                    create<TextPiece>(")")});
  }
  return p;
}

Piece *PieceFactory::visit(const AssignNode *node) {
  Piece *left = dispatchExpr(node->target);
  Piece *right = dispatchExpr(node->value);
  return create<AssignPiece>(left, std::string(node->op), right);
}

Piece *PieceFactory::visit(const VariableNode *node) {
  std::string varStr = std::string(node->name);
  if (!node->templateArgs.empty()) {
    varStr += "<";
    for (size_t i = 0; i < node->templateArgs.size(); ++i) {
      varStr += node->templateArgs[i]->toString();
      if (i < node->templateArgs.size() - 1)
        varStr += ", ";
    }
    varStr += ">";
  }
  return create<TextPiece>(varStr);
}

Piece *PieceFactory::visit(const NumberNode *node) {
  return create<TextPiece>(std::string(node->raw));
}

Piece *PieceFactory::visit(const StringNode *node) {
  std::string escaped = "\"";
  for (char c : node->value) {
    switch (c) {
    case '\n':
      escaped += "\\n";
      break;
    case '\t':
      escaped += "\\t";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\0':
      escaped += "\\0";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    default:
      escaped += c;
      break;
    }
  }
  escaped += "\"";
  return create<StringPiece>(escaped);
}

Piece *PieceFactory::visit(const BoolNode *node) {
  return create<TextPiece>(node->value ? "true" : "false");
}

Piece *PieceFactory::visit(const CharNode *node) {
  std::string escaped;
  switch (node->value) {
  case '\n':
    escaped = "\\n";
    break;
  case '\t':
    escaped = "\\t";
    break;
  case '\r':
    escaped = "\\r";
    break;
  case '\0':
    escaped = "\\0";
    break;
  case '\\':
    escaped = "\\\\";
    break;
  case '\'':
    escaped = "\\'";
    break;
  default:
    escaped = std::string(1, static_cast<char>(node->value));
    break;
  }
  return create<TextPiece>("'" + escaped + "'");
}

Piece *PieceFactory::visit(const RuneNode *node) {
  return create<TextPiece>("U'" + std::to_string(node->value) + "'");
}

Piece *PieceFactory::visit(const NullNode *node) {
  return create<TextPiece>("null");
}

Piece *PieceFactory::visit(const TypeLiteralNode *node) {
  return create<TextPiece>(node->representedType->toString());
}

Piece *PieceFactory::visit(const UnaryOpNode *node) {
  Piece *expr = dispatchExpr(node->expr);
  if (node->isPostfix) {
    return create<ConcatPiece>(
        std::vector<Piece *>{expr, create<TextPiece>(std::string(node->op))});
  }
  return create<ConcatPiece>(
      std::vector<Piece *>{create<TextPiece>(std::string(node->op)), expr});
}

Piece *PieceFactory::visit(const BinaryOpNode *node) {
  Piece *left = dispatchExpr(node->left);
  Piece *right = dispatchExpr(node->right);
  return create<InfixPiece>(left, std::string(node->op), right);
}

Piece *PieceFactory::visit(const TernaryOpNode *node) {
  return create<ConcatPiece>(std::vector<Piece *>{
      dispatchExpr(node->condition), create<TextPiece>(" ? "),
      dispatchExpr(node->trueExpr), create<TextPiece>(" : "),
      dispatchExpr(node->falseExpr)});
}

Piece *PieceFactory::visit(const BlockNode *node) {
  std::vector<Piece *> stmts;
  for (size_t i = 0; i < node->statements.size(); ++i) {
    auto *stmt = node->statements[i];
    Piece *p = dispatchStmt(stmt);
    if (isExpressionStatement(stmt->kind)) {
      p = create<ConcatPiece>(std::vector<Piece *>{p, create<TextPiece>(";")});
    }
    stmts.push_back(p);

    if (i < node->statements.size() - 1) {
      auto *nextStmt = node->statements[i + 1];
      int diff = getActualStartLine(nextStmt) - stmt->endLine;
      if (diff > 1) {
        stmts.push_back(create<NewlinesPiece>(diff));
      }
    }
  }
  return create<BlockPiece>(std::move(stmts));
}

Piece *PieceFactory::visit(const IfNode *node) {
  Piece *cond = dispatchExpr(node->condition);
  Piece *thenBlock = dispatchStmt(node->thenBlock);
  Piece *elseBlock = node->elseBlock ? dispatchStmt(node->elseBlock) : nullptr;
  return create<ControlFlowPiece>("if", cond, thenBlock, elseBlock);
}

Piece *PieceFactory::visit(const WhileNode *node) {
  Piece *cond = dispatchExpr(node->condition);
  Piece *body = dispatchStmt(node->body);
  return create<ControlFlowPiece>("while", cond, body, nullptr);
}

Piece *PieceFactory::visit(const ForNode *node) {
  std::vector<Piece *> forCond;

  if (node->initStatement) {
    forCond.push_back(dispatchStmt(node->initStatement));

    if (isExpressionStatement(node->initStatement->kind)) {
      forCond.push_back(create<TextPiece>("; "));
    } else {
      forCond.push_back(create<TextPiece>(" "));
    }
  } else {
    forCond.push_back(create<TextPiece>("; "));
  }

  if (node->condition)
    forCond.push_back(dispatchExpr(node->condition));
  forCond.push_back(create<TextPiece>("; "));

  if (node->increment)
    forCond.push_back(dispatchExpr(node->increment));

  Piece *condPiece = create<ConcatPiece>(std::move(forCond));
  return create<ControlFlowPiece>("for", condPiece, dispatchStmt(node->body),
                                  nullptr);
}

Piece *PieceFactory::visit(const FunctionCallNode *node) {
  Piece *target = dispatchExpr(node->target);
  std::vector<Piece *> args;

  auto actualArgs = node->hasRawArgs ? node->rawArgs : node->args;
  auto actualNames = node->hasRawArgs ? node->rawArgNames : node->argNames;

  for (size_t i = 0; i < actualArgs.size(); ++i) {
    Piece *argPiece = dispatchExpr(actualArgs[i]);
    if (!actualNames.empty() && !actualNames[i].empty()) {
      argPiece = create<ConcatPiece>(std::vector<Piece *>{
          create<TextPiece>(std::string(actualNames[i]) + ": "), argPiece});
    }
    args.push_back(argPiece);
  }
  Piece *listPiece = create<ListPiece>(create<TextPiece>("("), std::move(args),
                                       create<TextPiece>(")"));
  return create<CallPiece>(target, listPiece);
}

Piece *PieceFactory::visit(const ArrayLiteralNode *node) {
  std::vector<Piece *> elements;
  for (const auto *elem : node->elements) {
    elements.push_back(dispatchExpr(elem));
  }
  return create<ListPiece>(create<TextPiece>("["), std::move(elements),
                           create<TextPiece>("]"));
}

Piece *PieceFactory::visit(const ArraySubscriptNode *node) {
  return create<ConcatPiece>(
      std::vector<Piece *>{dispatchExpr(node->base), create<TextPiece>("["),
                           dispatchExpr(node->index), create<TextPiece>("]")});
}

Piece *PieceFactory::visit(const MemberAccessNode *node) {
  std::string memberStr = "." + std::string(node->memberName);
  if (!node->templateArgs.empty()) {
    memberStr += "<";
    for (size_t i = 0; i < node->templateArgs.size(); ++i) {
      memberStr += node->templateArgs[i]->toString();
      if (i < node->templateArgs.size() - 1)
        memberStr += ", ";
    }
    memberStr += ">";
  }
  return create<ConcatPiece>(std::vector<Piece *>{
      dispatchExpr(node->object), create<TextPiece>(memberStr)});
}

Piece *PieceFactory::visit(const VarDeclNode *node) {
  std::vector<Piece *> parts;
  for (auto *ann : node->annotations) {
    parts.push_back(dispatch(ann));
  }

  std::string pfx = "";
  if (node->hasPublicMod)
    pfx += "public ";
  if (node->hasPrivateMod)
    pfx += "private ";
  if (node->isStatic)
    pfx += "static ";
  if (node->type) {
    std::string typeStr = node->rawTypeStr.empty()
                              ? node->type->toString()
                              : std::string(node->rawTypeStr);
    pfx += typeStr + " ";
  }
  pfx += std::string(node->varName);

  Piece *decl = create<TextPiece>(pfx);
  if (node->initializer) {
    decl = create<AssignPiece>(decl, "=", dispatchExpr(node->initializer));
  }

  Piece *mainDecl =
      create<ConcatPiece>(std::vector<Piece *>{decl, create<TextPiece>(";")});

  if (parts.empty()) {
    return mainDecl;
  }

  parts.push_back(mainDecl);
  return create<SequencePiece>(std::move(parts));
}

Piece *PieceFactory::visit(const ParamDeclNode *node) {
  std::string pfx = "";
  if (node->isRequired)
    pfx += "required ";
  if (node->type) {
    std::string typeStr = node->rawTypeStr.empty()
                              ? node->type->toString()
                              : std::string(node->rawTypeStr);
    pfx += typeStr + " ";
  }
  pfx += std::string(node->name);

  Piece *p = create<TextPiece>(pfx);
  if (node->defaultValue) {
    p = create<AssignPiece>(p, "=", dispatchExpr(node->defaultValue));
  }
  return p;
}

Piece *PieceFactory::visit(const FunctionDeclNode *node) {
  std::vector<Piece *> sigParts;

  for (auto *ann : node->annotations) {
    sigParts.push_back(dispatch(ann));
  }

  std::string pfx = "";
  if (node->hasPublicMod)
    pfx += "public ";
  if (node->hasPrivateMod)
    pfx += "private ";
  if (node->isStatic)
    pfx += "static ";
  if (node->isConst)
    pfx += "const ";

  /* Prevent injecting 'void' into constructors/destructors by relying
     strictly on the raw parsed string rather than the inferred AST type. */
  if (!node->rawReturnTypeStr.empty()) {
    pfx += std::string(node->rawReturnTypeStr) + " ";
  }

  if (node->name == "~" && node->parentRecord) {
    pfx += "~" + std::string(node->parentRecord->getName());
  } else {
    pfx += std::string(node->name);
  }

  if (node->isTemplate) {
    pfx += "<";
    for (size_t i = 0; i < node->templateParams.size(); ++i) {
      pfx += std::string(node->templateParams[i]);
      if (i < node->templateParams.size() - 1) {
        pfx += ", ";
      }
    }
    pfx += ">";
  }

  std::vector<Piece *> signature;
  signature.push_back(create<TextPiece>(pfx));

  std::vector<Piece *> params;
  bool insideNamed = false;
  for (size_t i = 0; i < node->params.size(); ++i) {
    auto *p = node->params[i];
    Piece *pPiece = dispatchStmt(p);
    if (p->isNamed && !insideNamed) {
      pPiece = create<ConcatPiece>(
          std::vector<Piece *>{create<TextPiece>("{"), pPiece});
      insideNamed = true;
    }
    if (insideNamed && i == node->params.size() - 1) {
      pPiece = create<ConcatPiece>(
          std::vector<Piece *>{pPiece, create<TextPiece>("}")});
    }
    params.push_back(pPiece);
  }

  if (node->isVariadic) {
    params.push_back(create<TextPiece>("..."));
  }

  signature.push_back(create<ListPiece>(
      create<TextPiece>("("), std::move(params), create<TextPiece>(")")));

  Piece *mainSig = create<ConcatPiece>(std::move(signature));

  if (node->body) {
    if (node->body->isExpressionBody && !node->body->statements.empty()) {
      Piece *bodyPiece = nullptr;
      const ASTNode *innerStmt = node->body->statements[0];

      if (auto *retNode = llvm::dyn_cast<ReturnNode>(innerStmt)) {
        bodyPiece = dispatchExpr(retNode->value);
      } else if (auto *exprStmt = llvm::dyn_cast<ExprNode>(innerStmt)) {
        bodyPiece = dispatchExpr(exprStmt);
      } else {
        bodyPiece = dispatchStmt(innerStmt);
      }

      if (!bodyPiece) {
        bodyPiece = dispatchStmt(innerStmt);
      }

      mainSig = create<ConcatPiece>(
          std::vector<Piece *>{mainSig, create<TextPiece>(" => "), bodyPiece,
                               create<TextPiece>(";")});
    } else {
      mainSig = create<ConcatPiece>(std::vector<Piece *>{
          mainSig, create<TextPiece>(" "), dispatchStmt(node->body)});
    }
  } else {
    mainSig = create<ConcatPiece>(
        std::vector<Piece *>{mainSig, create<TextPiece>(";")});
  }

  if (sigParts.empty()) {
    return mainSig;
  }

  sigParts.push_back(mainSig);
  return create<SequencePiece>(std::move(sigParts));
}

Piece *PieceFactory::visit(const ReturnNode *node) {
  if (node->value) {
    return create<ConcatPiece>(std::vector<Piece *>{
        create<TextPiece>("return "), dispatchExpr(node->value),
        create<TextPiece>(";")});
  }
  return create<TextPiece>("return;");
}

Piece *PieceFactory::visit(const BreakNode *node) {
  return create<TextPiece>("break;");
}
Piece *PieceFactory::visit(const ContinueNode *node) {
  return create<TextPiece>("continue;");
}

Piece *PieceFactory::visit(const NewExprNode *node) {
  std::vector<Piece *> parts;
  std::string typeStr = node->rawAllocatedTypeStr.empty()
                            ? node->allocatedType->toString()
                            : std::string(node->rawAllocatedTypeStr);
  parts.push_back(create<TextPiece>("new " + typeStr));
  if (node->arraySize) {
    parts.push_back(create<TextPiece>("["));
    parts.push_back(dispatchExpr(node->arraySize));
    parts.push_back(create<TextPiece>("]"));
  } else if (node->hasParens) {
    std::vector<Piece *> args;
    auto actualArgs = node->hasRawArgs ? node->rawArgs : node->args;
    auto actualNames = node->hasRawArgs ? node->rawArgNames : node->argNames;

    for (size_t i = 0; i < actualArgs.size(); ++i) {
      Piece *argPiece = dispatchExpr(actualArgs[i]);
      if (!actualNames.empty() && !actualNames[i].empty()) {
        argPiece = create<ConcatPiece>(std::vector<Piece *>{
            create<TextPiece>(std::string(actualNames[i]) + ": "), argPiece});
      }
      args.push_back(argPiece);
    }
    parts.push_back(create<ListPiece>(create<TextPiece>("("), std::move(args),
                                      create<TextPiece>(")")));
  }
  return create<ConcatPiece>(std::move(parts));
}

Piece *PieceFactory::visit(const DeleteExprNode *node) {
  std::vector<Piece *> parts;
  parts.push_back(create<TextPiece>("delete"));
  if (node->isArray)
    parts.push_back(create<TextPiece>("[]"));
  parts.push_back(create<TextPiece>(" "));
  parts.push_back(dispatchExpr(node->ptr));
  return create<ConcatPiece>(std::move(parts));
}

Piece *PieceFactory::visit(const CastNode *node) {
  std::string typeStr = node->rawTargetTypeStr.empty()
                            ? node->targetType->toString()
                            : std::string(node->rawTargetTypeStr);
  return create<InfixPiece>(dispatchExpr(node->expr), "as",
                            create<TextPiece>(typeStr));
}

Piece *PieceFactory::visit(const ImplicitCastNode *node) {
  return dispatchExpr(node->expr);
}

Piece *PieceFactory::visit(const ModuleNode *node) {
  std::vector<Piece *> stmts;
  for (auto imp : node->rawImports) {
    stmts.push_back(create<TextPiece>("import \"" + std::string(imp) + "\";"));
  }
  for (auto exp : node->rawExports) {
    stmts.push_back(create<TextPiece>("export \"" + std::string(exp) + "\";"));
  }
  if (!node->rawImports.empty() || !node->rawExports.empty()) {
    stmts.push_back(create<BlankLinePiece>());
  }
  for (size_t i = 0; i < node->statements.size(); ++i) {
    auto *stmt = node->statements[i];
    Piece *p = dispatchStmt(stmt);
    if (isExpressionStatement(stmt->kind)) {
      p = create<ConcatPiece>(std::vector<Piece *>{p, create<TextPiece>(";")});
    }
    stmts.push_back(p);

    if (i < node->statements.size() - 1) {
      auto *nextStmt = node->statements[i + 1];
      int diff = getActualStartLine(nextStmt) - stmt->endLine;
      if (diff > 1) {
        stmts.push_back(create<NewlinesPiece>(diff));
      }
    }
  }
  return create<SequencePiece>(std::move(stmts));
}

Piece *PieceFactory::visit(const EnumDeclNode *node) {
  std::vector<Piece *> parts;
  for (auto *ann : node->annotations) {
    parts.push_back(dispatch(ann));
  }

  std::string pfx = "enum " + std::string(node->name);
  std::vector<Piece *> members;
  for (auto *m : node->members)
    members.push_back(dispatchStmt(m));

  Piece *body = create<ListPiece>(create<TextPiece>("{ "), std::move(members),
                                  create<TextPiece>(" }"));
  Piece *mainEnum = create<ConcatPiece>(std::vector<Piece *>{
      create<TextPiece>(pfx), create<TextPiece>(" "), body});

  if (parts.empty())
    return mainEnum;
  parts.push_back(mainEnum);
  return create<SequencePiece>(std::move(parts));
}

Piece *PieceFactory::visit(const EnumMemberNode *node) {
  Piece *p = create<TextPiece>(std::string(node->name));
  if (node->initializer) {
    p = create<AssignPiece>(p, "=", dispatchExpr(node->initializer));
  }
  return p;
}

template <typename T>
Piece *createRecord(PieceFactory *factory, const T *node, const char *kw) {
  std::vector<Piece *> parts;
  for (auto *ann : node->annotations) {
    parts.push_back(factory->dispatch(ann));
  }

  std::string pfx = std::string(kw) + " " + std::string(node->name);

  if (node->isTemplate) {
    pfx += "<";
    for (size_t i = 0; i < node->templateParams.size(); ++i) {
      pfx += std::string(node->templateParams[i]);
      if (i < node->templateParams.size() - 1) {
        pfx += ", ";
      }
    }
    pfx += ">";
  }

  Piece *mainRecord;

  if (node->isOpaque) {
    mainRecord = factory->create<TextPiece>(pfx + ";");
  } else {
    std::vector<Piece *> stmts;

    std::vector<const ASTNode *> allMembers;
    for (auto *f : node->fields) {
      allMembers.push_back(f);
    }
    for (auto *c : node->constructors) {
      if (!c->isImplicit) {
        allMembers.push_back(c);
      }
    }
    if (node->destructor && !node->destructor->isImplicit) {
      allMembers.push_back(node->destructor);
    }
    for (auto *m : node->methods) {
      allMembers.push_back(m);
    }

    std::sort(
        allMembers.begin(), allMembers.end(),
        [](const ASTNode *a, const ASTNode *b) { return a->line < b->line; });

    for (size_t i = 0; i < allMembers.size(); ++i) {
      stmts.push_back(factory->dispatchStmt(allMembers[i]));
      if (i < allMembers.size() - 1) {
        auto *nextStmt = allMembers[i + 1];
        int diff = getActualStartLine(nextStmt) - allMembers[i]->endLine;
        if (diff > 1) {
          stmts.push_back(factory->create<NewlinesPiece>(diff));
        }
      }
    }

    Piece *body = factory->create<BlockPiece>(std::move(stmts));
    mainRecord = factory->create<ConcatPiece>(
        std::vector<Piece *>{factory->create<TextPiece>(pfx),
                             factory->create<TextPiece>(" "), body});
  }

  if (parts.empty()) {
    return mainRecord;
  }

  parts.push_back(mainRecord);
  return factory->create<SequencePiece>(std::move(parts));
}

Piece *PieceFactory::visit(const StructDeclNode *node) {
  return createRecord(this, node, "struct");
}
Piece *PieceFactory::visit(const ClassDeclNode *node) {
  return createRecord(this, node, "class");
}
Piece *PieceFactory::visit(const UnionDeclNode *node) {
  return createRecord(this, node, "union");
}

Piece *PieceFactory::visit(const TypedefDeclNode *node) {
  std::vector<Piece *> parts;
  for (auto *ann : node->annotations) {
    parts.push_back(dispatch(ann));
  }

  std::string tName;
  if (!node->targetEntityName.empty()) {
    tName = std::string(node->targetEntityName);
  } else {
    tName = node->rawTargetTypeStr.empty()
                ? node->targetType->toString()
                : std::string(node->rawTargetTypeStr);
  }

  Piece *mainTd = create<TextPiece>("typedef " + std::string(node->aliasName) +
                                    " = " + tName + ";");

  if (parts.empty())
    return mainTd;
  parts.push_back(mainTd);
  return create<SequencePiece>(std::move(parts));
}

Piece *PieceFactory::visit(const AnnotationDeclNode *node) {
  std::vector<Piece *> parts;
  for (auto *ann : node->annotations) {
    parts.push_back(dispatch(ann));
  }

  std::string pfx = "annotation " + std::string(node->name);
  std::vector<Piece *> stmts;

  std::vector<const ASTNode *> allMembers;
  for (auto *f : node->fields) {
    allMembers.push_back(f);
  }
  if (node->constructor && !node->constructor->isImplicit) {
    allMembers.push_back(node->constructor);
  }

  std::sort(
      allMembers.begin(), allMembers.end(),
      [](const ASTNode *a, const ASTNode *b) { return a->line < b->line; });

  for (size_t i = 0; i < allMembers.size(); ++i) {
    stmts.push_back(dispatchStmt(allMembers[i]));
    if (i < allMembers.size() - 1) {
      auto *nextStmt = allMembers[i + 1];
      int diff = getActualStartLine(nextStmt) - allMembers[i]->endLine;
      if (diff > 1) {
        stmts.push_back(create<NewlinesPiece>(diff));
      }
    }
  }

  Piece *body = create<BlockPiece>(std::move(stmts));
  Piece *mainAnn = create<ConcatPiece>(std::vector<Piece *>{
      create<TextPiece>(pfx), create<TextPiece>(" "), body});

  if (parts.empty())
    return mainAnn;
  parts.push_back(mainAnn);
  return create<SequencePiece>(std::move(parts));
}

Piece *PieceFactory::visit(const AnnotationNode *node) {
  std::vector<Piece *> parts;
  parts.push_back(create<TextPiece>("@" + std::string(node->name)));
  if (!node->args.empty()) {
    std::vector<Piece *> args;
    for (auto *a : node->args)
      args.push_back(dispatchExpr(a));
    parts.push_back(create<ListPiece>(create<TextPiece>("("), std::move(args),
                                      create<TextPiece>(")")));
  }
  return create<ConcatPiece>(std::move(parts));
}

Piece *PieceFactory::visit(const SwitchNode *node) {
  Piece *cond = dispatchExpr(node->condition);
  std::vector<Piece *> cases;

  for (size_t i = 0; i < node->cases.size(); ++i) {
    auto *c = node->cases[i];
    cases.push_back(dispatchStmt(c));

    if (i < node->cases.size() - 1) {
      auto *nextCase = node->cases[i + 1];
      int diff = getActualStartLine(nextCase) - c->endLine;
      if (diff > 1) {
        cases.push_back(create<NewlinesPiece>(diff));
      }
    }
  }

  Piece *body = create<BlockPiece>(std::move(cases));
  return create<ControlFlowPiece>("switch", cond, body, nullptr);
}

Piece *PieceFactory::visit(const CaseNode *node) {
  Piece *labelPiece = nullptr;

  if (node->value) {
    labelPiece = create<ConcatPiece>(std::vector<Piece *>{
        create<TextPiece>("case "), dispatchExpr(node->value),
        create<TextPiece>(":")});
  } else {
    labelPiece = create<TextPiece>("default:");
  }

  std::vector<Piece *> stmts;
  for (size_t i = 0; i < node->statements.size(); ++i) {
    auto *s = node->statements[i];
    Piece *p = dispatchStmt(s);

    if (isExpressionStatement(s->kind)) {
      p = create<ConcatPiece>(std::vector<Piece *>{p, create<TextPiece>(";")});
    }
    stmts.push_back(p);

    if (i < node->statements.size() - 1) {
      auto *nextStmt = node->statements[i + 1];
      int diff = getActualStartLine(nextStmt) - s->endLine;
      if (diff > 1) {
        stmts.push_back(create<NewlinesPiece>(diff));
      }
    }
  }

  return create<CasePiece>(labelPiece, std::move(stmts));
}

} // namespace utopia