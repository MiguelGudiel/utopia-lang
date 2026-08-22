#pragma once
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Sema/Sema.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace utopia {

/* Detects declarations that are never referenced within their own module
 * tree: unused imports, private functions/methods/records never called,
 * unused local variables, unused private fields/globals, unused parameters
 * and unused using directives. Runs after the type checker so every
 * reference is already resolved (resolvedDecl / resolvedFunc / ...). */
class UnusedCodePass : public SemaPass,
                       public ASTVisitor<UnusedCodePass, void> {
  SemaContext *ctx = nullptr;
  std::unordered_set<const ModuleNode *> visitedModules;

  /* Files already reported across driver runs; guards against duplicate
   * warnings when several roots reach the same module. */
  std::unordered_set<std::string> reportedFiles;

  /* Declarations referenced anywhere in the analyzed tree. */
  std::unordered_set<const DeclNode *> usedDecls;

  /* Template bodies are never type-checked (only their instantiations are),
   * so references inside them are unresolved: candidates must not be
   * collected from template contexts. The same applies to instantiated
   * clones (in instantiatedTemplates), which are used by construction. */
  int templateDepth = 0;
  bool collectingInstantiated = false;
  bool suppressCandidates() const {
    return templateDepth > 0 || collectingInstantiated;
  }

  /* Files where each reference occurs: keyed by the path of the file that
   * contains the reference. Used to decide whether an import directive is
   * needed by its own file. */
  std::unordered_map<std::string, std::unordered_set<std::string>>
      usedFilesByFile;
  /* Fully qualified names of used declarations per referencing file (for
   * using directives). */
  std::unordered_map<std::string, std::unordered_set<std::string>>
      usedFqNamesByFile;

  /* Candidates (file-private, reported per file) */

  struct DeclCandidate {
    const DeclNode *decl;
    std::string file;
    const char *kindName; /* "function", "method", "record", "variable" */
  };
  std::vector<DeclCandidate> declCandidates;

  struct FieldCandidate {
    const VarDeclNode *field;
    std::string file;
  };
  std::vector<FieldCandidate> fieldCandidates;

  struct ParamCandidate {
    const ParamDeclNode *param;
    std::string file;
  };
  std::vector<ParamCandidate> paramCandidates;

  struct UsingCandidate {
    std::string ns;
    std::string file;
    int line;
    int column;
    int length;
  };
  std::vector<UsingCandidate> usingCandidates;

  struct ImportCandidate {
    std::string path;
    std::string file;
    int line;
    int column;
    int endLine;
    int endColumn;
    const ModuleNode *resolved = nullptr;
  };
  std::vector<ImportCandidate> importCandidates;

  /* Transitive set of module file paths reachable through an import (the
   * prelude excluded: it is implicitly injected into every module). */
  std::unordered_map<const ModuleNode *, std::unordered_set<std::string>>
      moduleFileCache;

  void markUsed(const DeclNode *decl);
  void recordTypeUsage(const Type *type);
  void collectModuleFiles(const ModuleNode *module,
                          std::unordered_set<std::string> &files,
                          std::unordered_set<const ModuleNode *> &visited);
  static bool isPreludeModule(const ModuleNode *module);
  const DeclNode *resolveRecordType(const Type *type);

  bool isPrivateFunc(const FunctionDeclNode *fn) const;
  bool isPrivateMethod(const FunctionDeclNode *fn) const;

  /* Registers a parameter of a function body as an unused-parameter
  * candidate (subject to the '_' / 'this' exemptions). */
  void addParamCandidate(const ParamDeclNode *param);

  /* The shared body traversal of a function-like declaration. */
  void dispatchFunctionBody(const FunctionDeclNode *fn);

  /* Shared handling of record declarations: fields, methods, constructors,
   * destructor and the record itself as an unused-type candidate. */
  void visitRecord(const DeclNode *node, llvm::ArrayRef<VarDeclNode *> fields,
                   llvm::ArrayRef<FunctionDeclNode *> methods,
                   llvm::ArrayRef<FunctionDeclNode *> constructors,
                   FunctionDeclNode *destructor);

  /* Emits the warnings for one fully visited module file. */
  void reportFile(const ModuleNode *node, const std::string &filePath);

public:
  bool run(const ModuleNode *module, SemaContext &context) override;
  const char *getName() const override { return "UnusedCode"; }

  void visit(const ModuleNode *node);
  void visit(const FunctionDeclNode *node);
  void visit(const VarDeclNode *node);
  void visit(const ParamDeclNode *node);
  void visit(const UsingNode *node);
  void visit(const NamespaceDeclNode *node);
  void visit(const StructDeclNode *node);
  void visit(const ClassDeclNode *node);
  void visit(const UnionDeclNode *node);
  void visit(const EnumDeclNode *node);
  void visit(const TypedefDeclNode *node);
  void visit(const LambdaNode *node);
  void visit(const BlockNode *node);
  void visit(const IfNode *node);
  void visit(const ForNode *node);
  void visit(const ForInNode *node);
  void visit(const WhileNode *node);
  void visit(const SwitchNode *node);
  void visit(const CaseNode *node);
  void visit(const TryStmtNode *node);
  void visit(const ThrowStmtNode *node);
  void visit(const AssertStmtNode *node);
  void visit(const ReturnNode *node);
  void visit(const FunctionCallNode *node);
  void visit(const VariableNode *node);
  void visit(const MemberAccessNode *node);
  void visit(const AssignNode *node);
  void visit(const UnaryOpNode *node);
  void visit(const BinaryOpNode *node);
  void visit(const TernaryOpNode *node);
  void visit(const CastNode *node);
  void visit(const IsExprNode *node);
  void visit(const NewExprNode *node);
  void visit(const DeleteExprNode *node);
  void visit(const ConstExprNode *node);
  void visit(const DestructorCallNode *node);
  void visit(const ArraySubscriptNode *node);
  void visit(const ArrayLiteralNode *node);
  void visit(const MapLiteralNode *node);
  void visit(const AwaitExprNode *node);
  void visit(const ImplicitCastNode *node);
  void visit(const TypeLiteralNode *node);
  void visit(const AnnotationNode *node) {}
  void visit(const AnnotationDeclNode *node) {}
  void visit(const NumberNode *) {}
  void visit(const BoolNode *) {}
  void visit(const CharNode *) {}
  void visit(const RuneNode *) {}
  void visit(const StringNode *) {}
  void visit(const NullNode *) {}
  void visit(const BreakNode *) {}
  void visit(const ContinueNode *) {}
  void visit(const EnumMemberNode *node) {}
};

} // namespace utopia
