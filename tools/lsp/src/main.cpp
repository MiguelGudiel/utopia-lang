#include "SearchVisitor.hpp"
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Format/Formatter.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include "utopia/Parser/Parser.hpp"
#include "utopia/Sema/Sema.hpp"
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <thread>

using json = nlohmann::json;

namespace utopia::lsp {

struct DocumentState {
  std::string text;
  std::shared_ptr<ASTContext> astCtx;
  ModuleNode *ast = nullptr;
  std::shared_ptr<DiagnosticsEngine> diags;
  std::shared_ptr<SemaContext> sema;
};

std::map<std::string, DocumentState> documents;
std::shared_mutex docMutex;
std::mutex stdoutMutex;

std::mutex cacheMutex;
std::map<std::string, ModuleLoaderConfig> projectConfigCache;
std::map<std::string, std::filesystem::path> uriToProjectRoot;

std::mutex workerMutex;
std::condition_variable workerCV;
std::condition_variable doneCV;
std::string pendingUri;
std::string pendingText;
bool hasPendingChange = false;
bool isProcessing = false;
bool forceProcess = false;
std::atomic<bool> isRunning{true};

/*
 * Synchronizes the main LSP thread with the worker thread.
 * If there is a pending document change, it forces the worker to skip
 * the debounce timer, process the file immediately, and waits until completion.
 */
void syncWorker() {
  std::unique_lock<std::mutex> lock(workerMutex);
  if (hasPendingChange) {
    forceProcess = true;
    workerCV.notify_one();
  }
  if (hasPendingChange || isProcessing) {
    doneCV.wait(lock, [] { return !hasPendingChange && !isProcessing; });
  }
}

void sendResponse(const json &res) {
  std::string content =
      res.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

  std::lock_guard<std::mutex> lock(stdoutMutex);
  std::cout << "Content-Length: " << content.length() << "\r\n\r\n"
            << content << std::flush;
}

class LocalVarCollector : public ASTVisitor<LocalVarCollector, void> {
public:
  int targetLine;
  std::vector<const VarDeclNode *> locals;

  LocalVarCollector(int line) : targetLine(line) {}

  void visit(const VarDeclNode *n) {
    if (n->line <= targetLine)
      locals.push_back(n);
    if (n->initializer)
      dispatch(n->initializer);
  }

  void visit(const BlockNode *n) {
    for (auto *s : n->statements)
      dispatch(s);
  }

  void visit(const IfNode *n) {
    if (n->condition)
      dispatch(n->condition);
    if (n->thenBlock)
      dispatch(n->thenBlock);
    if (n->elseBlock)
      dispatch(n->elseBlock);
  }

  void visit(const ForNode *n) {
    if (n->initStatement)
      dispatch(n->initStatement);
    if (n->condition)
      dispatch(n->condition);
    if (n->increment)
      dispatch(n->increment);
    if (n->body)
      dispatch(n->body);
  }

  void visit(const WhileNode *n) {
    if (n->condition)
      dispatch(n->condition);
    if (n->body)
      dispatch(n->body);
  }

  void visit(const SwitchNode *n) {
    if (n->condition)
      dispatch(n->condition);
    for (auto *c : n->cases)
      dispatch(c);
  }

  void visit(const CaseNode *n) {
    if (n->value)
      dispatch(n->value);
    for (auto *s : n->statements)
      dispatch(s);
  }

  void visit(const NumberNode *) {}
  void visit(const BoolNode *) {}
  void visit(const CharNode *) {}
  void visit(const RuneNode *) {}
  void visit(const StringNode *) {}
  void visit(const NullNode *) {}
  void visit(const TypeLiteralNode *) {}
  void visit(const VariableNode *) {}
  void visit(const UnaryOpNode *) {}
  void visit(const BinaryOpNode *) {}
  void visit(const TernaryOpNode *) {}
  void visit(const AssignNode *) {}
  void visit(const ArrayLiteralNode *) {}
  void visit(const ArraySubscriptNode *) {}
  void visit(const MemberAccessNode *) {}
  void visit(const FunctionCallNode *) {}
  void visit(const CastNode *) {}
  void visit(const NewExprNode *) {}
  void visit(const DeleteExprNode *) {}
  void visit(const ImplicitCastNode *) {}
  void visit(const ReturnNode *) {}
  void visit(const BreakNode *) {}
  void visit(const ContinueNode *) {}
  void visit(const ParamDeclNode *) {}
  void visit(const FunctionDeclNode *) {}
  void visit(const StructDeclNode *) {}
  void visit(const ClassDeclNode *) {}
  void visit(const UnionDeclNode *) {}
  void visit(const EnumDeclNode *) {}
  void visit(const EnumMemberNode *) {}
  void visit(const AnnotationDeclNode *) {}
  void visit(const TypedefDeclNode *) {}
  void visit(const AnnotationNode *) {}
  void visit(const ModuleNode *) {}
};

std::string pathToUri(std::string_view path) {
  if (path.empty()) {
    return "";
  }
  std::string uri(path);
  std::replace(uri.begin(), uri.end(), '\\', '/');
#if defined(_WIN32)
  if (!uri.empty() && uri[0] != '/') {
    uri = "/" + uri;
  }
#endif
  if (!uri.starts_with("file://")) {
    uri = "file://" + uri;
  }
  return uri;
}

std::string getFileText(const std::string &path) {
  std::string uri = pathToUri(path);
  if (documents.contains(uri)) {
    return documents[uri].text;
  }
  std::ifstream file(path);
  if (file) {
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }
  return "";
}

struct SourceLocation {
  int line;
  int col;
  int length;
};

SourceLocation getExactNameLocation(const std::string &text,
                                    const DeclNode *decl) {
  if (!decl) {
    return {-1, -1, 0};
  }

  if (decl->identifierColumn > 0 && decl->identifierLength > 0) {
    return {decl->line > 0 ? decl->line - 1 : 0, decl->identifierColumn - 1,
            decl->identifierLength};
  }

  SourceLocation loc{decl->line > 0 ? decl->line - 1 : 0,
                     decl->column > 0 ? decl->column - 1 : 0,
                     decl->length > 0 ? decl->length : 1};
  if (text.empty() || decl->line <= 0)
    return loc;

  std::string_view name;
  if (auto *varDecl = llvm::dyn_cast<VarDeclNode>(decl))
    name = varDecl->varName;
  else if (auto *funcDecl = llvm::dyn_cast<FunctionDeclNode>(decl))
    name = funcDecl->name;
  else if (auto *paramDecl = llvm::dyn_cast<ParamDeclNode>(decl))
    name = paramDecl->name;
  else if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(decl))
    name = structDecl->name;
  else if (auto *classDecl = llvm::dyn_cast<ClassDeclNode>(decl))
    name = classDecl->name;
  else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(decl))
    name = unionDecl->name;
  else if (auto *enumDecl = llvm::dyn_cast<EnumDeclNode>(decl))
    name = enumDecl->name;
  else if (auto *typedefDecl = llvm::dyn_cast<TypedefDeclNode>(decl))
    name = typedefDecl->aliasName;
  else if (auto *enumMem = llvm::dyn_cast<EnumMemberNode>(decl))
    name = enumMem->name;
  else if (auto *annDecl = llvm::dyn_cast<AnnotationDeclNode>(decl))
    name = annDecl->name;

  if (name.empty())
    return loc;

  int currentLine = 0;
  size_t startIdx = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (currentLine == loc.line) {
      startIdx = i + loc.col;
      break;
    }
    if (text[i] == '\n')
      currentLine++;
  }

  if (startIdx >= text.length())
    return loc;

  size_t maxSearchLen = std::min(text.length() - startIdx, (size_t)150);
  std::string_view searchArea(text.data() + startIdx, maxSearchLen);

  size_t searchPos = 0;
  while ((searchPos = searchArea.find(name, searchPos)) !=
         std::string_view::npos) {
    bool leftOk = searchPos == 0 || (!std::isalnum(searchArea[searchPos - 1]) &&
                                     searchArea[searchPos - 1] != '_');
    bool rightOk = searchPos + name.length() >= searchArea.length() ||
                   (!std::isalnum(searchArea[searchPos + name.length()]) &&
                    searchArea[searchPos + name.length()] != '_');

    if (leftOk && rightOk) {
      int newCol = 0;
      int newLine = 0;
      for (size_t i = 0; i < startIdx + searchPos; ++i) {
        if (text[i] == '\n') {
          newLine++;
          newCol = 0;
        } else {
          newCol++;
        }
      }
      return {newLine, newCol, (int)name.length()};
    }
    searchPos += name.length();
  }

  return {loc.line, loc.col, (int)name.length()};
}

