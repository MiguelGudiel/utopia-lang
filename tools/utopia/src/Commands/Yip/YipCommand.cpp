#include "Commands/Yip/YipCommand.hpp"
#include "Core/EnvLoader.hpp"
#include "Network/HttpClient.hpp"
#include "ProjectManager.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
typedef int SOCKET;
#endif

namespace utopia {

YipCommand::YipCommand() {
  m_subcommandsHelp["get"] =
      "Resolve and download dependencies defined in build.yaml";
  m_subcommandsHelp["add"] = "Add a new dependency to build.yaml and fetch it";
  m_subcommandsHelp["publish"] = "Publish the current package to the registry";
  m_subcommandsHelp["login"] =
      "Save your API token (generated from the web dashboard)";
}

std::string YipCommand::getName() const { return "yip"; }

std::string YipCommand::getDescription() const {
  return "Utopia Package Manager (like pub, built like Cargo).";
}

void YipCommand::printYipHelp() const {
  std::cout << "Yip - Utopia Package Manager\n"
            << "Usage: utopia yip <subcommand> [args]\n\n"
            << "Subcommands:\n";
  for (const auto &[cmd, desc] : m_subcommandsHelp) {
    std::cout << "  " << cmd << "\t" << desc << "\n";
  }
}

int YipCommand::execute(const std::vector<std::string> &args,
                        const std::string &executablePath) {
  if (args.empty()) {
    printYipHelp();
    return 1;
  }

  std::string subCommand = args[0];
  std::vector<std::string> subArgs(args.begin() + 1, args.end());

  if (subCommand == "get") {
    return handleGet(subArgs);
  } else if (subCommand == "add") {
    return handleAdd(subArgs);
  } else if (subCommand == "publish") {
    return handlePublish(subArgs);
  } else if (subCommand == "login") {
    return handleLogin(subArgs);
  } else {
    std::cerr << "Unknown yip subcommand: " << subCommand << "\n";
    printYipHelp();
    return 1;
  }
}

std::string YipCommand::getToken() const {
  std::string envToken = EnvLoader::get("YIP_TOKEN");
  if (!envToken.empty())
    return envToken;

  std::string homeDir = EnvLoader::get("HOME");
  if (homeDir.empty())
    homeDir = EnvLoader::get("USERPROFILE");

  std::filesystem::path credsFile =
      std::filesystem::path(homeDir) / ".utopia" / "credentials.json";
  if (std::filesystem::exists(credsFile)) {
    std::ifstream f(credsFile);
    if (f) {
      try {
        nlohmann::json j;
        f >> j;
        if (j.contains("token"))
          return j["token"];
      } catch (...) {
      }
    }
  }
  return "";
}

int YipCommand::handleLogin(const std::vector<std::string> &args) {
  std::string token;

  if (!args.empty()) {
    // Support manual flow for headless environments
    token = args[0];
  } else {
#ifndef UTOPIA_YIP_BASE_URL
#define UTOPIA_YIP_BASE_URL ""
#endif
    std::string baseUrl = UTOPIA_YIP_BASE_URL;
    if (baseUrl.empty()) {
      baseUrl = EnvLoader::get("YIP_BASE_URL", "http://localhost:8080");
    }

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
      std::cerr << "Error: WSAStartup failed.\n";
      return 1;
    }
#endif

    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
      std::cerr << "Error: Could not create socket for local authentication "
                   "server.\n";
      return 1;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
    address.sin_port = 0; // Request dynamic free port from OS

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) ==
        SOCKET_ERROR) {
      std::cerr << "Error: Could not bind to local port.\n";
      return 1;
    }

    socklen_t addr_len = sizeof(address);
    if (getsockname(server_fd, (struct sockaddr *)&address, &addr_len) ==
        SOCKET_ERROR) {
      std::cerr << "Error: Could not resolve local port assignment.\n";
      return 1;
    }

    int port = ntohs(address.sin_port);

    if (listen(server_fd, 1) == SOCKET_ERROR) {
      std::cerr << "Error: Could not start listening on local port.\n";
      return 1;
    }

    std::string authUrl =
        baseUrl + "/auth/github/login?cli_port=" + std::to_string(port);
    std::cout << "Opening browser to authenticate...\n";
    std::cout << "If the browser doesn't open automatically, visit:\n";
    std::cout << "  " << authUrl << "\n\n";
    std::cout << "Waiting for authentication..." << std::flush;

