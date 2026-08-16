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

/* Returns the request id when present, null otherwise. Handlers must never
 * touch req["id"] directly: a malformed request would otherwise abort the
 * whole server through nlohmann's assertion. */
json requestId(const json &req) {
  if (req.contains("id"))
    return req["id"];
  return nullptr;
}



class LocalVarCollector : public ASTVisitor<LocalVarCollector, void> {
public:
  int targetLine;
  std::vector<const VarDeclNode *> locals;
  std::vector<std::string> activeUsings;
  std::string currentNamespace;
  const FunctionDeclNode *closestFunc = nullptr;

  LocalVarCollector(int line) : targetLine(line) {}

  void visit(const ModuleNode *n) {
    for (auto *s : n->statements) {
      if (s->line <= targetLine)
        dispatch(s);
    }
  }

  void visit(const NamespaceDeclNode *n) {
    if (n->line <= targetLine) {
      std::string oldNs = currentNamespace;
      currentNamespace = currentNamespace.empty()
                             ? std::string(n->name)
                             : currentNamespace + "." + std::string(n->name);
      for (auto *s : n->statements) {
        if (s->line <= targetLine)
          dispatch(s);
      }
      if (targetLine > n->endLine && !n->isFileScoped) {
        currentNamespace = oldNs;
      }
    }
  }

  void visit(const UsingNode *n) {
    if (n->line <= targetLine)
      activeUsings.push_back(std::string(n->name));
  }

  void visit(const FunctionDeclNode *n) {
    if (n->line <= targetLine && n->endLine >= targetLine) {
      closestFunc = n;
      for (auto *p : n->params)
        dispatch(p);
      if (n->body)
        dispatch(n->body);
    }
  }

  void visit(const ParamDeclNode *n) {}

  void visit(const VarDeclNode *n) {
    if (n->line <= targetLine)
      locals.push_back(n);
    if (n->initializer)
      dispatch(n->initializer);
  }

  void visit(const BlockNode *n) {
    for (auto *s : n->statements) {
      if (s->line <= targetLine)
        dispatch(s);
    }
  }

  void visit(const IfNode *n) {
    if (n->condition)
      dispatch(n->condition);
    if (n->thenBlock && n->thenBlock->line <= targetLine)
      dispatch(n->thenBlock);
    if (n->elseBlock && n->elseBlock->line <= targetLine)
      dispatch(n->elseBlock);
  }

  void visit(const ForNode *n) {
    if (n->initStatement && n->initStatement->line <= targetLine)
      dispatch(n->initStatement);
    if (n->condition)
      dispatch(n->condition);
    if (n->increment)
      dispatch(n->increment);
    if (n->body && n->body->line <= targetLine)
      dispatch(n->body);
  }

  void visit(const WhileNode *n) {
    if (n->condition)
      dispatch(n->condition);
    if (n->body && n->body->line <= targetLine)
      dispatch(n->body);
  }

  void visit(const SwitchNode *n) {
    if (n->condition)
      dispatch(n->condition);
    for (auto *c : n->cases) {
      if (c->line <= targetLine)
        dispatch(c);
    }
  }

  void visit(const CaseNode *n) {
    if (n->value)
      dispatch(n->value);
    for (auto *s : n->statements) {
      if (s->line <= targetLine)
        dispatch(s);
    }
  }

