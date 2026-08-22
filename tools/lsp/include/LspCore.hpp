#pragma once
/* Shared declarations for the Utopia language server. Each feature handler
 * lives in its own translation unit (Hover, Definition, Completion, ...) and
 * depends only on this core plus SearchVisitor. */

#include "SearchVisitor.hpp"
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Format/Formatter.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include "utopia/Parser/Parser.hpp"
#include "utopia/Sema/Sema.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace utopia::lsp {

using json = nlohmann::json;

/* Protocol helpers */

/* Returns the request id when present, null otherwise. Handlers must never
 * touch req["id"] directly: a malformed request would otherwise abort the
 * whole server through nlohmann's assertion. */
json requestId(const json &req);

/* Serializes a response with the LSP Content-Length framing. Safe to call
 * from any thread. */
void sendResponse(const json &res);

std::string pathToUri(std::string_view path);
std::string uriToPath(const std::string &uri);

/* Document state and per-project configuration */

struct DocumentState {
  std::string text;
  std::shared_ptr<ASTContext> astCtx;
  ModuleNode *ast = nullptr;
  std::shared_ptr<DiagnosticsEngine> diags;
  std::shared_ptr<SemaContext> sema;
};

/* Owns the open documents and the per-project module-loading configuration
 * caches. Handlers read documents through get(); the analysis worker writes
 * through processFile(). All methods are thread-safe. */
class DocumentManager {
public:
  /* Analysis of a document text (called on the worker thread): rebuilds the
   * AST + Sema state and publishes diagnostics for the document. */
  void processFile(const std::string &uri, std::string text);

  /* Latest analyzed state for a document URI. Returns false when the
   * document is unknown. */
  bool get(const std::string &uri, DocumentState &out) const;

  /* Text of a document URI, or of a plain filesystem path when the file is
   * not open (used to read declarations in other files). */
  std::string textFor(const std::string &uri) const;

  /* Copy of every analyzed document (URI -> state). */
  std::vector<std::pair<std::string, DocumentState>> snapshot() const;

  /* ModuleLoaderConfig for the project that owns 'filePath'; the config is
   * computed once per project root and cached. */
  ModuleLoaderConfig configFor(const std::string &uri,
                               const std::filesystem::path &filePath);

  /* The editor applied an edit to a build.yaml document (didOpen/didChange):
   * the in-memory text overrides the file on disk until the document is
   * closed, and every open document of the affected project is re-analyzed
   * so the warning configuration takes effect immediately. */
  void onBuildManifestChanged(const std::string &uri, const std::string &text);

  /* A build.yaml changed on disk (workspace/didChangeWatchedFiles or
   * didClose): drop caches and re-analyze the affected project. */
  void onBuildManifestSaved(const std::string &uri);

  /* The editor closed a build.yaml document: revert to the on-disk
   * manifest for the project. */
  void onBuildManifestClosed(const std::string &uri);

  /* Called periodically by the analysis worker: stats the build.yaml of
   * every project known to the server and refreshes those whose manifest
   * changed on disk. This keeps the project configuration (warnings,
   * packages, dependencies) in sync even when the client sends no
   * notifications at all. */
  void pollManifests();

private:
  std::filesystem::path projectRootFor(const std::string &uri,
                                       const std::filesystem::path &filePath);
  void loadPackages(const std::filesystem::path &manifestPath,
                    std::unordered_map<std::string, std::string> &packages,
                    std::vector<std::string> &includeDirs,
                    std::unordered_set<std::string> &visited);
  void applyAsyncConfig(ModuleLoaderConfig &modConfig,
                        const std::filesystem::path &projRoot);
  std::filesystem::path findProjectRoot(const std::filesystem::path &current) const;

  /* Text of a project's build.yaml: the editor's in-memory version when the
   * manifest is open, the file on disk otherwise. */
  std::string manifestTextFor(const std::filesystem::path &projRoot) const;

  /* Invalidates the cached configuration of a project and re-analyzes every
   * open document that belongs to it. */
  void refreshProject(const std::string &projRoot);

  /* Re-analyzes every open document whose project root matches. */
  void reanalyzeProject(const std::string &projRoot);

  mutable std::shared_mutex docMutex;
  std::map<std::string, DocumentState> documents;

  mutable std::mutex cacheMutex;
  std::map<std::string, ModuleLoaderConfig> projectConfigCache;
  std::map<std::string, std::filesystem::file_time_type> configMtimes;
  std::map<std::string, std::filesystem::path> uriToProjectRoot;
  /* Project root -> in-memory text of open build.yaml documents. */
  std::map<std::string, std::string> manifestTexts;
  /* Project root -> last seen on-disk mtime of build.yaml (pollManifests). */
  std::map<std::string, std::filesystem::file_time_type> manifestDiskMtimes;
};

/* The analysis worker and its synchronization (defined in Server.cpp). */
void requestBackgroundAnalysis(const std::string &uri, std::string text);
void syncWorker();
void stopWorker();

/* Blocks reading LSP messages from stdin until 'exit' (defined in
 * Server.cpp). */
void runServer();

/* Single server-wide document store (defined in Server.cpp). */
extern DocumentManager documents;

/* Local variable collection for a cursor position */

/* Walks the module from the top, collecting every declaration whose line is
 * at or before the target line. Functions enclosing the line are descended
 * into so their parameters and locals are visible. */
class LocalVarCollector : public ASTVisitor<LocalVarCollector, void> {
public:
  int targetLine;
  std::vector<const VarDeclNode *> locals;
  std::vector<std::string> activeUsings;
  std::string currentNamespace;
  const FunctionDeclNode *closestFunc = nullptr;

