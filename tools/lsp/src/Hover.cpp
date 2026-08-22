#include "LspCore.hpp"
#include <cctype>

namespace utopia::lsp {

namespace {

/* The type-name component under the cursor within 'rawTypeStr' (found near
 * nodeLine/nodeCol in the document). Returns the dotted chain ('Map.String')
 * accumulated up to the hovered identifier, or "" when not on a type
 * identifier. */
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
  std::string chain;
  for (auto &tok : toks) {
    if (tok.type != TokenType::IDENTIFIER)
      continue;
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
  return "";
}

} // namespace

void handleHover(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = nullptr;

  DocumentState doc;
  if (documents.get(uri, doc) && doc.ast) {
    SearchVisitor searcher(line, col, &doc.text);
    const ASTNode *node = searcher.find(doc.ast);

    if (node) {
      std::string hoverText;
      const DeclNode *declTarget = nullptr;

      LocalVarCollector collector(line);
      collector.dispatch(doc.ast);

      auto resolveWithCollector = [&](const std::string &name) {
        return ::utopia::lsp::resolveWithCollector(name, collector,
                                                   doc.sema.get(), doc.ast);
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
      } else if (auto *isNode = llvm::dyn_cast_or_null<IsExprNode>(node)) {
        if (isNode && isNode->targetType) {
          std::string hoveredChain;
          if (!isNode->rawTargetTypeStr.empty()) {
            hoveredChain = getHoveredTypeComponent(
                doc.text, isNode->rawTargetTypeStr, isNode->line,
                isNode->column, line, col);
          }
          if (!hoveredChain.empty() && doc.sema) {
            declTarget = resolveWithCollector(hoveredChain);
            if (declTarget)
              hoverText = getHoverTextForDecl(declTarget);
          }
          if (hoverText.empty()) {
            if (auto typeDecl = getTypeDeclaration(isNode->targetType)) {
              declTarget = typeDecl;
              hoverText = getHoverTextForDecl(typeDecl);
            } else {
              hoverText =
                  "```utopia\n" + isNode->targetType->toString() + "\n```";
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

} // namespace utopia::lsp
