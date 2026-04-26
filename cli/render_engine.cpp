#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/Accel.hpp"
#include "core/Camera.hpp"
#include "core/Ray.hpp"
#include "io/Image.hpp"
#include "runtime/Profiling.hpp"
#include "runtime/ResourceTracker.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneConfig.hpp"

#if !defined(_WIN32)
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#if defined(TORIRENDER_USE_MPI)
#include <mpi.h>
#endif

#if defined(TORIRENDER_USE_OPENMP)
#include <omp.h>
#endif

#if defined(TORIRENDER_USE_OPENACC)
#include <openacc.h>
#endif

/*
Render Engine Architecture
--------------------------
Role:
- Single runtime driver shared by `torirender_cpu` and `torirender_gpu`.
- Rendering physics is unchanged; only execution backend and orchestration differ.

Backend:
- CPU binary: serial or MPI/OpenMP parallel.
- GPU binary: forced parallel, OpenACC offload, one MPI rank per GPU.

Pipeline:
1) Parse CLI and load JSON scene/runtime config.
2) Validate runtime contract (mode, ranks, threads, device availability).
3) Initialize MPI/OpenMP/OpenACC based on build flags.
4) Partition image rows across ranks and render local tiles.
5) Gather local buffers to rank 0 via MPI_Gatherv.
6) Rank 0 writes PNG, heartbeat status file, render metrics CSV row, and resource reports.

Output contract:
- `final/images/<backend>_<resolution>_ssp<_>_depth<_>_<timestamp>.png`
- `final/status/<run_id>.status`
- `final/serial_metrics.csv` or `final/parallel_metrics.csv` (mode-based)
- `final/resource_metrics.csv` (per-rank append-only)
- `final/run_reports/resource_report_<run_id>.txt`

Operational guarantees:
- Deterministic run ID and filename timestamps.
- Heartbeat auto-throttling for long runs.
- MPI-wide fail-fast on distributed configuration errors.
*/

namespace {

#if !defined(TORIRENDER_GIT_COMMIT)
#define TORIRENDER_GIT_COMMIT "unknown"
#endif

#if defined(TORIRENDER_EXEC_GPU)
constexpr bool kIsGpuBinary = true;
constexpr const char* kBackendTag = "gpu";
#else
constexpr bool kIsGpuBinary = false;
constexpr const char* kBackendTag = "cpu";
#endif

constexpr int kDefaultHeartbeatSeconds = 300;
constexpr int kLongRunHeartbeatSeconds = 1200;
constexpr int kLongRunThresholdSeconds = 1200;

struct CliOptions {
  std::string configPath = "config/scene.json";
  std::string outputDir = "final";
  std::optional<std::string> modeOverride;
  std::optional<int> mpiRanksOverride;
  std::optional<int> ompThreadsOverride;
  std::optional<int> heartbeatOverride;
  bool profileEnabled = false;
  bool profilePerRank = false;
  std::string perfDir = "final/perf";
  std::string runLabel;
};

struct RenderPaths {
  std::filesystem::path outputDir;
  std::filesystem::path imagesDir;
  std::filesystem::path statusDir;
  std::filesystem::path imagePath;
  std::filesystem::path metricsCsvPath;
  std::filesystem::path statusPath;
  std::string imageFileName;
  std::string runId;
};

struct RowRange {
  int start = 0;
  int count = 0;
};

struct MpiContext {
  bool enabled = false;
  bool initializedByApp = false;
  int rank = 0;
  int size = 1;
};

struct CpuUsageSample {
  double userSeconds = 0.0;
  double systemSeconds = 0.0;
  long long peakMemoryKb = 0;
};

struct TileTimingStats {
  double sumSeconds = 0.0;
  double maxSeconds = 0.0;
  double minSeconds = std::numeric_limits<double>::infinity();
  std::uint64_t count = 0;

  void add(double seconds) {
    if (seconds < 0.0) {
      return;
    }
    sumSeconds += seconds;
    maxSeconds = std::max(maxSeconds, seconds);
    minSeconds = std::min(minSeconds, seconds);
    ++count;
  }

  double safeMin() const {
    return count == 0 ? 0.0 : minSeconds;
  }
};

struct RankPerfPacked {
  double rankComputeSeconds = 0.0;
  double rankTotalSeconds = 0.0;
  double rankRenderRegionSeconds = 0.0;
  double tileComputeSumSeconds = 0.0;
  double tileComputeMaxSeconds = 0.0;
  double tileComputeMinSeconds = 0.0;
  double ompParallelRegionSeconds = 0.0;
  unsigned long long tileCount = 0ULL;
  unsigned long long pixelCount = 0ULL;
  unsigned long long sampleCount = 0ULL;
};

struct BaselineQuery {
  std::string sceneFile;
  int width = 0;
  int height = 0;
  int samplesPerPixel = 0;
  int maxDepth = 0;
};

#if !defined(_WIN32)
// Convert POSIX timeval to seconds for CPU accounting.
double timevalToSeconds(const timeval& value) {
  return static_cast<double>(value.tv_sec) + (static_cast<double>(value.tv_usec) / 1'000'000.0);
}
#endif

// Return a local wall-clock timestamp string for rank start/end markers.
std::string wallClockNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm localTm{};
#if defined(_WIN32)
  localtime_s(&localTm, &t);
#else
  localtime_r(&t, &localTm);
#endif
  std::ostringstream out;
  out << std::put_time(&localTm, "%Y-%m-%d %H:%M:%S");
  return out.str();
}

// Read machine hostname for per-rank placement diagnostics.
std::string detectHostname() {
#if !defined(_WIN32)
  std::array<char, 256> buffer{};
  if (gethostname(buffer.data(), buffer.size() - 1) == 0) {
    buffer.back() = '\0';
    return std::string(buffer.data());
  }
#endif
  return "unknown";
}

// Detect CPU slots available to this process from scheduler/env fallback chain.
int detectProcessCpuCount() {
  const auto readPositiveEnv = [](const char* name) -> int {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
      return 0;
    }
    try {
      const int value = std::stoi(raw);
      return value > 0 ? value : 0;
    } catch (...) {
      return 0;
    }
  };

  int ncpus = readPositiveEnv("PBS_NCPUS");
  if (ncpus <= 0) {
    ncpus = readPositiveEnv("SLURM_CPUS_ON_NODE");
  }
  if (ncpus <= 0) {
    ncpus = readPositiveEnv("OMP_NUM_THREADS");
  }
  if (ncpus <= 0) {
    const unsigned int hw = std::thread::hardware_concurrency();
    ncpus = hw > 0U ? static_cast<int>(hw) : 0;
  }
  return ncpus;
}

// Snapshot process CPU usage and peak RSS for post-run resource analysis.
CpuUsageSample captureCpuUsageSample() {
  CpuUsageSample sample{};
#if !defined(_WIN32)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    sample.userSeconds = timevalToSeconds(usage.ru_utime);
    sample.systemSeconds = timevalToSeconds(usage.ru_stime);
    sample.peakMemoryKb = static_cast<long long>(usage.ru_maxrss);
  }
#endif
  return sample;
}

// Convert user tokens to lowercase for case-insensitive option handling.
std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

// Print concise CLI contract for positional and optional arguments.
void printUsage(const char* binaryName) {
  std::cerr << "Usage: " << binaryName
            << " [config_path] [output_dir] [--mode serial|parallel] [--mpi-ranks N]"
               " [--omp-threads N] [--heartbeat N] [--profile]"
               " [--profile-per-rank] [--perf-dir <path>] [--run-label <label>]\n";
}

// Parse integer option values safely without throwing to caller.
bool parseIntArg(const std::string& token, int& outValue) {
  try {
    outValue = std::stoi(token);
    return true;
  } catch (...) {
    return false;
  }
}

