#include "utopia/Parser/Parser.hpp"
#include "utopia/Driver/ModuleLoader.hpp"
#include <filesystem>

namespace utopia {

const Token &Parser::currentToken() const {
  if (cursor >= tokens.size())
    return tokens.back();
  return tokens[cursor];
}

const Token &Parser::peekToken(size_t offset) const {
  if (cursor + offset >= tokens.size())
    return tokens.back();
  return tokens[cursor + offset];
}

void Parser::advance() {
  if (cursor < tokens.size())
    cursor++;
}

bool Parser::match(TokenType type) {
  if (currentToken().type == type) {
    advance();
    return true;
  }
  return false;
}

void Parser::synchronize() {
  advance();
  while (currentToken().type != TokenType::EOF_TOK) {
    if (tokens[cursor - 1].type == TokenType::SEMICOLON)
      return;
    switch (currentToken().type) {
    case TokenType::TYPE_KW:
    case TokenType::RETURN:
    case TokenType::LBRACE:
      return;
    default:
      advance();
    }
  }
}

void Parser::expect(TokenType type, std::string_view errorMessage) {
  if (!match(type)) {
    reportError(currentToken().line, currentToken().column,
                (int)currentToken().value.length(),
                std::string(errorMessage) + " Found: '" +
                    std::string(currentToken().value) + "'");
    throw ParseException();
  }
}

std::string_view Parser::parseOperatorName() {
  static const std::unordered_map<TokenType, std::string_view> opMap = {
      {TokenType::LBRACKET, "[]"},
      {TokenType::PLUS, "+"},
      {TokenType::MINUS, "-"},
      {TokenType::STAR, "*"},
      {TokenType::SLASH, "/"},
      {TokenType::PERCENT, "%"},
      {TokenType::EQ, "=="},
      {TokenType::NEQ, "!="},
      {TokenType::LT, "<"},
      {TokenType::GT, ">"},
      {TokenType::LE, "<="},
      {TokenType::GE, ">="},
      {TokenType::ASSIGN, "="},
      {TokenType::PLUS_EQ, "+="},
      {TokenType::MINUS_EQ, "-="},
      {TokenType::STAR_EQ, "*="},
      {TokenType::SLASH_EQ, "/="},
      {TokenType::PERCENT_EQ, "%="},
      {TokenType::PLUS_PLUS, "++"},
      {TokenType::MINUS_MINUS, "--"},
      {TokenType::PIPE, "|"},
      {TokenType::AMPERSAND, "&"},
      {TokenType::CARET, "^"},
      {TokenType::LSHIFT, "<<"},
      {TokenType::RSHIFT, ">>"},
      {TokenType::PIPE_EQ, "|="},
      {TokenType::AMPERSAND_EQ, "&="},
      {TokenType::CARET_EQ, "^="},
      {TokenType::LSHIFT_EQ, "<<="},
      {TokenType::RSHIFT_EQ, ">>="},
      {TokenType::BANG, "!"},
      {TokenType::TILDE, "~"}};

  if (currentToken().type == TokenType::NEW_KW ||
      currentToken().type == TokenType::DELETE_KW) {
    std::string_view allocName = (currentToken().type == TokenType::NEW_KW)
                                     ? "operator new"
                                     : "operator delete";
    advance();
    return astCtx.copyString(allocName);
  }

  auto it = opMap.find(currentToken().type);
  if (it == opMap.end()) {
    reportError(currentToken().line, currentToken().column,
                currentToken().value.length(),
                "Invalid operator for overloading");
    throw ParseException();
  }

  std::string_view opStr = it->second;

  if (it->first == TokenType::LBRACKET) {
    advance();
    if (currentToken().type != TokenType::RBRACKET) {
      reportError(currentToken().line, currentToken().column,
                  currentToken().value.length(),
                  "Expected ']' after '[' for operator[]");
      throw ParseException();
    }
  }

  advance();
  std::string name = "operator" + std::string(opStr);
  return astCtx.copyString(name);
}

bool Parser::isKnownTypeName(std::string_view name) const {
  return astCtx.getBuiltinTypeByName(name) || astCtx.getRecordType(name) ||
         astCtx.getTypeAlias(name) || astCtx.getEnumTypeByName(name) ||
         astCtx.getVectorTypeByName(name);
}

TemplateConstraint Parser::parseTemplateConstraint() {
  /* 'extends' is current. A pseudo-type name wins unless a real type with
   * that name exists (a user class named 'Number' is a class constraint). */
  if (currentToken().type == TokenType::IDENTIFIER) {
    std::string_view cn = currentToken().value;
    if (!isKnownTypeName(cn)) {
      if (cn == "Object") {
        advance();
        return TemplateConstraint(TemplateConstraintKind::Object);
      }
      if (cn == "Record") {
        advance();
        return TemplateConstraint(TemplateConstraintKind::Record);
      }
      if (cn == "Number") {
        advance();
        return TemplateConstraint(TemplateConstraintKind::Number);
      }
      if (cn == "Integer") {
        advance();
        return TemplateConstraint(TemplateConstraintKind::Integer);
      }
      if (cn == "FloatingPoint") {
        advance();
        return TemplateConstraint(TemplateConstraintKind::FloatingPoint);
      }
      if (cn == "String") {
        /* 'T extends String' is a plain class constraint, just like any
         * other prelude class. */
      }
    }
  }
  return TemplateConstraint(parseType());
}

std::string Parser::mangleTemplateName(const std::string &baseFqName,
                                       llvm::ArrayRef<const Type *> args) const {
  std::string mangled = baseFqName;
  for (const auto *arg : args) {
    std::string argStr = arg->toString();
    for (char &c : argStr) {
      if (!isalnum((unsigned char)c))
        c = '_';
    }
    mangled += "_" + argStr;
  }
  return mangled;
}

bool Parser::isDeclaration() {
  if (currentToken().type == TokenType::CONST_KW ||
      currentToken().type == TokenType::VAR_KW ||
      currentToken().type == TokenType::STATIC_KW ||
      currentToken().type == TokenType::FINAL_KW ||
      currentToken().type == TokenType::TYPE_KW) {
    return true;
  }

  if (currentToken().type == TokenType::IDENTIFIER) {
    size_t offset = 0;
    std::string typeNameStr = std::string(peekToken(offset).value);

    while (peekToken(offset + 1).type == TokenType::DOT &&
           peekToken(offset + 2).type == TokenType::IDENTIFIER) {
      typeNameStr += ".";
      typeNameStr += peekToken(offset + 2).value;
      offset += 2;
    }

    if (peekToken(offset + 1).type == TokenType::LT) {
      int bracketDepth = 0;
      size_t tempOffset = offset + 1;
      while (peekToken(tempOffset).type != TokenType::EOF_TOK) {
        if (peekToken(tempOffset).type == TokenType::LT)
          bracketDepth++;
        else if (peekToken(tempOffset).type == TokenType::GT)
          bracketDepth--;
        else if (peekToken(tempOffset).type == TokenType::RSHIFT)
          bracketDepth -= 2;

        tempOffset++;
        if (bracketDepth <= 0)
          break;
      }
      offset = tempOffset - 1;
    }

    TokenType nextTok = peekToken(offset + 1).type;

    if (nextTok == TokenType::IDENTIFIER || nextTok == TokenType::OPERATOR_KW) {
      return true;
    }

    /* 'Name Function(...)' is a function pointer type declaration (e.g.
     * 'String Function() f = ...'); 'Function' is a keyword so this cannot
     * be an expression. */
    if (nextTok == TokenType::FUNCTION_KW) {
      return true;
    }

    if (nextTok == TokenType::AMPERSAND || nextTok == TokenType::STAR ||
        nextTok == TokenType::LBRACKET || nextTok == TokenType::LOGICAL_AND) {

      auto resolveType = [&](std::string_view name) -> bool {
        return astCtx.getRecordType(name) || astCtx.getTypeAlias(name) ||
               astCtx.getEnumTypeByName(name) || isTemplateParam(name) ||
               astCtx.getBuiltinTypeByName(name);
      };

      if (resolveType(typeNameStr))
        return true;

      std::string ns = getCurrentNamespace();
      while (!ns.empty()) {
        if (resolveType(ns + "." + typeNameStr))
          return true;
        size_t pos = ns.find_last_of('.');
        if (pos != std::string::npos)
          ns = ns.substr(0, pos);
        else
          break;
      }

      /* Usings may qualify types declared in imported modules. */
      for (const auto &u : activeUsings) {
        if (resolveType(u + "." + typeNameStr))
          return true;
      }
    }
  }
  return false;
}

const Type *Parser::parseTypeModifiers(const Type *baseType, bool inNewExpr,
                                       bool allowRValueRef) {
  const Type *ty = baseType;
  while (currentToken().type == TokenType::STAR ||
         currentToken().type == TokenType::AMPERSAND ||
         (allowRValueRef && currentToken().type == TokenType::LOGICAL_AND) ||
         currentToken().type == TokenType::CONST_KW ||
         (!inNewExpr && currentToken().type == TokenType::LBRACKET)) {
    if (currentToken().type == TokenType::CONST_KW) {
      ty = astCtx.getConstType(ty);
      advance();
    } else if (currentToken().type == TokenType::STAR) {
      ty = astCtx.getPointerType(ty);
      advance();
    } else if (currentToken().type == TokenType::AMPERSAND) {
      ty = astCtx.getReferenceType(ty);
      advance();
    } else if (allowRValueRef && currentToken().type == TokenType::LOGICAL_AND) {
      ty = astCtx.getRValueReferenceType(ty);
      advance();
    } else if (!inNewExpr && currentToken().type == TokenType::LBRACKET) {
      ty = applyArrayDeclarator(ty);
    }
  }
  return ty;
}

const Type *Parser::parseType(bool inNewExpr, bool allowRValueRef) {
  bool isConst = match(TokenType::CONST_KW);

  if (currentToken().type != TokenType::TYPE_KW &&
      currentToken().type != TokenType::IDENTIFIER) {
    reportError(currentToken().line, currentToken().column,
                (int)currentToken().value.length(), "Expected type name");
    throw ParseException();
  }

  std::string typeNameStr = std::string(currentToken().value);
  advance();

  while (currentToken().type == TokenType::DOT &&
         peekToken().type == TokenType::IDENTIFIER) {
    advance();
    typeNameStr += ".";
    typeNameStr += currentToken().value;
    advance();
  }

  std::string_view base = astCtx.copyString(typeNameStr);
  const Type *ty = nullptr;

  if (isTemplateParam(base)) {
    ty = astCtx.getTemplateParamType(base);
  } else {
    auto resolveType = [&](std::string_view name) -> const Type * {
      const Type *t = astCtx.getBuiltinTypeByName(name);
      if (!t)
        t = astCtx.getVectorTypeByName(name);
      if (!t)
        t = astCtx.getRecordType(name);
      if (!t)
        t = astCtx.getTypeAlias(name);
      if (!t)
        t = astCtx.getEnumTypeByName(name);
      return t;
    };

    ty = resolveType(base);

    if (!ty) {
      std::string ns = getCurrentNamespace();
      while (!ns.empty()) {
        ty = resolveType(ns + "." + std::string(base));
        if (ty)
          break;
        size_t pos = ns.find_last_of('.');
        if (pos != std::string::npos) {
          ns = ns.substr(0, pos);
        } else {
          break;
        }
      }
    }

    /* Usings may qualify types declared in imported modules. */
    if (!ty) {
      for (const auto &u : activeUsings) {
        ty = resolveType(u + "." + std::string(base));
        if (ty)
          break;
      }
    }
  }

  if (!ty) {
    ty = astCtx.getTemplateParamType(base);
  }

  if (currentToken().type == TokenType::LT) {
    advance();
    std::vector<const Type *> tArgs;
    if (currentToken().type != TokenType::GT) {
      do {
        if (currentToken().type == TokenType::GT ||
            currentToken().type == TokenType::RSHIFT)
          break;
        tArgs.push_back(parseType());
      } while (match(TokenType::COMMA));
    }

    if (currentToken().type == TokenType::RSHIFT) {
      const_cast<Token &>(currentToken()).type = TokenType::GT;
      /* Keep the original view: it spans both '>' characters, so raw type
       * strings computed from this token still cover the full '>>'. */
    } else {
      expect(TokenType::GT, "Expected '>'");
    }
    ty =
        astCtx.getTemplateInstType(base, astCtx.copyArray<const Type *>(tArgs));
  }

  if (isConst) {
    ty = astCtx.getConstType(ty);
  }

  ty = parseTypeModifiers(ty, inNewExpr, allowRValueRef);

  if (match(TokenType::FUNCTION_KW)) {
    expect(TokenType::LPAREN, "Expected '(' after 'Function'");
    std::vector<const Type *> paramTypes;
    if (currentToken().type != TokenType::RPAREN &&
        currentToken().type != TokenType::EOF_TOK) {
      do {
        if (currentToken().type == TokenType::RPAREN)
          break;
        paramTypes.push_back(parseType());
      } while (match(TokenType::COMMA));
    }
    expect(TokenType::RPAREN, "Expected ')' after function parameters");

    ty = astCtx.getFunctionType(ty, astCtx.copyArray<const Type *>(paramTypes));
    ty = astCtx.getPointerType(ty);

    ty = parseTypeModifiers(ty, inNewExpr, allowRValueRef);
  }

  return ty;
}

std::string Parser::consumeComments() {
  std::string doc;
  for (size_t i = 0; i < currentToken().leadingComments.size(); ++i) {
    auto c = currentToken().leadingComments[i];
    if (i > 0) {
      auto prev = currentToken().leadingComments[i - 1];
      const char *gapStart = prev.data() + prev.length();
      const char *gapEnd = c.data();
      if (gapEnd != nullptr && gapStart <= gapEnd) {
        int newlines = 0;
        for (const char *p = gapStart; p < gapEnd; ++p) {
          if (*p == '\n')
            newlines++;
        }
        if (newlines == 0)
          newlines = 1;
        doc += std::string(newlines, '\n');
      } else {
        doc += "\n";
      }
    }
    doc += c;
  }

  if (!currentToken().leadingComments.empty()) {
    auto last = currentToken().leadingComments.back();
    const char *gapStart = last.data() + last.length();
    const char *gapEnd = currentToken().value.data();
    if (gapEnd != nullptr && gapStart <= gapEnd) {
      int newlines = 0;
      for (const char *p = gapStart; p < gapEnd; ++p) {
        if (*p == '\n')
          newlines++;
      }
      doc += std::string(newlines, '\n');
    }
  }

  const_cast<Token &>(currentToken()).leadingComments.clear();
  return doc;
}

/* Whether a comment token is a preprocessor directive line ('#...'). */
static bool isDirectiveComment(std::string_view c) {
  return !c.empty() && c[0] == '#';
}

std::string Parser::consumeModuleComments(
    std::vector<ModuleNode::TopLevelItem> &items,
    std::string *preDirectiveDoc) {
  std::string doc;
  bool pastDirective = false;

  auto target = [&]() -> std::string & {
    return (preDirectiveDoc && !pastDirective) ? *preDirectiveDoc : doc;
  };

  for (size_t i = 0; i < currentToken().leadingComments.size(); ++i) {
    auto c = currentToken().leadingComments[i];

    if (isDirectiveComment(c)) {
      /* The gap before the first directive belongs to the leading comment
       * block (e.g. the module header): preserve the blank lines the user
       * wrote between it and the directive. */
      if (!pastDirective && preDirectiveDoc && i > 0) {
        auto prev = currentToken().leadingComments[i - 1];
        const char *gapStart = prev.data() + prev.length();
        const char *gapEnd = c.data();
        if (gapEnd != nullptr && gapStart <= gapEnd) {
          int newlines = 0;
          for (const char *p = gapStart; p < gapEnd; ++p) {
            if (*p == '\n')
              newlines++;
          }
          *preDirectiveDoc += std::string(newlines, '\n');
        }
      }

      ModuleNode::TopLevelItem item;
      item.kind = ModuleNode::TopLevelItem::Kind::Directive;
      std::string_view text = c.substr(1);
      while (!text.empty() &&
             (text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
      }
      item.text = astCtx.copyString(text);
      items.push_back(item);
      pastDirective = true;
      continue;
    }

    std::string &out = target();

    if (i > 0) {
      auto prev = currentToken().leadingComments[i - 1];
      const char *gapStart = prev.data() + prev.length();
      const char *gapEnd = c.data();
      if (gapEnd != nullptr && gapStart <= gapEnd) {
        int newlines = 0;
        for (const char *p = gapStart; p < gapEnd; ++p) {
          if (*p == '\n')
            newlines++;
        }
        if (newlines == 0)
          newlines = 1;
        out += std::string(newlines, '\n');
      } else {
        out += "\n";
      }
    }
    out += c;
  }

  if (!currentToken().leadingComments.empty()) {
    auto last = currentToken().leadingComments.back();
    const char *gapStart = last.data() + last.length();
    const char *gapEnd = currentToken().value.data();
    if (gapEnd != nullptr && gapStart <= gapEnd) {
      int newlines = 0;
      for (const char *p = gapStart; p < gapEnd; ++p) {
        if (*p == '\n')
          newlines++;
      }
      target() += std::string(newlines, '\n');
    } else {
      target() += "\n";
    }
  }

  const_cast<Token &>(currentToken()).leadingComments.clear();
  return doc;
}

ModuleNode *Parser::parseModule(std::string_view filePath) {
  auto module = astCtx.create<ModuleNode>(filePath);
  std::vector<std::string_view> imports;
  std::vector<std::string_view> exports;
  std::vector<ModuleNode::DirectiveInfo> importInfo;
  std::vector<ModuleNode::DirectiveInfo> exportInfo;
  std::vector<ModuleNode::TopLevelItem> topLevelItems;
  std::vector<ASTNode *> statements;
  std::string moduleDoc;

  NamespaceDeclNode *fileScopedNs = nullptr;

  while (currentToken().type != TokenType::EOF_TOK) {
    try {
      if (currentToken().type == TokenType::IDENTIFIER &&
          (currentToken().value == "import" ||
           currentToken().value == "export")) {

        std::string importDoc = consumeModuleComments(topLevelItems,
                                                   &moduleDoc);

        bool isExport = (currentToken().value == "export");
        int kwLine = currentToken().line;
        int kwCol = currentToken().column;
        advance();

        if (currentToken().type != TokenType::STRING_LITERAL) {
          reportError(
              currentToken().line, currentToken().column,
              (int)currentToken().value.length(),
              isExport
                  ? "Expected string literal for module path after 'export'"
                  : "Expected string literal for module path after 'import'");
          throw ParseException();
        }

        int pathLine = currentToken().line;
        int pathCol = currentToken().column;
        std::string_view path = currentToken().value;
        int pathLen = path.length();

        advance();
        if (currentToken().type != TokenType::SEMICOLON) {
          expect(TokenType::SEMICOLON, "Expected ';' after statement");
        }
        int semiLine = currentToken().line;
        int semiCol = currentToken().column;
        advance();

        ModuleNode::DirectiveInfo info;
        info.path = path;
        info.line = kwLine;
        info.column = kwCol;
        info.endLine = semiLine;
        info.endColumn = semiCol + 1;

        if (isExport) {
          exports.push_back(path);
          exportInfo.push_back(info);
        } else {
          imports.push_back(path);
          importInfo.push_back(info);
        }

        ModuleNode::TopLevelItem item;
        item.kind = isExport ? ModuleNode::TopLevelItem::Kind::Export
                             : ModuleNode::TopLevelItem::Kind::Import;
        item.text = path;
        /* The leading comments keep their raw gaps: the formatter maps the
         * trailing newlines to the whitespace after the comment block. */
        item.doc = astCtx.copyString(importDoc);
        topLevelItems.push_back(item);

        /* Synchronously invoke the module loader upon evaluating an
         * import/export directive. This pre-populates the ASTContext with
         * available foreign types and guarantees downstream identifier
         * resolution.
         */
        if (moduleLoader) {
          std::filesystem::path currentDir =
              std::filesystem::path(filePath).parent_path();
          ModuleNode *loaded =
              moduleLoader->loadModule(std::string(path), currentDir, pathLine,
                                       pathCol, pathLen, filePath);
          if (loaded) {
            if (isExport) {
              exportInfo.back().resolvedModule = loaded;
            } else {
              importInfo.back().resolvedModule = loaded;
            }
          }
        }

      } else if (currentToken().type == TokenType::NAMESPACE_KW) {
        std::string doc =
            consumeModuleComments(topLevelItems, &moduleDoc);
        bool isFileScoped = false;
        auto ns = parseNamespaceDecl(isFileScoped);
        if (!doc.empty()) {
          ns->docString = astCtx.copyString(doc);
        }
        if (isFileScoped) {
          if (fileScopedNs) {
            reportError(ns->line, ns->column, ns->length,
                        "Only one file-scoped namespace is allowed.");
          } else {
            fileScopedNs = ns;
            statements.push_back(ns);
            namespaceStack.push_back(std::string(ns->name));
          }
        } else {
          statements.push_back(ns);
        }
        topLevelItems.push_back(
            {ModuleNode::TopLevelItem::Kind::Statement, {}, ns});
      } else if (currentToken().type == TokenType::USING_KW) {
        std::string doc =
            consumeModuleComments(topLevelItems, &moduleDoc);
        auto u = parseUsing();
        if (!doc.empty()) {
          u->docString = astCtx.copyString(doc);
        }
        statements.push_back(u);
        topLevelItems.push_back(
            {ModuleNode::TopLevelItem::Kind::Statement, {}, u});
      } else if (currentToken().type != TokenType::EOF_TOK) {
        if (currentToken().type == TokenType::RBRACE) {
          reportError(currentToken().line, currentToken().column, 1,
                      "Stray '}' in module scope");
          advance();
          continue;
        }
        std::string doc =
            consumeModuleComments(topLevelItems, &moduleDoc);
        auto stmt = parseStatement();
        if (stmt) {
          /* A doc that is only the trailing gap to the statement (the
           * directive's line terminator) is not comment content. */
          std::string docTrimmed = doc;
          while (!docTrimmed.empty() && docTrimmed.back() == '\n')
            docTrimmed.pop_back();
          if (!docTrimmed.empty() && stmt->docString.empty()) {
            stmt->docString = astCtx.copyString(doc);
          }
          statements.push_back(stmt);
          topLevelItems.push_back(
              {ModuleNode::TopLevelItem::Kind::Statement, {}, stmt});
        }
      }
    } catch (const ParseException &) {
      synchronize();
    }
  }

  std::string eofDoc =
      consumeModuleComments(topLevelItems, &moduleDoc);
  std::string eofDocTrimmed = eofDoc;
  while (!eofDocTrimmed.empty() && eofDocTrimmed.back() == '\n')
    eofDocTrimmed.pop_back();
  if (!eofDocTrimmed.empty()) {
    if (!statements.empty()) {
      ASTNode *lastStmt = statements.back();
      std::string combined;
      if (!lastStmt->trailingComment.empty()) {
        combined = std::string(lastStmt->trailingComment) + "\n" + eofDoc;
      } else {
        combined = "\n" + eofDoc;
      }
      lastStmt->trailingComment = astCtx.copyString(combined);
    } else {
      if (moduleDoc.empty()) {
        moduleDoc = eofDoc;
      } else {
        moduleDoc += eofDoc;
      }
    }
  }

  if (fileScopedNs) {
    namespaceStack.pop_back();
    auto it = std::find(statements.begin(), statements.end(), fileScopedNs);
    if (it != statements.end()) {
      std::vector<ASTNode *> inner(it + 1, statements.end());
      fileScopedNs->statements = astCtx.copyArray<ASTNode *>(inner);
      statements.erase(it + 1, statements.end());
    }
  }

  if (!moduleDoc.empty()) {
    module->docString = astCtx.copyString(moduleDoc);
  }

  module->rawImports = astCtx.copyArray<std::string_view>(imports);
  module->rawExports = astCtx.copyArray<std::string_view>(exports);
  module->importInfo = astCtx.copyArray<ModuleNode::DirectiveInfo>(importInfo);
  module->exportInfo = astCtx.copyArray<ModuleNode::DirectiveInfo>(exportInfo);
  module->topLevelItems =
      astCtx.copyArray<ModuleNode::TopLevelItem>(topLevelItems);
  module->statements = astCtx.copyArray<ASTNode *>(statements);
  return module;
}

llvm::ArrayRef<AnnotationNode *> Parser::parseAnnotations() {
  std::vector<AnnotationNode *> annotations;
  while (currentToken().type == TokenType::AT) {
    annotations.push_back(parseAnnotation());
  }
  return astCtx.copyArray<AnnotationNode *>(annotations);
}

AnnotationNode *Parser::parseAnnotation() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  std::string_view name = currentToken().value;
  if (currentToken().type == TokenType::IDENTIFIER ||
      currentToken().type == TokenType::TYPE_KW ||
      currentToken().type == TokenType::CONST_KW ||
      currentToken().type == TokenType::ANNOTATION_KW) {
    advance();
  } else {
    expect(TokenType::IDENTIFIER, "Expected annotation name");
  }

  std::vector<ExprNode *> args;
  bool hasTrailingComma = false;

  if (match(TokenType::LPAREN)) {
    if (currentToken().type != TokenType::RPAREN &&
        currentToken().type != TokenType::EOF_TOK) {
      do {
        if (currentToken().type == TokenType::RPAREN) {
          hasTrailingComma = true;
          break;
        }
        args.push_back(parseExpression());
      } while (match(TokenType::COMMA));
    }
    expect(TokenType::RPAREN, "Expected ')' after annotation arguments");
  }

  int len = (currentToken().column - col);
  auto node = astCtx.create<AnnotationNode>(
      name, astCtx.copyArray<ExprNode *>(args), line, col, len);
  node->hasTrailingComma = hasTrailingComma;
  return node;
}

const Type *Parser::applyArrayDeclarator(const Type *baseType) {
  std::vector<uint64_t> sizes;
  while (match(TokenType::LBRACKET)) {
    if (match(TokenType::RBRACKET)) {
      sizes.push_back(0);
    } else {
      auto sizeExpr = parseExpression();
      expect(TokenType::RBRACKET, "Expected ']'");
      if (sizeExpr->kind == NodeKind::Number &&
          !static_cast<NumberNode *>(sizeExpr)->isFloat) {
        uint64_t size = 0;
        try {
          size = std::stoull(
              std::string(static_cast<NumberNode *>(sizeExpr)->raw), nullptr,
              0);
        } catch (const std::exception &) {
          /* Out-of-range or malformed literals (e.g. 'int[0x]') must not
           * terminate the compiler with a raw exception: report them like
           * any other source error. */
          reportError(sizeExpr->line, sizeExpr->column, sizeExpr->length,
                      "Array size literal is out of range");
          size = 1;
        }
        sizes.push_back(size);
      } else {
        reportError(sizeExpr->line, sizeExpr->column, sizeExpr->length,
                    "Array size must be a constant integer literal");
        sizes.push_back(1);
      }
    }
  }

  const Type *result = baseType;
  for (auto it = sizes.rbegin(); it != sizes.rend(); ++it) {
    result = astCtx.getArrayType(result, *it);
  }
  return result;
}

std::vector<ParamDeclNode *>
Parser::parseParameterList(bool &isVariadic, bool &hasTrailingComma,
                           bool allowUntypedParams) {
  std::vector<ParamDeclNode *> params;
  isVariadic = false;
  hasTrailingComma = false;
  bool inNamedBlock = false;
  bool optionalPositionalStarted = false;

  while (currentToken().type != TokenType::RPAREN &&
         currentToken().type != TokenType::EOF_TOK) {

    if (match(TokenType::ELLIPSIS)) {
      isVariadic = true;
      if (match(TokenType::COMMA)) {
        hasTrailingComma = true;
      }
      break;
    }

    if (!inNamedBlock && match(TokenType::LBRACE)) {
      inNamedBlock = true;
    }

    if (inNamedBlock && currentToken().type == TokenType::RBRACE) {
      advance();
      if (match(TokenType::COMMA)) {
        hasTrailingComma = true;
      }
      break;
    }

    if (currentToken().type == TokenType::RPAREN) {
      break;
    }

    std::string doc = consumeComments();

    int pLine = currentToken().line;
    int pCol = currentToken().column;

    bool isRequired = match(TokenType::REQUIRED_KW);
    if (isRequired && !inNamedBlock) {
      reportError(
          pLine, pCol, 8,
          "The 'required' modifier can only be applied to named parameters.");
      throw ParseException();
    }

    /* Dart-style initializing formal: 'Class(this.field)' declares a
     * parameter named 'field' whose type is the field's type. */
    bool isThisParam = false;
    if (currentToken().type == TokenType::THIS_KW &&
        peekToken().type == TokenType::DOT &&
        peekToken(2).type == TokenType::IDENTIFIER) {
      isThisParam = true;
    }

    const Type *pType = nullptr;
    std::string_view rawTypeStr;

    if (isThisParam) {
      int thisCol = currentToken().column;
      advance();
      advance();
      std::string_view fieldName = currentToken().value;
      int thisLen = 4 + 1 + (int)fieldName.length();
      expect(TokenType::IDENTIFIER, "Expected field name after 'this.'");

      ExprNode *defVal = nullptr;
      if (match(TokenType::ASSIGN)) {
        defVal = parseExpression();
      }

      if (isRequired && defVal) {
        reportError(pLine, pCol, thisLen,
                    "Required named parameter '" + std::string(fieldName) +
                        "' cannot have a default value.");
        throw ParseException();
      }

      /* The type is resolved from the record's fields during declaration
       * collection and semantic analysis; until then it stays null. */
      auto thisParam = astCtx.create<ParamDeclNode>(
          nullptr, fieldName, defVal, inNamedBlock, isRequired, pLine, pCol,
          thisLen);
      thisParam->isThisParam = true;
      thisParam->identifierColumn = thisCol;
      thisParam->identifierLength = thisLen;
      thisParam->rawTypeStr = "this." + std::string(fieldName);

      if (!doc.empty())
        thisParam->docString = astCtx.copyString(doc);
      if (cursor > 0 && !tokens[cursor - 1].trailingComment.empty()) {
        thisParam->trailingComment = tokens[cursor - 1].trailingComment;
      }

      for (auto *p : params) {
        if (p->name == fieldName) {
          reportError(pLine, pCol, thisLen,
                      "Redefinition of parameter '" +
                          std::string(fieldName) + "'.");
          throw ParseException();
        }
      }

      params.push_back(thisParam);

      if (!match(TokenType::COMMA)) {
        if (inNamedBlock) {
          expect(TokenType::RBRACE,
                 "Expected '}' to close named parameter list.");
          if (match(TokenType::COMMA)) {
            hasTrailingComma = true;
          }
        }
        break;
      } else {
        if (inNamedBlock && currentToken().type == TokenType::RBRACE) {
          hasTrailingComma = true;
          advance();
          if (match(TokenType::COMMA)) {
            hasTrailingComma = true;
          }
          break;
        }
        if (currentToken().type == TokenType::RPAREN) {
          hasTrailingComma = true;
          break;
        }
      }
      continue;
    }

    /* Untyped parameters (Dart-style lambdas): an identifier not followed by
     * another identifier or type modifier is the parameter name itself. */
    bool isUntyped =
        allowUntypedParams && currentToken().type == TokenType::IDENTIFIER;
    if (isUntyped) {
      TokenType nxt = peekToken().type;
      isUntyped = (nxt == TokenType::COMMA || nxt == TokenType::RPAREN ||
                   nxt == TokenType::ASSIGN || nxt == TokenType::LBRACE);
    }

    if (!isUntyped) {
      const char *typeStart = currentToken().value.data();
      pType = parseType();
      const char *typeEnd =
          tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
      rawTypeStr = std::string_view(typeStart, typeEnd - typeStart);
    }

    std::string_view pName = currentToken().value;
    int idCol = currentToken().column;
    int pLen = (int)pName.length();
    expect(TokenType::IDENTIFIER, "Expected parameter name.");

    ExprNode *defVal = nullptr;
    if (match(TokenType::ASSIGN)) {
      defVal = parseExpression();
    }

    if (!inNamedBlock) {
      if (defVal) {
        optionalPositionalStarted = true;
      } else if (optionalPositionalStarted) {
        reportError(pLine, pCol, pLen,
                    "Mandatory positional parameters cannot appear after "
                    "optional positional parameters.");
        throw ParseException();
      }
    } else {
      if (isRequired && defVal) {
        reportError(pLine, pCol, pLen,
                    "Required named parameter '" + std::string(pName) +
                        "' cannot have a default value.");
        throw ParseException();
      }
    }

    for (auto *p : params) {
      if (p->name == pName) {
        reportError(pLine, pCol, pLen,
                    "Redefinition of parameter '" + std::string(pName) + "'.");
        throw ParseException();
      }
    }

    auto paramNode = astCtx.create<ParamDeclNode>(
        pType, pName, defVal, inNamedBlock, isRequired, pLine, pCol, pLen);
    paramNode->rawTypeStr = rawTypeStr;
    paramNode->identifierColumn = idCol;
    paramNode->identifierLength = pLen;

    if (!doc.empty())
      paramNode->docString = astCtx.copyString(doc);
    if (cursor > 0 && !tokens[cursor - 1].trailingComment.empty()) {
      paramNode->trailingComment = tokens[cursor - 1].trailingComment;
    }
    params.push_back(paramNode);

    if (!match(TokenType::COMMA)) {
      if (inNamedBlock) {
        expect(TokenType::RBRACE,
               "Expected '}' to close named parameter list.");
        if (match(TokenType::COMMA)) {
          hasTrailingComma = true;
        }
      }
      break;
    } else {
      if (inNamedBlock && currentToken().type == TokenType::RBRACE) {
        hasTrailingComma = true;
        advance();
        if (match(TokenType::COMMA)) {
          hasTrailingComma = true;
        }
        break;
      }
      if (currentToken().type == TokenType::RPAREN) {
        hasTrailingComma = true;
        break;
      }
    }
  }
  return params;
}

BlockNode *Parser::parseFunctionBody(const Type *returnType) {
  if (match(TokenType::ARROW)) {
    int line = currentToken().line;
    int col = currentToken().column;

    auto expr = parseExpression();

    int endLine = currentToken().line;
    int endCol = currentToken().column + currentToken().value.length();
    expect(TokenType::SEMICOLON, "Expected ';' after '=>' expression");

    auto block = astCtx.create<BlockNode>(line, col);
    block->isExpressionBody = true;
    ASTNode *stmt = expr;

    if (returnType && !returnType->isVoid()) {
      stmt = astCtx.create<ReturnNode>(expr, line, col, endCol - col);
    }

    block->statements = astCtx.copyArray<ASTNode *>(stmt);
    block->finalize(endCol);
    block->endLine = endLine;

    return block;
  }

  return parseBlock();
}

void Parser::checkRecordMemberRedefinition(
    std::string_view name, const std::vector<VarDeclNode *> &fields,
    const std::vector<FunctionDeclNode *> &methods,
    const FunctionDeclNode *newMethod, int line, int col, int len) {
  for (auto *f : fields) {
    if (f->varName == name) {
      reportError(line, col, len,
                  "Redefinition of field '" + std::string(name) + "'.");
      throw ParseException();
    }
  }
  if (newMethod) {
    for (auto *m : methods) {
      if (m->name == name) {
        bool sameSignature = true;
        if (m->params.size() != newMethod->params.size()) {
          sameSignature = false;
        } else {
          for (size_t i = 0; i < m->params.size(); ++i) {
            /* Skip unresolved 'this.x' parameter types here too; methods
             * never carry them, constructors are handled elsewhere. */
            if (!m->params[i]->type || !newMethod->params[i]->type ||
                m->params[i]->type->toString() !=
                    newMethod->params[i]->type->toString()) {
              sameSignature = false;
              break;
            }
          }
        }

        /* The const qualifier is part of the overload-resolution signature. */
        if (sameSignature && m->isConst != newMethod->isConst) {
          sameSignature = false;
        }

        if (sameSignature) {
          reportError(line, col, len,
                      "Redefinition of method '" + std::string(name) +
                          "' with the same signature.");
          throw ParseException();
        }
      }
    }
  } else {
    for (auto *m : methods) {
      if (m->name == name) {
        reportError(line, col, len,
                    "Redefinition of '" + std::string(name) +
                        "' as a different kind of symbol.");
        throw ParseException();
      }
    }
  }
}

void Parser::checkConstructorRedefinition(
    const std::vector<FunctionDeclNode *> &constructors,
    const FunctionDeclNode *newCtor, int line, int col, int len) {
  for (auto *c : constructors) {
    bool sameSignature = true;
    if (c->params.size() != newCtor->params.size()) {
      sameSignature = false;
    } else {
      for (size_t i = 0; i < c->params.size(); ++i) {
        /* Types of 'this.x' parameters are resolved after the record body
         * is parsed; they cannot participate in the early signature check. */
        if (!c->params[i]->type || !newCtor->params[i]->type ||
            c->params[i]->type->toString() !=
                newCtor->params[i]->type->toString()) {
          sameSignature = false;
          break;
        }
      }
    }
    if (sameSignature) {
      reportError(line, col, len,
                  "Redefinition of constructor with the same signature.");
      throw ParseException();
    }
  }
}

DeclNode *
Parser::parseAnnotationDecl(llvm::ArrayRef<AnnotationNode *> annotations) {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  expect(TokenType::CLASS_KW, "Expected 'class' after 'annotation'");

  std::string_view name = currentToken().value;
  int idCol = currentToken().column;
  int idLen = name.length();
  expect(TokenType::IDENTIFIER, "Expected annotation class name");

  std::string fqNameStr = getFQName(name);
  std::string_view fqName = astCtx.copyString(fqNameStr);
  RecordType *classTy = astCtx.createRecordType(TypeKind::Class, fqName);
  classTy->setOpaque(false);

  expect(TokenType::LBRACE, "Expected '{'");

  std::vector<VarDeclNode *> fields;
  FunctionDeclNode *constructor = nullptr;

  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {

    std::string doc = consumeComments();
    auto memberAnnotations = parseAnnotations();

    bool isPub = false, isPriv = false, isProt = false;
    while (currentToken().type == TokenType::PUBLIC_KW ||
           currentToken().type == TokenType::PRIVATE_KW ||
           currentToken().type == TokenType::PROTECTED_KW) {
      if (currentToken().type == TokenType::PUBLIC_KW)
        isPub = true;
      if (currentToken().type == TokenType::PRIVATE_KW)
        isPriv = true;
      if (currentToken().type == TokenType::PROTECTED_KW)
        isProt = true;
      advance();
    }

    if (currentToken().type == TokenType::RBRACE ||
        currentToken().type == TokenType::EOF_TOK) {
      if (!memberAnnotations.empty()) {
        reportError(currentToken().line, currentToken().column,
                    (int)currentToken().value.length(),
                    "Annotations must be attached to a declaration.");
      }
      break;
    }

    if (currentToken().type == TokenType::CONST_KW &&
        peekToken().type == TokenType::IDENTIFIER &&
        peekToken().value == name) {
      int cLine = currentToken().line;
      int cCol = currentToken().column;
      int ctorIdCol = peekToken().column;

      advance();
      advance();
      expect(TokenType::LPAREN, "Expected '('");

      bool isVariadic = false;
      bool hasTrailingComma = false;
      auto params = parseParameterList(isVariadic, hasTrailingComma);
      if (isVariadic) {
        reportError(cLine, cCol, name.length(),
                    "Annotation constructors cannot be variadic.");
        throw ParseException();
      }
      expect(TokenType::RPAREN, "Expected ')'");

      if (constructor != nullptr) {
        reportError(cLine, cCol, name.length(),
                    "Redefinition of annotation constructor.");
        throw ParseException();
      }

      constructor = astCtx.create<FunctionDeclNode>(astCtx.VoidTy, name, cLine,
                                                    cCol, true, true);
      constructor->parentRecord = classTy;
      constructor->params = astCtx.copyArray<ParamDeclNode *>(params);
      constructor->annotations = memberAnnotations;
      constructor->hasPublicMod = isPub;
      constructor->hasPrivateMod = isPriv;
      constructor->hasProtectedMod = isProt;
      constructor->identifierColumn = ctorIdCol;
      constructor->identifierLength = name.length();
      constructor->hasTrailingComma = hasTrailingComma;

      if (!doc.empty())
        constructor->docString = astCtx.copyString(doc);
      if (cursor > 0 && !tokens[cursor - 1].trailingComment.empty()) {
        constructor->trailingComment = tokens[cursor - 1].trailingComment;
      }

      constructor->body = parseFunctionBody(astCtx.VoidTy);
      constructor->length =
          constructor->body->column + constructor->body->length - cCol;
      constructor->endLine = constructor->body->endLine;
      continue;
    }

    int mLine = currentToken().line;
    int mCol = currentToken().column;

    const char *typeStart = currentToken().value.data();
    const Type *memType = parseType();
    const char *typeEnd =
        tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
    std::string_view rawTypeStr(typeStart, typeEnd - typeStart);

    if (currentToken().type == TokenType::LPAREN) {
      reportError(currentToken().line, currentToken().column, 1,
                  "Missing type for member '" + memType->toString() + "'.");
      throw ParseException();
    }

    std::string_view memName = currentToken().value;
    int memIdCol = currentToken().column;
    int memIdLen = memName.length();

    expect(TokenType::IDENTIFIER, "Expected member name");

    int endLine = currentToken().line;
    int endCol = currentToken().column + 1;
    expect(TokenType::SEMICOLON, "Expected ';'");

    checkRecordMemberRedefinition(memName, fields, {}, nullptr, mLine, mCol,
                                  memName.length());

    auto field = astCtx.create<VarDeclNode>(memType, memName, nullptr, mLine,
                                            mCol, endCol - mCol);
    field->annotations = memberAnnotations;
    field->hasPublicMod = isPub;
    field->hasPrivateMod = isPriv;
    field->hasProtectedMod = isProt;
    field->rawTypeStr = rawTypeStr;
    field->endLine = endLine;
    field->identifierColumn = memIdCol;
    field->identifierLength = memIdLen;

    if (!doc.empty())
      field->docString = astCtx.copyString(doc);
    if (cursor > 0 && !tokens[cursor - 1].trailingComment.empty()) {
      field->trailingComment = tokens[cursor - 1].trailingComment;
    }

    fields.push_back(field);
  }

  std::string closingDoc = consumeComments();

  int endLine = currentToken().line;
  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACE, "Expected '}'");

