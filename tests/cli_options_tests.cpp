/// @file cli_options_tests.cpp
/// @brief Covers the shared command-line layer: OptionParser's value syntax
/// and exact diagnostics, the shared Config flags and their validation, the
/// generated help text, and TerminationSignal.

#include "agent/config.hpp"
#include "agent/reward.hpp"
#include "agent/session_options.hpp"
#include "cli/config_options.hpp"
#include "cli/option_parser.hpp"
#include "cli/termination_signal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::string_view_literals;

/// @brief Parse @p arguments with @p parser; empty on success, otherwise
/// the error text.
[[nodiscard]] std::string
parse_error(pigpen::cli::OptionParser &parser,
            const std::vector<std::string_view> &arguments) {
  const auto parsed = parser.parse(arguments);
  return parsed ? std::string{} : parsed.error();
}

struct Targets {
  bool verbose{};
  std::string name{"initial"};
  std::optional<std::string> note{};
  std::optional<std::filesystem::path> directory{};
  std::uint32_t count{7};
  std::optional<std::uint32_t> limit{};
  double ratio{};
  std::vector<std::string> tags{};
};

[[nodiscard]] pigpen::cli::OptionParser make_parser(Targets &targets) {
  pigpen::cli::OptionParser parser;
  parser.flag("--verbose", targets.verbose, true, "Talk more");
  parser.text("--name", "TEXT", targets.name, "A name");
  parser.text("--note", "TEXT", targets.note, "A note",
              pigpen::cli::EmptyText::rejected);
  parser.text("--dir", "PATH", targets.directory, "A directory");
  parser.integer("--count", "INTEGER", targets.count, 1, 10, "A count");
  parser.integer("--limit", "INTEGER", targets.limit, 0,
                 std::numeric_limits<std::uint32_t>::max(), "A limit");
  parser.real("--ratio", "NUMBER", targets.ratio, 0.0, 2.0, "A ratio");
  parser.value(
      "--tag", "TAG", "A repeatable tag",
      [&targets](const std::string_view tag) -> pigpen::cli::ParseResult {
        if (tag == "bad") {
          return std::unexpected("tag bad is not allowed");
        }
        targets.tags.emplace_back(tag);
        return {};
      });
  return parser;
}

} // namespace

TEST_CASE("option parser accepts both value syntaxes for every kind") {
  Targets targets;
  auto parser = make_parser(targets);
  CHECK(parse_error(parser, {}).empty());
  CHECK(targets.name == "initial");
  CHECK_FALSE(targets.limit);

  CHECK(parse_error(parser,
                    {"--verbose", "--name", "blob", "--note=hello", "--dir",
                     "out/logs", "--count=3", "--limit", "4294967295",
                     "--ratio", "0.25", "--tag", "a", "--tag=b"})
            .empty());
  CHECK(targets.verbose);
  CHECK(targets.name == "blob");
  CHECK(targets.note == "hello");
  CHECK(targets.directory == std::filesystem::path{"out/logs"});
  CHECK(targets.count == 3);
  CHECK(targets.limit == std::numeric_limits<std::uint32_t>::max());
  CHECK(targets.ratio == 0.25);
  CHECK(targets.tags == std::vector<std::string>{"a", "b"});

  // An inline value keeps everything after the first '='.
  CHECK(parse_error(parser, {"--name=a=b"}).empty());
  CHECK(targets.name == "a=b");
  // A separate value may be empty unless the option rejects that.
  CHECK(parse_error(parser, {"--name", ""}).empty());
  CHECK(targets.name.empty());
}

