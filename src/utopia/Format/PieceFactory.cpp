#include "utopia/Format/PieceFactory.hpp"
#include "utopia/Format/Solver.hpp"
#include <algorithm>
#include <string>

namespace utopia {

/* Helpers */

static bool isExpressionStatement(NodeKind kind) {
  return kind == NodeKind::FunctionCall || kind == NodeKind::Assign ||
         kind == NodeKind::UnaryOp || kind == NodeKind::BinaryOp ||
         kind == NodeKind::TernaryOp || kind == NodeKind::Variable ||
         kind == NodeKind::Number || kind == NodeKind::String ||
         kind == NodeKind::Delete || kind == NodeKind::New ||
         kind == NodeKind::DestructorCall || kind == NodeKind::Cast ||
         kind == NodeKind::Is || kind == NodeKind::ArrayLiteral ||
         kind == NodeKind::MapLiteral || kind == NodeKind::ArraySubscript ||
         kind == NodeKind::MemberAccess || kind == NodeKind::Lambda ||
         kind == NodeKind::Null || kind == NodeKind::Await;
}

static int getActualStartLine(const ASTNode *node) {
  if (!node)
    return 0;
  int startLine = node->line;

  if (auto *decl = llvm::dyn_cast_or_null<DeclNode>(node)) {
    if (!decl->annotations.empty()) {
      startLine = decl->annotations.front()->line;
    }
  }

  /* Discount lines occupied by multi-line docstrings to calculate the true
     starting point of the entity. */
  if (!node->docString.empty()) {
    int newlines = 0;
    for (char c : node->docString) {
      if (c == '\n')
        newlines++;
    }
    startLine -= newlines;
  }
  return startLine;
}

static int getActualEndLine(const ASTNode *node) {
  if (!node)
    return 0;
  int endLine = node->endLine;

  if (!node->trailingComment.empty()) {
    int newlines = 0;
    for (char c : node->trailingComment) {
      if (c == '\n')
        newlines++;
    }
    endLine += newlines;
  }
  return endLine;
}

/* Whether a member or top-level declaration with a non-empty body should have
 * a blank line forced after it. Mirrors dart_style's `hasNonEmptyBody`. */
static bool hasNonEmptyBody(const ASTNode *node) {
  switch (node->kind) {
  case NodeKind::FunctionDecl: {
    auto *func = static_cast<const FunctionDeclNode *>(node);
    return func->body != nullptr && !func->body->isExpressionBody &&
           !func->body->statements.empty();
  }
  case NodeKind::ClassDecl:
  case NodeKind::StructDecl:
  case NodeKind::UnionDecl:
  case NodeKind::EnumDecl:
  case NodeKind::AnnotationDecl:
  case NodeKind::NamespaceDecl:
    return true;
  default:
    return false;
  }
}

/* Precedence of binary operators, matching the parser. Higher binds tighter.
 */
static int opPrecedence(std::string_view op) {
  if (op == "||")
    return 1;
  if (op == "&&")
    return 2;
  if (op == "|")
    return 3;
  if (op == "^")
    return 4;
  if (op == "&")
    return 5;
  if (op == "==" || op == "!=")
    return 6;
  if (op == "<" || op == "<=" || op == ">" || op == ">=")
    return 7;
  if (op == "<<" || op == ">>")
    return 8;
  if (op == "+" || op == "-")
    return 9;
  if (op == "*" || op == "/" || op == "%")
    return 10;
  return -1;
}

/* Tracks the contents of a nested tree of argument lists and collection
 * literals.
 *
 * In general, the formatter tries to pack as much as it can on a single line
 * until it hits the page width. However, with deeply nested call trees the
 * expression nesting can get deep even in a short piece of code. It can be
 * much easier to track the nesting structure and identify siblings in the
 * expression tree if it's forced to split more eagerly.
 *
 * A nested operation counts if it occurs anywhere transitively inside the
 * elements or argument list of the operation, regardless of any other AST
 * nodes that may intercede. */
struct ExpressionContents {
  enum class Type { Collection, NamedCollection, CallWithNamedArgument, OtherCall };

  struct Contents {
    Type type;
    int collections = 0;
    int namedArguments = 0;
    int nestedNamedArguments = 0;

    int totalNamedArguments() const {
      return namedArguments + nestedNamedArguments;
    }
  };

  std::vector<Contents> stack;

  ExpressionContents() {
    stack.push_back({Type::OtherCall, 0, 0, 0});
  }

  /* Begins tracking an argument list. */
  void beginCall(const llvm::ArrayRef<ExprNode *> &arguments,
                 const llvm::ArrayRef<std::string_view> &names) {
    Type type = Type::OtherCall;

    /* Count the non-trivial named arguments in this call. */
    int namedArguments = 0;
    for (size_t i = 0; i < arguments.size(); i++) {
      if (!names.empty() && !names[i].empty()) {
        type = Type::CallWithNamedArgument;
        if (!isTrivial(arguments[i]))
          namedArguments++;
      }
    }

    stack.push_back({type, 0, namedArguments, 0});
  }

  /* Ends the most recently begun call and returns `true` if its argument list
   * should eagerly split. */
  bool endCall() {
    Contents contents = end();

    /* If there are "too many" named arguments in this call and the calls it
     * contains, then split it. The rule is that the entire call tree must
     * contain at least three named arguments, at least one must be in the
     * outermost call being split, and at least one must *not* be in the
     * outermost call. */
    return contents.totalNamedArguments() > 2 && contents.namedArguments > 0 &&
           contents.nestedNamedArguments > 0;
  }

  /* Begin tracking a collection literal and its contents. */
  void beginCollection(bool isNamed) {
    stack.back().collections++;
    stack.push_back({isNamed ? Type::NamedCollection : Type::Collection, 0, 0,
                     0});
  }

  /* Ends the most recently begun collection literal and returns whether it
   * should eagerly split. */
  bool endCollection(size_t elementCount) {
    Contents contents = end();

    /* Split any collection that contains another non-empty collection. */
    if (contents.collections > 0)
      return true;

    /* If the collection is itself a named argument in a surrounding call that
     * may be forced to eagerly split, then split the collection too. */
    return elementCount > 1 && contents.type == Type::NamedCollection &&
           contents.totalNamedArguments() > 0;
  }

  Contents end() {
    Contents contents = stack.back();
    stack.pop_back();

    /* Transitively include this operation's contents in the surrounding one.
 */
    Contents &parent = stack.back();
    parent.collections += contents.collections;
    parent.nestedNamedArguments +=
        contents.namedArguments + contents.nestedNamedArguments;

    return contents;
  }

  /* Whether [expression] is "trivial". */
  static bool isTrivial(const ExprNode *expression) {
    switch (expression->kind) {
    case NodeKind::Null:
    case NodeKind::Boolean:
    case NodeKind::Number:
      return true;
    case NodeKind::UnaryOp: {
      auto *unary = static_cast<const UnaryOpNode *>(expression);
      return unary->op == "-" && !unary->isPostfix &&
             isTrivial(unary->expr);
    }
    default:
      return false;
    }
  }
};

PieceFactory::PieceFactory(int pageWidth)
    : pageWidth(pageWidth), contents(std::make_unique<ExpressionContents>()) {}

PieceFactory::~PieceFactory() {}

/* Incrementally builds a SequencePiece, handling blank lines that may appear
 * before, between, or after its contents. */
class SequenceBuilder {
public:
  PieceFactory *factory;
  const Piece *leftBracket_ = nullptr;
  std::vector<const Piece *> elements;
  const Piece *rightBracket_ = nullptr;
  bool allowBlank = false;
  const ASTNode *lastNode = nullptr;

  explicit SequenceBuilder(PieceFactory *f) : factory(f) {}

  void leftBracket(const Piece *p) { leftBracket_ = p; }

  void rightBracket(const Piece *p) { rightBracket_ = p; }

  void add(const Piece *piece, Indent indent = Indent::None,
           bool allowBlankAfter = true) {
    elements.push_back(factory->create<SequenceElementPiece>(indent, piece));
    allowBlank = allowBlankAfter;
  }