  if (!closingDoc.empty()) {
    ASTNode *lastMember = nullptr;
    for (auto *f : fields)
      if (!lastMember || f->endLine > lastMember->endLine)
        lastMember = f;
    if (constructor &&
        (!lastMember || constructor->endLine > lastMember->endLine))
      lastMember = constructor;

    if (lastMember) {
      std::string combined;
      if (!lastMember->trailingComment.empty()) {
        combined = std::string(lastMember->trailingComment) + "\n" + closingDoc;
      } else {
        combined = "\n" + closingDoc;
      }
      lastMember->trailingComment = astCtx.copyString(combined);
    }
  }

  std::vector<FieldInfo> fInfos;
  for (size_t i = 0; i < fields.size(); ++i) {
    fInfos.push_back({fields[i]->varName, fields[i]->type, (uint32_t)i,
                      fields[i]->isPublic(fields[i]->varName),
                      fields[i]->isProtected(fields[i]->varName)});
  }
  classTy->setFields(astCtx.copyArray<FieldInfo>(fInfos));

  auto node = astCtx.create<AnnotationDeclNode>(name, line, col, endCol - col);
  node->fqName = fqName;
  node->fields = astCtx.copyArray<VarDeclNode *>(fields);
  node->constructor = constructor;
  node->annotations = annotations;
  node->endLine = endLine;
  node->identifierColumn = idCol;
  node->identifierLength = idLen;

