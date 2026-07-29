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

class ModuleLoader;

class Parser {
public:
  Parser(ASTContext &context, llvm::ArrayRef<Token> tokenStream,
         DiagnosticsEngine &de, std::string_view path,
         ModuleLoader *loader = nullptr)
      : astCtx(context), tokens(tokenStream), diags(de), filePath(path),
        moduleLoader(loader) {}

  ModuleNode *parseModule(std::string_view filePath);

  DeclNode *parseClassDecl();
  DeclNode *
  parseDeclarationOrFunction(llvm::ArrayRef<AnnotationNode *> annotations = {});

  std::string_view instantiatingName = "";
  std::string_view templateBaseName = "";
  std::unordered_map<std::string_view, const Type *> templateArgs;

private:
  ASTContext &astCtx;
  llvm::ArrayRef<Token> tokens;
  size_t cursor = 0;

  DiagnosticsEngine &diags;
  std::string_view filePath;
  ModuleLoader *moduleLoader;

  std::vector<std::string_view> currentTemplateParams;

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

  void pushTemplateParam(std::string_view name) {
    currentTemplateParams.push_back(name);
  }
  void popTemplateParams(size_t count) {
    currentTemplateParams.resize(currentTemplateParams.size() - count);
  }
  bool isTemplateParam(std::string_view name) {
    return std::find(currentTemplateParams.begin(), currentTemplateParams.end(),
                     name) != currentTemplateParams.end();
  }

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

  DeclNode *parseEnumDecl();
  DeclNode *parseStructDecl();
  BlockNode *parseBlock();
  BlockNode *parseFunctionBody(const Type *returnType);
  ReturnNode *parseReturn();
  ExprNode *parseExpressionStatement();
  ForNode *parseForStatement();
  WhileNode *parseWhileStatement();

  ExprNode *parseExpression();
  ExprNode *parseAssignment();
  ExprNode *parseLogicalOr();
  ExprNode *parseLogicalAnd();
  ExprNode *parseBitwiseOr();
  ExprNode *parseBitwiseXor();
  ExprNode *parseBitwiseAnd();
  ExprNode *parseEquality();
  ExprNode *parseRelational();
  ExprNode *parseShift();
  ExprNode *parseAdditive();
  ExprNode *parseTerm();
  ExprNode *parseCast();
  ExprNode *parseUnary();
  ExprNode *parsePostfix();
  ExprNode *parsePrimary();
};

} // namespace utopia