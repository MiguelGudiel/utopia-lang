#include "LspCore.hpp"
#include <algorithm>
#include <iostream>
#include <cerrno>
#include <mutex>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace utopia::lsp {

namespace {
/* Serializes concurrent writes from the analysis worker and the request
 * handlers; the LSP framing has no atomicity guarantee otherwise. */
std::mutex stdoutMutex;
} // namespace

json requestId(const json &req) {
  if (req.contains("id"))
    return req["id"];
  return nullptr;
}

void sendResponse(const json &res) {
  std::string content =
      res.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  std::string framed = "Content-Length: " + std::to_string(content.length()) +
                       "\r\n\r\n" + content;

  std::lock_guard<std::mutex> lock(stdoutMutex);
#if defined(_WIN32)
  /* POSIX write(2) is unavailable; stdio is fine here. */
  std::cout << framed << std::flush;
#else
  /* Write straight to fd 1 instead of std::cout << std::flush. The analysis
   * worker responds from a non-main thread, and libstdc++'s synced stdio
   * buffer is shared with the compiler's Logger (which also writes to
   * std::cout): a flush from one thread does not reliably push the other's
   * buffered output to the pipe, so diagnostics could sit unflushed for a
   * minute or more while the client waits. A raw write(2) of the complete,
   * mutex-guarded frame cannot be delayed by stdio state. */
  ssize_t written = 0;
  while (written < (ssize_t)framed.size()) {
    ssize_t n = ::write(STDOUT_FILENO, framed.data() + written,
                        framed.size() - (size_t)written);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    written += n;
  }
#endif
}

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

} // namespace utopia::lsp
