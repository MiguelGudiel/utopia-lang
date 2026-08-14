#include "Network/HttpClient.hpp"
#include <curl/curl.h>
#include <iostream>

namespace utopia {

static size_t WriteCallback(void *contents, size_t size, size_t nmemb,
                            void *userp) {
  size_t realsize = size * nmemb;
  auto *str = static_cast<std::string *>(userp);
  str->append(static_cast<char *>(contents), realsize);
  return realsize;
}

HttpClient::HttpClient(const HttpOptions &options) : m_options(options) {
  curl_global_init(CURL_GLOBAL_ALL);
  m_multiHandle = curl_multi_init();

  if (m_options.enableHttp2) {
    curl_multi_setopt(m_multiHandle, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
  }
}

HttpClient::~HttpClient() {
  if (m_multiHandle) {
    curl_multi_cleanup(m_multiHandle);
  }
  curl_global_cleanup();
}

void HttpClient::get(const std::string &path,
                     std::function<void(HttpResponse)> onComplete) {
  HttpRequest req;
  req.path = path;
  req.onComplete = std::move(onComplete);

  void *easyHandle = curl_easy_init();
  m_pendingRequests.emplace_back(easyHandle, std::move(req));
}

void HttpClient::post(const std::string &path, const std::string &body,
                      std::function<void(HttpResponse)> onComplete) {
  HttpRequest req;
  req.path = path;
  req.method = "POST";
  req.body = body;
  req.onComplete = std::move(onComplete);

  void *easyHandle = curl_easy_init();
  m_pendingRequests.emplace_back(easyHandle, std::move(req));
}

void HttpClient::put(const std::string &path, const std::string &body,
                     const std::map<std::string, std::string> &headers,
                     std::function<void(HttpResponse)> onComplete) {
  HttpRequest req;
  req.path = path;
  req.method = "PUT";
  req.body = body;
  req.headers = headers;
  req.onComplete = std::move(onComplete);

  void *easyHandle = curl_easy_init();
  m_pendingRequests.emplace_back(easyHandle, std::move(req));
}

void HttpClient::setupEasyHandle(void *easyHandle, const HttpRequest &request,
                                 std::string *responseBody) {
  std::string fullUrl = m_options.baseUrl + request.path;

  curl_easy_setopt(easyHandle, CURLOPT_URL, fullUrl.c_str());
  curl_easy_setopt(easyHandle, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(easyHandle, CURLOPT_WRITEDATA, responseBody);

  curl_easy_setopt(easyHandle, CURLOPT_CONNECTTIMEOUT,
                   m_options.connectTimeoutSeconds);

  curl_easy_setopt(easyHandle, CURLOPT_TIMEOUT, m_options.timeoutSeconds);

  curl_easy_setopt(easyHandle, CURLOPT_FOLLOWLOCATION, 1L);

#ifdef _WIN32
  curl_easy_setopt(easyHandle, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif

  if (request.method == "POST") {
    curl_easy_setopt(easyHandle, CURLOPT_POST, 1L);
    if (!request.body.empty()) {
      curl_easy_setopt(easyHandle, CURLOPT_POSTFIELDS, request.body.data());
      curl_easy_setopt(easyHandle, CURLOPT_POSTFIELDSIZE, request.body.size());
    }
  } else if (request.method == "PUT") {
    curl_easy_setopt(easyHandle, CURLOPT_CUSTOMREQUEST, "PUT");
    if (!request.body.empty()) {
      curl_easy_setopt(easyHandle, CURLOPT_POSTFIELDS, request.body.data());
      curl_easy_setopt(easyHandle, CURLOPT_POSTFIELDSIZE, request.body.size());
    }
  }

  if (m_options.enableHttp2) {
    if (fullUrl.starts_with("http://")) {
      curl_easy_setopt(easyHandle, CURLOPT_HTTP_VERSION,
                       CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
    } else {
      curl_easy_setopt(easyHandle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
    }
  }

  struct curl_slist *headers = nullptr;
  for (const auto &[key, value] : m_options.defaultHeaders) {
    std::string headerStr = key + ": " + value;
    headers = curl_slist_append(headers, headerStr.c_str());
  }
  for (const auto &[key, value] : request.headers) {
    std::string headerStr = key + ": " + value;
    headers = curl_slist_append(headers, headerStr.c_str());
  }

  if (headers) {
    curl_easy_setopt(easyHandle, CURLOPT_HTTPHEADER, headers);
  }
}

void HttpClient::executeAll() {
  std::vector<std::string> responseBodies(m_pendingRequests.size());

  for (size_t i = 0; i < m_pendingRequests.size(); ++i) {
    setupEasyHandle(m_pendingRequests[i].first, m_pendingRequests[i].second,
                    &responseBodies[i]);
    curl_multi_add_handle(m_multiHandle, m_pendingRequests[i].first);
  }

  int stillRunning = 0;
  do {
    CURLMcode mc = curl_multi_perform(m_multiHandle, &stillRunning);
    if (stillRunning) {
      curl_multi_poll(m_multiHandle, nullptr, 0, 1000, nullptr);
    }
  } while (stillRunning);

  int msgsLeft = 0;
  CURLMsg *msg = nullptr;
  while ((msg = curl_multi_info_read(m_multiHandle, &msgsLeft))) {
    if (msg->msg == CURLMSG_DONE) {
      void *easyHandle = msg->easy_handle;

      for (size_t i = 0; i < m_pendingRequests.size(); ++i) {
        if (m_pendingRequests[i].first == easyHandle) {
          HttpResponse response;
          curl_easy_getinfo(easyHandle, CURLINFO_RESPONSE_CODE,
                            &response.statusCode);

          if (msg->data.result == CURLE_OK) {
            response.body = std::move(responseBodies[i]);
          } else {
            response.error = curl_easy_strerror(msg->data.result);
          }

          m_pendingRequests[i].second.onComplete(std::move(response));
          break;
        }
      }
      curl_multi_remove_handle(m_multiHandle, easyHandle);
      curl_easy_cleanup(easyHandle);
    }
  }

  m_pendingRequests.clear();
}

} // namespace utopia