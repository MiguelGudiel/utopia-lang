#include "LspCore.hpp"
#include "utopia/Common/Logger.hpp"

int main() {
  /* The compiler's Logger writes to std::cout, which is the LSP protocol
   * stream; any stray log line would corrupt the framing for the client.
   * The server reports its own errors through stderr, so only errors (or
   * nothing) may reach the console. */
  utopia::Logger::setLevel(utopia::LogLevel::Error);
  utopia::lsp::runServer();
}
