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

class MetricsWriter final {
public:
  [[nodiscard]] static std::expected<std::unique_ptr<MetricsWriter>,
                                     std::string>
  create(const std::filesystem::path &log_directory, const Config &config,
         std::string prompt_variant);

  ~MetricsWriter();

  MetricsWriter(const MetricsWriter &) = delete;
  MetricsWriter &operator=(const MetricsWriter &) = delete;

  [[nodiscard]] std::expected<void, std::string>
  record_tool(const WorldEvent &event, int score_after);
  [[nodiscard]] std::expected<void, std::string>
  record_turn(const TurnRecord &record);
  [[nodiscard]] std::expected<void, std::string>
  finish(const EpisodeResult &result, int final_score);

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  [[nodiscard]] bool finalized() const noexcept { return finalized_; }

private:
  MetricsWriter(std::filesystem::path path, std::ofstream stream,
                std::chrono::steady_clock::time_point started);

  [[nodiscard]] std::expected<void, std::string>
  write_line(const nlohmann::json &record, bool flush = false);
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