// Parse CLI options and optional positional config/output paths.
bool parseArgs(int argc, char** argv, CliOptions& out) {
  int positionalCount = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--mode") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --mode\n";
        return false;
      }
      out.modeOverride = toLower(argv[++i]);
      continue;
    }

    if (arg == "--mpi-ranks") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --mpi-ranks\n";
        return false;
      }
      int value = 0;
      if (!parseIntArg(argv[++i], value) || value <= 0) {
        std::cerr << "Invalid value for --mpi-ranks\n";
        return false;
      }
      out.mpiRanksOverride = value;
      continue;
    }

    if (arg == "--omp-threads") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --omp-threads\n";
        return false;
      }
      int value = 0;
      if (!parseIntArg(argv[++i], value) || value <= 0) {
        std::cerr << "Invalid value for --omp-threads\n";
        return false;
      }
      out.ompThreadsOverride = value;
      continue;
    }

    if (arg == "--heartbeat") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --heartbeat\n";
        return false;
      }
      int value = 0;
      if (!parseIntArg(argv[++i], value) || value <= 0) {
        std::cerr << "Invalid value for --heartbeat\n";
        return false;
      }
      out.heartbeatOverride = value;
      continue;
    }

    if (arg == "--profile") {
      out.profileEnabled = true;
      continue;
    }

    if (arg == "--profile-per-rank") {
      out.profilePerRank = true;
      continue;
    }

    if (arg == "--perf-dir") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --perf-dir\n";
        return false;
      }
      out.perfDir = argv[++i];
      continue;
    }

    // Backward-compatible alias; output is now text per run in a directory.
    if (arg == "--perf-csv") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --perf-csv\n";
        return false;
      }
      const std::filesystem::path legacyPath(argv[++i]);
      if (legacyPath.has_extension()) {
        out.perfDir = legacyPath.parent_path().empty() ? "." : legacyPath.parent_path().string();
      } else {
        out.perfDir = legacyPath.string();
      }
      continue;
    }

    if (arg == "--run-label") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --run-label\n";
        return false;
      }
      out.runLabel = argv[++i];
      continue;
    }

    if (arg.rfind("--", 0) == 0) {
      std::cerr << "Unknown option: " << arg << "\n";
      return false;
    }

    if (positionalCount == 0) {
      out.configPath = arg;
      ++positionalCount;
      continue;
    }

    if (positionalCount == 1) {
      out.outputDir = arg;
      ++positionalCount;
      continue;
    }

    std::cerr << "Too many positional arguments\n";
    return false;
  }

  return true;
}

// Ensure output directory tree exists before writing files.
bool ensureDir(const std::filesystem::path& path, std::string& errorMessage) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    errorMessage = "Failed to create directory: " + path.string() + " (" + ec.message() + ")";
    return false;
  }
  return true;
}

// Create deterministic local timestamp token for filenames and run IDs.
std::string timestampTokenNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm localTm{};
#if defined(_WIN32)
  localtime_s(&localTm, &t);
#else
  localtime_r(&t, &localTm);
#endif
  std::ostringstream out;
  out << std::put_time(&localTm, "%Y-%m-%d_time_%Hh%Mm%Ss");
  return out.str();
}

// Compute per-rank contiguous row range for static image partitioning.
RowRange rowRangeForRank(int rank, int size, int totalRows) {
  const int rowsPerRank = totalRows / size;
  const int remainder = totalRows % size;
  const int extra = rank < remainder ? 1 : 0;
  const int start = (rank * rowsPerRank) + std::min(rank, remainder);
  return RowRange{start, rowsPerRank + extra};
}

// SplitMix64 core step used for deterministic per-pixel RNG stream.
TORIRENDER_ACC_ROUTINE_SEQ
std::uint64_t splitMix64(std::uint64_t& state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// Map RNG state to uniform [0, 1) sample for jitter generation.
TORIRENDER_ACC_ROUTINE_SEQ
double randomUnit(std::uint64_t& state) noexcept {
  constexpr double kInv53 = 1.0 / 9007199254740992.0;
  const std::uint64_t bits = splitMix64(state);
  return static_cast<double>(bits >> 11) * kInv53;
}

// Trace one pixel (with optional jittered multi-sample accumulation).
TORIRENDER_ACC_ROUTINE_SEQ
torirender::Vec3 renderPixelColor(const torirender::Camera& camera,
                                  const torirender::Scene& scene,
                                  int x,
                                  int y,
                                  int imageWidth,
                                  int samplesPerPixel,
                                  int maxDepth,
                                  std::uint64_t rngSeed) {
  torirender::Vec3 accumulated{};
  for (int sample = 0; sample < samplesPerPixel; ++sample) {
    torirender::Ray ray;
    if (samplesPerPixel == 1) {
      ray = camera.getRay(x, y);
    } else {
      std::uint64_t sampleState = rngSeed;
      sampleState ^= static_cast<std::uint64_t>((y * imageWidth) + x) * 0x9E3779B97F4A7C15ULL;
      sampleState ^= static_cast<std::uint64_t>(sample) * 0xBF58476D1CE4E5B9ULL;
      const double jitterX = randomUnit(sampleState) - 0.5;
      const double jitterY = randomUnit(sampleState) - 0.5;
      ray = camera.getRay(static_cast<double>(x) + jitterX, static_cast<double>(y) + jitterY);
    }
    accumulated += scene.trace(ray, maxDepth);
  }
  return accumulated / static_cast<double>(samplesPerPixel);
}

// Persist human-readable heartbeat status for long-running renders.
void writeHeartbeat(const std::filesystem::path& statusPath,
                    const std::string& runId,
                    const std::string& backend,
                    const std::string& mode,
                    int mpiRanks,
                    int ompThreads,
                    int gpus,
                    std::uint64_t donePixels,
                    std::uint64_t totalPixels,
                    double elapsedSeconds,
                    int heartbeatSeconds) {
  const double progress =
      totalPixels > 0 ? (100.0 * static_cast<double>(donePixels) / static_cast<double>(totalPixels))
                      : 0.0;
  double etaSeconds = 0.0;
  if (donePixels > 0 && totalPixels > donePixels) {
    const double rate = static_cast<double>(donePixels) / std::max(elapsedSeconds, 1e-9);
    etaSeconds = static_cast<double>(totalPixels - donePixels) / std::max(rate, 1e-9);
  }

  const std::filesystem::path tempPath = statusPath.string() + ".tmp";
  std::ofstream out(tempPath, std::ios::trunc);
  if (!out) {
    return;
  }

  out << "run_id=" << runId << '\n';
  out << "backend=" << backend << '\n';
  out << "mode=" << mode << '\n';
  out << "mpi_ranks=" << mpiRanks << '\n';
  out << "omp_threads=" << ompThreads << '\n';
  out << "gpus=" << gpus << '\n';
  out << "done_pixels=" << donePixels << '\n';
  out << "total_pixels=" << totalPixels << '\n';
  out << "progress_percent=" << std::fixed << std::setprecision(2) << progress << '\n';
  out << "elapsed_seconds=" << std::fixed << std::setprecision(3) << elapsedSeconds << '\n';
  out << "eta_seconds=" << std::fixed << std::setprecision(3) << etaSeconds << '\n';
  out << "heartbeat_seconds=" << heartbeatSeconds << '\n';

  if (!out) {
    return;
  }

  std::error_code ec;
  std::filesystem::rename(tempPath, statusPath, ec);
  if (ec) {
    std::filesystem::remove(tempPath, ec);
  }
}

std::string formatDouble(double value, int precision = 9) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

std::string normalizeScenePath(const std::string& rawPath) {
  if (rawPath.empty()) {
    return rawPath;
  }

  std::error_code ec;
  const std::filesystem::path path(rawPath);
  const auto normalized = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    return normalized.string();
  }

  const auto lexical = path.lexically_normal();
  return lexical.string();
}

std::string trimCopy(const std::string& text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }

  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }

  return text.substr(begin, end - begin);
}

std::optional<std::pair<std::string, std::string>> parseKeyValueLine(const std::string& line) {
  const auto equalPos = line.find('=');
  if (equalPos == std::string::npos) {
    return std::nullopt;
  }

  const std::string key = trimCopy(line.substr(0, equalPos));
  if (key.empty()) {
    return std::nullopt;
  }
  const std::string value = trimCopy(line.substr(equalPos + 1));
  return std::make_pair(key, value);
}