TEST_CASE("option parser reports each problem with the existing text") {
  Targets targets;
  auto parser = make_parser(targets);
  CHECK(parse_error(parser, {"blob"}) ==
        "unexpected positional argument: blob");
  CHECK(parse_error(parser, {"--bogus"}) == "unknown option: --bogus");
  CHECK(parse_error(parser, {"--bogus=1"}) == "unknown option: --bogus");
  CHECK(parse_error(parser, {"--verbose=yes"}) ==
        "--verbose does not take a value");
  CHECK(parse_error(parser, {"--verbose="}) ==
        "--verbose does not take a value");
  CHECK(parse_error(parser, {"--name"}) == "--name requires a value");
  CHECK(parse_error(parser, {"--name="}) == "--name requires a value");
  CHECK(parse_error(parser, {"--name", "--verbose"}) ==
        "--name requires a value");
  CHECK(parse_error(parser, {"--note", ""}) == "--note requires a value");
  CHECK(parse_error(parser, {"--count", "11"}) ==
        "--count must be in the range 1..10");
  CHECK(parse_error(parser, {"--count=0"}) ==
        "--count must be in the range 1..10");
  CHECK(parse_error(parser, {"--count", "-1"}) ==
        "--count must be in the range 1..10");
  CHECK(parse_error(parser, {"--count", "3x"}) ==
        "--count must be in the range 1..10");
  CHECK(parse_error(parser, {"--count", ""}) ==
        "--count must be in the range 1..10");
  CHECK(parse_error(parser, {"--limit", "4294967296"}) ==
        "--limit must be in the range 0..4294967295");
  CHECK(parse_error(parser, {"--ratio", "nan"}) ==
        "--ratio must be a finite number in the range 0.0..2.0");
  CHECK(parse_error(parser, {"--ratio=inf"}) ==
        "--ratio must be a finite number in the range 0.0..2.0");
  CHECK(parse_error(parser, {"--ratio", "2.5"}) ==
        "--ratio must be a finite number in the range 0.0..2.0");
  CHECK(parse_error(parser, {"--tag", "bad"}) == "tag bad is not allowed");

  // The first error wins and later arguments are not applied.
  CHECK(parse_error(parser, {"--count", "99", "--name", "late"}) ==
        "--count must be in the range 1..10");
  CHECK(targets.name != "late");
}

TEST_CASE("option parser help aligns descriptions and wraps them") {
  pigpen::cli::OptionParser parser;
  bool flag{};
  std::uint32_t rounds{};
  std::string text;
  parser.flag("--help", flag, true, "Show this help and exit");
  parser.integer("--max-tool-rounds", "INTEGER", rounds, 1, 64,
                 "Tool rounds per turn");
  parser.text("--a-very-long-option-name", "VALUE", text, "Wrapped below");
  parser.text("--long", "TEXT", text,
              "one two three four five six seven eight nine ten eleven "
              "twelve thirteen fourteen");
  CHECK(parser.help() ==
        "  --help                    Show this help and exit\n"
        "  --max-tool-rounds INTEGER Tool rounds per turn\n"
        "  --a-very-long-option-name VALUE\n"
        "                            Wrapped below\n"
        "  --long TEXT               one two three four five six seven eight "
        "nine ten\n"
        "                            eleven twelve thirteen fourteen\n");
}

TEST_CASE("number parsing helpers name the option and the range") {
  CHECK(pigpen::cli::parse_unsigned("--seed", "18446744073709551615", 0,
                                    std::numeric_limits<std::uint64_t>::max())
            .value() == std::numeric_limits<std::uint64_t>::max());
  CHECK(pigpen::cli::parse_unsigned("--seed", "18446744073709551616", 0,
                                    std::numeric_limits<std::uint64_t>::max())
            .error() == "--seed must be in the range 0..18446744073709551615");
  CHECK(pigpen::cli::parse_real("--t", "1.5", 0.0, 2.0).value() == 1.5);
  CHECK(pigpen::cli::parse_real("--t", "0.5", -1.0, 0.25).error() ==
        "--t must be a finite number in the range -1.0..0.25");
  CHECK(pigpen::cli::format_real(2.0) == "2.0");
  CHECK(pigpen::cli::format_real(0.25) == "0.25");
}

