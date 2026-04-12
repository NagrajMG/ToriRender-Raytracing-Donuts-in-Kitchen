#include "runtime/ResourceTracker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace torirender::runtime {

namespace {

template <std::size_t N>
void copyStringToFixed(const std::string& value, char (&dest)[N]) {
  static_assert(N > 0, "fixed-size destination must be non-empty");
  std::snprintf(dest, N, "%s", value.c_str());
}

std::string trimFixedString(const char* src) {
  if (src == nullptr) {
    return {};
  }
  return std::string(src);
}

double safeNonNegative(double value) {
  return value < 0.0 ? 0.0 : value;
}

}  // namespace

ResourceTracker::ResourceTracker(std::string runId, std::filesystem::path outputRoot)
    : outputRoot_(std::move(outputRoot)) {
  local_.runId = std::move(runId);
}

void ResourceTracker::start_section(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  sectionStarts_[name] = std::chrono::steady_clock::now();
}

void ResourceTracker::end_section(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sectionStarts_.find(name);
  if (it == sectionStarts_.end()) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(now - it->second).count();
  sectionSeconds_[name] += elapsed;
  sectionStarts_.erase(it);
}

void ResourceTracker::set_section_seconds(const std::string& name, double seconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  sectionSeconds_[name] = safeNonNegative(seconds);
}

double ResourceTracker::section_seconds(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sectionSeconds_.find(name);
  return it == sectionSeconds_.end() ? 0.0 : it->second;
}

void ResourceTracker::log_mpi_info(int rank,
                                   int totalRanks,
                                   const std::string& hostname,
                                   int ncpus,
                                   const std::string& rankStartTime,
                                   const std::string& rankEndTime) {
  std::lock_guard<std::mutex> lock(mutex_);
  local_.rank = rank;
  local_.totalRanks = std::max(totalRanks, 1);
  local_.hostname = hostname.empty() ? std::string("unknown") : hostname;
  local_.ncpus = std::max(ncpus, 0);
  local_.rankStartTime = rankStartTime;
  if (!rankEndTime.empty()) {
    local_.rankEndTime = rankEndTime;
  }
}

void ResourceTracker::log_gpu_info(int gpuId, int totalGpus) {
  std::lock_guard<std::mutex> lock(mutex_);
  local_.gpuId = gpuId;
  local_.ngpus = std::max(totalGpus, 0);
}

void ResourceTracker::log_cpu_threads(int ompThreads) {
  std::lock_guard<std::mutex> lock(mutex_);
  local_.ompThreads = std::max(ompThreads, 1);
}

void ResourceTracker::log_work_distribution(
    int rowStart, int rowCount, int imageWidth, int imageHeight, std::uint64_t raysProcessed) {
  std::lock_guard<std::mutex> lock(mutex_);
  local_.rowStart = std::max(rowStart, 0);
  local_.rowCount = std::max(rowCount, 0);
  local_.imageWidth = std::max(imageWidth, 0);
  local_.imageHeight = std::max(imageHeight, 0);
  local_.raysProcessed = raysProcessed;
}

void ResourceTracker::log_gpu_execution(std::uint64_t kernelLaunches,
                                        bool kernelsAsync,
                                        double gpuActiveTime,
                                        double transferTime) {
  std::lock_guard<std::mutex> lock(mutex_);
  local_.kernelLaunches = kernelLaunches;
  local_.kernelsAsync = kernelsAsync;
  local_.gpuActiveTime = safeNonNegative(gpuActiveTime);
  local_.kernelTime = safeNonNegative(gpuActiveTime);
  local_.transferTime = safeNonNegative(transferTime);
}

