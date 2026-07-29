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
  std::string_view opStr;
  switch (currentToken().type) {
  case TokenType::PLUS:
    opStr = "+";
    break;
  case TokenType::MINUS:
    opStr = "-";
    break;
  case TokenType::STAR:
    opStr = "*";
    break;
  case TokenType::SLASH:
    opStr = "/";
    break;
  case TokenType::PERCENT:
    opStr = "%";
    break;
  case TokenType::EQ:
    opStr = "==";
    break;
  case TokenType::NEQ:
    opStr = "!=";
    break;
  case TokenType::LT:
    opStr = "<";
    break;
  case TokenType::GT:
    opStr = ">";
    break;
  case TokenType::LE:
    opStr = "<=";
    break;
  case TokenType::GE:
    opStr = ">=";
    break;
  case TokenType::ASSIGN:
    opStr = "=";
    break;
  case TokenType::PLUS_EQ:
    opStr = "+=";
    break;
  case TokenType::MINUS_EQ:
    opStr = "-=";
    break;
  case TokenType::STAR_EQ:
    opStr = "*=";
    break;
  case TokenType::SLASH_EQ:
    opStr = "/=";
    break;
  case TokenType::PERCENT_EQ:
    opStr = "%=";
    break;
  case TokenType::PLUS_PLUS:
    opStr = "++";
    break;
  case TokenType::MINUS_MINUS:
    opStr = "--";
    break;
  case TokenType::PIPE:
    opStr = "|";
    break;
  case TokenType::AMPERSAND:
    opStr = "&";
    break;
  case TokenType::CARET:
    opStr = "^";
    break;
  case TokenType::LSHIFT:
    opStr = "<<";
    break;
  case TokenType::RSHIFT:
    opStr = ">>";
    break;
  case TokenType::PIPE_EQ:
    opStr = "|=";
    break;
  case TokenType::AMPERSAND_EQ:
    opStr = "&=";
    break;
  case TokenType::CARET_EQ:
    opStr = "^=";
    break;
  case TokenType::LSHIFT_EQ:
    opStr = "<<=";
    break;
  case TokenType::RSHIFT_EQ:
    opStr = ">>=";
    break;
  case TokenType::BANG:
    opStr = "!";
    break;
  case TokenType::TILDE:
    opStr = "~";
    break;
  default:
    reportError(currentToken().line, currentToken().column,
                currentToken().value.length(),
                "Invalid operator for overloading");
    throw ParseException();
  }
  advance();
  std::string name = "operator" + std::string(opStr);
  return astCtx.copyString(name);
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

  if (!instantiatingName.empty() && base == templateBaseName) {
    base = instantiatingName;
  }

  const Type *ty = nullptr;

  if (auto it = templateArgs.find(base); it != templateArgs.end()) {
    ty = it->second;
  } else if (isTemplateParam(base)) {
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
    bool foundAsTemplateParam = false;
    size_t la = 0;

    /* Lookahead to resolve generic return types in template functions/methods.
     * Scans forward to verify if the unknown identifier is explicitly declared
     * as a template parameter before the function arguments begin. */
    while (peekToken(la).type != TokenType::EOF_TOK &&
           peekToken(la).type != TokenType::LPAREN &&
           peekToken(la).type != TokenType::SEMICOLON &&
           peekToken(la).type != TokenType::LBRACE) {

      if (peekToken(la).type == TokenType::LT) {
        size_t inner = la + 1;
        while (peekToken(inner).type != TokenType::EOF_TOK &&
               peekToken(inner).type != TokenType::GT &&
               peekToken(inner).type != TokenType::LPAREN) {

          if (peekToken(inner).type == TokenType::IDENTIFIER &&
              peekToken(inner).value == base) {
            foundAsTemplateParam = true;
            break;
          }
          inner++;
        }
      }

      if (foundAsTemplateParam)
        break;
      la++;
    }

    if (foundAsTemplateParam) {
      ty = astCtx.getTemplateParamType(base);
    } else {
      reportError(currentToken().line, currentToken().column,
                  (int)base.length(), "Unknown type: " + std::string(base));
      throw ParseException();
    }
  }

  advance();

  if (currentToken().type == TokenType::LT) {
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
    ty =
        astCtx.getTemplateInstType(base, astCtx.copyArray<const Type *>(tArgs));
  }

  if (isConst) {
    ty = astCtx.getConstType(ty);
  }

  /* Parse function type definitions (e.g., int Function(int, float))
   */
  if (match(TokenType::FUNCTION_KW)) {
    expect(TokenType::LPAREN, "Expected '(' after 'Function'");
    std::vector<const Type *> paramTypes;
    if (currentToken().type != TokenType::RPAREN &&
        currentToken().type != TokenType::EOF_TOK) {
      do {
        paramTypes.push_back(parseType());
      } while (match(TokenType::COMMA));
    }
    expect(TokenType::RPAREN, "Expected ')' after function parameters");

    ty = astCtx.getFunctionType(ty, astCtx.copyArray<const Type *>(paramTypes));
    /* Function variables inherently decay to pointers to support C-ABI and
     * dynamic dispatch */
    ty = astCtx.getPointerType(ty);
  }

  /*
   * Process array brackets, pointers, references, and r-value references.
   * TokenType::LOGICAL_AND is naturally emitted by the lexer for '&&'
   */
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

