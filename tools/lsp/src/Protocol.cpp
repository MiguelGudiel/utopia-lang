#include "LspCore.hpp"
#include <algorithm>
#include <iostream>
#include <mutex>

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

  std::lock_guard<std::mutex> lock(stdoutMutex);
  std::cout << "Content-Length: " << content.length() << "\r\n\r\n"
            << content << std::flush;
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
