#include "LspCore.hpp"
#include <algorithm>

namespace utopia::lsp {

namespace {

/* Token type indices must match the legend advertised at initialize time. */
enum class SemTokenType : int {
  Class,
  Struct,
  Enum,
  Type,
  Function,
  Method,
  Property,
  Variable,
  Parameter,
  EnumMember,
  Macro,
  Keyword,
  Namespace,
};

/* LSP token modifier bits (bitmask): declaration = 1, static = 2, readonly =
 * 4. */
enum TokenModifier : int {
  ModDeclaration = 1 << 0,
  ModStatic = 1 << 1,
  ModReadonly = 1 << 2,
};

struct SemanticToken {
  int line;
  int col;
  int length;
  int type;
  int modifiers;

  bool operator<(const SemanticToken &other) const {
    if (line != other.line)
      return line < other.line;
    return col < other.col;
  }
};

class SemanticTokenVisitor : public ASTVisitor<SemanticTokenVisitor, void> {
public:
  std::vector<SemanticToken> tokens;
  const std::string &docText;
  ASTContext *astCtx;

  SemanticTokenVisitor(const std::string &text, ASTContext *ctx)
      : docText(text), astCtx(ctx) {}

  void addToken(int line, int col, int length, int type, int modifiers = 0) {
    if (line < 0 || col < 0 || length <= 0)
      return;
    tokens.push_back({line, col, length, type, modifiers});
  }

  /* Whether the document text at (1-based line, col) starts with 'name'.
   * Synthetic nodes rewritten by Sema (e.g. the implicit 'this' receiver of
   * a receiverless method call) carry the call's position instead of their
   * own text: they must not emit a token there, or they would overwrite the
   * real identifier's coloring. */
  bool textMatches(const ASTNode *n, std::string_view name) const {
    if (n->line <= 0 || n->column <= 0)
      return false;
    int currentLine = 1;
    size_t idx = 0;
    for (size_t i = 0; i < docText.length(); ++i) {
      if (currentLine == n->line) {
        idx = i + (n->column > 0 ? n->column - 1 : 0);
        break;
      }
      if (docText[i] == '\n')
        currentLine++;
    }
    if (idx + name.length() > docText.length())
      return false;
    return docText.compare(idx, name.length(), name) == 0;
  }

  /* Tokenizes a raw type string directly from the source code and precisely
   * targets valid type identifiers within it, ignoring generic brackets,
   * pointers or references. */
  void highlightTypeString(std::string_view rawTypeStr, int nodeLine,
                           int nodeCol) {
    if (rawTypeStr.empty() || !astCtx)
      return;

    int currentLine = 1;
    size_t searchStart = 0;
    for (size_t i = 0; i < docText.length(); ++i) {
      if (currentLine == nodeLine) {
        searchStart = i + (nodeCol > 0 ? nodeCol - 1 : 0);
        break;
      }
      if (docText[i] == '\n')
        currentLine++;
    }

    size_t foundIdx = docText.find(rawTypeStr, searchStart);
    if (foundIdx == std::string::npos || foundIdx > searchStart + 150) {
      foundIdx = searchStart;
    }

    Lexer lexer(rawTypeStr);
    auto tokensList = lexer.tokenize();
    for (size_t i = 0; i < tokensList.size(); ++i) {
      const auto &tok = tokensList[i];
      if (tok.type != TokenType::IDENTIFIER)
        continue;
      size_t tokAbsIdx = foundIdx + (tok.value.data() - rawTypeStr.data());

      int absLine = 1;
      int absCol = 1;
      for (size_t j = 0; j < tokAbsIdx; ++j) {
        if (docText[j] == '\n') {
          absLine++;
          absCol = 1;
        } else {
          absCol++;
        }
      }

      int tokenClass = (int)SemTokenType::Type;
      bool isNamespace = i + 1 < tokensList.size() &&
                         tokensList[i + 1].type == TokenType::DOT;
      if (isNamespace) {
        tokenClass = (int)SemTokenType::Namespace;
      } else {
        std::string chain;
        for (size_t k = 0; k <= i; ++k) {
          if (tokensList[k].type == TokenType::IDENTIFIER) {
            if (!chain.empty())
              chain += ".";
            chain += tokensList[k].value;
          }
        }

        if (astCtx->getRecordType(chain)) {
          auto *recTy = astCtx->getRecordType(chain);
          tokenClass = recTy->getKind() == TypeKind::Struct
                           ? (int)SemTokenType::Struct
                           : (int)SemTokenType::Class;
        } else if (astCtx->getEnumTypeByName(chain)) {
          tokenClass = (int)SemTokenType::Enum;
        } else if (astCtx->getNamespace(chain)) {
          tokenClass = (int)SemTokenType::Namespace;
        } else if (astCtx->getRecordType(tok.value)) {
          auto *recTy = astCtx->getRecordType(tok.value);
          tokenClass = recTy->getKind() == TypeKind::Struct
                           ? (int)SemTokenType::Struct
                           : (int)SemTokenType::Class;
        } else if (astCtx->getEnumTypeByName(tok.value)) {
          tokenClass = (int)SemTokenType::Enum;
        } else if (astCtx->getNamespace(tok.value)) {
          tokenClass = (int)SemTokenType::Namespace;
        }
      }

      addToken(absLine - 1, absCol - 1, tok.value.length(), tokenClass, 0);
    }
  }

