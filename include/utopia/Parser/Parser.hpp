#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/AST/ASTContext.hpp"
#include "utopia/Common/Diagnostics.hpp"
#include "utopia/Common/Types.hpp"
#include "utopia/Lexer/Token.hpp"
#include <exception>
#include <span>

namespace utopia {

class ParseException : public std::exception {
public:
  const char *what() const noexcept override {
    return "Utopia Parse Exception";
  }
};

class Parser {
public:
  Parser(ASTContext &context, std::span<const Token> tokenStream,
         DiagnosticsEngine &de, std::string_view path)
      : astCtx(context), tokens(tokenStream), diags(de), filePath(path) {}

  ModuleNode *parseModule(std::string_view filePath);

private:
  ASTContext &astCtx;
  std::span<const Token> tokens;
  size_t cursor = 0;

  DiagnosticsEngine &diags;
  std::string_view filePath;

  const Token &currentToken() const;
  const Token &peekToken(size_t offset = 1) const;
  void advance();
  bool match(TokenType type);
  void expect(TokenType type, std::string_view errorMessage);

  void report(DiagLevel level, int line, int col, int len,
              std::string_view msg) {
    diags.report(
        {level, line, col, len, std::string(msg), std::string(filePath)});
  }
  void reportError(int line, int col, int len, std::string_view message) {
    report(DiagLevel::Error, line, col, len, message);
  }
  void synchronize();

  const Type *parseType(bool inNewExpr = false);
  const Type *applyArrayDeclarator(const Type *baseType);
  std::string consumeComments();
  ExprNode *parseArrayLiteral();
  ASTNode *parseStatement();
  IfNode *parseIfStatement();
  llvm::ArrayRef<AnnotationNode *> parseAnnotations();
  AnnotationNode *parseAnnotation();
  DeclNode *parseAnnotationDecl(llvm::ArrayRef<AnnotationNode *> annotations);
  DeclNode *parseTypedefDecl();
  std::vector<ParamDeclNode *> parseParameterList(const Type *classTy,
                                                  bool &isVariadic);

  DeclNode *parseDeclarationOrFunction();
  DeclNode *parseStructDecl();
  DeclNode *parseClassDecl();
  BlockNode *parseBlock();
  BlockNode *parseFunctionBody(const Type* returnType);
  ReturnNode *parseReturn();
  ExprNode *parseExpressionStatement();
  ForNode *parseForStatement();
  WhileNode *parseWhileStatement();

  ExprNode *parseExpression();
  ExprNode *parseAssignment();
  ExprNode *parseLogicalOr();
  ExprNode *parseLogicalAnd();
  ExprNode *parseEquality();
  ExprNode *parseRelational();
  ExprNode *parseAdditive();
  ExprNode *parseTerm();
  ExprNode *parseCast();
  ExprNode *parseUnary();
  ExprNode *parsePostfix();
  ExprNode *parsePrimary();
};

} // namespace utopia