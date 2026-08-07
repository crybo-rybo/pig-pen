#include "agent/metrics_writer.hpp"

#include "world/world.hpp"

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

[[nodiscard]] std::tm local_time(const std::time_t value) {
  std::tm result{};
#if defined(_WIN32)
  localtime_s(&result, &value);
#else
  localtime_r(&value, &result);
#endif
  return result;
}

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

[[nodiscard]] std::string
iso_timestamp(const std::chrono::system_clock::time_point now) {
  const auto as_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream result;
  const auto value = local_time(as_time_t);
  result << std::put_time(&value, "%Y-%m-%dT%H:%M:%S%z");
  return result.str();
}

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

[[nodiscard]] nlohmann::json position_json(const world::Position position) {
  return {{"x", position.x}, {"y", position.y}};
}

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
      {"seed", config.seed},
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
           {"known_item_values", config.known_item_values},
           {"reward_feedback", config.reward_feedback},
           {"opaque_look", config.opaque_look},
       }},
  };
  if (auto status = writer->write_line(header, true); !status) {
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
MetricsWriter::record_tool(const WorldEvent &event, const int score_after) {
  if (finalized_) {
    return std::unexpected("cannot record a tool after the metrics footer");
  }
  ++tool_counts_[event.tool];
  last_score_ = score_after;
  if (event.tool == "eat" && event.result.value("ok", false) &&
      event.result.contains("ate") && event.result.at("ate").is_string()) {
    ++eaten_counts_[event.result.at("ate").get<std::string>()];
  }
  return write_line({
      {"type", "tool"},
      {"turn", event.turn},
      {"tick", event.tick},
      {"tool", event.tool},
      {"args", event.arguments},
      {"result", event.result},
      {"before", position_json(event.before)},
      {"after", position_json(event.after)},
      {"score_after", score_after},
  });
}

std::expected<void, std::string>
MetricsWriter::record_turn(const TurnRecord &record) {
  if (finalized_) {
    return std::unexpected("cannot record a turn after the metrics footer");
  }
  turns_recorded_ = std::max(turns_recorded_, record.turn);
  return write_line(
      {
          {"type", "turn"},
          {"turn", record.turn},
          {"status", turn_status_name(record.status)},
          {"user_message", record.user_message},
          {"assistant_text", record.assistant_text},
          {"error", record.error},
          {"input_tokens", record.input_tokens},
          {"output_tokens", record.output_tokens},
          {"latency_ms", record.latency.count()},
      },
      true);
}

std::expected<void, std::string>
MetricsWriter::finish(const EpisodeResult &result, const int final_score) {
  return write_footer(std::string{finish_reason_name(result.reason)},
                      result.turns_used, result.error, final_score, true);
}

std::expected<void, std::string>
MetricsWriter::write_line(const nlohmann::json &record, const bool flush) {
  stream_ << record.dump() << '\n';
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
      {
          {"type", "footer"},
          {"complete", complete},
          {"finish_reason", std::move(reason)},
          {"error", std::move(error)},
          {"final_score", final_score},
          {"items_eaten", eaten_counts_},
          {"tool_call_counts", tool_counts_},
          {"turns_used", turns_used},
          {"duration_ms", duration.count()},
      },
      true);
  if (status) {
    finalized_ = true;
  }
  return status;
}

} // namespace pigpen::agent
