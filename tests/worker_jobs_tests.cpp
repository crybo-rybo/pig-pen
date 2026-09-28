/// @file worker_jobs_tests.cpp
/// @brief Covers the worker's job list: `--seeds` parsing (single seeds,
/// ranges, bounds, and each diagnostic), rollout prefixes, seed-major
/// expansion with rollout ids and sampling seeds (including the 32-bit
/// ceiling), duplicate seeds, the job cap, `--header` parsing with its
/// reserved names, OptionParser::rejected(), and `--jobs -` job lines:
/// their parsing, each rejection, and the stream's defaults and unique
/// rollout ids.

#include "cli/worker_jobs.hpp"

#include "agent/job_spec.hpp"
#include "cli/option_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using pigpen::cli::WorkerJob;

constexpr auto u64_max = std::numeric_limits<std::uint64_t>::max();
constexpr auto u32_max = std::numeric_limits<std::uint32_t>::max();

/// @brief Every job of @p jobs, in index order.
[[nodiscard]] std::vector<WorkerJob>
all_jobs(const pigpen::cli::WorkerJobs &jobs) {
  std::vector<WorkerJob> result;
  for (std::size_t index = 0; index < jobs.size(); ++index) {
    result.push_back(jobs.at(index));
  }
  return result;
}

/// @brief The seeds of one --seeds value, or its diagnostic.
[[nodiscard]] std::pair<std::vector<std::uint64_t>, std::string>
seeds_of(const std::string_view value) {
  std::vector<std::uint64_t> seeds;
  auto parsed = pigpen::cli::parse_seed_list(value, seeds);
  return {seeds, parsed ? std::string{} : parsed.error()};
}

} // namespace

TEST_CASE("seed lists accept seeds, inclusive ranges, and the full seed range",
          "[worker]") {
  CHECK(seeds_of("7") ==
        std::pair{std::vector<std::uint64_t>{7}, std::string{}});
  CHECK(seeds_of("3-5,9,0-0") ==
        std::pair{std::vector<std::uint64_t>{3, 4, 5, 9, 0}, std::string{}});
  CHECK(seeds_of("18446744073709551614-18446744073709551615").first ==
        std::vector<std::uint64_t>{u64_max - 1, u64_max});

  // Repeated values append, in order.
  std::vector<std::uint64_t> seeds{1};
  REQUIRE(pigpen::cli::parse_seed_list("2-3", seeds));
  CHECK(seeds == std::vector<std::uint64_t>{1, 2, 3});
}

TEST_CASE("seed lists name each malformed entry and change nothing",
          "[worker]") {
  const std::string range_text =
      " is not a seed or an A-B range of seeds in 0..18446744073709551615";
  CHECK(seeds_of("").second == "--seeds has an empty entry in \"\"");
  CHECK(seeds_of("1,,2").second == "--seeds has an empty entry in \"1,,2\"");
  CHECK(seeds_of("1,").second == "--seeds has an empty entry in \"1,\"");
  CHECK(seeds_of("x").second == "--seeds entry \"x\"" + range_text);
  CHECK(seeds_of("-3").second == "--seeds entry \"-3\"" + range_text);
  CHECK(seeds_of("3-").second == "--seeds entry \"3-\"" + range_text);
  CHECK(seeds_of("1-2-3").second == "--seeds entry \"1-2-3\"" + range_text);
  CHECK(seeds_of(" 4").second == "--seeds entry \" 4\"" + range_text);
  CHECK(seeds_of("18446744073709551616").second ==
        "--seeds entry \"18446744073709551616\"" + range_text);
  CHECK(seeds_of("5-3").second == "--seeds range \"5-3\" must not descend");

  // The cap is checked before a range is expanded.
  CHECK(seeds_of("0-18446744073709551615").second ==
        "--seeds lists more than 1000000 seeds");
  CHECK(seeds_of("0-999999").first.size() == pigpen::cli::max_worker_jobs);
  CHECK(seeds_of("0-1000000").second ==
        "--seeds lists more than 1000000 seeds");
  std::vector<std::uint64_t> seeds(pigpen::cli::max_worker_jobs, 0);
  CHECK_FALSE(pigpen::cli::parse_seed_list("1", seeds));
  CHECK(seeds.size() == pigpen::cli::max_worker_jobs);

  // A bad entry late in the value leaves earlier ones unapplied.
  std::vector<std::uint64_t> kept{9};
  CHECK_FALSE(pigpen::cli::parse_seed_list("1,2,x", kept));
  CHECK(kept == std::vector<std::uint64_t>{9});
}