std::string Parser::consumeComments() {
  std::string doc;
  while (currentToken().type == TokenType::COMMENT) {
    if (!doc.empty())
      doc += "\n";
    doc += currentToken().value;
    advance();
  }
  return doc;
}

ModuleNode *Parser::parseModule(std::string_view filePath) {
  auto module = astCtx.create<ModuleNode>(filePath);
  std::vector<std::string_view> imports;
  std::vector<ASTNode *> statements;

  while (currentToken().type != TokenType::EOF_TOK) {
    try {
      if (currentToken().type == TokenType::COMMENT &&
          peekToken().value == "import") {
        consumeComments();
      }

      if (currentToken().type == TokenType::IDENTIFIER &&
          currentToken().value == "import") {
        advance();

        if (currentToken().type != TokenType::STRING_LITERAL) {
          reportError(currentToken().line, currentToken().column,
                      (int)currentToken().value.length(),
                      "Expected string literal for module path after 'import'");
          throw ParseException();
        }

        std::string_view path = currentToken().value;
        advance();

        expect(TokenType::SEMICOLON, "Expected ';' after import statement");
        imports.push_back(path);

        /* Synchronously invoke the module loader upon evaluating an import
         * directive. This pre-populates the ASTContext with available
         * foreign types and guarantees downstream identifier resolution.
         */
        if (moduleLoader) {
          std::filesystem::path currentDir =
              std::filesystem::path(filePath).parent_path();
          moduleLoader->loadModule(std::string(path), currentDir);
        }

      } else if (currentToken().type != TokenType::EOF_TOK) {
        auto stmt = parseStatement();
        if (stmt)
          statements.push_back(stmt);
      }
    } catch (const ParseException &) {
      synchronize();
    }
  }

  module->rawImports = astCtx.copyArray<std::string_view>(imports);
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
      currentToken().type == TokenType::EXTERN_KW ||
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
        sizes.push_back(
            std::stoull(std::string(static_cast<NumberNode *>(sizeExpr)->raw)));
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

std::vector<ParamDeclNode *> Parser::parseParameterList(const Type *classTy,
                                                        bool &isVariadic) {
  std::vector<ParamDeclNode *> params;
  isVariadic = false;
  bool inNamedBlock = false;
  bool optionalPositionalStarted = false;

  if (classTy) {
    params.push_back(astCtx.create<ParamDeclNode>(
        astCtx.getPointerType(classTy), "this", nullptr, false, false,
        currentToken().line, currentToken().column, 4));
  }

  while (currentToken().type == TokenType::COMMENT)
    advance();

  while (currentToken().type != TokenType::RPAREN &&
         currentToken().type != TokenType::EOF_TOK) {

    while (currentToken().type == TokenType::COMMENT)
      advance();
    if (currentToken().type == TokenType::RPAREN)
      break;

    if (match(TokenType::ELLIPSIS)) {
      isVariadic = true;
      break;
    }

    if (!inNamedBlock && match(TokenType::LBRACE)) {
      inNamedBlock = true;
      while (currentToken().type == TokenType::COMMENT)
        advance();
      if (currentToken().type == TokenType::RBRACE) {
        // Empty named block permitted
      }
    }

    if (inNamedBlock && currentToken().type == TokenType::RBRACE) {
      advance();
      break;
    }

    int pLine = currentToken().line;
    int pCol = currentToken().column;

    bool isRequired = match(TokenType::REQUIRED_KW);
    if (isRequired && !inNamedBlock) {
      reportError(
          pLine, pCol, 8,
          "The 'required' modifier can only be applied to named parameters.");
      throw ParseException();
    }

    const Type *pType = parseType();
    std::string_view pName = currentToken().value;
    int pLen = (int)pName.length();
    expect(TokenType::IDENTIFIER, "Expected parameter name.");

    /* Apply array-to-pointer decay for parameter declarations just like C++ */
    if (pType->getKind() == TypeKind::Array) {
      pType = astCtx.getPointerType(
          static_cast<const ArrayType *>(pType)->getElementType());
    }

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

    params.push_back(astCtx.create<ParamDeclNode>(
        pType, pName, defVal, inNamedBlock, isRequired, pLine, pCol, pLen));

    while (currentToken().type == TokenType::COMMENT)
      advance();

    if (!match(TokenType::COMMA)) {
      if (inNamedBlock) {
        expect(TokenType::RBRACE,
               "Expected '}' to close named parameter list.");
      }
      break;
    } else {
      while (currentToken().type == TokenType::COMMENT)
        advance();
      if (inNamedBlock && currentToken().type == TokenType::RBRACE) {
        advance();
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

    int endCol = currentToken().column + currentToken().value.length();
    expect(TokenType::SEMICOLON, "Expected ';' after '=>' expression");

    auto block = astCtx.create<BlockNode>(line, col);
    ASTNode *stmt = expr;

    /* Transparently inject a ReturnNode if the function intrinsically expects a
     * value */
    if (returnType && !returnType->isVoid()) {
      stmt = astCtx.create<ReturnNode>(expr, line, col, endCol - col);
    }

    block->statements = astCtx.copyArray<ASTNode *>(stmt);
    block->finalize(endCol);

    return block;
  }

  return parseBlock();
}

DeclNode *
Parser::parseAnnotationDecl(llvm::ArrayRef<AnnotationNode *> annotations) {
  int line = currentToken().line;
  int col = currentToken().column;
  advance(); /* consume 'annotation' */

  expect(TokenType::CLASS_KW, "Expected 'class' after 'annotation'");

  std::string_view name = currentToken().value;
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

      advance(); /* const */
      advance(); /* name */
      expect(TokenType::LPAREN, "Expected '('");

      bool isVariadic = false;
      auto params = parseParameterList(classTy, isVariadic);
      if (isVariadic) {
        reportError(cLine, cCol, name.length(),
                    "Annotation constructors cannot be variadic.");
        throw ParseException();
      }
      expect(TokenType::RPAREN, "Expected ')'");

      constructor = astCtx.create<FunctionDeclNode>(astCtx.VoidTy, name, cLine,
                                                    cCol, true, true);
      constructor->params = astCtx.copyArray<ParamDeclNode *>(params);
      constructor->annotations = memberAnnotations;
      constructor->hasPublicMod = isPub;
      constructor->hasPrivateMod = isPriv;
      if (!doc.empty())
        constructor->docString = astCtx.copyString(doc);

      constructor->body = parseFunctionBody(astCtx.VoidTy);
      continue;
    }

    int mLine = currentToken().line;
    int mCol = currentToken().column;
    const Type *memType = parseType();
    std::string_view memName = currentToken().value;

    expect(TokenType::IDENTIFIER, "Expected member name");
    expect(TokenType::SEMICOLON, "Expected ';'");

    auto field = astCtx.create<VarDeclNode>(memType, memName, nullptr, mLine,
                                            mCol, memName.length());
    field->annotations = memberAnnotations;
    field->hasPublicMod = isPub;
    field->hasPrivateMod = isPriv;
    if (!doc.empty())
      field->docString = astCtx.copyString(doc);

    fields.push_back(field);
  }

  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACE, "Expected '}'");

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

  /* Gracefully handle empty blocks or trailing comments mapping directly to
   * closure */
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
  } else if (currentToken().type == TokenType::STRUCT_KW) {
    node = parseStructDecl();
  } else if (currentToken().type == TokenType::CLASS_KW) {
    node = parseClassDecl();
  } else if (currentToken().type == TokenType::IF_KW) {
    node = parseIfStatement();
  } else if (currentToken().type == TokenType::FOR_KW) {
    node = parseForStatement();
  } else if (currentToken().type == TokenType::WHILE_KW) {
    node = parseWhileStatement();
  } else if (currentToken().type == TokenType::TYPE_KW ||
             currentToken().type == TokenType::CONST_KW ||
             currentToken().type == TokenType::EXTERN_KW ||
             currentToken().type == TokenType::STATIC_KW ||
             (currentToken().type == TokenType::IDENTIFIER &&
              (peekToken().type == TokenType::IDENTIFIER ||
               astCtx.getRecordType(currentToken().value) != nullptr))) {
    node = parseDeclarationOrFunction(annotations);
  } else if (currentToken().type == TokenType::RETURN) {
    node = parseReturn();
  } else if (currentToken().type == TokenType::LBRACE) {
    node = parseBlock();
  } else {
    node = parseExpressionStatement();
  }

  if (node && !doc.empty())
    node->docString = astCtx.copyString(doc);

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
  expect(TokenType::IDENTIFIER, "Expected alias name after 'typedef'");

  expect(TokenType::ASSIGN, "Expected '=' in typedef declaration");

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
  return decl;
}