bool parsePositiveDouble(const std::string& token, double& out) {
  try {
    const double parsed = std::stod(token);
    if (!std::isfinite(parsed) || parsed <= 0.0) {
      return false;
    }
    out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<double> findSerialBaselineSeconds(const std::filesystem::path& perfOutputPath,
                                                const BaselineQuery& query) {
  std::filesystem::path searchDir = perfOutputPath;
  std::error_code ec;
  if (std::filesystem::is_regular_file(searchDir, ec)) {
    searchDir = searchDir.parent_path();
    ec.clear();
  }

  if (searchDir.empty()) {
    return std::nullopt;
  }
  if (!std::filesystem::exists(searchDir, ec) || ec ||
      !std::filesystem::is_directory(searchDir, ec) || ec) {
    return std::nullopt;
  }

  const auto sceneNorm = normalizeScenePath(query.sceneFile);
  std::optional<double> latest;
  std::string latestTimestamp;

  for (const auto& entry : std::filesystem::directory_iterator(searchDir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() != ".txt") {
      continue;
    }

    std::ifstream input(entry.path());
    if (!input) {
      continue;
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty()) {
        continue;
      }
      const auto parsed = parseKeyValueLine(line);
      if (!parsed.has_value()) {
        continue;
      }
      values[parsed->first] = parsed->second;
    }

    const auto valueAt = [&](const char* key) -> const std::string* {
      const auto it = values.find(key);
      if (it == values.end()) {
        return nullptr;
      }
      return &it->second;
    };

    const std::string* mode = valueAt("mode");
    const std::string* sceneFile = valueAt("scene_file");
    const std::string* widthText = valueAt("image_width");
    const std::string* heightText = valueAt("image_height");
    const std::string* sppText = valueAt("samples_per_pixel");
    const std::string* depthText = valueAt("max_depth");
    const std::string* totalText = valueAt("total_wall_seconds");
    if (mode == nullptr || sceneFile == nullptr || widthText == nullptr || heightText == nullptr ||
        sppText == nullptr || depthText == nullptr || totalText == nullptr) {
      continue;
    }

    if (toLower(*mode) != "serial") {
      continue;
    }

    if (normalizeScenePath(*sceneFile) != sceneNorm) {
      continue;
    }

    try {
      const int width = std::stoi(*widthText);
      const int height = std::stoi(*heightText);
      const int spp = std::stoi(*sppText);
      const int depth = std::stoi(*depthText);
      if (width != query.width || height != query.height || spp != query.samplesPerPixel ||
          depth != query.maxDepth) {
        continue;
      }
    } catch (...) {
      continue;
    }

    double totalSeconds = 0.0;
    if (!parsePositiveDouble(*totalText, totalSeconds)) {
      continue;
    }

    std::string timestamp = "";
    if (const std::string* timestampText = valueAt("timestamp"); timestampText != nullptr) {
      timestamp = *timestampText;
    }
    if (!latest.has_value() || timestamp >= latestTimestamp) {
      latest = totalSeconds;
      latestTimestamp = timestamp;
    }
  }

  return latest;
}

double safeDivide(double numerator, double denominator) {
  if (std::fabs(denominator) <= 1e-12) {
    return 0.0;
  }
  return numerator / denominator;
}

// Append one metrics row per finished run; create header lazily.
bool appendMetricsCsv(const std::filesystem::path& csvPath,
                      const std::string& runId,
                      const std::string& timestamp,
                      const std::string& backend,
                      const std::string& mode,
                      const std::string& imageFile,
                      int width,
                      int height,
                      int ssp,
                      int depth,
                      int mpiRanks,
                      int ompThreads,
                      int gpus,
                      double seconds,
                      std::string& errorMessage) {
  constexpr const char* kHeader =
      "run_id,timestamp,backend,mode,image_file,resolution,ssp,depth,mpi_ranks,omp_threads,gpus,"
      "time_seconds";

  bool writeHeader = true;
  std::error_code ec;
  if (std::filesystem::exists(csvPath, ec)) {
    if (ec) {
      errorMessage =
          "Failed to access metrics CSV path: " + csvPath.string() + " (" + ec.message() + ")";
      return false;
    }
    const auto size = std::filesystem::file_size(csvPath, ec);
    if (ec) {
      errorMessage =
          "Failed to query metrics CSV size: " + csvPath.string() + " (" + ec.message() + ")";
      return false;
    }
    writeHeader = size == 0;
  }

  std::ofstream csv(csvPath, std::ios::app);
  if (!csv) {
    errorMessage = "Failed to open metrics CSV for append: " + csvPath.string();
    return false;
  }

  if (writeHeader) {
    csv << kHeader << '\n';
  }

  csv << runId << ',' << timestamp << ',' << backend << ',' << mode << ',' << imageFile << ','
      << width << 'x' << height << ',' << ssp << ',' << depth << ',' << mpiRanks << ','
      << ompThreads << ',' << gpus << ',' << std::fixed << std::setprecision(6) << seconds << '\n';

  if (!csv) {
    errorMessage = "Failed to write metrics row to: " + csvPath.string();
    return false;
  }

  return true;
}

void addField(std::vector<torirender::runtime::CsvField>& fields,
              std::string name,
              const std::string& value) {
  fields.push_back(torirender::runtime::CsvField{std::move(name), value});
}

void addField(std::vector<torirender::runtime::CsvField>& fields, std::string name, int value) {
  fields.push_back(torirender::runtime::CsvField{std::move(name), std::to_string(value)});
}

void addField(std::vector<torirender::runtime::CsvField>& fields,
              std::string name,
              std::uint64_t value) {
  fields.push_back(torirender::runtime::CsvField{std::move(name), std::to_string(value)});
}

void addField(std::vector<torirender::runtime::CsvField>& fields, std::string name, double value) {
  fields.push_back(torirender::runtime::CsvField{std::move(name), formatDouble(value)});
}

bool writeProfileTextReport(const std::filesystem::path& reportPath,
                            const std::vector<torirender::runtime::CsvField>& fields,
                            std::string& errorMessage) {
  const std::filesystem::path parent = reportPath.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      errorMessage = "Failed to create profiling report directory: " + parent.string() + " (" +
                     ec.message() + ")";
      return false;
    }
  }

  std::ofstream out(reportPath, std::ios::trunc);
  if (!out) {
    errorMessage = "Failed to open profiling report for write: " + reportPath.string();
    return false;
  }

  for (const auto& field : fields) {
    out << field.name << '=' << field.value << '\n';
  }
  if (!out) {
    errorMessage = "Failed while writing profiling report: " + reportPath.string();
    return false;
  }
  return true;
}

#if defined(TORIRENDER_USE_MPI)
// Abort all ranks with a single user-facing message from rank 0.
int mpiBail(const std::string& message, int exitCode = 1) {
  int rank = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  if (rank == 0) {
    std::cerr << message << '\n';
  }
  MPI_Abort(MPI_COMM_WORLD, exitCode);
  return exitCode;
}
#endif

int runMain(int argc, char** argv) {
  const auto runWallStart = std::chrono::steady_clock::now();
  CliOptions cli;
  if (!parseArgs(argc, argv, cli)) {
    printUsage(argv[0]);
    return 1;
  }

  const bool profilingEnabled =
#if defined(TORIRENDER_ENABLE_PROFILING)
      cli.profileEnabled;
#else
      false;
#endif

#if !defined(TORIRENDER_ENABLE_PROFILING)
  if (cli.profileEnabled) {
    std::cerr << "Profiling requested, but this binary was built with TORIRENDER_ENABLE_PROFILING=OFF.\n";
  }
#endif

  torirender::runtime::SectionProfiler sectionProfiler;

  torirender::SceneConfig config{};
  std::string errorMessage;
  {
    const auto parseStart = std::chrono::steady_clock::now();
    if (!torirender::loadSceneConfigFromJsonFile(cli.configPath, config, &errorMessage)) {
      std::cerr << "Failed to load scene config from " << cli.configPath << ": " << errorMessage
                << '\n';
      return 1;
    }
    const double sceneParseSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - parseStart).count();
    if (profilingEnabled) {
      sectionProfiler.add_seconds("scene_parse_seconds", sceneParseSeconds);
    }
  }

  std::string mode = toLower(config.runtime.mode);
  if (cli.modeOverride.has_value()) {
    mode = toLower(*cli.modeOverride);
  }
  if (mode.empty()) {
    mode = "serial";
  }

  if (kIsGpuBinary) {
    if (mode != "serial" && mode != "parallel") {
      mode = "parallel";
    }
    if (cli.modeOverride.has_value() && *cli.modeOverride != "parallel") {
      std::cerr << "torirender_gpu always runs in parallel mode.\n";
      return 1;
    }
    mode = "parallel";
  } else if (mode != "serial" && mode != "parallel") {
    std::cerr << "Invalid mode: " << mode << " (expected serial or parallel)\n";
    return 1;
  }

  const int desiredMpiRanks = std::max(
      1, cli.mpiRanksOverride.has_value() ? *cli.mpiRanksOverride : config.runtime.mpiRanks);
  int desiredOmpThreads = std::max(
      1, cli.ompThreadsOverride.has_value() ? *cli.ompThreadsOverride : config.runtime.ompThreads);
  if (mode == "serial") {
    desiredOmpThreads = 1;
  }
  if (kIsGpuBinary) {
    desiredOmpThreads = 1;
  }

  const bool heartbeatOverridden = cli.heartbeatOverride.has_value();
  int heartbeatSeconds =
      heartbeatOverridden ? *cli.heartbeatOverride : config.runtime.heartbeatSeconds;
  if (heartbeatSeconds <= 0) {
    heartbeatSeconds = kDefaultHeartbeatSeconds;
  }

  MpiContext mpi{};
  const auto mpiInitStart = std::chrono::steady_clock::now();
#if defined(TORIRENDER_USE_MPI)
  int alreadyInitialized = 0;
  MPI_Initialized(&alreadyInitialized);
  if (!alreadyInitialized) {
    if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
      std::cerr << "Failed to initialize MPI.\n";
      return 1;
    }
    mpi.initializedByApp = true;
  }
  mpi.enabled = true;
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi.rank);
  MPI_Comm_size(MPI_COMM_WORLD, &mpi.size);
#else
  mpi.enabled = false;
  mpi.rank = 0;
  mpi.size = 1;
