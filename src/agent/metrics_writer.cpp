/// @file metrics_writer.cpp
/// @brief MetricsWriter implementation; the contract is in the header.
#include "agent/metrics_writer.hpp"

#include "world/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace pigpen::agent {
namespace {

/// @brief Replace filename-hostile bytes with '_'; an empty component
/// becomes "model" so the log stem stays well-formed.
[[nodiscard]] std::string sanitized_filename_component(std::string value) {
  for (auto &character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte) == 0 && character != '-' && character != '_' &&
        character != '.') {
      character = '_';
    }
  }
  return value.empty() ? "model" : value;
}

/// @brief Portable, reentrant localtime.
[[nodiscard]] std::tm local_time(const std::time_t value) {
  std::tm result{};
#if defined(_WIN32)
  localtime_s(&result, &value);
#else
  localtime_r(&value, &result);
#endif
  return result;
}

/// @brief Millisecond-resolution local timestamp for the log filename.
[[nodiscard]] std::string
timestamp(const std::chrono::system_clock::time_point now) {
  const auto as_time_t = std::chrono::system_clock::to_time_t(now);
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch()) %
      std::chrono::seconds{1};
  std::ostringstream result;
  const auto value = local_time(as_time_t);
  result << std::put_time(&value, "%Y%m%d-%H%M%S") << '-' << std::setw(3)
         << std::setfill('0') << milliseconds.count();
  return result.str();
}

/// @brief ISO-8601 local timestamp recorded in the header line.
[[nodiscard]] std::string
iso_timestamp(const std::chrono::system_clock::time_point now) {
  const auto as_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream result;
  const auto value = local_time(as_time_t);
  result << std::put_time(&value, "%Y-%m-%dT%H:%M:%S%z");
  return result.str();
}

/// @brief Build <timestamp>-<model>-<seed>.jsonl, appending -N until the
/// name is unused so concurrent episodes never share a log.
[[nodiscard]] std::filesystem::path
unique_log_path(const std::filesystem::path &directory, const Config &config,
                const std::chrono::system_clock::time_point now) {
  const auto stem = timestamp(now) + '-' +
                    sanitized_filename_component(config.model) + '-' +
                    std::to_string(config.seed);
  auto candidate = directory / (stem + ".jsonl");
  for (std::size_t suffix = 1; std::filesystem::exists(candidate); ++suffix) {
    candidate = directory / (stem + '-' + std::to_string(suffix) + ".jsonl");
  }
  return candidate;
}

/// @brief Project a grid position into the log's {"x", "y"} shape.
[[nodiscard]] nlohmann::json position_json(const world::Position position) {
  return {{"x", position.x}, {"y", position.y}};
}

/// @brief Stable lowercase name recorded in turn lines.
[[nodiscard]] std::string_view turn_status_name(const TurnStatus status) {
  switch (status) {
  case TurnStatus::completed:
    return "completed";
  case TurnStatus::cancelled:
    return "cancelled";
  case TurnStatus::error:
    return "error";
  }
  return "unknown";
}

} // namespace

std::expected<std::unique_ptr<MetricsWriter>, std::string>
MetricsWriter::create(const std::filesystem::path &log_directory,
                      const Config &config, std::string prompt_variant) {
  std::error_code directory_error;
  std::filesystem::create_directories(log_directory, directory_error);
  if (directory_error) {
    return std::unexpected("could not create log directory " +
                           log_directory.string() + ": " +
                           directory_error.message());
  }

  const auto wall_started = std::chrono::system_clock::now();
  auto path = unique_log_path(log_directory, config, wall_started);
  std::ofstream stream{path, std::ios::out | std::ios::trunc};
  if (!stream) {
    return std::unexpected("could not open metrics log: " + path.string());
  }

  auto writer = std::unique_ptr<MetricsWriter>{new MetricsWriter{
      std::move(path), std::move(stream), std::chrono::steady_clock::now()}};
  const nlohmann::json header = {
      {"type", "header"},
      {"model", config.model},
      {"base_url", config.base_url},
      {"temperature", config.temperature},
      {"max_output_tokens", config.max_output_tokens},
      {"seed", config.seed},
      {"sampling_seed", config.sampling_seed
                            ? nlohmann::json(*config.sampling_seed)
                            : nlohmann::json(nullptr)},
      {"started_at", iso_timestamp(wall_started)},
      {"prompt_variant", std::move(prompt_variant)},
      {"scenario",
       {
           {"grid",
            {{"width", world::World::width}, {"height", world::World::height}}},
           {"spawn", position_json(world::World::spawn)},
           {"items",
            {{"berry", world::World::default_berry_count},
             {"apple", world::World::default_apple_count},
             {"truffle", world::World::default_truffle_count},
             {"toadstool", world::World::default_toadstool_count}}},
           {"turn_budget", config.turn_budget},
           {"max_tool_rounds", config.max_tool_rounds},
           {"max_world_tool_calls_per_turn", max_world_tool_calls_per_turn},
           {"known_item_values", config.known_item_values},
           {"reward_feedback", config.reward_feedback},
           {"opaque_look", config.opaque_look},
       }},
  };
  if (auto status = writer->write_line(header.dump(), true); !status) {
    return std::unexpected(std::move(status.error()));
  }
  return writer;
}

