#include "Core/ArtifactCache.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>

namespace utopia {

namespace {

/* ------------------------------------------------------------------ */
/* Compact SHA-256 (FIPS 180-4) implementation.                        */
/* ------------------------------------------------------------------ */

struct Sha256 {
  uint32_t state[8];
  uint64_t bitLen;
  uint8_t buffer[64];
  size_t buffered;

  Sha256() : bitLen(0), buffered(0) {
    state[0] = 0x6a09e667;
    state[1] = 0xbb67ae85;
    state[2] = 0x3c6ef372;
    state[3] = 0xa54ff53a;
    state[4] = 0x510e527f;
    state[5] = 0x9b05688c;
    state[6] = 0x1f83d9ab;
    state[7] = 0x5be0cd19;
  }

  static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
  }

  void processBlock(const uint8_t *p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
      w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
             (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
    }
    for (unsigned i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (unsigned i = 0; i < 64; ++i) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + S1 + ch + K[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  /* Raw append that does not update the message bit length (padding). */
  void updateRaw(const void *data, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    if (buffered > 0) {
      size_t take = std::min(size_t(64 - buffered), len);
      std::memcpy(buffer + buffered, p, take);
      buffered += take;
      p += take;
      len -= take;
      if (buffered == 64) {
        processBlock(buffer);
        buffered = 0;
      }
    }
    while (len >= 64) {
      processBlock(p);
      p += 64;
      len -= 64;
    }
    if (len > 0) {
      std::memcpy(buffer, p, len);
      buffered = len;
    }
  }

  void update(const void *data, size_t len) {
    bitLen += uint64_t(len) * 8;
    updateRaw(data, len);
  }

  std::string finalize() {
    static const char hex[] = "0123456789abcdef";
    uint8_t p = 0x80;
    updateRaw(&p, 1);

    uint8_t zeros[64] = {0};
    size_t rem = 64 - buffered;
    if (rem < 8) {
      updateRaw(zeros, rem);
    } else {
      updateRaw(zeros, rem - 8);
    }

    uint8_t lenBytes[8];
    uint64_t bits = bitLen;
    for (int i = 7; i >= 0; --i) {
      lenBytes[i] = uint8_t(bits & 0xff);
      bits >>= 8;
    }
    updateRaw(lenBytes, 8);

    if (buffered > 0) {
      processBlock(buffer);
      buffered = 0;
    }

    std::string out;
    out.reserve(64);
    for (uint32_t s : state) {
      for (int i = 3; i >= 0; --i) {
        out.push_back(hex[(s >> (i * 8 + 4)) & 0xf]);
        out.push_back(hex[(s >> (i * 8)) & 0xf]);
      }
    }
    return out;
  }
};

std::string sha256OfStream(std::istream &in) {
  Sha256 ctx;
  char buf[1 << 16];
  while (in) {
    in.read(buf, sizeof(buf));
    std::streamsize got = in.gcount();
    if (got > 0) {
      ctx.update(buf, size_t(got));
    }
  }
  return ctx.finalize();
}

std::string sanitizeNameComponent(const std::string &raw) {
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    if (c == '/' || c == '\\' || c == ':' || c == '\0') {
      out.push_back('_');
    } else {
      out.push_back(c);
    }
  }
  if (out.empty() || out == "." || out == "..") {
    out = "project";
  }
  return out;
}

void copyTree(const fs::path &src, const fs::path &dst) {
  if (!fs::exists(src)) {
    return;
  }
  fs::create_directories(dst);
  for (const auto &entry : fs::directory_iterator(src)) {
    const auto &from = entry.path();
    auto to = dst / from.filename();
    if (entry.is_directory()) {
      copyTree(from, to);
    } else {
      fs::create_directories(to.parent_path());
      fs::copy_file(from, to, fs::copy_options::overwrite_existing);
    }
  }
}

fs::path homeDir() {
  const char *home = std::getenv("HOME");
  if (!home) {
    home = std::getenv("USERPROFILE");
  }
  return home ? fs::path(home) : fs::current_path();
}

} // namespace

std::string ArtifactCache::hashData(const std::string &data) {
  Sha256 ctx;
  ctx.update(data.data(), data.size());
  return ctx.finalize();
}

std::string ArtifactCache::hashFile(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "";
  }
  return sha256OfStream(in);
}

std::string ArtifactCache::computeCompilerId(const fs::path &executablePath) {
  if (executablePath.empty()) {
    return "unknown-compiler";
  }

  static std::string cachedId;
  if (!cachedId.empty()) {
    return cachedId;
  }

  std::error_code ec;
  auto status = fs::status(executablePath, ec);
  if (ec || !fs::is_regular_file(status)) {
    return "unknown-compiler";
  }
  auto fileSize = fs::file_size(executablePath, ec);
  auto fileTime = fs::last_write_time(executablePath, ec);
  if (ec) {
    return "unknown-compiler";
  }

  /* Hashing a statically-linked LLVM binary takes seconds, so the result is
   * memoized in a sidecar file keyed by (path, size, mtime): the hash is
   * recomputed only when the compiler is actually rebuilt. */
  const std::string keyPath = fs::weakly_canonical(executablePath).string();
  const uint64_t keySize = uint64_t(fileSize);
  const int64_t keyMtime = fileTime.time_since_epoch().count();

  fs::path sidecar = cacheRoot().parent_path() / "compiler-id.json";
  std::string id;
  try {
    if (fs::exists(sidecar)) {
      nlohmann::json j;
      {
        std::ifstream in(sidecar);
        in >> j;
      }
      if (j["path"].get<std::string>() == keyPath &&
          j["size"].get<uint64_t>() == keySize &&
          j["mtime"].get<int64_t>() == keyMtime) {
        id = j["hash"].get<std::string>();
      }
    }
  } catch (...) {
    id.clear();
  }

  if (id.empty()) {
    id = hashFile(executablePath);
    if (id.empty()) {
      return "unknown-compiler";
    }
    try {
      nlohmann::json j;
      j["path"] = keyPath;
      j["size"] = keySize;
      j["mtime"] = keyMtime;
      j["hash"] = id;
      fs::create_directories(sidecar.parent_path());
      std::ofstream out(sidecar);
      out << j.dump(2);
    } catch (...) {
    }
  }

  cachedId = id;
  return id;
}

fs::path ArtifactCache::cacheRoot() {
  return homeDir() / ".utopia" / "cache" / "artifacts";
}

std::string ArtifactCache::computeFingerprint(const CacheInputs &inputs) {
  const std::string sep = "\x1f";
  std::string data;
  data += "utopia-build-cache-v1";
  data += sep + "compiler" + sep + inputs.compilerId;
  data += sep + "name" + sep + inputs.projectName;
  data += sep + "version" + sep + inputs.version;
  data += sep + "root" + sep + inputs.projectRoot;
  data += sep + "target" + sep + inputs.target;
  data += sep + "opt" + sep + std::to_string(inputs.optLevel);
  data += sep + "debug" + sep + (inputs.isDebug ? "1" : "0");
  data += sep + "async" + sep + (inputs.asyncEnabled ? "1" : "0");
  data += sep + "emitllvm" + sep + (inputs.emitLLVM ? "1" : "0");
  data += sep + "emitasm" + sep + (inputs.emitAsm ? "1" : "0");
  data += sep + "triple" + sep + inputs.targetTriple;
  data += sep + "cpu" + sep + inputs.targetCpu;
  data += sep + "mattr" + sep + inputs.targetFeatures;
  data += sep + "sysroot" + sep + inputs.sysroot;

  std::vector<std::string> macros = inputs.macros;
  std::sort(macros.begin(), macros.end());
  for (const auto &m : macros) {
    data += sep + "macro" + sep + m;
  }

  std::vector<std::string> flags = inputs.linkerFlags;
  std::sort(flags.begin(), flags.end());
  for (const auto &f : flags) {
    data += sep + "lflag" + sep + f;
  }

  data += sep + "script" + sep + inputs.buildScriptHash;

  /* The content of every source file, ordered for determinism. */
  std::vector<std::string> sources = inputs.sources;
  std::sort(sources.begin(), sources.end());
  for (const auto &s : sources) {
    data += sep + "src" + sep + s + sep + hashFile(s);
  }

  std::vector<std::string> deps = inputs.dependencies;
  std::sort(deps.begin(), deps.end());
  for (const auto &d : deps) {
    data += sep + "dep" + sep + d;
  }

  return hashData(data);
}

fs::path ArtifactCache::entryDir(const std::string &name,
                                 const std::string &version,
                                 const std::string &fingerprint) {
  return cacheRoot() / sanitizeNameComponent(name) /
         sanitizeNameComponent(version) / fingerprint;
}

bool ArtifactCache::restore(const std::string &name, const std::string &version,
                            const std::string &fingerprint,
                            const fs::path &outputDir,
                            const fs::path &expectedArtifact, bool keepObj) {
  fs::path entry = entryDir(name, version, fingerprint);
  if (!fs::is_directory(entry)) {
    return false;
  }

  for (const char *sub : {"bin", "lib"}) {
    copyTree(entry / sub, outputDir / sub);
  }
  if (keepObj) {
    copyTree(entry / "obj", outputDir / "obj");
  }

  if (!fs::is_regular_file(expectedArtifact)) {
    /* Corrupt or partial entry; drop it so it gets rebuilt. */
    fs::remove_all(entry);
    return false;
  }
  return true;
}

bool ArtifactCache::store(const std::string &name, const std::string &version,
                          const std::string &fingerprint,
                          const fs::path &outputDir,
                          const std::string &manifestJson, bool keepObj) {
  fs::path entry = entryDir(name, version, fingerprint);
  if (fs::exists(entry)) {
    return true;
  }

  bool hasBin = fs::exists(outputDir / "bin");
  bool hasLib = fs::exists(outputDir / "lib");
  bool hasObj = keepObj && fs::exists(outputDir / "obj");
  if (!hasBin && !hasLib) {
    return false;
  }

  /* Write to a temporary directory and rename for atomic publication. */
  std::random_device rd;
  std::mt19937_64 gen(rd());
  fs::path tmp = entry.parent_path() /
                 ("tmp-" + std::to_string(gen()));

  fs::create_directories(tmp);
  if (hasBin) {
    copyTree(outputDir / "bin", tmp / "bin");
  }
  if (hasLib) {
    copyTree(outputDir / "lib", tmp / "lib");
  }
  if (hasObj) {
    copyTree(outputDir / "obj", tmp / "obj");
  }

  std::ofstream manifest(tmp / "manifest.json");
  if (manifest) {
    manifest << manifestJson;
  }

  std::error_code ec;
  fs::rename(tmp, entry, ec);
  if (ec) {
    if (fs::exists(entry)) {
      /* Lost a race against another build process; the entry is fine. */
      fs::remove_all(tmp);
      return true;
    }
    fs::remove_all(tmp);
    return false;
  }
  return true;
}

} // namespace utopia