const DeclNode *getTypeDeclaration(const Type *ty) {
  if (!ty)
    return nullptr;
  const Type *unqual = ty->getUnqualifiedType();

  while (unqual->isPointerType())
    unqual = static_cast<const PointerType *>(unqual)
                 ->getPointeeType()
                 ->getUnqualifiedType();

  while (unqual->isReferenceType() ||
         unqual->getKind() == TypeKind::RValueReference) {
    if (unqual->isReferenceType())
      unqual = static_cast<const ReferenceType *>(unqual)
                   ->getPointeeType()
                   ->getUnqualifiedType();
    else
      unqual = static_cast<const RValueReferenceType *>(unqual)
                   ->getPointeeType()
                   ->getUnqualifiedType();
  }

  while (unqual->getKind() == TypeKind::Array)
    unqual = static_cast<const ArrayType *>(unqual)
                 ->getElementType()
                 ->getUnqualifiedType();

  if (unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Union) {
    return static_cast<const RecordType *>(unqual)->getDeclaration();
  } else if (unqual->getKind() == TypeKind::Enum) {
    return static_cast<const EnumType *>(unqual)->getDeclaration();
  } else if (unqual->getKind() == TypeKind::Alias) {
    return static_cast<const AliasType *>(unqual)->getDeclaration();
  }
  return nullptr;
}

std::string getHoverTextForDecl(const DeclNode *decl) {
  if (!decl)
    return "";
  std::string text;

  if (auto *classDecl = llvm::dyn_cast<ClassDeclNode>(decl)) {
    text = "```utopia\nclass " + std::string(classDecl->name) + "\n```";
  } else if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(decl)) {
    text = "```utopia\nstruct " + std::string(structDecl->name) + "\n```";
  } else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(decl)) {
    text = "```utopia\nunion " + std::string(unionDecl->name) + "\n```";
  } else if (auto *enumDecl = llvm::dyn_cast<EnumDeclNode>(decl)) {
    text = "```utopia\nenum " + std::string(enumDecl->name) + "\n```";
  } else if (auto *typedefDecl = llvm::dyn_cast<TypedefDeclNode>(decl)) {
    text =
        "```utopia\ntypedef " + std::string(typedefDecl->aliasName) + "\n```";
  } else if (auto *annDecl = llvm::dyn_cast<AnnotationDeclNode>(decl)) {
    text = "```utopia\nannotation " + std::string(annDecl->name) + "\n```";
  }

  if (!decl->docString.empty())
    text += "\n---\n" + std::string(decl->docString);

  return text;
}

std::string formatFunctionSignature(const FunctionDeclNode *func) {
  std::string sig = "";

  for (auto *ann : func->annotations) {
    sig += "@" + std::string(ann->name);
    if (!ann->args.empty()) {
      sig += "(...)";
    }
    sig += "\n";
  }

  if (func->isExtern)
    sig += "extern ";

  if (func->returnType && !func->isImplicit) {
    if (!func->rawReturnTypeStr.empty()) {
      sig += std::string(func->rawReturnTypeStr) + " ";
    } else {
      sig += func->returnType->toString() + " ";
    }
  }

  sig += std::string(func->name);

  if (func->isTemplate && !func->templateParams.empty()) {
    sig += "<";
    for (size_t i = 0; i < func->templateParams.size(); ++i) {
      sig += std::string(func->templateParams[i]);
      if (i + 1 < func->templateParams.size())
        sig += ", ";
    }
    sig += ">";
  }

  sig += "(";
  bool inNamed = false;
  bool firstParam = true;

  for (size_t i = 0; i < func->params.size(); ++i) {
    const ParamDeclNode *p = func->params[i];

    /* Skip the implicit instance pointer for methods */
    if (p->name == "this")
      continue;

    if (p->isNamed && !inNamed) {
      if (!firstParam)
        sig += ", ";
      sig += "{";
      inNamed = true;
      firstParam = true;
    } else {
      if (!firstParam)
        sig += ", ";
    }

    firstParam = false;

    if (p->isRequired)
      sig += "required ";

    if (!p->rawTypeStr.empty()) {
      sig += std::string(p->rawTypeStr) + " ";
    } else if (p->type) {
      sig += p->type->toString() + " ";
    }

    sig += std::string(p->name);

    if (p->defaultValue) {
      std::string defStr = Formatter::format(p->defaultValue);
      while (!defStr.empty() &&
             (defStr.back() == '\n' || defStr.back() == '\r' ||
              defStr.back() == ';')) {
        defStr.pop_back();
      }
      sig += " = " + defStr;
    }
  }

  if (func->isVariadic) {
    if (!firstParam)
      sig += ", ";
    sig += "...";
  }

  if (inNamed)
    sig += "}";
  sig += ")";

  if (func->isConst && func->isMethod) {
    sig += " const";
  }

  return sig;
}

std::vector<const FunctionDeclNode *> getOverloads(const FunctionDeclNode *func,
                                                   SemaContext *sema) {
  std::vector<const FunctionDeclNode *> overloads;
  if (!func)
    return overloads;

  if (func->parentRecord) {
    const DeclNode *recDecl = func->parentRecord->getDeclaration();
    if (recDecl) {
      llvm::ArrayRef<FunctionDeclNode *> methods;
      llvm::ArrayRef<FunctionDeclNode *> ctors;

      if (recDecl->kind == NodeKind::ClassDecl) {
        methods = static_cast<const ClassDeclNode *>(recDecl)->methods;
        ctors = static_cast<const ClassDeclNode *>(recDecl)->constructors;
      } else if (recDecl->kind == NodeKind::StructDecl) {
        methods = static_cast<const StructDeclNode *>(recDecl)->methods;
        ctors = static_cast<const StructDeclNode *>(recDecl)->constructors;
      } else if (recDecl->kind == NodeKind::UnionDecl) {
        methods = static_cast<const UnionDeclNode *>(recDecl)->methods;
        ctors = static_cast<const UnionDeclNode *>(recDecl)->constructors;
      }

      if (func->name == func->parentRecord->getName()) {
        for (auto *c : ctors) {
          if (!c->isImplicit)
            overloads.push_back(c);
        }
      } else {
        for (auto *m : methods) {
          if (m->name == func->name)
            overloads.push_back(m);
        }
      }
    }
  } else if (sema) {
    auto results = sema->lookup(func->name);
    for (const DeclNode *d : results) {
      if (d->kind == NodeKind::FunctionDecl) {
        overloads.push_back(static_cast<const FunctionDeclNode *>(d));
      }
    }
  }

  if (std::find(overloads.begin(), overloads.end(), func) == overloads.end()) {
    overloads.insert(overloads.begin(), func);
  }

  return overloads;
}

std::string buildFunctionHover(const FunctionDeclNode *targetFunc) {
  if (!targetFunc)
    return "";
  std::string res =
      "```utopia\n" + formatFunctionSignature(targetFunc) + "\n```";
  if (!targetFunc->docString.empty()) {
    res += "\n---\n" + std::string(targetFunc->docString);
  }
  return res;
}