#endif
  const double mpiInitSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - mpiInitStart).count();
  if (profilingEnabled) {
    sectionProfiler.add_seconds("mpi_init_seconds", mpiInitSeconds);
  }

  const auto rankStartSteady = std::chrono::steady_clock::now();
  const std::string rankStartWall = wallClockNow();
  const std::string rankHostname = detectHostname();
  const int rankCpuSlots = detectProcessCpuCount();
  const CpuUsageSample cpuStartSample = captureCpuUsageSample();

  double localSceneTime = 0.0;
  double localKernelTime = 0.0;
  double localTransferTime = 0.0;
  double localMpiTime = 0.0;
  double localOutputTime = 0.0;
  double localGpuActiveTime = 0.0;
  std::uint64_t localKernelLaunches = 0;
  bool localKernelsAsync = false;
  int assignedGpuId = -1;

  double localMpiBroadcastSeconds = 0.0;
  double localMpiScatterSeconds = 0.0;
  double localMpiGatherSeconds = 0.0;
  double localSynchronizationSeconds = 0.0;
  double localSyncBeforeRenderSeconds = 0.0;
  double localCameraSetupSeconds = 0.0;
  double localBvhBuildSeconds = 0.0;
  double localRenderRegionWallSeconds = 0.0;
  double localOmpParallelRegionSeconds = 0.0;
  double localFinalizationSeconds = 0.0;
  TileTimingStats localTileStats{};
  double localRankComputeSeconds = 0.0;
  std::uint64_t localTileCount = 0;
  std::uint64_t localPixelCount = 0;
  std::uint64_t localSampleCount = 0;

  if (mode == "parallel" && !mpi.enabled) {
#if defined(TORIRENDER_USE_MPI)
    (void)mpi;
#else
    std::cerr << "Parallel mode requires MPI support in this build.\n";
    return 1;
#endif
  }

  if (mode == "serial" && mpi.size != 1) {
#if defined(TORIRENDER_USE_MPI)
    return mpiBail("Serial mode requires exactly one MPI rank.");
#else
    std::cerr << "Serial mode requires exactly one rank.\n";
    return 1;
#endif
  }

  if (desiredMpiRanks != mpi.size) {
#if defined(TORIRENDER_USE_MPI)
    return mpiBail("Requested mpi_ranks does not match launched MPI size.");
#else
    std::cerr << "Requested mpi_ranks does not match available MPI size.\n";
    return 1;
#endif
  }

#if defined(TORIRENDER_USE_OPENMP)
  omp_set_dynamic(0);
  omp_set_num_threads(desiredOmpThreads);
#endif

  int detectedGpus = 0;
#if defined(TORIRENDER_USE_OPENACC)
  acc_device_t deviceType = acc_device_nvidia;
  detectedGpus = acc_get_num_devices(deviceType);
  if (detectedGpus <= 0) {
    deviceType = acc_device_default;
    detectedGpus = acc_get_num_devices(deviceType);
  }

  if (kIsGpuBinary) {
    if (detectedGpus <= 0) {
#if defined(TORIRENDER_USE_MPI)
      return mpiBail("No OpenACC-compatible GPU device found.");
#else
      std::cerr << "No OpenACC-compatible GPU device found.\n";
      return 1;
#endif
    }

    if (mpi.size > detectedGpus) {
#if defined(TORIRENDER_USE_MPI)
      return mpiBail("GPU binary requires 1 MPI rank per GPU. Ranks exceed visible GPUs.");
#else
      std::cerr << "GPU binary requires 1 MPI rank per GPU.\n";
      return 1;
#endif
    }

    acc_set_device_num(mpi.rank, deviceType);
    acc_init(deviceType);
    assignedGpuId = acc_get_device_num(deviceType);
  }
#else
  if (kIsGpuBinary) {
#if defined(TORIRENDER_USE_MPI)
    return mpiBail("torirender_gpu was built without OpenACC support.");
#else
    std::cerr << "torirender_gpu was built without OpenACC support.\n";
    return 1;
#endif
  }
#endif

  const int width = std::max(config.camera.imageWidth, 1);
  const int height = std::max(config.camera.imageHeight, 1);
  const int samplesPerPixel = std::max(config.camera.samplesPerPixel, 1);
  const int maxDepth = std::max(config.camera.maxDepth, 0);
  const int pEffective = std::max(1, mpi.size * desiredOmpThreads);

  std::string timestampToken;
  if (mpi.rank == 0) {
    timestampToken = timestampTokenNow();
  }

#if defined(TORIRENDER_USE_MPI)
  {
    std::array<char, 64> buffer{};
    if (mpi.rank == 0) {
      std::snprintf(buffer.data(), buffer.size(), "%s", timestampToken.c_str());
    }
    const auto mpiCallStart = std::chrono::steady_clock::now();
    MPI_Bcast(buffer.data(), static_cast<int>(buffer.size()), MPI_CHAR, 0, MPI_COMM_WORLD);
    const double callSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - mpiCallStart).count();
    localMpiBroadcastSeconds += callSeconds;
    localMpiTime += callSeconds;
    if (mpi.rank != 0) {
      timestampToken = buffer.data();
    }
  }
#endif

  RenderPaths paths{};
  paths.outputDir = std::filesystem::path(cli.outputDir.empty() ? "final" : cli.outputDir);
  paths.imagesDir = paths.outputDir / "images";
  paths.statusDir = paths.outputDir / "status";
  paths.metricsCsvPath =
      paths.outputDir / (mode == "serial" ? "serial_metrics.csv" : "parallel_metrics.csv");

  const std::string runId = std::string(kBackendTag) + '_' + mode + '_' + timestampToken;
  paths.runId = runId;

  paths.imageFileName = std::string(kBackendTag) + '_' + std::to_string(width) + "x" +
                        std::to_string(height) + "_ssp" + std::to_string(samplesPerPixel) +
                        "_depth" + std::to_string(maxDepth) + '_' + timestampToken + ".png";
  paths.imagePath = paths.imagesDir / paths.imageFileName;
  paths.statusPath = paths.statusDir / (runId + ".status");

  torirender::runtime::ResourceTracker resourceTracker(runId, paths.outputDir);
  resourceTracker.log_mpi_info(
      mpi.rank, mpi.size, rankHostname, rankCpuSlots, rankStartWall, std::string());
  resourceTracker.log_cpu_threads(desiredOmpThreads);
  resourceTracker.log_gpu_info(assignedGpuId, detectedGpus);

  if (mpi.rank == 0) {
    if (!ensureDir(paths.outputDir, errorMessage) || !ensureDir(paths.imagesDir, errorMessage) ||
        !ensureDir(paths.statusDir, errorMessage)) {
      std::cerr << errorMessage << '\n';
#if defined(TORIRENDER_USE_MPI)
      MPI_Abort(MPI_COMM_WORLD, 1);
#endif
      return 1;
    }

    std::cout << "ToriRender start\n";
    std::cout << "  backend: " << kBackendTag << '\n';
    std::cout << "  mode: " << mode << '\n';
    std::cout << "  resolution: " << width << 'x' << height << '\n';
    std::cout << "  spp: " << samplesPerPixel << '\n';
    std::cout << "  depth: " << maxDepth << '\n';
    std::cout << "  mpi_ranks: " << mpi.size << '\n';
    std::cout << "  omp_threads: " << desiredOmpThreads << '\n';
    std::cout << "  output image: " << paths.imagePath.string() << '\n';
    std::cout << "  metrics csv: " << paths.metricsCsvPath.string() << '\n';
  }

#if defined(TORIRENDER_USE_MPI)
  {
    const auto mpiCallStart = std::chrono::steady_clock::now();
    MPI_Barrier(MPI_COMM_WORLD);
    const double barrierSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - mpiCallStart).count();
    localMpiTime += barrierSeconds;
    localSynchronizationSeconds += barrierSeconds;
    localSyncBeforeRenderSeconds += barrierSeconds;
  }