  if (!constructor) {
    reportError(line, col, endCol - col,
                "Annotation classes require a const constructor.");
    throw ParseException();
  }

  return node;
}

std::string Parser::getCurrentNamespace() const {
  std::string ns;
  for (size_t i = 0; i < namespaceStack.size(); ++i) {
    ns += namespaceStack[i];
    if (i < namespaceStack.size() - 1)
      ns += ".";
  }
  return ns;
}

std::string Parser::getFQName(std::string_view name) const {
  std::string ns = getCurrentNamespace();
  return ns.empty() ? std::string(name) : ns + "." + std::string(name);
}

NamespaceDeclNode *Parser::parseNamespaceDecl(bool &isFileScoped) {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  std::string nameStr;
  while (true) {
    expect(TokenType::IDENTIFIER, "Expected namespace name component");
    nameStr += tokens[cursor - 1].value;
    if (match(TokenType::DOT)) {
      nameStr += ".";
    } else {
      break;
    }
  }
  std::string_view name = astCtx.copyString(nameStr);
  int len = tokens[cursor - 1].column + tokens[cursor - 1].value.length() - col;

  std::string fqNameStr = getFQName(name);
  std::string_view fqName = astCtx.copyString(fqNameStr);

  auto node = astCtx.create<NamespaceDeclNode>(name, line, col, len);
  node->fqName = fqName;

  if (match(TokenType::SEMICOLON)) {
    node->endLine = tokens[cursor - 1].line;
    isFileScoped = true;
    node->isFileScoped = true;
    return node;
  }

  isFileScoped = false;
  expect(TokenType::LBRACE, "Expected '{' or ';' after namespace name");

  namespaceStack.push_back(std::string(name));

  /* Usings declared inside the namespace must not leak out of it. */
  size_t prevUsings = activeUsings.size();

  std::vector<ASTNode *> stmts;
  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {
    try {
      if (auto stmt = parseStatement()) {
        stmts.push_back(stmt);
      }
    } catch (const ParseException &) {
      synchronize();
    }
  }

  activeUsings.resize(prevUsings);
  namespaceStack.pop_back();

  node->endLine = currentToken().line;
  expect(TokenType::RBRACE, "Expected '}'");
  node->statements = astCtx.copyArray<ASTNode *>(stmts);
  return node;
}

UsingNode *Parser::parseUsing() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  std::string nameStr;
  while (true) {
    expect(TokenType::IDENTIFIER, "Expected using name component");
    nameStr += tokens[cursor - 1].value;
    if (match(TokenType::DOT)) {
      nameStr += ".";
    } else {
      break;
    }
  }
  std::string_view name = astCtx.copyString(nameStr);
  int endCol = tokens[cursor - 1].column + tokens[cursor - 1].value.length();
  expect(TokenType::SEMICOLON, "Expected ';'");

  activeUsings.push_back(std::string(name));

  auto node = astCtx.create<UsingNode>(name, line, col, endCol - col);
  node->endLine = tokens[cursor - 1].line;
  return node;
}

ASTNode *Parser::parseStatement() {
  std::string doc = consumeComments();
  auto annotations = parseAnnotations();

  bool isPub = false, isPriv = false, isProt = false, isAbstract = false;
  while (currentToken().type == TokenType::PUBLIC_KW ||
         currentToken().type == TokenType::PRIVATE_KW ||
         currentToken().type == TokenType::PROTECTED_KW ||
         currentToken().type == TokenType::ABSTRACT_KW) {
    if (currentToken().type == TokenType::PUBLIC_KW)
      isPub = true;
    if (currentToken().type == TokenType::PRIVATE_KW)
      isPriv = true;
    if (currentToken().type == TokenType::PROTECTED_KW)
      isProt = true;
    if (currentToken().type == TokenType::ABSTRACT_KW)
      isAbstract = true;
    advance();
  }

  /* 'final' only applies to classes and variables. When it prefixes a
   * record keyword it is consumed here; variable declarations keep the
   * token for parseDeclarationOrFunction. */
  bool isFinalDecl = false;
  if (currentToken().type == TokenType::FINAL_KW &&
      (peekToken().type == TokenType::CLASS_KW ||
       peekToken().type == TokenType::STRUCT_KW ||
       peekToken().type == TokenType::UNION_KW)) {
    isFinalDecl = true;
    advance();
  }

  if (currentToken().type == TokenType::RBRACE ||
      currentToken().type == TokenType::EOF_TOK) {
    if (!annotations.empty()) {
      reportError(currentToken().line, currentToken().column,
                  (int)currentToken().value.length(),
                  "Annotations are strictly permitted on declarations only.");
    }
    if (isPub || isPriv || isProt || isAbstract) {
      reportError(
          currentToken().line, currentToken().column,
          (int)currentToken().value.length(),
          "Access modifiers are strictly permitted on declarations only.");
    }
    return nullptr;
  }

  ASTNode *node = nullptr;

  if (currentToken().type == TokenType::NAMESPACE_KW) {
    bool isFileScoped = false;
    node = parseNamespaceDecl(isFileScoped);
    if (isFileScoped) {
      reportError(node->line, node->column, node->length,
                  "File-scoped namespaces can only be declared at the top of "
                  "the file.");
    }
  } else if (currentToken().type == TokenType::USING_KW) {
    node = parseUsing();
  } else if (currentToken().type == TokenType::TYPEDEF_KW) {
    node = parseTypedefDecl();
  } else if (currentToken().type == TokenType::ENUM_KW) {
    node = parseEnumDecl();
  } else if (currentToken().type == TokenType::ANNOTATION_KW) {
    node = parseAnnotationDecl(annotations);
  } else if (currentToken().type == TokenType::UNION_KW) {
    node = parseRecordDecl(TypeKind::Union);
  } else if (currentToken().type == TokenType::STRUCT_KW) {
    node = parseRecordDecl(TypeKind::Struct);
  } else if (currentToken().type == TokenType::CLASS_KW) {
    node = parseRecordDecl(TypeKind::Class, isAbstract, isFinalDecl);
  } else if (currentToken().type == TokenType::IF_KW) {
    node = parseIfStatement();
  } else if (currentToken().type == TokenType::FOR_KW) {
    node = parseForStatement();
  } else if (currentToken().type == TokenType::WHILE_KW) {
    node = parseWhileStatement();
  } else if (currentToken().type == TokenType::TRY_KW) {
    node = parseTryStatement();
  } else if (currentToken().type == TokenType::THROW_KW) {
    node = parseThrowStatement();
  } else if (currentToken().type == TokenType::ASSERT_KW) {
    node = parseAssertStatement();
  } else if (currentToken().type == TokenType::SWITCH_KW) {
    node = parseSwitchStatement();
  } else if (currentToken().type == TokenType::BREAK_KW) {
    node = parseBreakStatement();
  } else if (currentToken().type == TokenType::CONTINUE_KW) {
    node = parseContinueStatement();
  } else if (isDeclaration()) {
    node = parseDeclarationOrFunction(annotations);
  } else if (currentToken().type == TokenType::RETURN) {
    node = parseReturn();
  } else if (currentToken().type == TokenType::LBRACE) {
    node = parseBlock();
  } else {
    node = parseExpressionStatement();
  }

  if (isAbstract && (!node || node->kind != NodeKind::ClassDecl)) {
    reportError(node ? node->line : currentToken().line,
                node ? node->column : currentToken().column,
                node ? node->length : currentToken().value.length(),
                "The 'abstract' modifier is strictly permitted on class "
                "declarations only.");
  }

  if (isFinalDecl && (!node || node->kind != NodeKind::ClassDecl)) {
    reportError(node ? node->line : currentToken().line,
                node ? node->column : currentToken().column,
                node ? node->length : currentToken().value.length(),
                "The 'final' modifier is strictly permitted on class "
                "declarations and variables only.");
  }

  if (node) {
    if (!doc.empty())
      node->docString = astCtx.copyString(doc);
    if (cursor > 0 && !tokens[cursor - 1].trailingComment.empty()) {
      node->trailingComment = tokens[cursor - 1].trailingComment;
    }
  }

  if (node && !annotations.empty()) {
    if (node->kind == NodeKind::VarDecl ||
        node->kind == NodeKind::FunctionDecl ||
        node->kind == NodeKind::StructDecl ||
        node->kind == NodeKind::ClassDecl ||
        node->kind == NodeKind::AnnotationDecl ||
        node->kind == NodeKind::ParamDecl) {
      static_cast<DeclNode *>(node)->annotations = annotations;
    } else {
      reportError(node->line, node->column, node->length,
                  "Annotations are strictly permitted on declarations only.");
    }
  }

  if (isPub || isPriv || isProt) {
    if (node && (node->kind == NodeKind::VarDecl ||
                 node->kind == NodeKind::FunctionDecl ||
                 node->kind == NodeKind::StructDecl ||
                 node->kind == NodeKind::ClassDecl ||
                 node->kind == NodeKind::AnnotationDecl ||
                 node->kind == NodeKind::EnumDecl ||
                 node->kind == NodeKind::TypedefDecl)) {
      auto decl = static_cast<DeclNode *>(node);
      decl->hasPublicMod = isPub;
      decl->hasPrivateMod = isPriv;
      decl->hasProtectedMod = isProt;
    } else {
      reportError(
          node ? node->line : currentToken().line,
          node ? node->column : currentToken().column,
          node ? node->length : currentToken().value.length(),
          "Access modifiers are strictly permitted on declarations only.");
    }
  }

  return node;
}

DeclNode *Parser::parseTypedefDecl() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  std::string_view name = currentToken().value;
  int idCol = currentToken().column;
  int idLen = name.length();
  expect(TokenType::IDENTIFIER, "Expected alias name after 'typedef'");

  expect(TokenType::ASSIGN, "Expected '=' in typedef declaration");

  const char *typeStart = currentToken().value.data();
  const Type *targetType = nullptr;
  std::string_view targetEntity = "";

  /* Fallback peek to distinguish known types from plain function identifiers */
  const Type *knownBase = nullptr;
  if (currentToken().type == TokenType::IDENTIFIER) {
    auto resolveKnown = [&](std::string_view tn) -> const Type * {
      const Type *t = astCtx.getBuiltinTypeByName(tn);
      if (!t)
        t = astCtx.getRecordType(tn);
      if (!t)
        t = astCtx.getTypeAlias(tn);
      if (!t)
        t = astCtx.getEnumTypeByName(tn);
      return t;
    };
    knownBase = resolveKnown(currentToken().value);
    /* Enums, records and aliases from other modules register under their
     * fully-qualified name (NS.Type); walk the enclosing namespaces. */
    if (!knownBase) {
      std::string ns = getCurrentNamespace();
      while (!ns.empty()) {
        knownBase = resolveKnown(ns + "." + std::string(currentToken().value));
        if (knownBase)
          break;
        size_t pos = ns.find_last_of('.');
        if (pos != std::string::npos)
          ns = ns.substr(0, pos);
        else
          break;
      }
    }
  }

  if (knownBase || currentToken().type == TokenType::CONST_KW ||
      currentToken().type == TokenType::TYPE_KW) {
    targetType = parseType();
  } else if (currentToken().type == TokenType::IDENTIFIER) {
    targetEntity = currentToken().value;
    advance();
  } else {
    reportError(currentToken().line, currentToken().column,
                currentToken().value.length(),
                "Expected type or function identifier in typedef");
    throw ParseException();
  }

  const char *typeEnd =
      tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
  std::string_view rawTypeStr(typeStart, typeEnd - typeStart);

  int endLine = currentToken().line;
  int endCol = currentToken().column + (int)currentToken().value.length();
  expect(TokenType::SEMICOLON, "Expected ';' after typedef target");

  auto aliasTy = astCtx.create<AliasType>(name);
  if (targetType)
    aliasTy->setTarget(targetType);

  astCtx.addTypeAlias(name, aliasTy);

  std::string fqNameStr = getFQName(name);
  std::string_view fqName = astCtx.copyString(fqNameStr);

  auto decl =
      astCtx.create<TypedefDeclNode>(name, targetType, line, col, endCol - col);
  decl->fqName = fqName;
  decl->targetEntityName = targetEntity;
  decl->aliasType = aliasTy;
  if (targetType) {
    decl->rawTargetTypeStr = rawTypeStr;
  }
  decl->endLine = endLine;
  decl->identifierColumn = idCol;
  decl->identifierLength = idLen;

  return decl;
}

BlockNode *Parser::parseStatementAsBlock() {
  if (currentToken().type == TokenType::LBRACE) {
    return parseBlock();
  }

  int startLine = currentToken().line;
  int startCol = currentToken().column;

  /* Usings declared inside the single-statement block must not leak out. */
  size_t prevUsings = activeUsings.size();

  auto stmt = parseStatement();

  activeUsings.resize(prevUsings);

  auto block = astCtx.create<BlockNode>(startLine, startCol);
  block->hasBraces = false;
  if (stmt) {
    std::vector<ASTNode *> statements = {stmt};
    block->statements = astCtx.copyArray<ASTNode *>(statements);
    block->length = (stmt->column + stmt->length) - startCol;
    block->endLine = stmt->endLine;
  } else {
    block->statements = {};
    block->length = 0;
    block->endLine = startLine;
  }

  return block;
}

IfNode *Parser::parseIfStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  expect(TokenType::LPAREN, "Expected '(' after 'if'");
  auto cond = parseExpression();
  expect(TokenType::RPAREN, "Expected ')' after if condition");

  auto thenBlock = parseStatementAsBlock();
  ASTNode *elseBlock = nullptr;

  if (match(TokenType::ELSE_KW)) {
    if (currentToken().type == TokenType::IF_KW) {
      elseBlock = parseIfStatement();
    } else {
      elseBlock = parseStatementAsBlock();
    }
  }

  int len = (elseBlock ? (elseBlock->column + elseBlock->length)
                       : (thenBlock->column + thenBlock->length)) -
            col;
  auto node = astCtx.create<IfNode>(cond, thenBlock, elseBlock, line, col, len);
  node->endLine = elseBlock ? elseBlock->endLine : thenBlock->endLine;
  return node;
}

