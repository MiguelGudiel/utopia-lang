#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include <memory>
#include <vector>

namespace utopia {

class Parser {
public:
  explicit Parser(const std::vector<Token> &tokens);
  std::unique_ptr<ProgramNode> parseProgram();
  bool isFunctionStart() const;

private:
  const std::vector<Token> &tokens;
  size_t cursor = 0;

  const Token &currentToken() const;
  const Token &peekNextToken() const;
  void advance();
  bool match(TokenType type);
  void expect(TokenType type, const std::string &errorMessage);

  bool isTypeToken() const;
  std::string consumeType();
  std::string parseTypeName();

  void parseImport();
  std::unique_ptr<FunctionNode> parseFunction();
  std::unique_ptr<ASTNode> parseStatement();

  // Expression Parsing (Ascending Precedence)
  std::unique_ptr<ExprNode> parseExpression();
  std::unique_ptr<ExprNode> parseLogicalOr();
  std::unique_ptr<ExprNode> parseLogicalAnd();
  std::unique_ptr<ExprNode> parseEquality();
  std::unique_ptr<ExprNode> parseRelational();
  std::unique_ptr<ExprNode> parseAdditive();
  std::unique_ptr<ExprNode> parseTerm();
  std::unique_ptr<ExprNode> parsePrimary();

  void finalizeNode(ASTNode *node, const Token &startToken);
};

} // namespace utopia