  /* Appends a blank line before the next piece in the sequence. */
  void addBlank() {
    if (elements.empty())
      return;
    if (!allowBlank)
      return;
    auto *last = const_cast<SequenceElementPiece *>(
        static_cast<const SequenceElementPiece *>(elements.back()));
    last->blankAfter = true;
  }

  /* Adds a statement, preserving blank lines from the source and forcing them
   * after members with non-empty bodies. */
  void addNode(const ASTNode *node, Indent indent = Indent::None,
               bool blankBefore = false) {
    if (blankBefore)
      addBlank();

    if (lastNode && node &&
        getActualStartLine(node) - getActualEndLine(lastNode) > 1) {
      addBlank();
    }

    add(factory->statementPiece(node), indent);
    lastNode = node;
  }

  Piece *build(bool forceSplit = false) {
    /* If the sequence only contains a single piece, just return it directly
     * and discard the unnecessary wrapping. */
    if (leftBracket_ == nullptr && elements.size() == 1 &&
        rightBracket_ == nullptr) {
      return const_cast<Piece *>(elements.front());
    }

    /* If there are no elements, don't bother making a SequencePiece or
     * BlockPiece. */
    if (elements.empty()) {
      std::vector<const Piece *> pieces;
      if (leftBracket_)
        pieces.push_back(leftBracket_);
      if (forceSplit || leftBracket_ == nullptr) {
        pieces.push_back(factory->create<NewlinePiece>());
      }
      if (rightBracket_)
        pieces.push_back(rightBracket_);
      return factory->create<ConcatPiece>(std::move(pieces));
    }

    /* Discard any trailing blank line after the last element. */
    auto *lastElem = const_cast<SequenceElementPiece *>(
        static_cast<const SequenceElementPiece *>(elements.back()));
    lastElem->blankAfter = false;

    auto *sequence =
        factory->create<SequencePiece>(std::vector<const Piece *>(elements));
    if (leftBracket_ != nullptr && rightBracket_ != nullptr) {
      return factory->create<BlockPiece>(leftBracket_, sequence, rightBracket_);
    }

    return sequence;
  }
};

/* Statement dispatch */

Piece *PieceFactory::statementPiece(const ASTNode *node) {
  Piece *p = dispatchStmt(node);
  if (isExpressionStatement(node->kind)) {
    p = create<ConcatPiece>(std::vector<const Piece *>{
        p, create<TextPiece>(";")});
  }
  return p;
}

/* Places annotation pieces on their own lines before [main], each followed by
 * a newline. */
Piece *prependAnnotations(PieceFactory *factory,
                          const std::vector<const Piece *> &anns,
                          Piece *main) {
  if (anns.empty())
    return main;
  std::vector<const Piece *> parts;
  for (const Piece *ann : anns) {
    parts.push_back(ann);
    parts.push_back(factory->create<NewlinePiece>());
  }
  parts.push_back(main);
  return factory->create<ConcatPiece>(std::move(parts));
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
    /* Directly append the trailing comment without injecting artificial
       spaces, as the Lexer already captures the exact preceding whitespace. */
    p = create<ConcatPiece>(std::vector<const Piece *>{
        p, create<CommentPiece>(tComment)});
  }

  if (!node->docString.empty()) {
    std::string doc = std::string(node->docString);
    int trailingNewlines = 0;
    while (!doc.empty() && doc.back() == '\n') {
      trailingNewlines++;
      doc.pop_back();
    }

    Whitespace trailing = Whitespace::Space;
    if (trailingNewlines > 1) {
      trailing = Whitespace::BlankLine;
    } else if (trailingNewlines == 1) {
      trailing = Whitespace::Newline;
    }

    p = create<ConcatPiece>(std::vector<const Piece *>{
        create<CommentPiece>(doc, trailing), p});
  }
  return p;
}

/* Expressions */

Piece *PieceFactory::buildChain(const ExprNode *node) {
  /* Dispatches an expression without attempting chain extraction. Used for
   * chain targets to avoid infinite recursion. */
  auto dispatchExprNoChain = [&](const ExprNode *e) -> Piece * {
    if (!e)
      return nullptr;
    Piece *p = dispatch(e);
    if (e->hasParens) {
      return create<ConcatPiece>(std::vector<const Piece *>{
          create<TextPiece>("("), p, create<TextPiece>(")")});
    }
    return p;
  };

  /* The left-most target of the chain. */
  const Piece *target = nullptr;

  /* Whether the target of the chain is a call or collection with a single
   * argument or element. */
  bool hasSingleElementTarget = false;

  /* The dotted property accesses and method calls following the target. */
  std::vector<ChainCall> calls;

  std::function<void(const ExprNode *)> visitTarget =
      [&](const ExprNode *e) {
        if (!e)
          return;

        /* Unwrap parens, await, and postfix to find whether the underlying
         * expression has a single element. */
        const ExprNode *inner = e;
        while (true) {
          if (inner->hasParens)
            break;
          if (const auto *await = llvm::dyn_cast<AwaitExprNode>(inner)) {
            inner = await->expr;
          } else {
            break;
          }
        }

        if (const auto *call = llvm::dyn_cast<FunctionCallNode>(inner)) {
          auto actualArgs = call->hasRawArgs ? call->rawArgs : call->args;
          hasSingleElementTarget = actualArgs.size() == 1;
        } else if (inner->kind == NodeKind::ArrayLiteral) {
          hasSingleElementTarget =
              static_cast<const ArrayLiteralNode *>(inner)->elements.size() ==
              1;
        } else if (inner->kind == NodeKind::MapLiteral) {
          hasSingleElementTarget =
              static_cast<const MapLiteralNode *>(inner)->keys.size() == 1;
        }

        target = dispatchExprNoChain(e);
      };

  std::function<void(const ExprNode *, const ExprNode *)> unwrapPostfix;
  std::function<void(const ExprNode *)> unwrap;

  /* Builds the `.member` part of a property access or method call link,
   * including any template arguments. */
  auto buildMemberStr = [&](const MemberAccessNode *ma) {
    std::string memberStr = "." + std::string(ma->memberName);
    if (!ma->templateArgs.empty()) {
      memberStr += "<";
      for (size_t i = 0; i < ma->templateArgs.size(); ++i) {
        memberStr += ma->templateArgs[i]->toString();
        if (i < ma->templateArgs.size() - 1)
          memberStr += ", ";
      }
      memberStr += ">";
    }
    return memberStr;
  };

  /* Given [expression], which is the expression for some call chain,
   * traverses the selectors to fill in the list of calls and initialize the
   * target. */
  unwrap = [&](const ExprNode *e) {
    if (!e) {
      visitTarget(e);
      return;
    }

    if (e->hasParens) {
      /* Parenthesized expressions break the chain. */
      visitTarget(e);
      return;
    }

    switch (e->kind) {
    case NodeKind::FunctionCall: {
      auto *call = static_cast<const FunctionCallNode *>(e);
      auto *ma = llvm::dyn_cast<MemberAccessNode>(call->target);

      if (!ma) {
        /* A bare call like `foo(1, 2)` is the target of the chain. */
        visitTarget(e);
        return;
      }

      unwrap(ma->object);

      /* Build the method call link: `.method(args)`. */
      ChainCallType type = ChainCallType::UnsplittableCall;

      auto actualArgs = call->hasRawArgs ? call->rawArgs : call->args;
      auto actualNames = call->hasRawArgs ? call->rawArgNames : call->argNames;

      contents->beginCall(actualArgs, actualNames);

      const Piece *listPiece =
          buildList(actualArgs, actualNames, "(", ")", ListStyle{},
                    /*allowBlockArgument=*/true, /*blockShaped=*/true);

      if (call->hasTrailingComma)
        listPiece->pin(State::Split);

      if (contents->endCall()) {
        const auto *lp = dynamic_cast<const ListPiece *>(listPiece);
        if (lp == nullptr || !lp->hasBlockElement()) {
          listPiece->pin(State::Split);
        }
      }

      const Piece *callPiece = create<ConcatPiece>(
          std::vector<const Piece *>{create<TextPiece>(buildMemberStr(ma)),
                                     listPiece});

      if (const auto *lp = dynamic_cast<const ListPiece *>(listPiece);
          lp != nullptr && lp->hasBlockElement()) {
        type = ChainCallType::BlockFormatCall;
      } else if (!actualArgs.empty() || call->hasTrailingComma) {
        type = ChainCallType::SplittableCall;
      }

      calls.push_back(ChainCall{callPiece, type});
      return;
    }

    case NodeKind::MemberAccess: {
      auto *ma = static_cast<const MemberAccessNode *>(e);
      unwrap(ma->object);

      const Piece *propertyPiece =
          create<TextPiece>(buildMemberStr(ma));
      calls.push_back(ChainCall{propertyPiece, ChainCallType::Property});
      return;
    }

    case NodeKind::ArraySubscript: {
      /* Postfix expressions are applied to the preceding call, or become part
       * of the target if there is no preceding call. */
      auto *sub = static_cast<const ArraySubscriptNode *>(e);
      unwrapPostfix(sub, sub->base);
      return;
    }

    default:
      visitTarget(e);
    }
  };

  /* When [expression] is some kind of postfix expression containing
   * [postfixPart], attempts to apply the postfix expression to the preceding
   * call in the call chain. */
  unwrapPostfix = [&](const ExprNode *expression, const ExprNode *postfixPart) {
    unwrap(postfixPart);

    auto wrapPostfix = [&](const Piece *targetPiece) {
      return create<ConcatPiece>(std::vector<const Piece *>{
          targetPiece, create<TextPiece>("["),
          dispatchExpr(static_cast<const ArraySubscriptNode *>(expression)
                           ->index),
          create<TextPiece>("]")});
    };

    if (calls.empty()) {
      /* The postfix expression ends up being part of the target. */
      hasSingleElementTarget = false;
      target = wrapPostfix(target);
    } else {
      calls.back().call = wrapPostfix(calls.back().call);
    }
  };

  unwrap(node);

  /* If there are no calls, there's no chain. */
  if (calls.empty() || target == nullptr)
    return nullptr;

  /* Count the number of contiguous properties at the beginning of the chain.
 */
  int leadingProperties = 0;
  while (leadingProperties < (int)calls.size() &&
         calls[leadingProperties].type == ChainCallType::Property) {
    leadingProperties++;
  }

  /* Count the number of leading properties and unsplittable calls. */
  int leadingUnsplittable = leadingProperties;
  while (leadingUnsplittable < (int)calls.size() &&
         !calls[leadingUnsplittable].canSplit()) {
    leadingUnsplittable++;
  }

  /* See if we can block format the chain on one of its calls. We allow the
   * last call in a chain to block format, but we only allow it to do so if
   * either the preceding calls can't split or the last call is actually a
   * block formatted argument list and not just a split argument list.
   *
   * Further, we allow the second-to-last call in the chain to be the block
   * formatted call if the last call is a property or unsplittable call and the
   * preceding call can block format. This allows for common hanging operations
   * like `toList()`. */
  int lastCallIndex = (int)calls.size() - 1;
  if (!calls[lastCallIndex].canSplit() && calls.size() > 1 &&
      calls[lastCallIndex - 1].type == ChainCallType::BlockFormatCall) {
    lastCallIndex = (int)calls.size() - 2;
  }

  int blockCallIndex = -1;
  if (leadingUnsplittable == lastCallIndex &&
      calls[lastCallIndex].canSplit()) {
    blockCallIndex = lastCallIndex;
  } else if (calls[lastCallIndex].type == ChainCallType::BlockFormatCall) {
    blockCallIndex = lastCallIndex;
  }

  return create<ChainPiece>(target, std::move(calls),
                            /*cascade=*/false, leadingProperties,
                            blockCallIndex, Indent::Expression,
                            hasSingleElementTarget);
}