StmtNode *Parser::parseForStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  expect(TokenType::LPAREN, "Expected '(' after 'for'");

  if (isForInHeader()) {
    return parseForInStatement(line, col);
  }

  ASTNode *initStmt = nullptr;
  if (currentToken().type != TokenType::SEMICOLON) {
    if (currentToken().type == TokenType::TYPE_KW ||
        currentToken().type == TokenType::CONST_KW ||
        currentToken().type == TokenType::VAR_KW ||
        currentToken().type == TokenType::FINAL_KW ||
        currentToken().type == TokenType::STATIC_KW ||
        (currentToken().type == TokenType::IDENTIFIER &&
         (peekToken().type == TokenType::IDENTIFIER ||
          astCtx.getRecordType(currentToken().value) != nullptr))) {
      initStmt = parseDeclarationOrFunction();
    } else {
      initStmt = parseExpressionStatement();
    }
  } else {
    expect(TokenType::SEMICOLON, "Expected ';'");
  }

  ExprNode *cond = nullptr;
  if (currentToken().type != TokenType::SEMICOLON) {
    cond = parseExpression();
  }
  expect(TokenType::SEMICOLON, "Expected ';'");

  ExprNode *inc = nullptr;
  if (currentToken().type != TokenType::RPAREN) {
    inc = parseExpression();
  }
  expect(TokenType::RPAREN, "Expected ')' after for clauses");

  auto body = parseStatementAsBlock();

  int len = (body->column + body->length) - col;
  auto node = astCtx.create<ForNode>(initStmt, cond, inc, body, line, col, len);
  node->endLine = body->endLine;
  return node;
}

/* Lookahead over the 'for (...)' header: this is a Dart-style for-in loop
 * ('for (var x in e)', 'for (final& x in e)', 'for (T x in e)') when an
 * identifier is followed by the contextual keyword 'in' before any ';', '='
 * or '(' appears. 'in' is not a reserved word, so it stays an IDENTIFIER and
 * is only recognized here. No valid C-style for can contain the adjacent
 * pair 'name in' before ';'/'='/('; 'in' is not an operator, so the scan
 * never misclassifies a classic for loop. */
bool Parser::isForInHeader() {
  size_t i = 0;
  while (true) {
    TokenType t = peekToken(i).type;
    if (t == TokenType::EOF_TOK || t == TokenType::SEMICOLON ||
        t == TokenType::ASSIGN || t == TokenType::LPAREN ||
        t == TokenType::RPAREN) {
      return false;
    }
    if (t == TokenType::IDENTIFIER) {
      TokenType next = peekToken(i + 1).type;
      if (next == TokenType::IDENTIFIER && peekToken(i + 1).value == "in") {
        return true;
      }
    }
    i++;
  }
}

ForInNode *Parser::parseForInStatement(int line, int col) {
  bool isFinalDecl = false;
  bool isRefBinding = false;
  bool isImplicitlyTyped = false;
  const Type *nodeType = nullptr;
  std::string_view rawTypeStr;

  if (match(TokenType::VAR_KW)) {
    isImplicitlyTyped = true;
    nodeType = astCtx.AutoTy;
    rawTypeStr = "var";
  } else if (match(TokenType::FINAL_KW)) {
    isFinalDecl = true;
    isImplicitlyTyped = true;
    nodeType = astCtx.AutoTy;
    rawTypeStr = "final";
  }

  if (isImplicitlyTyped) {
    /* 'var& x' / 'final& x': bind a reference to the element (no copy);
     * 'final&' also binds a const reference. Sema fixes the type up to
     * Reference(T) / Reference(const T) from the iterator's element type. */
    if (currentToken().type == TokenType::AMPERSAND) {
      isRefBinding = true;
      advance();
      rawTypeStr = astCtx.copyString(std::string(rawTypeStr) + "&");
    }
  } else {
    const char *typeStart = currentToken().value.data();
    nodeType = parseType();
    const char *typeEnd =
        tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
    rawTypeStr = std::string_view(typeStart, typeEnd - typeStart);
  }

  int idCol = currentToken().column;
  std::string_view id = currentToken().value;
  int idLen = (int)id.length();
  expect(TokenType::IDENTIFIER,
         "Expected an identifier for the for-in loop variable");

  if (!(currentToken().type == TokenType::IDENTIFIER &&
        currentToken().value == "in")) {
    reportError(currentToken().line, currentToken().column,
                (int)currentToken().value.length(),
                "Expected 'in' after the for-in loop variable");
    throw ParseException();
  }
  advance();

  auto iterable = parseExpression();
  expect(TokenType::RPAREN, "Expected ')' after the for-in iterable");

  auto body = parseStatementAsBlock();

  auto varDecl =
      astCtx.create<VarDeclNode>(nodeType, id, nullptr, line, idCol, idLen);
  varDecl->fqName = astCtx.copyString(getFQName(id));
  varDecl->rawTypeStr = rawTypeStr;
  varDecl->isFinal = isFinalDecl;
  varDecl->identifierColumn = idCol;
  varDecl->identifierLength = idLen;

  int len = (body->column + body->length) - col;
  auto node = astCtx.create<ForInNode>(varDecl, iterable, body, line, col, len);
  node->isRefBinding = isRefBinding;
  node->endLine = body->endLine;
  return node;
}

WhileNode *Parser::parseWhileStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  expect(TokenType::LPAREN, "Expected '(' after 'while'");
  auto cond = parseExpression();
  expect(TokenType::RPAREN, "Expected ')' after while condition");

  auto body = parseStatementAsBlock();

  int len = (body->column + body->length) - col;
  auto node = astCtx.create<WhileNode>(cond, body, line, col, len);
  node->endLine = body->endLine;
  return node;
}

/* C++-style exception handling: 'try { ... } catch (T e) { ... }' with an
 * arbitrary number of catch clauses. 'catch (...)' matches every type. */
TryStmtNode *Parser::parseTryStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  auto body = parseBlock();

  std::vector<CatchClauseNode *> clauses;
  int endLine = body->endLine;
  int endCol = body->column + body->length;

  while (currentToken().type == TokenType::CATCH_KW) {
    int cLine = currentToken().line;
    int cCol = currentToken().column;
    advance();

    expect(TokenType::LPAREN, "Expected '(' after 'catch'");

    const Type *catchType = nullptr;
    std::string_view varName = "";
    bool isCatchAll = false;
    std::string_view rawTypeStr = "";

    if (currentToken().type == TokenType::ELLIPSIS) {
      isCatchAll = true;
      advance();
    } else {
      const char *typeStart = currentToken().value.data();
      catchType = parseType();
      const char *typeEnd = tokens[cursor - 1].value.data() +
                            tokens[cursor - 1].value.length();
      rawTypeStr =
          astCtx.copyString(std::string_view(typeStart, typeEnd - typeStart));
    }

    /* Optional binding variable: 'catch (T e)'. */
    if (currentToken().type == TokenType::IDENTIFIER) {
      varName = currentToken().value;
      advance();
    }

    expect(TokenType::RPAREN, "Expected ')' after catch parameter");

    auto cBody = parseBlock();

    auto clause = astCtx.create<CatchClauseNode>(catchType, varName, cBody,
                                                 cLine, cCol, 1);
    clause->rawTypeStr = rawTypeStr;
    clause->isCatchAll = isCatchAll;
    clause->length = (cBody->column + cBody->length) - cCol;
    clause->endLine = cBody->endLine;
    clauses.push_back(clause);

    endLine = cBody->endLine;
    endCol = cBody->column + cBody->length;
  }

  if (clauses.empty()) {
    reportError(currentToken().line, currentToken().column,
                (int)currentToken().value.length(),
                "Expected at least one 'catch' clause after 'try' block");
  }

  int len = endCol - col;
  auto node =
      astCtx.create<TryStmtNode>(body, astCtx.copyArray<CatchClauseNode *>(clauses),
                                 line, col, len);
  node->endLine = endLine;
  return node;
}

/* 'throw expr;' or a bare 'throw;' inside a catch clause (rethrow). */
ThrowStmtNode *Parser::parseThrowStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  ExprNode *value = nullptr;
  if (currentToken().type != TokenType::SEMICOLON) {
    value = parseExpression();
  }

  int len = currentToken().column + (int)currentToken().value.length() - col;
  expect(TokenType::SEMICOLON, "Expected ';' after throw expression");

  auto node = astCtx.create<ThrowStmtNode>(value, line, col, len);
  node->endLine = tokens[cursor - 1].line;
  return node;
}

/* 'assert(expr);' aborts with the source location when the expression is
 * false. */
AssertStmtNode *Parser::parseAssertStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  expect(TokenType::LPAREN, "Expected '(' after 'assert'");
  auto cond = parseExpression();
  expect(TokenType::RPAREN, "Expected ')' after assert condition");

  int len = currentToken().column + (int)currentToken().value.length() - col;
  expect(TokenType::SEMICOLON, "Expected ';' after assert statement");

  auto node = astCtx.create<AssertStmtNode>(cond, line, col, len);
  node->endLine = tokens[cursor - 1].line;
  return node;
}

SwitchNode *Parser::parseSwitchStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  expect(TokenType::LPAREN, "Expected '(' after 'switch'");
  auto cond = parseExpression();
  expect(TokenType::RPAREN, "Expected ')' after switch condition");

  expect(TokenType::LBRACE, "Expected '{' after switch expression");

  std::vector<CaseNode *> cases;
  bool hasDefault = false;

  /* Usings declared inside the switch must not leak out of it. */
  size_t prevUsings = activeUsings.size();

  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {

    std::string doc = consumeComments();

    if (currentToken().type == TokenType::RBRACE ||
        currentToken().type == TokenType::EOF_TOK) {
      break;
    }

    int cLine = currentToken().line;
    int cCol = currentToken().column;
    ExprNode *caseVal = nullptr;

    if (match(TokenType::CASE_KW)) {
      caseVal = parseExpression();
      expect(TokenType::COLON, "Expected ':' after case value");
    } else if (match(TokenType::DEFAULT_KW)) {
      expect(TokenType::COLON, "Expected ':' after 'default'");
      if (hasDefault) {
        reportError(cLine, cCol, 7,
                    "Multiple 'default' labels in switch statement");
      }
      hasDefault = true;
    } else {
      reportError(currentToken().line, currentToken().column,
                  currentToken().value.length(),
                  "Expected 'case' or 'default' in switch body");
      synchronize();
      break;
    }

    std::vector<ASTNode *> stmts;
    while (true) {
      TokenType nextTy = currentToken().type;
      if (nextTy == TokenType::CASE_KW || nextTy == TokenType::DEFAULT_KW ||
          nextTy == TokenType::RBRACE || nextTy == TokenType::EOF_TOK) {
        break;
      }

      try {
        if (auto stmt = parseStatement()) {
          stmts.push_back(stmt);
        }
      } catch (const ParseException &) {
        synchronize();
      }
    }

    int cLen = currentToken().column - cCol;
    auto caseNode = astCtx.create<CaseNode>(
        caseVal, astCtx.copyArray<ASTNode *>(stmts), cLine, cCol, cLen);

    /* Track the actual lower bound of the case block for accurate line
       difference measurement in the formatter */
    caseNode->endLine = stmts.empty() ? cLine : stmts.back()->endLine;

    if (!doc.empty())
      caseNode->docString = astCtx.copyString(doc);
    if (cursor > 0 && !tokens[cursor - 1].trailingComment.empty()) {
      caseNode->trailingComment = tokens[cursor - 1].trailingComment;
    }

    cases.push_back(caseNode);
  }

  std::string closingDoc = consumeComments();

  int endLine = currentToken().line;
  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACE, "Expected '}' at end of switch block");

  activeUsings.resize(prevUsings);

  if (!closingDoc.empty() && !cases.empty()) {
    ASTNode *lastCase = cases.back();
    std::string combined;
    if (!lastCase->trailingComment.empty()) {
      combined = std::string(lastCase->trailingComment) + "\n" + closingDoc;
    } else {
      combined = "\n" + closingDoc;
    }
    lastCase->trailingComment = astCtx.copyString(combined);
  }

  auto node =
      astCtx.create<SwitchNode>(cond, astCtx.copyArray<CaseNode *>(cases),
                                hasDefault, line, col, endCol - col);
  node->endLine = endLine;
  return node;
}

BreakNode *Parser::parseBreakStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();
  int endCol = currentToken().column + currentToken().value.length();
  expect(TokenType::SEMICOLON, "Expected ';' after 'break'");
  return astCtx.create<BreakNode>(line, col, endCol - col);
}

ContinueNode *Parser::parseContinueStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();
  int endCol = currentToken().column + currentToken().value.length();
  expect(TokenType::SEMICOLON, "Expected ';' after 'continue'");
  return astCtx.create<ContinueNode>(line, col, endCol - col);
}

ExprNode *Parser::parseLogicalOr() {
  auto left = parseLogicalAnd();
  while (currentToken().type == TokenType::LOGICAL_OR) {
    int line = left->line;
    int col = left->column;
    std::string_view op = currentToken().value;
    advance();
    auto right = parseLogicalAnd();
    left = astCtx.create<BinaryOpNode>(op, left, right, line, col);
  }
  return left;
}

ExprNode *Parser::parseLogicalAnd() {
  auto left = parseBitwiseOr();
  while (currentToken().type == TokenType::LOGICAL_AND) {
    int line = left->line;
    int col = left->column;
    std::string_view op = currentToken().value;
    advance();
    auto right = parseBitwiseOr();
    left = astCtx.create<BinaryOpNode>(op, left, right, line, col);
  }
  return left;
}

ExprNode *Parser::parseEquality() {
  auto left = parseRelational();
  while (currentToken().type == TokenType::EQ ||
         currentToken().type == TokenType::NEQ) {
    int line = left->line;
    int col = left->column;
    std::string_view op = currentToken().value;
    advance();
    auto right = parseRelational();
    left = astCtx.create<BinaryOpNode>(op, left, right, line, col);
  }
  return left;
}

ExprNode *Parser::parseRelational() {
  auto left = parseShift();
  while (true) {
    if (currentToken().type == TokenType::LT ||
        currentToken().type == TokenType::GT ||
        currentToken().type == TokenType::LE ||
        currentToken().type == TokenType::GE) {
      int line = left->line;
      int col = left->column;
      std::string_view op = currentToken().value;
      advance();
      auto right = parseShift();
      left = astCtx.create<BinaryOpNode>(op, left, right, line, col);
      continue;
    }

    if (currentToken().type == TokenType::IS_KW) {
      int line = left->line;
      int col = left->column;
      advance();

      /* 'is!' negates the type test: 'expr is! Type'. */
      bool negate = match(TokenType::BANG);

      /* '&&' is not treated as the rvalue-reference suffix here: 'x is T &&
       * y' must parse as '(x is T) && y'. */
      const char *typeStart = currentToken().value.data();
      const Type *targetType = parseType(false, /*allowRValueRef=*/false);
      const char *typeEnd =
          tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
      std::string_view rawTypeStr(typeStart, typeEnd - typeStart);

      int endCol = tokens[cursor - 1].column + tokens[cursor - 1].value.length();
      auto isNode =
          astCtx.create<IsExprNode>(left, targetType, line, col, endCol - col);
      isNode->isNegated = negate;
      isNode->rawTargetTypeStr = rawTypeStr;
      left = isNode;
      continue;
    }

    break;
  }
  return left;
}

ExprNode *Parser::parseBitwiseOr() {
  auto left = parseBitwiseXor();
  while (currentToken().type == TokenType::PIPE) {
    int line = left->line;
    int col = left->column;
    std::string_view op = currentToken().value;
    advance();
    auto right = parseBitwiseXor();
    left = astCtx.create<BinaryOpNode>(op, left, right, line, col);
  }
  return left;
}

ExprNode *Parser::parseBitwiseXor() {
  auto left = parseBitwiseAnd();
  while (currentToken().type == TokenType::CARET) {
    int line = left->line;
    int col = left->column;
    std::string_view op = currentToken().value;
    advance();
    auto right = parseBitwiseAnd();
    left = astCtx.create<BinaryOpNode>(op, left, right, line, col);
  }
  return left;
}

ExprNode *Parser::parseBitwiseAnd() {
  auto left = parseEquality();
  while (currentToken().type == TokenType::AMPERSAND) {
    int line = left->line;
    int col = left->column;
    std::string_view op = currentToken().value;
    advance();
    auto right = parseEquality();
    left = astCtx.create<BinaryOpNode>(op, left, right, line, col);
  }
  return left;
}

ExprNode *Parser::parseShift() {
  auto left = parseAdditive();
  while (currentToken().type == TokenType::LSHIFT ||
         currentToken().type == TokenType::RSHIFT) {
    int line = left->line;
    int col = left->column;
    std::string_view op = currentToken().value;
    advance();
    auto right = parseAdditive();
    left = astCtx.create<BinaryOpNode>(op, left, right, line, col);
  }
  return left;
}

ExprNode *Parser::parseAdditive() {
  auto left = parseTerm();
  while (currentToken().type == TokenType::PLUS ||
         currentToken().type == TokenType::MINUS) {
    int line = left->line;
    int col = left->column;
    std::string_view op = currentToken().value;
    advance();
    auto right = parseTerm();
    left = astCtx.create<BinaryOpNode>(op, left, right, line, col);
  }
  return left;
}