IfNode *Parser::parseIfStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  expect(TokenType::LPAREN, "Expected '(' after 'if'");
  auto cond = parseExpression();
  expect(TokenType::RPAREN, "Expected ')' after if condition");

  auto thenBlock = parseBlock();
  ASTNode *elseBlock = nullptr;

  if (match(TokenType::ELSE_KW)) {
    if (currentToken().type == TokenType::IF_KW) {
      elseBlock = parseIfStatement();
    } else {
      elseBlock = parseBlock();
    }
  }

  int len = (elseBlock ? (elseBlock->column + elseBlock->length)
                       : (thenBlock->column + thenBlock->length)) -
            col;
  return astCtx.create<IfNode>(cond, thenBlock, elseBlock, line, col, len);
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

  auto body = parseBlock();

  int len = (body->column + body->length) - col;
  return astCtx.create<ForNode>(initStmt, cond, inc, body, line, col, len);
}

WhileNode *Parser::parseWhileStatement() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance(); // consume 'while'

  expect(TokenType::LPAREN, "Expected '(' after 'while'");
  auto cond = parseExpression();
  expect(TokenType::RPAREN, "Expected ')' after while condition");

  auto body = parseBlock();

  int len = (body->column + body->length) - col;
  return astCtx.create<WhileNode>(cond, body, line, col, len);
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
    const Type *allocTy = parseType(true);
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
      while (currentToken().type == TokenType::COMMENT)
        advance();

      if (currentToken().type != TokenType::RPAREN) {
        do {
          while (currentToken().type == TokenType::COMMENT)
            advance();
          if (currentToken().type == TokenType::RPAREN)
            break;

          if (currentToken().type == TokenType::IDENTIFIER &&
              peekToken().type == TokenType::COLON) {
            namedStarted = true;
            argNames.push_back(currentToken().value);
            advance();
            advance();
            while (currentToken().type == TokenType::COMMENT)
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
          }

          while (currentToken().type == TokenType::COMMENT)
            advance();
        } while (match(TokenType::COMMA));
      }
      while (currentToken().type == TokenType::COMMENT)
        advance();
      expect(TokenType::RPAREN, "Expected ')'");
    }

    return astCtx.create<NewExprNode>(
        allocTy, arraySize, astCtx.copyArray<ExprNode *>(args),
        astCtx.copyArray<std::string_view>(argNames), hasParens, line, col,
        currentToken().column - col);
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
    return astCtx.create<UnaryOpNode>(op, expr, line, col);
  }

  return parsePostfix();
}

