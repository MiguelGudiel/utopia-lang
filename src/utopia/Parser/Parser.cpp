#include "utopia/Parser/Parser.hpp"
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
         t == TokenType::KW_STRING_TYPE || t == TokenType::KW_BOOL ||
         t == TokenType::KW_UINT || t == TokenType::KW_VOID;
}

std::string Parser::consumeType() {
  if (!isTypeToken()) {
    throw std::runtime_error("Syntax Error: Expected a data type.");
  }
  std::string typeName = currentToken().value;
  advance();
  return typeName;
}

std::string Parser::parseTypeName() {
  std::string typeName = consumeType();
  while (match(TokenType::STAR))
    typeName += "*";
  if (match(TokenType::QUESTION))
    typeName += "?";
  return typeName;
}

bool Parser::isFunctionStart() const {
  TokenType t = currentToken().type;
  // A function begins with a type or a modifier
  return isTypeToken() || t == TokenType::KW_INLINE ||
         t == TokenType::KW_FORCE_INLINE;
}

std::unique_ptr<ProgramNode> Parser::parseProgram() {
  auto program = std::make_unique<ProgramNode>();
  while (currentToken().type != TokenType::EOF_TOK) {
    if (currentToken().type == TokenType::KW_IMPORT) {
      parseImport();
    } else if (isFunctionStart()) {
      program->functions.push_back(parseFunction());
    } else {
      const Token &stray = currentToken();
      throw std::runtime_error(
          std::to_string(stray.line) + ":" + std::to_string(stray.column) +
          "|Syntax Error: Unexpected token '" + stray.value + "'");
    }
  }
  return program;
}

void Parser::parseImport() {
  expect(TokenType::KW_IMPORT, "Expected 'import'");
  expect(TokenType::STRING, "Expected cadena");
  expect(TokenType::SEMICOLON, "Expected ';'");
}

std::unique_ptr<FunctionNode> Parser::parseFunction() {
  Token startTok = currentToken();

  InlineState inlineState = InlineState::None;
  if (match(TokenType::KW_INLINE)) {
    inlineState = InlineState::Inline;
  } else if (match(TokenType::KW_FORCE_INLINE)) {
    inlineState = InlineState::ForceInline;
  }

  std::string retType = parseTypeName();
  std::string funcName = currentToken().value;
  expect(TokenType::IDENTIFIER, "Expected el nombre");
  expect(TokenType::LPAREN, "Expected '('");

  std::vector<std::pair<std::string, std::string>> args;
  while (currentToken().type != TokenType::RPAREN) {
    std::string argType = parseTypeName();
    std::string argName = currentToken().value;
    expect(TokenType::IDENTIFIER, "Expected nombre del arg");
    args.push_back({argType, argName});
    if (currentToken().type == TokenType::COMMA)
      advance();
  }
  expect(TokenType::RPAREN, "Expected ')'");
  expect(TokenType::LBRACE, "Expected '{'");

  auto funcNode =
      std::make_unique<FunctionNode>(inlineState, retType, funcName, args);
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
  auto left = parsePrimary();
  while (currentToken().type == TokenType::STAR ||
         currentToken().type == TokenType::SLASH ||
         currentToken().type == TokenType::PERCENT) {
    std::string op = currentToken().value;
    advance();
    auto right = parsePrimary();
    left =
        std::make_unique<BinaryOpNode>(op, std::move(left), std::move(right));
    finalizeNode(left.get(), startTok);
  }
  return left;
}

