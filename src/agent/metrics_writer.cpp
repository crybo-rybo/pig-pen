/// @file metrics_writer.cpp
/// @brief MetricsWriter implementation; the contract is in the header.
#include "agent/metrics_writer.hpp"

#include "world/world.hpp"

#include <scry/reflection.hpp>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

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

// One reflected aggregate per JSONL record. Scry's codec writes keys in
// lexical order, enums by enumerator name, and each record's tag as "type".
// Records borrow their strings from the values they describe.

struct GridSize {
  int width{};
  int height{};
};

struct ScenarioRecord {
  GridSize grid{};
  world::Position spawn{};
  MetricsWriter::ItemCounts items{};
  std::uint32_t turn_budget{};
  std::uint32_t max_tool_rounds{};
  std::uint32_t max_world_tool_calls_per_turn{};
  bool known_item_values{};
  bool reward_feedback{};
  bool opaque_look{};
};

// clang-format off: clang-format would glue the class annotations to `struct`.
struct [[= scry::reflection::tag{"header"}]] HeaderRecord {
  std::string_view model{};
  std::string_view base_url{};
  double temperature{};
  std::uint32_t max_output_tokens{};
  std::uint64_t seed{};
  std::optional<std::uint32_t> sampling_seed{};
  std::string_view started_at{};
  std::string_view prompt_variant{};
  ScenarioRecord scenario{};
};

struct [[= scry::reflection::tag{"tool"}]] ToolRecord {
  std::size_t turn{};
  std::uint64_t tick{};
  std::uint64_t scry_turn_id{};
  std::string_view call_id{};
  std::uint32_t round{};
  std::uint32_t index{};
  core::ToolKind tool{};
  /// Scry's canonical payloads, spliced verbatim after validation.
  scry::Json args{};
  scry::Json result{};
  world::Position before{};
  world::Position after{};
  bool action_executed{};
  bool result_dispatched{};
  int score_after{};
};

struct [[= scry::reflection::tag{"turn"}]] TurnLogRecord {
  std::uint32_t turn{};
  core::TurnStatus status{};
  std::string_view user_message{};
  std::string_view assistant_text{};
  std::string_view error{};
  std::uint64_t input_tokens{};
  std::uint64_t output_tokens{};
  std::size_t tool_calls{};
  std::optional<core::TurnToolStats> scry_tools{};
  bool zero_tool_turn{};
  std::int64_t latency_ms{};
};

struct [[= scry::reflection::tag{"footer"}]] FooterRecord {
  bool complete{};
  std::string_view finish_reason{};
  std::string_view error{};
  int final_score{};
  MetricsWriter::ItemCounts items_eaten{};
  MetricsWriter::ToolCounts tool_call_counts{};
  std::uint32_t turns_used{};
  std::int64_t duration_ms{};
};
// clang-format on

using LogRecord =
    std::variant<HeaderRecord, ToolRecord, TurnLogRecord, FooterRecord>;

/// @brief Append one JSONL record and flush so a crash loses nothing.
[[nodiscard]] std::expected<void, std::string>
write_line(std::ofstream &stream, const std::filesystem::path &path,
           const LogRecord &record) {
  const auto encoded = scry::reflection::encode(record);
  if (!encoded) {
    return std::unexpected("could not encode metrics record: " +
                           encoded.error().message);
  }
  stream << encoded->text << '\n' << std::flush;
  if (!stream) {
    return std::unexpected("failed writing metrics log: " + path.string());
  }
  return {};
}

[[nodiscard]] std::size_t &count_of(MetricsWriter::ItemCounts &counts,
                                    const world::ItemType item) {
  switch (item) {
  case world::ItemType::berry:
    return counts.berry;
  case world::ItemType::apple:
    return counts.apple;
  case world::ItemType::truffle:
    return counts.truffle;
  case world::ItemType::toadstool:
    return counts.toadstool;
  }
  std::unreachable();
}

[[nodiscard]] std::size_t &count_of(MetricsWriter::ToolCounts &counts,
                                    const core::ToolKind kind) {
  switch (kind) {
  case core::ToolKind::move:
    return counts.move;
  case core::ToolKind::look:
    return counts.look;
  case core::ToolKind::eat:
    return counts.eat;
  }
  std::unreachable();
}

} // namespace