ExprNode *Parser::parseUnary() {
  if (currentToken().type == TokenType::AWAIT_KW) {
    int line = currentToken().line;
    int col = currentToken().column;
    int len = currentToken().value.length();
    if (!asyncEnabled) {
      reportError(line, col, len,
                  "'await' is disabled for this build (async support is "
                  "turned off).");
      throw ParseException();
    }
    advance();
    auto expr = parseUnary();
    return astCtx.create<AwaitExprNode>(expr, line, col,
                                        (expr->column + expr->length) - col);
  }

  if (match(TokenType::NEW_KW)) {
    int line = currentToken().line;
    int col = currentToken().column;

    if (currentToken().type == TokenType::IDENTIFIER &&
        currentToken().value == "uninitialized") {
      reportError(line, col, 12,
                  "'new uninitialized' has been removed; use "
                  "Memory.alloc(size, align) instead.");
      throw ParseException();
    }
    if (currentToken().type == TokenType::LPAREN) {
      reportError(line, col, 1,
                  "Placement new has been removed; use "
                  "Memory.construct<T>(ptr, args...) instead.");
      throw ParseException();
    }

    const char *typeStart = currentToken().value.data();
    /* 'new Foo.named(...)': when 'Foo.big' is not a type, the '.big' is a
     * named constructor. parseType would swallow the qualified name, so
     * hide the dot while the base type is parsed. */
    bool newNamedCtor = false;
    if (currentToken().type == TokenType::IDENTIFIER &&
        peekToken().type == TokenType::DOT &&
        peekToken(2).type == TokenType::IDENTIFIER &&
        peekToken(3).type == TokenType::LPAREN) {
      std::string qual = std::string(currentToken().value) + "." +
                         std::string(peekToken(2).value);
      bool isQualifiedType = astCtx.getRecordType(qual) ||
                             astCtx.getTypeAlias(qual) ||
                             astCtx.getEnumTypeByName(qual);
      if (!isQualifiedType) {
        newNamedCtor = true;
        const_cast<Token &>(tokens[cursor + 1]).type = TokenType::UNKNOWN;
      }
    }
    const Type *allocTy = parseType(true);
    if (newNamedCtor) {
      const_cast<Token &>(tokens[cursor]).type = TokenType::DOT;
    }
    const char *typeEnd =
        tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
    std::string_view rawTypeStr(typeStart, typeEnd - typeStart);

    /* Dart-style named constructor: 'new Foo.named(...)'. */
    std::string_view namedCtorName;
    if (currentToken().type == TokenType::DOT &&
        peekToken().type == TokenType::IDENTIFIER &&
        peekToken(2).type == TokenType::LPAREN) {
      namedCtorName = peekToken().value;
      advance();
      advance();
    }

    ExprNode *arraySize = nullptr;

    if (match(TokenType::LBRACKET)) {
      arraySize = parseExpression();
      expect(TokenType::RBRACKET, "Expected ']'");
    }

    std::vector<ExprNode *> args;
    std::vector<std::string_view> argNames;
    bool hasParens = false;
    bool namedStarted = false;
    bool hasTrailingComma = false;

    if (match(TokenType::LPAREN)) {
      hasParens = true;

      if (currentToken().type != TokenType::RPAREN) {
        do {
          if (currentToken().type == TokenType::RPAREN) {
            hasTrailingComma = true;
            break;
          }

          if (currentToken().type == TokenType::IDENTIFIER &&
              peekToken().type == TokenType::COLON) {
            namedStarted = true;
            argNames.push_back(currentToken().value);
            advance();
            advance();
            args.push_back(parseExpression());
          } else {
            if (namedStarted) {
              reportError(
                  currentToken().line, currentToken().column,
                  currentToken().value.length(),
                  "Positional arguments cannot appear after named arguments.");
              throw ParseException();
            }
            argNames.push_back("");
            args.push_back(parseExpression());

            if (currentToken().type == TokenType::IDENTIFIER) {
              reportError(
                  currentToken().line, currentToken().column,
                  currentToken().value.length(),
                  "Unexpected identifier in argument list. Missing comma?");
              throw ParseException();
            }
          }

        } while (match(TokenType::COMMA));
      }
      expect(TokenType::RPAREN, "Expected ')'");
    }

    auto argsRef = astCtx.copyArray<ExprNode *>(args);
    auto namesRef = astCtx.copyArray<std::string_view>(argNames);
    auto node = astCtx.create<NewExprNode>(allocTy, arraySize, argsRef,
                                           namesRef, hasParens, line, col,
                                           currentToken().column - col);
    node->namedCtorName = namedCtorName;
    node->rawAllocatedTypeStr = rawTypeStr;
    node->rawArgs = argsRef;
    node->rawArgNames = namesRef;
    node->hasRawArgs = true;
    node->hasTrailingComma = hasTrailingComma;
    return node;
  }

  if (match(TokenType::DELETE_KW)) {
    int line = currentToken().line;
    int col = currentToken().column;
    bool isArray = false;

    if (currentToken().type == TokenType::IDENTIFIER &&
        currentToken().value == "uninitialized") {
      reportError(line, col, 12,
                  "'delete uninitialized' has been removed; use "
                  "Memory.destruct(ptr) and Memory.free(raw) instead.");
      throw ParseException();
    }

    if (match(TokenType::LBRACKET)) {
      expect(TokenType::RBRACKET, "Expected ']' after '[' in delete");
      isArray = true;
    }

    auto ptr = parseUnary();
    auto delNode = astCtx.create<DeleteExprNode>(ptr, isArray, line, col,
                                                 currentToken().column - col);
    return delNode;
  }

  if (currentToken().type == TokenType::STAR ||
      currentToken().type == TokenType::AMPERSAND ||
      currentToken().type == TokenType::MINUS ||
      currentToken().type == TokenType::PLUS ||
      currentToken().type == TokenType::BANG ||
      currentToken().type == TokenType::TILDE ||
      currentToken().type == TokenType::PLUS_PLUS ||
      currentToken().type == TokenType::MINUS_MINUS) {
    int line = currentToken().line;
    int col = currentToken().column;
    std::string_view op = currentToken().value;
    advance();
    auto expr = parseUnary();
    return astCtx.create<UnaryOpNode>(op, expr, line, col, false);
  }

  return parsePostfix();
}

ExprNode *Parser::parseTernary() {
  auto expr = parseLogicalOr();
  if (match(TokenType::QUESTION)) {
    int line = expr->line;
    int col = expr->column;

    auto trueExpr = parseExpression();
    expect(TokenType::COLON, "Expected ':' in ternary operator");
    auto falseExpr = parseAssignment();

    int endCol = falseExpr->column + falseExpr->length;
    return astCtx.create<TernaryOpNode>(expr, trueExpr, falseExpr, line, col,
                                        endCol - col);
  }
  return expr;
}