std::unique_ptr<ExprNode> Parser::parsePrimary() {
  Token startTok = currentToken();

  if (match(TokenType::LPAREN)) {
    auto expr = parseExpression();
    expect(TokenType::RPAREN,
           "Expected ')' para cerrar la expresion agrupada");
    return expr;
  }

  if (match(TokenType::AMPERSAND))
    return std::make_unique<AddressOfNode>(parsePrimary());
  if (match(TokenType::STAR))
    return std::make_unique<DerefNode>(parsePrimary());
  if (match(TokenType::KW_NULL))
    return std::make_unique<NullLiteralNode>();

  if (match(TokenType::BANG)) {
    return std::make_unique<NullAssertNode>(parsePrimary());
  }

  if (match(TokenType::KW_NEW)) {
    std::string typeName = consumeType();
    std::unique_ptr<ExprNode> init = nullptr;
    if (match(TokenType::LPAREN)) {
      init = parseExpression();
      expect(TokenType::RPAREN, "Expected ')'");
    }
    return std::make_unique<NewNode>(typeName, std::move(init));
  }

  if (isTypeToken()) {
    std::string typeName = consumeType();
    expect(TokenType::LPAREN, "Expected '(' para cast");
    std::vector<std::unique_ptr<ExprNode>> args;
    args.push_back(parseExpression());
    expect(TokenType::RPAREN, "Expected ')'");
    auto node = std::make_unique<CallNode>(typeName, std::move(args));
    finalizeNode(node.get(), startTok);
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
        // Recursive support for 'else if'
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

  // WHILE
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

  // FOR
  if (match(TokenType::KW_FOR)) {
    expect(TokenType::LPAREN, "Expected '(' despues de 'for'");

    std::unique_ptr<ASTNode> init = nullptr;
    if (!match(TokenType::SEMICOLON)) {
      init = parseStatement();
    }

    // Condition (Ej: i < 10)
    std::unique_ptr<ExprNode> cond = nullptr;
    if (currentToken().type != TokenType::SEMICOLON) {
      cond = parseExpression();
    }
    expect(TokenType::SEMICOLON,
           "Expected ';' despues de la condicion del for");

    std::unique_ptr<ASTNode> update = nullptr;
    if (currentToken().type != TokenType::RPAREN) {
      auto expr = parseExpression();
      if (match(TokenType::ASSIGN)) {
        auto val = parseExpression();
        update = std::make_unique<AssignNode>(std::move(expr), std::move(val));
      } else {
        update = std::move(expr);
      }
    }
    expect(TokenType::RPAREN,
           "Expected ')' despues de la actualizacion del for");

    // for body
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

  bool isConst = match(TokenType::KW_CONST);
  if (isConst || isTypeToken()) {
    std::string typeName = parseTypeName();
    std::string varName = currentToken().value;
    expect(TokenType::IDENTIFIER, "Expected el nombre de la variable");

    std::unique_ptr<ExprNode> init = nullptr;
    if (match(TokenType::ASSIGN)) {
      init = parseExpression();
    } else {
      init = std::make_unique<NumberNode>(0);
    }
    auto node = std::make_unique<VarDeclNode>(typeName, varName, isConst,
                                              std::move(init));
    expect(TokenType::SEMICOLON, "Expected ';'");
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (match(TokenType::KW_DELETE)) {
    auto ptrExpr = parseExpression();
    auto node = std::make_unique<DeleteNode>(std::move(ptrExpr));
    expect(TokenType::SEMICOLON, "Expected ';'");
    finalizeNode(node.get(), startTok);
    return node;
  }

  if (match(TokenType::KW_RETURN)) {
    std::unique_ptr<ExprNode> expr = nullptr;
    // Support for early return in void functions: return;
    if (currentToken().type != TokenType::SEMICOLON) {
      expr = parseExpression();
    }
    auto node = std::make_unique<ReturnNode>(std::move(expr));
    expect(TokenType::SEMICOLON, "Expected ';'");
    finalizeNode(node.get(), startTok);
    return node;
  }

  auto expr = parseExpression();
  if (match(TokenType::ASSIGN)) {
    auto valueExpr = parseExpression();
    expect(TokenType::SEMICOLON, "Expected ';'");

    auto node =
        std::make_unique<AssignNode>(std::move(expr), std::move(valueExpr));
    finalizeNode(node.get(), startTok);
    return node;
  }

  expect(TokenType::SEMICOLON, "Expected ';'");
  finalizeNode(expr.get(), startTok);
  return expr;
}

} // namespace utopia