DeclNode *Parser::parseStructDecl() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  std::string_view name = currentToken().value;
  expect(TokenType::IDENTIFIER, "Expected struct name");

  RecordType *structTy = astCtx.createRecordType(TypeKind::Struct, name);

  if (match(TokenType::SEMICOLON)) {
    auto node = astCtx.create<StructDeclNode>(name, line, col,
                                              currentToken().column - col);
    node->isOpaque = true;
    node->recordType = structTy;
    return node;
  }

  structTy->setOpaque(false);

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
      expect(TokenType::IDENTIFIER, "Expected struct name after '~'");

      if (dtorName != name) {
        reportError(dLine, dCol, dtorName.length(),
                    "Destructor name must match the struct name.");
        throw ParseException();
      }

      expect(TokenType::LPAREN, "Expected '('");
      expect(TokenType::RPAREN, "Expected ')'");

      destructor = astCtx.create<FunctionDeclNode>(astCtx.VoidTy, "~", dLine,
                                                   dCol, false, true);

      std::vector<ParamDeclNode *> params;
      params.push_back(
          astCtx.create<ParamDeclNode>(astCtx.getPointerType(structTy), "this",
                                       nullptr, false, false, dLine, dCol, 4));

      destructor->params = astCtx.copyArray<ParamDeclNode *>(params);
      destructor->annotations = memberAnnotations;
      destructor->hasPublicMod = isPub;
      destructor->hasPrivateMod = isPriv;
      if (!doc.empty())
        destructor->docString = astCtx.copyString(doc);

      destructor->body = parseFunctionBody(astCtx.VoidTy);
      continue;
    }

    bool isConstCtor = false;
    if (currentToken().type == TokenType::CONST_KW &&
        peekToken().type == TokenType::IDENTIFIER &&
        peekToken().value == name && peekToken(2).type == TokenType::LPAREN) {
      isConstCtor = true;
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
      auto params = parseParameterList(structTy, isVariadic);
      expect(TokenType::RPAREN, "Expected ')'");

      auto constructor =
          astCtx.create<FunctionDeclNode>(astCtx.VoidTy, name, cLine, cCol,
                                          isConstCtor, true, false, isVariadic);
      constructor->params = astCtx.copyArray<ParamDeclNode *>(params);
      constructor->annotations = memberAnnotations;
      constructor->hasPublicMod = isPub;
      constructor->hasPrivateMod = isPriv;
      if (!doc.empty())
        constructor->docString = astCtx.copyString(doc);

      constructor->body = parseFunctionBody(astCtx.VoidTy);
      constructors.push_back(constructor);
      continue;
    }

    size_t methodStartIdx = cursor;
    bool isStatic = false;
    bool isExtern = false;

    while (currentToken().type == TokenType::STATIC_KW ||
           currentToken().type == TokenType::EXTERN_KW) {
      if (currentToken().type == TokenType::STATIC_KW)
        isStatic = true;
      if (currentToken().type == TokenType::EXTERN_KW)
        isExtern = true;
      advance();
    }

    if (!isExtern) {
      for (const auto *ann : memberAnnotations) {
        if (ann->name == "extern") {
          isExtern = true;
          break;
        }
      }
    }

    int mLine = currentToken().line;
    int mCol = currentToken().column;
    const Type *memType = parseType();

    std::string_view memName;
    if (match(TokenType::OPERATOR_KW)) {
      memName = parseOperatorName();
    } else {
      memName = currentToken().value;
      expect(TokenType::IDENTIFIER, "Expected member name");
    }

    std::vector<std::string_view> methodTParams;
    if (match(TokenType::LT)) {
      if (currentToken().type != TokenType::GT) {
        do {
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
      auto params = parseParameterList(
          (isExtern || isStatic) ? nullptr : structTy, isVariadic);
      expect(TokenType::RPAREN, "Expected ')'");

      auto method = astCtx.create<FunctionDeclNode>(
          memType, memName, mLine, mCol, false, true, isExtern, isVariadic);
      method->isStatic = isStatic;
      method->params = astCtx.copyArray<ParamDeclNode *>(params);
      method->annotations = memberAnnotations;
      method->hasPublicMod = isPub;
      method->hasPrivateMod = isPriv;
      if (!doc.empty())
        method->docString = astCtx.copyString(doc);

      if (isExtern) {
        expect(TokenType::SEMICOLON,
               "Expected ';' after extern method declaration");
      } else {
        method->body = parseFunctionBody(memType);
      }

      if (!methodTParams.empty()) {
        method->isTemplate = true;
        method->templateParams =
            astCtx.copyArray<std::string_view>(methodTParams);
        method->templateBodyTokens =
            tokens.slice(methodStartIdx, cursor - methodStartIdx);
        popTemplateParams(methodTParams.size());
      }

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

      ExprNode *init = nullptr;
      if (match(TokenType::ASSIGN)) {
        init = parseExpression();
      }
      expect(TokenType::SEMICOLON, "Expected ';'");

      auto field = astCtx.create<VarDeclNode>(memType, memName, init, mLine,
                                              mCol, memName.length());
      field->isStatic = isStatic;
      field->annotations = memberAnnotations;
      field->hasPublicMod = isPub;
      field->hasPrivateMod = isPriv;
      if (!doc.empty())
        field->docString = astCtx.copyString(doc);

      fields.push_back(field);
    }
  }

  if (constructors.empty()) {
    auto defaultCtor = astCtx.create<FunctionDeclNode>(
        astCtx.VoidTy, name, line, col, false, true, false, false, true);

    std::vector<ParamDeclNode *> params;
    params.push_back(
        astCtx.create<ParamDeclNode>(astCtx.getPointerType(structTy), "this",
                                     nullptr, false, false, line, col, 4));
    defaultCtor->params = astCtx.copyArray<ParamDeclNode *>(params);

    auto emptyBody = astCtx.create<BlockNode>(line, col);
    emptyBody->statements = {};
    emptyBody->length = 0;
    defaultCtor->body = emptyBody;

    constructors.push_back(defaultCtor);
  }

  if (!destructor) {
    destructor = astCtx.create<FunctionDeclNode>(
        astCtx.VoidTy, "~", line, col, false, true, false, false, true);
    std::vector<ParamDeclNode *> params;
    params.push_back(
        astCtx.create<ParamDeclNode>(astCtx.getPointerType(structTy), "this",
                                     nullptr, false, false, line, col, 4));
    destructor->params = astCtx.copyArray<ParamDeclNode *>(params);

    auto emptyBody = astCtx.create<BlockNode>(line, col);
    emptyBody->statements = {};
    emptyBody->length = 0;
    destructor->body = emptyBody;
  }

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
  structTy->setFields(astCtx.copyArray<FieldInfo>(fInfos));

  auto node = astCtx.create<StructDeclNode>(name, line, col, endCol - col);
  node->fields = astCtx.copyArray<VarDeclNode *>(fields);
  node->methods = astCtx.copyArray<FunctionDeclNode *>(methods);
  node->constructors = astCtx.copyArray<FunctionDeclNode *>(constructors);
  node->destructor = destructor;

  return node;
}