DeclNode *Parser::parseRecordDecl(TypeKind kind, bool isAbstract,
                                  bool isFinal) {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  std::string_view name = currentToken().value;
  int idCol = currentToken().column;
  int idLen = name.length();
  expect(TokenType::IDENTIFIER, "Expected record name");

  std::vector<std::string_view> tParams;
  std::vector<TemplateConstraint> tConstraints;
  std::vector<const Type *> specArgs;
  bool sawConcreteArg = false;
  bool isTemplateDecl = false;
  bool isSpec = false;
  std::string_view rawTplStr;
  std::string specMangledName;

  if (currentToken().type == TokenType::LT) {
    size_t ltTok = cursor;
    advance();
    if (currentToken().type != TokenType::GT) {
      do {
        /* An unknown identifier is a template parameter; a known type (or a
         * nested template instantiation like 'List<String>') is a concrete
         * argument, which makes this declaration a specialization. */
        if (currentToken().type == TokenType::IDENTIFIER &&
            !isKnownTypeName(currentToken().value) &&
            !isTemplateParam(currentToken().value)) {
          for (auto tp : tParams) {
            if (tp == currentToken().value) {
              reportError(currentToken().line, currentToken().column,
                          currentToken().value.length(),
                          "Redefinition of template parameter '" +
                              std::string(tp) + "'.");
              throw ParseException();
            }
          }
          tParams.push_back(currentToken().value);
          pushTemplateParam(tParams.back());
          expect(TokenType::IDENTIFIER, "Expected template parameter name");
          TemplateConstraint tc;
          if (match(TokenType::EXTENDS_KW)) {
            tc = parseTemplateConstraint();
          }
          tConstraints.push_back(tc);
          specArgs.push_back(astCtx.getTemplateParamType(tParams.back()));
        } else {
          sawConcreteArg = true;
          specArgs.push_back(parseType());
        }
      } while (match(TokenType::COMMA));
    }
    if (currentToken().type == TokenType::RSHIFT) {
      const_cast<Token &>(currentToken()).type = TokenType::GT;
      /* Keep the original view: it spans both '>' characters, so raw type
       * strings computed from this token still cover the full '>>'. */
    } else {
      expect(TokenType::GT, "Expected '>'");
    }
    const char *rawStart = tokens[ltTok].value.data() + 1;
    const char *rawEnd = tokens[cursor - 1].value.data();
    rawTplStr = astCtx.copyString(std::string_view(rawStart, rawEnd - rawStart));

    int existingArity = astCtx.getRegisteredTemplateArity(name);
    if (sawConcreteArg) {
      /* Complete or partial specialization. */
      if (existingArity == -1) {
        reportError(
            line, col, currentToken().column - col,
            "Cannot specialize '" + std::string(name) +
                "': its primary template has not been declared. Declare "
                "'class " + std::string(name) + "<...>' first.");
        throw ParseException();
      }
      if (existingArity != (int)specArgs.size()) {
        reportError(line, col, currentToken().column - col,
                    "Specialization of '" + std::string(name) + "' has " +
                        std::to_string(specArgs.size()) +
                        " arguments, but the primary template has " +
                        std::to_string(existingArity) + ".");
        throw ParseException();
      }
      isSpec = true;
      isTemplateDecl = !tParams.empty();
    } else if (!specArgs.empty()) {
      /* All parameters: the primary template. (Modules are parsed more than
       * once in some pipelines, so a re-registration is silently accepted.
       * The arity map is idempotent.) */
      isTemplateDecl = true;
      astCtx.registerTemplateName(name);
      astCtx.registerTemplateArity(name, tParams.size());
    }
  }

  const Type *baseClass = nullptr;
  std::vector<const Type *> interfaces;
  std::string_view rawBaseClassStr;
  std::vector<std::string_view> rawInterfaces;

  if (kind == TypeKind::Class) {
    if (match(TokenType::EXTENDS_KW)) {
      const char *typeStart = currentToken().value.data();
      baseClass = parseType();
      const char *typeEnd =
          tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
      rawBaseClassStr = astCtx.copyString(
          std::string_view(typeStart, typeEnd - typeStart));
    }
    if (match(TokenType::IMPLEMENTS_KW)) {
      do {
        const char *typeStart = currentToken().value.data();
        interfaces.push_back(parseType());
        const char *typeEnd =
            tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
        rawInterfaces.push_back(
            astCtx.copyString(std::string_view(typeStart, typeEnd - typeStart)));
      } while (match(TokenType::COMMA));
    }
  }

  std::string fqNameStr = getFQName(name);
  std::string_view fqName = astCtx.copyString(fqNameStr);
  std::string_view recordNameSv = fqName;
  if (isSpec) {
    /* Complete specializations get the canonical instantiation name
     * ('List_int32'), so resolveIfTemplate finds the record by the same
     * mangled lookup it uses for instantiated primaries. */
    specMangledName = mangleTemplateName(fqNameStr, specArgs);
    recordNameSv = astCtx.copyString(specMangledName);
  }
  RecordType *recordTy = astCtx.createRecordType(kind, recordNameSv);

  if (match(TokenType::SEMICOLON)) {
    DeclNode *node = nullptr;
    int len = currentToken().column - col;

    if (kind == TypeKind::Class) {
      auto cNode = astCtx.create<ClassDeclNode>(name, line, col, len);
      cNode->fqName = fqName;
      cNode->isOpaque = true;
      cNode->isAbstract = isAbstract;
      cNode->isFinal = isFinal;
      cNode->recordType = recordTy;
      cNode->baseClass = baseClass;
      cNode->interfaces = astCtx.copyArray<const Type *>(interfaces);
      cNode->rawBaseClassStr = rawBaseClassStr;
      cNode->rawInterfaces = astCtx.copyArray<std::string_view>(rawInterfaces);
      node = cNode;
    } else if (kind == TypeKind::Struct) {
      auto sNode = astCtx.create<StructDeclNode>(name, line, col, len);
      sNode->fqName = fqName;
      sNode->isOpaque = true;
      sNode->recordType = recordTy;
      node = sNode;
    } else {
      auto uNode = astCtx.create<UnionDeclNode>(name, line, col, len);
      uNode->fqName = fqName;
      uNode->isOpaque = true;
      uNode->recordType = recordTy;
      node = uNode;
    }

    node->identifierColumn = idCol;
    node->identifierLength = idLen;

    popTemplateParams(tParams.size());
    return node;
  }

  recordTy->setOpaque(false);
  expect(TokenType::LBRACE, "Expected '{'");

  std::vector<VarDeclNode *> fields;
  std::vector<FunctionDeclNode *> methods;
  std::vector<FunctionDeclNode *> constructors;
  FunctionDeclNode *destructor = nullptr;

  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {

    std::string doc = consumeComments();
    auto memberAnnotations = parseAnnotations();

    bool isPub = false, isPriv = false, isProt = false;
    while (currentToken().type == TokenType::PUBLIC_KW ||
           currentToken().type == TokenType::PRIVATE_KW ||
           currentToken().type == TokenType::PROTECTED_KW) {
      if (currentToken().type == TokenType::PUBLIC_KW)
        isPub = true;
      if (currentToken().type == TokenType::PRIVATE_KW)
        isPriv = true;
      if (currentToken().type == TokenType::PROTECTED_KW)
        isProt = true;
      advance();
    }

    if (currentToken().type == TokenType::RBRACE ||
        currentToken().type == TokenType::EOF_TOK) {
      if (!memberAnnotations.empty()) {
        reportError(currentToken().line, currentToken().column,
                    (int)currentToken().value.length(),
                    "Annotations must be attached to a declaration.");
      }
      break;
    }

    if (currentToken().type == TokenType::TILDE) {
      int dLine = currentToken().line;
      int dCol = currentToken().column;

      advance();

      std::string_view dtorName = currentToken().value;
      int dtorIdCol = currentToken().column;
      int dtorIdLen = dtorName.length() + 1;
      expect(TokenType::IDENTIFIER, "Expected record name after '~'");

      if (dtorName != name) {
        reportError(dLine, dCol, dtorName.length(),
                    "Destructor name must match the record name.");
        throw ParseException();
      }

      expect(TokenType::LPAREN, "Expected '('");
      expect(TokenType::RPAREN, "Expected ')'");

      if (destructor != nullptr) {
        reportError(dLine, dCol, dtorName.length(),
                    "Redefinition of destructor.");
        throw ParseException();
      }

      destructor = astCtx.create<FunctionDeclNode>(astCtx.VoidTy, "~", dLine,
                                                   dCol, false, true);
      destructor->parentRecord = recordTy;
      destructor->annotations = memberAnnotations;
      destructor->hasPublicMod = isPub;
      destructor->hasPrivateMod = isPriv;
      destructor->hasProtectedMod = isProt;
      destructor->identifierColumn = dCol;
      destructor->identifierLength = dtorIdLen;

      if (!doc.empty())
        destructor->docString = astCtx.copyString(doc);
      if (cursor > 0 && !tokens[cursor - 1].trailingComment.empty()) {
        destructor->trailingComment = tokens[cursor - 1].trailingComment;
      }

      destructor->body = parseFunctionBody(astCtx.VoidTy);
      destructor->length =
          destructor->body->column + destructor->body->length - dCol;
      destructor->endLine = destructor->body->endLine;
      continue;
    }

    bool isConstCtor = false;
    bool isNamedCtor = false;
    std::string_view ctorSimpleName = name;
    int ctorIdCol = currentToken().column;

    /* Default constructor: 'Foo(' or 'const Foo('. */
    bool defaultCtorStart =
        currentToken().type == TokenType::IDENTIFIER &&
        currentToken().value == name && peekToken().type == TokenType::LPAREN;
    bool constDefaultCtorStart =
        currentToken().type == TokenType::CONST_KW &&
        peekToken().type == TokenType::IDENTIFIER &&
        peekToken().value == name && peekToken(2).type == TokenType::LPAREN;

    /* Dart-style named constructor: 'Foo.name(' or 'const Foo.name('. */
    bool namedCtorStart =
        currentToken().type == TokenType::IDENTIFIER &&
        currentToken().value == name && peekToken().type == TokenType::DOT &&
        peekToken(2).type == TokenType::IDENTIFIER &&
        peekToken(3).type == TokenType::LPAREN;
    bool constNamedCtorStart =
        currentToken().type == TokenType::CONST_KW &&
        peekToken().type == TokenType::IDENTIFIER &&
        peekToken().value == name && peekToken(2).type == TokenType::DOT &&
        peekToken(3).type == TokenType::IDENTIFIER &&
        peekToken(4).type == TokenType::LPAREN;

    if (constDefaultCtorStart || constNamedCtorStart) {
      isConstCtor = true;
      ctorIdCol = peekToken().column;
    }
    if (namedCtorStart || constNamedCtorStart) {
      isNamedCtor = true;
      /* 'Foo.name(' -> name at peek(2); 'const Foo.name(' -> peek(3). */
      ctorSimpleName =
          constNamedCtorStart ? peekToken(3).value : peekToken(2).value;
    }

    if (isConstCtor || defaultCtorStart || namedCtorStart) {
      int cLine = currentToken().line;
      int cCol = currentToken().column;

      if (isConstCtor)
        advance();
      advance();
      if (isNamedCtor) {
        advance(); /* '.' */
        advance(); /* the constructor name */
      }
      advance(); /* '(' */

      bool isVariadic = false;
      bool hasTrailingComma = false;
      auto params = parseParameterList(isVariadic, hasTrailingComma);
      expect(TokenType::RPAREN, "Expected ')'");

      FunctionCallNode *superCall = nullptr;
      std::vector<AssignNode *> fieldInits;
      if (match(TokenType::COLON)) {
        bool sawSuper = false;
        do {
          if (currentToken().type == TokenType::SUPER_KW) {
            if (sawSuper) {
              reportError(currentToken().line, currentToken().column,
                          currentToken().value.length(),
                          "Only one 'super(...)' call is allowed in the "
                          "initializer list.");
              throw ParseException();
            }
            sawSuper = true;

            int superLine = currentToken().line;
            int superCol = currentToken().column;
            expect(TokenType::SUPER_KW, "Expected 'super' after ':'");
            auto superVar =
                astCtx.create<VariableNode>("super", superLine, superCol, 5);

            expect(TokenType::LPAREN, "Expected '(' after 'super'");

            std::vector<ExprNode *> args;
            std::vector<std::string_view> argNames;
            bool namedStarted = false;
            bool superTrailingComma = false;

            if (currentToken().type != TokenType::RPAREN) {
              do {
                if (currentToken().type == TokenType::RPAREN) {
                  superTrailingComma = true;
                  break;
                }

                if (currentToken().type == TokenType::IDENTIFIER &&
                    peekToken().type == TokenType::COLON) {
                  namedStarted = true;
                  argNames.push_back(currentToken().value);
                  advance();
                  advance();
                  args.push_back(parseExpression());
                } else {
                  if (namedStarted) {
                    reportError(currentToken().line, currentToken().column,
                                currentToken().value.length(),
                                "Positional arguments cannot appear after "
                                "named arguments.");
                    throw ParseException();
                  }
                  argNames.push_back("");
                  args.push_back(parseExpression());
                }

              } while (match(TokenType::COMMA));
            }

            int endCol =
                currentToken().column + (int)currentToken().value.length();
            expect(TokenType::RPAREN, "Expected ')'");

            auto argsRef = astCtx.copyArray<ExprNode *>(args);
            auto namesRef = astCtx.copyArray<std::string_view>(argNames);
            superCall = astCtx.create<FunctionCallNode>(
                superVar, argsRef, namesRef, superLine, superCol,
                endCol - superCol);
            superCall->rawArgs = argsRef;
            superCall->rawArgNames = namesRef;
            superCall->hasRawArgs = true;
            superCall->hasTrailingComma = superTrailingComma;
            superCall->isSuperCall = true;

            /* Dart requires the super call to be the last initializer. */
            if (currentToken().type == TokenType::COMMA) {
              reportError(currentToken().line, currentToken().column,
                          currentToken().value.length(),
                          "The 'super(...)' call must be the last entry in "
                          "the initializer list.");
              throw ParseException();
            }
            break;
          }

          if (sawSuper) {
            reportError(currentToken().line, currentToken().column,
                        currentToken().value.length(),
                        "Field initializers cannot appear after the "
                        "'super(...)' call.");
            throw ParseException();
          }

          /* ': this.field = expr' field initializer. */
          int fiLine = currentToken().line;
          int fiCol = currentToken().column;
          expect(TokenType::THIS_KW, "Expected 'this.field = expr' or "
                                     "'super(...)' in initializer list");
          expect(TokenType::DOT, "Expected '.' after 'this'");
          std::string_view fiName = currentToken().value;
          expect(TokenType::IDENTIFIER, "Expected field name after 'this.'");
          int fiIdCol = currentToken().column - (int)fiName.length();
          expect(TokenType::ASSIGN, "Expected '=' after field name");
          auto fiValue = parseExpression();
          int fiEndCol =
              currentToken().column + (int)currentToken().value.length();

          auto thisVar = astCtx.create<VariableNode>("this", fiLine, fiCol, 4);
          auto fieldAccess = astCtx.create<MemberAccessNode>(
              thisVar, fiName, fiIdCol, fiCol, fiEndCol - fiCol);
          auto fieldAssign = astCtx.create<AssignNode>(
              "=", fieldAccess, fiValue, fiLine, fiCol, fiEndCol - fiCol);
          fieldAssign->isFieldInit = true;
          fieldInits.push_back(fieldAssign);
        } while (match(TokenType::COMMA));
      }

      auto constructor =
          astCtx.create<FunctionDeclNode>(astCtx.VoidTy, ctorSimpleName, cLine,
                                          cCol, isConstCtor, true, false,
                                          isVariadic);
      constructor->isNamedCtor = isNamedCtor;
      constructor->superCall = superCall;
      constructor->fieldInitializers =
          astCtx.copyArray<AssignNode *>(fieldInits);
      constructor->parentRecord = recordTy;
      constructor->params = astCtx.copyArray<ParamDeclNode *>(params);
      constructor->annotations = memberAnnotations;
      constructor->hasPublicMod = isPub;
      constructor->hasPrivateMod = isPriv;
      constructor->hasProtectedMod = isProt;
      constructor->identifierColumn = ctorIdCol;
      constructor->identifierLength = ctorSimpleName.length();
      constructor->hasTrailingComma = hasTrailingComma;

      if (!doc.empty())
        constructor->docString = astCtx.copyString(doc);
      if (cursor > 0 && !tokens[cursor - 1].trailingComment.empty()) {
        constructor->trailingComment = tokens[cursor - 1].trailingComment;
      }

      constructor->body = parseFunctionBody(astCtx.VoidTy);
      constructor->length =
          constructor->body->column + constructor->body->length - cCol;
      constructor->endLine = constructor->body->endLine;
      checkConstructorRedefinition(constructors, constructor, cLine, cCol,
                                   name.length());
      constructors.push_back(constructor);
      continue;
    }

    bool isStatic = false;
    bool isExtern = false;
    bool isIntrinsic = false;
    bool isFinalField = false;

    while (currentToken().type == TokenType::STATIC_KW ||
           currentToken().type == TokenType::FINAL_KW) {
      if (currentToken().type == TokenType::STATIC_KW)
        isStatic = true;
      else
        isFinalField = true;
      advance();
    }

    for (const auto *ann : memberAnnotations) {
      if (ann->name == "extern") {
        isExtern = true;
      } else if (ann->name == "intrinsic") {
        isIntrinsic = true;
      }
    }

    int mLine = currentToken().line;
    int mCol = currentToken().column;

    const char *typeStart = currentToken().value.data();
    const Type *memType = parseType();
    const char *typeEnd =
        tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
    std::string_view rawTypeStr(typeStart, typeEnd - typeStart);

    std::string_view memName;
    int memIdCol = currentToken().column;
    int memIdLen = 0;

    if (match(TokenType::OPERATOR_KW)) {
      memName = parseOperatorName();
      memIdLen = memName.length();
    } else {
      if (currentToken().type == TokenType::LPAREN) {
        reportError(currentToken().line, currentToken().column, 1,
                    "Missing return type for method '" + memType->toString() +
                        "'.");
        throw ParseException();
      }
      memName = currentToken().value;
      memIdLen = memName.length();
      expect(TokenType::IDENTIFIER, "Expected member name");
    }

    std::vector<std::string_view> methodTParams;
    std::vector<TemplateConstraint> methodTConstraints;
    if (match(TokenType::LT)) {
      astCtx.registerMemberTemplateName(memName);
      if (currentToken().type != TokenType::GT) {
        do {
          for (auto tp : methodTParams) {
            if (tp == currentToken().value) {
              reportError(currentToken().line, currentToken().column,
                          currentToken().value.length(),
                          "Redefinition of template parameter '" +
                              std::string(tp) + "'.");
              throw ParseException();
            }
          }
          methodTParams.push_back(currentToken().value);
          pushTemplateParam(methodTParams.back());
          expect(TokenType::IDENTIFIER, "Expected template parameter name");
          TemplateConstraint tc;
          if (match(TokenType::EXTENDS_KW)) {
            tc = parseTemplateConstraint();
          }
          methodTConstraints.push_back(tc);
        } while (match(TokenType::COMMA));
      }
      if (currentToken().type == TokenType::RSHIFT) {
        const_cast<Token &>(currentToken()).type = TokenType::GT;
        /* Keep the original view so raw strings cover the full '>>'. */
      } else {
        expect(TokenType::GT, "Expected '>'");
      }
    }

    if (match(TokenType::LPAREN)) {
      if (isFinalField) {
        reportError(mLine, mCol, memName.length(),
                    "The 'final' modifier cannot be applied to a method.");
        throw ParseException();
      }

      bool isVariadic = false;
      bool hasTrailingComma = false;
      auto params = parseParameterList(isVariadic, hasTrailingComma);
      expect(TokenType::RPAREN, "Expected ')'");

      bool isAsyncMethod = match(TokenType::ASYNC_KW);
      if (isAsyncMethod && !asyncEnabled) {
        reportError(currentToken().line, currentToken().column, 5,
                    "'async' methods are disabled for this build (async "
                    "support is turned off).");
        throw ParseException();
      }

      auto method = astCtx.create<FunctionDeclNode>(
          memType, memName, mLine, mCol, false, true, isExtern, isVariadic);
      method->parentRecord = recordTy;
      method->isStatic = isStatic;
      method->isAsync = isAsyncMethod;
      method->params = astCtx.copyArray<ParamDeclNode *>(params);
      method->annotations = memberAnnotations;
      method->hasPublicMod = isPub;
      method->hasPrivateMod = isPriv;
      method->hasProtectedMod = isProt;
      method->rawReturnTypeStr = rawTypeStr;
      method->identifierColumn = memIdCol;
      method->identifierLength = memIdLen;
      method->hasTrailingComma = hasTrailingComma;

      if (!doc.empty())
        method->docString = astCtx.copyString(doc);

      if (isExtern || isIntrinsic) {
        int endLine = currentToken().line;
        int endCol = currentToken().column + 1;
        expect(TokenType::SEMICOLON,
               "Expected ';' after extern or intrinsic method declaration");
        method->length = endCol - mCol;
        method->endLine = endLine;
      } else if (isAbstract && currentToken().type == TokenType::SEMICOLON) {
        int endLine = currentToken().line;
        int endCol = currentToken().column + 1;
        advance();
        method->length = endCol - mCol;
        method->endLine = endLine;
        method->isAbstract = true;
        method->body = nullptr;
      } else {
        method->body = parseFunctionBody(memType);
        method->length = method->body->column + method->body->length - mCol;
        method->endLine = method->body->endLine;
      }

      if (cursor > 0 && !tokens[cursor - 1].trailingComment.empty()) {
        method->trailingComment = tokens[cursor - 1].trailingComment;
      }

      if (!methodTParams.empty()) {
        method->isTemplate = true;
        method->templateParams =
            astCtx.copyArray<std::string_view>(methodTParams);
        method->templateConstraints =
            astCtx.copyArray<TemplateConstraint>(methodTConstraints);
        popTemplateParams(methodTParams.size());
      }

      checkRecordMemberRedefinition(memName, fields, methods, method, mLine,
                                    mCol, memName.length());
      methods.push_back(method);
    } else {
      if (!methodTParams.empty()) {
        reportError(mLine, mCol, memName.length(),
                    "Variables cannot have template parameters.");
        throw ParseException();
      }
      if (isExtern) {
        reportError(mLine, mCol, memName.length(),
                    "Variables cannot be declared as extern.");
        throw ParseException();
      }
      if (isIntrinsic) {
        reportError(mLine, mCol, memName.length(),
                    "Variables cannot be declared as intrinsic.");
        throw ParseException();
      }

      ExprNode *init = nullptr;
      if (match(TokenType::ASSIGN)) {
        init = parseExpression();
      }
      int endLine = currentToken().line;
      int endCol = currentToken().column + 1;
      expect(TokenType::SEMICOLON, "Expected ';'");

      checkRecordMemberRedefinition(memName, fields, methods, nullptr, mLine,
                                    mCol, memName.length());

      auto field = astCtx.create<VarDeclNode>(memType, memName, init, mLine,
                                              mCol, endCol - mCol);
      field->isStatic = isStatic;
      field->isFinal = isFinalField;
      field->annotations = memberAnnotations;
      field->hasPublicMod = isPub;
      field->hasPrivateMod = isPriv;
      field->hasProtectedMod = isProt;
      field->rawTypeStr = rawTypeStr;
      field->endLine = endLine;
      field->identifierColumn = memIdCol;
      field->identifierLength = memIdLen;

      if (!doc.empty())
        field->docString = astCtx.copyString(doc);
      if (cursor > 0 && !tokens[cursor - 1].trailingComment.empty()) {
        field->trailingComment = tokens[cursor - 1].trailingComment;
      }

      fields.push_back(field);
    }
  }

  if (constructors.empty()) {
    auto defaultCtor = astCtx.create<FunctionDeclNode>(
        astCtx.VoidTy, name, line, col, false, true, false, false, true);
    defaultCtor->parentRecord = recordTy;

    auto emptyBody = astCtx.create<BlockNode>(line, col);
    emptyBody->statements = {};
    emptyBody->length = 0;
    defaultCtor->body = emptyBody;

    constructors.push_back(defaultCtor);
  }

  if (!destructor) {
    destructor = astCtx.create<FunctionDeclNode>(
        astCtx.VoidTy, "~", line, col, false, true, false, false, true);
    destructor->parentRecord = recordTy;

    auto emptyBody = astCtx.create<BlockNode>(line, col);
    emptyBody->statements = {};
    emptyBody->length = 0;
    destructor->body = emptyBody;
  }

  int endLine = currentToken().line;
  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACE, "Expected '}'");

  std::vector<FieldInfo> fInfos;
  uint32_t instanceFieldIndex = 0;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i]->isStatic)
      continue;
    fInfos.push_back({fields[i]->varName, fields[i]->type, instanceFieldIndex++,
                      fields[i]->isPublic(fields[i]->varName),
                      fields[i]->isProtected(fields[i]->varName)});
  }
  recordTy->setFields(astCtx.copyArray<FieldInfo>(fInfos));

  DeclNode *node = nullptr;
  int len = currentToken().column - col;

  if (kind == TypeKind::Class) {
    auto cNode = astCtx.create<ClassDeclNode>(name, line, col, len);
    cNode->fqName = fqName;
    cNode->fields = astCtx.copyArray<VarDeclNode *>(fields);
    cNode->methods = astCtx.copyArray<FunctionDeclNode *>(methods);
    cNode->constructors = astCtx.copyArray<FunctionDeclNode *>(constructors);
    cNode->destructor = destructor;
    cNode->baseClass = baseClass;
    cNode->interfaces = astCtx.copyArray<const Type *>(interfaces);
      cNode->rawBaseClassStr = rawBaseClassStr;
      cNode->rawInterfaces = astCtx.copyArray<std::string_view>(rawInterfaces);
    cNode->endLine = endLine;
    cNode->recordType = recordTy;
    cNode->isAbstract = isAbstract;
    cNode->isFinal = isFinal;
    if (!tParams.empty()) {
      cNode->isTemplate = true;
      cNode->templateParams = astCtx.copyArray<std::string_view>(tParams);
    }
    cNode->isTemplate = isTemplateDecl;
    cNode->templateConstraints =
        astCtx.copyArray<TemplateConstraint>(tConstraints);
    if (isSpec) {
      cNode->isTemplateSpecialization = true;
      cNode->specializationArgs = astCtx.copyArray<const Type *>(specArgs);
      cNode->specializationBaseName = fqName;
      cNode->fqName = recordNameSv;
    }
    cNode->rawTemplateListStr = rawTplStr;
    node = cNode;
  } else if (kind == TypeKind::Struct) {
    auto sNode = astCtx.create<StructDeclNode>(name, line, col, len);
    sNode->fqName = fqName;
    sNode->fields = astCtx.copyArray<VarDeclNode *>(fields);
    sNode->methods = astCtx.copyArray<FunctionDeclNode *>(methods);
    sNode->constructors = astCtx.copyArray<FunctionDeclNode *>(constructors);
    sNode->destructor = destructor;
    sNode->endLine = endLine;
    sNode->recordType = recordTy;
    if (!tParams.empty()) {
      sNode->isTemplate = true;
      sNode->templateParams = astCtx.copyArray<std::string_view>(tParams);
    }
    sNode->isTemplate = isTemplateDecl;
    sNode->templateConstraints =
        astCtx.copyArray<TemplateConstraint>(tConstraints);
    if (isSpec) {
      sNode->isTemplateSpecialization = true;
      sNode->specializationArgs = astCtx.copyArray<const Type *>(specArgs);
      sNode->specializationBaseName = fqName;
      sNode->fqName = recordNameSv;
    }
    sNode->rawTemplateListStr = rawTplStr;
    node = sNode;
  } else {
    auto uNode = astCtx.create<UnionDeclNode>(name, line, col, len);
    uNode->fqName = fqName;
    uNode->fields = astCtx.copyArray<VarDeclNode *>(fields);
    uNode->methods = astCtx.copyArray<FunctionDeclNode *>(methods);
    uNode->constructors = astCtx.copyArray<FunctionDeclNode *>(constructors);
    uNode->destructor = destructor;
    uNode->endLine = endLine;
    uNode->recordType = recordTy;
    if (!tParams.empty()) {
      uNode->isTemplate = true;
      uNode->templateParams = astCtx.copyArray<std::string_view>(tParams);
    }
    uNode->isTemplate = isTemplateDecl;
    uNode->templateConstraints =
        astCtx.copyArray<TemplateConstraint>(tConstraints);
    if (isSpec) {
      uNode->isTemplateSpecialization = true;
      uNode->specializationArgs = astCtx.copyArray<const Type *>(specArgs);
      uNode->specializationBaseName = fqName;
      uNode->fqName = recordNameSv;
    }
    uNode->rawTemplateListStr = rawTplStr;
    node = uNode;
  }

  node->identifierColumn = idCol;
  node->identifierLength = idLen;

  popTemplateParams(tParams.size());
  return node;
}

DeclNode *Parser::parseEnumDecl() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  std::string_view name = currentToken().value;
  int idCol = currentToken().column;
  int idLen = name.length();
  expect(TokenType::IDENTIFIER, "Expected enum name");

  const Type *underlyingType = astCtx.Int32Ty;
  if (match(TokenType::COLON)) {
    underlyingType = parseType();
    if (!underlyingType->isInteger()) {
      reportError(line, col, currentToken().column - col,
                  "Enum underlying type must be an integer type");
      throw ParseException();
    }
  }

  std::string fqNameStr = getFQName(name);
  std::string_view fqName = astCtx.copyString(fqNameStr);

  /* Eagerly register the enum type so subsequent parameters/variables can use
   * it as a valid type */
  astCtx.getEnumType(fqName, underlyingType);

  expect(TokenType::LBRACE, "Expected '{'");

  std::vector<EnumMemberNode *> members;
  bool hasTrailingComma = false;

  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {

    if (currentToken().type == TokenType::RBRACE)
      break;

    bool isPub = false, isPriv = false, isProt = false;
    while (currentToken().type == TokenType::PUBLIC_KW ||
           currentToken().type == TokenType::PRIVATE_KW ||
           currentToken().type == TokenType::PROTECTED_KW) {
      if (currentToken().type == TokenType::PUBLIC_KW)
        isPub = true;
      if (currentToken().type == TokenType::PRIVATE_KW)
        isPriv = true;
      if (currentToken().type == TokenType::PROTECTED_KW)
        isProt = true;
      advance();
    }

    std::string doc = consumeComments();

    int mLine = currentToken().line;
    int mCol = currentToken().column;
    std::string_view mName = currentToken().value;
    int midCol = currentToken().column;
    int midLen = mName.length();
    expect(TokenType::IDENTIFIER, "Expected enum member name");

    for (auto *existing : members) {
      if (existing->name == mName) {
        reportError(mLine, mCol, currentToken().column - mCol,
                    "Redefinition of enum member '" + std::string(mName) +
                        "'.");
        throw ParseException();
      }
    }

    ExprNode *init = nullptr;
    if (match(TokenType::ASSIGN)) {
      init = parseExpression();
    }

    int endLine = init ? init->endLine : mLine;
    int endCol = init ? (init->column + init->length) : currentToken().column;

    auto memberNode =
        astCtx.create<EnumMemberNode>(mName, init, mLine, mCol, endCol - mCol);
    memberNode->endLine = endLine;
    memberNode->hasPublicMod = isPub;
    memberNode->hasPrivateMod = isPriv;
    memberNode->hasProtectedMod = isProt;
    memberNode->identifierColumn = midCol;
    memberNode->identifierLength = midLen;

    if (!doc.empty())
      memberNode->docString = astCtx.copyString(doc);
    if (cursor > 0 && !tokens[cursor - 1].trailingComment.empty()) {
      memberNode->trailingComment = tokens[cursor - 1].trailingComment;
    }

    members.push_back(memberNode);

    if (!match(TokenType::COMMA)) {
      break;
    } else if (currentToken().type == TokenType::RBRACE) {
      hasTrailingComma = true;
      break;
    }
  }

  std::string closingDoc = consumeComments();

  int endLine = currentToken().line;
  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACE, "Expected '}'");

  if (!closingDoc.empty() && !members.empty()) {
    ASTNode *lastMember = members.back();
    std::string combined;
    if (!lastMember->trailingComment.empty()) {
      combined = std::string(lastMember->trailingComment) + "\n" + closingDoc;
    } else {
      combined = "\n" + closingDoc;
    }
    lastMember->trailingComment = astCtx.copyString(combined);
  }

  auto node = astCtx.create<EnumDeclNode>(name, underlyingType, line, col,
                                          endCol - col);
  node->fqName = fqName;
  node->members = astCtx.copyArray<EnumMemberNode *>(members);
  /* Register with the fully qualified name: the type system deduplicates
   * EnumType by name, and keeping a second instance under the simple name
   * makes the simple-name lookup shadow the namespace-qualified one
   * (breaking enum assignability inside namespaces). */
  node->enumType = astCtx.getEnumType(fqName, underlyingType);
  node->endLine = endLine;
  node->identifierColumn = idCol;
  node->identifierLength = idLen;
  node->hasTrailingComma = hasTrailingComma;

  return node;
}

