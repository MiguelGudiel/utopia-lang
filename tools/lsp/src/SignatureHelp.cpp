#include "LspCore.hpp"

namespace utopia::lsp {

void handleSignatureHelp(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = nullptr;

  DocumentState doc;
  if (documents.get(uri, doc) && doc.ast) {
    SearchVisitor searcher(line, col, &doc.text);
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

            std::string pLabel;
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

} // namespace utopia::lsp