void ResourceTracker::log_cpu_usage(double userSeconds,
                                    double systemSeconds,
                                    long long peakMemoryKb,
                                    double cpuWallTime) {
  std::lock_guard<std::mutex> lock(mutex_);
  local_.cpuUserTime = safeNonNegative(userSeconds);
  local_.cpuSystemTime = safeNonNegative(systemSeconds);
  local_.peakMemoryKb = std::max(peakMemoryKb, 0LL);
  local_.cpuWallTime = safeNonNegative(cpuWallTime);
  local_.cpuIdleEstimate =
      safeNonNegative(local_.cpuWallTime - (local_.cpuUserTime + local_.cpuSystemTime));
}

void ResourceTracker::set_total_runtime(double totalSeconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  local_.totalTime = safeNonNegative(totalSeconds);
}

void ResourceTracker::set_rank_end_time(const std::string& rankEndTime) {
  std::lock_guard<std::mutex> lock(mutex_);
  local_.rankEndTime = rankEndTime;
}

const ResourceRecord& ResourceTracker::local_record() const {
  return local_;
}

ResourceRecord& ResourceTracker::local_record() {
  return local_;
}

void ResourceTracker::set_all_records(std::vector<ResourceRecord> records) {
  std::lock_guard<std::mutex> lock(mutex_);
  allRecords_ = std::move(records);
}

const std::vector<ResourceRecord>& ResourceTracker::all_records() const {
  return allRecords_;
}

ResourceRecordPacked ResourceTracker::pack_record(const ResourceRecord& record) {
  ResourceRecordPacked packed{};

  copyStringToFixed(record.runId, packed.runId);
  packed.rank = record.rank;
  packed.totalRanks = record.totalRanks;
  copyStringToFixed(record.hostname, packed.hostname);
  packed.ncpus = record.ncpus;

  packed.gpuId = record.gpuId;
  packed.ngpus = record.ngpus;
  packed.gpuShared = record.gpuShared ? 1 : 0;
  packed.ompThreads = record.ompThreads;

  copyStringToFixed(record.rankStartTime, packed.rankStartTime);
  copyStringToFixed(record.rankEndTime, packed.rankEndTime);

  packed.sceneTime = record.sceneTime;
  packed.kernelTime = record.kernelTime;
  packed.transferTime = record.transferTime;
  packed.mpiTime = record.mpiTime;
  packed.outputTime = record.outputTime;
  packed.totalTime = record.totalTime;

  packed.raysProcessed = static_cast<unsigned long long>(record.raysProcessed);
  packed.rowStart = record.rowStart;
  packed.rowCount = record.rowCount;
  packed.imageWidth = record.imageWidth;
  packed.imageHeight = record.imageHeight;

  packed.kernelLaunches = static_cast<unsigned long long>(record.kernelLaunches);
  packed.kernelsAsync = record.kernelsAsync ? 1 : 0;
  packed.gpuActiveTime = record.gpuActiveTime;

  packed.cpuUserTime = record.cpuUserTime;
  packed.cpuSystemTime = record.cpuSystemTime;
  packed.peakMemoryKb = record.peakMemoryKb;
  packed.cpuWallTime = record.cpuWallTime;
  packed.cpuIdleEstimate = record.cpuIdleEstimate;

  return packed;
}

ResourceRecord ResourceTracker::unpack_record(const ResourceRecordPacked& packed) {
  ResourceRecord record{};
  record.runId = trimFixedString(packed.runId);
  record.rank = packed.rank;
  record.totalRanks = packed.totalRanks;
  record.hostname = trimFixedString(packed.hostname);
  record.ncpus = packed.ncpus;

  record.gpuId = packed.gpuId;
  record.ngpus = packed.ngpus;
  record.gpuShared = packed.gpuShared != 0;
  record.ompThreads = packed.ompThreads;

  record.rankStartTime = trimFixedString(packed.rankStartTime);
  record.rankEndTime = trimFixedString(packed.rankEndTime);

  record.sceneTime = packed.sceneTime;
  record.kernelTime = packed.kernelTime;
  record.transferTime = packed.transferTime;
  record.mpiTime = packed.mpiTime;
  record.outputTime = packed.outputTime;
  record.totalTime = packed.totalTime;

  record.raysProcessed = static_cast<std::uint64_t>(packed.raysProcessed);
  record.rowStart = packed.rowStart;
  record.rowCount = packed.rowCount;
  record.imageWidth = packed.imageWidth;
  record.imageHeight = packed.imageHeight;

  record.kernelLaunches = static_cast<std::uint64_t>(packed.kernelLaunches);
  record.kernelsAsync = packed.kernelsAsync != 0;
  record.gpuActiveTime = packed.gpuActiveTime;

  record.cpuUserTime = packed.cpuUserTime;
  record.cpuSystemTime = packed.cpuSystemTime;
  record.peakMemoryKb = packed.peakMemoryKb;
  record.cpuWallTime = packed.cpuWallTime;
  record.cpuIdleEstimate = packed.cpuIdleEstimate;
  return record;
}