DeclNode *Parser::parseDeclarationOrFunction(
    llvm::ArrayRef<AnnotationNode *> annotations) {
  int line = currentToken().line;
  int col = currentToken().column;

  bool isExtern = false;
  bool isStatic = false;
  bool isIntrinsic = false;
  bool isFinalDecl = false;

  while (currentToken().type == TokenType::STATIC_KW ||
         currentToken().type == TokenType::FINAL_KW) {
    if (currentToken().type == TokenType::STATIC_KW)
      isStatic = true;
    else
      isFinalDecl = true;
    advance();
  }

  for (const auto *ann : annotations) {
    if (ann->name == "extern") {
      isExtern = true;
    } else if (ann->name == "intrinsic") {
      isIntrinsic = true;
    }
  }

  const Type *nodeType = nullptr;
  std::string_view rawTypeStr;

  bool isImplicitlyTyped = false;
  bool isConstDecl = false;

  if (match(TokenType::VAR_KW)) {
    if (isFinalDecl) {
      reportError(line, col, 5,
                  "The 'var' and 'final' modifiers cannot be combined.");
      throw ParseException();
    }
    isImplicitlyTyped = true;
    nodeType = astCtx.AutoTy;
    rawTypeStr = "var";
  } else if (currentToken().type == TokenType::CONST_KW &&
             peekToken().type == TokenType::IDENTIFIER &&
             (peekToken(2).type == TokenType::ASSIGN ||
              peekToken(2).type == TokenType::SEMICOLON)) {
    if (isFinalDecl) {
      reportError(line, col, 5,
                  "The 'const' and 'final' modifiers cannot be combined.");
      throw ParseException();
    }
    advance();
    isImplicitlyTyped = true;
    isConstDecl = true;
    nodeType = astCtx.getConstType(astCtx.AutoTy);
    rawTypeStr = "const";
  } else if (isFinalDecl && currentToken().type == TokenType::IDENTIFIER &&
             (peekToken().type == TokenType::ASSIGN ||
              peekToken().type == TokenType::SEMICOLON)) {
    /* 'final x = 5;': type inferred from the initializer, like 'var'. */
    isImplicitlyTyped = true;
    nodeType = astCtx.AutoTy;
    rawTypeStr = "final";
  }

  if (!isImplicitlyTyped) {
    const char *typeStart = currentToken().value.data();
    nodeType = parseType();
    const char *typeEnd =
        tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
    rawTypeStr = std::string_view(typeStart, typeEnd - typeStart);
  }

  std::string_view id;
  int idCol = currentToken().column;
  int idLen = 0;

  if (match(TokenType::OPERATOR_KW)) {
    id = parseOperatorName();
    idLen = id.length();
  } else {
    if (currentToken().type == TokenType::LPAREN) {
      reportError(currentToken().line, currentToken().column, 1,
                  "Missing return type for function '" + nodeType->toString() +
                      "'.");
      throw ParseException();
    }
    id = currentToken().value;
    idLen = id.length();
    expect(TokenType::IDENTIFIER, "Expected identifier after type");
  }

  std::vector<std::string_view> tParams;
  std::vector<TemplateConstraint> tConstraints;
  if (match(TokenType::LT)) {
    astCtx.registerTemplateName(id);
    if (currentToken().type != TokenType::GT) {
      do {
        for (auto tp : tParams) {
          if (tp == currentToken().value) {
            reportError(currentToken().line, currentToken().column,
                        currentToken().value.length(),
                        "Redefinition of template parameter '" +
                            std::string(tp) + "'.");
            throw ParseException();
          }
        }
        tParams.push_back(currentToken().value);
        pushTemplateParam(tParams.back());
        expect(TokenType::IDENTIFIER, "Expected template parameter name");
        TemplateConstraint tc;
        if (match(TokenType::EXTENDS_KW)) {
          tc = parseTemplateConstraint();
        }
        tConstraints.push_back(tc);
      } while (match(TokenType::COMMA));
    }
    if (currentToken().type == TokenType::RSHIFT) {
      const_cast<Token &>(currentToken()).type = TokenType::GT;
      /* Keep the original view: it spans both '>' characters, so raw type
       * strings computed from this token still cover the full '>>'. */
    } else {
      expect(TokenType::GT, "Expected '>'");
    }
  }

  std::string fqNameStr = getFQName(id);
  std::string_view fqName = astCtx.copyString(fqNameStr);

  /*
   * Disambiguate the C++ "most vexing parse": 'Type name(expr...)' is a
   * variable declaration with a constructor-call initializer whenever the
   * tokens inside the parentheses cannot form a parameter list, e.g.
   * 'unique_ptr<Foo> up(new Foo());'. A leading type (keyword or
   * 'Type name' pair) keeps the declaration a function declaration.
   */
  bool isVarWithCtorCall = false;
  if (currentToken().type == TokenType::LPAREN && tParams.empty() &&
      !isImplicitlyTyped) {
    TokenType firstTok = peekToken(1).type;
    if (firstTok == TokenType::IDENTIFIER) {
      TokenType secondTok = peekToken(2).type;
      if (secondTok == TokenType::IDENTIFIER ||
          secondTok == TokenType::STAR || secondTok == TokenType::AMPERSAND ||
          secondTok == TokenType::LOGICAL_AND ||
          secondTok == TokenType::COMMA || secondTok == TokenType::LT ||
          secondTok == TokenType::RSHIFT) {
        isVarWithCtorCall = false;
      } else {
        isVarWithCtorCall = true;
      }
    } else if (firstTok != TokenType::RPAREN &&
               firstTok != TokenType::TYPE_KW &&
               firstTok != TokenType::CONST_KW &&
               firstTok != TokenType::VAR_KW &&
               firstTok != TokenType::STATIC_KW &&
               firstTok != TokenType::LBRACE) {
      isVarWithCtorCall = true;
    }
  }

  if (isVarWithCtorCall) {
    std::vector<ExprNode *> args;
    std::vector<std::string_view> argNames;
    bool hasTrailingComma = false;

    expect(TokenType::LPAREN, "Expected '('");
    if (currentToken().type != TokenType::RPAREN) {
      do {
        if (currentToken().type == TokenType::RPAREN) {
          hasTrailingComma = true;
          break;
        }
        if (currentToken().type == TokenType::IDENTIFIER &&
            peekToken().type == TokenType::COLON) {
          argNames.push_back(currentToken().value);
          advance();
          advance();
          args.push_back(parseExpression());
        } else {
          argNames.push_back("");
          args.push_back(parseExpression());
        }
      } while (match(TokenType::COMMA));
    }

    int callEndLine = currentToken().line;
    int callEndCol = currentToken().column + (int)currentToken().value.length();
    expect(TokenType::RPAREN, "Expected ')'");

    auto argsRef = astCtx.copyArray<ExprNode *>(args);
    auto namesRef = astCtx.copyArray<std::string_view>(argNames);

    /* The constructor-call initializer targets the declared type, e.g.
     * 'Text t1("hello")' becomes 't1 = Text("hello")'. Template types keep
     * their base name and template arguments so the Sema phase can resolve
     * the instantiated constructor. */
    ExprNode *target = nullptr;
    const Type *baseType = nodeType->getUnqualifiedType();
    if (auto *instTy = llvm::dyn_cast<TemplateInstType>(baseType)) {
      auto *tVar =
          astCtx.create<VariableNode>(instTy->getBaseName(), line, col, idLen);
      tVar->templateArgs =
          astCtx.copyArray<const Type *>(instTy->getTemplateArgs());
      target = tVar;
    } else {
      std::string_view targetName =
          astCtx.copyString(baseType->toString());
      target =
          astCtx.create<VariableNode>(targetName, idCol, col, idLen);
    }

    auto callNode = astCtx.create<FunctionCallNode>(
        target, argsRef, namesRef, line, col, callEndCol - col);
    callNode->endLine = callEndLine;

    ExprNode *init = callNode;
    int endLine = currentToken().line;
    int endCol = currentToken().column + (int)currentToken().value.length();
    expect(TokenType::SEMICOLON, "Expected ';' after variable declaration");

    auto varDecl =
        astCtx.create<VarDeclNode>(nodeType, id, init, line, col, endCol - col);
    varDecl->fqName = fqName;
    varDecl->rawTypeStr = rawTypeStr;
    varDecl->endLine = endLine;
    varDecl->identifierColumn = idCol;
    varDecl->identifierLength = idLen;
    varDecl->isStatic = isStatic;
    varDecl->isFinal = isFinalDecl;

    return varDecl;
  }

  if (match(TokenType::LPAREN)) {
    if (isFinalDecl) {
      reportError(line, col, idLen,
                  "The 'final' modifier is strictly permitted on class "
                  "declarations and variables only.");
      throw ParseException();
    }

    bool isVariadic = false;
    bool hasTrailingComma = false;
    auto params = parseParameterList(isVariadic, hasTrailingComma);
    expect(TokenType::RPAREN, "Expected ')' after parameters");

    bool isAsync = match(TokenType::ASYNC_KW);
    if (isAsync && !asyncEnabled) {
      reportError(currentToken().line, currentToken().column, 5,
                  "'async' functions are disabled for this build (async "
                  "support is turned off).");
      throw ParseException();
    }
    bool isFuncConst = match(TokenType::CONST_KW);

    auto funcDecl = astCtx.create<FunctionDeclNode>(
        nodeType, id, line, col, isFuncConst, false, isExtern, isVariadic);
    funcDecl->fqName = fqName;
    funcDecl->isStatic = isStatic;
    funcDecl->isAsync = isAsync;
    funcDecl->params = astCtx.copyArray<ParamDeclNode *>(params);
    funcDecl->rawReturnTypeStr = rawTypeStr;
    funcDecl->identifierColumn = idCol;
    funcDecl->identifierLength = idLen;
    funcDecl->hasTrailingComma = hasTrailingComma;

    if (isExtern || isIntrinsic) {
      int endLine = currentToken().line;
      int endCol = currentToken().column + currentToken().value.length();
      expect(TokenType::SEMICOLON,
             "Expected ';' after extern or intrinsic function declaration");
      funcDecl->length = endCol - col;
      funcDecl->endLine = endLine;
    } else {
      funcDecl->body = parseFunctionBody(nodeType);
      funcDecl->length = funcDecl->body->column + funcDecl->body->length - col;
      funcDecl->endLine = funcDecl->body->endLine;
    }

    popTemplateParams(tParams.size());

    if (!tParams.empty()) {
      funcDecl->isTemplate = true;
      funcDecl->templateParams = astCtx.copyArray<std::string_view>(tParams);
      funcDecl->templateConstraints =
          astCtx.copyArray<TemplateConstraint>(tConstraints);
    }
    return funcDecl;
  }

  if (isIntrinsic) {
    reportError(line, col, idLen, "Variables cannot be declared as intrinsic.");
    throw ParseException();
  }

  ExprNode *init = nullptr;
  if (match(TokenType::ASSIGN)) {
    init = parseExpression();
  }
  int endLine = currentToken().line;
  int endCol = currentToken().column + (int)currentToken().value.length();
  expect(TokenType::SEMICOLON, "Expected ';' after variable declaration");

  auto varDecl =
      astCtx.create<VarDeclNode>(nodeType, id, init, line, col, endCol - col);
  varDecl->fqName = fqName;
  varDecl->rawTypeStr = rawTypeStr;
  varDecl->endLine = endLine;
  varDecl->identifierColumn = idCol;
  varDecl->identifierLength = idLen;
  varDecl->isStatic = isStatic;
  varDecl->isFinal = isFinalDecl;

  return varDecl;
}

BlockNode *Parser::parseBlock() {
  int startLine = currentToken().line;
  int startCol = currentToken().column;
  expect(TokenType::LBRACE, "Expected '{'");

  auto block = astCtx.create<BlockNode>(startLine, startCol);
  std::vector<ASTNode *> statements;

  /* Usings declared inside the block must not leak into enclosing scopes. */
  size_t prevUsings = activeUsings.size();

  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {
    try {
      if (auto stmt = parseStatement()) {
        statements.push_back(stmt);
      }
    } catch (const ParseException &) {
      synchronize();
    }
  }

  std::string closingDoc = consumeComments();

  int endLine = currentToken().line;
  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACE, "Expected '}'");

  activeUsings.resize(prevUsings);

  if (!closingDoc.empty() && !statements.empty()) {
    ASTNode *lastStmt = statements.back();
    std::string combined;
    if (!lastStmt->trailingComment.empty()) {
      combined = std::string(lastStmt->trailingComment) + "\n" + closingDoc;
    } else {
      combined = "\n" + closingDoc;
    }
    lastStmt->trailingComment = astCtx.copyString(combined);
  }

  block->statements = astCtx.copyArray<ASTNode *>(statements);
  block->finalize(endCol);
  block->endLine = endLine;
  return block;
}

ReturnNode *Parser::parseReturn() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();
  ExprNode *val = nullptr;
  if (currentToken().type != TokenType::SEMICOLON) {
    val = parseExpression();
  }
  int endLine = currentToken().line;
  int endCol = currentToken().column + (int)currentToken().value.length();
  expect(TokenType::SEMICOLON, "Expected ';'");
  auto ret = astCtx.create<ReturnNode>(val, line, col, endCol - col);
  ret->endLine = endLine;
  return ret;
}

ExprNode *Parser::parseExpressionStatement() {
  auto expr = parseExpression();
  int endLine = currentToken().line;
  expect(TokenType::SEMICOLON, "Expected ';'");
  expr->endLine = endLine;
  return expr;
}

ExprNode *Parser::parseExpression() { return parseAssignment(); }

ExprNode *Parser::parseAssignment() {
  auto expr = parseTernary();

  switch (currentToken().type) {
  case TokenType::ASSIGN:
  case TokenType::PLUS_EQ:
  case TokenType::MINUS_EQ:
  case TokenType::STAR_EQ:
  case TokenType::SLASH_EQ:
  case TokenType::PERCENT_EQ:
  case TokenType::AMPERSAND_EQ:
  case TokenType::PIPE_EQ:
  case TokenType::CARET_EQ:
  case TokenType::LSHIFT_EQ:
  case TokenType::RSHIFT_EQ: {
    int line = expr->line;
    int col = expr->column;
    std::string_view op = currentToken().value;
    advance();
    auto value = parseAssignment();
    int endCol = value->column + value->length;
    return astCtx.create<AssignNode>(op, expr, value, line, col, endCol - col);
  }
  default:
    return expr;
  }
}

ExprNode *Parser::parseArrayLiteral() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  std::vector<ExprNode *> elements;
  bool hasTrailingComma = false;
  if (currentToken().type != TokenType::RBRACKET &&
      currentToken().type != TokenType::EOF_TOK) {
    do {
      if (currentToken().type == TokenType::RBRACKET) {
        hasTrailingComma = true;
        break;
      }
      elements.push_back(parseExpression());
    } while (match(TokenType::COMMA));
  }

  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACKET, "Expected ']' at end of array literal");

  auto node = astCtx.create<ArrayLiteralNode>(
      astCtx.copyArray<ExprNode *>(elements), line, col, endCol - col);
  node->hasTrailingComma = hasTrailingComma;
  return node;
}

ExprNode *Parser::parseMapLiteral() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  std::vector<ExprNode *> keys;
  std::vector<ExprNode *> values;
  bool hasTrailingComma = false;

  if (currentToken().type != TokenType::RBRACE &&
      currentToken().type != TokenType::EOF_TOK) {
    do {
      if (currentToken().type == TokenType::RBRACE) {
        hasTrailingComma = true;
        break;
      }
      auto key = parseExpression();
      expect(TokenType::COLON,
             "Expected ':' between key and value in map literal");
      auto value = parseExpression();
      keys.push_back(key);
      values.push_back(value);
    } while (match(TokenType::COMMA));
  }

  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACE, "Expected '}' at end of map literal");

  auto node = astCtx.create<MapLiteralNode>(
      astCtx.copyArray<ExprNode *>(keys), astCtx.copyArray<ExprNode *>(values),
      line, col, endCol - col);
  node->hasTrailingComma = hasTrailingComma;
  return node;
}

ExprNode *Parser::parseTerm() {
  auto left = parseCast();
  while (currentToken().type == TokenType::STAR ||
         currentToken().type == TokenType::SLASH ||
         currentToken().type == TokenType::PERCENT) {
    int line = left->line;
    int col = left->column;
    std::string_view op = currentToken().value;
    advance();
    auto right = parseCast();
    left = astCtx.create<BinaryOpNode>(op, left, right, line, col);
  }
  return left;
}

ExprNode *Parser::parseCast() {
  auto left = parseUnary();
  while (currentToken().type == TokenType::AS) {
    int line = left->line;
    int col = left->column;
    advance();

    const char *typeStart = currentToken().value.data();
    const Type *targetType = parseType();
    const char *typeEnd =
        tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
    std::string_view rawTypeStr(typeStart, typeEnd - typeStart);

    int endCol = tokens[cursor - 1].column + tokens[cursor - 1].value.length();

    left = astCtx.create<CastNode>(left, targetType, line, col, endCol - col);
    static_cast<CastNode *>(left)->rawTargetTypeStr = rawTypeStr;
  }
  return left;
}