void handleHover(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = nullptr;

  std::shared_lock<std::shared_mutex> lock(docMutex);
  if (documents.contains(uri)) {
    auto &doc = documents[uri];
    SearchVisitor searcher(line, col);
    const ASTNode *node = searcher.find(doc.ast);

    if (node) {
      std::string hoverText;
      const DeclNode *declTarget = nullptr;

      if (node->kind == NodeKind::Variable) {
        auto varNode = static_cast<const VariableNode *>(node);
        if (varNode->resolvedDecl) {
          declTarget = varNode->resolvedDecl;
          if (varNode->resolvedDecl->kind == NodeKind::VarDecl) {
            auto decl = static_cast<const VarDeclNode *>(varNode->resolvedDecl);
            hoverText = "```utopia\n" + decl->type->toString() + " " +
                        std::string(decl->varName) + "\n```";
            if (!decl->docString.empty())
              hoverText += "\n---\n" + std::string(decl->docString);
          } else if (varNode->resolvedDecl->kind == NodeKind::FunctionDecl) {
            auto decl =
                static_cast<const FunctionDeclNode *>(varNode->resolvedDecl);
            hoverText = buildFunctionHover(decl);
          } else if (varNode->resolvedDecl->kind == NodeKind::ParamDecl) {
            auto decl =
                static_cast<const ParamDeclNode *>(varNode->resolvedDecl);
            hoverText = "```utopia\n" + decl->type->toString() + " " +
                        std::string(decl->name) + "\n```";
          } else {
            hoverText = getHoverTextForDecl(varNode->resolvedDecl);
          }
        }
      } else if (node->kind == NodeKind::FunctionCall) {
        auto callNode = static_cast<const FunctionCallNode *>(node);
        if (callNode->resolvedFunc) {
          declTarget = callNode->resolvedFunc;
          hoverText = buildFunctionHover(callNode->resolvedFunc);
        }
      } else if (node->kind == NodeKind::MemberAccess) {
        auto ma = static_cast<const MemberAccessNode *>(node);
        if (ma->isMethodRef && ma->resolvedMethod) {
          declTarget = ma->resolvedMethod;
          hoverText = buildFunctionHover(ma->resolvedMethod);
        } else if (ma->isStaticFieldRef && ma->resolvedVar) {
          declTarget = ma->resolvedVar;
          hoverText = "```utopia\n" + ma->resolvedVar->type->toString() + " " +
                      std::string(ma->resolvedVar->varName) + "\n```";
          if (!ma->resolvedVar->docString.empty())
            hoverText += "\n---\n" + std::string(ma->resolvedVar->docString);
        } else if (ma->isEnumMember && ma->enumMember) {
          declTarget = ma->enumMember;
          hoverText =
              "```utopia\n" + std::string(ma->enumMember->name) + "\n```";
          if (!ma->enumMember->docString.empty())
            hoverText += "\n---\n" + std::string(ma->enumMember->docString);
        }
      } else if (node->kind == NodeKind::Cast) {
        auto castNode = static_cast<const CastNode *>(node);
        if (castNode->targetType) {
          if (auto typeDecl = getTypeDeclaration(castNode->targetType)) {
            declTarget = typeDecl;
            hoverText = getHoverTextForDecl(typeDecl);
          } else {
            hoverText =
                "```utopia\n" + castNode->targetType->toString() + "\n```";
          }
        }
      } else if (node->kind == NodeKind::FunctionDecl ||
                 node->kind == NodeKind::VarDecl ||
                 node->kind == NodeKind::ParamDecl ||
                 node->kind == NodeKind::ClassDecl ||
                 node->kind == NodeKind::StructDecl ||
                 node->kind == NodeKind::UnionDecl ||
                 node->kind == NodeKind::EnumDecl ||
                 node->kind == NodeKind::EnumMember ||
                 node->kind == NodeKind::TypedefDecl ||
                 node->kind == NodeKind::AnnotationDecl) {

        declTarget = static_cast<const DeclNode *>(node);
        auto loc = getExactNameLocation(doc.text, declTarget);

        if (col - 1 < loc.col) {
          const Type *t = nullptr;
          if (declTarget->kind == NodeKind::VarDecl)
            t = static_cast<const VarDeclNode *>(declTarget)->type;
          else if (declTarget->kind == NodeKind::FunctionDecl)
            t = static_cast<const FunctionDeclNode *>(declTarget)->returnType;
          else if (declTarget->kind == NodeKind::ParamDecl)
            t = static_cast<const ParamDeclNode *>(declTarget)->type;

          if (t) {
            if (auto typeDecl = getTypeDeclaration(t)) {
              declTarget = typeDecl;
              hoverText = getHoverTextForDecl(typeDecl);
            } else {
              hoverText = "```utopia\n" + t->toString() + "\n```";
            }
          }
        }

        if (hoverText.empty()) {
          if (declTarget->kind == NodeKind::FunctionDecl) {
            hoverText = buildFunctionHover(
                static_cast<const FunctionDeclNode *>(declTarget));
          } else if (declTarget->kind == NodeKind::VarDecl) {
            auto declNode = static_cast<const VarDeclNode *>(declTarget);
            hoverText = "```utopia\n" + declNode->type->toString() + " " +
                        std::string(declNode->varName) + "\n```";
            if (!declNode->docString.empty())
              hoverText += "\n---\n" + std::string(declNode->docString);
          } else if (declTarget->kind == NodeKind::ParamDecl) {
            auto declNode = static_cast<const ParamDeclNode *>(declTarget);
            hoverText = "```utopia\n" + declNode->type->toString() + " " +
                        std::string(declNode->name) + "\n```";
          } else {
            hoverText = getHoverTextForDecl(declTarget);
          }
        }
      }

      if (!hoverText.empty()) {
        int hoverLine = node->line > 0 ? node->line - 1 : 0;
        int hoverCol = node->column > 0 ? node->column - 1 : 0;
        int hoverLen = node->length > 0 ? node->length : 1;

        if (node->kind == NodeKind::FunctionDecl ||
            node->kind == NodeKind::VarDecl ||
            node->kind == NodeKind::ParamDecl ||
            node->kind == NodeKind::ClassDecl ||
            node->kind == NodeKind::StructDecl ||
            node->kind == NodeKind::UnionDecl ||
            node->kind == NodeKind::EnumDecl ||
            node->kind == NodeKind::TypedefDecl ||
            node->kind == NodeKind::AnnotationDecl) {

          if (declTarget == node) {
            auto loc = getExactNameLocation(
                doc.text, static_cast<const DeclNode *>(node));
            hoverLine = loc.line;
            hoverCol = loc.col;
            hoverLen = loc.length;
          }
        }

        res = {{"contents", {{"kind", "markdown"}, {"value", hoverText}}},
               {"range",
                {{"start", {{"line", hoverLine}, {"character", hoverCol}}},
                 {"end",
                  {{"line", hoverLine}, {"character", hoverCol + hoverLen}}}}}};
      }
    }
  }
  sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", res}});
}

void handleSignatureHelp(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = nullptr;

  std::shared_lock<std::shared_mutex> lock(docMutex);
  if (documents.contains(uri)) {
    auto &doc = documents[uri];
    SearchVisitor searcher(line, col);
    searcher.find(doc.ast);

    if (searcher.innermostCall) {
      auto callNode = searcher.innermostCall;
      const FunctionDeclNode *targetFunc = callNode->resolvedFunc;

      if (!targetFunc && callNode->target->kind == NodeKind::Variable) {
        auto varNode = static_cast<const VariableNode *>(callNode->target);
        if (varNode->resolvedDecl &&
            varNode->resolvedDecl->kind == NodeKind::FunctionDecl) {
          targetFunc =
              static_cast<const FunctionDeclNode *>(varNode->resolvedDecl);
        }
      } else if (!targetFunc &&
                 callNode->target->kind == NodeKind::MemberAccess) {
        auto maNode = static_cast<const MemberAccessNode *>(callNode->target);
        if (maNode->resolvedMethod) {
          targetFunc = maNode->resolvedMethod;
        }
      }

      if (targetFunc) {
        auto overloads = getOverloads(targetFunc, doc.sema.get());

        json signatures = json::array();
        int activeSignature = 0;

        for (size_t i = 0; i < overloads.size(); ++i) {
          auto *f = overloads[i];
          json sig = {{"label", formatFunctionSignature(f)}};
          if (!f->docString.empty()) {
            sig["documentation"] = {{"kind", "markdown"},
                                    {"value", std::string(f->docString)}};
          }

          json parameters = json::array();
          for (const auto *p : f->params) {
            if (p->name == "this")
              continue;

            std::string pLabel = "";
            if (p->isRequired)
              pLabel += "required ";

            if (!p->rawTypeStr.empty())
              pLabel += std::string(p->rawTypeStr) + " ";
            else if (p->type)
              pLabel += p->type->toString() + " ";

            pLabel += std::string(p->name);

            json paramInfo = {{"label", pLabel}};
            if (!p->docString.empty()) {
              paramInfo["documentation"] = {
                  {"kind", "markdown"}, {"value", std::string(p->docString)}};
            }
            parameters.push_back(paramInfo);
          }
          sig["parameters"] = parameters;
          signatures.push_back(sig);

          if (f == callNode->resolvedFunc) {
            activeSignature = (int)i;
          }
        }

        int activeParameter = 0;
        if (!callNode->args.empty()) {
          activeParameter = callNode->args.size() - 1;
        }

        res = {{"signatures", signatures},
               {"activeSignature", activeSignature},
               {"activeParameter", activeParameter}};
      }
    }
  }

  sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", res}});
}

