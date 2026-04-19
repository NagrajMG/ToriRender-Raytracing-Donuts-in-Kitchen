#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
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
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/Accel.hpp"
#include "core/Camera.hpp"
#include "core/Ray.hpp"
#include "io/Image.hpp"
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
               " [--omp-threads N] [--heartbeat N]\n";
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
  CliOptions cli;
  if (!parseArgs(argc, argv, cli)) {
    printUsage(argv[0]);
    return 1;
  }

  torirender::SceneConfig config{};
  std::string errorMessage;
  if (!torirender::loadSceneConfigFromJsonFile(cli.configPath, config, &errorMessage)) {
    std::cerr << "Failed to load scene config from " << cli.configPath << ": " << errorMessage
              << '\n';
    return 1;
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
    localMpiTime +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - mpiCallStart).count();
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
    localMpiTime +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - mpiCallStart).count();
  }
#endif

  const auto sceneStart = std::chrono::steady_clock::now();
  const torirender::Camera camera(config.camera.lookFrom,
                                  config.camera.lookAt,
                                  config.camera.viewUp,
                                  config.camera.vfov,
                                  static_cast<double>(width) / static_cast<double>(height),
                                  width,
                                  height);
  const torirender::Scene scene(config);
  localSceneTime =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - sceneStart).count();

  const RowRange localRange = rowRangeForRank(mpi.rank, mpi.size, height);
  const std::uint64_t localPrimaryRays = static_cast<std::uint64_t>(localRange.count) *
                                         static_cast<std::uint64_t>(width) *
                                         static_cast<std::uint64_t>(samplesPerPixel);
  resourceTracker.log_work_distribution(
      localRange.start, localRange.count, width, height, localPrimaryRays);

  std::vector<torirender::Vec3> localColors(
      static_cast<std::size_t>(localRange.count) * static_cast<std::size_t>(width),
      torirender::Vec3{});

  const std::uint64_t totalPixels =
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  std::uint64_t localPixelsDone = 0;

  const auto renderStart = std::chrono::steady_clock::now();
  auto lastHeartbeat = renderStart;
  int currentHeartbeatSeconds = heartbeatSeconds > 0 ? heartbeatSeconds : kDefaultHeartbeatSeconds;

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

          // Keep heartbeat on host-side, but sync once per tile batch.
#pragma acc wait(kAccAsyncQueue)
          const auto tileKernelEnd = std::chrono::steady_clock::now();
          const double tileKernelSeconds =
              std::chrono::duration<double>(tileKernelEnd - tileKernelStart).count();
          localKernelTime += tileKernelSeconds;
          localGpuActiveTime += tileKernelSeconds;
          ++localKernelLaunches;

          localPixelsDone +=
              static_cast<std::uint64_t>(tileEnd - tileStart) * static_cast<std::uint64_t>(width);
          updateHeartbeat();
        }
      }
      const auto gpuDataRegionEnd = std::chrono::steady_clock::now();
      const double dataRegionSeconds =
          std::chrono::duration<double>(gpuDataRegionEnd - gpuDataRegionStart).count();
      localTransferTime += std::max(0.0, dataRegionSeconds - localKernelTime);
    } else
#endif
    {
#if defined(TORIRENDER_USE_OPENMP)
#pragma omp parallel
      {
        for (int tileOffset = 0; tileOffset < localRange.count; tileOffset += tileRows) {
          const int tileStart = localRange.start + tileOffset;
          const int tileEnd = std::min(localRange.start + localRange.count, tileStart + tileRows);

#pragma omp for schedule(static)
          for (int y = tileStart; y < tileEnd; ++y) {
            for (int x = 0; x < width; ++x) {
              const std::size_t localIndex =
                  static_cast<std::size_t>(y - localRange.start) * static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(x);
              localColors[localIndex] = renderPixelColor(
                  camera, scene, x, y, width, samplesPerPixel, maxDepth, config.camera.rngSeed);
            }
          }

#pragma omp single
          {
            localPixelsDone +=
                static_cast<std::uint64_t>(tileEnd - tileStart) * static_cast<std::uint64_t>(width);
            updateHeartbeat();
          }
        }
      }
#else
      for (int tileOffset = 0; tileOffset < localRange.count; tileOffset += tileRows) {
        const int tileStart = localRange.start + tileOffset;
        const int tileEnd = std::min(localRange.start + localRange.count, tileStart + tileRows);

        for (int y = tileStart; y < tileEnd; ++y) {
          for (int x = 0; x < width; ++x) {
            const std::size_t localIndex =
                static_cast<std::size_t>(y - localRange.start) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x);
            localColors[localIndex] = renderPixelColor(
                camera, scene, x, y, width, samplesPerPixel, maxDepth, config.camera.rngSeed);
          }
        }

        localPixelsDone +=
            static_cast<std::uint64_t>(tileEnd - tileStart) * static_cast<std::uint64_t>(width);
        updateHeartbeat();
      }
#endif
    }
  } else {
    for (int tileOffset = 0; tileOffset < localRange.count; tileOffset += tileRows) {
      const int tileStart = localRange.start + tileOffset;
      const int tileEnd = std::min(localRange.start + localRange.count, tileStart + tileRows);

      for (int y = tileStart; y < tileEnd; ++y) {
        for (int x = 0; x < width; ++x) {
          const std::size_t localIndex =
              static_cast<std::size_t>(y - localRange.start) * static_cast<std::size_t>(width) +
              static_cast<std::size_t>(x);
          localColors[localIndex] = renderPixelColor(
              camera, scene, x, y, width, samplesPerPixel, maxDepth, config.camera.rngSeed);
        }
      }

      localPixelsDone +=
          static_cast<std::uint64_t>(tileEnd - tileStart) * static_cast<std::uint64_t>(width);
      updateHeartbeat();
    }
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
      localMpiTime +=
          std::chrono::duration<double>(std::chrono::steady_clock::now() - mpiCallStart).count();
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
                                        static_cast<std::size_t>(height) * sizeof(torirender::Vec3);
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
      localMpiTime +=
          std::chrono::duration<double>(std::chrono::steady_clock::now() - mpiCallStart).count();
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
    localMpiTime +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - mpiCallStart).count();
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
      std::cout
          << "  resource report: "
          << (paths.outputDir / "run_reports" / ("resource_report_" + runId + ".txt")).string()
          << '\n';
    }
  }

#if defined(TORIRENDER_USE_MPI)
  if (mpi.initializedByApp) {
    MPI_Finalize();
    mpi.initializedByApp = false;
  }
#endif

  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return runMain(argc, argv);
}