Piece *PieceFactory::dispatchExpr(const ExprNode *node) {  if (!node)
    return nullptr;

  if (!node->hasParens && (node->kind == NodeKind::FunctionCall ||
                           node->kind == NodeKind::MemberAccess)) {
    if (Piece *chain = buildChain(node)) {
      return chain;
    }
  }

  Piece *p = dispatch(node);
  if (node->hasParens) {
    return create<ConcatPiece>(std::vector<const Piece *>{
        create<TextPiece>("("), p, create<TextPiece>(")")});
  }
  return p;
}

bool PieceFactory::canBlockFormat(const ExprNode *node) const {
  switch (node->kind) {
  case NodeKind::Lambda: {
    auto *lambda = static_cast<const LambdaNode *>(node);
    /* A function expression with a non-empty block body. */
    return lambda->body != nullptr && !lambda->body->statements.empty();
  }
  case NodeKind::ArrayLiteral:
    return !static_cast<const ArrayLiteralNode *>(node)->elements.empty();
  case NodeKind::MapLiteral:
    return !static_cast<const MapLiteralNode *>(node)->keys.empty();
  default:
    return false;
  }
}

int PieceFactory::candidateBlockArgument(
    const llvm::ArrayRef<ExprNode *> &args,
    const llvm::ArrayRef<std::string_view> &names) {
  /* The index of the function expression argument, or -1 if none has been
   * found or -2 if there are multiple. */
  int functionIndex = -1;

  /* The index of the collection literal argument, or -1 if none has been
   * found or -2 if there are multiple. */
  int collectionIndex = -1;

  for (size_t i = 0; i < args.size(); i++) {
    if (!names.empty() && !names[i].empty())
      continue;

    switch (args[i]->kind) {
    case NodeKind::Lambda:
      if (static_cast<const LambdaNode *>(args[i])->body != nullptr) {
        if (functionIndex >= 0) {
          functionIndex = -2;
        } else {
          functionIndex = static_cast<int>(i);
        }
      }
      break;

    case NodeKind::ArrayLiteral:
    case NodeKind::MapLiteral:
      if (canBlockFormat(args[i])) {
        if (collectionIndex >= 0) {
          collectionIndex = -2;
        } else {
          collectionIndex = static_cast<int>(i);
        }
      }
      break;

    default:
      break;
    }
  }

  if (functionIndex >= 0)
    return functionIndex;
  if (collectionIndex >= 0)
    return collectionIndex;

  return -1;
}

Piece *PieceFactory::buildList(const llvm::ArrayRef<ExprNode *> &args,
                               const llvm::ArrayRef<std::string_view> &names,
                               const char *leftBracket,
                               const char *rightBracket, ListStyle style,
                               bool allowBlockArgument, bool blockShaped) {
  /* If the list is completely empty, write the brackets directly inline so
   * that we create fewer pieces. */
  if (args.empty()) {
    return create<ConcatPiece>(std::vector<const Piece *>{
        create<TextPiece>(leftBracket), create<TextPiece>(rightBracket)});
  }

  int candidateIndex = -1;
  if (allowBlockArgument) {
    candidateIndex = candidateBlockArgument(args, names);

    /* The block argument must be positional. */
    if (candidateIndex >= 0 && !names.empty() && !names[candidateIndex].empty())
      candidateIndex = -1;

    /* Only allow up to one trailing argument after the block argument. */
    if (candidateIndex >= 0 && candidateIndex < (int)args.size() - 2)
      candidateIndex = -1;
  }

  std::vector<const Piece *> elements;
  for (size_t i = 0; i < args.size(); i++) {
    const Piece *content = nullptr;
    if (!names.empty() && !names[i].empty()) {
      /* Named arguments split at the `:` like an assignment. */
      namedArgDepth++;
      content = dispatchExpr(args[i]);
      namedArgDepth--;

      content = create<AssignPiece>(
          create<ConcatPiece>(std::vector<const Piece *>{
              create<TextPiece>(std::string(names[i])),
              create<TextPiece>(":")}),
          content);
    } else {
      content = dispatchExpr(args[i]);
    }

    auto *element = create<ListElementPiece>(content);
    if ((int)i == candidateIndex) {
      element->allowNewlinesWhenUnsplit = true;
    }
    elements.push_back(element);
  }

  auto *list = create<ListPiece>(
      create<TextPiece>(leftBracket), std::move(elements),
      create<TextPiece>(rightBracket), style,
      static_cast<int>(args.size()) - 1, blockShaped);
  return list;
}