  explicit LocalVarCollector(int line) : targetLine(line) {}

  void visit(const ModuleNode *n);
  void visit(const NamespaceDeclNode *n);
  void visit(const UsingNode *n);
  void visit(const FunctionDeclNode *n);
  void visit(const ParamDeclNode *n) {}
  void visit(const VarDeclNode *n);
  void visit(const BlockNode *n);
  void visit(const IfNode *n);
  void visit(const ForNode *n);
  void visit(const ForInNode *n);
  void visit(const WhileNode *n);
  void visit(const SwitchNode *n);
  void visit(const CaseNode *n);
  void visit(const TryStmtNode *n);
  void visit(const ThrowStmtNode *n);
  void visit(const AssertStmtNode *n);
  void visit(const ConstExprNode *n);
  void visit(const AssignNode *n);
  void visit(const NumberNode *) {}
  void visit(const BoolNode *) {}
  void visit(const CharNode *) {}
  void visit(const RuneNode *) {}
  void visit(const StringNode *) {}
  void visit(const NullNode *) {}
  void visit(const TypeLiteralNode *) {}
  void visit(const VariableNode *) {}
  void visit(const UnaryOpNode *) {}
  void visit(const AwaitExprNode *node) { dispatch(node->expr); }
  void visit(const BinaryOpNode *) {}
  void visit(const TernaryOpNode *) {}
  void visit(const ArrayLiteralNode *) {}
  void visit(const MapLiteralNode *) {}
  void visit(const ArraySubscriptNode *) {}
  void visit(const MemberAccessNode *) {}
  void visit(const FunctionCallNode *) {}
  void visit(const LambdaNode *n);
  void visit(const CastNode *) {}
  void visit(const IsExprNode *) {}
  void visit(const NewExprNode *) {}
  void visit(const DeleteExprNode *) {}
  void visit(const DestructorCallNode *) {}
  void visit(const ImplicitCastNode *) {}
  void visit(const ReturnNode *) {}
  void visit(const BreakNode *) {}
  void visit(const ContinueNode *) {}
  void visit(const StructDeclNode *n);
  void visit(const ClassDeclNode *n);
  void visit(const UnionDeclNode *n);
  void visit(const EnumDeclNode *) {}
  void visit(const EnumMemberNode *) {}
  void visit(const AnnotationDeclNode *) {}
  void visit(const TypedefDeclNode *) {}
  void visit(const AnnotationNode *) {}
};

/* Shared AST helpers */

struct SourceLocation {
  int line;
  int col;
  int length;
};

/* Locates the identifier of a declaration inside its source text. Declarations
 * recorded by the parser carry exact identifier positions; the fallback
 * searches for the name near the declaration start. */
SourceLocation getExactNameLocation(const std::string &text,
                                    const DeclNode *decl);

/* The declaration of the record/enum/alias behind a type, unwrapping pointer,
 * reference, const and array wrappers. */
const DeclNode *getTypeDeclaration(const Type *ty);

/* Resolves an import/export URI ('utopia:*', 'package:*', 'prelude' or a
 * relative path) to an absolute file path, mirroring the compiler's
 * ModuleLoader::resolveImportURI. Returns "" on failure. */
std::string resolveModuleUriLsp(const ModuleLoaderConfig &config,
                                const std::string &uri,
                                const std::filesystem::path &currentDir);

/* Resolves an identifier (possibly dotted) visible from the collector's
 * position: locals are checked first, then the current namespace, then the
 * active usings, then the module root. */
const DeclNode *resolveWithCollector(const std::string &name,
                                     const LocalVarCollector &collector,
                                     SemaContext *sema,
                                     const ModuleNode *root);

std::string getHoverTextForDecl(const DeclNode *decl);
std::string formatFunctionSignature(const FunctionDeclNode *func);
std::vector<const FunctionDeclNode *> getOverloads(const FunctionDeclNode *func,
                                                   SemaContext *sema);
std::string buildFunctionHover(const FunctionDeclNode *targetFunc);

/* For smart-pointer records, follows the 'operator*' chain to the pointee
 * record (mirrors the compiler's auto-deref member resolution). */
const DeclNode *getAutoDerefTarget(const DeclNode *recDecl);

/* Module-wide symbol index (for completion, symbols, references) */

struct GlobalSymbols {
  std::unordered_map<std::string, std::vector<const DeclNode *>>
      namespaceMembers;
  std::vector<const DeclNode *> rootGlobals;
};

/* Collects every top-level declaration reachable from the module (imports and
 * exports included), indexed by namespace. */
GlobalSymbols collectGlobals(const ModuleNode *root);

/* Access control */

/* Whether 'member' (a field/method of 'memberRecord') may be referenced from
 * code inside 'accessContext' (the record of the enclosing function, or null
 * for global scope). Private members are only reachable within their own
 * record; protected members additionally from derived classes. Members
 * without an explicit modifier are private when their name starts with '_'. */
bool isMemberVisible(const DeclNode *member, const RecordType *memberRecord,
                     const RecordType *accessContext);

/* Feature handlers */

void handleHover(const json &req);
void handleDefinition(const json &req);
void handleTypeDefinition(const json &req);
void handleImplementation(const json &req);
void handleCompletion(const json &req);
void handleSignatureHelp(const json &req);
void handleFormatting(const json &req);
void handleSemanticTokens(const json &req);
void handleDocumentSymbols(const json &req);
void handleWorkspaceSymbols(const json &req);
void handleReferences(const json &req);
void handleDocumentHighlight(const json &req);
void handleFoldingRange(const json &req);
void handleDocumentLinks(const json &req);
void handleCodeAction(const json &req);

} // namespace utopia::lsp