ExprNode *Parser::parsePostfix() {
  auto expr = parsePrimary();
  while (true) {
    if (match(TokenType::DOT)) {
      int line = expr->line;
      int col = expr->column;

      if (match(TokenType::TILDE)) {
        /* Manual destructor call: 'obj.~TypeName()' (C++ placement-new
         * companion: destroy before freeing the raw memory). */
        int dtorLine = expr->line;
        const Type *dtorTy = parseType();
        int endCol = currentToken().column + currentToken().value.length();
        expect(TokenType::LPAREN, "Expected '(' after destructor type");
        expect(TokenType::RPAREN, "Expected ')' in destructor call");
        expr = astCtx.create<DestructorCallNode>(expr, dtorTy, dtorLine, col,
                                                 endCol - col);
        continue;
      }

      std::string_view memberName = currentToken().value;
      int memLen = memberName.length();

      int memCol = currentToken().column;
      expect(TokenType::IDENTIFIER, "Expected member name after '.'");

      bool isTemplateCall = false;
      if (currentToken().type == TokenType::LT &&
          (astCtx.isTemplateName(memberName) ||
           astCtx.isMemberTemplateName(memberName))) {
        isTemplateCall = true;
      }

      auto maNode = astCtx.create<MemberAccessNode>(expr, memberName, line, col,
                                                    (memCol + memLen) - col);

      if (expr->kind == NodeKind::Variable &&
          static_cast<VariableNode *>(expr)->name == "super") {
        maNode->isSuperAccess = true;
      }

      if (isTemplateCall) {
        advance();
        std::vector<const Type *> tArgs;
        if (currentToken().type != TokenType::GT) {
          do {
            tArgs.push_back(parseType());
          } while (match(TokenType::COMMA));
        }
        if (currentToken().type == TokenType::RSHIFT) {
          const_cast<Token &>(currentToken()).type = TokenType::GT;
          /* Keep the original view so raw strings cover the full '>>'. */
        } else {
          expect(TokenType::GT, "Expected '>'");
        }
        maNode->templateArgs = astCtx.copyArray<const Type *>(tArgs);
      }

      expr = maNode;
    } else if (match(TokenType::LBRACKET)) {
      int line = expr->line;
      int col = expr->column;
      auto index = parseExpression();
      int endCol = currentToken().column + currentToken().value.length();
      expect(TokenType::RBRACKET, "Expected ']'");
      expr = astCtx.create<ArraySubscriptNode>(expr, index, line, col,
                                               endCol - col);
    } else if (match(TokenType::LPAREN)) {
      int line = expr->line;
      int col = expr->column;
      std::vector<ExprNode *> args;
      std::vector<std::string_view> argNames;
      bool namedStarted = false;
      bool hasTrailingComma = false;

      if (currentToken().type != TokenType::RPAREN) {
        do {
          if (currentToken().type == TokenType::RPAREN) {
            hasTrailingComma = true;
            break;
          }

          if (currentToken().type == TokenType::IDENTIFIER &&
              peekToken().type == TokenType::COLON) {
            namedStarted = true;
            argNames.push_back(currentToken().value);
            advance();
            advance();
            args.push_back(parseExpression());
          } else {
            if (namedStarted) {
              reportError(
                  currentToken().line, currentToken().column,
                  currentToken().value.length(),
                  "Positional arguments cannot appear after named arguments.");
              throw ParseException();
            }
            argNames.push_back("");
            args.push_back(parseExpression());

            if (currentToken().type == TokenType::IDENTIFIER) {
              reportError(
                  currentToken().line, currentToken().column,
                  currentToken().value.length(),
                  "Unexpected identifier in argument list. If this is a "
                  "function declaration, you are missing the return type.");
              throw ParseException();
            }
          }

        } while (match(TokenType::COMMA));
      }

      int endLine = currentToken().line;
      int endCol = currentToken().column + (int)currentToken().value.length();
      expect(TokenType::RPAREN, "Expected ')'");

      auto argsRef = astCtx.copyArray<ExprNode *>(args);
      auto namesRef = astCtx.copyArray<std::string_view>(argNames);
      auto callNode = astCtx.create<FunctionCallNode>(expr, argsRef, namesRef,
                                                      line, col, endCol - col);
      callNode->endLine = endLine;
      callNode->rawArgs = argsRef;
      callNode->rawArgNames = namesRef;
      callNode->hasRawArgs = true;
      callNode->hasTrailingComma = hasTrailingComma;

      if (expr->kind == NodeKind::Variable &&
          static_cast<VariableNode *>(expr)->name == "super") {
        callNode->isSuperCall = true;
      }

      expr = callNode;
    } else if (currentToken().type == TokenType::PLUS_PLUS ||
               currentToken().type == TokenType::MINUS_MINUS) {
      int line = expr->line;
      int col = expr->column;
      std::string_view op = currentToken().value;
      advance();
      expr = astCtx.create<UnaryOpNode>(op, expr, line, col, true);
    } else {
      break;
    }
  }
  return expr;
}

bool Parser::looksLikeLambdaParams(size_t openOffset) const {
  TokenType firstTok = peekToken(openOffset + 1).type;
  if (firstTok == TokenType::RPAREN)
    return true; /* () */
  if (firstTok == TokenType::TYPE_KW || firstTok == TokenType::CONST_KW ||
      firstTok == TokenType::LBRACE || firstTok == TokenType::REQUIRED_KW)
    return true; /* (int a) / (const uint8* s) / ({int x}) / ({required bool b}) */
  if (firstTok == TokenType::IDENTIFIER) {
    TokenType secondTok = peekToken(openOffset + 2).type;
    if (secondTok == TokenType::IDENTIFIER || /* (Foo a) */
        secondTok == TokenType::COMMA ||      /* (a, b) */
        secondTok == TokenType::RPAREN ||     /* (a) */
        secondTok == TokenType::STAR ||       /* (Foo* p) */
        secondTok == TokenType::AMPERSAND ||  /* (Foo& r) */
        secondTok == TokenType::LOGICAL_AND ||/* (Foo&& r) */
        secondTok == TokenType::LT ||         /* (Foo<Bar> p) */
        secondTok == TokenType::RSHIFT) {
      return true;
    }
  }
  return false;
}

bool Parser::lambdaFollowedByBody(size_t openOffset) const {
  /* Scan to the balanced ')' and check that it is followed by '=>' or '{'. */
  int depth = 0;
  size_t i = openOffset;
  while (i < tokens.size()) {
    TokenType t = peekToken(i).type;
    if (t == TokenType::LPAREN || t == TokenType::LT ||
        t == TokenType::LBRACKET || t == TokenType::LBRACE) {
      depth++;
    } else if (t == TokenType::RPAREN || t == TokenType::GT ||
               t == TokenType::RBRACKET || t == TokenType::RBRACE) {
      depth--;
      if (depth <= 0)
        break;
    }
    i++;
  }
  if (i + 1 >= tokens.size())
    return false;
  TokenType next = peekToken(i + 1).type;
  /* 'async' may appear between the parameter list and the body:
   * '(int v) async { ... }'. */
  if (next == TokenType::ASYNC_KW) {
    if (i + 2 >= tokens.size())
      return false;
    next = peekToken(i + 2).type;
  }
  return next == TokenType::ARROW || next == TokenType::LBRACE;
}

std::vector<ParamDeclNode *> Parser::parseLambdaParams() {
  bool isVariadic = false;
  bool hasTrailingComma = false;
  int vLine = currentToken().line;
  int vCol = currentToken().column;
  auto params =
      parseParameterList(isVariadic, hasTrailingComma, /*allowUntyped=*/true);
  if (isVariadic) {
    reportError(vLine, vCol, 3,
                "Variadic lambda parameters are not supported.");
    throw ParseException();
  }
  return params;
}

ExprNode *Parser::parseLambda(const Type *explicitReturnType) {
  int line = currentToken().line;
  int col = currentToken().column;

  expect(TokenType::LPAREN, "Expected '(' to start lambda parameters");
  auto params = parseLambdaParams();
  expect(TokenType::RPAREN, "Expected ')' after lambda parameters");

  auto lambda = astCtx.create<LambdaNode>(line, col, 1);
  lambda->explicitReturnType = explicitReturnType;
  lambda->isAsync = match(TokenType::ASYNC_KW);
  if (lambda->isAsync && !asyncEnabled) {
    reportError(currentToken().line, currentToken().column, 5,
                "'async' lambdas are disabled for this build (async support "
                "is turned off).");
    throw ParseException();
  }
  lambda->params = astCtx.copyArray<ParamDeclNode *>(params);

  int endLine = currentToken().line;
  int endCol = currentToken().column + (int)currentToken().value.length();

  if (match(TokenType::ARROW)) {
    auto expr = parseExpression();
    endLine = currentToken().line;
    endCol = currentToken().column + (int)currentToken().value.length();

    /* Unlike function declarations, the trailing ';' belongs to the
     * enclosing statement, not to the lambda expression itself. */
    lambda->isExpressionBody = true;
    lambda->exprBody = expr;
    lambda->length = endCol - col;
    lambda->endLine = endLine;
    return lambda;
  }

  auto block = parseBlock();
  lambda->body = block;
  lambda->length = block->column + block->length - col;
  lambda->endLine = block->endLine;
  return lambda;
}

ExprNode *Parser::parsePrimary() {
  int line = currentToken().line;
  int col = currentToken().column;

  /* Lambdas: '() => e', '(int a, int b) { ... }', '(a, b) => a + b', and
   * explicit-return variants 'int (int a) => a + 1'. */
  if (currentToken().type == TokenType::LPAREN &&
      lambdaFollowedByBody(0) && looksLikeLambdaParams(0)) {
    return parseLambda(nullptr);
  }
  if ((currentToken().type == TokenType::TYPE_KW ||
       currentToken().type == TokenType::IDENTIFIER) &&
      peekToken().type == TokenType::LPAREN && lambdaFollowedByBody(1) &&
      looksLikeLambdaParams(1)) {
    const Type *retTy = parseType();
    return parseLambda(retTy);
  }

  if (currentToken().type == TokenType::TYPE_KW) {
    const Type *t = parseType();
    int len = currentToken().column - col;
    auto node = astCtx.create<TypeLiteralNode>(t, line, col, len);
    return node;
  }

  if (currentToken().type == TokenType::NULL_KW) {
    int len = currentToken().value.length();
    advance();
    return astCtx.create<NullNode>(line, col, len);
  }

  /* Dart-style const expression: 'const Foo(1, 2)', 'const expr'. Only in
   * expression position; statement-level 'const' is a declaration. */
  if (currentToken().type == TokenType::CONST_KW) {
    int cLen = currentToken().value.length();
    advance();
    auto inner = parseUnary();
    int endCol = inner->column + inner->length;
    return astCtx.create<ConstExprNode>(inner, line, col, endCol - col);
  }

  if (currentToken().type == TokenType::LBRACKET) {
    return parseArrayLiteral();
  }

  if (currentToken().type == TokenType::LBRACE) {
    return parseMapLiteral();
  }

  if (match(TokenType::LPAREN)) {
    auto expr = parseExpression();
    expect(TokenType::RPAREN, "Expected ')'");
    expr->hasParens = true;
    return expr;
  }

  if (currentToken().type == TokenType::THIS_KW ||
      currentToken().type == TokenType::SUPER_KW) {
    std::string_view name = currentToken().value;
    int len = name.length();
    advance();
    return astCtx.create<VariableNode>(name, line, col, len);
  }

  if (currentToken().type == TokenType::TRUE_KW ||
      currentToken().type == TokenType::FALSE_KW) {
    bool val = currentToken().type == TokenType::TRUE_KW;
    int len = (int)currentToken().value.length();
    advance();
    return astCtx.create<BoolNode>(val, line, col, len);
  }

  if (currentToken().type == TokenType::NUMBER) {
    std::string_view raw = currentToken().value;
    int len = (int)raw.length();
    advance();

    bool isHex =
        raw.length() > 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X');

    bool isFloat = false;
    if (!isHex) {
      isFloat = raw.find('.') != std::string_view::npos ||
                raw.find('e') != std::string_view::npos ||
                raw.find('E') != std::string_view::npos ||
                raw.find('f') != std::string_view::npos ||
                raw.find('F') != std::string_view::npos;
    }

    return astCtx.create<NumberNode>(raw, isFloat, line, col, len);
  }

  if (currentToken().type == TokenType::CHAR_LITERAL) {
    std::string_view raw = currentToken().value;
    uint8_t val = 0;
    if (raw.length() >= 3) {
      if (raw[1] == '\\') {
        if (raw.length() > 3 && (raw[2] == 'x' || raw[2] == 'X')) {
          /* Parse hexadecimal character escape */
          for (size_t i = 3; i < raw.length() - 1; ++i) {
            char hc = raw[i];
            bool isHexDigit = (hc >= '0' && hc <= '9') ||
                              (hc >= 'a' && hc <= 'f') ||
                              (hc >= 'A' && hc <= 'F');
            if (isHexDigit) {
              uint8_t nibble = (hc >= '0' && hc <= '9')   ? (hc - '0')
                               : (hc >= 'a' && hc <= 'f') ? (hc - 'a' + 10)
                               : (hc >= 'A' && hc <= 'F') ? (hc - 'A' + 10)
                                                          : 0;
              val = (val << 4) | nibble;
            } else {
              break;
            }
          }
        } else if (raw.length() > 2 && raw[2] >= '0' && raw[2] <= '7') {
          /* Parse octal character escape */
          for (size_t i = 2; i < raw.length() - 1; ++i) {
            char oc = raw[i];
            if (oc >= '0' && oc <= '7') {
              val = (val << 3) | (oc - '0');
            } else {
              break;
            }
          }
        } else {
          /* Parse standard character escapes */
          switch (raw[2]) {
          case 'n':
            val = '\n';
            break;
          case 't':
            val = '\t';
            break;
          case 'r':
            val = '\r';
            break;
          case '0':
            val = '\0';
            break;
          case '\\':
            val = '\\';
            break;
          case '\'':
            val = '\'';
            break;
          default:
            val = raw[2];
            break;
          }
        }
      } else {
        val = raw[1];
      }
    }
    int len = raw.length();
    advance();
    return astCtx.create<CharNode>(val, line, col, len);
  }

  if (currentToken().type == TokenType::RUNE_LITERAL) {
    std::string_view raw = currentToken().value;
    uint32_t val = 0;
    if (raw.length() >= 4) {
      std::string_view inner = raw.substr(2, raw.length() - 3);
      if (!inner.empty()) {
        if (inner[0] == '\\') {
          if (inner.length() > 1 && (inner[1] == 'x' || inner[1] == 'X')) {
            /* Parse hexadecimal rune escape */
            for (size_t i = 2; i < inner.length(); ++i) {
              char hc = inner[i];
              bool isHexDigit = (hc >= '0' && hc <= '9') ||
                                (hc >= 'a' && hc <= 'f') ||
                                (hc >= 'A' && hc <= 'F');
              if (isHexDigit) {
                uint32_t nibble = (hc >= '0' && hc <= '9')   ? (hc - '0')
                                  : (hc >= 'a' && hc <= 'f') ? (hc - 'a' + 10)
                                  : (hc >= 'A' && hc <= 'F') ? (hc - 'A' + 10)
                                                             : 0;
                val = (val << 4) | nibble;
              } else {
                break;
              }
            }
          } else if (inner.length() > 1 && inner[1] >= '0' && inner[1] <= '7') {
            /* Parse octal rune escape */
            for (size_t i = 1; i < inner.length(); ++i) {
              char oc = inner[i];
              if (oc >= '0' && oc <= '7') {
                val = (val << 3) | (oc - '0');
              } else {
                break;
              }
            }
          } else if (inner.length() > 1) {
            /* Parse standard rune escapes */
            switch (inner[1]) {
            case 'n':
              val = '\n';
              break;
            case 't':
              val = '\t';
              break;
            case 'r':
              val = '\r';
              break;
            case '0':
              val = '\0';
              break;
            case '\\':
              val = '\\';
              break;
            case '\'':
              val = '\'';
              break;
            default:
              val = inner[1];
              break;
            }
          }
        } else {
          /* Fallback to UTF-8 decoding */
          unsigned char c = inner[0];
          if (c < 0x80) {
            val = c;
          } else if ((c & 0xE0) == 0xC0) {
            val = ((c & 0x1F) << 6) | (inner[1] & 0x3F);
          } else if ((c & 0xF0) == 0xE0) {
            val = ((c & 0x0F) << 12) | ((inner[1] & 0x3F) << 6) |
                  (inner[2] & 0x3F);
          } else if ((c & 0xF8) == 0xF0) {
            val = ((c & 0x07) << 18) | ((inner[1] & 0x3F) << 12) |
                  ((inner[2] & 0x3F) << 6) | (inner[3] & 0x3F);
          }
        }
      }
    }
    int len = raw.length();
    advance();
    return astCtx.create<RuneNode>(val, line, col, len);
  }

  if (currentToken().type == TokenType::STRING_LITERAL) {
    std::string_view inner = currentToken().value;
    int len = inner.length() + 2;
    advance();

    std::string unescaped;
    unescaped.reserve(inner.length());
    for (size_t i = 0; i < inner.length(); ++i) {
      if (inner[i] == '\\' && i + 1 < inner.length()) {
        ++i;
        if (inner[i] == 'x' || inner[i] == 'X') {
          /* Evaluate hex sequences bound to a maximum of 2 bytes */
          uint8_t hexVal = 0;
          size_t hexCount = 0;
          while (i + 1 < inner.length() && hexCount < 2) {
            char hc = inner[i + 1];
            bool isHexDigit = (hc >= '0' && hc <= '9') ||
                              (hc >= 'a' && hc <= 'f') ||
                              (hc >= 'A' && hc <= 'F');
            if (!isHexDigit)
              break;

            ++i;
            ++hexCount;
            uint8_t nibble = (hc >= '0' && hc <= '9')   ? (hc - '0')
                             : (hc >= 'a' && hc <= 'f') ? (hc - 'a' + 10)
                             : (hc >= 'A' && hc <= 'F') ? (hc - 'A' + 10)
                                                        : 0;
            hexVal = (hexVal << 4) | nibble;
          }
          if (hexCount > 0) {
            unescaped += static_cast<char>(hexVal);
          } else {
            unescaped += 'x';
          }
        } else if (inner[i] >= '0' && inner[i] <= '7') {
          /* Evaluate octal sequences bound to a maximum of 3 bytes */
          uint8_t octVal = inner[i] - '0';
          size_t octCount = 1;
          while (i + 1 < inner.length() && octCount < 3 &&
                 inner[i + 1] >= '0' && inner[i + 1] <= '7') {
            ++i;
            ++octCount;
            octVal = (octVal << 3) | (inner[i] - '0');
          }
          unescaped += static_cast<char>(octVal);
        } else {
          /* Evaluate standard structural escapes */
          switch (inner[i]) {
          case 'n':
            unescaped += '\n';
            break;
          case 't':
            unescaped += '\t';
            break;
          case 'r':
            unescaped += '\r';
            break;
          case '0':
            unescaped += '\0';
            break;
          case '\\':
            unescaped += '\\';
            break;
          case '"':
            unescaped += '"';
            break;
          default:
            unescaped += inner[i];
            break;
          }
        }
      } else {
        unescaped += inner[i];
      }
    }

    std::string_view finalStr = astCtx.copyString(unescaped);
    return astCtx.create<StringNode>(finalStr, line, col, len);
  }

  if (currentToken().type == TokenType::IDENTIFIER) {
    std::string_view name = currentToken().value;
    int len = (int)name.length();

    /* Compile-time source-location intrinsics, mirroring C/C++: __FILE__
     * expands to the current file path and __LINE__ to the line number. */
    if (name == "__FILE__") {
      advance();
      std::string_view fileStr = astCtx.copyString(filePath);
      return astCtx.create<StringNode>(fileStr, line, col, len);
    }
    if (name == "__LINE__") {
      advance();
      std::string lineStr = std::to_string(line);
      std::string_view lineView = astCtx.copyString(lineStr);
      return astCtx.create<NumberNode>(lineView, false, line, col, len);
    }

    /* A bare template parameter used in a value position is a type
     * reference: 'sizeof(T)', 'alignof(T)', 'typeof(T)'. */
    if (isTemplateParam(name)) {
      const Type *t = astCtx.getTemplateParamType(name);
      advance();
      return astCtx.create<TypeLiteralNode>(t, line, col, len);
    }

    advance();

    bool isTemplateCall = false;
    if (currentToken().type == TokenType::LT && astCtx.isTemplateName(name)) {
      isTemplateCall = true;
    }

    auto varNode = astCtx.create<VariableNode>(name, line, col, len);

    if (isTemplateCall) {
      advance();
      std::vector<const Type *> tArgs;
      if (currentToken().type != TokenType::GT) {
        do {
          tArgs.push_back(parseType());
        } while (match(TokenType::COMMA));
      }
      if (currentToken().type == TokenType::RSHIFT) {
        const_cast<Token &>(currentToken()).type = TokenType::GT;
        /* Keep the original view so raw strings cover the full '>>'. */
      } else {
        expect(TokenType::GT, "Expected '>'");
      }
      varNode->templateArgs = astCtx.copyArray<const Type *>(tArgs);
    }

    return varNode;
  }

  reportError(line, col,
              currentToken().column + (int)currentToken().value.length(),
              "Unexpected token in primary expression: '" +
                  std::string(currentToken().value) + "'");
  throw ParseException();
}

} // namespace utopia