void handleDefinition(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = json::array();

  std::shared_lock<std::shared_mutex> lock(docMutex);
  if (documents.contains(uri)) {
    auto &doc = documents[uri];
    SearchVisitor searcher(line, col);
    const ASTNode *node = searcher.find(doc.ast);

    if (node) {
      const DeclNode *targetDecl = nullptr;

      if (auto *varNode = llvm::dyn_cast<VariableNode>(node)) {
        if (varNode->resolvedDecl) {
          targetDecl = varNode->resolvedDecl;
        } else if (varNode->isField && varNode->parentType) {
          auto recTy = static_cast<const RecordType *>(varNode->parentType);
          auto decl = recTy->getDeclaration();
          if (decl) {
            llvm::ArrayRef<VarDeclNode *> fields;
            if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(decl)) {
              fields = cDecl->fields;
            } else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(decl)) {
              fields = sDecl->fields;
            } else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(decl)) {
              fields = uDecl->fields;
            }
            for (auto *f : fields) {
              if (f->varName == varNode->name) {
                targetDecl = f;
                break;
              }
            }
          }
        }
      } else if (auto *callNode = llvm::dyn_cast<FunctionCallNode>(node)) {
        targetDecl = callNode->resolvedFunc;
      } else if (auto *ma = llvm::dyn_cast<MemberAccessNode>(node)) {
        if (ma->isMethodRef) {
          targetDecl = ma->resolvedMethod;
        } else if (ma->isStaticFieldRef) {
          targetDecl = ma->resolvedVar;
        } else if (ma->isEnumMember) {
          targetDecl = ma->enumMember;
        } else {
          const Type *baseTy = ma->object->exprType;
          if (baseTy) {
            if (baseTy->isPointerType()) {
              baseTy =
                  static_cast<const PointerType *>(baseTy)->getPointeeType();
            } else if (baseTy->isReferenceType() ||
                       baseTy->getKind() == TypeKind::RValueReference) {
              baseTy =
                  static_cast<const ReferenceType *>(baseTy)->getPointeeType();
            }

            const Type *unqual = baseTy->getUnqualifiedType();
            if (unqual->getKind() == TypeKind::Class ||
                unqual->getKind() == TypeKind::Struct ||
                unqual->getKind() == TypeKind::Union) {
              auto recTy = static_cast<const RecordType *>(unqual);
              auto decl = recTy->getDeclaration();
              if (decl) {
                llvm::ArrayRef<VarDeclNode *> fields;
                if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(decl)) {
                  fields = cDecl->fields;
                } else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(decl)) {
                  fields = sDecl->fields;
                } else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(decl)) {
                  fields = uDecl->fields;
                }
                for (auto *f : fields) {
                  if (f->varName == ma->memberName) {
                    targetDecl = f;
                    break;
                  }
                }
              }
            }
          }
        }
      } else if (auto *newNode = llvm::dyn_cast<NewExprNode>(node)) {
        targetDecl = newNode->resolvedConstructor;
      } else if (auto *castNode = llvm::dyn_cast<CastNode>(node)) {
        targetDecl = getTypeDeclaration(castNode->targetType);
      } else if (auto *declNode = llvm::dyn_cast<DeclNode>(node)) {
        targetDecl = declNode;
        auto loc = getExactNameLocation(doc.text, targetDecl);

        if (col - 1 < loc.col) {
          const Type *t = nullptr;
          if (auto *varDecl = llvm::dyn_cast<VarDeclNode>(targetDecl))
            t = varDecl->type;
          else if (auto *funcDecl =
                       llvm::dyn_cast<FunctionDeclNode>(targetDecl))
            t = funcDecl->returnType;
          else if (auto *paramDecl = llvm::dyn_cast<ParamDeclNode>(targetDecl))
            t = paramDecl->type;

          if (t) {
            if (auto typeDecl = getTypeDeclaration(t)) {
              targetDecl = typeDecl;
            } else {
              targetDecl = nullptr;
            }
          }
        }
      }

      if (targetDecl) {
        std::string targetUri = uri;
        std::string targetText = doc.text;

        if (!targetDecl->declFilePath.empty()) {
          targetUri = pathToUri(targetDecl->declFilePath);
          if (targetUri != uri) {
            targetText = getFileText(std::string(targetDecl->declFilePath));
          }
        }

        auto loc = getExactNameLocation(targetText, targetDecl);
        int defLine = loc.line;
        int defCol = loc.col;
        int defLen = loc.length;

        res = {
            {"uri", targetUri},
            {"range",
             {{"start", {{"line", defLine}, {"character", defCol}}},
              {"end", {{"line", defLine}, {"character", defCol + defLen}}}}}};
      }
    }
  }
  sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", res}});
}

