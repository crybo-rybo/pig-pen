/// @file metrics_writer.hpp
/// @brief Append-only JSONL episode log: header, tool, turn, footer.
///
/// A log always terminates with exactly one footer: the destructor writes
/// an "abandoned" footer whenever the episode ends before finish(), so even
/// abnormal shutdown leaves a finalized record.
#pragma once

#include "agent/config.hpp"
#include "agent/episode_summary.hpp"
#include "agent/episode_turn.hpp"
#include "agent/events.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace pigpen::agent {

/// @brief Writes one episode's JSONL log.
///
/// The footer of a finished episode is its EpisodeSummary. The writer keeps
/// running counts of what it recorded only for the "abandoned" footer, which
/// has no summary to draw on.
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
  /// @brief Append one finished model turn and its call tally.
  [[nodiscard]] std::expected<void, std::string>
  record_turn(const EpisodeTurn &turn);
  /// @brief Write @p summary as the final footer; the log accepts nothing
  /// afterwards.
  /// @return An error, writing nothing, when the summary is not complete().
  [[nodiscard]] std::expected<void, std::string>
  finish(const EpisodeSummary &summary);

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  MetricsWriter(std::filesystem::path path, std::ofstream stream,
                std::chrono::steady_clock::time_point started);

  /// @brief The incomplete footer the destructor writes.
  [[nodiscard]] std::expected<void, std::string> write_abandoned_footer();

  std::filesystem::path path_{};
  std::ofstream stream_{};
  std::chrono::steady_clock::time_point started_{};
  // Abandoned-footer counts, pre-seeded so every tool and item is listed.
  std::map<std::string, std::size_t> tool_counts_{
      {"move", 0}, {"look", 0}, {"eat", 0}};
  std::map<std::string, std::size_t> eaten_counts_{
      {"berry", 0}, {"apple", 0}, {"truffle", 0}, {"toadstool", 0}};
  std::uint32_t turns_recorded_{};
  int last_score_{};
  bool finalized_{false};
};

} // namespace pigpen::agent
