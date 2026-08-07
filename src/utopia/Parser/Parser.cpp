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

const Type *Parser::parseTypeModifiers(const Type *baseType, bool inNewExpr) {
  const Type *ty = baseType;
  while (currentToken().type == TokenType::STAR ||
         currentToken().type == TokenType::AMPERSAND ||
         currentToken().type == TokenType::LOGICAL_AND ||
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
    } else if (currentToken().type == TokenType::LOGICAL_AND) {
      ty = astCtx.getRValueReferenceType(ty);
      advance();
    } else if (!inNewExpr && currentToken().type == TokenType::LBRACKET) {
      ty = applyArrayDeclarator(ty);
    }
  }
  return ty;
}

const Type *Parser::parseType(bool inNewExpr) {
  bool isConst = match(TokenType::CONST_KW);

  if (currentToken().type != TokenType::TYPE_KW &&
      currentToken().type != TokenType::IDENTIFIER) {
    reportError(currentToken().line, currentToken().column,
                (int)currentToken().value.length(), "Expected type name");
    throw ParseException();
  }

  std::string_view base = currentToken().value;
  const Type *ty = nullptr;

  if (isTemplateParam(base)) {
    ty = astCtx.getTemplateParamType(base);
  } else {
    ty = astCtx.getBuiltinTypeByName(base);
    if (!ty)
      ty = astCtx.getRecordType(base);
    if (!ty)
      ty = astCtx.getTypeAlias(base);
    if (!ty)
      ty = astCtx.getEnumTypeByName(base);
  }

  if (!ty) {
    ty = astCtx.getTemplateParamType(base);
  }

  advance();

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
      const_cast<Token &>(currentToken()).value = ">";
    } else {
      expect(TokenType::GT, "Expected '>'");
    }
    ty =
        astCtx.getTemplateInstType(base, astCtx.copyArray<const Type *>(tArgs));
  }

  if (isConst) {
    ty = astCtx.getConstType(ty);
  }

  ty = parseTypeModifiers(ty, inNewExpr);

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

    ty = parseTypeModifiers(ty, inNewExpr);
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