DeclNode *Parser::parseClassDecl() {
  size_t startIdx = cursor;
  int line = currentToken().line;
  int col = currentToken().column;
  advance();

  std::string_view name = currentToken().value;
  if (!instantiatingName.empty() && isTopLevelInst) {
    name = instantiatingName;
    isTopLevelInst = false;
  }
  expect(TokenType::IDENTIFIER, "Expected class name");

  std::vector<std::string_view> tParams;
  if (match(TokenType::LT)) {
    if (currentToken().type != TokenType::GT) {
      do {
        tParams.push_back(currentToken().value);
        if (instantiatingName.empty()) {
          pushTemplateParam(tParams.back());
        }
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

  RecordType *classTy = astCtx.createRecordType(TypeKind::Class, name);

  if (match(TokenType::SEMICOLON)) {
    auto node = astCtx.create<ClassDeclNode>(name, line, col,
                                             currentToken().column - col);
    node->isOpaque = true;
    node->recordType = classTy;
    if (instantiatingName.empty())
      popTemplateParams(tParams.size());
    return node;
  }

  classTy->setOpaque(false);

  expect(TokenType::LBRACE, "Expected '{'");

  std::vector<VarDeclNode *> fields;
  std::vector<FunctionDeclNode *> methods;
  std::vector<FunctionDeclNode *> constructors;
  FunctionDeclNode *destructor = nullptr;

  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {

    /* Aggressively intercept decorators and documentation bound to internal
     * declarations */
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

    if (currentToken().type == TokenType::TILDE) {
      int dLine = currentToken().line;
      int dCol = currentToken().column;

      advance();

      std::string_view dtorName = currentToken().value;
      expect(TokenType::IDENTIFIER, "Expected class name after '~'");

      if (dtorName != (instantiatingName.empty() ? name : templateBaseName)) {
        reportError(dLine, dCol, dtorName.length(),
                    "Destructor name must match the class name.");
        throw ParseException();
      }

      expect(TokenType::LPAREN, "Expected '('");
      expect(TokenType::RPAREN, "Expected ')'");

      destructor = astCtx.create<FunctionDeclNode>(astCtx.VoidTy, "~", dLine,
                                                   dCol, false, true);

      /* Inject implicit contextual 'this' binding to maintain static soundness
       */
      std::vector<ParamDeclNode *> params;
      params.push_back(
          astCtx.create<ParamDeclNode>(astCtx.getPointerType(classTy), "this",
                                       nullptr, false, false, dLine, dCol, 4));

      destructor->params = astCtx.copyArray<ParamDeclNode *>(params);
      destructor->annotations = memberAnnotations;
      destructor->hasPublicMod = isPub;
      destructor->hasPrivateMod = isPriv;
      if (!doc.empty())
        destructor->docString = astCtx.copyString(doc);

      destructor->body = parseFunctionBody(astCtx.VoidTy);
      continue;
    }

    /* Constructor pattern matching */
    bool isConstCtor = false;
    if (currentToken().type == TokenType::CONST_KW &&
        peekToken().type == TokenType::IDENTIFIER &&
        peekToken().value ==
            (instantiatingName.empty() ? name : templateBaseName) &&
        peekToken(2).type == TokenType::LPAREN) {
      isConstCtor = true;
    }

    if (isConstCtor ||
        (currentToken().type == TokenType::IDENTIFIER &&
         currentToken().value ==
             (instantiatingName.empty() ? name : templateBaseName) &&
         peekToken().type == TokenType::LPAREN)) {
      int cLine = currentToken().line;
      int cCol = currentToken().column;

      if (isConstCtor)
        advance();
      advance();
      advance();

      bool isVariadic = false;
      auto params = parseParameterList(classTy, isVariadic);
      expect(TokenType::RPAREN, "Expected ')'");

      auto constructor =
          astCtx.create<FunctionDeclNode>(astCtx.VoidTy, name, cLine, cCol,
                                          isConstCtor, true, false, isVariadic);
      constructor->params = astCtx.copyArray<ParamDeclNode *>(params);
      constructor->annotations = memberAnnotations;
      constructor->hasPublicMod = isPub;
      constructor->hasPrivateMod = isPriv;
      if (!doc.empty())
        constructor->docString = astCtx.copyString(doc);

      constructor->body = parseFunctionBody(astCtx.VoidTy);
      constructors.push_back(constructor);
      continue;
    }

    size_t methodStartIdx = cursor;
    bool isStatic = false;
    bool isExtern = false;

    while (currentToken().type == TokenType::STATIC_KW ||
           currentToken().type == TokenType::EXTERN_KW) {
      if (currentToken().type == TokenType::STATIC_KW)
        isStatic = true;
      if (currentToken().type == TokenType::EXTERN_KW)
        isExtern = true;
      advance();
    }

    if (!isExtern) {
      for (const auto *ann : memberAnnotations) {
        if (ann->name == "extern") {
          isExtern = true;
          break;
        }
      }
    }

    int mLine = currentToken().line;
    int mCol = currentToken().column;
    const Type *memType = parseType();

    std::string_view memName;
    if (match(TokenType::OPERATOR_KW)) {
      memName = parseOperatorName();
    } else {
      memName = currentToken().value;
      expect(TokenType::IDENTIFIER, "Expected member name");
    }

    std::vector<std::string_view> methodTParams;
    if (match(TokenType::LT)) {
      if (currentToken().type != TokenType::GT) {
        do {
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
      auto params = parseParameterList(
          (isExtern || isStatic) ? nullptr : classTy, isVariadic);
      expect(TokenType::RPAREN, "Expected ')'");

      auto method = astCtx.create<FunctionDeclNode>(
          memType, memName, mLine, mCol, false, true, isExtern, isVariadic);
      method->isStatic = isStatic;
      method->params = astCtx.copyArray<ParamDeclNode *>(params);
      method->annotations = memberAnnotations;
      method->hasPublicMod = isPub;
      method->hasPrivateMod = isPriv;

      if (!doc.empty())
        method->docString = astCtx.copyString(doc);

      if (isExtern) {
        expect(TokenType::SEMICOLON,
               "Expected ';' after extern method declaration");
      } else {
        method->body = parseFunctionBody(memType);
      }

      if (!methodTParams.empty()) {
        method->isTemplate = true;
        method->templateParams =
            astCtx.copyArray<std::string_view>(methodTParams);
        method->templateBodyTokens =
            tokens.slice(methodStartIdx, cursor - methodStartIdx);
        popTemplateParams(methodTParams.size());
      }

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

      ExprNode *init = nullptr;
      if (match(TokenType::ASSIGN)) {
        init = parseExpression();
      }
      expect(TokenType::SEMICOLON, "Expected ';'");

      auto field = astCtx.create<VarDeclNode>(memType, memName, init, mLine,
                                              mCol, memName.length());
      field->isStatic = isStatic;
      field->annotations = memberAnnotations;
      field->hasPublicMod = isPub;
      field->hasPrivateMod = isPriv;
      if (!doc.empty())
        field->docString = astCtx.copyString(doc);

      fields.push_back(field);
    }
  }

  if (constructors.empty()) {
    auto defaultCtor = astCtx.create<FunctionDeclNode>(
        astCtx.VoidTy, name, line, col, false, true, false, false, true);

    std::vector<ParamDeclNode *> params;
    params.push_back(
        astCtx.create<ParamDeclNode>(astCtx.getPointerType(classTy), "this",
                                     nullptr, false, false, line, col, 4));
    defaultCtor->params = astCtx.copyArray<ParamDeclNode *>(params);

    auto emptyBody = astCtx.create<BlockNode>(line, col);
    emptyBody->statements = {};
    emptyBody->length = 0;
    defaultCtor->body = emptyBody;

    constructors.push_back(defaultCtor);
  }

  if (!destructor) {
    destructor = astCtx.create<FunctionDeclNode>(
        astCtx.VoidTy, "~", line, col, false, true, false, false, true);
    std::vector<ParamDeclNode *> params;
    params.push_back(
        astCtx.create<ParamDeclNode>(astCtx.getPointerType(classTy), "this",
                                     nullptr, false, false, line, col, 4));
    destructor->params = astCtx.copyArray<ParamDeclNode *>(params);

    auto emptyBody = astCtx.create<BlockNode>(line, col);
    emptyBody->statements = {};
    emptyBody->length = 0;
    destructor->body = emptyBody;
  }

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
  classTy->setFields(astCtx.copyArray<FieldInfo>(fInfos));

  auto node = astCtx.create<ClassDeclNode>(name, line, col, endCol - col);
  node->fields = astCtx.copyArray<VarDeclNode *>(fields);
  node->methods = astCtx.copyArray<FunctionDeclNode *>(methods);
  node->constructors = astCtx.copyArray<FunctionDeclNode *>(constructors);
  node->destructor = destructor;

  if (instantiatingName.empty()) {
    popTemplateParams(tParams.size());
  }

  if (!tParams.empty() && instantiatingName.empty()) {
    node->isTemplate = true;
    node->templateParams = astCtx.copyArray<std::string_view>(tParams);
    node->templateBodyTokens = tokens.slice(startIdx, cursor - startIdx);
  }

  return node;
}

DeclNode *Parser::parseEnumDecl() {
  int line = currentToken().line;
  int col = currentToken().column;
  advance(); /* consume 'enum' */

  std::string_view name = currentToken().value;
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
    while (currentToken().type == TokenType::COMMENT)
      advance();
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

    int mLine = currentToken().line;
    int mCol = currentToken().column;
    std::string_view mName = currentToken().value;
    expect(TokenType::IDENTIFIER, "Expected enum member name");

    ExprNode *init = nullptr;
    if (match(TokenType::ASSIGN)) {
      init = parseExpression();
    }

    auto memberNode = astCtx.create<EnumMemberNode>(
        mName, init, mLine, mCol, currentToken().column - mCol);
    memberNode->hasPublicMod = isPub;
    memberNode->hasPrivateMod = isPriv;
    members.push_back(memberNode);

    if (!match(TokenType::COMMA)) {
      break;
    }
  }

  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACE, "Expected '}'");

  auto node = astCtx.create<EnumDeclNode>(name, underlyingType, line, col,
                                          endCol - col);
  node->members = astCtx.copyArray<EnumMemberNode *>(members);
  node->enumType = astCtx.getEnumType(name, underlyingType);
  return node;
}

DeclNode *Parser::parseDeclarationOrFunction(
    llvm::ArrayRef<AnnotationNode *> annotations) {
  size_t startIdx = cursor;
  int line = currentToken().line;
  int col = currentToken().column;

  bool isExtern = false;
  bool isStatic = false;

  while (currentToken().type == TokenType::EXTERN_KW ||
         currentToken().type == TokenType::STATIC_KW) {
    if (currentToken().type == TokenType::EXTERN_KW)
      isExtern = true;
    if (currentToken().type == TokenType::STATIC_KW)
      isStatic = true;
    advance();
  }

  /* Fallback to checking the presence of the @extern annotation */
  if (!isExtern) {
    for (const auto *ann : annotations) {
      if (ann->name == "extern") {
        isExtern = true;
        break;
      }
    }
  }

  const Type *nodeType = parseType();

  std::string_view id = currentToken().value;
  if (!instantiatingName.empty() && isTopLevelInst) {
    id = instantiatingName;
    isTopLevelInst = false;
  }

  int idLen = (int)id.length();
  expect(TokenType::IDENTIFIER, "Expected identifier after type");

  std::vector<std::string_view> tParams;
  if (match(TokenType::LT)) {
    if (currentToken().type != TokenType::GT) {
      do {
        tParams.push_back(currentToken().value);
        if (instantiatingName.empty()) {
          pushTemplateParam(tParams.back());
        }
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
    auto params = parseParameterList(nullptr, isVariadic);
    expect(TokenType::RPAREN, "Expected ')' after parameters");

    bool isFuncConst = match(TokenType::CONST_KW);

    auto funcDecl = astCtx.create<FunctionDeclNode>(
        nodeType, id, line, col, isFuncConst, false, isExtern, isVariadic);
    funcDecl->isStatic = isStatic;
    funcDecl->params = astCtx.copyArray<ParamDeclNode *>(params);

    if (isExtern) {
      int endCol = currentToken().column + currentToken().value.length();
      expect(TokenType::SEMICOLON,
             "Expected ';' after extern function declaration");
      funcDecl->length = endCol - col;
    } else {
      funcDecl->body = parseFunctionBody(nodeType);
      funcDecl->length = funcDecl->body->column + funcDecl->body->length - col;
    }

    if (instantiatingName.empty()) {
      popTemplateParams(tParams.size());
    }

    if (!tParams.empty() && instantiatingName.empty()) {
      funcDecl->isTemplate = true;
      funcDecl->templateParams = astCtx.copyArray<std::string_view>(tParams);
      funcDecl->templateBodyTokens = tokens.slice(startIdx, cursor - startIdx);
    }
    return funcDecl;
  }

  if (isExtern) {
    reportError(line, col, idLen, "Variables cannot be declared as extern.");
    throw ParseException();
  }

  if (isStatic) {
    reportError(line, col, idLen, "Variables cannot be declared as static.");
    throw ParseException();
  }

  ExprNode *init = nullptr;
  if (match(TokenType::ASSIGN)) {
    init = parseExpression();
  }
  int endCol = currentToken().column + (int)currentToken().value.length();
  expect(TokenType::SEMICOLON, "Expected ';' after variable declaration");

  return astCtx.create<VarDeclNode>(nodeType, id, init, line, col,
                                    endCol - col);
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

  int endCol = currentToken().column + 1;
  expect(TokenType::RBRACE, "Expected '}'");
  block->statements = astCtx.copyArray<ASTNode *>(statements);
  block->finalize(endCol);
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
  int endCol = currentToken().column + (int)currentToken().value.length();
  expect(TokenType::SEMICOLON, "Expected ';'");
  return astCtx.create<ReturnNode>(val, line, col, endCol - col);
}

ExprNode *Parser::parseExpressionStatement() {
  auto expr = parseExpression();
  expect(TokenType::SEMICOLON, "Expected ';'");
  return expr;
}

ExprNode *Parser::parseExpression() { return parseAssignment(); }

ExprNode *Parser::parseAssignment() {
  auto expr = parseLogicalOr();

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

    const Type *targetType = parseType();

    left = astCtx.create<CastNode>(
        left, targetType, line, col,
        (currentToken().column + currentToken().value.length()) - col);
  }
  return left;
}

ExprNode *Parser::parsePostfix() {
  auto expr = parsePrimary();
  while (true) {
    while (currentToken().type == TokenType::COMMENT)
      advance();

    if (match(TokenType::DOT)) {
      int line = expr->line;
      int col = expr->column;
      std::string_view memberName = currentToken().value;
      int memLen = memberName.length();
      expect(TokenType::IDENTIFIER, "Expected member name after '.'");

      /* Check for template invocation on method access */
      bool isTemplateCall = false;
      if (currentToken().type == TokenType::LT) {
        size_t lookahead = 1;
        while (peekToken(lookahead).type != TokenType::EOF_TOK &&
               peekToken(lookahead).type != TokenType::SEMICOLON) {
          if (peekToken(lookahead).type == TokenType::GT ||
              peekToken(lookahead).type == TokenType::RSHIFT) {
            if (peekToken(lookahead + 1).type == TokenType::LPAREN) {
              isTemplateCall = true;
            }
            break;
          }
          lookahead++;
        }
      }

      auto maNode = astCtx.create<MemberAccessNode>(
          expr, memberName, line, col, (currentToken().column + memLen) - col);

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

      while (currentToken().type == TokenType::COMMENT)
        advance();

      if (currentToken().type != TokenType::RPAREN) {
        do {
          while (currentToken().type == TokenType::COMMENT)
            advance();
          if (currentToken().type == TokenType::RPAREN)
            break;

          if (currentToken().type == TokenType::IDENTIFIER &&
              peekToken().type == TokenType::COLON) {
            namedStarted = true;
            argNames.push_back(currentToken().value);
            advance();
            advance();
            while (currentToken().type == TokenType::COMMENT)
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
          }

          while (currentToken().type == TokenType::COMMENT)
            advance();
        } while (match(TokenType::COMMA));
      }
      while (currentToken().type == TokenType::COMMENT)
        advance();
      int endCol = currentToken().column + (int)currentToken().value.length();
      expect(TokenType::RPAREN, "Expected ')'");

      auto argsRef = astCtx.copyArray<ExprNode *>(args);
      auto namesRef = astCtx.copyArray<std::string_view>(argNames);
      expr = astCtx.create<FunctionCallNode>(expr, argsRef, namesRef, line, col,
                                             endCol - col);
    } else if (currentToken().type == TokenType::PLUS_PLUS ||
               currentToken().type == TokenType::MINUS_MINUS) {
      int line = expr->line;
      int col = expr->column;
      std::string_view op = currentToken().value;
      advance();
      /* Evaluate postfix increments properly by passing isPostfix=true */
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

    bool isFloat = raw.find('.') != std::string_view::npos ||
                   raw.find('e') != std::string_view::npos ||
                   raw.find('E') != std::string_view::npos ||
                   raw.find('f') != std::string_view::npos ||
                   raw.find('F') != std::string_view::npos;

    return astCtx.create<NumberNode>(raw, isFloat, line, col, len);
  }

  if (currentToken().type == TokenType::CHAR_LITERAL) {
    std::string_view raw = currentToken().value;
    uint8_t val = 0;
    if (raw.length() >= 3) {
      if (raw[1] == '\\') {
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
        unsigned char c = inner[0];
        if (c < 0x80) {
          val = c;
        } else if ((c & 0xE0) == 0xC0) {
          val = ((c & 0x1F) << 6) | (inner[1] & 0x3F);
        } else if ((c & 0xF0) == 0xE0) {
          val =
              ((c & 0x0F) << 12) | ((inner[1] & 0x3F) << 6) | (inner[2] & 0x3F);
        } else if ((c & 0xF8) == 0xF0) {
          val = ((c & 0x07) << 18) | ((inner[1] & 0x3F) << 12) |
                ((inner[2] & 0x3F) << 6) | (inner[3] & 0x3F);
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
    if (currentToken().type == TokenType::LT) {
      size_t lookahead = 1;
      while (peekToken(lookahead).type != TokenType::EOF_TOK &&
             peekToken(lookahead).type != TokenType::SEMICOLON) {
        if (peekToken(lookahead).type == TokenType::GT ||
            peekToken(lookahead).type == TokenType::RSHIFT) {
          if (peekToken(lookahead + 1).type == TokenType::LPAREN) {
            isTemplateCall = true;
          }
          break;
        }
        lookahead++;
      }
    }

    auto varNode = astCtx.create<VariableNode>(name, line, col, len);

    if (isTemplateCall) {
      advance(); // '<'
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