  void visit(const AssignNode *n) {
    if (n->target)
      dispatch(n->target);
    if (n->value)
      dispatch(n->value);
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
  void visit(const AwaitExprNode *node) { dispatch(node->expr); }
  void visit(const BinaryOpNode *) {}
  void visit(const TernaryOpNode *) {}
  void visit(const ArrayLiteralNode *) {}
  void visit(const ArraySubscriptNode *) {}
  void visit(const MemberAccessNode *) {}
  void visit(const FunctionCallNode *) {}
  void visit(const LambdaNode *n) {
    for (auto *p : n->params)
      dispatch(p);
    if (n->isExpressionBody && n->exprBody) {
      dispatch(n->exprBody);
    } else if (n->body) {
      dispatch(n->body);
    }
  }
  void visit(const CastNode *) {}
  void visit(const NewExprNode *) {}
  void visit(const DeleteExprNode *) {}
  void visit(const ImplicitCastNode *) {}
  void visit(const ReturnNode *) {}
  void visit(const BreakNode *) {}
  void visit(const ContinueNode *) {}
  void visit(const StructDeclNode *) {}
  void visit(const ClassDeclNode *) {}
  void visit(const UnionDeclNode *) {}
  void visit(const EnumDeclNode *) {}
  void visit(const EnumMemberNode *) {}
  void visit(const AnnotationDeclNode *) {}
  void visit(const TypedefDeclNode *) {}
  void visit(const AnnotationNode *) {}
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
  else if (auto *nsDecl = llvm::dyn_cast<NamespaceDeclNode>(decl))
    name = nsDecl->name;

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
    std::string prefix = classDecl->isAbstract ? "abstract class " : "class ";
    text = "```utopia\n" + prefix + std::string(classDecl->name) + "\n```";
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
  } else if (auto *nsDecl = llvm::dyn_cast<NamespaceDeclNode>(decl)) {
    /* Provide clear tooltip documentation for namespaces, including fully
     * qualified names */
    text = "```utopia\nnamespace " + std::string(nsDecl->fqName) + "\n```";
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

std::string getHoveredTypeComponent(const std::string &docText,
                                    std::string_view rawTypeStr, int nodeLine,
                                    int nodeCol, int cursorLine,
                                    int cursorCol) {
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
  auto toks = lexer.tokenize();
  std::string chain = "";
  for (auto &tok : toks) {
    if (tok.type == TokenType::IDENTIFIER) {
      if (!chain.empty())
        chain += ".";
      chain += tok.value;

      size_t tokAbsIdx = foundIdx + (tok.value.data() - rawTypeStr.data());
      int absLine = 1;
      int absCol = 1;
      for (size_t j = 0; j < tokAbsIdx; ++j) {
        if (docText[j] == '\n') {
          absLine++;
          absCol = 1;
        } else {
          absCol++;
        }
      }

      if (cursorLine == absLine && cursorCol >= absCol &&
          cursorCol < absCol + (int)tok.value.length()) {
        return chain;
      }
    }
  }
  return "";
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

      LocalVarCollector collector(line);
      collector.dispatch(doc.ast);

      auto resolveWithCollector =
          [&](const std::string &name) -> const DeclNode * {
        auto decls = doc.sema->symTable.lookupExact(name, doc.ast);
        if (!decls.empty())
          return decls.front();

        std::string ns = collector.currentNamespace;
        while (!ns.empty()) {
          decls = doc.sema->symTable.lookupExact(ns + "." + name, doc.ast);
          if (!decls.empty())
            return decls.front();
          size_t pos = ns.find_last_of('.');
          if (pos != std::string::npos)
            ns = ns.substr(0, pos);
          else
            break;
        }

        for (const auto &u : collector.activeUsings) {
          decls = doc.sema->symTable.lookupExact(u + "." + name, doc.ast);
          if (!decls.empty())
            return decls.front();
        }
        return nullptr;
      };

      if (node->kind == NodeKind::Variable) {
        auto varNode = llvm::dyn_cast_or_null<VariableNode>(node);
        if (varNode && varNode->resolvedDecl) {
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
        auto callNode = llvm::dyn_cast_or_null<FunctionCallNode>(node);
        if (callNode && callNode->resolvedFunc) {
          declTarget = callNode->resolvedFunc;
          hoverText = buildFunctionHover(callNode->resolvedFunc);
        }
      } else if (node->kind == NodeKind::MemberAccess) {
        auto ma = llvm::dyn_cast_or_null<MemberAccessNode>(node);
        if (ma) {

          if (ma->isMethodRef && ma->resolvedMethod) {
            declTarget = ma->resolvedMethod;
            hoverText = buildFunctionHover(ma->resolvedMethod);
          } else if (ma->isStaticFieldRef && ma->resolvedDecl) {
            declTarget = ma->resolvedDecl;
            if (auto *varDecl = llvm::dyn_cast<VarDeclNode>(ma->resolvedDecl)) {
              hoverText = "```utopia\n" + varDecl->type->toString() + " " +
                          std::string(varDecl->varName) + "\n```";
              if (!varDecl->docString.empty())
                hoverText += "\n---\n" + std::string(varDecl->docString);
            }
          } else if (ma->isEnumMember && ma->enumMember) {
            declTarget = ma->enumMember;
            hoverText =
                "```utopia\n" + std::string(ma->enumMember->name) + "\n```";
            if (!ma->enumMember->docString.empty())
              hoverText += "\n---\n" + std::string(ma->enumMember->docString);
          } else if (ma->resolvedDecl) {
            declTarget = ma->resolvedDecl;
            if (auto *funcDecl =
                    llvm::dyn_cast<FunctionDeclNode>(ma->resolvedDecl)) {
              hoverText = buildFunctionHover(funcDecl);
            } else {
              hoverText = getHoverTextForDecl(ma->resolvedDecl);
            }
          }
        }
      } else if (node->kind == NodeKind::Cast) {
        auto castNode = llvm::dyn_cast_or_null<CastNode>(node);
        if (castNode && castNode->targetType) {
          std::string hoveredChain;
          if (!castNode->rawTargetTypeStr.empty()) {
            hoveredChain = getHoveredTypeComponent(
                doc.text, castNode->rawTargetTypeStr, castNode->line,
                castNode->column, line, col);
          }
          if (!hoveredChain.empty() && doc.sema) {
            declTarget = resolveWithCollector(hoveredChain);
            if (declTarget)
              hoverText = getHoverTextForDecl(declTarget);
          }
          if (hoverText.empty()) {
            if (auto typeDecl = getTypeDeclaration(castNode->targetType)) {
              declTarget = typeDecl;
              hoverText = getHoverTextForDecl(typeDecl);
            } else {
              hoverText =
                  "```utopia\n" + castNode->targetType->toString() + "\n```";
            }
          }
        }
      } else if (auto *newNode = llvm::dyn_cast_or_null<NewExprNode>(node)) {
        declTarget = newNode->resolvedConstructor;
        std::string hoveredChain;
        if (!newNode->rawAllocatedTypeStr.empty()) {
          hoveredChain = getHoveredTypeComponent(
              doc.text, newNode->rawAllocatedTypeStr, newNode->line,
              newNode->column, line, col);
        }
        if (!hoveredChain.empty() && doc.sema) {
          auto *chainDecl = resolveWithCollector(hoveredChain);
          if (chainDecl && chainDecl->kind != NodeKind::FunctionDecl) {
            declTarget = chainDecl;
            hoverText = getHoverTextForDecl(declTarget);
          }
        }
      }

      if (hoverText.empty() && node &&
          (node->kind == NodeKind::FunctionDecl ||
           node->kind == NodeKind::VarDecl ||
           node->kind == NodeKind::ParamDecl ||
           node->kind == NodeKind::ClassDecl ||
           node->kind == NodeKind::StructDecl ||
           node->kind == NodeKind::UnionDecl ||
           node->kind == NodeKind::EnumDecl ||
           node->kind == NodeKind::EnumMember ||
           node->kind == NodeKind::TypedefDecl ||
           node->kind == NodeKind::AnnotationDecl ||
           node->kind == NodeKind::NamespaceDecl)) {

        declTarget = llvm::dyn_cast_or_null<DeclNode>(node);
        if (declTarget) {
          auto loc = getExactNameLocation(doc.text, declTarget);

          if (col - 1 < loc.col) {
            const Type *t = nullptr;
            std::string_view rawTypeStr;
            if (auto *varDecl =
                    llvm::dyn_cast_or_null<VarDeclNode>(declTarget)) {
              t = varDecl->type;
              rawTypeStr = varDecl->rawTypeStr;
            } else if (auto *funcDecl =
                           llvm::dyn_cast_or_null<FunctionDeclNode>(
                               declTarget)) {
              t = funcDecl->returnType;
              rawTypeStr = funcDecl->rawReturnTypeStr;
            } else if (auto *paramDecl =
                           llvm::dyn_cast_or_null<ParamDeclNode>(declTarget)) {
              t = paramDecl->type;
              rawTypeStr = paramDecl->rawTypeStr;
            }

            std::string hoveredChain;
            if (!rawTypeStr.empty()) {
              hoveredChain = getHoveredTypeComponent(
                  doc.text, rawTypeStr, declTarget->line, declTarget->column,
                  line, col);
            }

            if (!hoveredChain.empty() && doc.sema) {
              declTarget = resolveWithCollector(hoveredChain);
              if (declTarget) {
                if (auto *funcDecl =
                        llvm::dyn_cast_or_null<FunctionDeclNode>(declTarget)) {
                  hoverText = buildFunctionHover(funcDecl);
                } else {
                  hoverText = getHoverTextForDecl(declTarget);
                }
              }
            }

            if (hoverText.empty() && t) {
              if (auto typeDecl = getTypeDeclaration(t)) {
                declTarget = typeDecl;
                hoverText = getHoverTextForDecl(typeDecl);
              } else {
                hoverText = "```utopia\n" + t->toString() + "\n```";
              }
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
            node->kind == NodeKind::AnnotationDecl ||
            node->kind == NodeKind::NamespaceDecl) {

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
  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
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
        } else if (maNode->resolvedDecl &&
                   maNode->resolvedDecl->kind == NodeKind::FunctionDecl) {
          targetFunc =
              static_cast<const FunctionDeclNode *>(maNode->resolvedDecl);
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

  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
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

      if (auto *varNode = llvm::dyn_cast_or_null<VariableNode>(node)) {
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
      } else if (auto *callNode =
                     llvm::dyn_cast_or_null<FunctionCallNode>(node)) {
        targetDecl = callNode->resolvedFunc;
      } else if (auto *ma = llvm::dyn_cast_or_null<MemberAccessNode>(node)) {
        if (ma->isMethodRef) {
          targetDecl = ma->resolvedMethod;
        } else if (ma->isStaticFieldRef) {
          targetDecl = ma->resolvedDecl;
        } else if (ma->isEnumMember) {
          targetDecl = ma->enumMember;
        } else if (ma->resolvedDecl) {
          targetDecl = ma->resolvedDecl;
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
      } else if (auto *newNode = llvm::dyn_cast_or_null<NewExprNode>(node)) {
        targetDecl = newNode->resolvedConstructor;
      } else if (auto *castNode = llvm::dyn_cast_or_null<CastNode>(node)) {
        targetDecl = getTypeDeclaration(castNode->targetType);
      } else if (auto *declNode = llvm::dyn_cast_or_null<DeclNode>(node)) {
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
  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
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

  /* Rust-style auto-deref: if the record overloads 'operator*', follow its
   * return type to the pointee record (mirroring the compiler's member-access
   * resolution for smart pointers). */
  auto getAutoDerefTarget = [&](const DeclNode *recDecl) -> const DeclNode * {
    if (!recDecl)
      return nullptr;
    llvm::ArrayRef<FunctionDeclNode *> methods;
    if (recDecl->kind == NodeKind::ClassDecl)
      methods = static_cast<const ClassDeclNode *>(recDecl)->methods;
    else if (recDecl->kind == NodeKind::StructDecl)
      methods = static_cast<const StructDeclNode *>(recDecl)->methods;
    else if (recDecl->kind == NodeKind::UnionDecl)
      methods = static_cast<const UnionDeclNode *>(recDecl)->methods;
    else
      return nullptr;

    for (const auto *m : methods) {
      if (m->name == "operator*") {
        const Type *ret = m->returnType;
        if (!ret)
          return nullptr;
        const Type *unqual = ret->getUnqualifiedType();
        if (unqual->isPointerType()) {
          unqual = static_cast<const PointerType *>(unqual)
                       ->getPointeeType()
                       ->getUnqualifiedType();
        } else if (unqual->isReferenceType()) {
          unqual = static_cast<const ReferenceType *>(unqual)
                       ->getPointeeType()
                       ->getUnqualifiedType();
        } else if (unqual->getKind() == TypeKind::RValueReference) {
          unqual = static_cast<const RValueReferenceType *>(unqual)
                       ->getPointeeType()
                       ->getUnqualifiedType();
        }
        if (unqual->getKind() == TypeKind::Class ||
            unqual->getKind() == TypeKind::Struct ||
            unqual->getKind() == TypeKind::Union) {
          auto *recTy = static_cast<const RecordType *>(unqual);
          return recTy->getDeclaration();
        }
        return nullptr;
      }
    }
    return nullptr;
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
    while (idStart >= 0 &&
           (std::isalnum(targetLineStr[idStart]) ||
            targetLineStr[idStart] == '_' || targetLineStr[idStart] == '.')) {
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
        "if",        "else",      "while",      "for",      "switch",
        "case",      "default",   "break",      "continue", "return",
        "import",    "export",    "as",         "new",      "delete",
        "struct",    "union",     "class",      "extends",  "implements",
        "enum",      "typedef",   "annotation", "Function", "this",
        "null",      "true",      "false",      "public",   "private",
        "protected", "const",     "static",     "extern",   "required",
        "operator",  "namespace", "using",      "abstract"};

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
      std::unordered_map<std::string, std::vector<const DeclNode *>>
          namespaceMembers;
      std::vector<const DeclNode *> rootGlobals;

      std::function<void(const ModuleNode *)> collectGlobals =
          [&](const ModuleNode *mod) {
            if (!mod || visitedMods.contains(mod))
              return;
            visitedMods.insert(mod);

            std::function<void(llvm::ArrayRef<ASTNode *>, const std::string &)>
                collectStmts = [&](llvm::ArrayRef<ASTNode *> stmts,
                                   const std::string &currentNs) {
                  for (const auto *stmt : stmts) {
                    if (stmt->kind == NodeKind::NamespaceDecl) {
                      auto *nsDecl =
                          static_cast<const NamespaceDeclNode *>(stmt);

                      std::string nsName = std::string(nsDecl->name);
                      std::string runningNs = currentNs;

                      /* Break down multi-part namespaces (e.g., 'wow.Math')
                       * into individual virtual components to register them
                       * accurately in the scope tree. */
                      size_t start = 0;
                      while (true) {
                        size_t dot = nsName.find('.', start);
                        std::string part =
                            (dot == std::string::npos)
                                ? nsName.substr(start)
                                : nsName.substr(start, dot - start);

                        std::string nextNs =
                            runningNs.empty() ? part : runningNs + "." + part;

                        auto *virtualNs = doc.astCtx->getOrCreateNamespace(
                            doc.astCtx->copyString(nextNs));

                        /* Carry over the documentation to the terminal
                         * namespace node */
                        if (dot == std::string::npos &&
                            virtualNs->docString.empty()) {
                          virtualNs->docString = nsDecl->docString;
                        }

                        if (runningNs.empty()) {
                          if (std::find(rootGlobals.begin(), rootGlobals.end(),
                                        virtualNs) == rootGlobals.end()) {
                            rootGlobals.push_back(virtualNs);
                          }
                        } else {
                          auto &vec = namespaceMembers[runningNs];
                          if (std::find(vec.begin(), vec.end(), virtualNs) ==
                              vec.end()) {
                            vec.push_back(virtualNs);
                          }
                        }

                        runningNs = nextNs;

                        if (dot == std::string::npos)
                          break;
                        start = dot + 1;
                      }

                      collectStmts(nsDecl->statements, runningNs);
                    } else if (stmt->kind == NodeKind::FunctionDecl ||
                               stmt->kind == NodeKind::VarDecl ||
                               stmt->kind == NodeKind::ClassDecl ||
                               stmt->kind == NodeKind::StructDecl ||
                               stmt->kind == NodeKind::UnionDecl ||
                               stmt->kind == NodeKind::EnumDecl ||
                               stmt->kind == NodeKind::TypedefDecl ||
                               stmt->kind == NodeKind::AnnotationDecl) {
                      if (currentNs.empty())
                        rootGlobals.push_back(
                            static_cast<const DeclNode *>(stmt));
                      else
                        namespaceMembers[currentNs].push_back(
                            static_cast<const DeclNode *>(stmt));
                    }
                  }
                };

            collectStmts(mod->statements, "");

            for (const auto *imp : mod->importedModules)
              collectGlobals(imp);
            for (const auto *exp : mod->exportedModules)
              collectGlobals(exp);
          };

      collectGlobals(doc.ast);

      LocalVarCollector collector(line);
      collector.dispatch(doc.ast);

      if (isDotCompletion && !triggerWord.empty()) {
        const Type *instanceType = nullptr;
        const DeclNode *staticTypeDecl = nullptr;
        std::string currentStaticNs = "";

        std::vector<std::string> chain;
        std::stringstream ss(triggerWord);
        std::string item;
        while (std::getline(ss, item, '.')) {
          if (!item.empty())
            chain.push_back(item);
        }

        if (!chain.empty()) {
          std::string first = chain[0];

          if (collector.closestFunc) {
            if (first == "this" && collector.closestFunc->parentRecord) {
              instanceType = collector.closestFunc->parentRecord;
            } else {
              for (const auto *p : collector.closestFunc->params) {
                if (p->name == first) {
                  instanceType = p->type;
                  break;
                }
              }
              if (!instanceType) {
                for (const auto *l : collector.locals) {
                  if (l->varName == first) {
                    instanceType = l->type;
                    break;
                  }
                }
              }
            }
          }

          if (!instanceType) {
            auto checkDecls = [&](const std::vector<const DeclNode *> &decls) {
              for (const auto *decl : decls) {
                if (decl->kind == NodeKind::VarDecl &&
                    static_cast<const VarDeclNode *>(decl)->varName == first) {
                  instanceType = static_cast<const VarDeclNode *>(decl)->type;
                  return true;
                } else if (decl->kind == NodeKind::ClassDecl &&
                           static_cast<const ClassDeclNode *>(decl)->name ==
                               first) {
                  staticTypeDecl = decl;
                  return true;
                } else if (decl->kind == NodeKind::StructDecl &&
                           static_cast<const StructDeclNode *>(decl)->name ==
                               first) {
                  staticTypeDecl = decl;
                  return true;
                } else if (decl->kind == NodeKind::UnionDecl &&
                           static_cast<const UnionDeclNode *>(decl)->name ==
                               first) {
                  staticTypeDecl = decl;
                  return true;
                } else if (decl->kind == NodeKind::EnumDecl &&
                           static_cast<const EnumDeclNode *>(decl)->name ==
                               first) {
                  staticTypeDecl = decl;
                  return true;
                } else if (decl->kind == NodeKind::NamespaceDecl &&
                           static_cast<const NamespaceDeclNode *>(decl)->name ==
                               first) {
                  currentStaticNs =
                      static_cast<const NamespaceDeclNode *>(decl)->fqName;
                  staticTypeDecl = decl;
                  return true;
                }
              }
              return false;
            };

            if (!checkDecls(rootGlobals)) {
              if (!collector.currentNamespace.empty()) {
                checkDecls(namespaceMembers[collector.currentNamespace]);
              }
              if (!instanceType && !staticTypeDecl) {
                for (const auto &u : collector.activeUsings) {
                  if (checkDecls(namespaceMembers[u]))
                    break;
                }
              }
            }
          }

          for (size_t i = 1; i < chain.size(); ++i) {
            std::string part = chain[i];
            const Type *nextInstanceType = nullptr;
            const DeclNode *nextStaticDecl = nullptr;
            std::string nextStaticNs = "";

            if (instanceType) {
              const Type *unqual = instanceType->getUnqualifiedType();
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
              if (unqual->getKind() == TypeKind::Class ||
                  unqual->getKind() == TypeKind::Struct ||
                  unqual->getKind() == TypeKind::Union) {
                auto *recTy = static_cast<const RecordType *>(unqual);
                const DeclNode *rDecl = recTy->getDeclaration();
                if (!rDecl) {
                  auto findRec =
                      [&](const std::vector<const DeclNode *> &decls) {
                        for (const auto *decl : decls) {
                          if ((decl->kind == NodeKind::ClassDecl &&
                               static_cast<const ClassDeclNode *>(decl)->name ==
                                   recTy->getName()) ||
                              (decl->kind == NodeKind::StructDecl &&
                               static_cast<const StructDeclNode *>(decl)
                                       ->name == recTy->getName()) ||
                              (decl->kind == NodeKind::UnionDecl &&
                               static_cast<const UnionDeclNode *>(decl)->name ==
                                   recTy->getName())) {
                            return decl;
                          }
                        }
                        return (const DeclNode *)nullptr;
                      };
                  rDecl = findRec(rootGlobals);
                  if (!rDecl) {
                    for (const auto &pair : namespaceMembers) {
                      rDecl = findRec(pair.second);
                      if (rDecl)
                        break;
                    }
                  }
                }

                if (rDecl) {
                  llvm::ArrayRef<VarDeclNode *> fields;
                  if (rDecl->kind == NodeKind::ClassDecl)
                    fields = static_cast<const ClassDeclNode *>(rDecl)->fields;
                  else if (rDecl->kind == NodeKind::StructDecl)
                    fields = static_cast<const StructDeclNode *>(rDecl)->fields;
                  else if (rDecl->kind == NodeKind::UnionDecl)
                    fields = static_cast<const UnionDeclNode *>(rDecl)->fields;

                  for (const auto *f : fields) {
                    if (f->varName == part) {
                      nextInstanceType = f->type;
                      break;
                    }
                  }

                  /* Rust-style auto-deref for smart pointers: if the member is
                   * not on the record itself, follow operator* chains. */
                  if (!nextInstanceType) {
                    const DeclNode *derefDecl = getAutoDerefTarget(rDecl);
                    int derefDepth = 0;
                    while (derefDecl && derefDepth < 8) {
                      llvm::ArrayRef<VarDeclNode *> dFields;
                      if (derefDecl->kind == NodeKind::ClassDecl)
                        dFields =
                            static_cast<const ClassDeclNode *>(derefDecl)
                                ->fields;
                      else if (derefDecl->kind == NodeKind::StructDecl)
                        dFields =
                            static_cast<const StructDeclNode *>(derefDecl)
                                ->fields;
                      else if (derefDecl->kind == NodeKind::UnionDecl)
                        dFields =
                            static_cast<const UnionDeclNode *>(derefDecl)
                                ->fields;

                      for (const auto *f : dFields) {
                        if (f->varName == part) {
                          nextInstanceType = f->type;
                          break;
                        }
                      }
                      if (nextInstanceType)
                        break;
                      derefDecl = getAutoDerefTarget(derefDecl);
                      derefDepth++;
                    }
                  }
                }
              }
            } else if (staticTypeDecl) {
              if (staticTypeDecl->kind == NodeKind::NamespaceDecl) {
                for (const auto *decl : namespaceMembers[currentStaticNs]) {
                  if (decl->kind == NodeKind::VarDecl &&
                      static_cast<const VarDeclNode *>(decl)->varName == part) {
                    nextInstanceType =
                        static_cast<const VarDeclNode *>(decl)->type;
                    break;
                  } else if (decl->kind == NodeKind::ClassDecl &&
                             static_cast<const ClassDeclNode *>(decl)->name ==
                                 part) {
                    nextStaticDecl = decl;
                    break;
                  } else if (decl->kind == NodeKind::StructDecl &&
                             static_cast<const StructDeclNode *>(decl)->name ==
                                 part) {
                    nextStaticDecl = decl;
                    break;
                  } else if (decl->kind == NodeKind::UnionDecl &&
                             static_cast<const UnionDeclNode *>(decl)->name ==
                                 part) {
                    nextStaticDecl = decl;
                    break;
                  } else if (decl->kind == NodeKind::EnumDecl &&
                             static_cast<const EnumDeclNode *>(decl)->name ==
                                 part) {
                    nextStaticDecl = decl;
                    break;
                  } else if (decl->kind == NodeKind::NamespaceDecl &&
                             static_cast<const NamespaceDeclNode *>(decl)
                                     ->name == part) {
                    nextStaticDecl = decl;
                    nextStaticNs =
                        static_cast<const NamespaceDeclNode *>(decl)->fqName;
                    break;
                  }
                }
              } else if (staticTypeDecl->kind == NodeKind::ClassDecl ||
                         staticTypeDecl->kind == NodeKind::StructDecl ||
                         staticTypeDecl->kind == NodeKind::UnionDecl) {
                llvm::ArrayRef<VarDeclNode *> fields;
                if (staticTypeDecl->kind == NodeKind::ClassDecl)
                  fields = static_cast<const ClassDeclNode *>(staticTypeDecl)
                               ->fields;
                else if (staticTypeDecl->kind == NodeKind::StructDecl)
                  fields = static_cast<const StructDeclNode *>(staticTypeDecl)
                               ->fields;
                else if (staticTypeDecl->kind == NodeKind::UnionDecl)
                  fields = static_cast<const UnionDeclNode *>(staticTypeDecl)
                               ->fields;
                for (const auto *f : fields) {
                  if (f->isStatic && f->varName == part) {
                    nextInstanceType = f->type;
                    break;
                  }
                }
              }
            }

            instanceType = nextInstanceType;
            staticTypeDecl = nextStaticDecl;
            currentStaticNs = nextStaticNs;
            if (!instanceType && !staticTypeDecl)
              break;
          }
        }

        if (instanceType) {
          const Type *unqual = instanceType->getUnqualifiedType();
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

          if (unqual->getKind() == TypeKind::Class ||
              unqual->getKind() == TypeKind::Struct ||
              unqual->getKind() == TypeKind::Union) {
            auto *recTy = static_cast<const RecordType *>(unqual);
            const DeclNode *rDecl = recTy->getDeclaration();
            if (!rDecl) {
              auto findRec = [&](const std::vector<const DeclNode *> &decls) {
                for (const auto *decl : decls) {
                  if ((decl->kind == NodeKind::ClassDecl &&
                       static_cast<const ClassDeclNode *>(decl)->name ==
                           recTy->getName()) ||
                      (decl->kind == NodeKind::StructDecl &&
                       static_cast<const StructDeclNode *>(decl)->name ==
                           recTy->getName()) ||
                      (decl->kind == NodeKind::UnionDecl &&
                       static_cast<const UnionDeclNode *>(decl)->name ==
                           recTy->getName())) {
                    return decl;
                  }
                }
                return (const DeclNode *)nullptr;
              };
              rDecl = findRec(rootGlobals);
              if (!rDecl) {
                for (const auto &pair : namespaceMembers) {
                  rDecl = findRec(pair.second);
                  if (rDecl)
                    break;
                }
              }
            }

            if (rDecl) {
              llvm::ArrayRef<VarDeclNode *> fields;
              llvm::ArrayRef<FunctionDeclNode *> methods;
              if (rDecl->kind == NodeKind::ClassDecl) {
                fields = static_cast<const ClassDeclNode *>(rDecl)->fields;
                methods = static_cast<const ClassDeclNode *>(rDecl)->methods;
              } else if (rDecl->kind == NodeKind::StructDecl) {
                fields = static_cast<const StructDeclNode *>(rDecl)->fields;
                methods = static_cast<const StructDeclNode *>(rDecl)->methods;
              } else if (rDecl->kind == NodeKind::UnionDecl) {
                fields = static_cast<const UnionDeclNode *>(rDecl)->fields;
                methods = static_cast<const UnionDeclNode *>(rDecl)->methods;
              }

              for (const auto *f : fields) {
                if (!f->isStatic) {
                  std::string detail = f->type ? f->type->toString() : "auto";
                  addCompletion(std::string(f->varName), 5, detail,
                                std::string(f->docString));
                }
              }
              for (const auto *m : methods) {
                if (!m->isStatic && !m->name.starts_with("operator")) {
                  std::string detail =
                      m->returnType ? m->returnType->toString() : "auto";
                  addCompletion(std::string(m->name), 2, detail,
                                std::string(m->docString));
                }
              }

              /* Rust-style auto-deref: also suggest the pointee's members for
               * smart pointers (unique_ptr/shared_ptr/etc.). */
              const DeclNode *derefDecl = getAutoDerefTarget(rDecl);
              int derefDepth = 0;
              while (derefDecl && derefDepth < 8) {
                llvm::ArrayRef<VarDeclNode *> dFields;
                llvm::ArrayRef<FunctionDeclNode *> dMethods;
                if (derefDecl->kind == NodeKind::ClassDecl) {
                  dFields = static_cast<const ClassDeclNode *>(derefDecl)
                                ->fields;
                  dMethods = static_cast<const ClassDeclNode *>(derefDecl)
                                 ->methods;
                } else if (derefDecl->kind == NodeKind::StructDecl) {
                  dFields = static_cast<const StructDeclNode *>(derefDecl)
                                ->fields;
                  dMethods = static_cast<const StructDeclNode *>(derefDecl)
                                 ->methods;
                } else if (derefDecl->kind == NodeKind::UnionDecl) {
                  dFields = static_cast<const UnionDeclNode *>(derefDecl)
                                ->fields;
                  dMethods = static_cast<const UnionDeclNode *>(derefDecl)
                                 ->methods;
                }

                for (const auto *f : dFields) {
                  if (!f->isStatic) {
                    std::string detail =
                        f->type ? f->type->toString() : "auto";
                    addCompletion(std::string(f->varName), 5, detail,
                                  std::string(f->docString));
                  }
                }
                for (const auto *m : dMethods) {
                  if (!m->isStatic && !m->name.starts_with("operator")) {
                    std::string detail =
                        m->returnType ? m->returnType->toString() : "auto";
                    addCompletion(std::string(m->name), 2, detail,
                                  std::string(m->docString));
                  }
                }
                derefDecl = getAutoDerefTarget(derefDecl);
                derefDepth++;
              }
            }
          }
        } else if (staticTypeDecl) {
          if (staticTypeDecl->kind == NodeKind::NamespaceDecl) {
            for (const auto *decl : namespaceMembers[currentStaticNs]) {
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
              } else if (decl->kind == NodeKind::NamespaceDecl) {
                auto *n = static_cast<const NamespaceDeclNode *>(decl);
                addCompletion(std::string(n->name), 9, "namespace",
                              std::string(n->docString));
              }
            }
          } else if (staticTypeDecl->kind == NodeKind::ClassDecl ||
                     staticTypeDecl->kind == NodeKind::StructDecl ||
                     staticTypeDecl->kind == NodeKind::UnionDecl) {
            llvm::ArrayRef<VarDeclNode *> fields;
            llvm::ArrayRef<FunctionDeclNode *> methods;
            if (staticTypeDecl->kind == NodeKind::ClassDecl) {
              fields =
                  static_cast<const ClassDeclNode *>(staticTypeDecl)->fields;
              methods =
                  static_cast<const ClassDeclNode *>(staticTypeDecl)->methods;
            } else if (staticTypeDecl->kind == NodeKind::StructDecl) {
              fields =
                  static_cast<const StructDeclNode *>(staticTypeDecl)->fields;
              methods =
                  static_cast<const StructDeclNode *>(staticTypeDecl)->methods;
            } else if (staticTypeDecl->kind == NodeKind::UnionDecl) {
              fields =
                  static_cast<const UnionDeclNode *>(staticTypeDecl)->fields;
              methods =
                  static_cast<const UnionDeclNode *>(staticTypeDecl)->methods;
            }
            for (const auto *f : fields) {
              if (f->isStatic) {
                std::string detail = f->type ? f->type->toString() : "auto";
                addCompletion(std::string(f->varName), 5, detail,
                              std::string(f->docString));
              }
            }
            for (const auto *m : methods) {
              if (m->isStatic && !m->name.starts_with("operator")) {
                std::string detail =
                    m->returnType ? m->returnType->toString() : "auto";
                addCompletion(std::string(m->name), 2, detail,
                              std::string(m->docString));
              }
            }
          } else if (staticTypeDecl->kind == NodeKind::EnumDecl) {
            auto *e = static_cast<const EnumDeclNode *>(staticTypeDecl);
            for (const auto *em : e->members) {
              addCompletion(std::string(em->name), 20, "enum member",
                            std::string(em->docString));
            }
          }
        }
      } else if (!isDotCompletion) {
        if (collector.closestFunc) {
          for (const auto *p : collector.closestFunc->params) {
            if (p->name == "this")
              continue;
            std::string detail = p->type ? p->type->toString() : "auto";
            addCompletion(std::string(p->name), 6, detail,
                          std::string(p->docString));
          }
        }
        for (const auto *l : collector.locals) {
          std::string detail = l->type ? l->type->toString() : "auto";
          addCompletion(std::string(l->varName), 6, detail,
                        std::string(l->docString));
        }

        auto addDeclItems = [&](const std::vector<const DeclNode *> &decls) {
          for (const auto *decl : decls) {
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
            } else if (decl->kind == NodeKind::TypedefDecl) {
              auto *t = static_cast<const TypedefDeclNode *>(decl);
              addCompletion(std::string(t->aliasName), 8, "typedef",
                            std::string(t->docString));
            } else if (decl->kind == NodeKind::AnnotationDecl) {
              auto *a = static_cast<const AnnotationDeclNode *>(decl);
              addCompletion(std::string(a->name), 8, "annotation",
                            std::string(a->docString));
            } else if (decl->kind == NodeKind::NamespaceDecl) {
              auto *n = static_cast<const NamespaceDeclNode *>(decl);
              addCompletion(std::string(n->name), 9, "namespace",
                            std::string(n->docString));
            }
          }
        };

        addDeclItems(rootGlobals);
        if (!collector.currentNamespace.empty()) {
          addDeclItems(namespaceMembers[collector.currentNamespace]);
        }
        for (const auto &u : collector.activeUsings) {
          addDeclItems(namespaceMembers[u]);
        }
      }
    }
  }

  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", items}});
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
        } else if (dep["name"]) {
          /* Registry dependency (yip): resolve from the local package cache,
           * same layout ProjectBuilder uses: ~/.utopia/cache/yip/packages */
          std::string depName = dep["name"].as<std::string>();
          std::string depVersion =
              dep["version"] ? dep["version"].as<std::string>() : "";

          const char *homeEnv = std::getenv("HOME");
          std::string homeDir = homeEnv ? homeEnv : "";
          if (homeDir.empty()) {
            const char *userProfileEnv = std::getenv("USERPROFILE");
            homeDir = userProfileEnv ? userProfileEnv : "";
          }

          std::filesystem::path cacheRoot = std::filesystem::path(homeDir) /
                                            ".utopia" / "cache" / "yip" /
                                            "packages";
          std::filesystem::path pkgCacheDir = cacheRoot / depName;
          std::filesystem::path resolvedPath = pkgCacheDir / depVersion;

          if (depVersion.empty() || depVersion == "latest" ||
              depVersion == "any" || !std::filesystem::exists(resolvedPath)) {
            if (std::filesystem::exists(pkgCacheDir) &&
                std::filesystem::is_directory(pkgCacheDir)) {
              for (const auto &entry :
                   std::filesystem::directory_iterator(pkgCacheDir)) {
                if (entry.is_directory()) {
                  resolvedPath = entry.path();
                  break;
                }
              }
            }
          }

          std::filesystem::path depYaml = resolvedPath / "build.yaml";
          loadPackagesLSP(depYaml, packages, includeDirs, visited);
        }
      }
    }
  } catch (...) {
    /* Silently ignore YAML parsing errors in LSP to prevent crashes */
  }
}

/* Applies the async runtime configuration: enabled by default, or
 * 'async: false' in the project's build.yaml. The prelude's Future class
 * is guarded by the UTOPIA_ASYNC macro, so it must be defined exactly like
 * the compiler driver does. */
void applyAsyncConfig(ModuleLoaderConfig &modConfig,
                      const std::filesystem::path &projRoot) {
  if (!projRoot.empty()) {
    std::filesystem::path manifest = projRoot / "build.yaml";
    if (std::filesystem::exists(manifest)) {
      try {
        YAML::Node root = YAML::LoadFile(manifest.string());
        if (root["build"] && root["build"]["async"]) {
          modConfig.asyncEnabled = root["build"]["async"].as<bool>();
        }
      } catch (...) {
        /* Malformed build.yaml: keep the default. */
      }
    }
  }
  if (modConfig.asyncEnabled) {
    modConfig.definedMacros.insert("UTOPIA_ASYNC");
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

    applyAsyncConfig(modConfig, projRoot);

    std::lock_guard<std::mutex> lock(cacheMutex);
    projectConfigCache[projRootStr] = modConfig;
  }

  /* build.utp files load the builder API through the module loader. */
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
  } catch (const std::exception &e) {
    /* Analysis errors must never kill the server: report diagnostics from
     * whatever was parsed and keep the document state usable. */
    std::cerr << "[LSP] Analysis failed for " << uri << ": " << e.what()
              << "\n";
  } catch (...) {
    std::cerr << "[LSP] Analysis failed for " << uri
              << " (unknown error).\n";
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

      applyAsyncConfig(modConfig, projRoot);

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
  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
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
    auto tokensList = lexer.tokenize();
    for (size_t i = 0; i < tokensList.size(); ++i) {
      const auto &tok = tokensList[i];
      if (tok.type == TokenType::IDENTIFIER) {
        size_t tokAbsIdx = foundIdx + (tok.value.data() - rawTypeStr.data());

        int absLine = 1;
        int absCol = 1;
        for (size_t j = 0; j < tokAbsIdx; ++j) {
          if (docText[j] == '\n') {
            absLine++;
            absCol = 1;
          } else {
            absCol++;
          }
        }

        int tokenClass = 3;
        bool isNamespace = false;
        if (i + 1 < tokensList.size() &&
            tokensList[i + 1].type == TokenType::DOT) {
          isNamespace = true;
        }

        if (isNamespace) {
          tokenClass = 12;
        } else {
          std::string chain = "";
          for (size_t k = 0; k <= i; ++k) {
            if (tokensList[k].type == TokenType::IDENTIFIER) {
              if (!chain.empty())
                chain += ".";
              chain += tokensList[k].value;
            }
          }

          if (astCtx->getRecordType(chain)) {
            auto *recTy = astCtx->getRecordType(chain);
            if (recTy->getKind() == TypeKind::Struct)
              tokenClass = 1;
            else
              tokenClass = 0;
          } else if (astCtx->getEnumTypeByName(chain)) {
            tokenClass = 2;
          } else if (astCtx->getNamespace(chain)) {
            tokenClass = 12;
          } else if (astCtx->getRecordType(tok.value)) {
            auto *recTy = astCtx->getRecordType(tok.value);
            if (recTy->getKind() == TypeKind::Struct)
              tokenClass = 1;
            else
              tokenClass = 0;
          } else if (astCtx->getEnumTypeByName(tok.value)) {
            tokenClass = 2;
          } else if (astCtx->getNamespace(tok.value)) {
            tokenClass = 12;
          }
        }

        addToken(absLine - 1, absCol - 1, tok.value.length(), tokenClass, 0);
      }
    }
  }

  void visit(const NamespaceDeclNode *n) {
    auto loc = getExactNameLocation(docText, n);
    addToken(loc.line, loc.col, loc.length, 12, 0);
    for (auto *s : n->statements)
      dispatch(s);
  }

  void visit(const UsingNode *n) {
    highlightTypeString(n->name, n->line, n->column);
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
               n->resolvedDecl->kind == NodeKind::TypedefDecl ||
               n->resolvedDecl->kind == NodeKind::AnnotationDecl)
        type = 3;
      else if (n->resolvedDecl->kind == NodeKind::NamespaceDecl)
        type = 12;
      else if (n->resolvedDecl->kind == NodeKind::FunctionDecl) {
        auto fn = static_cast<const FunctionDeclNode *>(n->resolvedDecl);
        if (fn->parentRecord && fn->name == fn->parentRecord->getName()) {
          const DeclNode *recDecl = fn->parentRecord->getDeclaration();
          if (recDecl) {
            if (recDecl->kind == NodeKind::ClassDecl)
              type = 0;
            else if (recDecl->kind == NodeKind::StructDecl)
              type = 1;
            else if (recDecl->kind == NodeKind::UnionDecl)
              type = 3;
            else
              type = 0;
          } else {
            type = 0;
          }
        } else {
          type = 4;
        }
      } else if (n->resolvedDecl->kind == NodeKind::ParamDecl)
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
    } else if (n->resolvedDecl) {
      if (n->resolvedDecl->kind == NodeKind::ClassDecl)
        type = 0;
      else if (n->resolvedDecl->kind == NodeKind::StructDecl)
        type = 1;
      else if (n->resolvedDecl->kind == NodeKind::EnumDecl)
        type = 2;
      else if (n->resolvedDecl->kind == NodeKind::UnionDecl ||
               n->resolvedDecl->kind == NodeKind::TypedefDecl ||
               n->resolvedDecl->kind == NodeKind::AnnotationDecl)
        type = 3;
      else if (n->resolvedDecl->kind == NodeKind::NamespaceDecl)
        type = 12;
      else if (n->resolvedDecl->kind == NodeKind::FunctionDecl) {
        auto fn = static_cast<const FunctionDeclNode *>(n->resolvedDecl);
        if (fn->parentRecord && fn->name == fn->parentRecord->getName()) {
          const DeclNode *recDecl = fn->parentRecord->getDeclaration();
          if (recDecl) {
            if (recDecl->kind == NodeKind::ClassDecl)
              type = 0;
            else if (recDecl->kind == NodeKind::StructDecl)
              type = 1;
            else if (recDecl->kind == NodeKind::UnionDecl)
              type = 3;
            else
              type = 0;
          } else {
            type = 0;
          }
        } else {
          type = 4;
        }
      }
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
  void visit(const AwaitExprNode *n) { dispatch(n->expr); }
  void visit(const LambdaNode *n) {
    for (auto *p : n->params)
      dispatch(p);
    if (n->isExpressionBody && n->exprBody) {
      dispatch(n->exprBody);
    } else if (n->body) {
      dispatch(n->body);
    }
  }
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

      /* Lexical pass to inject Semantic Tokens for primitive types and
       * keywords. The AST Visitor explicitly skips built-in types because they
       * lack a concrete declaration node. This guarantees that all primitive
       * types and special keywords are highlighted correctly by the LSP,
       * overriding any incomplete static TextMate grammars. */
      Lexer lexer(doc.text);
      for (const auto &tok : lexer.tokenize()) {
        if (tok.type == TokenType::TYPE_KW) {
          visitor.addToken(tok.line > 0 ? tok.line - 1 : 0,
                           tok.column > 0 ? tok.column - 1 : 0,
                           tok.value.length(), 3, 0); /* 3 represents "type" */
        } else if (tok.type == TokenType::VAR_KW ||
                   tok.type == TokenType::CONST_KW ||
                   tok.type == TokenType::STATIC_KW ||
                   tok.type == TokenType::PUBLIC_KW ||
                   tok.type == TokenType::PRIVATE_KW ||
                   tok.type == TokenType::PROTECTED_KW ||
                   tok.type == TokenType::ABSTRACT_KW ||
                   tok.type == TokenType::REQUIRED_KW ||
                   tok.type == TokenType::NAMESPACE_KW ||
                   tok.type == TokenType::EXTENDS_KW ||
                   tok.type == TokenType::IMPLEMENTS_KW ||
                   tok.type == TokenType::SUPER_KW ||
                   tok.type == TokenType::USING_KW) {
          visitor.addToken(tok.line > 0 ? tok.line - 1 : 0,
                           tok.column > 0 ? tok.column - 1 : 0,
                           tok.value.length(), 11,
                           0); /* 11 represents "keyword" */
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
      {{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", {{"data", data}}}});
}

} // namespace utopia::lsp

int main() {
  std::thread worker(utopia::lsp::workerThread);

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.find("Content-Length:") == 0) {
      int len = 0;
      try {
        len = std::stoi(line.substr(15));
      } catch (...) {
        std::cerr << "[LSP] Invalid Content-Length header; skipping message.\n";
        continue;
      }
      if (len <= 0 || len > (1 << 26)) {
        /* Malformed or absurd lengths must not block or crash the server. */
        std::cerr << "[LSP] Bogus Content-Length (" << len
                  << "); skipping message.\n";
        continue;
      }
      while (std::getline(std::cin, line) && (line != "\r" && !line.empty()))
        ;

      std::vector<char> buf(len);
      std::cin.read(buf.data(), len);

      try {
        auto req = json::parse(std::string(buf.begin(), buf.end()));
        std::string method = req["method"];

        if (method == "initialize") {
          utopia::lsp::sendResponse(
              {{"jsonrpc", "2.0"},
               {"id", utopia::lsp::requestId(req)},
               {"result",
                {{"capabilities",
                  {{"textDocumentSync", 1},
                   {"hoverProvider", true},
                   {"definitionProvider", true},
                   {"completionProvider",
                    {{"triggerCharacters", {".", "@"}}}},
                   {"signatureHelpProvider",
                    {{"triggerCharacters", {"(", ","}}}},
                   {"documentFormattingProvider", true},
                   {"semanticTokensProvider",
                    {{"legend",
                      {{"tokenTypes",
                        {"class", "struct", "enum", "type", "function",
                         "method", "property", "variable", "parameter",
                         "enumMember", "macro", "keyword", "namespace"}},
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
                  : req["params"]["contentChanges"][0]["text"]
                        .get<std::string>();

          {
            std::lock_guard<std::mutex> lock(utopia::lsp::workerMutex);
            utopia::lsp::pendingUri = uri;
            utopia::lsp::pendingText = std::move(text);
            utopia::lsp::hasPendingChange = true;
          }
          utopia::lsp::workerCV.notify_one();
        } else if (method == "shutdown") {
          /* Client asks to shut down, server must return a null result */
          utopia::lsp::sendResponse({{"jsonrpc", "2.0"},
                                     {"id", utopia::lsp::requestId(req)},
                                     {"result", nullptr}});
        } else if (method == "exit") {
          utopia::lsp::isRunning = false;
          utopia::lsp::workerCV.notify_one();
          worker.join();
          return 0;
        }
      } catch (const std::exception &e) {
        /* A malformed or unexpected request must never take the server
         * down: log a clear error and keep serving. */
        std::cerr << "[LSP] Failed to process request: " << e.what() << "\n";
      } catch (...) {
        std::cerr << "[LSP] Failed to process request (unknown error).\n";
      }
    }
  }

  utopia::lsp::isRunning = false;
  utopia::lsp::workerCV.notify_one();
  worker.join();
  return 0;
}