ModuleNode *Parser::parseModule(std::string_view filePath) {
  auto module = astCtx.create<ModuleNode>(filePath);
  std::vector<std::string_view> imports;
  std::vector<std::string_view> exports;
  std::vector<ASTNode *> statements;
  std::string moduleDoc;

  while (currentToken().type != TokenType::EOF_TOK) {
    try {
      if (currentToken().type == TokenType::IDENTIFIER &&
          (currentToken().value == "import" ||
           currentToken().value == "export")) {

        std::string importDoc = consumeComments();
        if (!importDoc.empty()) {
          if (moduleDoc.empty()) {
            moduleDoc = importDoc;
          } else {
            moduleDoc += importDoc;
          }
        }

        bool isExport = (currentToken().value == "export");
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
        expect(TokenType::SEMICOLON, "Expected ';' after statement");

        if (isExport) {
          exports.push_back(path);
        } else {
          imports.push_back(path);
        }

        /* Synchronously invoke the module loader upon evaluating an
         * import/export directive. This pre-populates the ASTContext with
         * available foreign types and guarantees downstream identifier
         * resolution.
         */
        if (moduleLoader) {
          std::filesystem::path currentDir =
              std::filesystem::path(filePath).parent_path();
          moduleLoader->loadModule(std::string(path), currentDir, pathLine,
                                   pathCol, pathLen, filePath);
        }

      } else if (currentToken().type != TokenType::EOF_TOK) {
        if (currentToken().type == TokenType::RBRACE) {
          reportError(currentToken().line, currentToken().column, 1,
                      "Stray '}' in module scope");
          advance();
          continue;
        }
        auto stmt = parseStatement();
        if (stmt)
          statements.push_back(stmt);
      }
    } catch (const ParseException &) {
      synchronize();
    }
  }

  std::string eofDoc = consumeComments();
  if (!eofDoc.empty()) {
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

  if (!moduleDoc.empty()) {
    module->docString = astCtx.copyString(moduleDoc);
  }

  module->rawImports = astCtx.copyArray<std::string_view>(imports);
  module->rawExports = astCtx.copyArray<std::string_view>(exports);
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
  advance(); /* consume '@' */

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
  if (match(TokenType::LPAREN)) {
    if (currentToken().type != TokenType::RPAREN &&
        currentToken().type != TokenType::EOF_TOK) {
      do {
        args.push_back(parseExpression());
      } while (match(TokenType::COMMA));
    }
    expect(TokenType::RPAREN, "Expected ')' after annotation arguments");
  }

  int len = (currentToken().column - col);
  return astCtx.create<AnnotationNode>(name, astCtx.copyArray<ExprNode *>(args),
                                       line, col, len);
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
        sizes.push_back(std::stoull(
            std::string(static_cast<NumberNode *>(sizeExpr)->raw), nullptr, 0));
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

std::vector<ParamDeclNode *> Parser::parseParameterList(bool &isVariadic) {
  std::vector<ParamDeclNode *> params;
  isVariadic = false;
  bool inNamedBlock = false;
  bool optionalPositionalStarted = false;

  while (currentToken().type != TokenType::RPAREN &&
         currentToken().type != TokenType::EOF_TOK) {

    if (match(TokenType::ELLIPSIS)) {
      isVariadic = true;
      match(TokenType::COMMA);
      break;
    }

    if (!inNamedBlock && match(TokenType::LBRACE)) {
      inNamedBlock = true;
      if (currentToken().type == TokenType::RBRACE) {
        // Empty named block permitted
      }
    }

    if (inNamedBlock && currentToken().type == TokenType::RBRACE) {
      advance();
      match(TokenType::COMMA);
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

    const char *typeStart = currentToken().value.data();
    const Type *pType = parseType();
    const char *typeEnd =
        tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
    std::string_view rawTypeStr(typeStart, typeEnd - typeStart);

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
        match(TokenType::COMMA);
      }
      break;
    } else {
      if (inNamedBlock && currentToken().type == TokenType::RBRACE) {
        advance();
        match(TokenType::COMMA);
        break;
      }
      if (currentToken().type == TokenType::RPAREN) {
        break;
      }
    }
  }
  return params;
}

/*
 * Abstracted body parsing mechanism to handle both traditional blocks
 * and expression-bodied functions transparently.
 */
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

    /* Transparently inject a ReturnNode if the function intrinsically expects a
     * value */
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
            if (m->params[i]->type->toString() !=
                newMethod->params[i]->type->toString()) {
              sameSignature = false;
              break;
            }
          }
        }

        /* Method const qualifier evaluates into the overload resolution
         * signature */
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
        if (c->params[i]->type->toString() !=
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
  advance(); /* consume 'annotation' */

  expect(TokenType::CLASS_KW, "Expected 'class' after 'annotation'");

  std::string_view name = currentToken().value;
  int idCol = currentToken().column;
  int idLen = name.length();
  expect(TokenType::IDENTIFIER, "Expected annotation class name");

  RecordType *classTy = astCtx.createRecordType(TypeKind::Class, name);
  classTy->setOpaque(false);

  expect(TokenType::LBRACE, "Expected '{'");

  std::vector<VarDeclNode *> fields;
  FunctionDeclNode *constructor = nullptr;

  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {

    /* Metadata applies strictly recursively to annotation definitions as well
     */
    std::string doc = consumeComments();
    auto memberAnnotations = parseAnnotations();

    bool isPub = false, isPriv = false;
    while (currentToken().type == TokenType::PUBLIC_KW ||
           currentToken().type == TokenType::PRIVATE_KW) {
      if (currentToken().type == TokenType::PUBLIC_KW)
        isPub = true;
      if (currentToken().type == TokenType::PRIVATE_KW)
        isPriv = true;
      advance();
    }

    /* Intercept floating metadata and comments guarding the scope closure */
    if (currentToken().type == TokenType::RBRACE ||
        currentToken().type == TokenType::EOF_TOK) {
      if (!memberAnnotations.empty()) {
        reportError(currentToken().line, currentToken().column,
                    (int)currentToken().value.length(),
                    "Annotations must be attached to a declaration.");
      }
      break;
    }

    /* Enforce compile-time invariant: const constructor requirement */
    if (currentToken().type == TokenType::CONST_KW &&
        peekToken().type == TokenType::IDENTIFIER &&
        peekToken().value == name) {
      int cLine = currentToken().line;
      int cCol = currentToken().column;
      int ctorIdCol = peekToken().column;

      advance(); /* const */
      advance(); /* name */
      expect(TokenType::LPAREN, "Expected '('");

      bool isVariadic = false;
      auto params = parseParameterList(isVariadic);
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
      constructor->identifierColumn = ctorIdCol;
      constructor->identifierLength = name.length();

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
                      fields[i]->isPublic(fields[i]->varName)});
  }
  classTy->setFields(astCtx.copyArray<FieldInfo>(fInfos));

  auto node = astCtx.create<AnnotationDeclNode>(name, line, col, endCol - col);
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