#if defined(_WIN32)
    std::system(("start \"\" \"" + authUrl + "\"").c_str());
#elif defined(__APPLE__)
    std::system(("open \"" + authUrl + "\"").c_str());
#else
    std::system(("xdg-open \"" + authUrl + "\" > /dev/null 2>&1 &").c_str());
#endif

    SOCKET client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd == INVALID_SOCKET) {
      std::cerr
          << "\nError: Failed to accept incoming connection from browser.\n";
      return 1;
    }

    char buffer[4096] = {0};
    int valread = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (valread <= 0) {
      std::cerr << "\nError: No data received from authentication redirect.\n";
      return 1;
    }

    std::string request(buffer);

    // Parse the token from "GET /?token=yip_xxx HTTP/1.1"
    size_t tokenPos = request.find("?token=");
    if (tokenPos != std::string::npos) {
      tokenPos += 7;
      size_t endPos = request.find_first_of(" \r\n&", tokenPos);
      if (endPos != std::string::npos) {
        token = request.substr(tokenPos, endPos - tokenPos);
      }
    }

    std::string html =
        "<html><head><title>Utopia CLI Login</title>"
        "<style>body{font-family:sans-serif;display:flex;justify-content:"
        "center;align-items:center;height:100vh;background:#f9fafb;}"
        ".box{background:#fff;padding:2rem;border-radius:8px;box-shadow:0 4px "
        "6px rgba(0,0,0,0.1);text-align:center;}</style>"
        "</head><body><div class=\"box\"><h2>Authentication "
        "Successful!</h2><p>You can close this tab and return to the "
        "terminal.</p></div>"
        "<script>window.close();</script></body></html>";

    std::string httpResponse = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/html\r\n"
                               "Content-Length: " +
                               std::to_string(html.length()) +
                               "\r\n"
                               "Connection: close\r\n\r\n" +
                               html;

    send(client_fd, httpResponse.c_str(), httpResponse.length(), 0);

#ifdef _WIN32
    closesocket(client_fd);
    closesocket(server_fd);
    WSACleanup();
#else
    close(client_fd);
    close(server_fd);
#endif

    std::cout << "\n";
  }

  if (token.empty()) {
    std::cerr << "Error: Authentication failed. No valid token found in the "
                 "response.\n";
    return 1;
  }

  std::string homeDir = EnvLoader::get("HOME");
  if (homeDir.empty()) {
    homeDir = EnvLoader::get("USERPROFILE");
  }

  std::filesystem::path utopiaDir = std::filesystem::path(homeDir) / ".utopia";
  std::filesystem::create_directories(utopiaDir);

  std::ofstream f(utopiaDir / "credentials.json");
  f << nlohmann::json{{"token", token}}.dump(2);

  std::cout << "\033[1;32m[Success]\033[0m API token saved successfully.\n";
  std::cout << "You are now authenticated and ready to publish packages.\n";

  return 0;
}