#endif

  const auto sceneStart = std::chrono::steady_clock::now();
  {
    const auto cameraStart = std::chrono::steady_clock::now();
    const torirender::Camera camera(config.camera.lookFrom,
                                    config.camera.lookAt,
                                    config.camera.viewUp,
                                    config.camera.vfov,
                                    static_cast<double>(width) / static_cast<double>(height),
                                    width,
                                    height);
    localCameraSetupSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - cameraStart).count();

    const auto bvhStart = std::chrono::steady_clock::now();
    const torirender::Scene scene(config, profilingEnabled ? &localBvhBuildSeconds : nullptr);
    const double sceneBuildWall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - bvhStart).count();
    if (!profilingEnabled || localBvhBuildSeconds <= 0.0) {
      localBvhBuildSeconds = sceneBuildWall;
    }

    localSceneTime =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - sceneStart).count();

    const auto distributionStart = std::chrono::steady_clock::now();
    const RowRange localRange = rowRangeForRank(mpi.rank, mpi.size, height);
    const std::uint64_t localPrimaryRays = static_cast<std::uint64_t>(localRange.count) *
                                           static_cast<std::uint64_t>(width) *
                                           static_cast<std::uint64_t>(samplesPerPixel);
    localPixelCount = static_cast<std::uint64_t>(localRange.count) * static_cast<std::uint64_t>(width);
    localSampleCount = localPixelCount * static_cast<std::uint64_t>(samplesPerPixel);
    resourceTracker.log_work_distribution(
        localRange.start, localRange.count, width, height, localPrimaryRays);

    std::vector<torirender::Vec3> localColors(
        static_cast<std::size_t>(localRange.count) * static_cast<std::size_t>(width),
        torirender::Vec3{});
    localMpiScatterSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - distributionStart).count();

    const std::uint64_t totalPixels =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    std::uint64_t localPixelsDone = 0;

    const auto renderStart = std::chrono::steady_clock::now();
    auto lastHeartbeat = renderStart;
    int currentHeartbeatSeconds =
        heartbeatSeconds > 0 ? heartbeatSeconds : kDefaultHeartbeatSeconds;

    const auto updateHeartbeat = [&]() {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(now - renderStart).count();

      if (!heartbeatOverridden && elapsed >= kLongRunThresholdSeconds) {
        currentHeartbeatSeconds = kLongRunHeartbeatSeconds;
      }

      const auto sinceLast =
          std::chrono::duration_cast<std::chrono::seconds>(now - lastHeartbeat).count();
      if (mpi.rank == 0 && sinceLast >= currentHeartbeatSeconds) {
        const std::uint64_t estimatedDone =
            mpi.size > 1
                ? std::min(totalPixels, localPixelsDone * static_cast<std::uint64_t>(mpi.size))
                : localPixelsDone;
        writeHeartbeat(paths.statusPath,
                       runId,
                       kBackendTag,
                       mode,
                       mpi.size,
                       desiredOmpThreads,
                       kIsGpuBinary ? mpi.size : 0,
                       estimatedDone,
                       totalPixels,
                       static_cast<double>(elapsed),
                       currentHeartbeatSeconds);
        lastHeartbeat = now;
      }
    };

    const int tileRows = mode == "parallel" ? 32 : 4;
    const auto kernelStart = std::chrono::steady_clock::now();
    if (mode == "parallel") {
#if defined(TORIRENDER_USE_OPENACC)
      if (kIsGpuBinary) {
        torirender::Vec3* colorBuffer = localColors.data();
        const std::size_t colorCount = localColors.size();
        const int rowStart = localRange.start;
        constexpr int kAccAsyncQueue = 1;
        localKernelsAsync = true;
        const auto gpuDataRegionStart = std::chrono::steady_clock::now();

#pragma acc data copyin(camera, scene) copyout(colorBuffer[0 : colorCount])
        {
          for (int tileOffset = 0; tileOffset < localRange.count; tileOffset += tileRows) {
            const int tileStart = tileOffset;
            const int tileEnd = std::min(localRange.count, tileStart + tileRows);
            const auto tileKernelStart = std::chrono::steady_clock::now();

#pragma acc parallel loop gang vector collapse(2) independent async(kAccAsyncQueue) \
    present(colorBuffer[0 : colorCount], camera, scene)
            for (int localY = tileStart; localY < tileEnd; ++localY) {
              for (int x = 0; x < width; ++x) {
                const int y = rowStart + localY;
                const std::size_t localIndex =
                    static_cast<std::size_t>(localY) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x);
                colorBuffer[localIndex] = renderPixelColor(
                    camera, scene, x, y, width, samplesPerPixel, maxDepth, config.camera.rngSeed);
              }
            }

#pragma acc wait(kAccAsyncQueue)
            const auto tileKernelEnd = std::chrono::steady_clock::now();
            const double tileKernelSeconds =
                std::chrono::duration<double>(tileKernelEnd - tileKernelStart).count();
            localKernelTime += tileKernelSeconds;
            localGpuActiveTime += tileKernelSeconds;
            localTileStats.add(tileKernelSeconds);
            ++localKernelLaunches;

            localPixelsDone +=
                static_cast<std::uint64_t>(tileEnd - tileStart) * static_cast<std::uint64_t>(width);
            ++localTileCount;
            updateHeartbeat();
          }
        }
        const auto gpuDataRegionEnd = std::chrono::steady_clock::now();
        const double dataRegionSeconds =
            std::chrono::duration<double>(gpuDataRegionEnd - gpuDataRegionStart).count();
        localTransferTime += std::max(0.0, dataRegionSeconds - localKernelTime);
        localRankComputeSeconds = localTileStats.sumSeconds;
      } else
#endif
      {
#if defined(TORIRENDER_USE_OPENMP)
        const auto ompRegionStart = std::chrono::steady_clock::now();
        std::vector<double> threadUsefulSeconds;
        if (profilingEnabled) {
          threadUsefulSeconds.assign(static_cast<std::size_t>(omp_get_max_threads()), 0.0);
        }
        std::chrono::steady_clock::time_point tileWallStart{};
#pragma omp parallel
        {
          for (int tileOffset = 0; tileOffset < localRange.count; tileOffset += tileRows) {
            const int tileStart = localRange.start + tileOffset;
            const int tileEnd = std::min(localRange.start + localRange.count, tileStart + tileRows);

#pragma omp single
            { tileWallStart = std::chrono::steady_clock::now(); }

#pragma omp for schedule(static)
            for (int y = tileStart; y < tileEnd; ++y) {
              const auto rowStart =
                  profilingEnabled ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
              for (int x = 0; x < width; ++x) {
                const std::size_t localIndex =
                    static_cast<std::size_t>(y - localRange.start) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x);
                localColors[localIndex] = renderPixelColor(
                    camera, scene, x, y, width, samplesPerPixel, maxDepth, config.camera.rngSeed);
              }
              if (profilingEnabled) {
                const int tid = omp_get_thread_num();
                if (tid >= 0 && static_cast<std::size_t>(tid) < threadUsefulSeconds.size()) {
                  threadUsefulSeconds[static_cast<std::size_t>(tid)] +=
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - rowStart)
                          .count();
                }
              }
            }

#pragma omp single
            {
              const double tileWallSeconds =
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - tileWallStart)
                      .count();
              localTileStats.add(tileWallSeconds);
              localPixelsDone +=
                  static_cast<std::uint64_t>(tileEnd - tileStart) * static_cast<std::uint64_t>(width);
              ++localTileCount;
              updateHeartbeat();
            }
          }
        }
        localOmpParallelRegionSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - ompRegionStart).count();
        if (profilingEnabled && !threadUsefulSeconds.empty()) {
          const double usefulSum = std::accumulate(
              threadUsefulSeconds.begin(), threadUsefulSeconds.end(), 0.0);
          localRankComputeSeconds =
              safeDivide(usefulSum, static_cast<double>(std::max(desiredOmpThreads, 1)));
        }
#else
        for (int tileOffset = 0; tileOffset < localRange.count; tileOffset += tileRows) {
          const int tileStart = localRange.start + tileOffset;
          const int tileEnd = std::min(localRange.start + localRange.count, tileStart + tileRows);
          const auto tileStartClock = std::chrono::steady_clock::now();

          for (int y = tileStart; y < tileEnd; ++y) {
            for (int x = 0; x < width; ++x) {
              const std::size_t localIndex =
                  static_cast<std::size_t>(y - localRange.start) * static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(x);
              localColors[localIndex] = renderPixelColor(
                  camera, scene, x, y, width, samplesPerPixel, maxDepth, config.camera.rngSeed);
            }
          }
          const double tileWallSeconds =
              std::chrono::duration<double>(std::chrono::steady_clock::now() - tileStartClock)
                  .count();
          localTileStats.add(tileWallSeconds);
          localPixelsDone +=
              static_cast<std::uint64_t>(tileEnd - tileStart) * static_cast<std::uint64_t>(width);
          ++localTileCount;
          updateHeartbeat();
        }
        localRankComputeSeconds = localTileStats.sumSeconds;
#endif
      }
    } else {
      for (int tileOffset = 0; tileOffset < localRange.count; tileOffset += tileRows) {
        const int tileStart = localRange.start + tileOffset;
        const int tileEnd = std::min(localRange.start + localRange.count, tileStart + tileRows);
        const auto tileStartClock = std::chrono::steady_clock::now();

        for (int y = tileStart; y < tileEnd; ++y) {
          for (int x = 0; x < width; ++x) {
            const std::size_t localIndex =
                static_cast<std::size_t>(y - localRange.start) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x);
            localColors[localIndex] = renderPixelColor(
                camera, scene, x, y, width, samplesPerPixel, maxDepth, config.camera.rngSeed);
          }
        }
        const double tileWallSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - tileStartClock)
                .count();
        localTileStats.add(tileWallSeconds);
        localPixelsDone +=
            static_cast<std::uint64_t>(tileEnd - tileStart) * static_cast<std::uint64_t>(width);
        ++localTileCount;
        updateHeartbeat();
      }
      localRankComputeSeconds = localTileStats.sumSeconds;
    }

    localRenderRegionWallSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - renderStart).count();

    if (localTileStats.count == 0 && localRange.count > 0) {
      localTileStats.add(localRenderRegionWallSeconds);
      localTileCount = 1;
    }
    if (localRankComputeSeconds <= 0.0) {
      localRankComputeSeconds =
          localTileStats.sumSeconds > 0.0 ? localTileStats.sumSeconds : localRenderRegionWallSeconds;
    }

    if (!(kIsGpuBinary
#if defined(TORIRENDER_USE_OPENACC)
          && mode == "parallel"
#endif
          )) {
      localKernelTime =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - kernelStart).count();
    }

    std::vector<torirender::Vec3> finalColors;

