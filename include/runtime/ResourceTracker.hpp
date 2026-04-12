#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace torirender::runtime {

struct ResourceRecord {
  std::string runId;

  int rank = 0;
  int totalRanks = 1;
  std::string hostname = "unknown";
  int ncpus = 0;

  int gpuId = -1;
  int ngpus = 0;
  bool gpuShared = false;

  int ompThreads = 1;

  std::string rankStartTime;
  std::string rankEndTime;

  double sceneTime = 0.0;
  double kernelTime = 0.0;
  double transferTime = 0.0;
  double mpiTime = 0.0;
  double outputTime = 0.0;
  double totalTime = 0.0;

  std::uint64_t raysProcessed = 0;

  int rowStart = 0;
  int rowCount = 0;
  int imageWidth = 0;
  int imageHeight = 0;

  std::uint64_t kernelLaunches = 0;
  bool kernelsAsync = false;
  double gpuActiveTime = 0.0;

  double cpuUserTime = 0.0;
  double cpuSystemTime = 0.0;
  long long peakMemoryKb = 0;
  double cpuWallTime = 0.0;
  double cpuIdleEstimate = 0.0;
};

struct ResourceRecordPacked {
  char runId[96]{};
  int rank = 0;
  int totalRanks = 1;
  char hostname[64]{};
  int ncpus = 0;

  int gpuId = -1;
  int ngpus = 0;
  int gpuShared = 0;

  int ompThreads = 1;

  char rankStartTime[40]{};
  char rankEndTime[40]{};

  double sceneTime = 0.0;
  double kernelTime = 0.0;
  double transferTime = 0.0;
  double mpiTime = 0.0;
  double outputTime = 0.0;
  double totalTime = 0.0;

  unsigned long long raysProcessed = 0ULL;

  int rowStart = 0;
  int rowCount = 0;
  int imageWidth = 0;
  int imageHeight = 0;

  unsigned long long kernelLaunches = 0ULL;
  int kernelsAsync = 0;
  double gpuActiveTime = 0.0;

  double cpuUserTime = 0.0;
  double cpuSystemTime = 0.0;
  long long peakMemoryKb = 0;
  double cpuWallTime = 0.0;
  double cpuIdleEstimate = 0.0;
};

class ResourceTracker {
 public:
  explicit ResourceTracker(std::string runId,
                           std::filesystem::path outputRoot = std::filesystem::path("output"));

  // Section timers for coarse runtime phases.
  void start_section(const std::string& name);
  void end_section(const std::string& name);
  void set_section_seconds(const std::string& name, double seconds);
  double section_seconds(const std::string& name) const;

  // Context loggers required by runtime integration.
  void log_mpi_info(int rank,
                    int totalRanks,
                    const std::string& hostname,
                    int ncpus,
                    const std::string& rankStartTime,
                    const std::string& rankEndTime = std::string());
  void log_gpu_info(int gpuId, int totalGpus);
  void log_cpu_threads(int ompThreads);
  void log_work_distribution(
      int rowStart, int rowCount, int imageWidth, int imageHeight, std::uint64_t raysProcessed);
  void log_gpu_execution(std::uint64_t kernelLaunches,
                         bool kernelsAsync,
                         double gpuActiveTime,
                         double transferTime);
  void log_cpu_usage(double userSeconds,
                     double systemSeconds,
                     long long peakMemoryKb,
                     double cpuWallTime);
  void set_total_runtime(double totalSeconds);
  void set_rank_end_time(const std::string& rankEndTime);

  const ResourceRecord& local_record() const;
  ResourceRecord& local_record();

  void set_all_records(std::vector<ResourceRecord> records);
  const std::vector<ResourceRecord>& all_records() const;

  static ResourceRecordPacked pack_record(const ResourceRecord& record);
  static ResourceRecord unpack_record(const ResourceRecordPacked& packed);

  bool finalize_and_write(std::string* errorMessage = nullptr);

 private:
  void apply_section_totals(ResourceRecord& record) const;
  std::vector<std::string> build_warnings(std::vector<ResourceRecord>* records) const;
  bool append_csv(const std::filesystem::path& csvPath,
                  const std::vector<ResourceRecord>& records,
                  std::string* errorMessage) const;
  bool write_report(const std::filesystem::path& reportPath,
                    const std::vector<ResourceRecord>& records,
                    const std::vector<std::string>& warnings,
                    std::string* errorMessage) const;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> sectionStarts_;
  std::unordered_map<std::string, double> sectionSeconds_;
  ResourceRecord local_;
  std::vector<ResourceRecord> allRecords_;
  std::filesystem::path outputRoot_;
};

}  // namespace torirender::runtime