void handleCompletion(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int reqCol = req["params"]["position"]["character"].get<int>();

  json items = json::array();
  std::unordered_set<std::string> addedLabels;

  auto addCompletion = [&](const std::string &label, int kind,
                           const std::string &detail,
                           const std::string &docString = "") {
    if (addedLabels.contains(label))
      return;
    addedLabels.insert(label);
    json item = {{"label", label}, {"kind", kind}, {"detail", detail}};
    if (!docString.empty())
      item["documentation"] = docString;
    items.push_back(item);
  };

  auto addBuiltInAnnotations = [&]() {
    std::vector<std::string> builtInAnnotations = {
        "extern",          "export",       "align",     "packed",
        "nodiscard",       "deprecated",   "inline",    "forceInline",
        "readnone",        "readonly",     "nosync",    "nofree",
        "willreturn",      "mustprogress", "nocapture", "nonnull",
        "dereferenceable", "weak"};
    for (const auto &ann : builtInAnnotations) {
      addCompletion(ann, 8, "Built-in Annotation");
    }
  };

  std::string targetLineStr = "";

  std::shared_lock<std::shared_mutex> lock(docMutex);
  if (documents.contains(uri)) {
    const std::string &text = documents[uri].text;
    int currentLine = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < text.length(); ++i) {
      if (currentLine == line - 1) {
        lineStart = i;
        break;
      }
      if (text[i] == '\n')
        currentLine++;
    }
    for (size_t i = lineStart;
         i < text.length() && text[i] != '\n' && text[i] != '\r'; ++i) {
      targetLineStr += text[i];
    }
  }

  bool isDotCompletion = false;
  bool isAtCompletion = false;
  std::string triggerWord = "";

  int cursorIdx = reqCol - 1;
  if (cursorIdx >= (int)targetLineStr.length()) {
    cursorIdx = (int)targetLineStr.length() - 1;
  }

  int tokenStart = cursorIdx;
  while (tokenStart >= 0 && (std::isalnum(targetLineStr[tokenStart]) ||
                             targetLineStr[tokenStart] == '_')) {
    tokenStart--;
  }

  int triggerSymbolIdx = tokenStart;
  while (triggerSymbolIdx >= 0 &&
         std::isspace(targetLineStr[triggerSymbolIdx])) {
    triggerSymbolIdx--;
  }

  if (triggerSymbolIdx >= 0) {
    if (targetLineStr[triggerSymbolIdx] == '.') {
      isDotCompletion = true;
    } else if (targetLineStr[triggerSymbolIdx] == '@') {
      isAtCompletion = true;
    }
  }

  if (isDotCompletion) {
    int idEnd = triggerSymbolIdx - 1;
    while (idEnd >= 0 && std::isspace(targetLineStr[idEnd])) {
      idEnd--;
    }
    int idStart = idEnd;
    while (idStart >= 0 && (std::isalnum(targetLineStr[idStart]) ||
                            targetLineStr[idStart] == '_')) {
      idStart--;
    }
    idStart++;
    if (idStart <= idEnd) {
      triggerWord = targetLineStr.substr(idStart, idEnd - idStart + 1);
    }
  }

  if (isAtCompletion) {
    addBuiltInAnnotations();
  } else if (!isDotCompletion) {
    std::vector<std::string> keywords = {
        "if",      "else",     "while",      "for",      "switch", "case",
        "default", "break",    "continue",   "return",   "import", "export",
        "as",      "new",      "delete",     "struct",   "union",  "class",
        "enum",    "typedef",  "annotation", "Function", "this",   "null",
        "true",    "false",    "public",     "private",  "const",  "static",
        "extern",  "required", "operator"};

    std::vector<std::string> primitives = {
        "int8",   "int16",   "int32",   "int64",  "uint8", "uint16", "uint32",
        "uint64", "float32", "float64", "bool",   "void",  "String", "char",
        "rune",   "int",     "uint",    "double", "usize"};

    for (const auto &kw : keywords) {
      addCompletion(kw, 14, "Keyword");
    }
    for (const auto &pr : primitives) {
      addCompletion(pr, 7, "Primitive Type");
    }

    std::vector<std::string> macros = {
        "_WIN32", "__APPLE__", "__linux__", "__gnu_linux__", "__ANDROID__",
        "x64",    "x86_64",    "x86",       "arm64",         "arm"};

    for (const auto &mc : macros) {
      addCompletion(mc, 21, "Preprocessor Macro");
    }

    addBuiltInAnnotations();
  }

  if (documents.contains(uri)) {
    auto &doc = documents[uri];
    if (doc.ast) {
      std::unordered_set<const ModuleNode *> visitedMods;
      std::vector<const DeclNode *> globals;

      std::function<void(const ModuleNode *)> collectGlobals =
          [&](const ModuleNode *mod) {
            if (!mod || visitedMods.contains(mod))
              return;
            visitedMods.insert(mod);

            for (const auto *stmt : mod->statements) {
              if (stmt->kind == NodeKind::FunctionDecl ||
                  stmt->kind == NodeKind::VarDecl ||
                  stmt->kind == NodeKind::ClassDecl ||
                  stmt->kind == NodeKind::StructDecl ||
                  stmt->kind == NodeKind::UnionDecl ||
                  stmt->kind == NodeKind::EnumDecl ||
                  stmt->kind == NodeKind::TypedefDecl ||
                  stmt->kind == NodeKind::AnnotationDecl) {
                globals.push_back(static_cast<const DeclNode *>(stmt));
              }
            }

            for (const auto *imp : mod->importedModules)
              collectGlobals(imp);
            for (const auto *exp : mod->exportedModules)
              collectGlobals(exp);
          };

      collectGlobals(doc.ast);

      const FunctionDeclNode *closestFunc = nullptr;
      for (const auto *stmt : doc.ast->statements) {
        if (stmt->kind == NodeKind::FunctionDecl) {
          auto *f = static_cast<const FunctionDeclNode *>(stmt);
          if (f->line <= line) {
            if (!closestFunc || f->line > closestFunc->line) {
              closestFunc = f;
            }
          }
        }
      }

      if (isDotCompletion && !triggerWord.empty()) {
        const Type *instanceType = nullptr;
        const DeclNode *staticTypeDecl = nullptr;

        if (closestFunc) {
          if (triggerWord == "this" && closestFunc->parentRecord) {
            instanceType = closestFunc->parentRecord;
          } else {
            for (const auto *p : closestFunc->params) {
              if (p->name == triggerWord) {
                instanceType = p->type;
                break;
              }
            }
            if (!instanceType && closestFunc->body) {
              LocalVarCollector collector(line);
              collector.dispatch(closestFunc->body);
              for (const auto *local : collector.locals) {
                if (local->varName == triggerWord) {
                  instanceType = local->type;
                  break;
                }
              }
            }
          }
        }

        if (!instanceType) {
          for (const auto *decl : globals) {
            if (decl->kind == NodeKind::VarDecl) {
              auto *v = static_cast<const VarDeclNode *>(decl);
              if (v->varName == triggerWord) {
                instanceType = v->type;
                break;
              }
            }
          }
        }

        if (!instanceType) {
          for (const auto *decl : globals) {
            if (decl->kind == NodeKind::ClassDecl) {
              if (static_cast<const ClassDeclNode *>(decl)->name ==
                  triggerWord) {
                staticTypeDecl = decl;
                break;
              }
            } else if (decl->kind == NodeKind::StructDecl) {
              if (static_cast<const StructDeclNode *>(decl)->name ==
                  triggerWord) {
                staticTypeDecl = decl;
                break;
              }
            } else if (decl->kind == NodeKind::UnionDecl) {
              if (static_cast<const UnionDeclNode *>(decl)->name ==
                  triggerWord) {
                staticTypeDecl = decl;
                break;
              }
            } else if (decl->kind == NodeKind::EnumDecl) {
              if (static_cast<const EnumDeclNode *>(decl)->name ==
                  triggerWord) {
                staticTypeDecl = decl;
                break;
              }
            }
          }
        }

        if (instanceType) {
          const Type *unqual = instanceType->getUnqualifiedType();
          while (unqual->isPointerType()) {
            unqual = static_cast<const PointerType *>(unqual)
                         ->getPointeeType()
                         ->getUnqualifiedType();
          }
          while (unqual->isReferenceType()) {
            unqual = static_cast<const ReferenceType *>(unqual)
                         ->getPointeeType()
                         ->getUnqualifiedType();
          }
          if (unqual->getKind() == TypeKind::RValueReference) {
            unqual = static_cast<const RValueReferenceType *>(unqual)
                         ->getPointeeType()
                         ->getUnqualifiedType();
          }

          if (unqual->getKind() == TypeKind::Class ||
              unqual->getKind() == TypeKind::Struct ||
              unqual->getKind() == TypeKind::Union) {
            auto *recTy = static_cast<const RecordType *>(unqual);
            if (recTy->getDeclaration()) {
              staticTypeDecl = recTy->getDeclaration();
            } else {
              for (const auto *decl : globals) {
                if ((decl->kind == NodeKind::ClassDecl &&
                     static_cast<const ClassDeclNode *>(decl)->name ==
                         recTy->getName()) ||
                    (decl->kind == NodeKind::StructDecl &&
                     static_cast<const StructDeclNode *>(decl)->name ==
                         recTy->getName()) ||
                    (decl->kind == NodeKind::UnionDecl &&
                     static_cast<const UnionDeclNode *>(decl)->name ==
                         recTy->getName())) {
                  staticTypeDecl = decl;
                  break;
                }
              }
            }
          }
        }

        if (staticTypeDecl) {
          bool isInstance = (instanceType != nullptr);

          if (staticTypeDecl->kind == NodeKind::ClassDecl ||
              staticTypeDecl->kind == NodeKind::StructDecl ||
              staticTypeDecl->kind == NodeKind::UnionDecl) {
            llvm::ArrayRef<VarDeclNode *> fields;
            llvm::ArrayRef<FunctionDeclNode *> methods;

            if (staticTypeDecl->kind == NodeKind::ClassDecl) {
              auto *c = static_cast<const ClassDeclNode *>(staticTypeDecl);
              fields = c->fields;
              methods = c->methods;
            } else if (staticTypeDecl->kind == NodeKind::StructDecl) {
              auto *s = static_cast<const StructDeclNode *>(staticTypeDecl);
              fields = s->fields;
              methods = s->methods;
            } else {
              auto *u = static_cast<const UnionDeclNode *>(staticTypeDecl);
              fields = u->fields;
              methods = u->methods;
            }

            for (const auto *f : fields) {
              if (isInstance && !f->isStatic) {
                std::string detail = f->type ? f->type->toString() : "auto";
                addCompletion(std::string(f->varName), 5, detail,
                              std::string(f->docString));
              } else if (!isInstance && f->isStatic) {
                std::string detail = f->type ? f->type->toString() : "auto";
                addCompletion(std::string(f->varName), 5, detail,
                              std::string(f->docString));
              }
            }

            for (const auto *m : methods) {
              if (m->name.starts_with("operator"))
                continue;

              if (isInstance && !m->isStatic) {
                std::string detail =
                    m->returnType ? m->returnType->toString() : "auto";
                addCompletion(std::string(m->name), 2, detail,
                              std::string(m->docString));
              } else if (!isInstance && m->isStatic) {
                std::string detail =
                    m->returnType ? m->returnType->toString() : "auto";
                addCompletion(std::string(m->name), 2, detail,
                              std::string(m->docString));
              }
            }
          } else if (!isInstance &&
                     staticTypeDecl->kind == NodeKind::EnumDecl) {
            auto *e = static_cast<const EnumDeclNode *>(staticTypeDecl);
            for (const auto *em : e->members) {
              addCompletion(std::string(em->name), 20, "enum member",
                            std::string(em->docString));
            }
          }
        }
      } else if (isAtCompletion) {
        for (const auto *decl : globals) {
          if (decl->kind == NodeKind::AnnotationDecl) {
            auto *a = static_cast<const AnnotationDeclNode *>(decl);
            addCompletion(std::string(a->name), 8, "annotation",
                          std::string(a->docString));
          }
        }
      } else if (!isDotCompletion) {
        for (const auto *decl : globals) {
          if (decl->kind == NodeKind::FunctionDecl) {
            auto *f = static_cast<const FunctionDeclNode *>(decl);
            if (f->name.starts_with("operator"))
              continue;
            std::string detail =
                f->returnType ? f->returnType->toString() : "auto";
            addCompletion(std::string(f->name), 3, detail,
                          std::string(f->docString));
          } else if (decl->kind == NodeKind::VarDecl) {
            auto *v = static_cast<const VarDeclNode *>(decl);
            std::string detail = v->type ? v->type->toString() : "auto";
            addCompletion(std::string(v->varName), 6, detail,
                          std::string(v->docString));
          } else if (decl->kind == NodeKind::ClassDecl) {
            auto *c = static_cast<const ClassDeclNode *>(decl);
            addCompletion(std::string(c->name), 7, "class",
                          std::string(c->docString));
          } else if (decl->kind == NodeKind::StructDecl) {
            auto *s = static_cast<const StructDeclNode *>(decl);
            addCompletion(std::string(s->name), 22, "struct",
                          std::string(s->docString));
          } else if (decl->kind == NodeKind::UnionDecl) {
            auto *u = static_cast<const UnionDeclNode *>(decl);
            addCompletion(std::string(u->name), 22, "union",
                          std::string(u->docString));
          } else if (decl->kind == NodeKind::EnumDecl) {
            auto *e = static_cast<const EnumDeclNode *>(decl);
            addCompletion(std::string(e->name), 13, "enum",
                          std::string(e->docString));
            for (const auto *em : e->members) {
              addCompletion(std::string(em->name), 20, "enum member",
                            std::string(em->docString));
            }
          } else if (decl->kind == NodeKind::TypedefDecl) {
            auto *t = static_cast<const TypedefDeclNode *>(decl);
            addCompletion(std::string(t->aliasName), 8, "typedef",
                          std::string(t->docString));
          } else if (decl->kind == NodeKind::AnnotationDecl) {
            auto *a = static_cast<const AnnotationDeclNode *>(decl);
            addCompletion(std::string(a->name), 8, "annotation",
                          std::string(a->docString));
          }
        }

        if (closestFunc) {
          for (const auto *p : closestFunc->params) {
            std::string detail = p->type ? p->type->toString() : "auto";
            addCompletion(std::string(p->name), 6, detail,
                          std::string(p->docString));
          }
          if (closestFunc->body) {
            LocalVarCollector collector(line);
            collector.dispatch(closestFunc->body);
            for (const auto *local : collector.locals) {
              std::string detail =
                  local->type ? local->type->toString() : "auto";
              addCompletion(std::string(local->varName), 6, detail,
                            std::string(local->docString));
            }
          }
        }
      }
    }
  }

  sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", items}});
}