ASTNode *Parser::parseStatement() {
  std::string doc = consumeComments();
  auto annotations = parseAnnotations();

  bool isPub = false, isPriv = false;
  while (currentToken().type == TokenType::PUBLIC_KW ||
         currentToken().type == TokenType::PRIVATE_KW) {
    if (currentToken().type == TokenType::PUBLIC_KW)
      isPub = true;
    if (currentToken().type == TokenType::PRIVATE_KW)
      isPriv = true;
    advance();
  }

  if (currentToken().type == TokenType::RBRACE ||
      currentToken().type == TokenType::EOF_TOK) {
    if (!annotations.empty()) {
      reportError(currentToken().line, currentToken().column,
                  (int)currentToken().value.length(),
                  "Annotations are strictly permitted on declarations only.");
    }
    if (isPub || isPriv) {
      reportError(
          currentToken().line, currentToken().column,
          (int)currentToken().value.length(),
          "Access modifiers are strictly permitted on declarations only.");
    }
    return nullptr;
  }

  ASTNode *node = nullptr;

  if (currentToken().type == TokenType::TYPEDEF_KW) {
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
    node = parseRecordDecl(TypeKind::Class);
  } else if (currentToken().type == TokenType::IF_KW) {
    node = parseIfStatement();
  } else if (currentToken().type == TokenType::FOR_KW) {
    node = parseForStatement();
  } else if (currentToken().type == TokenType::WHILE_KW) {
    node = parseWhileStatement();
  } else if (currentToken().type == TokenType::SWITCH_KW) {
    node = parseSwitchStatement();
  } else if (currentToken().type == TokenType::BREAK_KW) {
    node = parseBreakStatement();
  } else if (currentToken().type == TokenType::CONTINUE_KW) {
    node = parseContinueStatement();
  } else if (currentToken().type == TokenType::CONST_KW ||
             currentToken().type == TokenType::VAR_KW ||
             currentToken().type == TokenType::STATIC_KW ||
             ((currentToken().type == TokenType::TYPE_KW ||
               (currentToken().type == TokenType::IDENTIFIER &&
                (astCtx.getRecordType(currentToken().value) != nullptr ||
                 astCtx.getTypeAlias(currentToken().value) != nullptr ||
                 astCtx.getEnumTypeByName(currentToken().value) != nullptr ||
                 isTemplateParam(currentToken().value)))) &&
              peekToken().type != TokenType::DOT) ||
             (currentToken().type == TokenType::IDENTIFIER &&
              peekToken().type == TokenType::IDENTIFIER)) {
    node = parseDeclarationOrFunction(annotations);
  } else if (currentToken().type == TokenType::RETURN) {
    node = parseReturn();
  } else if (currentToken().type == TokenType::LBRACE) {
    node = parseBlock();
  } else {
    node = parseExpressionStatement();
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

  if (isPub || isPriv) {
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
  advance(); /* consume 'typedef' */

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
    knownBase = astCtx.getBuiltinTypeByName(currentToken().value);
    if (!knownBase)
      knownBase = astCtx.getRecordType(currentToken().value);
    if (!knownBase)
      knownBase = astCtx.getTypeAlias(currentToken().value);
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

  auto decl =
      astCtx.create<TypedefDeclNode>(name, targetType, line, col, endCol - col);
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

  auto stmt = parseStatement();

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

ForNode *Parser::parseForStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance(); // consume 'for'

  expect(TokenType::LPAREN, "Expected '(' after 'for'");

  ASTNode *initStmt = nullptr;
  if (currentToken().type != TokenType::SEMICOLON) {
    if (currentToken().type == TokenType::TYPE_KW ||
        currentToken().type == TokenType::CONST_KW ||
        currentToken().type == TokenType::VAR_KW ||
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

WhileNode *Parser::parseWhileStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance(); // consume 'while'

  expect(TokenType::LPAREN, "Expected '(' after 'while'");
  auto cond = parseExpression();
  expect(TokenType::RPAREN, "Expected ')' after while condition");

  auto body = parseStatementAsBlock();

  int len = (body->column + body->length) - col;
  auto node = astCtx.create<WhileNode>(cond, body, line, col, len);
  node->endLine = body->endLine;
  return node;
}

SwitchNode *Parser::parseSwitchStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance(); /* consume 'switch' */

  expect(TokenType::LPAREN, "Expected '(' after 'switch'");
  auto cond = parseExpression();
  expect(TokenType::RPAREN, "Expected ')' after switch condition");

  expect(TokenType::LBRACE, "Expected '{' after switch expression");

  std::vector<CaseNode *> cases;
  bool hasDefault = false;

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

      if (auto stmt = parseStatement()) {
        stmts.push_back(stmt);
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
  while (currentToken().type == TokenType::LT ||
         currentToken().type == TokenType::GT ||
         currentToken().type == TokenType::LE ||
         currentToken().type == TokenType::GE) {
    int line = left->line;
    int col = left->column;
    std::string_view op = currentToken().value;
    advance();
    auto right = parseShift();
    left = astCtx.create<BinaryOpNode>(op, left, right, line, col);
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
  if (match(TokenType::NEW_KW)) {
    int line = currentToken().line;
    int col = currentToken().column;

    const char *typeStart = currentToken().value.data();
    const Type *allocTy = parseType(true);
    const char *typeEnd =
        tokens[cursor - 1].value.data() + tokens[cursor - 1].value.length();
    std::string_view rawTypeStr(typeStart, typeEnd - typeStart);

    ExprNode *arraySize = nullptr;

    if (match(TokenType::LBRACKET)) {
      arraySize = parseExpression();
      expect(TokenType::RBRACKET, "Expected ']'");
    }

    std::vector<ExprNode *> args;
    std::vector<std::string_view> argNames;
    bool hasParens = false;
    bool namedStarted = false;

    if (match(TokenType::LPAREN)) {
      hasParens = true;

      if (currentToken().type != TokenType::RPAREN) {
        do {
          if (currentToken().type == TokenType::RPAREN)
            break;

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
    node->rawAllocatedTypeStr = rawTypeStr;
    node->rawArgs = argsRef;
    node->rawArgNames = namesRef;
    node->hasRawArgs = true;
    return node;
  }

  if (match(TokenType::DELETE_KW)) {
    int line = currentToken().line;
    int col = currentToken().column;
    bool isArray = false;

    if (match(TokenType::LBRACKET)) {
      expect(TokenType::RBRACKET, "Expected ']' after '[' in delete");
      isArray = true;
    }

    auto ptr = parseUnary();
    return astCtx.create<DeleteExprNode>(ptr, isArray, line, col,
                                         currentToken().column - col);
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

DeclNode *Parser::parseRecordDecl(TypeKind kind) {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  std::string_view name = currentToken().value;
  int idCol = currentToken().column;
  int idLen = name.length();
  expect(TokenType::IDENTIFIER, "Expected record name");

  std::vector<std::string_view> tParams;
  if (match(TokenType::LT)) {
    astCtx.registerTemplateName(name);
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
      } while (match(TokenType::COMMA));
    }
    if (currentToken().type == TokenType::RSHIFT) {
      const_cast<Token &>(currentToken()).type = TokenType::GT;
      const_cast<Token &>(currentToken()).value = ">";
    } else {
      expect(TokenType::GT, "Expected '>'");
    }
  }

  RecordType *recordTy = astCtx.createRecordType(kind, name);

  if (match(TokenType::SEMICOLON)) {
    DeclNode *node = nullptr;
    int len = currentToken().column - col;

    if (kind == TypeKind::Class) {
      auto cNode = astCtx.create<ClassDeclNode>(name, line, col, len);
      cNode->isOpaque = true;
      cNode->recordType = recordTy;
      node = cNode;
    } else if (kind == TypeKind::Struct) {
      auto sNode = astCtx.create<StructDeclNode>(name, line, col, len);
      sNode->isOpaque = true;
      sNode->recordType = recordTy;
      node = sNode;
    } else {
      auto uNode = astCtx.create<UnionDeclNode>(name, line, col, len);
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

    bool isPub = false, isPriv = false;
    while (currentToken().type == TokenType::PUBLIC_KW ||
           currentToken().type == TokenType::PRIVATE_KW) {
      if (currentToken().type == TokenType::PUBLIC_KW)
        isPub = true;
      if (currentToken().type == TokenType::PRIVATE_KW)
        isPriv = true;
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
    int ctorIdCol = currentToken().column;
    if (currentToken().type == TokenType::CONST_KW &&
        peekToken().type == TokenType::IDENTIFIER &&
        peekToken().value == name && peekToken(2).type == TokenType::LPAREN) {
      isConstCtor = true;
      ctorIdCol = peekToken().column;
    }

    if (isConstCtor || (currentToken().type == TokenType::IDENTIFIER &&
                        currentToken().value == name &&
                        peekToken().type == TokenType::LPAREN)) {
      int cLine = currentToken().line;
      int cCol = currentToken().column;

      if (isConstCtor)
        advance();
      advance();
      advance();

      bool isVariadic = false;
      auto params = parseParameterList(isVariadic);
      expect(TokenType::RPAREN, "Expected ')'");

      auto constructor =
          astCtx.create<FunctionDeclNode>(astCtx.VoidTy, name, cLine, cCol,
                                          isConstCtor, true, false, isVariadic);
      constructor->parentRecord = recordTy;
      constructor->params = astCtx.copyArray<ParamDeclNode *>(params);
      constructor->annotations = memberAnnotations;
      constructor->hasPublicMod = isPub;
      constructor->hasPrivateMod = isPriv;
      constructor->identifierColumn = ctorIdCol;
      constructor->identifierLength = name.length();

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

    while (currentToken().type == TokenType::STATIC_KW) {
      isStatic = true;
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
    if (match(TokenType::LT)) {
      astCtx.registerTemplateName(memName);
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
        } while (match(TokenType::COMMA));
      }
      if (currentToken().type == TokenType::RSHIFT) {
        const_cast<Token &>(currentToken()).type = TokenType::GT;
        const_cast<Token &>(currentToken()).value = ">";
      } else {
        expect(TokenType::GT, "Expected '>'");
      }
    }

    if (match(TokenType::LPAREN)) {
      bool isVariadic = false;
      auto params = parseParameterList(isVariadic);
      expect(TokenType::RPAREN, "Expected ')'");

      auto method = astCtx.create<FunctionDeclNode>(
          memType, memName, mLine, mCol, false, true, isExtern, isVariadic);
      method->parentRecord = recordTy;
      method->isStatic = isStatic;
      method->params = astCtx.copyArray<ParamDeclNode *>(params);
      method->annotations = memberAnnotations;
      method->hasPublicMod = isPub;
      method->hasPrivateMod = isPriv;
      method->rawReturnTypeStr = rawTypeStr;
      method->identifierColumn = memIdCol;
      method->identifierLength = memIdLen;

      if (!doc.empty())
        method->docString = astCtx.copyString(doc);

      if (isExtern || isIntrinsic) {
        int endLine = currentToken().line;
        int endCol = currentToken().column + 1;
        expect(TokenType::SEMICOLON,
               "Expected ';' after extern or intrinsic method declaration");
        method->length = endCol - mCol;
        method->endLine = endLine;
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
      field->annotations = memberAnnotations;
      field->hasPublicMod = isPub;
      field->hasPrivateMod = isPriv;
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
                      fields[i]->isPublic(fields[i]->varName)});
  }
  recordTy->setFields(astCtx.copyArray<FieldInfo>(fInfos));

  DeclNode *node = nullptr;
  int len = endCol - col;

  if (kind == TypeKind::Class) {
    auto cNode = astCtx.create<ClassDeclNode>(name, line, col, len);
    cNode->fields = astCtx.copyArray<VarDeclNode *>(fields);
    cNode->methods = astCtx.copyArray<FunctionDeclNode *>(methods);
    cNode->constructors = astCtx.copyArray<FunctionDeclNode *>(constructors);
    cNode->destructor = destructor;
    cNode->endLine = endLine;
    cNode->recordType = recordTy;
    if (!tParams.empty()) {
      cNode->isTemplate = true;
      cNode->templateParams = astCtx.copyArray<std::string_view>(tParams);
    }
    node = cNode;
  } else if (kind == TypeKind::Struct) {
    auto sNode = astCtx.create<StructDeclNode>(name, line, col, len);
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
    node = sNode;
  } else {
    auto uNode = astCtx.create<UnionDeclNode>(name, line, col, len);
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
  advance(); /* consume 'enum' */

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

  /* Eagerly register the enum type so subsequent parameters/variables can use
   * it as a valid type */
  astCtx.getEnumType(name, underlyingType);

  expect(TokenType::LBRACE, "Expected '{'");

  std::vector<EnumMemberNode *> members;
  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {

    if (currentToken().type == TokenType::RBRACE)
      break;

    bool isPub = false, isPriv = false;
    while (currentToken().type == TokenType::PUBLIC_KW ||
           currentToken().type == TokenType::PRIVATE_KW) {
      if (currentToken().type == TokenType::PUBLIC_KW)
        isPub = true;
      if (currentToken().type == TokenType::PRIVATE_KW)
        isPriv = true;
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
  node->members = astCtx.copyArray<EnumMemberNode *>(members);
  node->enumType = astCtx.getEnumType(name, underlyingType);
  node->endLine = endLine;
  node->identifierColumn = idCol;
  node->identifierLength = idLen;

  return node;
}

DeclNode *Parser::parseDeclarationOrFunction(
    llvm::ArrayRef<AnnotationNode *> annotations) {
  int line = currentToken().line;
  int col = currentToken().column;

  bool isExtern = false;
  bool isStatic = false;
  bool isIntrinsic = false;

  while (currentToken().type == TokenType::STATIC_KW) {
    if (currentToken().type == TokenType::STATIC_KW)
      isStatic = true;
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
    isImplicitlyTyped = true;
    nodeType = astCtx.AutoTy;
    rawTypeStr = "var";
  } else if (currentToken().type == TokenType::CONST_KW &&
             peekToken().type == TokenType::IDENTIFIER &&
             (peekToken(2).type == TokenType::ASSIGN ||
              peekToken(2).type == TokenType::SEMICOLON)) {
    advance(); /* Consume 'const' */
    isImplicitlyTyped = true;
    isConstDecl = true;
    nodeType = astCtx.getConstType(astCtx.AutoTy);
    rawTypeStr = "const";
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
      } while (match(TokenType::COMMA));
    }
    if (currentToken().type == TokenType::RSHIFT) {
      const_cast<Token &>(currentToken()).type = TokenType::GT;
      const_cast<Token &>(currentToken()).value = ">";
    } else {
      expect(TokenType::GT, "Expected '>'");
    }
  }

  if (match(TokenType::LPAREN)) {
    bool isVariadic = false;
    auto params = parseParameterList(isVariadic);
    expect(TokenType::RPAREN, "Expected ')' after parameters");

    bool isFuncConst = match(TokenType::CONST_KW);

    auto funcDecl = astCtx.create<FunctionDeclNode>(
        nodeType, id, line, col, isFuncConst, false, isExtern, isVariadic);
    funcDecl->isStatic = isStatic;
    funcDecl->params = astCtx.copyArray<ParamDeclNode *>(params);
    funcDecl->rawReturnTypeStr = rawTypeStr;
    funcDecl->identifierColumn = idCol;
    funcDecl->identifierLength = idLen;

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
  varDecl->rawTypeStr = rawTypeStr;
  varDecl->endLine = endLine;
  varDecl->identifierColumn = idCol;
  varDecl->identifierLength = idLen;
  varDecl->isStatic = isStatic;

  return varDecl;
}

BlockNode *Parser::parseBlock() {
  int startLine = currentToken().line;
  int startCol = currentToken().column;
  expect(TokenType::LBRACE, "Expected '{'");

  auto block = astCtx.create<BlockNode>(startLine, startCol);
  std::vector<ASTNode *> statements;

  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {
    if (auto stmt = parseStatement()) {
      statements.push_back(stmt);
    }
  }

  std::string closingDoc = consumeComments();

  int endLine = currentToken().line;
  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACE, "Expected '}'");

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
  advance(); /* Consume '[' */

  std::vector<ExprNode *> elements;
  if (currentToken().type != TokenType::RBRACKET &&
      currentToken().type != TokenType::EOF_TOK) {
    do {
      if (currentToken().type == TokenType::RBRACKET) {
        break;
      }
      elements.push_back(parseExpression());
    } while (match(TokenType::COMMA));
  }

  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACKET, "Expected ']' at end of array literal");

  return astCtx.create<ArrayLiteralNode>(astCtx.copyArray<ExprNode *>(elements),
                                         line, col, endCol - col);
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

    /* Securely calculate the end column using the parsed type's last token */
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
      std::string_view memberName = currentToken().value;
      int memLen = memberName.length();

      /* Capture the exact column of the identifier before advancing the token
       * stream */
      int memCol = currentToken().column;
      expect(TokenType::IDENTIFIER, "Expected member name after '.'");

      /* Check for template invocation using semantic awareness rather than
       * lookahead */
      bool isTemplateCall = false;
      if (currentToken().type == TokenType::LT &&
          astCtx.isTemplateName(memberName)) {
        isTemplateCall = true;
      }

      auto maNode = astCtx.create<MemberAccessNode>(expr, memberName, line, col,
                                                    (memCol + memLen) - col);

      if (isTemplateCall) {
        advance(); /* Consume '<' */
        std::vector<const Type *> tArgs;
        if (currentToken().type != TokenType::GT) {
          do {
            tArgs.push_back(parseType());
          } while (match(TokenType::COMMA));
        }
        if (currentToken().type == TokenType::RSHIFT) {
          const_cast<Token &>(currentToken()).type = TokenType::GT;
          const_cast<Token &>(currentToken()).value = ">";
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

      if (currentToken().type != TokenType::RPAREN) {
        do {
          if (currentToken().type == TokenType::RPAREN)
            break;

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
      expr = astCtx.create<FunctionCallNode>(expr, argsRef, namesRef, line, col,
                                             endCol - col);
      expr->endLine = endLine;
      static_cast<FunctionCallNode *>(expr)->rawArgs = argsRef;
      static_cast<FunctionCallNode *>(expr)->rawArgNames = namesRef;
      static_cast<FunctionCallNode *>(expr)->hasRawArgs = true;
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

ExprNode *Parser::parsePrimary() {
  int line = currentToken().line;
  int col = currentToken().column;

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

  if (currentToken().type == TokenType::LBRACKET) {
    return parseArrayLiteral();
  }

  if (match(TokenType::LPAREN)) {
    auto expr = parseExpression();
    expect(TokenType::RPAREN, "Expected ')'");
    expr->hasParens = true;
    return expr;
  }

  if (currentToken().type == TokenType::THIS_KW) {
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
        const_cast<Token &>(currentToken()).value = ">";
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