void ResourceTracker::apply_section_totals(ResourceRecord& record) const {
  const auto readSection = [&](const std::string& a, const std::string& b = std::string()) {
    const auto ita = sectionSeconds_.find(a);
    if (ita != sectionSeconds_.end()) {
      return ita->second;
    }
    if (!b.empty()) {
      const auto itb = sectionSeconds_.find(b);
      if (itb != sectionSeconds_.end()) {
        return itb->second;
      }
    }
    return 0.0;
  };

  if (record.sceneTime <= 0.0) {
    record.sceneTime = readSection("scene", "scene_setup");
  }
  if (record.kernelTime <= 0.0) {
    record.kernelTime = readSection("kernel", "compute");
  }
  if (record.transferTime <= 0.0) {
    record.transferTime = readSection("transfer", "h2d_d2h");
  }
  if (record.mpiTime <= 0.0) {
    record.mpiTime = readSection("mpi", "communication");
  }
  if (record.outputTime <= 0.0) {
    record.outputTime = readSection("output", "write");
  }
  if (record.totalTime <= 0.0) {
    record.totalTime = readSection("total", "runtime");
  }

  record.sceneTime = safeNonNegative(record.sceneTime);
  record.kernelTime = safeNonNegative(record.kernelTime);
  record.transferTime = safeNonNegative(record.transferTime);
  record.mpiTime = safeNonNegative(record.mpiTime);
  record.outputTime = safeNonNegative(record.outputTime);
  record.totalTime = safeNonNegative(record.totalTime);
}

