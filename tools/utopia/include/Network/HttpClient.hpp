#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace utopia {

struct HttpOptions {
  std::string baseUrl;
  std::map<std::string, std::string> defaultHeaders;
  long timeoutSeconds = 30;
  bool enableHttp2 = true;
};

struct HttpResponse {
  long statusCode = 0;
  std::string body;
  std::map<std::string, std::string> headers;
  std::string error;
  bool isSuccess() const { return statusCode >= 200 && statusCode < 300; }
};

struct HttpRequest {
  std::string path;
  std::string method = "GET";
  std::string body;
  std::map<std::string, std::string> headers;
  std::function<void(HttpResponse)> onComplete;
};

class HttpClient {
public:
  explicit HttpClient(const HttpOptions &options);
  ~HttpClient();

  HttpClient(const HttpClient &) = delete;
  HttpClient &operator=(const HttpClient &) = delete;

  void get(const std::string &path,
           std::function<void(HttpResponse)> onComplete);

  void post(const std::string &path, const std::string &body,
            std::function<void(HttpResponse)> onComplete);

  void put(const std::string &path, const std::string &body,
           const std::map<std::string, std::string> &headers,
           std::function<void(HttpResponse)> onComplete);

  void executeAll();

private:
  HttpOptions m_options;
  void *m_multiHandle;
  std::vector<std::pair<void *, HttpRequest>> m_pendingRequests;

  void setupEasyHandle(void *easyHandle, const HttpRequest &request,
                       std::string *responseBody);
};

} // namespace utopia