std::string uriToPath(const std::string &uri) {
  const std::string filePrefix = "file://";
  if (uri.starts_with(filePrefix)) {
    std::string path = uri.substr(filePrefix.length());
#if defined(_WIN32)
    if (path.length() >= 3 && path[0] == '/' && path[2] == ':') {
      path = path.substr(1);
    }
#endif
    return path;
  }
  return uri;
}

std::filesystem::path findProjectRootLSP(std::filesystem::path current) {
  if (!std::filesystem::is_directory(current))
    current = current.parent_path();
  while (current.has_parent_path()) {
    if (std::filesystem::exists(current / "build.yaml"))
      return current;
    current = current.parent_path();
  }
  return "";
}

/*
 * Recursively parses build.yaml manifests to map subproject dependencies
 * to their respective include directories for LSP package resolution.
 */
void loadPackagesLSP(const std::filesystem::path &manifestPath,
                     std::unordered_map<std::string, std::string> &packages,
                     std::vector<std::string> &includeDirs,
                     std::unordered_set<std::string> &visited) {
  if (manifestPath.empty() || !std::filesystem::exists(manifestPath))
    return;

  std::string absPath = std::filesystem::absolute(manifestPath).string();
  if (visited.contains(absPath))
    return;
  visited.insert(absPath);

  try {
    YAML::Node root = YAML::LoadFile(manifestPath.string());
    std::string projName = "unknown";
    if (root["project"] && root["project"]["name"]) {
      projName = root["project"]["name"].as<std::string>();
    }

    std::filesystem::path baseDir = manifestPath.parent_path();
    std::string selfPkgRoot = baseDir.string();

    if (root["build"]) {
      auto b = root["build"];
      std::vector<std::string> dirs;

      if (b["source_dirs"] && b["source_dirs"].IsSequence()) {
        for (const auto &dir : b["source_dirs"]) {
          dirs.push_back((baseDir / dir.as<std::string>()).string());
        }
      }

      if (b["include_dirs"] && b["include_dirs"].IsSequence()) {
        for (const auto &inc : b["include_dirs"]) {
          dirs.push_back((baseDir / inc.as<std::string>()).string());
        }
      }

      for (const auto &d : dirs) {
        if (std::find(includeDirs.begin(), includeDirs.end(), d) ==
            includeDirs.end()) {
          includeDirs.push_back(d);
        }
      }

      if (!dirs.empty()) {
        selfPkgRoot = dirs.front();
      }
    }

    if (std::find(includeDirs.begin(), includeDirs.end(), baseDir.string()) ==
        includeDirs.end()) {
      includeDirs.push_back(baseDir.string());
    }

    packages[projName] = selfPkgRoot;

    if (root["dependencies"] && root["dependencies"].IsSequence()) {
      for (const auto &dep : root["dependencies"]) {
        if (dep["path"]) {
          std::string depPath = dep["path"].as<std::string>();
          std::filesystem::path depYaml = baseDir / depPath / "build.yaml";
          loadPackagesLSP(depYaml, packages, includeDirs, visited);
        }
      }
    }
  } catch (...) {
    /* Silently ignore YAML parsing errors in LSP to prevent crashes */
  }
}

void processFile(const std::string &uri, std::string text) {
  DocumentState newState;
  newState.text = std::move(text);
  newState.astCtx = std::make_shared<ASTContext>();
  newState.diags = std::make_shared<DiagnosticsEngine>();
  newState.diags->printToConsole = false;

  std::string filePath = uriToPath(uri);
  std::filesystem::path currentPath(filePath);

  std::filesystem::path projRoot;
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (uriToProjectRoot.contains(uri)) {
      projRoot = uriToProjectRoot[uri];
    } else {
      projRoot = findProjectRootLSP(currentPath);
      uriToProjectRoot[uri] = projRoot;
    }
  }

  ModuleLoaderConfig modConfig;
  bool foundCache = false;
  std::string projRootStr = projRoot.string();

  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (projectConfigCache.contains(projRootStr)) {
      modConfig = projectConfigCache[projRootStr];
      foundCache = true;
    }
  }

  if (!foundCache) {
    modConfig.projectRoot = projRoot;
    std::filesystem::path stdlibPath =
        projRoot.empty()
            ? ""
            : projRoot.parent_path().parent_path() / "libs" / "stdlib" / "lib";
    std::filesystem::path preludePath =
        projRoot.empty()
            ? ""
            : projRoot.parent_path().parent_path() / "libs" / "prelude" / "lib";
    std::filesystem::path buildLibPath =
        projRoot.empty()
            ? ""
            : projRoot.parent_path().parent_path() / "libs" / "builder" / "lib";

#ifdef UTOPIA_SOURCE_DIR
    if (!std::filesystem::exists(stdlibPath)) {
      stdlibPath =
          std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" / "stdlib" / "lib";
      preludePath =
          std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" / "prelude" / "lib";
      buildLibPath =
          std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" / "builder" / "lib";
    }
#endif

    modConfig.stdlibRoot = stdlibPath;
    modConfig.preludeRoot = preludePath;
    modConfig.buildLibRoot = buildLibPath;

    if (!projRoot.empty()) {
      std::unordered_set<std::string> visited;
      loadPackagesLSP(projRoot / "build.yaml", modConfig.packages,
                      modConfig.includeDirs, visited);
    }

#if defined(_WIN32)
    modConfig.definedMacros.insert("_WIN32");
#elif defined(__APPLE__)
    modConfig.definedMacros.insert("__APPLE__");
#elif defined(__linux__) || defined(__gnu_linux__)
    modConfig.definedMacros.insert("__gnu_linux__");
#endif
#if defined(__x86_64__) || defined(_M_X64)
    modConfig.definedMacros.insert("x64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    modConfig.definedMacros.insert("arm64");
#endif

    std::lock_guard<std::mutex> lock(cacheMutex);
    projectConfigCache[projRootStr] = modConfig;
  }

  /* Flag the current configuration to allow builder API resolution if
   * applicable */
  if (currentPath.filename() == "build.utp") {
    modConfig.isBuildScript = true;
  }

  ModuleLoader loader(*newState.astCtx, modConfig, *newState.diags);

  try {
    newState.ast = loader.loadModule(filePath, currentPath.parent_path(), 0, 0,
                                     0, filePath, newState.text);
    if (newState.ast) {
      newState.sema = std::make_shared<SemaContext>(*newState.astCtx,
                                                    *newState.diags, filePath);
      SemaPipeline pipeline;
      pipeline.run(newState.ast, *newState.sema);
    }
  } catch (...) {
  }

  sendResponse(
      {{"jsonrpc", "2.0"},
       {"method", "textDocument/publishDiagnostics"},
       {"params", {{"uri", uri}, {"diagnostics", newState.diags->toJSON()}}}});

  {
    std::unique_lock<std::shared_mutex> lock(docMutex);
    documents[uri] = std::move(newState);
  }
}

void handleFormatting(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  json res = nullptr;

  std::string textToFormat;
  std::filesystem::path projRoot;

  {
    std::shared_lock<std::shared_mutex> lock(docMutex);
    if (documents.contains(uri)) {
      textToFormat = documents[uri].text;
    }
  }

  if (!textToFormat.empty()) {
    std::string filePath = uriToPath(uri);
    std::filesystem::path currentPath(filePath);

    {
      std::lock_guard<std::mutex> lock(cacheMutex);
      if (uriToProjectRoot.contains(uri)) {
        projRoot = uriToProjectRoot[uri];
      } else {
        projRoot = findProjectRootLSP(currentPath);
        uriToProjectRoot[uri] = projRoot;
      }
    }

    ModuleLoaderConfig modConfig;
    bool foundCache = false;
    std::string projRootStr = projRoot.string();

    {
      std::lock_guard<std::mutex> lock(cacheMutex);
      if (projectConfigCache.contains(projRootStr)) {
        modConfig = projectConfigCache[projRootStr];
        foundCache = true;
      }
    }

    if (!foundCache) {
      modConfig.projectRoot = projRoot;
      std::filesystem::path stdlibPath =
          projRoot.empty() ? ""
                           : projRoot.parent_path().parent_path() / "libs" /
                                 "stdlib" / "lib";
      std::filesystem::path preludePath =
          projRoot.empty() ? ""
                           : projRoot.parent_path().parent_path() / "libs" /
                                 "prelude" / "lib";
      std::filesystem::path buildLibPath =
          projRoot.empty() ? ""
                           : projRoot.parent_path().parent_path() / "libs" /
                                 "builder" / "lib";

#ifdef UTOPIA_SOURCE_DIR
      if (!std::filesystem::exists(stdlibPath)) {
        stdlibPath = std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" /
                     "stdlib" / "lib";
        preludePath = std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" /
                      "prelude" / "lib";
        buildLibPath = std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" /
                       "builder" / "lib";
      }
#endif

      modConfig.stdlibRoot = stdlibPath;
      modConfig.preludeRoot = preludePath;
      modConfig.buildLibRoot = buildLibPath;

      if (!projRoot.empty()) {
        std::unordered_set<std::string> visited;
        loadPackagesLSP(projRoot / "build.yaml", modConfig.packages,
                        modConfig.includeDirs, visited);
      }

#if defined(_WIN32)
      modConfig.definedMacros.insert("_WIN32");
#elif defined(__APPLE__)
      modConfig.definedMacros.insert("__APPLE__");
#elif defined(__linux__) || defined(__gnu_linux__)
      modConfig.definedMacros.insert("__gnu_linux__");
#endif
#if defined(__x86_64__) || defined(_M_X64)
      modConfig.definedMacros.insert("x64");
#elif defined(__aarch64__) || defined(_M_ARM64)
      modConfig.definedMacros.insert("arm64");
#endif

      std::lock_guard<std::mutex> lock(cacheMutex);
      projectConfigCache[projRootStr] = modConfig;
    }

    if (currentPath.filename() == "build.utp") {
      modConfig.isBuildScript = true;
    }

    modConfig.isFormatting = true;

    ASTContext formatAstCtx;
    DiagnosticsEngine formatDiags;
    formatDiags.printToConsole = false;

    ModuleLoader loader(formatAstCtx, modConfig, formatDiags);

    try {
      ModuleNode *formatAst = loader.loadModule(
          filePath, currentPath.parent_path(), 0, 0, 0, filePath, textToFormat);
      if (formatAst) {
        std::string formatted = Formatter::format(formatAst);
        if (!formatted.empty()) {
          int lineCount =
              std::count(textToFormat.begin(), textToFormat.end(), '\n') + 1;
          res = json::array();
          res.push_back({{"range",
                          {{"start", {{"line", 0}, {"character", 0}}},
                           {"end", {{"line", lineCount}, {"character", 0}}}}},
                         {"newText", formatted}});
        }
      }
    } catch (...) {
    }
  }
  sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", res}});
}