#if defined(TORIRENDER_USE_MPI)
    if (mpi.size > 1) {
      const std::size_t localBytes = localColors.size() * sizeof(torirender::Vec3);
      if (localBytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return mpiBail("Local MPI send buffer exceeds MPI int count limit.");
      }
      const int localCountBytes = static_cast<int>(localBytes);

      std::vector<int> recvCounts;
      std::vector<int> displacements;
      if (mpi.rank == 0) {
        recvCounts.resize(static_cast<std::size_t>(mpi.size), 0);
        displacements.resize(static_cast<std::size_t>(mpi.size), 0);
        finalColors.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                           torirender::Vec3{});
      }

      {
        const auto mpiCallStart = std::chrono::steady_clock::now();
        MPI_Gather(&localCountBytes,
                   1,
                   MPI_INT,
                   mpi.rank == 0 ? recvCounts.data() : nullptr,
                   1,
                   MPI_INT,
                   0,
                   MPI_COMM_WORLD);
        const double gatherSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - mpiCallStart).count();
        localMpiGatherSeconds += gatherSeconds;
        localMpiTime += gatherSeconds;
      }

      if (mpi.rank == 0) {
        std::size_t offsetBytes = 0;
        for (int i = 0; i < mpi.size; ++i) {
          if (offsetBytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return mpiBail("MPI displacement exceeds MPI int count limit.");
          }
          displacements[static_cast<std::size_t>(i)] = static_cast<int>(offsetBytes);
          offsetBytes += static_cast<std::size_t>(recvCounts[static_cast<std::size_t>(i)]);
        }

        const std::size_t expectedBytes = static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height) *
                                          sizeof(torirender::Vec3);
        if (offsetBytes != expectedBytes) {
          return mpiBail("MPI gather byte count mismatch with final image buffer size.");
        }
      }

      {
        const auto mpiCallStart = std::chrono::steady_clock::now();
        MPI_Gatherv(localColors.data(),
                    localCountBytes,
                    MPI_BYTE,
                    mpi.rank == 0 ? finalColors.data() : nullptr,
                    mpi.rank == 0 ? recvCounts.data() : nullptr,
                    mpi.rank == 0 ? displacements.data() : nullptr,
                    MPI_BYTE,
                    0,
                    MPI_COMM_WORLD);
        const double gatherSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - mpiCallStart).count();
        localMpiGatherSeconds += gatherSeconds;
        localMpiTime += gatherSeconds;
      }
    } else {
      finalColors = std::move(localColors);
    }
#else
    finalColors = std::move(localColors);
#endif

    const auto renderEnd = std::chrono::steady_clock::now();
    double elapsedSeconds = std::chrono::duration<double>(renderEnd - renderStart).count();

#if defined(TORIRENDER_USE_MPI)
    if (mpi.enabled) {
      double maxElapsed = elapsedSeconds;
      const auto mpiCallStart = std::chrono::steady_clock::now();
      MPI_Reduce(&elapsedSeconds, &maxElapsed, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
      const double reduceSeconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - mpiCallStart).count();
      localSynchronizationSeconds += reduceSeconds;
      localMpiTime += reduceSeconds;
      elapsedSeconds = maxElapsed;
    }
#endif

    if (mpi.rank == 0) {
      const auto outputStart = std::chrono::steady_clock::now();
      writeHeartbeat(paths.statusPath,
                     runId,
                     kBackendTag,
                     mode,
                     mpi.size,
                     desiredOmpThreads,
                     kIsGpuBinary ? detectedGpus : 0,
                     totalPixels,
                     totalPixels,
                     elapsedSeconds,
                     currentHeartbeatSeconds);

      torirender::Image image(width, height);
      if (!image.setPixels(std::move(finalColors))) {
        std::cerr << "Failed to transfer render buffer into image object.\n";
#if defined(TORIRENDER_USE_MPI)
        MPI_Abort(MPI_COMM_WORLD, 1);
#endif
        return 1;
      }

      if (!image.save(paths.imagePath.string())) {
        std::cerr << "Failed to save image: " << paths.imagePath.string() << '\n';
#if defined(TORIRENDER_USE_MPI)
        MPI_Abort(MPI_COMM_WORLD, 1);
#endif
        return 1;
      }

      if (!appendMetricsCsv(paths.metricsCsvPath,
                            runId,
                            timestampToken,
                            kBackendTag,
                            mode,
                            paths.imageFileName,
                            width,
                            height,
                            samplesPerPixel,
                            maxDepth,
                            mpi.size,
                            desiredOmpThreads,
                            kIsGpuBinary ? detectedGpus : 0,
                            elapsedSeconds,
                            errorMessage)) {
        std::cerr << "Warning: " << errorMessage << '\n';
      }
      localOutputTime =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - outputStart).count();

      std::cout << "ToriRender completed\n";
      std::cout << "  image: " << paths.imagePath.string() << '\n';
      std::cout << "  metrics: " << paths.metricsCsvPath.string() << '\n';
      std::cout << "  status: " << paths.statusPath.string() << '\n';
      std::cout << "  elapsed: " << std::fixed << std::setprecision(3) << elapsedSeconds << " s\n";
    }

    const auto rankEndSteady = std::chrono::steady_clock::now();
    const double localTotalTime =
        std::chrono::duration<double>(rankEndSteady - rankStartSteady).count();
    const std::string rankEndWall = wallClockNow();
    const CpuUsageSample cpuEndSample = captureCpuUsageSample();

    resourceTracker.set_section_seconds("scene", localSceneTime);
    resourceTracker.set_section_seconds("kernel", localKernelTime);
    resourceTracker.set_section_seconds("transfer", localTransferTime);
    resourceTracker.set_section_seconds("mpi", localMpiTime);
    resourceTracker.set_section_seconds("output", localOutputTime);
    resourceTracker.set_section_seconds("total", localTotalTime);

    resourceTracker.log_gpu_execution(
        localKernelLaunches, localKernelsAsync, localGpuActiveTime, localTransferTime);
    resourceTracker.log_cpu_usage(cpuEndSample.userSeconds - cpuStartSample.userSeconds,
                                  cpuEndSample.systemSeconds - cpuStartSample.systemSeconds,
                                  cpuEndSample.peakMemoryKb,
                                  localTotalTime);
    resourceTracker.set_total_runtime(localTotalTime);
    resourceTracker.set_rank_end_time(rankEndWall);

    {
      auto& record = resourceTracker.local_record();
      record.sceneTime = localSceneTime;
      record.kernelTime = localKernelTime;
      record.transferTime = localTransferTime;
      record.mpiTime = localMpiTime;
      record.outputTime = localOutputTime;
      record.totalTime = localTotalTime;
      record.raysProcessed = localPrimaryRays;
      record.kernelLaunches = localKernelLaunches;
      record.kernelsAsync = localKernelsAsync;
      record.gpuActiveTime = localGpuActiveTime;
      record.cpuUserTime = cpuEndSample.userSeconds - cpuStartSample.userSeconds;
      record.cpuSystemTime = cpuEndSample.systemSeconds - cpuStartSample.systemSeconds;
      record.cpuWallTime = localTotalTime;
      record.cpuIdleEstimate =
          std::max(0.0, localTotalTime - (record.cpuUserTime + record.cpuSystemTime));
      record.peakMemoryKb = cpuEndSample.peakMemoryKb;
      record.rankEndTime = rankEndWall;
    }

    RankPerfPacked localPerf{};
    std::vector<RankPerfPacked> gatheredPerf;
    if (profilingEnabled) {
      localPerf.rankComputeSeconds = localRankComputeSeconds;
      localPerf.rankTotalSeconds = localTotalTime;
      localPerf.rankRenderRegionSeconds = localRenderRegionWallSeconds;
      localPerf.tileComputeSumSeconds = localTileStats.sumSeconds;
      localPerf.tileComputeMaxSeconds = localTileStats.maxSeconds;
      localPerf.tileComputeMinSeconds = localTileStats.safeMin();
      localPerf.ompParallelRegionSeconds = localOmpParallelRegionSeconds;
      localPerf.tileCount = static_cast<unsigned long long>(localTileCount);
      localPerf.pixelCount = static_cast<unsigned long long>(localPixelCount);
      localPerf.sampleCount = static_cast<unsigned long long>(localSampleCount);
#if defined(TORIRENDER_USE_MPI)
      if (mpi.size > 1) {
        if (mpi.rank == 0) {
          gatheredPerf.resize(static_cast<std::size_t>(mpi.size));
        }
        MPI_Gather(&localPerf,
                   static_cast<int>(sizeof(localPerf)),
                   MPI_BYTE,
                   mpi.rank == 0 ? gatheredPerf.data() : nullptr,
                   static_cast<int>(sizeof(localPerf)),
                   MPI_BYTE,
                   0,
                   MPI_COMM_WORLD);
      } else
#endif
      {
        if (mpi.rank == 0) {
          gatheredPerf.push_back(localPerf);
        }
      }
    }

    const auto finalizationStart = std::chrono::steady_clock::now();
