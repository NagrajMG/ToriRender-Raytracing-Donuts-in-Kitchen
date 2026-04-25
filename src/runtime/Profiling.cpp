#include "runtime/Profiling.hpp"

#include <algorithm>
#include <fstream>

namespace torirender::runtime {

ScopedSectionTimer::ScopedSectionTimer(SectionProfiler& profiler, std::string_view sectionName) noexcept
    : profiler_(&profiler),
      sectionName_(sectionName),
      start_(std::chrono::steady_clock::now()),
      active_(true) {
}

ScopedSectionTimer::~ScopedSectionTimer() {
  if (!active_ || profiler_ == nullptr) {
    return;
  }

  const auto end = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(end - start_).count();
  profiler_->add_seconds(sectionName_, elapsed);
}

void SectionProfiler::add_seconds(std::string_view sectionName, double seconds) {
  if (seconds <= 0.0) {
    return;
  }
  sectionSeconds_[std::string(sectionName)] += seconds;
}

double SectionProfiler::seconds(std::string_view sectionName) const {
  const auto it = sectionSeconds_.find(std::string(sectionName));
  if (it == sectionSeconds_.end()) {
    return 0.0;
  }
  return it->second;
}

void SectionProfiler::reset() {
  sectionSeconds_.clear();
}

const std::unordered_map<std::string, double>& SectionProfiler::all_sections() const {
  return sectionSeconds_;
}

ScopedSectionTimer SectionProfiler::scoped(std::string_view sectionName) noexcept {
  return ScopedSectionTimer(*this, sectionName);
}

std::string csv_escape(std::string_view value) {
  bool needsQuotes = false;
  for (const char c : value) {
    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
      needsQuotes = true;
      break;
    }
  }

  if (!needsQuotes) {
    return std::string(value);
  }

  std::string escaped;
  escaped.reserve(value.size() + 4U);
  escaped.push_back('"');
  for (const char c : value) {
    if (c == '"') {
      escaped.push_back('"');
    }
    escaped.push_back(c);
  }
  escaped.push_back('"');
  return escaped;
}

bool append_csv_row(const std::filesystem::path& csvPath,
                    const std::vector<CsvField>& fields,
                    std::string* errorMessage) {
  if (fields.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "No CSV fields provided.";
    }
    return false;
  }

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
        *errorMessage =
            "Failed to query CSV size: " + csvPath.string() + " (" + ec.message() + ")";
      }
      return false;
    }
    writeHeader = size == 0;
  }

  std::ofstream out(csvPath, std::ios::app);
  if (!out) {
    if (errorMessage != nullptr) {
      *errorMessage = "Failed to open CSV for append: " + csvPath.string();
    }
    return false;
  }

  if (writeHeader) {
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (i != 0U) {
        out << ',';
      }
      out << csv_escape(fields[i].name);
    }
    out << '\n';
  }

  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0U) {
      out << ',';
    }
    out << csv_escape(fields[i].value);
  }
  out << '\n';

  if (!out) {
    if (errorMessage != nullptr) {
      *errorMessage = "Failed while writing CSV row: " + csvPath.string();
    }
    return false;
  }
  return true;
}

}  // namespace torirender::runtime