  void visit(const NamespaceDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, (int)SemTokenType::Namespace, 0);
    for (auto *s : n->statements)
      dispatch(s);
  }

  void visit(const UsingNode *n) {
    highlightTypeString(n->name, n->line, n->column);
  }

  void visit(const TypeLiteralNode *n) {
    if (n->length <= 0)
      return;

    int currentLine = 1;
    size_t startIdx = 0;
    for (size_t i = 0; i < docText.length(); ++i) {
      if (currentLine == n->line) {
        startIdx = i + (n->column > 0 ? n->column - 1 : 0);
        break;
      }
      if (docText[i] == '\n')
        currentLine++;
    }

    if (startIdx < docText.length()) {
      std::string_view raw = std::string_view(
          docText.data() + startIdx,
          std::min((size_t)n->length, docText.length() - startIdx));
      highlightTypeString(raw, n->line, n->column);
    }
  }

  void visit(const VariableNode *n) {
    if (!n->resolvedDecl)
      return;

    /* Sema synthesizes a 'this' variable as the receiver of receiverless
     * method calls; its position is the call's, not the keyword's. Skipping
     * it when the text does not literally say 'this' prevents it from
     * overwriting the method name's token. */
    if (n->name == "this" && !textMatches(n, "this"))
      return;

    int type = (int)SemTokenType::Variable;
    int mods = 0;

    switch (n->resolvedDecl->kind) {
    case NodeKind::ClassDecl:
      type = (int)SemTokenType::Class;
      break;
    case NodeKind::StructDecl:
      type = (int)SemTokenType::Struct;
      break;
    case NodeKind::EnumDecl:
      type = (int)SemTokenType::Enum;
      break;
    case NodeKind::UnionDecl:
    case NodeKind::TypedefDecl:
    case NodeKind::AnnotationDecl:
      type = (int)SemTokenType::Type;
      break;
    case NodeKind::NamespaceDecl:
      type = (int)SemTokenType::Namespace;
      break;
    case NodeKind::FunctionDecl: {
      auto fn = static_cast<const FunctionDeclNode *>(n->resolvedDecl);
      if (fn->parentRecord && fn->name == fn->parentRecord->getName()) {
        /* A constructor reference types as its record. */
        const DeclNode *recDecl = fn->parentRecord->getDeclaration();
        if (recDecl) {
          if (recDecl->kind == NodeKind::StructDecl)
            type = (int)SemTokenType::Struct;
          else if (recDecl->kind == NodeKind::UnionDecl)
            type = (int)SemTokenType::Type;
          else
            type = (int)SemTokenType::Class;
        } else {
          type = (int)SemTokenType::Class;
        }
      } else {
        type = (int)SemTokenType::Function;
      }
      break;
    }
    case NodeKind::ParamDecl:
      type = (int)SemTokenType::Parameter;
      break;
    case NodeKind::VarDecl:
      if (static_cast<const VarDeclNode *>(n->resolvedDecl)->isStatic)
        mods |= ModStatic;
      break;
    default:
      break;
    }

    int trueLen = std::min(n->length, (int)n->name.length());
    addToken(n->line > 0 ? n->line - 1 : 0, n->column > 0 ? n->column - 1 : 0,
             trueLen, type, mods);
  }

