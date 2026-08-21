#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace utopia {

namespace fs = std::filesystem;

/* Content-addressed build cache (Cargo/NuGet style).
 *
 * Fully compiled artifacts are stored under a global cache directory:
 *
 *   ~/.utopia/cache/artifacts/<project>/<version>/<fingerprint>/
 *       bin/   lib/   obj/   manifest.json
 *
 * The fingerprint is a SHA-256 over everything that influences the build
 * output: the compiler binary itself, the project identity, all build
 * options, the content of every source file, and the fingerprints of every
 * dependency (recursively). Two projects that require the same dependency at
 * different versions produce different entries, so both versions coexist. */
struct CacheInputs {
  std::string compilerId;
  std::string projectName;
  std::string version;
  std::string projectRoot;
  std::string target;
  int optLevel = 0;
  bool isDebug = false;
  bool asyncEnabled = true;
  bool emitLLVM = false;
  bool emitAsm = false;
  std::string targetTriple;
  std::string targetCpu;
  std::string targetFeatures;
  std::string sysroot;
  std::vector<std::string> macros;
  std::vector<std::string> linkerFlags;
  std::vector<std::string> includeDirs;
  std::vector<std::string> packages;
  std::string outputName;
  std::string buildScriptHash;
  std::vector<std::string> sources;
  std::vector<std::string> dependencies;
};

class ArtifactCache {
public:
  /* SHA-256 helpers. */
  static std::string hashData(const std::string &data);
  static std::string hashFile(const fs::path &path);
  static std::string computeCompilerId(const fs::path &executablePath);

  /* Root of the global cache: ~/.utopia/cache/artifacts */
  static fs::path cacheRoot();

  /* Deterministic fingerprint for a build. Hashes the content of every
   * source file listed in `inputs.sources`. */
  static std::string computeFingerprint(const CacheInputs &inputs);

  /* Cache entry directory for a given project/version/fingerprint. */
  static fs::path entryDir(const std::string &name, const std::string &version,
                           const std::string &fingerprint);

  /* Copy a cached build into `outputDir`. Returns true on success; the
   * `expectedArtifact` path is verified to exist after the copy. */
  static bool restore(const std::string &name, const std::string &version,
                      const std::string &fingerprint,
                      const fs::path &outputDir,
                      const fs::path &expectedArtifact, bool keepObj);

  /* Store the outputs of a fresh build (bin/, lib/ and optionally obj/)
   * atomically. `manifestJson` is written for inspection. */
  static bool store(const std::string &name, const std::string &version,
                    const std::string &fingerprint,
                    const fs::path &outputDir, const std::string &manifestJson,
                    bool keepObj);
};

} // namespace utopia
