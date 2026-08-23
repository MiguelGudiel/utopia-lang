#include "LspCore.hpp"
#include <chrono>
#include <iostream>

namespace utopia::lsp {

DocumentManager documents;

namespace {

std::mutex workerMutex;
std::condition_variable workerCV;
std::condition_variable doneCV;
std::string pendingUri;
std::string pendingText;
bool hasPendingChange = false;
bool isProcessing = false;
bool forceProcess = false;
std::atomic<bool> isRunning{true};
/* When the last change was announced; the worker only starts an analysis
 * once the document has been quiet for kQuietAnalysisMs. A full analysis
 * re-parses and re-checks the whole dependency graph (prelude + imports),
 * which takes seconds, so without this gate continuous typing would chain
 * one analysis into the next and pin a core for the whole session. */
std::chrono::steady_clock::time_point lastChangeTime =
    std::chrono::steady_clock::now();
constexpr auto kQuietAnalysisMs = std::chrono::milliseconds(300);

void workerThread() {
  while (isRunning) {
    std::string uri, text;
    bool pollManifests = false;
    {
      std::unique_lock<std::mutex> lock(workerMutex);
      /* Wake periodically even when idle so the manifest poller runs: the
       * project configuration must refresh when build.yaml changes on disk
       * even if the client sends no notification at all. */
      workerCV.wait_for(lock, std::chrono::seconds(1), [] {
        return hasPendingChange || forceProcess || !isRunning;
      });
      if (!isRunning)
        break;

      if (!hasPendingChange && !forceProcess) {
        /* Idle tick: look for build.yaml changes. */
        pollManifests = true;
      } else {
        /* Wait until the document has been quiet for the debounce window.
         * wait_until is used instead of a notified-reset wait_for: the
         * notifications that announce each keystroke would otherwise keep
         * resetting the timer, so a fast typist could starve the worker
         * forever — and with 'hasPendingChange' never cleared, every
         * syncWorker() call (completion, hover, semantic tokens, ...)
         * would block indefinitely. */
        while (!forceProcess) {
          auto quietUntil = lastChangeTime + kQuietAnalysisMs;
          if (std::chrono::steady_clock::now() >= quietUntil)
            break;
          workerCV.wait_until(lock, quietUntil);
        }

        forceProcess = false;
        hasPendingChange = false;
        uri = pendingUri;
        text = std::move(pendingText);
        isProcessing = true;
      }
    }

    if (pollManifests) {
      documents.pollManifests();
      continue;
    }

    if (!uri.empty()) {
      documents.processFile(uri, text);
    }

    {
      std::lock_guard<std::mutex> lock(workerMutex);
      isProcessing = false;
      doneCV.notify_all();
    }
  }
}

/* The request dispatcher; returns false only for 'exit'. */
bool dispatchRequest(const json &req) {
  std::string method = req["method"];

  if (method == "initialize") {
    sendResponse(
        {{"jsonrpc", "2.0"},
         {"id", requestId(req)},
         {"result",
          {{"capabilities",
            {{"textDocumentSync",
              {{"openClose", true}, {"change", 1}, {"save", true}}},
             {"hoverProvider", true},
             {"definitionProvider", true},
             {"typeDefinitionProvider", true},
             {"implementationProvider", true},
             {"referencesProvider", true},
             {"documentHighlightProvider", true},
             {"documentSymbolProvider", true},
             {"workspaceSymbolProvider", true},
             {"foldingRangeProvider", true},
              {"documentLinkProvider", {{"resolveProvider", false}}},
             {"completionProvider",
              {{"triggerCharacters", {".", "@"}}}},
             {"signatureHelpProvider",
              {{"triggerCharacters", {"(", ","}}}},
             {"documentFormattingProvider", true},
             {"codeActionProvider",
              {{"codeActionKinds", {"quickfix"}}}},
             {"semanticTokensProvider",
              {{"legend",
                {{"tokenTypes",
                  {"class", "struct", "enum", "type", "function", "method",
                   "property", "variable", "parameter", "enumMember",
                   "macro", "keyword", "namespace"}},
                 {"tokenModifiers",
                  {"declaration", "static", "readonly"}}}},
               {"range", false},
               {"full", true}}}}}}}});
  } else if (method == "textDocument/hover") {
    handleHover(req);
  } else if (method == "textDocument/definition") {
    handleDefinition(req);
  } else if (method == "textDocument/typeDefinition") {
    handleTypeDefinition(req);
  } else if (method == "textDocument/implementation") {
    handleImplementation(req);
  } else if (method == "textDocument/references") {
    handleReferences(req);
  } else if (method == "textDocument/documentHighlight") {
    handleDocumentHighlight(req);
  } else if (method == "textDocument/documentSymbol") {
    handleDocumentSymbols(req);
  } else if (method == "workspace/symbol") {
    handleWorkspaceSymbols(req);
  } else if (method == "textDocument/foldingRange") {
    handleFoldingRange(req);
  } else if (method == "textDocument/documentLink") {
    handleDocumentLinks(req);
  } else if (method == "textDocument/completion") {
    handleCompletion(req);
  } else if (method == "textDocument/signatureHelp") {
    handleSignatureHelp(req);
  } else if (method == "textDocument/formatting") {
    handleFormatting(req);
  } else if (method == "textDocument/semanticTokens/full") {
    handleSemanticTokens(req);
  } else if (method == "textDocument/codeAction") {
    handleCodeAction(req);
  } else if (method == "textDocument/didOpen" ||
             method == "textDocument/didChange") {
    std::string uri = req["params"]["textDocument"]["uri"];
    std::string text =
        (method == "textDocument/didOpen")
            ? req["params"]["textDocument"]["text"].get<std::string>()
            : req["params"]["contentChanges"][0]["text"].get<std::string>();

    /* build.yaml is not Utopia source: an edit to the manifest re-reads the
     * project configuration and re-analyzes its open documents instead of
     * parsing YAML as a module. */
    if (uri.ends_with("build.yaml")) {
      documents.onBuildManifestChanged(uri, std::move(text));
    } else {
      requestBackgroundAnalysis(uri, std::move(text));
    }
  } else if (method == "textDocument/didClose") {
    std::string uri = req["params"]["textDocument"]["uri"];
    if (uri.ends_with("build.yaml")) {
      documents.onBuildManifestClosed(uri);
    }
  } else if (method == "textDocument/didSave") {
    std::string uri = req["params"]["textDocument"]["uri"];
    if (uri.ends_with("build.yaml")) {
      documents.onBuildManifestSaved(uri);
    }
  } else if (method == "workspace/didChangeWatchedFiles") {
    for (const auto &change : req["params"]["changes"]) {
      std::string uri = change["uri"].get<std::string>();
      if (uri.ends_with("build.yaml")) {
        documents.onBuildManifestSaved(uri);
      }
    }
  } else if (method == "shutdown") {
    sendResponse({{"jsonrpc", "2.0"},
                  {"id", requestId(req)},
                  {"result", nullptr}});
  } else if (method == "exit") {
    return false;
  }
  return true;
}

} // namespace

void requestBackgroundAnalysis(const std::string &uri, std::string text) {
  {
    std::lock_guard<std::mutex> lock(workerMutex);
    pendingUri = uri;
    pendingText = std::move(text);
    lastChangeTime = std::chrono::steady_clock::now();
    hasPendingChange = true;
  }
  workerCV.notify_one();
}

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

void stopWorker() {
  isRunning = false;
  workerCV.notify_one();
}

void runServer() {
  std::thread worker(workerThread);

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.find("Content-Length:") != 0)
      continue;

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
      if (!dispatchRequest(req)) {
        stopWorker();
        worker.join();
        return;
      }
    } catch (const std::exception &e) {
      /* A malformed or unexpected request must never take the server
       * down: log a clear error and keep serving. */
      std::cerr << "[LSP] Failed to process request: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "[LSP] Failed to process request (unknown error).\n";
    }
  }

  stopWorker();
  worker.join();
}

} // namespace utopia::lsp