int YipCommand::handlePublish(const std::vector<std::string> &args) {
  std::filesystem::path projRoot =
      findProjectRoot(std::filesystem::current_path());
  if (projRoot.empty()) {
    std::cerr << "Fatal: build.yaml not found in the current tree.\n";
    return 1;
  }

  ProjectConfig config;
  try {
    config = parseBuildManifest(projRoot / "build.yaml");
  } catch (const std::exception &e) {
    std::cerr << "Manifest Error: " << e.what() << "\n";
    return 1;
  }

  if (config.version.empty() || config.version == "0.0.0") {
    std::cerr << "Error: Valid project version is missing in build.yaml.\n";
    return 1;
  }

  std::string token = getToken();
  if (token.empty()) {
    std::cerr << "Error: Not authenticated. Run 'utopia yip login <username>' "
                 "first or set YIP_TOKEN.\n";
    return 1;
  }

  std::cout << "Packaging " << config.name << " v" << config.version << "...\n";

  std::filesystem::path tarPath =
      std::filesystem::temp_directory_path() /
      (config.name + "_" + config.version + ".tar.gz");

  std::string cmd = "tar -czf \"" + tarPath.string() +
                    "\" --exclude=build --exclude=.git --exclude=.cache -C \"" +
                    projRoot.string() + "\" .";
  if (std::system(cmd.c_str()) != 0) {
    std::cerr << "Error: Failed to compress the project into a tarball.\n";
    return 1;
  }

  std::ifstream tarFile(tarPath, std::ios::binary);
  if (!tarFile) {
    std::cerr << "Error: Could not read the generated tarball.\n";
    return 1;
  }
  std::string tarBody((std::istreambuf_iterator<char>(tarFile)),
                      std::istreambuf_iterator<char>());
  tarFile.close();
  std::filesystem::remove(tarPath);

  nlohmann::json depsJson = nlohmann::json::array();
  for (const auto &dep : config.dependencies) {
    if (!dep.name.empty() && !dep.version.empty()) {
      depsJson.push_back({{"name", dep.name}, {"req", dep.version}});
    }
  }

  HttpOptions opts;
#ifndef UTOPIA_YIP_BASE_URL
#define UTOPIA_YIP_BASE_URL ""
#endif

  opts.baseUrl = UTOPIA_YIP_BASE_URL;
  if (opts.baseUrl.empty()) {
    opts.baseUrl = EnvLoader::get("YIP_BASE_URL");
  }
  opts.enableHttp2 = true;

  HttpClient client(opts);
  std::map<std::string, std::string> headers = {
      {"Authorization", "Bearer " + token},
      {"Content-Type", "application/gzip"},
      {"X-Yip-Deps", depsJson.dump()}};

  std::cout << "Publishing to registry...\n";
  bool success = false;

  client.put("/api/v1/packages/" + config.name + "/" + config.version, tarBody,
             headers, [&](HttpResponse res) {
               if (res.isSuccess()) {
                 std::cout << "Successfully published " << config.name << " v"
                           << config.version << "!\n";
                 success = true;
               } else {
                 std::cerr << "Failed to publish (HTTP " << res.statusCode
                           << "): "
                           << (res.statusCode == 0 ? res.error : res.body)
                           << "\n";
               }
             });

  client.executeAll();
  return success ? 0 : 1;
}