std::vector<std::string> ResourceTracker::build_warnings(
    std::vector<ResourceRecord>* records) const {
  std::vector<std::string> warnings;
  if (records == nullptr || records->empty()) {
    return warnings;
  }

  std::unordered_map<std::string, int> gpuUseCount;
  std::unordered_map<std::string, int> hostMaxGpus;
  for (const auto& record : *records) {
    if (record.gpuId >= 0) {
      const std::string key = record.hostname + "#" + std::to_string(record.gpuId);
      ++gpuUseCount[key];
    }
    auto it = hostMaxGpus.find(record.hostname);
    if (it == hostMaxGpus.end()) {
      hostMaxGpus.emplace(record.hostname, std::max(record.ngpus, 0));
    } else {
      it->second = std::max(it->second, std::max(record.ngpus, 0));
    }
  }

  for (auto& record : *records) {
    record.gpuShared = false;
    if (record.gpuId < 0) {
      continue;
    }
    const std::string key = record.hostname + "#" + std::to_string(record.gpuId);
    const auto useIt = gpuUseCount.find(key);
    if (useIt != gpuUseCount.end() && useIt->second > 1) {
      record.gpuShared = true;
    }
  }

  int totalVisibleGpus = 0;
  for (const auto& entry : hostMaxGpus) {
    totalVisibleGpus += std::max(entry.second, 0);
  }
  const int totalRanks = static_cast<int>(records->size());
  if (totalVisibleGpus > 0 && totalRanks > totalVisibleGpus) {
    warnings.emplace_back("MPI ranks exceed visible GPUs (rank-to-GPU oversubscription detected).");
  }

  const auto byRuntime = [](const ResourceRecord& a, const ResourceRecord& b) {
    return a.totalTime < b.totalTime;
  };
  const auto [minIt, maxIt] = std::minmax_element(records->begin(), records->end(), byRuntime);
  if (minIt != records->end() && minIt->totalTime > 0.0) {
    const double imbalance = maxIt->totalTime / minIt->totalTime;
    if (imbalance > 1.20) {
      std::ostringstream msg;
      msg << "Runtime imbalance detected across ranks (max/min = " << std::fixed
          << std::setprecision(3) << imbalance << ").";
      warnings.push_back(msg.str());
    }
  }

  double avgKernelRatio = 0.0;
  double avgCpuBusyRatio = 0.0;
  int validKernelRatioCount = 0;
  int validCpuRatioCount = 0;
  for (const auto& record : *records) {
    if (record.totalTime > 0.0) {
      avgKernelRatio += record.kernelTime / record.totalTime;
      ++validKernelRatioCount;
    }
    if (record.cpuWallTime > 0.0) {
      avgCpuBusyRatio +=
          std::min(1.0, (record.cpuUserTime + record.cpuSystemTime) / record.cpuWallTime);
      ++validCpuRatioCount;
    }
  }
  if (validKernelRatioCount > 0) {
    avgKernelRatio /= static_cast<double>(validKernelRatioCount);
  }
  if (validCpuRatioCount > 0) {
    avgCpuBusyRatio /= static_cast<double>(validCpuRatioCount);
  }

  if (avgCpuBusyRatio > 0.85 && avgKernelRatio < 0.40) {
    warnings.emplace_back("CPU bottleneck suspected (high CPU busy time with low kernel share).");
  }

  if (avgKernelRatio < 0.25) {
    warnings.emplace_back(
        "Low GPU active-time ratio observed; transfer, synchronization, or host work may "
        "dominate.");
  }

  return warnings;
}

bool ResourceTracker::append_csv(const std::filesystem::path& csvPath,
                                 const std::vector<ResourceRecord>& records,
                                 std::string* errorMessage) const {
  constexpr const char* kHeader =
      "run_id,rank,hostname,gpu_id,total_ranks,ncpus,ngpus,omp_threads,scene_time,kernel_time,"
      "transfer_time,mpi_time,output_time,total_time,rays_processed";

  bool writeHeader = true;
  std::error_code ec;
  if (std::filesystem::exists(csvPath, ec)) {
    if (ec) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "Failed to access CSV path: " + csvPath.string() + " (" + ec.message() + ")";
      }
      return false;
    }
    const auto size = std::filesystem::file_size(csvPath, ec);
    if (ec) {
      if (errorMessage != nullptr) {
        *errorMessage = "Failed to read CSV size: " + csvPath.string() + " (" + ec.message() + ")";
      }
      return false;
    }
    writeHeader = size == 0;
  }

  std::ofstream csv(csvPath, std::ios::app);
  if (!csv) {
    if (errorMessage != nullptr) {
      *errorMessage = "Failed to open CSV for append: " + csvPath.string();
    }
    return false;
  }

  if (writeHeader) {
    csv << kHeader << '\n';
  }

  for (const auto& record : records) {
    csv << record.runId << ',' << record.rank << ',' << record.hostname << ',' << record.gpuId
        << ',' << record.totalRanks << ',' << record.ncpus << ',' << record.ngpus << ','
        << record.ompThreads << ',' << std::fixed << std::setprecision(6) << record.sceneTime << ','
        << record.kernelTime << ',' << record.transferTime << ',' << record.mpiTime << ','
        << record.outputTime << ',' << record.totalTime << ',' << record.raysProcessed << '\n';
  }

  if (!csv) {
    if (errorMessage != nullptr) {
      *errorMessage = "Failed while writing CSV rows: " + csvPath.string();
    }
    return false;
  }

  return true;
}

