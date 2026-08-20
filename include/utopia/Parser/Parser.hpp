#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/AST/ASTContext.hpp"
#include "utopia/Common/Diagnostics.hpp"
#include "utopia/Common/Types.hpp"
#include "utopia/Lexer/Token.hpp"
#include <exception>

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
         ModuleLoader *loader = nullptr, bool asyncEnabled = true)
      : astCtx(context), tokens(tokenStream), diags(de), filePath(path),
        moduleLoader(loader), asyncEnabled(asyncEnabled) {}

  ModuleNode *parseModule(std::string_view filePath);

  DeclNode *
  parseDeclarationOrFunction(llvm::ArrayRef<AnnotationNode *> annotations = {});
  std::vector<ParamDeclNode *> parseParameterList(bool &isVariadic,
                                                  bool &hasTrailingComma,
                                                  bool allowUntypedParams = false);

private:
  ASTContext &astCtx;
  llvm::ArrayRef<Token> tokens;
  size_t cursor = 0;

  DiagnosticsEngine &diags;
  std::string_view filePath;
  ModuleLoader *moduleLoader;
  bool asyncEnabled;

  std::vector<std::string_view> currentTemplateParams;
  std::vector<std::string> namespaceStack;
  std::vector<std::string> activeUsings;

  std::string getCurrentNamespace() const;
  std::string getFQName(std::string_view name) const;

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

  void checkRecordMemberRedefinition(
      std::string_view name, const std::vector<VarDeclNode *> &fields,
      const std::vector<FunctionDeclNode *> &methods,
      const FunctionDeclNode *newMethod, int line, int col, int len);

  void checkConstructorRedefinition(
      const std::vector<FunctionDeclNode *> &constructors,
      const FunctionDeclNode *newCtor, int line, int col, int len);

  std::string_view parseOperatorName();

  const Type *parseType(bool inNewExpr = false,
                        bool allowRValueRef = true);
  const Type *applyArrayDeclarator(const Type *baseType);
  const Type *parseTypeModifiers(const Type *baseType, bool inNewExpr,
                                 bool allowRValueRef = true);

  std::string consumeComments();
  ExprNode *parseArrayLiteral();
  ExprNode *parseMapLiteral();
  NamespaceDeclNode *parseNamespaceDecl(bool &isFileScoped);
  UsingNode *parseUsing();
  bool isDeclaration();
  ASTNode *parseStatement();
  IfNode *parseIfStatement();
  llvm::ArrayRef<AnnotationNode *> parseAnnotations();
  AnnotationNode *parseAnnotation();
  DeclNode *parseAnnotationDecl(llvm::ArrayRef<AnnotationNode *> annotations);
  DeclNode *parseTypedefDecl();

  DeclNode *parseRecordDecl(TypeKind kind, bool isAbstract = false,
                            bool isFinal = false);
  DeclNode *parseEnumDecl();
  BlockNode *parseBlock();
  BlockNode *parseStatementAsBlock();
  BlockNode *parseFunctionBody(const Type *returnType);
  ReturnNode *parseReturn();
  ExprNode *parseExpressionStatement();
  ForNode *parseForStatement();
  WhileNode *parseWhileStatement();
  SwitchNode *parseSwitchStatement();
  BreakNode *parseBreakStatement();
  ContinueNode *parseContinueStatement();

  ExprNode *parseExpression();
  ExprNode *parseAssignment();
  ExprNode *parseTernary();
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
  ExprNode *parseLambda(const Type *explicitReturnType);
  std::vector<ParamDeclNode *> parseLambdaParams();
  bool looksLikeLambdaParams(size_t openOffset) const;
  bool lambdaFollowedByBody(size_t openOffset) const;
};

} // namespace utopia