Piece *PieceFactory::visit(const BinaryOpNode *node) {
  /* In a tree of binary AST nodes, all operators at the same precedence are
   * treated as a single chain of operators that either all split or none do.
   * Operands within those (which may themselves be chains of higher precedence
   * binary operators) are then formatted independently. */
  int precedence = opPrecedence(node->op);

  std::vector<const Piece *> operandPieces;
  std::vector<std::string> ops;

  std::function<void(const ExprNode *)> flatten =
      [&](const ExprNode *e) {
        if (const auto *bin = llvm::dyn_cast<BinaryOpNode>(e)) {
          if (opPrecedence(bin->op) == precedence) {
            flatten(bin->left);
            ops.push_back(std::string(bin->op));
            flatten(bin->right);
            return;
          }
        }
        operandPieces.push_back(dispatchExpr(e));
      };
  flatten(node);

  std::vector<const Piece *> operands;
  for (size_t i = 0; i < ops.size(); i++) {
    /* The hanging operator is embedded in the preceding operand, so `1 + 2`
     * becomes Infix(`1 +`, `2`). */
    operands.push_back(create<ConcatPiece>(std::vector<const Piece *>{
        operandPieces[i], create<TextPiece>(" "),
        create<TextPiece>(ops[i])}));
  }
  operands.push_back(operandPieces.back());

  return create<InfixPiece>(std::move(operands), Indent::Infix,
                            /*conditional=*/false);
}

Piece *PieceFactory::visit(const TernaryOpNode *node) {
  /* Flatten a series of else-if-like chained conditionals into a single long
   * infix piece. */
  std::vector<const Piece *> operands;
  operands.push_back(dispatchExpr(node->condition));

  bool hasNestedConditional = false;

  auto addOperand = [&](const char *op, const ExprNode *operand) {
    const Piece *operandPiece = dispatchExpr(operand);

    /* If conditional expressions are directly nested in a branch, force them
     * to split too. */
    if (operand->kind == NodeKind::TernaryOp) {
      operandPiece->pin(State::Split);
      hasNestedConditional = true;
    }

    operands.push_back(create<ConcatPiece>(std::vector<const Piece *>{
        create<TextPiece>(op), create<SpacePiece>(), operandPiece}));
  };

  const TernaryOpNode *current = node;
  while (true) {
    addOperand("?", current->trueExpr);
    if (current->trueExpr->kind == NodeKind::TernaryOp) {
      hasNestedConditional = true;
    }

    if (const auto *elseTernary =
            llvm::dyn_cast<TernaryOpNode>(current->falseExpr)) {
      addOperand(":", elseTernary->condition);
      hasNestedConditional = true;
      current = elseTernary;
    } else {
      addOperand(":", current->falseExpr);
      break;
    }
  }

  auto *piece =
      create<InfixPiece>(std::move(operands), Indent::Infix, /*conditional=*/true);

  /* If conditional expressions are directly nested, force them all to split,
   * both parents and children. */
  if (hasNestedConditional) {
    piece->pin(State::Split);
  }

  return piece;
}

Piece *PieceFactory::visit(const AssignNode *node) {
  const Piece *left = dispatchExpr(node->target);
  const Piece *right = dispatchExpr(node->value);

  /* The operator is embedded in the left-hand side. */
  const Piece *leftPiece = create<ConcatPiece>(std::vector<const Piece *>{
      left, create<TextPiece>(" "), create<TextPiece>(std::string(node->op))});

  return create<AssignPiece>(leftPiece, right);
}

Piece *PieceFactory::visit(const UnaryOpNode *node) {
  const Piece *expr = dispatchExpr(node->expr);
  if (node->isPostfix) {
    return create<ConcatPiece>(std::vector<const Piece *>{
        expr, create<TextPiece>(std::string(node->op))});
  }
  return create<ConcatPiece>(std::vector<const Piece *>{
      create<TextPiece>(std::string(node->op)), expr});
}

Piece *PieceFactory::visit(const AwaitExprNode *node) {
  return create<ConcatPiece>(std::vector<const Piece *>{
      create<TextPiece>("await "), dispatchExpr(node->expr)});
}

Piece *PieceFactory::visit(const CastNode *node) {
  std::string typeStr = node->rawTargetTypeStr.empty()
                            ? node->targetType->toString()
                            : std::string(node->rawTargetTypeStr);

  /* The leading operator goes in the following operand, so `foo as int`
   * becomes Infix(`foo`, `as int`). */
  return create<InfixPiece>(
      std::vector<const Piece *>{
          dispatchExpr(node->expr),
          create<ConcatPiece>(std::vector<const Piece *>{
              create<TextPiece>("as "), create<TextPiece>(typeStr)})},
      Indent::Expression);
}

Piece *PieceFactory::visit(const IsExprNode *node) {
  std::string typeStr = node->rawTargetTypeStr.empty()
                            ? node->targetType->toString()
                            : std::string(node->rawTargetTypeStr);

  /* `foo is Type` / `foo is! Type` becomes Infix(`foo`, `is Type`). */
  std::string op = node->isNegated ? "is! " : "is ";
  return create<InfixPiece>(
      std::vector<const Piece *>{
          dispatchExpr(node->expr),
          create<ConcatPiece>(std::vector<const Piece *>{
              create<TextPiece>(op), create<TextPiece>(typeStr)})},
      Indent::Expression);
}

Piece *PieceFactory::visit(const ArraySubscriptNode *node) {
  return create<ConcatPiece>(std::vector<const Piece *>{
      dispatchExpr(node->base), create<TextPiece>("["),
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
  return create<ConcatPiece>(std::vector<const Piece *>{
      dispatchExpr(node->object), create<TextPiece>(memberStr)});
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
  const char hexDigits[] = "0123456789ABCDEF";
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
      /* Escape non-printable control characters back to hex representation */
      if (static_cast<unsigned char>(c) < 32 ||
          static_cast<unsigned char>(c) == 127) {
        escaped += "\\x";
        escaped += hexDigits[(static_cast<unsigned char>(c) >> 4) & 0x0F];
        escaped += hexDigits[static_cast<unsigned char>(c) & 0x0F];
      } else {
        escaped += c;
      }
      break;
    }
  }
  escaped += "\"";
  /* String literals are "soft": they don't force surrounding pieces to split.
 */
  return create<TextPiece>(escaped, /*soft=*/true);
}

Piece *PieceFactory::visit(const BoolNode *node) {
  return create<TextPiece>(node->value ? "true" : "false");
}