  void visit(const MemberAccessNode *n) {
    dispatch(n->object);
    int type = (int)SemTokenType::Property;
    int mods = 0;
    if (n->isMethodRef)
      type = (int)SemTokenType::Method;
    else if (n->isEnumMember)
      type = (int)SemTokenType::EnumMember;
    else if (n->isStaticFieldRef) {
      type = (int)SemTokenType::Property;
      mods |= ModStatic;
    } else if (n->resolvedDecl) {
      switch (n->resolvedDecl->kind) {
      case NodeKind::ClassDecl:
        type = (int)SemTokenType::Class;
        break;
      case NodeKind::StructDecl:
        type = (int)SemTokenType::Struct;
        break;
      case NodeKind::EnumDecl:
        type = (int)SemTokenType::Enum;
        break;
      case NodeKind::UnionDecl:
      case NodeKind::TypedefDecl:
      case NodeKind::AnnotationDecl:
        type = (int)SemTokenType::Type;
        break;
      case NodeKind::NamespaceDecl:
        type = (int)SemTokenType::Namespace;
        break;
      case NodeKind::FunctionDecl: {
        auto fn = static_cast<const FunctionDeclNode *>(n->resolvedDecl);
        if (fn->parentRecord && fn->name == fn->parentRecord->getName()) {
          const DeclNode *recDecl = fn->parentRecord->getDeclaration();
          if (recDecl) {
            if (recDecl->kind == NodeKind::StructDecl)
              type = (int)SemTokenType::Struct;
            else if (recDecl->kind == NodeKind::UnionDecl)
              type = (int)SemTokenType::Type;
            else
              type = (int)SemTokenType::Class;
          } else {
            type = (int)SemTokenType::Class;
          }
        } else {
          type = (int)SemTokenType::Function;
        }
        break;
      }
      default:
        break;
      }
    }

    int memberCol = (n->column > 0 ? n->column - 1 : 0) + n->length -
                    n->memberName.length();
    addToken(n->line > 0 ? n->line - 1 : 0, memberCol, n->memberName.length(),
             type, mods);
  }

  void visit(const FunctionCallNode *n) {
    dispatch(n->target);
    for (auto *arg : n->args)
      dispatch(arg);
  }

  void visit(const VarDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, (int)SemTokenType::Variable,
             n->isStatic ? ModStatic : 0);

    if (!n->rawTypeStr.empty()) {
      highlightTypeString(n->rawTypeStr, n->line, n->column);
    }

    if (n->initializer)
      dispatch(n->initializer);
  }

  void visit(const ParamDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, (int)SemTokenType::Parameter, 0);

    if (!n->rawTypeStr.empty()) {
      highlightTypeString(n->rawTypeStr, n->line, n->column);
    }

    if (n->defaultValue)
      dispatch(n->defaultValue);
  }

  void visit(const FunctionDeclNode *n) {
    if (n->isImplicit)
      return;

    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length,
             n->isMethod ? (int)SemTokenType::Method : (int)SemTokenType::Function,
             n->isStatic ? ModStatic : 0);

    if (!n->rawReturnTypeStr.empty()) {
      highlightTypeString(n->rawReturnTypeStr, n->line, n->column);
    }

    for (auto *p : n->params)
      dispatch(p);
    if (n->body)
      dispatch(n->body);
  }

  void visit(const ClassDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, (int)SemTokenType::Class, 0);
    for (auto *f : n->fields)
      dispatch(f);
    for (auto *m : n->methods)
      dispatch(m);
    for (auto *c : n->constructors)
      dispatch(c);
    if (n->destructor)
      dispatch(n->destructor);
  }

  void visit(const StructDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, (int)SemTokenType::Struct, 0);
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
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, (int)SemTokenType::Type, 0);
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
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, (int)SemTokenType::Enum, 0);
    for (auto *m : n->members)
      dispatch(m);
  }

  void visit(const EnumMemberNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, (int)SemTokenType::EnumMember, 0);
    if (n->initializer)
      dispatch(n->initializer);
  }

  void visit(const TypedefDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, (int)SemTokenType::Type, 0);

    if (!n->rawTargetTypeStr.empty()) {
      highlightTypeString(n->rawTargetTypeStr, n->line, n->column);
    }
  }

  void visit(const AnnotationDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, (int)SemTokenType::Class, 0);
    for (auto *f : n->fields)
      dispatch(f);
    if (n->constructor)
      dispatch(n->constructor);
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
      if (!clause->isCatchAll) {
        highlightTypeString(clause->rawTypeStr, clause->line,
                            clause->column + 7);
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
    if (n->isExpressionBody && n->exprBody) {
      dispatch(n->exprBody);
    } else if (n->body) {
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
  void visit(const CastNode *n) {
    dispatch(n->expr);
    if (!n->rawTargetTypeStr.empty()) {
      highlightTypeString(n->rawTargetTypeStr, n->line, n->column);
    }
  }
  void visit(const IsExprNode *n) {
    dispatch(n->expr);
    if (!n->rawTargetTypeStr.empty()) {
      highlightTypeString(n->rawTargetTypeStr, n->line, n->column);
    }
  }
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
    if (!n->rawAllocatedTypeStr.empty()) {
      highlightTypeString(n->rawAllocatedTypeStr, n->line, n->column);
    }
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
  void visit(const ModuleNode *n) {
    for (auto *s : n->statements)
      dispatch(s);
  }

  void visit(const BreakNode *) {}
  void visit(const ContinueNode *) {}
  void visit(const NumberNode *) {}
  void visit(const BoolNode *) {}
  void visit(const CharNode *) {}
  void visit(const RuneNode *) {}
  void visit(const StringNode *) {}
  void visit(const NullNode *) {}
};

} // namespace