TEST_CASE("rollout prefixes are visible ASCII", "[worker]") {
  CHECK(pigpen::cli::validate_rollout_prefix("run42"));
  CHECK(pigpen::cli::validate_rollout_prefix("exp/a-b_c.1"));
  CHECK(pigpen::cli::validate_rollout_prefix("").error() ==
        "--rollout-prefix cannot be empty");
  for (const std::string_view bad :
       {"run 42", "run\t42", "run\n", "r\xc3\xa9"}) {
    CHECK(pigpen::cli::validate_rollout_prefix(bad).error() ==
          "--rollout-prefix must be visible ASCII without spaces");
  }
}

TEST_CASE("jobs are seed-major with rollout ids and sampling seeds",
          "[worker]") {
  const std::vector<std::uint64_t> seeds{1003, 7};
  const auto jobs = pigpen::cli::WorkerJobs::create(seeds, 2, "run42", 10);
  REQUIRE(jobs);
  CHECK(jobs->size() == 4);
  CHECK(all_jobs(*jobs) == std::vector<WorkerJob>{
                               {.seed = 1003,
                                .sample = 0,
                                .rollout_id = "run42/1003/0",
                                .sampling_seed = 10},
                               {.seed = 1003,
                                .sample = 1,
                                .rollout_id = "run42/1003/1",
                                .sampling_seed = 11},
                               {.seed = 7,
                                .sample = 0,
                                .rollout_id = "run42/7/0",
                                .sampling_seed = 10},
                               {.seed = 7,
                                .sample = 1,
                                .rollout_id = "run42/7/1",
                                .sampling_seed = 11},
                           });

  // Without a base the sampling seed stays unset (server-random).
  const auto unseeded =
      pigpen::cli::WorkerJobs::create(seeds, 1, "rollout", {});
  REQUIRE(unseeded);
  REQUIRE(unseeded->size() == 2);
  CHECK(unseeded->at(1) == WorkerJob{.seed = 7,
                                     .sample = 0,
                                     .rollout_id = "rollout/7/0",
                                     .sampling_seed = std::nullopt});
}

