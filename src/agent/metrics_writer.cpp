/// @file metrics_writer.cpp
/// @brief MetricsWriter implementation; the contract is in the header.
#include "agent/metrics_writer.hpp"

#include "agent/record_json.hpp"
#include "agent/summary_json.hpp"
#include "world/world.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string_view>
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

/// @brief Append one serialised JSONL line and flush so a crash loses
/// nothing.
[[nodiscard]] std::expected<void, std::string>
write_raw_line(std::ofstream &stream, const std::filesystem::path &path,
               const std::string_view line) {
  stream << line << '\n' << std::flush;
  if (!stream) {
    return std::unexpected("failed writing metrics log: " + path.string());
  }
  return {};
}

/// @brief Append one JSONL record.
[[nodiscard]] std::expected<void, std::string>
write_line(std::ofstream &stream, const std::filesystem::path &path,
           const nlohmann::json &record) {
  return write_raw_line(stream, path, dump_line(record));
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
                      const Config &config, std::string prompt_variant,
                      std::string rollout_id) {
  std::error_code directory_error;
  std::filesystem::create_directories(log_directory, directory_error);
  if (directory_error) {
    return std::unexpected("could not create log directory " +
                           log_directory.string() + ": " +
                           directory_error.message());
  }

  const auto wall_started = std::chrono::system_clock::now();
  // <timestamp>-<model>-<seed>.jsonl, suffixed -N until an exclusive create
  // succeeds, so concurrent episodes never share or clobber a log.
  const auto stem = timestamp(wall_started) + '-' +
                    sanitized_filename_component(config.model) + '-' +
                    std::to_string(config.seed);
  auto path = log_directory / (stem + ".jsonl");
  std::ofstream stream{path, std::ios::out | std::ios::noreplace};
  for (std::size_t suffix = 1; !stream && std::filesystem::exists(path);
       ++suffix) {
    path = log_directory / (stem + '-' + std::to_string(suffix) + ".jsonl");
    stream = std::ofstream{path, std::ios::out | std::ios::noreplace};
  }
  if (!stream) {
    return std::unexpected("could not open metrics log: " + path.string());
  }

  auto writer = std::unique_ptr<MetricsWriter>{
      new MetricsWriter{std::move(path), std::move(stream),
                        std::chrono::steady_clock::now(), rollout_id}};
  // The Config fields are shared with the worker's episode record.
  auto header = config_json(config);
  header["type"] = "header";
  header["started_at"] = iso_timestamp(wall_started);
  header["prompt_variant"] = std::move(prompt_variant);
  header["rollout_id"] = rollout_id.empty()
                             ? nlohmann::json(nullptr)
                             : nlohmann::json(std::move(rollout_id));
  if (auto status = write_line(writer->stream_, writer->path_, header);
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return writer;
}

MetricsWriter::MetricsWriter(
    std::filesystem::path path, std::ofstream stream,
    const std::chrono::steady_clock::time_point started, std::string rollout_id)
    : path_(std::move(path)), stream_(std::move(stream)), started_(started),
      rollout_id_(std::move(rollout_id)) {}

MetricsWriter::~MetricsWriter() {
  if (!finalized_ && stream_) {
    static_cast<void>(write_abandoned_footer());
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
  return write_line(stream_, path_,
                    {
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
                    });
}

std::expected<void, std::string>
MetricsWriter::record_turn(const EpisodeTurn &turn) {
  if (finalized_) {
    return std::unexpected("cannot record a turn after the metrics footer");
  }
  const auto &record = turn.record;
  turns_recorded_ = record.turn;
  nlohmann::json tool_stats = nullptr;
  if (record.tool_stats) {
    const auto &stats = *record.tool_stats;
    tool_stats = {{"rounds", stats.rounds},
                  {"calls", stats.calls},
                  {"rejected_calls", stats.rejected_calls},
                  {"round_limit_reached", stats.round_limit_reached},
                  {"unexecuted_calls", stats.unexecuted_calls}};
  }
  nlohmann::json calls = nullptr;
  if (turn.calls) {
    calls = {{"executed", turn.calls->executed},
             {"invalid", turn.calls->invalid},
             {"budget_refused", turn.calls->budget_refused},
             {"host_refused", turn.calls->host_refused}};
  }
  return write_line(
      stream_, path_,
      {
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
          {"calls", std::move(calls)},
          {"zero_tool_turn",
           record.status == TurnStatus::completed && record.tool_calls == 0U},
          {"latency_ms", record.latency.count()},
      });
}

std::expected<void, std::string>
MetricsWriter::finish(const EpisodeSummary &summary) {
  if (finalized_) {
    return std::unexpected("metrics log already has a footer");
  }
  if (!summary.complete()) {
    return std::unexpected("cannot write a footer for an unfinished episode");
  }
  auto status = write_raw_line(stream_, path_, to_json_line(summary, "footer"));
  if (status) {
    finalized_ = true;
  }
  return status;
}

std::expected<void, std::string> MetricsWriter::write_abandoned_footer() {
  const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_);
  auto status = write_line(
      stream_, path_,
      {
          {"type", "footer"},
          {"rollout_id", rollout_id_.empty() ? nlohmann::json(nullptr)
                                             : nlohmann::json(rollout_id_)},
          {"complete", false},
          {"finish_reason", "abandoned"},
          {"error", "episode ended before finalization"},
          {"final_score", last_score_},
          {"items_eaten", eaten_counts_},
          {"tool_call_counts", tool_counts_},
          {"turns_used", turns_recorded_},
          {"duration_ms", duration.count()},
      });
  if (status) {
    finalized_ = true;
  }
  return status;
}

} // namespace pigpen::agent
