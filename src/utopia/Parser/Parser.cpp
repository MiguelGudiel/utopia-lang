#include "utopia/Parser/Parser.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>

namespace utopia {

Parser::Parser(const std::vector<Token> &tokens) : tokens(tokens) {}

void Parser::finalizeNode(ASTNode *node, const Token &startToken) {
  const Token &endToken = tokens[cursor > 0 ? cursor - 1 : 0];
  node->setRange(startToken.line, startToken.column, endToken.line,
                 endToken.column + (int)endToken.value.length());
}

const Token &Parser::currentToken() const { return tokens[cursor]; }
const Token &Parser::peekNextToken() const {
  return (cursor + 1 < tokens.size()) ? tokens[cursor + 1] : tokens.back();
}
void Parser::advance() {
  if (cursor < tokens.size() - 1)
    cursor++;
}

bool Parser::match(TokenType type) {
  if (currentToken().type == type) {
    advance();
    return true;
  }
  return false;
}

void Parser::expect(TokenType type, const std::string &errorMessage) {
  if (!match(type)) {
    const Token &tok = currentToken();
    std::string info = std::to_string(tok.line) + ":" +
                       std::to_string(tok.column) + "|" + errorMessage +
                       " (Found: '" + tok.value + "')";
    throw std::runtime_error(info);
  }
}

bool Parser::isTypeToken() const {
  TokenType t = currentToken().type;
  return t == TokenType::KW_INT || t == TokenType::KW_FLOAT ||
         t == TokenType::KW_DOUBLE || t == TokenType::KW_BOOL ||
         t == TokenType::KW_UINT || t == TokenType::KW_VOID ||
         t == TokenType::KW_CHAR || t == TokenType::KW_UCHAR ||
         t == TokenType::KW_SHORT || t == TokenType::KW_USHORT ||
         t == TokenType::KW_LONG || t == TokenType::KW_ULONG;
}

DeclPreamble Parser::parsePreamble() {
  DeclPreamble preamble;
  while (true) {
    if (match(TokenType::AT)) {
      std::string decName = currentToken().value;
      expect(TokenType::IDENTIFIER, "Expected decorator name");
      preamble.decorators.push_back(decName);
      if (match(TokenType::LPAREN)) {
        int depth = 1;
        while (depth > 0 && currentToken().type != TokenType::EOF_TOK) {
          if (currentToken().type == TokenType::LPAREN)
            depth++;
          else if (currentToken().type == TokenType::RPAREN)
            depth--;
          advance();
        }
      }
    } else if (match(TokenType::KW_PUBLIC)) {
      preamble.access = AccessModifier::Public;
    } else if (match(TokenType::KW_PRIVATE)) {
      preamble.access = AccessModifier::Private;
    } else if (match(TokenType::KW_INLINE)) {
      preamble.inlineState = InlineState::Inline;
    } else if (match(TokenType::KW_FORCE_INLINE)) {
      preamble.inlineState = InlineState::ForceInline;
    } else if (match(TokenType::KW_CONST)) {
      preamble.isConst = true;
    } else if (match(TokenType::KW_STATIC)) {
      preamble.isStatic = true;
    } else {
      break;
    }
  }
  return preamble;
}

bool Parser::isVarDeclaration() const {
  size_t tempCursor = cursor;

  while (tempCursor < tokens.size()) {
    TokenType t = tokens[tempCursor].type;
    if (t == TokenType::KW_CONST || t == TokenType::KW_PUBLIC ||
        t == TokenType::KW_PRIVATE || t == TokenType::KW_INLINE ||
        t == TokenType::KW_FORCE_INLINE || t == TokenType::KW_STATIC) {
      tempCursor++;
    } else if (t == TokenType::AT) {
      tempCursor++;
      if (tempCursor < tokens.size() &&
          tokens[tempCursor].type == TokenType::IDENTIFIER) {
        tempCursor++;
        if (tempCursor < tokens.size() &&
            tokens[tempCursor].type == TokenType::LPAREN) {
          int depth = 1;
          tempCursor++;
          while (depth > 0 && tempCursor < tokens.size()) {
            if (tokens[tempCursor].type == TokenType::LPAREN)
              depth++;
            else if (tokens[tempCursor].type == TokenType::RPAREN)
              depth--;
            tempCursor++;
          }
        }
      }
    } else {
      break;
    }
  }

  TokenType t = tokens[tempCursor].type;

  if (t == TokenType::KW_INT || t == TokenType::KW_FLOAT ||
      t == TokenType::KW_DOUBLE || t == TokenType::KW_BOOL ||
      t == TokenType::KW_UINT || t == TokenType::KW_VOID ||
      t == TokenType::KW_CHAR || t == TokenType::KW_UCHAR ||
      t == TokenType::KW_SHORT || t == TokenType::KW_USHORT ||
      t == TokenType::KW_LONG || t == TokenType::KW_ULONG) {
    return true;
  }

  if (t == TokenType::IDENTIFIER) {
    tempCursor++;
    while (tempCursor < tokens.size()) {
      TokenType next = tokens[tempCursor].type;
      if (next == TokenType::STAR || next == TokenType::QUESTION) {
        tempCursor++;
      } else if (next == TokenType::IDENTIFIER) {
        return true;
      } else {
        return false;
      }
    }
  }
  return false;
}

std::string Parser::consumeType() {
  TokenType t = currentToken().type;

  if (isTypeToken() || t == TokenType::IDENTIFIER) {
    std::string typeName = currentToken().value;
    advance();
    return typeName;
  }

  const Token &tok = currentToken();
  throw std::runtime_error(
      std::to_string(tok.line) + ":" + std::to_string(tok.column) +
      "|Syntax Error: Expected a data type. (Found: '" + tok.value + "')");
}

std::string Parser::parseTypeName() {
  std::string typeName = consumeType();
  while (match(TokenType::STAR))
    typeName += "*";
  if (match(TokenType::QUESTION))
    typeName += "?";
  if (match(TokenType::AND))
    typeName += "&&";
  if (match(TokenType::AMPERSAND))
    typeName += "&";
  return typeName;
}

bool Parser::isFunctionStart() const {
  TokenType t = currentToken().type;
  size_t tempCursor = cursor;

  if (t == TokenType::TILDE) {
    tempCursor++;
    if (tempCursor < tokens.size() &&
        tokens[tempCursor].type == TokenType::IDENTIFIER) {
      tempCursor++;
      if (tempCursor < tokens.size() &&
          tokens[tempCursor].type == TokenType::LPAREN) {
        return true;
      }
    }
  }

  if (t == TokenType::IDENTIFIER && tempCursor + 1 < tokens.size()) {
    if (tokens[tempCursor + 1].type == TokenType::LPAREN) {
      return true;
    }
  }

  if (isTypeToken() || t == TokenType::IDENTIFIER) {
    tempCursor++;
    while (tempCursor < tokens.size() &&
           (tokens[tempCursor].type == TokenType::STAR ||
            tokens[tempCursor].type == TokenType::QUESTION)) {
      tempCursor++;
    }
    if (tempCursor < tokens.size() &&
        tokens[tempCursor].type == TokenType::IDENTIFIER) {
      tempCursor++;
      if (tempCursor < tokens.size() &&
          tokens[tempCursor].type == TokenType::LPAREN) {
        return true;
      }
    }
  }
  return false;
}

std::unique_ptr<ModuleNode> Parser::parseModule(const std::string &filename) {
  auto module = std::make_unique<ModuleNode>(filename);

  while (currentToken().type != TokenType::EOF_TOK) {
    if (currentToken().type == TokenType::KW_IMPORT) {
      parseImportInto(module.get());
      continue;
    }

    DeclPreamble preamble = parsePreamble();

    if (match(TokenType::KW_STRUCT)) {
      module->structs.push_back(parseStructDecl(false, preamble));
    } else if (match(TokenType::KW_CLASS)) {
      module->structs.push_back(parseStructDecl(true, preamble));
    } else if (match(TokenType::KW_EXTENSION)) {
      module->extensions.push_back(parseExtension());
    } else if (isFunctionStart()) {
      auto func = parseFunction(preamble);
      std::cerr << "[Parser] Added function " << func->name << " to module "
                << filename << "\n";
      module->functions.push_back(std::move(func));

    } else if (isVarDeclaration()) {
      auto stmt = parseStatement();
      if (auto varDecl = dynamic_cast<VarDeclNode *>(stmt.get())) {
        stmt.release();
        module->globalVars.push_back(std::unique_ptr<VarDeclNode>(varDecl));
      } else {
        throw std::runtime_error("Expected global variable declaration");
      }
    } else {
      const Token &stray = currentToken();
      throw std::runtime_error(
          std::to_string(stray.line) + ":" + std::to_string(stray.column) +
          "|Syntax Error: Unexpected token '" + stray.value + "'");
    }
  }
  return module;
}

std::unique_ptr<StructDeclNode>
Parser::parseStructDecl(bool isClass, const DeclPreamble &preamble) {
  Token startTok = currentToken();
  std::string name = currentToken().value;
  expect(TokenType::IDENTIFIER, "The name was expected");

  auto node = std::make_unique<StructDeclNode>(name, isClass);

  if (match(TokenType::KW_EXTENDS)) {
    if (!isClass) {
      const Token &tok = currentToken();
      throw std::runtime_error(std::to_string(tok.line) + ":" +
                               std::to_string(tok.column) +
                               "|Syntax Error: Structs are data containers and "
                               "cannot inherit. Use 'class'.");
    }
    node->baseClass = parseTypeName();
  }

  if (match(TokenType::KW_IMPLEMENTS)) {
    if (!isClass) {
      const Token &tok = currentToken();
      throw std::runtime_error(
          std::to_string(tok.line) + ":" + std::to_string(tok.column) +
          "|Syntax Error: Structs cannot implement interfaces. Use 'class'.");
    }
    do {
      node->interfaces.push_back(parseTypeName());
    } while (match(TokenType::COMMA));
  }

  expect(TokenType::LBRACE, "Expected '{'");

  std::vector<StructField> fields;
  std::vector<std::unique_ptr<FunctionNode>> methods;

  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {

    DeclPreamble memberPreamble = parsePreamble();

    if (isFunctionStart() || currentToken().value == name ||
        currentToken().type == TokenType::TILDE) {
      if (!isClass) {
        const Token &tok = currentToken();
        throw std::runtime_error(
            std::to_string(tok.line) + ":" + std::to_string(tok.column) +
            "|Syntax Error: Structs cannot have methods or constructors.");
      }
      auto method = parseMethod(name, memberPreamble);
      methods.push_back(std::move(method));
      continue;
    }

    std::string typeName = parseTypeName();
    std::string fieldName = currentToken().value;
    expect(TokenType::IDENTIFIER, "The name of the field was expected.");

    std::unique_ptr<ExprNode> fieldInit = nullptr;
    if (match(TokenType::ASSIGN)) {
      fieldInit = parseExpression();
    }

    expect(TokenType::SEMICOLON, "Expected ';'");

    fields.push_back({memberPreamble.access, memberPreamble.isStatic, typeName,
                      fieldName, memberPreamble.decorators,
                      std::move(fieldInit)});
  }
  expect(TokenType::RBRACE, "Expected '}'");

  node->methods = std::move(methods);
  node->fields = std::move(fields);
  node->decorators = preamble.decorators;
  finalizeNode(node.get(), startTok);
  return node;
}

std::unique_ptr<ExtensionNode> Parser::parseExtension() {
  Token startTok = currentToken();
  std::string extName;
  if (currentToken().type == TokenType::IDENTIFIER) {
    extName = currentToken().value;
    advance();
  }

  expect(TokenType::KW_ON, "Expected 'on' keyword");
  std::string target = parseTypeName();

  auto node = std::make_unique<ExtensionNode>(target, extName);
  expect(TokenType::LBRACE, "Expected '{'");
  while (!match(TokenType::RBRACE)) {
    node->methods.push_back(parseMethod(target, parsePreamble()));
  }
  return node;
}

std::unique_ptr<FunctionNode>
Parser::parseMethod(const std::string &className,
                    const DeclPreamble &preamble) {
  Token startTok = currentToken();

  bool isConstructor = false;
  bool isDestructor = false;
  std::string retType = "void";
  std::string funcName;

  if (match(TokenType::TILDE)) {
    isDestructor = true;
    std::string n = currentToken().value;
    expect(TokenType::IDENTIFIER, "Expected class name for destructor");
    funcName = "~" + n;
  } else if (currentToken().value == className &&
             peekNextToken().type == TokenType::LPAREN) {
    isConstructor = true;
    funcName = currentToken().value;
    advance();
  } else {
    retType = parseTypeName();
    funcName = currentToken().value;
    expect(TokenType::IDENTIFIER, "Expected method name");
  }

  expect(TokenType::LPAREN, "Expected '('");

  std::vector<FunctionParam> args;
  while (currentToken().type != TokenType::RPAREN) {
    bool isReq = match(TokenType::KW_REQUIRED);

    if (match(TokenType::KW_THIS)) {
      expect(TokenType::DOT, "Expected '.' after this in constructor param");
      std::string argName = currentToken().value;
      expect(TokenType::IDENTIFIER, "Expected field name");
      args.push_back({"", argName, isReq, true});
    } else {
      std::string argType = parseTypeName();
      std::string argName = currentToken().value;
      expect(TokenType::IDENTIFIER, "Expected param name");
      args.push_back({argType, argName, isReq, false});
    }

    if (currentToken().type == TokenType::COMMA)
      advance();
  }
  expect(TokenType::RPAREN, "Expected ')'");
  auto funcNode = std::make_unique<FunctionNode>(
      preamble.inlineState, preamble.access, preamble.decorators, retType,
      funcName, args, true, preamble.isStatic, isConstructor, isDestructor,
      className);

  if (match(TokenType::SEMICOLON)) {
    finalizeNode(funcNode.get(), startTok);
    return funcNode;
  }

  expect(TokenType::LBRACE, "Expected '{'");

  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {
    funcNode->body.push_back(parseStatement());
  }

  expect(TokenType::RBRACE, "Expected '}'");
  finalizeNode(funcNode.get(), startTok);
  return funcNode;
}

void Parser::parseImportInto(ModuleNode *module) {
  expect(TokenType::KW_IMPORT, "Expected 'import'");
  std::string path = currentToken().value;
  expect(TokenType::STRING, "Expected string literal");
  expect(TokenType::SEMICOLON, "Expected ';'");
  module->imports.push_back(path);
}

std::unique_ptr<FunctionNode>
Parser::parseFunction(const DeclPreamble &preamble) {
  Token startTok = currentToken();

  std::string retType = parseTypeName();
  std::string funcName = currentToken().value;
  expect(TokenType::IDENTIFIER, "Expected name");
  expect(TokenType::LPAREN, "Expected '('");

  std::vector<FunctionParam> args;

  while (currentToken().type != TokenType::RPAREN) {
    std::string argType = parseTypeName();
    std::string argName = currentToken().value;
    expect(TokenType::IDENTIFIER, "Expected arg name");
    args.push_back({argType, argName, false, false});
    if (currentToken().type == TokenType::COMMA)
      advance();
  }
  expect(TokenType::RPAREN, "Expected ')'");
  expect(TokenType::LBRACE, "Expected '{'");

  auto funcNode = std::make_unique<FunctionNode>(
      preamble.inlineState, preamble.access, preamble.decorators, retType,
      funcName, args, false, preamble.isStatic);

  while (currentToken().type != TokenType::RBRACE &&
         currentToken().type != TokenType::EOF_TOK) {
    funcNode->body.push_back(parseStatement());
  }
  expect(TokenType::RBRACE, "Expected '}'");
  finalizeNode(funcNode.get(), startTok);
  return funcNode;
}

std::unique_ptr<ExprNode> Parser::parseExpression() { return parseLogicalOr(); }

std::unique_ptr<ExprNode> Parser::parseLogicalOr() {
  auto left = parseLogicalAnd();
  while (currentToken().type == TokenType::OR) {
    std::string op = currentToken().value;
    advance();
    auto right = parseLogicalAnd();
    left =
        std::make_unique<BinaryOpNode>(op, std::move(left), std::move(right));
  }
  return left;
}

std::unique_ptr<ExprNode> Parser::parseLogicalAnd() {
  auto left = parseEquality();
  while (currentToken().type == TokenType::AND) {
    std::string op = currentToken().value;
    advance();
    auto right = parseEquality();
    left =
        std::make_unique<BinaryOpNode>(op, std::move(left), std::move(right));
  }
  return left;
}

std::unique_ptr<ExprNode> Parser::parseEquality() {
  auto left = parseRelational();
  while (currentToken().type == TokenType::EQ ||
         currentToken().type == TokenType::NEQ) {
    std::string op = currentToken().value;
    advance();
    auto right = parseRelational();
    left =
        std::make_unique<BinaryOpNode>(op, std::move(left), std::move(right));
  }
  return left;
}

std::unique_ptr<ExprNode> Parser::parseRelational() {
  auto left = parseAdditive();
  while (currentToken().type == TokenType::LT ||
         currentToken().type == TokenType::LTE ||
         currentToken().type == TokenType::GT ||
         currentToken().type == TokenType::GTE) {
    std::string op = currentToken().value;
    advance();
    auto right = parseAdditive();
    left =
        std::make_unique<BinaryOpNode>(op, std::move(left), std::move(right));
  }
  return left;
}

std::unique_ptr<ExprNode> Parser::parseAdditive() {
  auto left = parseTerm();
  while (currentToken().type == TokenType::PLUS ||
         currentToken().type == TokenType::MINUS) {
    std::string op = currentToken().value;
    advance();
    auto right = parseTerm();
    left =
        std::make_unique<BinaryOpNode>(op, std::move(left), std::move(right));
  }
  return left;
}

std::unique_ptr<ExprNode> Parser::parseTerm() {
  Token startTok = currentToken();
  auto left = parseCast();
  while (currentToken().type == TokenType::STAR ||
         currentToken().type == TokenType::SLASH ||
         currentToken().type == TokenType::PERCENT) {
    std::string op = currentToken().value;
    advance();
    auto right = parseCast();
    left =
        std::make_unique<BinaryOpNode>(op, std::move(left), std::move(right));
    finalizeNode(left.get(), startTok);
  }
  return left;
}

std::unique_ptr<ExprNode> Parser::parseCast() {
  Token startTok = currentToken();
  auto node = parsePrimary();

  while (match(TokenType::KW_AS)) {
    std::string targetType = parseTypeName();
    auto castNode = std::make_unique<CastNode>(std::move(node), targetType);
    finalizeNode(castNode.get(), startTok);
    node = std::move(castNode);
  }

  return node;
}

std::unique_ptr<ExprNode> Parser::parsePrimary() {
  auto node = parsePrimaryBase();

  while (true) {
    if (match(TokenType::BANG)) {
      Token startTok = currentToken();
      node = std::make_unique<NullAssertNode>(std::move(node));
      finalizeNode(node.get(), startTok);
      continue;
    }

    if (match(TokenType::DOT)) {
      Token startTok = currentToken();
      std::string fieldName = currentToken().value;
      expect(TokenType::IDENTIFIER,
             "The name of the field was expected after the '.'");

      if (match(TokenType::LPAREN)) {
        std::vector<std::unique_ptr<ExprNode>> args;
        while (currentToken().type != TokenType::RPAREN &&
               currentToken().type != TokenType::EOF_TOK) {
          args.push_back(parseExpression());
          if (currentToken().type == TokenType::COMMA)
            advance();
        }
        expect(TokenType::RPAREN, "Expected ')'");

        auto callNode = std::make_unique<CallNode>(fieldName, std::move(args),
                                                   std::move(node));
        node = std::move(callNode);
        finalizeNode(node.get(), startTok);
      } else {
        node = std::make_unique<MemberAccessNode>(std::move(node), fieldName);
        finalizeNode(node.get(), startTok);
      }
    } else if (match(TokenType::LBRACKET)) {
      Token startTok = currentToken();
      auto index = parseExpression();
      expect(TokenType::RBRACKET, "Expected ']' after array index.");
      auto subNode =
          std::make_unique<SubscriptNode>(std::move(node), std::move(index));
      finalizeNode(subNode.get(), startTok);
      node = std::move(subNode);
    } else {
      break;
    }
  }

  return node;
}

std::unique_ptr<ExprNode> Parser::parsePrimaryBase() {
  Token startTok = currentToken();

  if (match(TokenType::LPAREN)) {
    auto expr = parseExpression();
    expect(TokenType::RPAREN, "Expected ')' to close the grouped expression");
    return expr;
  }

  if (match(TokenType::KW_MOVE)) {
    auto expr = std::make_unique<MoveNode>(parseExpression());
    finalizeNode(expr.get(), startTok);
    return expr;
  }

  if (match(TokenType::AMPERSAND))
    return std::make_unique<AddressOfNode>(parsePrimary());
  if (match(TokenType::STAR))
    return std::make_unique<DerefNode>(parsePrimary());
  if (match(TokenType::MINUS))
    return std::make_unique<UnaryMinusNode>(parsePrimary());
  if (match(TokenType::KW_NULL))
    return std::make_unique<NullLiteralNode>();

  if (match(TokenType::BANG)) {
    // Prefix '!': Logical Negation.
    return std::make_unique<LogicalNotNode>(parsePrimary());
  }

  if (match(TokenType::KW_THIS))
    return std::make_unique<ThisNode>();

  if (match(TokenType::KW_SUPER)) {
    if (match(TokenType::LPAREN)) {
      std::vector<std::unique_ptr<ExprNode>> args;
      while (currentToken().type != TokenType::RPAREN &&
             currentToken().type != TokenType::EOF_TOK) {
        args.push_back(parseExpression());
        if (currentToken().type == TokenType::COMMA)
          advance();
      }
      expect(TokenType::RPAREN, "Expected ')' after super arguments");
      auto callNode = std::make_unique<CallNode>("@super", std::move(args));
      finalizeNode(callNode.get(), startTok);
      return callNode;
    }
    auto node = std::make_unique<SuperNode>();
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (match(TokenType::KW_NEW)) {
    std::string typeName = consumeType();
    std::vector<std::unique_ptr<ExprNode>> args;

    auto node = std::make_unique<NewNode>(typeName, std::move(args));

    while (match(TokenType::LBRACKET)) {
      node->arraySizes.push_back(parseExpression());
      expect(TokenType::RBRACKET, "Expected ']' after array size");
    }

    if (match(TokenType::LPAREN)) {
      while (currentToken().type != TokenType::RPAREN &&
             currentToken().type != TokenType::EOF_TOK) {

        node->arguments.push_back(parseExpression());

        if (currentToken().type == TokenType::COMMA)
          advance();
      }
      expect(TokenType::RPAREN, "Expected ')' after constructor arguments");
    }

    return node;
  }

  if (currentToken().type == TokenType::NUMBER) {
    auto node = std::make_unique<NumberNode>(std::stoi(currentToken().value));
    advance();
    return node;
  }
  if (currentToken().type == TokenType::FLOAT_LITERAL) {
    auto node = std::make_unique<FloatNode>(std::stod(currentToken().value));
    advance();
    return node;
  }
  if (currentToken().type == TokenType::FLOAT_LITERAL_FLOAT) {
    double val = std::stod(currentToken().value);
    auto node = std::make_unique<FloatNode>(val, false); // float
    advance();
    return node;
  }
  if (currentToken().type == TokenType::FLOAT_LITERAL_DOUBLE) {
    double val = std::stod(currentToken().value);
    auto node = std::make_unique<FloatNode>(val, true); // double
    advance();
    return node;
  }
  if (currentToken().type == TokenType::STRING) {
    auto node = std::make_unique<StringNode>(currentToken().value);
    advance();
    return node;
  }
  if (match(TokenType::KW_TRUE))
    return std::make_unique<BoolNode>(true);
  if (match(TokenType::KW_FALSE))
    return std::make_unique<BoolNode>(false);

  if (currentToken().type == TokenType::IDENTIFIER) {
    std::string name = currentToken().value;
    advance();
    if (match(TokenType::LPAREN)) {
      std::vector<std::unique_ptr<ExprNode>> args;
      while (currentToken().type != TokenType::RPAREN &&
             currentToken().type != TokenType::EOF_TOK) {
        args.push_back(parseExpression());
        if (currentToken().type == TokenType::COMMA)
          advance();
      }
      auto callNode = std::make_unique<CallNode>(name, std::move(args));
      expect(TokenType::RPAREN, "Expected ')'");
      finalizeNode(callNode.get(), startTok);
      return callNode;
    }
    return std::make_unique<VariableNode>(name);
  }

  const Token &tok = currentToken();
  throw std::runtime_error(
      std::to_string(tok.line) + ":" + std::to_string(tok.column) +
      "|Error de Sintaxis: Expresion invalida '" + tok.value + "'");
}

std::unique_ptr<ASTNode> Parser::parseStatement() {
  Token startTok = currentToken();

  if (match(TokenType::LBRACE)) {
    std::vector<std::unique_ptr<ASTNode>> stmts;
    while (!match(TokenType::RBRACE) &&
           currentToken().type != TokenType::EOF_TOK) {
      stmts.push_back(parseStatement());
    }
    auto node = std::make_unique<BlockNode>(std::move(stmts));
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (match(TokenType::KW_IF)) {
    expect(TokenType::LPAREN, "Expected '('");
    auto condition = parseExpression();
    expect(TokenType::RPAREN, "Expected ')'");

    expect(TokenType::LBRACE, "Expected '{'");
    std::vector<std::unique_ptr<ASTNode>> thenBody;
    while (!match(TokenType::RBRACE) &&
           currentToken().type != TokenType::EOF_TOK) {
      thenBody.push_back(parseStatement());
    }

    std::vector<std::unique_ptr<ASTNode>> elseBody;
    if (match(TokenType::KW_ELSE)) {
      if (currentToken().type == TokenType::KW_IF) {
        elseBody.push_back(parseStatement());
      } else {
        expect(TokenType::LBRACE, "Expected '{' after else");
        while (!match(TokenType::RBRACE) &&
               currentToken().type != TokenType::EOF_TOK) {
          elseBody.push_back(parseStatement());
        }
      }
    }

    auto node =
        std::make_unique<IfNode>(std::move(condition), std::move(thenBody));
    node->elseBody = std::move(elseBody);
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (match(TokenType::KW_WHILE)) {
    expect(TokenType::LPAREN, "Expected '(' despues de 'while'");
    auto condition = parseExpression();
    expect(TokenType::RPAREN, "Expected ')' despues de la condicion");

    expect(TokenType::LBRACE, "Expected '{'");
    std::vector<std::unique_ptr<ASTNode>> body;
    while (!match(TokenType::RBRACE) &&
           currentToken().type != TokenType::EOF_TOK) {
      body.push_back(parseStatement());
    }

    auto node =
        std::make_unique<WhileNode>(std::move(condition), std::move(body));
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (match(TokenType::KW_FOR)) {
    expect(TokenType::LPAREN, "Expected '(' despues de 'for'");

    std::unique_ptr<ASTNode> init = nullptr;
    if (!match(TokenType::SEMICOLON)) {
      init = parseStatement();
    }

    std::unique_ptr<ExprNode> cond = nullptr;
    if (currentToken().type != TokenType::SEMICOLON) {
      cond = parseExpression();
    }
    expect(TokenType::SEMICOLON,
           "Expected ';' despues de la condicion del for");

    std::unique_ptr<ASTNode> update = nullptr;
    if (currentToken().type != TokenType::RPAREN) {
      auto expr = parseExpression();
      if (match(TokenType::PLUS_PLUS)) {
        update = std::make_unique<AssignNode>(
            std::move(expr), std::make_unique<NumberNode>(1), "+=");
      } else if (match(TokenType::MINUS_MINUS)) {
        update = std::make_unique<AssignNode>(
            std::move(expr), std::make_unique<NumberNode>(1), "-=");
      } else if (currentToken().type == TokenType::ASSIGN ||
                 currentToken().type == TokenType::PLUS_EQ ||
                 currentToken().type == TokenType::MINUS_EQ ||
                 currentToken().type == TokenType::STAR_EQ ||
                 currentToken().type == TokenType::SLASH_EQ) {
        std::string op = currentToken().value;
        advance();
        auto val = parseExpression();
        update =
            std::make_unique<AssignNode>(std::move(expr), std::move(val), op);
      } else {
        update = std::move(expr);
      }
    }
    expect(TokenType::RPAREN,
           "Expected ')' despues de la actualizacion del for");

    expect(TokenType::LBRACE, "Expected '{'");
    std::vector<std::unique_ptr<ASTNode>> body;
    while (!match(TokenType::RBRACE) &&
           currentToken().type != TokenType::EOF_TOK) {
      body.push_back(parseStatement());
    }

    auto node = std::make_unique<ForNode>(std::move(init), std::move(cond),
                                          std::move(update), std::move(body));
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (match(TokenType::KW_BREAK)) {
    auto node = std::make_unique<BreakNode>();
    expect(TokenType::SEMICOLON, "Expected ';'");
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (match(TokenType::KW_CONTINUE)) {
    auto node = std::make_unique<ContinueNode>();
    expect(TokenType::SEMICOLON, "Expected ';'");
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (isVarDeclaration()) {
    DeclPreamble preamble = parsePreamble();
    std::string typeName = parseTypeName();
    std::string varName = currentToken().value;
    expect(TokenType::IDENTIFIER, "Expected the variable name");

    std::unique_ptr<ExprNode> init = nullptr;

    auto node =
        std::make_unique<VarDeclNode>(typeName, varName, preamble.isConst,
                                      preamble.isStatic, std::move(init));

    std::unique_ptr<ExprNode> arrSize = nullptr;
    while (match(TokenType::LBRACKET)) {
      node->arraySizes.push_back(parseExpression());
      expect(TokenType::RBRACKET, "Expected ']' after array size");
    }

    if (match(TokenType::ASSIGN)) {
      // Inject the expression into the live AST.
      // Writing to the dead local variable spawned phantom nulls.
      node->initializer = parseExpression();
    } else if (match(TokenType::LPAREN)) {
      /*
       * STACK ALLOCATION SYNTAX SUGAR
       * Intercepts "SmartArray arr(5);" and transparently desugars it to
       * "SmartArray arr = SmartArray(5);" by forcing a CallNode into the
       * initializer.
       */
      std::vector<std::unique_ptr<ExprNode>> args;
      while (currentToken().type != TokenType::RPAREN &&
             currentToken().type != TokenType::EOF_TOK) {
        args.push_back(parseExpression());
        if (currentToken().type == TokenType::COMMA)
          advance();
      }
      expect(TokenType::RPAREN, "Expected ')'");
      node->initializer = std::make_unique<CallNode>(typeName, std::move(args));
    }

    node->decorators = preamble.decorators;
    expect(TokenType::SEMICOLON, "Expected ';'");
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (match(TokenType::KW_DELETE)) {
    bool isArr = false;
    if (match(TokenType::LBRACKET)) {
      expect(TokenType::RBRACKET, "Expected ']' for array delete");
      isArr = true;
    }
    auto ptrExpr = parseExpression();
    auto node = std::make_unique<DeleteNode>(std::move(ptrExpr), isArr);
    expect(TokenType::SEMICOLON, "Expected ';'");
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (match(TokenType::KW_RETURN)) {
    std::unique_ptr<ExprNode> expr = nullptr;
    if (currentToken().type != TokenType::SEMICOLON) {
      expr = parseExpression();
    }
    auto node = std::make_unique<ReturnNode>(std::move(expr));
    expect(TokenType::SEMICOLON, "Expected ';'");
    finalizeNode(node.get(), startTok);
    return node;
  }

  auto expr = parseExpression();
  if (match(TokenType::PLUS_PLUS)) {
    auto node = std::make_unique<AssignNode>(
        std::move(expr), std::make_unique<NumberNode>(1), "+=");
    expect(TokenType::SEMICOLON, "Expected ';'");
    finalizeNode(node.get(), startTok);
    return node;
  }
  if (match(TokenType::MINUS_MINUS)) {
    auto node = std::make_unique<AssignNode>(
        std::move(expr), std::make_unique<NumberNode>(1), "-=");
    expect(TokenType::SEMICOLON, "Expected ';'");
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (currentToken().type == TokenType::ASSIGN ||
      currentToken().type == TokenType::PLUS_EQ ||
      currentToken().type == TokenType::MINUS_EQ ||
      currentToken().type == TokenType::STAR_EQ ||
      currentToken().type == TokenType::SLASH_EQ) {
    std::string op = currentToken().value;
    advance();
    auto valueExpr = parseExpression();
    expect(TokenType::SEMICOLON, "Expected ';'");

    auto node =
        std::make_unique<AssignNode>(std::move(expr), std::move(valueExpr), op);
    finalizeNode(node.get(), startTok);
    return node;
  }

  expect(TokenType::SEMICOLON, "Expected ';'");
  finalizeNode(expr.get(), startTok);
  return expr;
}

} // namespace utopia