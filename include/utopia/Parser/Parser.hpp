#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include <memory>
#include <vector>

namespace utopia {

struct DeclPreamble {
  AccessModifier access = AccessModifier::Implicit;
  InlineState inlineState = InlineState::None;
  std::vector<std::string> decorators;
  bool isConst = false;
};

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
  bool isVarDeclaration() const;
  std::string consumeType();
  std::string parseTypeName();

  void parseImport();
  std::unique_ptr<FunctionNode> parseFunction();
  std::unique_ptr<FunctionNode> parseMethod(const std::string &className);
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
  std::unique_ptr<StructDeclNode> parseStructDecl(bool isClass);
  std::unique_ptr<ExprNode> parsePrimaryBase();
  DeclPreamble parsePreamble();
  std::unique_ptr<StructDeclNode> parseStructDecl(bool isClass,
                                                  const DeclPreamble &preamble);
  std::unique_ptr<FunctionNode> parseFunction(const DeclPreamble &preamble);
  std::unique_ptr<FunctionNode> parseMethod(const std::string &className,
                                            const DeclPreamble &preamble);

  void finalizeNode(ASTNode *node, const Token &startToken);
};

} // namespace utopia