MetricsWriter::MetricsWriter(
    std::filesystem::path path, std::ofstream stream,
    const std::chrono::steady_clock::time_point started)
    : path_(std::move(path)), stream_(std::move(stream)), started_(started),
      tool_counts_{{"move", 0}, {"look", 0}, {"eat", 0}},
      eaten_counts_{
          {"berry", 0}, {"apple", 0}, {"truffle", 0}, {"toadstool", 0}} {}

MetricsWriter::~MetricsWriter() {
  if (!finalized_ && stream_) {
    static_cast<void>(write_footer("abandoned", turns_recorded_,
                                   "episode ended before finalization",
                                   last_score_, false));
  }
}

std::expected<void, std::string>
MetricsWriter::record_tool(const ToolActivity &activity) {
  if (finalized_) {
    return std::unexpected("cannot record a tool after the metrics footer");
  }
  auto arguments =
      nlohmann::json::parse(activity.arguments_json, nullptr, false);
  auto result = nlohmann::json::parse(activity.result_json, nullptr, false);
  if (arguments.is_discarded() || result.is_discarded()) {
    return std::unexpected("scry produced invalid canonical tool JSON");
  }
  ++tool_counts_[std::string{tool_kind_name(activity.kind)}];
  last_score_ = activity.score_after;
  if (activity.eaten) {
    ++eaten_counts_[std::string{world::item_name(*activity.eaten)}];
  }
  return write_line(nlohmann::json{
      {"type", "tool"},
      {"turn", activity.turn},
      {"tick", activity.tick},
      {"scry_turn_id", activity.scry_turn_id},
      {"call_id", activity.call_id},
      {"round", activity.round},
      {"index", activity.index},
      {"tool", tool_kind_name(activity.kind)},
      {"args", std::move(arguments)},
      {"result", std::move(result)},
      {"before", position_json(activity.before)},
      {"after", position_json(activity.after)},
      {"action_executed", true},
      {"result_dispatched", activity.result_dispatched},
      {"score_after", activity.score_after},
  }
                        .dump());
}

std::expected<void, std::string>
MetricsWriter::record_turn(const TurnRecord &record) {
  if (finalized_) {
    return std::unexpected("cannot record a turn after the metrics footer");
  }
  turns_recorded_ = std::max(turns_recorded_, record.turn);
  nlohmann::json tool_stats = nullptr;
  if (record.tool_stats) {
    const auto &stats = *record.tool_stats;
    tool_stats = {{"rounds", stats.rounds},
                  {"calls", stats.calls},
                  {"rejected_calls", stats.rejected_calls},
                  {"round_limit_reached", stats.round_limit_reached},
                  {"unexecuted_calls", stats.unexecuted_calls}};
  }
  return write_line(
      nlohmann::json{
          {"type", "turn"},
          {"turn", record.turn},
          {"status", turn_status_name(record.status)},
          {"user_message", record.user_message},
          {"assistant_text", record.assistant_text},
          {"error", record.error},
          {"input_tokens", record.input_tokens},
          {"output_tokens", record.output_tokens},
          {"tool_calls", record.tool_calls},
          {"scry_tools", std::move(tool_stats)},
          {"zero_tool_turn",
           record.status == TurnStatus::completed && record.tool_calls == 0U},
          {"latency_ms", record.latency.count()},
      }
          .dump(),
      true);
}

std::expected<void, std::string>
MetricsWriter::finish(const EpisodeResult &result, const int final_score) {
  return write_footer(std::string{finish_reason_name(result.reason)},
                      result.turns_used, result.error, final_score, true);
}

std::expected<void, std::string> MetricsWriter::write_line(std::string record,
                                                           const bool flush) {
  stream_ << record << '\n';
  if (flush) {
    stream_.flush();
  }
  if (!stream_) {
    return std::unexpected("failed writing metrics log: " + path_.string());
  }
  return {};
}

std::expected<void, std::string>
MetricsWriter::write_footer(std::string reason, const std::uint32_t turns_used,
                            std::string error, const int final_score,
                            const bool complete) {
  if (finalized_) {
    return std::unexpected("metrics log already has a footer");
  }
  const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_);
  auto status = write_line(
      nlohmann::json{
          {"type", "footer"},
          {"complete", complete},
          {"finish_reason", std::move(reason)},
          {"error", std::move(error)},
          {"final_score", final_score},
          {"items_eaten", eaten_counts_},
          {"tool_call_counts", tool_counts_},
          {"turns_used", turns_used},
          {"duration_ms", duration.count()},
      }
          .dump(),
      true);
  if (status) {
    finalized_ = true;
  }
  return status;
}

} // namespace pigpen::agent