void workerThread() {
  while (isRunning) {
    std::string uri, text;
    {
      std::unique_lock<std::mutex> lock(workerMutex);
      workerCV.wait(
          lock, [] { return hasPendingChange || forceProcess || !isRunning; });
      if (!isRunning)
        break;

      bool interrupted = true;
      /* Only wait the 200ms debounce interval if processing isn't being forced
       */
      while (interrupted && !forceProcess) {
        auto status = workerCV.wait_for(lock, std::chrono::milliseconds(200));
        if (status == std::cv_status::timeout) {
          interrupted = false;
        }
      }

      forceProcess = false;
      hasPendingChange = false;
      uri = pendingUri;
      text = std::move(pendingText);
      isProcessing = true;
    }

    if (!uri.empty()) {
      processFile(uri, text);
    }

    {
      std::lock_guard<std::mutex> lock(workerMutex);
      isProcessing = false;
      doneCV.notify_all();
    }
  }
}

struct SemanticToken {
  int line;
  int col;
  int length;
  int type;
  int modifiers;

  bool operator<(const SemanticToken &other) const {
    if (line != other.line)
      return line < other.line;
    return col < other.col;
  }
};

class SemanticTokenVisitor : public ASTVisitor<SemanticTokenVisitor, void> {
public:
  std::vector<SemanticToken> tokens;
  const std::string &docText;
  ASTContext *astCtx;

  SemanticTokenVisitor(const std::string &text, ASTContext *ctx)
      : docText(text), astCtx(ctx) {}

  void addToken(int line, int col, int length, int type, int modifiers = 0) {
    if (line < 0 || col < 0 || length <= 0)
      return;
    tokens.push_back({line, col, length, type, modifiers});
  }

  /*
   * Tokenizes a raw type string directly from the source code and precisely
   * targets valid type identifiers within it, ignoring generic brackets,
   * pointers or references.
   */
  void highlightTypeString(std::string_view rawTypeStr, int nodeLine,
                           int nodeCol) {
    if (rawTypeStr.empty() || !astCtx)
      return;

    int currentLine = 1;
    size_t searchStart = 0;
    for (size_t i = 0; i < docText.length(); ++i) {
      if (currentLine == nodeLine) {
        searchStart = i + (nodeCol > 0 ? nodeCol - 1 : 0);
        break;
      }
      if (docText[i] == '\n')
        currentLine++;
    }

    size_t foundIdx = docText.find(rawTypeStr, searchStart);
    if (foundIdx == std::string::npos || foundIdx > searchStart + 150) {
      foundIdx = searchStart;
    }

    Lexer lexer(rawTypeStr);
    for (const auto &tok : lexer.tokenize()) {
      if (tok.type == TokenType::IDENTIFIER) {
        if (astCtx->getRecordType(tok.value) ||
            astCtx->getTypeAlias(tok.value) ||
            astCtx->getEnumTypeByName(tok.value) ||
            astCtx->isTemplateName(tok.value)) {

          size_t tokAbsIdx = foundIdx + (tok.value.data() - rawTypeStr.data());

          int absLine = 1;
          int absCol = 1;
          for (size_t i = 0; i < tokAbsIdx; ++i) {
            if (docText[i] == '\n') {
              absLine++;
              absCol = 1;
            } else {
              absCol++;
            }
          }

          addToken(absLine - 1, absCol - 1, tok.value.length(), 3, 0);
        }
      }
    }
  }

  void visit(const TypeLiteralNode *n) {
    if (n->length <= 0)
      return;

    int currentLine = 1;
    size_t startIdx = 0;
    for (size_t i = 0; i < docText.length(); ++i) {
      if (currentLine == n->line) {
        startIdx = i + (n->column > 0 ? n->column - 1 : 0);
        break;
      }
      if (docText[i] == '\n')
        currentLine++;
    }

    if (startIdx < docText.length()) {
      std::string_view raw = std::string_view(
          docText.data() + startIdx,
          std::min((size_t)n->length, docText.length() - startIdx));
      highlightTypeString(raw, n->line, n->column);
    }
  }

  void visit(const VariableNode *n) {
    if (n->resolvedDecl) {
      int type = 7; /* variable */
      int mods = 0;

      if (n->resolvedDecl->kind == NodeKind::ClassDecl)
        type = 0;
      else if (n->resolvedDecl->kind == NodeKind::StructDecl)
        type = 1;
      else if (n->resolvedDecl->kind == NodeKind::EnumDecl)
        type = 2;
      else if (n->resolvedDecl->kind == NodeKind::UnionDecl ||
               n->resolvedDecl->kind == NodeKind::TypedefDecl)
        type = 3;
      else if (n->resolvedDecl->kind == NodeKind::FunctionDecl)
        type = 4;
      else if (n->resolvedDecl->kind == NodeKind::ParamDecl)
        type = 8;
      else if (n->resolvedDecl->kind == NodeKind::VarDecl) {
        if (static_cast<const VarDeclNode *>(n->resolvedDecl)->isStatic)
          mods |= 2;
      }

      int trueLen = std::min(n->length, (int)n->name.length());
      addToken(n->line > 0 ? n->line - 1 : 0, n->column > 0 ? n->column - 1 : 0,
               trueLen, type, mods);
    }
  }

  void visit(const MemberAccessNode *n) {
    dispatch(n->object);
    int type = 6; /* property */
    int mods = 0;
    if (n->isMethodRef)
      type = 5; /* method */
    else if (n->isEnumMember)
      type = 9; /* enumMember */
    else if (n->isStaticFieldRef) {
      type = 6;
      mods |= 2; /* static */
    }

    int memberCol = (n->column > 0 ? n->column - 1 : 0) + n->length -
                    n->memberName.length();
    addToken(n->line > 0 ? n->line - 1 : 0, memberCol, n->memberName.length(),
             type, mods);
  }

  void visit(const FunctionCallNode *n) {
    dispatch(n->target);
    for (auto *arg : n->args)
      dispatch(arg);
  }

  void visit(const VarDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, 7, n->isStatic ? 2 : 0);

    if (!n->rawTypeStr.empty()) {
      highlightTypeString(n->rawTypeStr, n->line, n->column);
    }

