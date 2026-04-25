#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace torirender::runtime {

class SectionProfiler;

class ScopedSectionTimer {
 public:
  ScopedSectionTimer(SectionProfiler& profiler, std::string_view sectionName) noexcept;
  ~ScopedSectionTimer();

  ScopedSectionTimer(const ScopedSectionTimer&) = delete;
  ScopedSectionTimer& operator=(const ScopedSectionTimer&) = delete;

 private:
  SectionProfiler* profiler_;
  std::string sectionName_;
  std::chrono::steady_clock::time_point start_;
  bool active_;
};

class SectionProfiler {
 public:
  void add_seconds(std::string_view sectionName, double seconds);
  double seconds(std::string_view sectionName) const;
  void reset();
  const std::unordered_map<std::string, double>& all_sections() const;
  ScopedSectionTimer scoped(std::string_view sectionName) noexcept;

 private:
  std::unordered_map<std::string, double> sectionSeconds_;
};

struct CsvField {
  std::string name;
  std::string value;
};

std::string csv_escape(std::string_view value);
bool append_csv_row(const std::filesystem::path& csvPath,
                    const std::vector<CsvField>& fields,
                    std::string* errorMessage = nullptr);

}  // namespace torirender::runtime