TEST_CASE("shared config options fill every Config field and session option") {
  pigpen::agent::Config config;
  pigpen::agent::SessionOptions options;
  pigpen::cli::OptionParser parser;
  pigpen::cli::add_config_options(parser, config, options);

  CHECK(parse_error(parser, {"--base-url",
                             "http://model-host.test/v1",
                             "--model=registry.example/pig-model:Q4_K_M",
                             "--seed",
                             "18446744073709551615",
                             "--turns",
                             "10000",
                             "--max-tool-rounds=64",
                             "--temperature",
                             "2.0",
                             "--sampling-seed",
                             "4294967295",
                             "--hidden-values",
                             "--no-reward-feedback",
                             "--opaque-look",
                             "--prompt-variant",
                             "blind",
                             "--reward",
                             "invalid_call=-2",
                             "--reward=zero_tool_turn=-1.5"})
            .empty());
  CHECK(config.base_url == "http://model-host.test/v1");
  CHECK(config.model == "registry.example/pig-model:Q4_K_M");
  CHECK(config.seed == std::numeric_limits<std::uint64_t>::max());
  CHECK(config.turn_budget == pigpen::agent::turn_budget_limit);
  CHECK(config.max_tool_rounds == pigpen::agent::tool_rounds_limit);
  CHECK(config.temperature == 2.0);
  CHECK(config.sampling_seed == std::numeric_limits<std::uint32_t>::max());
  CHECK_FALSE(config.known_item_values);
  CHECK_FALSE(config.reward_feedback);
  CHECK(config.opaque_look);
  CHECK(options.prompt_variant == "blind");
  CHECK(options.reward_weights.invalid_call == -2.0);
  CHECK(options.reward_weights.zero_tool_turn == -1.5);
  CHECK_FALSE(options.log_directory);
  CHECK(pigpen::cli::validate_config_options(config, options));
}

TEST_CASE("shared config options keep the headless CLI's diagnostics") {
  const auto error = [](const std::vector<std::string_view> &arguments) {
    pigpen::agent::Config config;
    pigpen::agent::SessionOptions options;
    pigpen::cli::OptionParser parser;
    pigpen::cli::add_config_options(parser, config, options);
    return parse_error(parser, arguments);
  };
  CHECK(error({"--max-tool-rounds", "65"}) ==
        "--max-tool-rounds must be in the range 1..64");
  CHECK(error({"--turns", "0"}) == "--turns must be in the range 1..10000");
  CHECK(error({"--temperature", "nan"}) ==
        "--temperature must be a finite number in the range 0.0..2.0");
  CHECK(error({"--sampling-seed", "4294967296"}) ==
        "--sampling-seed must be in the range 0..4294967295");
  CHECK(error({"--reward", "bogus=1"})
            .starts_with("unknown reward weight \"bogus\""));
  CHECK(error({"--reward=invalid_call=inf"})
            .starts_with("reward weight invalid_call must be a finite number"));
  CHECK(error({"--hidden-values=true"}) ==
        "--hidden-values does not take a value");
}

TEST_CASE("shared config options can leave --seed to the front end") {
  pigpen::agent::Config config;
  pigpen::agent::SessionOptions options;
  pigpen::cli::OptionParser parser;
  pigpen::cli::add_config_options(parser, config, options,
                                  pigpen::cli::WorldSeedOption::omitted);
  CHECK(parse_error(parser, {"--seed", "4"}) == "unknown option: --seed");
  CHECK_FALSE(parser.help().contains("--seed "));
  CHECK(parse_error(parser, {"--turns", "3", "--model", "m"}).empty());
  CHECK(config.turn_budget == 3);
  CHECK(config.seed == 0);
}

TEST_CASE("UTF-8 validation accepts well-formed text only") {
  using pigpen::cli::is_valid_utf8;
  CHECK(is_valid_utf8(""));
  CHECK(is_valid_utf8("pig-model:Q4_K_M"));
  CHECK(is_valid_utf8("caf\xc3\xa9"));                // U+00E9
  CHECK(is_valid_utf8("\xe2\x82\xac"));               // U+20AC
  CHECK(is_valid_utf8("\xf0\x9f\x90\x96"));           // U+1F416
  CHECK(is_valid_utf8("\xf4\x8f\xbf\xbf"));           // U+10FFFF
  CHECK(is_valid_utf8("\xed\x9f\xbf"));               // U+D7FF
  CHECK_FALSE(is_valid_utf8("\xff"));                 // never a lead byte
  CHECK_FALSE(is_valid_utf8("\x80"));                 // stray continuation
  CHECK_FALSE(is_valid_utf8("caf\xc3"));              // truncated
  CHECK_FALSE(is_valid_utf8("\xe2\x82"));             // truncated
  CHECK_FALSE(is_valid_utf8("\xc3\x28"));             // bad continuation
  CHECK_FALSE(is_valid_utf8("\xc0\xaf"));             // overlong '/'
  CHECK_FALSE(is_valid_utf8("\xe0\x80\xaf"));         // overlong
  CHECK_FALSE(is_valid_utf8("\xf0\x80\x80\xaf"));     // overlong
  CHECK_FALSE(is_valid_utf8("\xed\xa0\x80"));         // surrogate U+D800
  CHECK_FALSE(is_valid_utf8("\xf4\x90\x80\x80"));     // past U+10FFFF
  CHECK_FALSE(is_valid_utf8("\xf8\x88\x80\x80\x80")); // five bytes
  CHECK(pigpen::cli::require_utf8("--model", "ok"));
  CHECK(pigpen::cli::require_utf8("--model", "\xff").error() ==
        "--model must be valid UTF-8");
}