std::expected<std::unique_ptr<MetricsWriter>, std::string>
MetricsWriter::create(const std::filesystem::path &log_directory,
                      const core::Config &config, std::string prompt_variant) {
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

  auto writer = std::unique_ptr<MetricsWriter>{new MetricsWriter{
      std::move(path), std::move(stream), std::chrono::steady_clock::now()}};
  const auto started_at = iso_timestamp(wall_started);
  const HeaderRecord header{
      .model = config.model,
      .base_url = config.base_url,
      .temperature = config.temperature,
      .max_output_tokens = config.max_output_tokens,
      .seed = config.seed,
      .sampling_seed = config.sampling_seed,
      .started_at = started_at,
      .prompt_variant = prompt_variant,
      .scenario =
          {
              .grid = {.width = world::World::width,
                       .height = world::World::height},
              .spawn = world::World::spawn,
              .items = {.berry = world::World::default_berry_count,
                        .apple = world::World::default_apple_count,
                        .truffle = world::World::default_truffle_count,
                        .toadstool = world::World::default_toadstool_count},
              .turn_budget = config.turn_budget,
              .max_tool_rounds = config.max_tool_rounds,
              .max_world_tool_calls_per_turn =
                  core::max_world_tool_calls_per_turn,
              .known_item_values = config.known_item_values,
              .reward_feedback = config.reward_feedback,
              .opaque_look = config.opaque_look,
          },
  };
  if (auto status = write_line(writer->stream_, writer->path_, header);
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return writer;
}

MetricsWriter::MetricsWriter(
    std::filesystem::path path, std::ofstream stream,
    const std::chrono::steady_clock::time_point started)
    : path_(std::move(path)), stream_(std::move(stream)), started_(started) {}

MetricsWriter::~MetricsWriter() {
  if (!finalized_ && stream_) {
    static_cast<void>(write_footer("abandoned", turns_recorded_,
                                   "episode ended before finalization",
                                   last_score_, false));
  }
}

std::expected<void, std::string>
MetricsWriter::record_tool(const core::ToolActivity &activity) {
  if (finalized_) {
    return std::unexpected("cannot record a tool after the metrics footer");
  }
  auto written = write_line(stream_, path_,
                            ToolRecord{
                                .turn = activity.turn,
                                .tick = activity.tick,
                                .scry_turn_id = activity.scry_turn_id,
                                .call_id = activity.call_id,
                                .round = activity.round,
                                .index = activity.index,
                                .tool = activity.kind,
                                .args = {activity.arguments_json},
                                .result = {activity.result_json},
                                .before = activity.before,
                                .after = activity.after,
                                .action_executed = true,
                                .result_dispatched = activity.result_dispatched,
                                .score_after = activity.score_after,
                            });
  if (!written) {
    return written;
  }
  ++count_of(tool_counts_, activity.kind);
  last_score_ = activity.score_after;
  if (activity.eaten) {
    ++count_of(eaten_counts_, *activity.eaten);
  }
  return {};
}

std::expected<void, std::string>
MetricsWriter::record_turn(const core::TurnRecord &record) {
  if (finalized_) {
    return std::unexpected("cannot record a turn after the metrics footer");
  }
  turns_recorded_ = record.turn;
  return write_line(
      stream_, path_,
      TurnLogRecord{
          .turn = record.turn,
          .status = record.status,
          .user_message = record.user_message,
          .assistant_text = record.assistant_text,
          .error = record.error,
          .input_tokens = record.input_tokens,
          .output_tokens = record.output_tokens,
          .tool_calls = record.tool_calls,
          .scry_tools = record.tool_stats,
          .zero_tool_turn = record.status == core::TurnStatus::completed &&
                            record.tool_calls == 0U,
          .latency_ms = record.latency.count(),
      });
}

std::expected<void, std::string>
MetricsWriter::finish(const core::EpisodeResult &result,
                      const int final_score) {
  return write_footer(core::finish_reason_name(result.reason),
                      result.turns_used, result.error, final_score, true);
}

std::expected<void, std::string> MetricsWriter::write_footer(
    const std::string_view reason, const std::uint32_t turns_used,
    const std::string_view error, const int final_score, const bool complete) {
  if (finalized_) {
    return std::unexpected("metrics log already has a footer");
  }
  const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_);
  auto status = write_line(stream_, path_,
                           FooterRecord{
                               .complete = complete,
                               .finish_reason = reason,
                               .error = error,
                               .final_score = final_score,
                               .items_eaten = eaten_counts_,
                               .tool_call_counts = tool_counts_,
                               .turns_used = turns_used,
                               .duration_ms = duration.count(),
                           });
  if (status) {
    finalized_ = true;
  }
  return status;
}

} // namespace pigpen::agent