bool ResourceTracker::write_report(const std::filesystem::path& reportPath,
                                   const std::vector<ResourceRecord>& records,
                                   const std::vector<std::string>& warnings,
                                   std::string* errorMessage) const {
  std::ofstream out(reportPath, std::ios::trunc);
  if (!out) {
    if (errorMessage != nullptr) {
      *errorMessage = "Failed to open report file: " + reportPath.string();
    }
    return false;
  }

  if (records.empty()) {
    out << "Resource report unavailable: no records.\n";
    return true;
  }

  const std::string runId = records.front().runId;

  std::vector<double> runtimes;
  runtimes.reserve(records.size());
  std::uint64_t totalRays = 0;
  int totalCpuSlots = 0;
  std::unordered_map<std::string, int> hostGpuMax;
  std::unordered_map<std::string, int> hostRankCount;
  for (const auto& record : records) {
    runtimes.push_back(record.totalTime);
    totalRays += record.raysProcessed;
    totalCpuSlots += record.ncpus;
    auto gpuIt = hostGpuMax.find(record.hostname);
    if (gpuIt == hostGpuMax.end()) {
      hostGpuMax.emplace(record.hostname, std::max(record.ngpus, 0));
    } else {
      gpuIt->second = std::max(gpuIt->second, std::max(record.ngpus, 0));
    }
    ++hostRankCount[record.hostname];
  }

  const auto [minIt, maxIt] = std::minmax_element(runtimes.begin(), runtimes.end());
  const double minRuntime = minIt != runtimes.end() ? *minIt : 0.0;
  const double maxRuntime = maxIt != runtimes.end() ? *maxIt : 0.0;
  const double avgRuntime = runtimes.empty()
                                ? 0.0
                                : std::accumulate(runtimes.begin(), runtimes.end(), 0.0) /
                                      static_cast<double>(runtimes.size());
  const double imbalanceMetric = avgRuntime > 0.0 ? (maxRuntime - minRuntime) / avgRuntime : 0.0;

  int totalVisibleGpus = 0;
  for (const auto& entry : hostGpuMax) {
    totalVisibleGpus += std::max(entry.second, 0);
  }

  double avgGpuUtilEstimate = 0.0;
  int gpuUtilCount = 0;
  for (const auto& record : records) {
    if (record.totalTime > 0.0) {
      avgGpuUtilEstimate += std::min(1.0, record.kernelTime / record.totalTime);
      ++gpuUtilCount;
    }
  }
  if (gpuUtilCount > 0) {
    avgGpuUtilEstimate /= static_cast<double>(gpuUtilCount);
  }

  out << "======================================================================\n";
  out << "                       ToriRender Resource Report\n";
  out << "======================================================================\n\n";

  out << "[Run Summary]\n";
  out << "Run ID: " << runId << '\n';
  out << "Total Ranks: " << records.size() << '\n';
  out << "Hosts Used: " << hostRankCount.size() << '\n';
  out << "Visible GPUs (sum across hosts): " << totalVisibleGpus << '\n';
  out << "CPU Slots Reported (sum ncpus): " << totalCpuSlots << '\n';
  out << "Total Rays Processed: " << totalRays << "\n\n";

  out << "[Load Balance]\n";
  out << std::fixed << std::setprecision(6);
  out << "Min Rank Runtime (s): " << minRuntime << '\n';
  out << "Max Rank Runtime (s): " << maxRuntime << '\n';
  out << "Avg Rank Runtime (s): " << avgRuntime << '\n';
  out << "Imbalance Metric ((max-min)/avg): " << imbalanceMetric << "\n\n";

  out << "[GPU Utilization Estimate]\n";
  out << "Average kernel/total ratio: " << avgGpuUtilEstimate << '\n';
  out << "Interpretation: closer to 1.0 means more time in GPU kernel execution.\n\n";

  out << "[Per-Rank Breakdown]\n";
  out << "rank,host,gpu,shared_gpu,ncpus,omp_threads,rank_start,rank_end,row_start,row_count,"
         "rays,kernel_launches,kernels_async,gpu_active_s,scene_s,kernel_s,transfer_s,mpi_s,"
         "output_s,total_s,cpu_user_s,cpu_sys_s,cpu_idle_s,peak_mem_kb\n";
  for (const auto& record : records) {
    out << record.rank << ',' << record.hostname << ',' << record.gpuId << ','
        << (record.gpuShared ? "yes" : "no") << ',' << record.ncpus << ',' << record.ompThreads
        << ',' << record.rankStartTime << ',' << record.rankEndTime << ',' << record.rowStart << ','
        << record.rowCount << ',' << record.raysProcessed << ',' << record.kernelLaunches << ','
        << (record.kernelsAsync ? "yes" : "no") << ',' << record.gpuActiveTime << ','
        << record.sceneTime << ',' << record.kernelTime << ',' << record.transferTime << ','
        << record.mpiTime << ',' << record.outputTime << ',' << record.totalTime << ','
        << record.cpuUserTime << ',' << record.cpuSystemTime << ',' << record.cpuIdleEstimate << ','
        << record.peakMemoryKb << '\n';
  }
  out << '\n';

  out << "[Warnings]\n";
  if (warnings.empty()) {
    out << "None\n";
  } else {
    for (const auto& warning : warnings) {
      out << "- " << warning << '\n';
    }
  }
  out << '\n';

  out << "[Efficiency Hints]\n";
  out << "1. If ranks share a GPU, reduce ranks per node or enforce 1 rank per GPU.\n";
  out << "2. If imbalance is high, adjust tile or row partitioning.\n";
  out << "3. If transfer time is high, batch host-device movement.\n";
  out << "4. If MPI time is high, reduce synchronization and gather overhead.\n";

  if (!out) {
    if (errorMessage != nullptr) {
      *errorMessage = "Failed while writing report: " + reportPath.string();
    }
    return false;
  }

  return true;
}