TEST_CASE("shared config validation rejects text that is not UTF-8") {
  const auto error = [](const auto &change) {
    pigpen::agent::Config config;
    config.model = "m";
    pigpen::agent::SessionOptions options;
    change(config, options);
    const auto valid = pigpen::cli::validate_config_options(config, options);
    return valid ? std::string{} : valid.error();
  };
  CHECK(error([](auto &, auto &) {}).empty());
  CHECK(error([](auto &config, auto &) { config.model = "pig\xff"; }) ==
        "--model must be valid UTF-8");
  CHECK(error([](auto &config, auto &) {
          config.base_url = "http://h\xc3/v1";
        }) == "--base-url must be valid UTF-8");
  CHECK(error([](auto &, auto &options) {
          options.prompt_variant = "\xed\xa0\x80";
        }) == "--prompt-variant must be valid UTF-8");
  // Emptiness is still reported first.
  CHECK(error([](auto &config, auto &) {
          config.base_url.clear();
          config.model = "\xff";
        }) == "--base-url cannot be empty");
}

TEST_CASE("shared config validation reports the first empty value in order") {
  pigpen::agent::Config config;
  pigpen::agent::SessionOptions options{.log_directory = ""};
  config.base_url.clear();
  options.prompt_variant.clear();
  CHECK(pigpen::cli::validate_config_options(config, options).error() ==
        "--base-url cannot be empty");
  config.base_url = "http://127.0.0.1:11434/v1";
  CHECK(pigpen::cli::validate_config_options(config, options).error() ==
        "--model is required");
  config.model = "m";
  CHECK(pigpen::cli::validate_config_options(config, options).error() ==
        "--log-dir cannot be empty");
  options.log_directory.reset();
  CHECK(pigpen::cli::validate_config_options(config, options).error() ==
        "--prompt-variant cannot be empty");
  options.prompt_variant = "default";
  CHECK(pigpen::cli::validate_config_options(config, options));
}

TEST_CASE("shared config help states defaults and every reward weight name") {
  pigpen::agent::Config config;
  config.turn_budget = 12;
  pigpen::agent::SessionOptions options;
  pigpen::cli::OptionParser parser;
  pigpen::cli::add_config_options(parser, config, options);
  const auto help = parser.help();
  CHECK(help.contains("  --base-url URL            Model endpoint (default: "
                      "http://127.0.0.1:11434/v1)\n"));
  CHECK(help.contains("Episode turn budget, 1..10000 (default: 12)"));
  CHECK(help.contains("Sampling temperature, 0.0..2.0 (default: 0.0)"));
  CHECK(help.contains("(default: unset)"));
  CHECK(help.contains("  --reward NAME=VALUE"));
  for (const auto &field : pigpen::agent::reward_weight_fields) {
    CHECK(help.contains(std::string{field.name}));
  }
}

TEST_CASE("termination signal records SIGINT and SIGTERM until reinstalled") {
  pigpen::cli::TerminationSignal signal;
  REQUIRE(signal.install());
  CHECK(signal.received() == 0);
  REQUIRE(std::raise(SIGTERM) == 0);
  CHECK(signal.received() == SIGTERM);
  REQUIRE(std::raise(SIGINT) == 0);
  CHECK(signal.received() == SIGINT);
  REQUIRE(signal.install());
  CHECK(signal.received() == 0);
}