#if defined(TORIRENDER_USE_MPI)
    if (mpi.size > 1) {
      const torirender::runtime::ResourceRecordPacked localPacked =
          torirender::runtime::ResourceTracker::pack_record(resourceTracker.local_record());
      std::vector<torirender::runtime::ResourceRecordPacked> gathered;
      if (mpi.rank == 0) {
        gathered.resize(static_cast<std::size_t>(mpi.size));
      }

      MPI_Gather(&localPacked,
                 static_cast<int>(sizeof(localPacked)),
                 MPI_BYTE,
                 mpi.rank == 0 ? gathered.data() : nullptr,
                 static_cast<int>(sizeof(localPacked)),
                 MPI_BYTE,
                 0,
                 MPI_COMM_WORLD);

      if (mpi.rank == 0) {
        std::vector<torirender::runtime::ResourceRecord> records;
        records.reserve(gathered.size());
        for (const auto& packed : gathered) {
          records.push_back(torirender::runtime::ResourceTracker::unpack_record(packed));
        }
        resourceTracker.set_all_records(std::move(records));
      }
    }
#endif

    if (mpi.rank == 0) {
      std::string resourceError;
      if (!resourceTracker.finalize_and_write(&resourceError)) {
        std::cerr << "Warning: failed to write resource usage report: " << resourceError << '\n';
      } else {
        std::cout << "  resource metrics: " << (paths.outputDir / "resource_metrics.csv").string()
                  << '\n';
        std::cout << "  resource report: "
                  << (paths.outputDir / "run_reports" / ("resource_report_" + runId + ".txt")).string()
                  << '\n';
      }
    }
    localFinalizationSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - finalizationStart).count();

    double mpiFinalizeSeconds = 0.0;
#if defined(TORIRENDER_USE_MPI)
    if (mpi.initializedByApp) {
      const auto finalizeStart = std::chrono::steady_clock::now();
      MPI_Finalize();
      mpiFinalizeSeconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - finalizeStart).count();
      mpi.initializedByApp = false;
    }