TEST_CASE("job expansion rejects what would make rollouts ambiguous or "
          "overflow",
          "[worker]") {
  const auto error = [](const std::vector<std::uint64_t> &seeds,
                        const std::uint32_t samples,
                        const std::optional<std::uint32_t> base) {
    auto jobs = pigpen::cli::WorkerJobs::create(seeds, samples, "p", base);
    return jobs ? std::string{} : jobs.error();
  };
  CHECK(error({}, 1, {}) == "--seeds is required");
  CHECK(error({4, 5, 4}, 1, {}) == "--seeds lists seed 4 more than once");
  CHECK(error({1}, 0, {}) ==
        "--seeds and --samples must give 1..1000000 jobs, not 1 x 0");
  CHECK(error(std::vector<std::uint64_t>(1'001, 0), 1'000, {}) ==
        "--seeds lists seed 0 more than once");
  std::vector<std::uint64_t> many(1'001);
  for (std::uint64_t seed = 0; seed < many.size(); ++seed) {
    many[seed] = seed;
  }
  CHECK(error(many, 1'000, {}) ==
        "--seeds and --samples must give 1..1000000 jobs, not 1001 x 1000");
  many.pop_back();
  CHECK(error(many, 1'000, {}).empty());

  // The last sample's sampling seed must still fit in 32 bits.
  CHECK(error({1}, 1, u32_max).empty());
  CHECK(error({1}, 2, u32_max - 1).empty());
  CHECK(error({1}, 2, u32_max) ==
        "--sampling-seed-base 4294967295 leaves no room for 2 samples: the "
        "last sampling seed must not exceed 4294967295");
  const auto last = pigpen::cli::WorkerJobs::create(
      std::vector<std::uint64_t>{1}, 3, "p", u32_max - 2);
  REQUIRE(last);
  CHECK(last->at(2).sampling_seed == u32_max);

  // A full-size batch is described, not materialised: the last job is
  // built from its index alone.
  std::vector<std::uint64_t> seeds(pigpen::cli::max_worker_jobs / 4);
  for (std::uint64_t seed = 0; seed < seeds.size(); ++seed) {
    seeds[seed] = seed * 3;
  }
  const auto full = pigpen::cli::WorkerJobs::create(seeds, 4, "big", {});
  REQUIRE(full);
  CHECK(full->size() == pigpen::cli::max_worker_jobs);
  CHECK(full->at(full->size() - 1) == WorkerJob{.seed = (seeds.size() - 1) * 3,
                                                .sample = 3,
                                                .rollout_id = "big/749997/3",
                                                .sampling_seed = std::nullopt});
}

TEST_CASE("request headers take NAME=VALUE and keep the worker's own names",
          "[worker]") {
  std::vector<std::pair<std::string, std::string>> headers;
  REQUIRE(pigpen::cli::parse_request_header("X-Trainer-Run=42", headers));
  REQUIRE(pigpen::cli::parse_request_header("X-Empty=", headers));
  REQUIRE(pigpen::cli::parse_request_header("X-Eq=a=b", headers));
  CHECK(headers ==
        std::vector<std::pair<std::string, std::string>>{
            {"X-Trainer-Run", "42"}, {"X-Empty", ""}, {"X-Eq", "a=b"}});

  const auto error = [&headers](const std::string_view assignment) {
    auto parsed = pigpen::cli::parse_request_header(assignment, headers);
    return parsed ? std::string{} : parsed.error();
  };
  CHECK(error("X-Trainer") == "--header must be NAME=VALUE, not \"X-Trainer\"");
  CHECK(error("=1") == "--header needs a name before '='");
  CHECK(error("x-pigpen-rollout=r") ==
        "--header x-pigpen-rollout is set by the worker itself");
  CHECK(error("X-PIGPEN-SEED=1") ==
        "--header X-PIGPEN-SEED is set by the worker itself");
  CHECK(headers.size() == 3);
}

TEST_CASE("a rejected option always fails with its message and is hidden",
          "[worker][parser]") {
  pigpen::cli::OptionParser parser;
  std::uint64_t seed{};
  parser.integer("--seed-count", "INTEGER", seed, 0, 9, "Seeds");
  parser.rejected("--seed", "--seed is not accepted; use --seeds");
  const auto error = [&parser](std::vector<std::string_view> arguments) {
    auto parsed = parser.parse(arguments);
    return parsed ? std::string{} : parsed.error();
  };
  CHECK(error({"--seed"}) == "--seed is not accepted; use --seeds");
  CHECK(error({"--seed", "4"}) == "--seed is not accepted; use --seeds");
  CHECK(error({"--seed=4"}) == "--seed is not accepted; use --seeds");
  CHECK(error({"--seed-count", "4"}).empty());
  CHECK(seed == 4);
  CHECK_FALSE(parser.help().contains("--seed "));
  CHECK(parser.help().contains("--seed-count"));
}

TEST_CASE("job lines parse into their fields exactly as given",
          "[worker][jobs]") {
  using pigpen::agent::JobSpec;
  using pigpen::agent::parse_job_spec;
  CHECK(parse_job_spec(R"({"seed":7})") == JobSpec{.seed = 7});
  CHECK(parse_job_spec(
            R"( {"sampling_seed":4294967295,"rollout_id":"a/b","sample":3,)"
            R"("seed":18446744073709551615} )") ==
        JobSpec{.seed = u64_max,
                .sample = 3,
                .rollout_id = "a/b",
                .sampling_seed = u32_max});
  // null is the same as leaving sampling_seed out.
  CHECK(parse_job_spec(R"({"seed":0,"sampling_seed":null})") ==
        JobSpec{.seed = 0});
  // A carriage return before the newline is JSON whitespace.
  CHECK(parse_job_spec("{\"seed\":1}\r") == JobSpec{.seed = 1});
}

TEST_CASE("job lines that are not jobs say why", "[worker][jobs]") {
  const auto error = [](const std::string_view line) {
    auto spec = pigpen::agent::parse_job_spec(line);
    return spec ? std::string{} : spec.error();
  };
  const std::string seed_range =
      "\"seed\" must be an integer in 0..18446744073709551615";
  const std::string sample_range =
      "\"sample\" must be an integer in 0..4294967295";
  CHECK(error("") == "invalid JSON at byte 1");
  CHECK(error("nope") == "invalid JSON at byte 2");
  CHECK(error(R"({"seed":1)") == "invalid JSON at byte 10");
  CHECK(error(R"({"seed":1} {"seed":2})") == "invalid JSON at byte 12");
  CHECK(error("{\"seed\":1,\"rollout_id\":\"r\xff\"}")
            .starts_with("invalid JSON at byte"));
  CHECK(error("[1]") == "a job must be a JSON object");
  CHECK(error("7") == "a job must be a JSON object");
  CHECK(error(R"({"seed":1,"samples":2})") == "unknown key \"samples\"");
  CHECK(error("{}") == "\"seed\" is required");
  CHECK(error(R"({"sample":1})") == "\"seed\" is required");
  for (const auto *const seed :
       {"-1", "1.0", "1e3", "\"1\"", "null", "true", "18446744073709551616"}) {
    CHECK(error(std::string{R"({"seed":)"} + seed + "}") == seed_range);
  }
  CHECK(error(R"({"seed":1,"sample":4294967296})") == sample_range);
  CHECK(error(R"({"seed":1,"sample":-1})") == sample_range);
  CHECK(error(R"({"seed":1,"sample":null})") == sample_range);
  CHECK(error(R"({"seed":1,"rollout_id":7})") ==
        "\"rollout_id\" must be a string");
  CHECK(error(R"({"seed":1,"rollout_id":null})") ==
        "\"rollout_id\" must be a string");
  for (const auto *const value : {"4294967296", "\"5\"", "-2", "0.5"}) {
    CHECK(error(std::string{R"({"seed":1,"sampling_seed":)"} + value + "}") ==
          "\"sampling_seed\" must be null or an integer in 0..4294967295");
  }
}

TEST_CASE("a job stream fills in defaults and keeps rollout ids unique",
          "[worker][jobs]") {
  pigpen::cli::JobStream stream{"run42"};
  const auto accept = [&stream](const std::string_view line,
                                const std::size_t number) {
    auto job = stream.accept(line, number);
    return job ? *job : WorkerJob{.rollout_id = "error: " + job.error()};
  };
  CHECK(accept(R"({"seed":1003})", 1) ==
        WorkerJob{.seed = 1003, .sample = 0, .rollout_id = "run42/1003/0"});
  CHECK(accept(R"({"seed":1003,"sample":2,"sampling_seed":9})", 2) ==
        WorkerJob{.seed = 1003,
                  .sample = 2,
                  .rollout_id = "run42/1003/2",
                  .sampling_seed = 9});
  CHECK(accept(R"({"seed":5,"rollout_id":"trial-9/a"})", 3) ==
        WorkerJob{.seed = 5, .sample = 0, .rollout_id = "trial-9/a"});

  // Repeating an id, spelled out or defaulted, names the line that used it.
  CHECK(accept(R"({"seed":6,"rollout_id":"run42/1003/0"})", 4).rollout_id ==
        "error: rollout id \"run42/1003/0\" was already used on line 1");
  CHECK(accept(R"({"seed":1003,"sample":2})", 5).rollout_id ==
        "error: rollout id \"run42/1003/2\" was already used on line 2");
  CHECK(accept(R"({"seed":5,"rollout_id":"trial-9/a"})", 6).rollout_id ==
        "error: rollout id \"trial-9/a\" was already used on line 3");

  // An id must be a header value: non-empty visible ASCII (the values are
  // JSON text, so the tab is an escape).
  for (const std::string_view bad : {"", "a b", "tab\\there", "r\xc3\xa9"}) {
    CHECK(accept(std::string{R"({"seed":1,"rollout_id":")"} + std::string{bad} +
                     "\"}",
                 7)
              .rollout_id ==
          "error: \"rollout_id\" must be non-empty visible ASCII without "
          "spaces");
  }
  CHECK(accept("[]", 8).rollout_id == "error: a job must be a JSON object");

  // A rejected line reserves nothing: its id is still free afterwards.
  CHECK(accept(R"({"seed":8,"rollout_id":"free","extra":1})", 9).rollout_id ==
        "error: unknown key \"extra\"");
  CHECK(accept(R"({"seed":8,"rollout_id":"free"})", 10) ==
        WorkerJob{.seed = 8, .sample = 0, .rollout_id = "free"});
}