Piece *PieceFactory::visit(const CharNode *node) {
  std::string escaped;
  const char hexDigits[] = "0123456789ABCDEF";
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
    /* Escape non-printable control characters back to hex representation */
    if (node->value < 32 || node->value == 127) {
      escaped += "\\x";
      escaped += hexDigits[(node->value >> 4) & 0x0F];
      escaped += hexDigits[node->value & 0x0F];
    } else {
      escaped = std::string(1, static_cast<char>(node->value));
    }
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

Piece *PieceFactory::visit(const ImplicitCastNode *node) {
  return dispatchExpr(node->expr);
}

Piece *PieceFactory::visit(const LambdaNode *node) {
  std::vector<const Piece *> pieces;

  if (node->explicitReturnType) {
    pieces.push_back(
        create<TextPiece>(node->explicitReturnType->toString() + " "));
  }

  std::vector<const Piece *> params;
  for (const auto *p : node->params) {
    params.push_back(create<ListElementPiece>(dispatchStmt(p)));
  }
  auto *paramList = create<ListPiece>(
      create<TextPiece>("("), std::move(params), create<TextPiece>(")"),
      ListStyle{}, node->params.empty() ? -1 : (int)node->params.size() - 1,
      /*blockShaped=*/true);
  pieces.push_back(paramList);

  if (node->isExpressionBody && node->exprBody) {
    /* Prefer splitting at `=>` and keeping the expression together unless it
     * is a collection literal. */
    bool isCollection = node->exprBody->kind == NodeKind::ArrayLiteral ||
                        node->exprBody->kind == NodeKind::MapLiteral;
    pieces.push_back(create<AssignPiece>(
        create<TextPiece>(" =>"), dispatchExpr(node->exprBody),
        /*avoidSplit=*/isCollection));
  } else if (node->body) {
    pieces.push_back(create<SpacePiece>());
    pieces.push_back(dispatch(node->body));
  }

  return create<ConcatPiece>(std::move(pieces));
}

Piece *PieceFactory::visit(const FunctionCallNode *node) {
  /* This is a "bare" function call like `foo(1, 2)`. */
  const Piece *target = dispatchExpr(node->target);

  auto actualArgs = node->hasRawArgs ? node->rawArgs : node->args;
  auto actualNames = node->hasRawArgs ? node->rawArgNames : node->argNames;

  if (actualArgs.empty() && !node->hasTrailingComma) {
    return create<ConcatPiece>(std::vector<const Piece *>{
        target, create<TextPiece>("()")});
  }

  contents->beginCall(actualArgs, actualNames);

  const Piece *listPiece =
      buildList(actualArgs, actualNames, "(", ")", ListStyle{},
                /*allowBlockArgument=*/true, /*blockShaped=*/true);

  if (node->hasTrailingComma) {
    listPiece->pin(State::Split);
  }

  if (contents->endCall()) {
    /* Don't force an argument list to fully split if it could block split. */
    const auto *lp = dynamic_cast<const ListPiece *>(listPiece);
    if (lp == nullptr || !lp->hasBlockElement()) {
      listPiece->pin(State::Split);
    }
  }

  return create<ConcatPiece>(std::vector<const Piece *>{target, listPiece});
}

Piece *PieceFactory::visit(const ArrayLiteralNode *node) {
  contents->beginCollection(/*isNamed=*/namedArgDepth > 0);

  std::vector<const Piece *> elements;
  for (const auto *elem : node->elements) {
    elements.push_back(create<ListElementPiece>(dispatchExpr(elem)));
  }

  auto *list = create<ListPiece>(
      create<TextPiece>("["), std::move(elements), create<TextPiece>("]"),
      ListStyle{}, node->elements.empty() ? -1 : (int)node->elements.size() - 1,
      /*blockShaped=*/true);

  if (contents->endCollection(node->elements.size()) ||
      node->hasTrailingComma) {
    list->pin(State::Split);
  }

  return list;
}

Piece *PieceFactory::visit(const MapLiteralNode *node) {
  contents->beginCollection(/*isNamed=*/namedArgDepth > 0);

  std::vector<const Piece *> entries;
  for (size_t i = 0; i < node->keys.size(); i++) {
    /* Map entries split at the `:` like an assignment. */
    const Piece *entry = create<AssignPiece>(
        create<ConcatPiece>(std::vector<const Piece *>{
            dispatchExpr(node->keys[i]), create<TextPiece>(":")}),
        dispatchExpr(node->values[i]));
    entries.push_back(create<ListElementPiece>(entry));
  }

  auto *list = create<ListPiece>(
      create<TextPiece>("{"), std::move(entries), create<TextPiece>("}"),
      ListStyle{}, node->keys.empty() ? -1 : (int)node->keys.size() - 1,
      /*blockShaped=*/true);

  if (contents->endCollection(node->keys.size()) || node->hasTrailingComma) {
    list->pin(State::Split);
  }

  return list;
}

Piece *PieceFactory::visit(const NewExprNode *node) {
  std::vector<const Piece *> parts;
  std::string typeStr = node->rawAllocatedTypeStr.empty()
                            ? node->allocatedType->toString()
                            : std::string(node->rawAllocatedTypeStr);
  parts.push_back(create<TextPiece>("new"));
  parts.push_back(create<TextPiece>(" " + typeStr));
  if (node->arraySize) {
    parts.push_back(create<TextPiece>("["));
    parts.push_back(dispatchExpr(node->arraySize));
    parts.push_back(create<TextPiece>("]"));
  } else if (node->hasParens) {
    auto actualArgs = node->hasRawArgs ? node->rawArgs : node->args;
    auto actualNames = node->hasRawArgs ? node->rawArgNames : node->argNames;

    contents->beginCall(actualArgs, actualNames);

    const Piece *listPiece =
        buildList(actualArgs, actualNames, "(", ")", ListStyle{},
                  /*allowBlockArgument=*/true, /*blockShaped=*/true);

    if (node->hasTrailingComma)
      listPiece->pin(State::Split);

    if (contents->endCall()) {
      const auto *lp = dynamic_cast<const ListPiece *>(listPiece);
      if (lp == nullptr || !lp->hasBlockElement()) {
        listPiece->pin(State::Split);
      }
    }

    parts.push_back(listPiece);
  }
  return create<ConcatPiece>(std::move(parts));
}

Piece *PieceFactory::visit(const DeleteExprNode *node) {
  std::vector<const Piece *> parts;
  parts.push_back(create<TextPiece>("delete"));
  if (node->isArray)
    parts.push_back(create<TextPiece>("[]"));
  parts.push_back(create<TextPiece>(" "));
  parts.push_back(dispatchExpr(node->ptr));
  return create<ConcatPiece>(std::move(parts));
}

Piece *PieceFactory::visit(const ConstExprNode *node) {
  std::vector<const Piece *> parts;
  parts.push_back(create<TextPiece>("const "));
  parts.push_back(dispatchExpr(node->expr));
  return create<ConcatPiece>(std::move(parts));
}

Piece *PieceFactory::visit(const DestructorCallNode *node) {
  std::vector<const Piece *> parts;
  parts.push_back(dispatchExpr(node->object));
  parts.push_back(create<TextPiece>(".~" + node->targetType->toString() +
                                    "()"));
  return create<ConcatPiece>(std::move(parts));
}

Piece *PieceFactory::visit(const ReturnNode *node) {
  if (node->value) {
    return create<ConcatPiece>(std::vector<const Piece *>{
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

Piece *PieceFactory::visit(const UsingNode *node) {
  return create<TextPiece>("using " + std::string(node->name) + ";");
}

/* 'try { ... } catch (T e) { ... } catch (...) { ... }': every catch clause
 * gets its own block section, always split, mirroring the C++ style. */
Piece *PieceFactory::visit(const TryStmtNode *node) {
  auto *ctrl = create<ControlFlowPiece>();

  const Piece *tryHeader = create<ConcatPiece>(std::vector<const Piece *>{
      create<TextPiece>("try"), create<SpacePiece>()});
  ctrl->add(tryHeader, dispatchStmt(node->body), node->body->hasBraces);

  for (const auto *clause : node->clauses) {
    std::string header = "catch (";
    if (clause->isCatchAll) {
      header += "...";
    } else {
      header += std::string(clause->rawTypeStr);
      if (!clause->varName.empty()) {
        header += " " + std::string(clause->varName);
      }
    }
    header += ") ";
    const Piece *catchHeader = create<TextPiece>(header);
    ctrl->add(catchHeader, dispatchStmt(clause->body),
              clause->body->hasBraces);
  }

  ctrl->pin(State::Split);
  return ctrl;
}

Piece *PieceFactory::visit(const ThrowStmtNode *node) {
  if (!node->value) {
    return create<TextPiece>("throw;");
  }
  return create<ConcatPiece>(std::vector<const Piece *>{
      create<TextPiece>("throw "), dispatchExpr(node->value),
      create<TextPiece>(";")});
}

Piece *PieceFactory::visit(const AssertStmtNode *node) {
  return create<ConcatPiece>(std::vector<const Piece *>{
      create<TextPiece>("assert("), dispatchExpr(node->condition),
      create<TextPiece>(");")});
}

/* Declarations */

Piece *PieceFactory::visit(const VarDeclNode *node) {
  std::vector<const Piece *> parts;
  for (auto *ann : node->annotations) {
    parts.push_back(dispatch(ann));
  }

  std::string pfx = "";
  if (node->hasPublicMod)
    pfx += "public ";
  if (node->hasPrivateMod)
    pfx += "private ";
  if (node->hasProtectedMod)
    pfx += "protected ";
  if (node->isStatic)
    pfx += "static ";
  if (node->isFinal)
    pfx += "final ";
  if (node->type) {
    std::string typeStr = node->rawTypeStr.empty()
                              ? node->type->toString()
                              : std::string(node->rawTypeStr);
    pfx += typeStr + " ";
  }
  pfx += std::string(node->varName);

  Piece *decl = create<TextPiece>(pfx);
  if (node->initializer) {
    const Piece *leftPiece = create<ConcatPiece>(std::vector<const Piece *>{
        decl, create<TextPiece>(" =")});
    decl = create<AssignPiece>(leftPiece, dispatchExpr(node->initializer));
  }

  Piece *mainDecl = create<ConcatPiece>(std::vector<const Piece *>{
      decl, create<TextPiece>(";")});

  return prependAnnotations(this, parts, mainDecl);
}

Piece *PieceFactory::visit(const ParamDeclNode *node) {
  std::string pfx = "";
  if (node->isRequired)
    pfx += "required ";
  if (node->isThisParam) {
    pfx += "this." + std::string(node->name);
  } else {
    if (node->type) {
      std::string typeStr = node->rawTypeStr.empty()
                                ? node->type->toString()
                                : std::string(node->rawTypeStr);
      pfx += typeStr + " ";
    }
    pfx += std::string(node->name);
  }

  Piece *p = create<TextPiece>(pfx);
  if (node->defaultValue) {
    const Piece *leftPiece = create<ConcatPiece>(std::vector<const Piece *>{
        p, create<TextPiece>(" =")});
    p = create<AssignPiece>(leftPiece, dispatchExpr(node->defaultValue));
  }
  return p;
}

Piece *PieceFactory::visit(const FunctionDeclNode *node) {
  std::vector<const Piece *> sigParts;

  for (auto *ann : node->annotations) {
    sigParts.push_back(dispatch(ann));
  }

  std::string pfx = "";
  if (node->hasPublicMod)
    pfx += "public ";
  if (node->hasPrivateMod)
    pfx += "private ";
  if (node->hasProtectedMod)
    pfx += "protected ";
  if (node->isStatic)
    pfx += "static ";
  if (node->isConst)
    pfx += "const ";

  /* Prevent injecting 'void' into constructors/destructors by relying
     strictly on the raw parsed string rather than the inferred AST type. */
  if (!node->rawReturnTypeStr.empty()) {
    pfx += std::string(node->rawReturnTypeStr) + " ";
  }

  /* The record type name is fully qualified; the destructor must print the
   * simple name so the output parses back ('~' + record name). */
  if (node->name == "~" && node->parentRecord) {
    std::string recName = std::string(node->parentRecord->getName());
    size_t dot = recName.find_last_of('.');
    if (dot != std::string::npos)
      recName = recName.substr(dot + 1);
    pfx += "~" + recName;
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

  std::vector<const Piece *> signature;
  signature.push_back(create<TextPiece>(pfx));

  std::vector<const Piece *> params;
  bool insideNamed = false;
  for (size_t i = 0; i < node->params.size(); ++i) {
    auto *p = node->params[i];
    std::vector<const Piece *> paramPieces;
    if (p->isNamed && !insideNamed) {
      paramPieces.push_back(create<TextPiece>("{"));
      insideNamed = true;
    }
    paramPieces.push_back(dispatchStmt(p));
    if (insideNamed && i == node->params.size() - 1) {
      paramPieces.push_back(create<TextPiece>("}"));
    }
    params.push_back(
        create<ListElementPiece>(create<ConcatPiece>(std::move(paramPieces))));
  }

  if (node->isVariadic) {
    params.push_back(create<ListElementPiece>(create<TextPiece>("...")));
  }

  signature.push_back(create<ListPiece>(
      create<TextPiece>("("), std::move(params), create<TextPiece>(")"),
      ListStyle{}, node->params.empty() ? -1 : (int)node->params.size() - 1,
      /*blockShaped=*/true));

  if (node->isAsync) {
    signature.push_back(create<TextPiece>(" async"));
  }

  Piece *mainSig = create<ConcatPiece>(std::move(signature));

  if (!node->fieldInitializers.empty() || node->superCall) {
    std::vector<const Piece *> initParts;
    initParts.push_back(create<TextPiece>(" : "));
    bool first = true;
    for (const auto *init : node->fieldInitializers) {
      if (!first)
        initParts.push_back(create<TextPiece>(", "));
      first = false;
      initParts.push_back(dispatchExpr(init));
    }
    if (node->superCall) {
      if (!first)
        initParts.push_back(create<TextPiece>(", "));
      initParts.push_back(dispatchExpr(node->superCall));
    }
    mainSig = create<ConcatPiece>(std::vector<const Piece *>{
        mainSig, create<ConcatPiece>(std::move(initParts))});
  }

  if (node->body) {
    if (node->body->isExpressionBody && !node->body->statements.empty()) {
      const Piece *bodyPiece = nullptr;
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

      /* Prefer splitting at `=>` and keeping the expression together unless
       * it is a collection literal. */
      bool isCollection = false;
      if (auto *retNode = llvm::dyn_cast<ReturnNode>(innerStmt)) {
        isCollection = retNode->value &&
                       (retNode->value->kind == NodeKind::ArrayLiteral ||
                        retNode->value->kind == NodeKind::MapLiteral);
      }

      mainSig = create<ConcatPiece>(std::vector<const Piece *>{
          mainSig,
          create<AssignPiece>(create<TextPiece>(" =>"), bodyPiece,
                              /*avoidSplit=*/isCollection),
          create<TextPiece>(";")});
    } else {
      mainSig = create<ConcatPiece>(std::vector<const Piece *>{
          mainSig, create<SpacePiece>(), dispatchStmt(node->body)});
    }
  } else {
    mainSig = create<ConcatPiece>(std::vector<const Piece *>{
        mainSig, create<TextPiece>(";")});
  }

  return prependAnnotations(this, sigParts, mainSig);
}

Piece *PieceFactory::visit(const EnumMemberNode *node) {
  Piece *p = create<TextPiece>(std::string(node->name));
  if (node->initializer) {
    const Piece *leftPiece = create<ConcatPiece>(std::vector<const Piece *>{
        p, create<TextPiece>(" =")});
    p = create<AssignPiece>(leftPiece, dispatchExpr(node->initializer));
  }
  return p;
}

Piece *PieceFactory::visit(const EnumDeclNode *node) {
  std::vector<const Piece *> parts;
  for (auto *ann : node->annotations) {
    parts.push_back(dispatch(ann));
  }

  std::string pfx = "enum " + std::string(node->name);
  std::vector<const Piece *> members;
  for (auto *m : node->members) {
    members.push_back(create<ListElementPiece>(dispatchStmt(m)));
  }

  Piece *body = create<ListPiece>(
      create<TextPiece>("{"), std::move(members), create<TextPiece>("}"),
      ListStyle{/*commas=*/Commas::Trailing, /*splitCost=*/Cost::Normal,
                /*spaceWhenUnsplit=*/true},
      node->members.empty() ? -1 : (int)node->members.size() - 1,
      /*blockShaped=*/true);

  if (node->hasTrailingComma)
    body->pin(State::Split);

  Piece *mainEnum = create<ConcatPiece>(std::vector<const Piece *>{
      create<TextPiece>(pfx), create<TextPiece>(" "), body});

  return prependAnnotations(this, parts, mainEnum);
}

Piece *PieceFactory::visit(const AnnotationNode *node) {
  std::vector<const Piece *> parts;
  parts.push_back(create<TextPiece>("@" + std::string(node->name)));
  if (!node->args.empty()) {
    std::vector<const Piece *> args;
    for (auto *a : node->args) {
      args.push_back(create<ListElementPiece>(dispatchExpr(a)));
    }
    parts.push_back(create<ListPiece>(
        create<TextPiece>("("), std::move(args), create<TextPiece>(")"),
        ListStyle{}, (int)node->args.size() - 1, /*blockShaped=*/true));
  }
  return create<ConcatPiece>(std::move(parts));
}

template <typename T>
Piece *createRecord(PieceFactory *factory, const T *node, const char *kw) {
  std::vector<const Piece *> parts;
  for (auto *ann : node->annotations) {
    parts.push_back(factory->dispatch(ann));
  }

  std::string pfx = "";
  if (auto *cls = llvm::dyn_cast<ClassDeclNode>(node)) {
    if (cls->isAbstract) {
      pfx += "abstract ";
    }
    if (cls->isFinal) {
      pfx += "final ";
    }
  }
  pfx += std::string(kw) + " " + std::string(node->name);

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

  if (auto *cls = llvm::dyn_cast<ClassDeclNode>(node)) {
    /* The resolved types carry fully qualified names; the raw source text
     * keeps the original spelling so the output parses back unchanged. */
    if (cls->baseClass) {
      pfx += " extends " +
             (cls->rawBaseClassStr.empty()
                  ? cls->baseClass->toString()
                  : std::string(cls->rawBaseClassStr));
    }
    if (!cls->interfaces.empty()) {
      pfx += " implements ";
      for (size_t i = 0; i < cls->interfaces.size(); ++i) {
        if (i < cls->rawInterfaces.size() && !cls->rawInterfaces[i].empty()) {
          pfx += std::string(cls->rawInterfaces[i]);
        } else {
          pfx += cls->interfaces[i]->toString();
        }
        if (i < cls->interfaces.size() - 1)
          pfx += ", ";
      }
    }
  }

  Piece *mainRecord;

  if (node->isOpaque) {
    mainRecord = factory->create<TextPiece>(pfx + ";");
  } else {
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

    SequenceBuilder seq(factory);
    seq.leftBracket(factory->create<TextPiece>("{"));

    for (size_t i = 0; i < allMembers.size(); ++i) {
      bool blankBefore =
          i > 0 && hasNonEmptyBody(allMembers[i - 1]);
      seq.addNode(allMembers[i], Indent::None, blankBefore);
    }

    seq.rightBracket(factory->create<TextPiece>("}"));
    mainRecord = factory->create<ConcatPiece>(std::vector<const Piece *>{
        factory->create<TextPiece>(pfx), factory->create<TextPiece>(" "),
        seq.build()});
  }

  return prependAnnotations(factory, parts, mainRecord);
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

Piece *PieceFactory::visit(const AnnotationDeclNode *node) {
  std::vector<const Piece *> parts;
  for (auto *ann : node->annotations) {
    parts.push_back(dispatch(ann));
  }

  std::string pfx = "annotation class " + std::string(node->name);
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

  SequenceBuilder seq(this);
  seq.leftBracket(create<TextPiece>("{"));

  for (size_t i = 0; i < allMembers.size(); ++i) {
    bool blankBefore = i > 0 && hasNonEmptyBody(allMembers[i - 1]);
    seq.addNode(allMembers[i], Indent::None, blankBefore);
  }

  seq.rightBracket(create<TextPiece>("}"));
  Piece *mainAnn = create<ConcatPiece>(std::vector<const Piece *>{
      create<TextPiece>(pfx), create<TextPiece>(" "), seq.build()});

  return prependAnnotations(this, parts, mainAnn);
}

Piece *PieceFactory::visit(const TypedefDeclNode *node) {
  std::vector<const Piece *> parts;
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

  return prependAnnotations(this, parts, mainTd);
}

/* Statements and control flow */

Piece *PieceFactory::visit(const BlockNode *node) {
  if (node->hasBraces) {
    SequenceBuilder seq(this);
    seq.leftBracket(create<TextPiece>("{"));

    for (size_t i = 0; i < node->statements.size(); ++i) {
      bool blankBefore = i > 0 && hasNonEmptyBody(node->statements[i - 1]);
      seq.addNode(node->statements[i], Indent::None, blankBefore);
    }

    seq.rightBracket(create<TextPiece>("}"));
    return seq.build();
  }

  /* A braces-less block is a series of statements written directly. */
  if (node->statements.size() == 1) {
    return statementPiece(node->statements[0]);
  }

  SequenceBuilder seq(this);
  for (const auto *stmt : node->statements) {
    seq.addNode(stmt);
  }
  return seq.build();
}

Piece *PieceFactory::visit(const IfNode *node) {
  auto *ctrl = create<ControlFlowPiece>();

  std::function<void(const char *, const IfNode *)> traverse =
      [&](const char *elsePrefix, const IfNode *n) {
        /* The header: `if (condition) ` or `else if (condition) `. */
        const Piece *header = create<ConcatPiece>(std::vector<const Piece *>{
            create<TextPiece>(elsePrefix), create<TextPiece>("if"),
            create<SpacePiece>(), create<TextPiece>("("),
            dispatchExpr(n->condition), create<TextPiece>(")"),
            create<SpacePiece>()});

        /* Edge case: When the then branch is a block and there is an else
         * clause after it, force the block to split even if empty. */
        const Piece *thenPiece = nullptr;
        if (n->thenBlock->hasBraces && n->elseBlock != nullptr &&
            n->thenBlock->statements.empty()) {
          thenPiece = create<ConcatPiece>(std::vector<const Piece *>{
              create<TextPiece>("{"), create<NewlinePiece>(),
              create<TextPiece>("}")});
        } else {
          thenPiece = dispatchStmt(n->thenBlock);
        }
        ctrl->add(header, thenPiece, /*isBlock=*/n->thenBlock->hasBraces);

        if (const auto *elseIf =
                llvm::dyn_cast_or_null<IfNode>(n->elseBlock)) {
          /* Hit an else-if, so flatten it into the chain with the `else`
           * becoming part of the next section's header. */
          traverse("else ", elseIf);
        } else if (n->elseBlock) {
          const Piece *elseHeader =
              create<ConcatPiece>(std::vector<const Piece *>{
                  create<TextPiece>("else"), create<SpacePiece>()});
          bool elseIsBlock = n->elseBlock->kind == NodeKind::Block &&
                             static_cast<const BlockNode *>(n->elseBlock)
                                     ->hasBraces;
          ctrl->add(elseHeader, dispatchStmt(n->elseBlock), elseIsBlock);
        }
      };

  traverse("", node);

  /* If statements almost always split at the clauses unless the if is a
   * simple if with only a single unbraced then statement and no else clause. */
  if (node->thenBlock->hasBraces || node->elseBlock != nullptr) {
    ctrl->pin(State::Split);
  }

  return ctrl;
}

Piece *PieceFactory::visit(const WhileNode *node) {
  const Piece *header = create<ConcatPiece>(std::vector<const Piece *>{
      create<TextPiece>("while"), create<SpacePiece>(),
      create<TextPiece>("("), dispatchExpr(node->condition),
      create<TextPiece>(")"), create<SpacePiece>()});

  auto *ctrl = create<ControlFlowPiece>();
  ctrl->add(header, dispatchStmt(node->body),
            /*isBlock=*/node->body->hasBraces);
  return ctrl;
}

Piece *PieceFactory::visit(const ForNode *node) {
  /* In a C-style for loop, treat the for loop parts like an argument list
   * where each clause is a separate element. */
  std::vector<const Piece *> parts;

  if (node->initStatement) {
    Piece *initPiece = dispatchStmt(node->initStatement);
    /* Variable declarations already include their `;`. */
    if (isExpressionStatement(node->initStatement->kind)) {
      initPiece = create<ConcatPiece>(std::vector<const Piece *>{
          initPiece, create<TextPiece>(";")});
    }
    parts.push_back(create<ListElementPiece>(initPiece));
  } else {
    parts.push_back(create<ListElementPiece>(create<TextPiece>(";")));
  }

  if (node->condition) {
    parts.push_back(create<ListElementPiece>(create<ConcatPiece>(
        std::vector<const Piece *>{dispatchExpr(node->condition),
                                   create<TextPiece>(";")})));
  } else {
    parts.push_back(create<ListElementPiece>(create<TextPiece>(";")));
  }

  if (node->increment) {
    parts.push_back(create<ListElementPiece>(dispatchExpr(node->increment)));
  }

  auto *partsList = create<ListPiece>(
      create<TextPiece>("("), std::move(parts), create<TextPiece>(")"),
      ListStyle{/*commas=*/Commas::None}, (int)parts.size() - 1,
      /*blockShaped=*/true);

  const Piece *header = create<ConcatPiece>(std::vector<const Piece *>{
      create<TextPiece>("for"), create<SpacePiece>(), partsList,
      create<SpacePiece>()});

  auto *ctrl = create<ControlFlowPiece>();
  ctrl->add(header, dispatchStmt(node->body),
            /*isBlock=*/node->body->hasBraces);
  return ctrl;
}

Piece *PieceFactory::visit(const SwitchNode *node) {
  const Piece *header = create<ConcatPiece>(std::vector<const Piece *>{
      create<TextPiece>("switch"), create<SpacePiece>(),
      create<TextPiece>("("), dispatchExpr(node->condition),
      create<TextPiece>(")"), create<SpacePiece>()});

  SequenceBuilder seq(this);
  seq.leftBracket(create<TextPiece>("{"));

  for (const auto *c : node->cases) {
    const Piece *labelPiece = nullptr;
    if (c->value) {
      labelPiece = create<ConcatPiece>(std::vector<const Piece *>{
          create<TextPiece>("case "), dispatchExpr(c->value),
          create<TextPiece>(":")});
    } else {
      labelPiece = create<TextPiece>("default:");
    }

    /* Don't allow any blank lines between the `case` line and the first
     * statement in the case (or the next case if this case has no body). */
    seq.add(labelPiece, Indent::None, /*allowBlankAfter=*/false);

    for (const auto *s : c->statements) {
      seq.addNode(s, Indent::Block);
    }
  }

  seq.rightBracket(create<TextPiece>("}"));
  Piece *body = seq.build();

  auto *ctrl = create<ControlFlowPiece>();
  ctrl->add(header, body, /*isBlock=*/true);
  return ctrl;
}

Piece *PieceFactory::visit(const CaseNode *node) {
  /* The case label is built by visit(SwitchNode); this handles standalone
   * cases defensively. */
  Piece *labelPiece = nullptr;
  if (node->value) {
    labelPiece = create<ConcatPiece>(std::vector<const Piece *>{
        create<TextPiece>("case "), dispatchExpr(node->value),
        create<TextPiece>(":")});
  } else {
    labelPiece = create<TextPiece>("default:");
  }
  return labelPiece;
}

/* Modules */

Piece *PieceFactory::visit(const NamespaceDeclNode *node) {
  std::vector<const Piece *> parts;
  for (auto *ann : node->annotations) {
    parts.push_back(dispatch(ann));
  }

  std::string pfx = "namespace " + std::string(node->name);

  if (node->isFileScoped) {
    Piece *mainNs = create<TextPiece>(pfx + ";");

    if (parts.empty() && node->statements.empty())
      return mainNs;

    if (!node->statements.empty()) {
      SequenceBuilder seq(this);
      for (size_t i = 0; i < node->statements.size(); ++i) {
        bool blankBefore =
            i > 0 && hasNonEmptyBody(node->statements[i - 1]);
        seq.addNode(node->statements[i], Indent::None, blankBefore);
      }
      const Piece *body = prependAnnotations(this, parts, seq.build());
      return create<ConcatPiece>(std::vector<const Piece *>{
          mainNs, create<BlankLinePiece>(), body});
    }
    return prependAnnotations(this, parts, mainNs);
  }

  SequenceBuilder seq(this);
  seq.leftBracket(create<TextPiece>("{"));

  for (size_t i = 0; i < node->statements.size(); ++i) {
    bool blankBefore = i > 0 && hasNonEmptyBody(node->statements[i - 1]);
    seq.addNode(node->statements[i], Indent::None, blankBefore);
  }

  seq.rightBracket(create<TextPiece>("}"));
  Piece *body = seq.build();

  Piece *mainNs = create<ConcatPiece>(std::vector<const Piece *>{
      create<TextPiece>(pfx), create<TextPiece>(" "), body});

  return prependAnnotations(this, parts, mainNs);
}

Piece *PieceFactory::visit(const ModuleNode *node) {
  SequenceBuilder seq(this);

  if (!node->docString.empty()) {
    std::string doc = std::string(node->docString);
    int trailingNewlines = 0;
    while (!doc.empty() && doc.back() == '\n') {
      trailingNewlines++;
      doc.pop_back();
    }

    Whitespace trailing = Whitespace::Space;
    if (trailingNewlines > 1) {
      trailing = Whitespace::BlankLine;
    } else if (trailingNewlines == 1) {
      trailing = Whitespace::Newline;
    }

    if (!doc.empty()) {
      seq.add(create<CommentPiece>(doc, trailing), Indent::None,
              /*allowBlankAfter=*/false);
    }
  }

  /* With preprocessor directives present, the module is emitted in source
   * order: hoisting imports or sorting members would move code across
   * conditional branches and change what compiles. */
  bool hasDirectives = false;
  for (const auto &item : node->topLevelItems) {
    if (item.kind == ModuleNode::TopLevelItem::Kind::Directive) {
      hasDirectives = true;
      break;
    }
  }

  /* A file-scoped namespace absorbs the following top-level statements at
   * parse time, which would make the ordered path print them twice. */
  bool fileScoped =
      node->statements.size() == 1 &&
      llvm::isa_and_nonnull<NamespaceDeclNode>(node->statements[0]) &&
      static_cast<const NamespaceDeclNode *>(node->statements[0])
          ->isFileScoped;

  if (hasDirectives && !fileScoped && !node->topLevelItems.empty()) {
    const ASTNode *lastStmt = nullptr;
    int interveningItems = 0;
    for (const auto &item : node->topLevelItems) {
      if (item.kind == ModuleNode::TopLevelItem::Kind::Statement) {
        bool blankBefore = false;
        if (lastStmt) {
          if (hasNonEmptyBody(lastStmt)) {
            blankBefore = true;
          } else {
            /* Blank lines are preserved, discounting the lines occupied by
             * the imports/directives sitting between the statements. */
            int gap = getActualStartLine(item.node) -
                      getActualEndLine(lastStmt) - 1 - interveningItems;
            blankBefore = gap > 0;
          }
        }
        if (blankBefore)
          seq.addBlank();
        seq.add(statementPiece(item.node), Indent::None);
        lastStmt = item.node;
        interveningItems = 0;
      } else {
        std::string text;
        if (item.kind == ModuleNode::TopLevelItem::Kind::Import) {
          text = "import \"" + std::string(item.text) + "\";";
        } else if (item.kind == ModuleNode::TopLevelItem::Kind::Export) {
          text = "export \"" + std::string(item.text) + "\";";
        } else {
          text = "#" + std::string(item.text);
        }
        seq.add(create<TextPiece>(text), Indent::None, /*allowBlankAfter=*/true);
        interveningItems++;
      }
    }
    return seq.build();
  }

  for (auto imp : node->rawImports) {
    seq.add(create<TextPiece>("import \"" + std::string(imp) + "\";"));
  }
  for (auto exp : node->rawExports) {
    seq.add(create<TextPiece>("export \"" + std::string(exp) + "\";"));
  }

  if (!node->rawImports.empty() || !node->rawExports.empty()) {
    if (!node->statements.empty()) {
      seq.addBlank();
    }
  }

  for (size_t i = 0; i < node->statements.size(); ++i) {
    bool blankBefore = i > 0 && hasNonEmptyBody(node->statements[i - 1]);
    seq.addNode(node->statements[i], Indent::None, blankBefore);
  }

  return seq.build();
}

} // namespace utopia