#endif
    localFinalizationSeconds += mpiFinalizeSeconds;

    const double totalWallSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - runWallStart).count();

    if (profilingEnabled && mpi.rank == 0) {
      double maxRankComputeSeconds = 0.0;
      double minRankComputeSeconds = std::numeric_limits<double>::infinity();
      double meanRankComputeSeconds = 0.0;
      double maxRankTotalSeconds = 0.0;
      double rankRenderRegionMax = 0.0;
      double tileComputeSumSeconds = 0.0;
      double tileComputeMaxSeconds = 0.0;
      double tileComputeMinSeconds = std::numeric_limits<double>::infinity();
      std::uint64_t aggregatedTileCount = 0;
      std::uint64_t aggregatedPixelCount = 0;
      std::uint64_t aggregatedSampleCount = 0;

      for (const auto& rankPerf : gatheredPerf) {
        maxRankComputeSeconds = std::max(maxRankComputeSeconds, rankPerf.rankComputeSeconds);
        minRankComputeSeconds = std::min(minRankComputeSeconds, rankPerf.rankComputeSeconds);
        meanRankComputeSeconds += rankPerf.rankComputeSeconds;
        maxRankTotalSeconds = std::max(maxRankTotalSeconds, rankPerf.rankTotalSeconds);
        rankRenderRegionMax = std::max(rankRenderRegionMax, rankPerf.rankRenderRegionSeconds);
        tileComputeSumSeconds += rankPerf.tileComputeSumSeconds;
        tileComputeMaxSeconds = std::max(tileComputeMaxSeconds, rankPerf.tileComputeMaxSeconds);
        if (rankPerf.tileCount > 0ULL) {
          tileComputeMinSeconds = std::min(tileComputeMinSeconds, rankPerf.tileComputeMinSeconds);
        }
        aggregatedTileCount += static_cast<std::uint64_t>(rankPerf.tileCount);
        aggregatedPixelCount += static_cast<std::uint64_t>(rankPerf.pixelCount);
        aggregatedSampleCount += static_cast<std::uint64_t>(rankPerf.sampleCount);
      }

      if (!gatheredPerf.empty()) {
        meanRankComputeSeconds /= static_cast<double>(gatheredPerf.size());
      }
      if (!std::isfinite(minRankComputeSeconds)) {
        minRankComputeSeconds = 0.0;
      }
      if (!std::isfinite(tileComputeMinSeconds)) {
        tileComputeMinSeconds = 0.0;
      }

      const double loadImbalanceSeconds =
          std::max(0.0, maxRankComputeSeconds - meanRankComputeSeconds);
      const double loadImbalanceRatio =
          meanRankComputeSeconds > 0.0 ? (maxRankComputeSeconds / meanRankComputeSeconds) : 0.0;

      const double preRenderWallSeconds =
          std::chrono::duration<double>(sceneStart - runWallStart).count() + localSceneTime;
      const double serialConfigSetupSeconds = std::max(
          0.0,
          preRenderWallSeconds - sectionProfiler.seconds("scene_parse_seconds") - localCameraSetupSeconds -
              localBvhBuildSeconds - mpiInitSeconds - localMpiBroadcastSeconds -
              localMpiScatterSeconds - localSyncBeforeRenderSeconds);

      const double sigmaSetupSeconds =
          sectionProfiler.seconds("scene_parse_seconds") + localCameraSetupSeconds +
          localBvhBuildSeconds + serialConfigSetupSeconds + localOutputTime;

      const double communicationOverheadSeconds =
          localMpiBroadcastSeconds + localMpiScatterSeconds + localMpiGatherSeconds;
      const double synchronizationOverheadSeconds = localSynchronizationSeconds;
      const double schedulingOverheadSeconds =
          std::max(0.0, localOmpParallelRegionSeconds - localTileStats.sumSeconds);
      const double outputOverheadSeconds = localOutputTime;

      BaselineQuery baselineQuery{};
      baselineQuery.sceneFile = cli.configPath;
      baselineQuery.width = width;
      baselineQuery.height = height;
      baselineQuery.samplesPerPixel = samplesPerPixel;
      baselineQuery.maxDepth = maxDepth;

      std::optional<double> baselineSeconds;
      if (mode == "serial") {
        baselineSeconds = totalWallSeconds;
      } else {
        baselineSeconds = findSerialBaselineSeconds(cli.perfDir, baselineQuery);
      }

      if (!baselineSeconds.has_value() && mode != "serial") {
        std::cerr << "Warning: no matching serial baseline found for profiling model decomposition.\n";
      }

      const double tsSerialBaselineSeconds = baselineSeconds.value_or(0.0);
      const double sigmaSeconds = sigmaSetupSeconds;
      const double phiSerialSeconds = tsSerialBaselineSeconds > 0.0
                                          ? (tsSerialBaselineSeconds - sigmaSeconds)
                                          : 0.0;
      const double idealPhiOverPSeconds = pEffective > 0
                                              ? safeDivide(phiSerialSeconds, static_cast<double>(pEffective))
                                              : 0.0;
      const double kappaEstimatedRawSeconds = tsSerialBaselineSeconds > 0.0
                                                  ? (totalWallSeconds - sigmaSeconds - idealPhiOverPSeconds)
                                                  : 0.0;
      const double kappaEstimatedClampedSeconds = std::max(0.0, kappaEstimatedRawSeconds);
      const double tpModelReconstructedSeconds =
          sigmaSeconds + idealPhiOverPSeconds + kappaEstimatedRawSeconds;
      const double overheadFractionOfTp = safeDivide(kappaEstimatedRawSeconds, totalWallSeconds);
      const double sigmaFractionOfTs = safeDivide(sigmaSeconds, tsSerialBaselineSeconds);
      const double phiFractionOfTs = safeDivide(phiSerialSeconds, tsSerialBaselineSeconds);

      const double speedup = safeDivide(tsSerialBaselineSeconds, totalWallSeconds);
      const double efficiency = safeDivide(speedup, static_cast<double>(pEffective));
      const double f = safeDivide(sigmaSeconds, tsSerialBaselineSeconds);
      const double amdahlIdealSpeedup =
          (tsSerialBaselineSeconds > 0.0 && pEffective > 0)
              ? safeDivide(1.0, f + (1.0 - f) / static_cast<double>(pEffective))
              : 0.0;
      const double karpFlattE = (pEffective > 1 && speedup > 0.0)
                                    ? safeDivide((1.0 / speedup) - (1.0 / static_cast<double>(pEffective)),
                                                 1.0 - (1.0 / static_cast<double>(pEffective)))
                                    : 0.0;
      const double parallelEfficiencyLoss = pEffective > 0 ? (1.0 - efficiency) : 0.0;

      const double otherOverheadSeconds =
          kappaEstimatedRawSeconds - communicationOverheadSeconds - synchronizationOverheadSeconds -
          schedulingOverheadSeconds - loadImbalanceSeconds - outputOverheadSeconds;

      sectionProfiler.add_seconds("total_wall_seconds", totalWallSeconds);
      sectionProfiler.add_seconds("sigma_setup_seconds", sigmaSetupSeconds);
      sectionProfiler.add_seconds("camera_setup_seconds", localCameraSetupSeconds);
      sectionProfiler.add_seconds("bvh_build_seconds", localBvhBuildSeconds);
      sectionProfiler.add_seconds("mpi_broadcast_seconds", localMpiBroadcastSeconds);
      sectionProfiler.add_seconds("mpi_scatter_or_task_distribution_seconds", localMpiScatterSeconds);
      sectionProfiler.add_seconds("render_region_wall_seconds", rankRenderRegionMax);
      sectionProfiler.add_seconds("omp_parallel_region_seconds", localOmpParallelRegionSeconds);
      sectionProfiler.add_seconds("tile_compute_sum_seconds", tileComputeSumSeconds);
      sectionProfiler.add_seconds("tile_compute_max_seconds", tileComputeMaxSeconds);
      sectionProfiler.add_seconds("tile_compute_min_seconds", tileComputeMinSeconds);
      sectionProfiler.add_seconds("mpi_gather_seconds", localMpiGatherSeconds);
      sectionProfiler.add_seconds("output_write_seconds", localOutputTime);
      sectionProfiler.add_seconds("synchronization_seconds", localSynchronizationSeconds);
      sectionProfiler.add_seconds("finalization_seconds", localFinalizationSeconds);

      std::vector<torirender::runtime::CsvField> fields;
      fields.reserve(128);
      addField(fields, "run_id", runId);
      addField(fields, "timestamp", timestampToken);
      addField(fields, "backend", kBackendTag);
      addField(fields, "mode", mode);
      addField(fields, "run_label", cli.runLabel);
      addField(fields, "image_width", width);
      addField(fields, "image_height", height);
      addField(fields, "resolution", std::to_string(width) + "x" + std::to_string(height));
      addField(fields, "samples_per_pixel", samplesPerPixel);
      addField(fields, "max_depth", maxDepth);
      addField(fields, "mpi_ranks", mpi.size);
      addField(fields, "omp_threads", desiredOmpThreads);
      addField(fields, "p_effective", pEffective);
      addField(fields, "scene_file", normalizeScenePath(cli.configPath));
      addField(fields, "output_file", paths.imagePath.string());
      addField(fields, "git_commit_if_available", TORIRENDER_GIT_COMMIT);

      addField(fields, "total_wall_seconds", totalWallSeconds);
      addField(fields, "sigma_setup_seconds", sigmaSetupSeconds);
      addField(fields, "scene_parse_seconds", sectionProfiler.seconds("scene_parse_seconds"));
      addField(fields, "bvh_build_seconds", localBvhBuildSeconds);
      addField(fields, "camera_setup_seconds", localCameraSetupSeconds);
      addField(fields, "mpi_init_seconds", mpiInitSeconds);
      addField(fields, "mpi_broadcast_seconds", localMpiBroadcastSeconds);
      addField(fields,
               "mpi_scatter_or_task_distribution_seconds",
               localMpiScatterSeconds);
      addField(fields, "render_region_wall_seconds", rankRenderRegionMax);
      addField(fields, "omp_parallel_region_seconds", localOmpParallelRegionSeconds);
      addField(fields, "tile_compute_sum_seconds", tileComputeSumSeconds);
      addField(fields, "tile_compute_max_seconds", tileComputeMaxSeconds);
      addField(fields, "tile_compute_min_seconds", tileComputeMinSeconds);
      addField(fields, "mpi_gather_seconds", localMpiGatherSeconds);
      addField(fields, "output_write_seconds", localOutputTime);
      addField(fields, "synchronization_seconds", localSynchronizationSeconds);
      addField(fields, "finalization_seconds", localFinalizationSeconds);

      addField(fields, "max_rank_compute_seconds", maxRankComputeSeconds);
      addField(fields, "mean_rank_compute_seconds", meanRankComputeSeconds);
      addField(fields, "min_rank_compute_seconds", minRankComputeSeconds);
      addField(fields, "max_rank_total_seconds", maxRankTotalSeconds);
      addField(fields, "rank_tile_count_sum", aggregatedTileCount);
      addField(fields, "rank_pixel_count_sum", aggregatedPixelCount);
      addField(fields, "rank_sample_count_sum", aggregatedSampleCount);
      addField(fields, "load_imbalance_seconds", loadImbalanceSeconds);
      addField(fields, "load_imbalance_ratio", loadImbalanceRatio);

      addField(fields, "communication_overhead_seconds", communicationOverheadSeconds);
      addField(fields, "synchronization_overhead_seconds", synchronizationOverheadSeconds);
      addField(fields, "scheduling_overhead_seconds", schedulingOverheadSeconds);
      addField(fields, "output_overhead_seconds", outputOverheadSeconds);
      addField(fields, "other_overhead_seconds", otherOverheadSeconds);

      addField(fields, "Ts_serial_baseline_seconds", tsSerialBaselineSeconds);
      addField(fields, "sigma_seconds", sigmaSeconds);
      addField(fields, "phi_serial_seconds", phiSerialSeconds);
      addField(fields, "ideal_phi_over_p_seconds", idealPhiOverPSeconds);
      addField(fields, "kappa_estimated_seconds", kappaEstimatedRawSeconds);
      addField(fields, "kappa_estimated_clamped_seconds", kappaEstimatedClampedSeconds);
      addField(fields, "Tp_model_reconstructed_seconds", tpModelReconstructedSeconds);
      addField(fields, "overhead_fraction_of_Tp", overheadFractionOfTp);
      addField(fields, "sigma_fraction_of_Ts", sigmaFractionOfTs);
      addField(fields, "phi_fraction_of_Ts", phiFractionOfTs);

      addField(fields, "speedup", speedup);
      addField(fields, "efficiency", efficiency);
      addField(fields, "amdahl_ideal_speedup_from_measured_sigma", amdahlIdealSpeedup);
      addField(fields, "karp_flatt_e", karpFlattE);
      addField(fields, "parallel_efficiency_loss", parallelEfficiencyLoss);

      addField(fields, "serial_baseline_found", baselineSeconds.has_value() ? 1 : 0);

      const std::filesystem::path summaryReportPath =
          std::filesystem::path(cli.perfDir) / (runId + ".txt");
      std::string perfError;
      if (!writeProfileTextReport(summaryReportPath, fields, perfError)) {
        std::cerr << "Warning: failed to write profiling report: " << perfError << '\n';
      }
    }

    if (profilingEnabled && cli.profilePerRank) {
      const std::filesystem::path perRankPath =
          std::filesystem::path(cli.perfDir) /
          (runId + "_rank" + std::to_string(mpi.rank) + ".txt");

      std::vector<torirender::runtime::CsvField> rankFields;
      addField(rankFields, "run_id", runId);
      addField(rankFields, "timestamp", timestampToken);
      addField(rankFields, "mode", mode);
      addField(rankFields, "backend", kBackendTag);
      addField(rankFields, "rank", mpi.rank);
      addField(rankFields, "mpi_ranks", mpi.size);
      addField(rankFields, "omp_threads", desiredOmpThreads);
      addField(rankFields, "rank_total_seconds", localTotalTime);
      addField(rankFields, "rank_compute_seconds", localRankComputeSeconds);
      addField(rankFields, "rank_render_region_seconds", localRenderRegionWallSeconds);
      addField(rankFields, "tile_count", localTileCount);
      addField(rankFields, "pixel_count", localPixelCount);
      addField(rankFields, "sample_count", localSampleCount);
      addField(rankFields, "tile_compute_sum_seconds", localTileStats.sumSeconds);
      addField(rankFields, "tile_compute_max_seconds", localTileStats.maxSeconds);
      addField(rankFields, "tile_compute_min_seconds", localTileStats.safeMin());

      std::string perRankError;
      if (!writeProfileTextReport(perRankPath, rankFields, perRankError)) {
        std::cerr << "Warning: failed to write per-rank profiling report: " << perRankError
                  << '\n';
      }
    }

    return 0;
  }

  std::cerr << "Internal error: unexpected render setup scope exit.\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  return runMain(argc, argv);
}
