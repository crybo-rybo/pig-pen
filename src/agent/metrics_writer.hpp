/// @file metrics_writer.hpp
/// @brief Append-only JSONL episode log: header, tool, turn, footer.
///
/// A log always terminates with exactly one footer: the destructor writes
/// an "abandoned" footer whenever the episode ends before finish(), so even
/// abnormal shutdown leaves a finalized record.
#pragma once

#include "agent/config.hpp"
#include "agent/episode_runner.hpp"
#include "agent/events.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace pigpen::agent {

/// @brief Writes one episode's JSONL log and reconciles its summary counts.
class MetricsWriter final {
public:
  /// @brief Create the log directory and file, then write the header line.
  /// @return The writer, or a message when the directory or file cannot be
  /// created or the header cannot be written.
  [[nodiscard]] static std::expected<std::unique_ptr<MetricsWriter>,
                                     std::string>
  create(const std::filesystem::path &log_directory, const Config &config,
         std::string prompt_variant);

  /// @brief Writes an incomplete "abandoned" footer if none was written yet.
  ~MetricsWriter();

  MetricsWriter(const MetricsWriter &) = delete;
  MetricsWriter &operator=(const MetricsWriter &) = delete;

  /// @brief Append one successfully decoded world-tool invocation.
  [[nodiscard]] std::expected<void, std::string>
  record_tool(const ToolActivity &activity);
  /// @brief Append one finished model turn.
  [[nodiscard]] std::expected<void, std::string>
  record_turn(const TurnRecord &record);
  /// @brief Write the final footer; the log accepts nothing afterwards.
  [[nodiscard]] std::expected<void, std::string>
  finish(const EpisodeResult &result, int final_score);

  /// @brief Absolute path of the JSONL file being written.
  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  /// @brief True once a footer has been written; the log is then immutable.
  [[nodiscard]] bool finalized() const noexcept { return finalized_; }

private:
  MetricsWriter(std::filesystem::path path, std::ofstream stream,
                std::chrono::steady_clock::time_point started);

  [[nodiscard]] std::expected<void, std::string> write_line(std::string record,
                                                            bool flush = false);
  [[nodiscard]] std::expected<void, std::string>
  write_footer(std::string reason, std::uint32_t turns_used, std::string error,
               int final_score, bool complete);

  std::filesystem::path path_{};
  std::ofstream stream_{};
  std::chrono::steady_clock::time_point started_{};
  std::map<std::string, std::size_t> tool_counts_{};
  std::map<std::string, std::size_t> eaten_counts_{};
  std::uint32_t turns_recorded_{};
  int last_score_{};
  bool finalized_{false};
};

} // namespace pigpen::agent