int YipCommand::handleGet(const std::vector<std::string> &args) {
  std::cout << "Resolving dependencies..." << std::endl;

  std::filesystem::path projRoot =
      findProjectRoot(std::filesystem::current_path());
  if (projRoot.empty()) {
    std::cerr << "Fatal: build.yaml not found in the current tree.\n";
    return 1;
  }

  std::string homeDir = EnvLoader::get("HOME");
  if (homeDir.empty()) {
    homeDir = EnvLoader::get("USERPROFILE");
  }
  std::filesystem::path cacheRoot =
      std::filesystem::path(homeDir) / ".utopia" / "cache" / "yip" / "packages";

  HttpOptions opts;
#ifndef UTOPIA_YIP_BASE_URL
#define UTOPIA_YIP_BASE_URL ""
#endif

  opts.baseUrl = UTOPIA_YIP_BASE_URL;
  if (opts.baseUrl.empty()) {
    opts.baseUrl = EnvLoader::get("YIP_BASE_URL");
  }

  if (opts.baseUrl.empty()) {
    std::cerr << "Fatal: YIP_BASE_URL is not configured.\n";
    return 1;
  }
  opts.enableHttp2 = true;
  opts.defaultHeaders["User-Agent"] = "UtopiaCLI/1.0";

  HttpClient client(opts);
  bool overallError = false;

  std::map<std::string, std::string> registryDepsToFetch;
  std::unordered_set<std::string> visitedProjects;

  auto collectDependencies = [&](const std::filesystem::path &projectPath,
                                 auto &self) -> void {
    std::filesystem::path absPath =
        std::filesystem::weakly_canonical(projectPath);
    if (visitedProjects.contains(absPath.string()))
      return;
    visitedProjects.insert(absPath.string());

    std::filesystem::path manifestPath = absPath / "build.yaml";
    if (!std::filesystem::exists(manifestPath))
      return;

    ProjectConfig config;
    try {
      config = parseBuildManifest(manifestPath);
    } catch (const std::exception &e) {
      std::cerr << "Manifest Error in " << manifestPath.string() << ": "
                << e.what() << "\n";
      overallError = true;
      return;
    }

    for (const auto &dep : config.dependencies) {
      if (!dep.path.empty()) {
        std::filesystem::path localDepPath = absPath / dep.path;
        self(localDepPath, self);
      } else if (!dep.name.empty()) {
        registryDepsToFetch[dep.name] = dep.version;
      }
    }
  };

  collectDependencies(projRoot, collectDependencies);

  if (overallError) {
    std::cerr << "Finished with errors during workspace evaluation.\n";
    return 1;
  }

  if (registryDepsToFetch.empty()) {
    std::cout
        << "No remote dependencies found. Dependencies resolved successfully."
        << std::endl;
    return 0;
  }

  for (const auto &[depName, requestedVersion] : registryDepsToFetch) {
    std::string targetVersion = requestedVersion;
    bool versionFound = false;
    bool hasErrors = false;

    std::cout << "Fetching registry info for " << depName << "..." << std::endl;

    client.get("/index/" + depName, [&](HttpResponse res) {
      if (!res.isSuccess()) {
        std::cerr << "Failed to fetch index for " << depName << " (HTTP "
                  << res.statusCode
                  << "): " << (res.statusCode == 0 ? res.error : res.body)
                  << "\n";
        hasErrors = true;
        return;
      }

      std::istringstream stream(res.body);
      std::string line;
      while (std::getline(stream, line)) {
        if (line.empty())
          continue;
        try {
          auto j = nlohmann::json::parse(line);
          std::string vers = j["vers"];

          if (targetVersion.empty() || targetVersion == "latest" ||
              targetVersion == "any" || vers == targetVersion) {
            targetVersion = vers;
            versionFound = true;
            break;
          }
        } catch (...) {
          continue;
        }
      }
    });

    client.executeAll();

    if (hasErrors) {
      overallError = true;
      continue;
    }

    if (!versionFound) {
      std::cerr << "Error: Compatible version for " << depName
                << " not found in registry.\n";
      overallError = true;
      continue;
    }

    std::filesystem::path depCacheDir = cacheRoot / depName / targetVersion;

    if (!std::filesystem::exists(depCacheDir)) {
      std::cout << "Downloading " << depName << " v" << targetVersion << "..."
                << std::endl;

      std::filesystem::create_directories(depCacheDir.parent_path());
      std::filesystem::path tarballPath =
          cacheRoot / depName / (targetVersion + ".tar.gz");

      client.get("/packages/" + depName + "/" + targetVersion + "/download",
                 [&](HttpResponse res) {
                   if (!res.isSuccess()) {
                     std::cerr
                         << "Failed to download " << depName << " v"
                         << targetVersion << " (HTTP " << res.statusCode
                         << "): "
                         << (res.statusCode == 0 ? res.error : "Server error")
                         << "\n";
                     hasErrors = true;
                     return;
                   }
                   std::ofstream out(tarballPath, std::ios::binary);
                   out.write(res.body.c_str(), res.body.size());
                   out.close();
                 });

      client.executeAll();

      if (hasErrors) {
        overallError = true;
        if (std::filesystem::exists(tarballPath)) {
          std::filesystem::remove(tarballPath);
        }
        continue;
      }

      std::filesystem::create_directories(depCacheDir);

      std::string tarCmd = "tar -xzf \"" + tarballPath.string() + "\" -C \"" +
                           depCacheDir.string() + "\"";
      int ret = std::system(tarCmd.c_str());

      if (ret != 0) {
        std::cerr << "Failed to extract package " << depName << "\n";
        overallError = true;
        continue;
      }

      std::filesystem::remove(tarballPath);
    } else {
      std::cout << "Using cached " << depName << " v" << targetVersion
                << std::endl;
    }
  }

  if (overallError) {
    std::cerr << "Finished with errors.\n";
    return 1;
  }

  std::cout << "Dependencies resolved successfully." << std::endl;
  return 0;
}

int YipCommand::handleAdd(const std::vector<std::string> &args) {
  if (args.empty()) {
    std::cerr << "Error: 'yip add' requires a package name.\n";
    return 1;
  }

  std::cout << "Adding package '" << args[0] << "' to build.yaml...\n";
  return 0;
}

} // namespace utopia