bool ResourceTracker::finalize_and_write(std::string* errorMessage) {
  ResourceRecord localCopy;
  std::vector<ResourceRecord> recordsCopy;
  std::filesystem::path outputRootCopy;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    localCopy = local_;
    outputRootCopy = outputRoot_;
    apply_section_totals(localCopy);
    if (allRecords_.empty()) {
      recordsCopy.push_back(localCopy);
    } else {
      recordsCopy = allRecords_;
      for (auto& record : recordsCopy) {
        if (record.runId.empty()) {
          record.runId = localCopy.runId;
        }
      }
    }
  }

  if (recordsCopy.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "No records available for resource report.";
    }
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(outputRootCopy, ec);
  if (ec) {
    if (errorMessage != nullptr) {
      *errorMessage = "Failed to create output directory: " + outputRootCopy.string() + " (" +
                      ec.message() + ")";
    }
    return false;
  }

  const std::filesystem::path reportsDir = outputRootCopy / "run_reports";
  std::filesystem::create_directories(reportsDir, ec);
  if (ec) {
    if (errorMessage != nullptr) {
      *errorMessage =
          "Failed to create report directory: " + reportsDir.string() + " (" + ec.message() + ")";
    }
    return false;
  }

  std::sort(recordsCopy.begin(),
            recordsCopy.end(),
            [](const ResourceRecord& a, const ResourceRecord& b) { return a.rank < b.rank; });

  std::vector<std::string> warnings = build_warnings(&recordsCopy);

  const std::filesystem::path csvPath = outputRootCopy / "resource_metrics.csv";
  const std::string runId =
      recordsCopy.front().runId.empty() ? std::string("unknown_run") : recordsCopy.front().runId;
  const std::filesystem::path reportPath = reportsDir / ("resource_report_" + runId + ".txt");

  if (!append_csv(csvPath, recordsCopy, errorMessage)) {
    return false;
  }

  if (!write_report(reportPath, recordsCopy, warnings, errorMessage)) {
    return false;
  }

  return true;
}

}  // namespace torirender::runtime