void handleSemanticTokens(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  json data = json::array();

  DocumentState doc;
  if (documents.get(uri, doc) && doc.ast) {
    SemanticTokenVisitor visitor(doc.text, doc.astCtx.get());
    visitor.dispatch(doc.ast);

    /* Lexical pass to inject semantic tokens for primitive types and
     * keywords. The AST visitor skips built-in types because they lack a
     * concrete declaration node; the lexer covers them plus the keywords
     * the grammar-based highlighting would otherwise rely on. */
    Lexer lexer(doc.text);
    for (const auto &tok : lexer.tokenize()) {
      int type = -1;
      if (tok.type == TokenType::TYPE_KW) {
        type = (int)SemTokenType::Type;
      } else if (tok.type == TokenType::VAR_KW ||
                 tok.type == TokenType::CONST_KW ||
                 tok.type == TokenType::STATIC_KW ||
                 tok.type == TokenType::PUBLIC_KW ||
                 tok.type == TokenType::PRIVATE_KW ||
                 tok.type == TokenType::PROTECTED_KW ||
                 tok.type == TokenType::ABSTRACT_KW ||
                 tok.type == TokenType::REQUIRED_KW ||
                 tok.type == TokenType::NAMESPACE_KW ||
                 tok.type == TokenType::EXTENDS_KW ||
                 tok.type == TokenType::IMPLEMENTS_KW ||
                 tok.type == TokenType::SUPER_KW ||
                 tok.type == TokenType::USING_KW ||
                 tok.type == TokenType::THIS_KW ||
                 tok.type == TokenType::FINAL_KW ||
                 tok.type == TokenType::IF_KW || tok.type == TokenType::ELSE_KW ||
                 tok.type == TokenType::FOR_KW || tok.type == TokenType::WHILE_KW ||
                 tok.type == TokenType::SWITCH_KW || tok.type == TokenType::CASE_KW ||
                 tok.type == TokenType::DEFAULT_KW || tok.type == TokenType::BREAK_KW ||
                 tok.type == TokenType::CONTINUE_KW || tok.type == TokenType::RETURN ||
                 tok.type == TokenType::NEW_KW || tok.type == TokenType::DELETE_KW ||
                 tok.type == TokenType::TRUE_KW || tok.type == TokenType::FALSE_KW ||
                 tok.type == TokenType::NULL_KW || tok.type == TokenType::ASYNC_KW ||
                 tok.type == TokenType::AWAIT_KW || tok.type == TokenType::TRY_KW ||
                 tok.type == TokenType::CATCH_KW || tok.type == TokenType::THROW_KW ||
                 tok.type == TokenType::ASSERT_KW ||
                 tok.type == TokenType::STRUCT_KW ||
                 tok.type == TokenType::UNION_KW ||
                 tok.type == TokenType::CLASS_KW ||
                 tok.type == TokenType::ENUM_KW ||
                 tok.type == TokenType::TYPEDEF_KW ||
                 tok.type == TokenType::ANNOTATION_KW) {
        type = (int)SemTokenType::Keyword;
      }
      if (type < 0)
        continue;
      visitor.addToken(tok.line > 0 ? tok.line - 1 : 0,
                       tok.column > 0 ? tok.column - 1 : 0,
                       tok.value.length(), type, 0);
    }

    std::vector<SemanticToken> tokens = std::move(visitor.tokens);
    std::sort(tokens.begin(), tokens.end());

    int prevLine = 0;
    int prevCol = 0;

    for (const auto &tok : tokens) {
      int deltaLine = tok.line - prevLine;
      int deltaCol = (deltaLine == 0) ? (tok.col - prevCol) : tok.col;

      /* Prevent negative deltas that could crash the VS Code LSP client
       * in the rare event of duplicate tokens at the exact same location. */
      if (deltaLine < 0 || (deltaLine == 0 && deltaCol < 0)) {
        continue;
      }

      data.push_back(deltaLine);
      data.push_back(deltaCol);
      data.push_back(tok.length);
      data.push_back(tok.type);
      data.push_back(tok.modifiers);

      prevLine = tok.line;
      prevCol = tok.col;
    }
  }

  sendResponse(
      {{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", {{"data", data}}}});
}

} // namespace utopia::lsp