    if (n->initializer)
      dispatch(n->initializer);
  }

  void visit(const ParamDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, 8, 0); /* parameter */

    if (!n->rawTypeStr.empty()) {
      highlightTypeString(n->rawTypeStr, n->line, n->column);
    }

    if (n->defaultValue)
      dispatch(n->defaultValue);
  }

  void visit(const FunctionDeclNode *n) {
    if (n->isImplicit)
      return;

    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, n->isMethod ? 5 : 4,
             n->isStatic ? 2 : 0);

    if (!n->rawReturnTypeStr.empty()) {
      highlightTypeString(n->rawReturnTypeStr, n->line, n->column);
    }

    for (auto *p : n->params)
      dispatch(p);
    if (n->body)
      dispatch(n->body);
  }

  void visit(const ClassDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, 0, 0);
    for (auto *f : n->fields)
      dispatch(f);
    for (auto *m : n->methods)
      dispatch(m);
    for (auto *c : n->constructors)
      dispatch(c);
    if (n->destructor)
      dispatch(n->destructor);
  }

  void visit(const StructDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, 1, 0);
    for (auto *f : n->fields)
      dispatch(f);
    for (auto *m : n->methods)
      dispatch(m);
    for (auto *c : n->constructors)
      dispatch(c);
    if (n->destructor)
      dispatch(n->destructor);
  }

  void visit(const UnionDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, 3, 0);
    for (auto *f : n->fields)
      dispatch(f);
    for (auto *m : n->methods)
      dispatch(m);
    for (auto *c : n->constructors)
      dispatch(c);
    if (n->destructor)
      dispatch(n->destructor);
  }

  void visit(const EnumDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, 2, 0);
    for (auto *m : n->members)
      dispatch(m);
  }

  void visit(const EnumMemberNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, 9, 0);
    if (n->initializer)
      dispatch(n->initializer);
  }

  void visit(const TypedefDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, 3, 0);

    if (!n->rawTargetTypeStr.empty()) {
      highlightTypeString(n->rawTargetTypeStr, n->line, n->column);
    }
  }

  void visit(const AnnotationDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, 0, 0);
    for (auto *f : n->fields)
      dispatch(f);
    if (n->constructor)
      dispatch(n->constructor);
  }

  void visit(const BlockNode *n) {
    for (auto *s : n->statements)
      dispatch(s);
  }
  void visit(const IfNode *n) {
    if (n->condition)
      dispatch(n->condition);
    if (n->thenBlock)
      dispatch(n->thenBlock);
    if (n->elseBlock)
      dispatch(n->elseBlock);
  }
  void visit(const ForNode *n) {
    if (n->initStatement)
      dispatch(n->initStatement);
    if (n->condition)
      dispatch(n->condition);
    if (n->increment)
      dispatch(n->increment);
    if (n->body)
      dispatch(n->body);
  }
  void visit(const WhileNode *n) {
    if (n->condition)
      dispatch(n->condition);
    if (n->body)
      dispatch(n->body);
  }
  void visit(const SwitchNode *n) {
    if (n->condition)
      dispatch(n->condition);
    for (auto *c : n->cases)
      dispatch(c);
  }
  void visit(const CaseNode *n) {
    if (n->value)
      dispatch(n->value);
    for (auto *s : n->statements)
      dispatch(s);
  }
  void visit(const AssignNode *n) {
    dispatch(n->target);
    dispatch(n->value);
  }
  void visit(const ReturnNode *n) {
    if (n->value)
      dispatch(n->value);
  }
  void visit(const UnaryOpNode *n) { dispatch(n->expr); }
  void visit(const BinaryOpNode *n) {
    dispatch(n->left);
    dispatch(n->right);
  }
  void visit(const TernaryOpNode *n) {
    dispatch(n->condition);
    dispatch(n->trueExpr);
    dispatch(n->falseExpr);
  }
  void visit(const CastNode *n) {
    dispatch(n->expr);
    if (!n->rawTargetTypeStr.empty()) {
      highlightTypeString(n->rawTargetTypeStr, n->line, n->column);
    }
  }
  void visit(const ImplicitCastNode *n) { dispatch(n->expr); }
  void visit(const ArraySubscriptNode *n) {
    dispatch(n->base);
    dispatch(n->index);
  }
  void visit(const ArrayLiteralNode *n) {
    for (auto *e : n->elements)
      dispatch(e);
  }
  void visit(const NewExprNode *n) {
    if (!n->rawAllocatedTypeStr.empty()) {
      highlightTypeString(n->rawAllocatedTypeStr, n->line, n->column);
    }
    if (n->arraySize)
      dispatch(n->arraySize);
    for (auto *a : n->args)
      dispatch(a);
  }
  void visit(const DeleteExprNode *n) { dispatch(n->ptr); }
  void visit(const AnnotationNode *n) {
    for (auto *a : n->args)
      dispatch(a);
  }
  void visit(const ModuleNode *n) {
    for (auto *s : n->statements)
      dispatch(s);
  }

  void visit(const BreakNode *) {}
  void visit(const ContinueNode *) {}
  void visit(const NumberNode *) {}
  void visit(const BoolNode *) {}
  void visit(const CharNode *) {}
  void visit(const RuneNode *) {}
  void visit(const StringNode *) {}
  void visit(const NullNode *) {}
};

void handleSemanticTokens(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  json data = json::array();

  std::shared_lock<std::shared_mutex> lock(docMutex);
  if (documents.contains(uri)) {
    auto &doc = documents[uri];
    if (doc.ast) {
      SemanticTokenVisitor visitor(doc.text, doc.astCtx.get());
      visitor.dispatch(doc.ast);

      /* Lexical pass to inject Semantic Tokens for primitive types.
       * The AST Visitor explicitly skips built-in types because they lack
       * a concrete declaration node. This guarantees that all primitive
       * types (including usize) are highlighted correctly by the LSP,
       * overriding any incomplete static TextMate grammars. */
      Lexer lexer(doc.text);
      for (const auto &tok : lexer.tokenize()) {
        if (tok.type == TokenType::TYPE_KW) {
          visitor.addToken(tok.line > 0 ? tok.line - 1 : 0,
                           tok.column > 0 ? tok.column - 1 : 0,
                           tok.value.length(), 3, 0); /* 3 represents "type" */
        }
      }

      std::vector<SemanticToken> tokens = std::move(visitor.tokens);
      std::sort(tokens.begin(), tokens.end());

      int prevLine = 0;
      int prevCol = 0;

      for (const auto &tok : tokens) {
        int deltaLine = tok.line - prevLine;
        int deltaCol = (deltaLine == 0) ? (tok.col - prevCol) : tok.col;

        /* Prevent negative deltas that could crash the VS Code LSP client
         * in the rare event of duplicate tokens at the exact same location. */
        if (deltaLine < 0 || (deltaLine == 0 && deltaCol < 0)) {
          continue;
        }

        data.push_back(deltaLine);
        data.push_back(deltaCol);
        data.push_back(tok.length);
        data.push_back(tok.type);
        data.push_back(tok.modifiers);

        prevLine = tok.line;
        prevCol = tok.col;
      }
    }
  }

  sendResponse(
      {{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", {{"data", data}}}});
}

} // namespace utopia::lsp

int main() {
  std::thread worker(utopia::lsp::workerThread);

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.find("Content-Length:") == 0) {
      int len = std::stoi(line.substr(15));
      while (std::getline(std::cin, line) && (line != "\r" && !line.empty()))
        ;

      std::vector<char> buf(len);
      std::cin.read(buf.data(), len);
      auto req = json::parse(std::string(buf.begin(), buf.end()));
      std::string method = req["method"];

      if (method == "initialize") {
        utopia::lsp::sendResponse(
            {{"jsonrpc", "2.0"},
             {"id", req["id"]},
             {"result",
              {{"capabilities",
                {{"textDocumentSync", 1},
                 {"hoverProvider", true},
                 {"definitionProvider", true},
                 {"completionProvider", {{"triggerCharacters", {".", "@"}}}},
                 {"signatureHelpProvider", {{"triggerCharacters", {"(", ","}}}},
                 {"documentFormattingProvider", true},
                 {"semanticTokensProvider",
                  {{"legend",
                    {{"tokenTypes",
                      {"class", "struct", "enum", "type", "function", "method",
                       "property", "variable", "parameter", "enumMember",
                       "macro"}},
                     {"tokenModifiers",
                      {"declaration", "static", "readonly"}}}},
                   {"range", false},
                   {"full", true}}}}}}}});
      } else if (method == "textDocument/hover") {
        utopia::lsp::handleHover(req);
      } else if (method == "textDocument/definition") {
        utopia::lsp::handleDefinition(req);
      } else if (method == "textDocument/completion") {
        utopia::lsp::handleCompletion(req);
      } else if (method == "textDocument/signatureHelp") {
        utopia::lsp::handleSignatureHelp(req);
      } else if (method == "textDocument/formatting") {
        utopia::lsp::handleFormatting(req);
      } else if (method == "textDocument/semanticTokens/full") {
        utopia::lsp::handleSemanticTokens(req);
      } else if (method == "textDocument/didOpen" ||
                 method == "textDocument/didChange") {
        std::string uri = req["params"]["textDocument"]["uri"];
        std::string text =
            (method == "textDocument/didOpen")
                ? req["params"]["textDocument"]["text"].get<std::string>()
                : req["params"]["contentChanges"][0]["text"].get<std::string>();

        {
          std::lock_guard<std::mutex> lock(utopia::lsp::workerMutex);
          utopia::lsp::pendingUri = uri;
          utopia::lsp::pendingText = std::move(text);
          utopia::lsp::hasPendingChange = true;
        }
        utopia::lsp::workerCV.notify_one();
      } else if (method == "exit") {
        utopia::lsp::isRunning = false;
        utopia::lsp::workerCV.notify_one();
        worker.join();
        return 0;
      }
    }
  }

  utopia::lsp::isRunning = false;
  utopia::lsp::workerCV.notify_one();
  worker.join();
  return 0;
}