/// @file text_catalog_tests.cpp
/// @brief Catalog decoding, compile-time storage, and malformed-input checks.
#include "text/catalog.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace {
constexpr unsigned char prompt_source[] = {
#embed "../resources/prompts.json"
};
constexpr unsigned char cli_source[] = {
#embed "../resources/cli.json"
};
constexpr auto prompts = pigpen::text::make_catalog<prompt_source>();
constexpr auto cli = pigpen::text::make_catalog<cli_source>();
static_assert(prompts.get("human_guidance") == "\n\nHuman guidance:\n");
static_assert(cli.get("headless_usage").starts_with("Usage: {}"));

constexpr bool constexpr_decoding() {
  const auto fields =
      pigpen::text::detail::Reader{
          R"({"text":["hello ","\u0077orld\n","\ud83d\udc37"],"empty":[]})"}
          .read();
  return fields[0].second == "hello world\n🐷" && fields[1].second.empty();
}
static_assert(constexpr_decoding());

template <typename Catalog, std::size_t Size>
void compare_json(const Catalog &catalog, const unsigned char (&source)[Size]) {
  const auto json = nlohmann::json::parse(source, source + Size);
  REQUIRE(catalog.entries.size() == json.size());
  for (const auto &[key, value] : json.items()) {
    std::string expected;
    if (value.is_array()) {
      for (const auto &fragment : value) {
        expected += fragment.template get<std::string>();
      }
    } else {
      expected = value.template get<std::string>();
    }
    const auto actual = catalog.get(key);
    CHECK(actual == expected);
    CHECK(actual.data()[actual.size()] == '\0');
  }
}
} // namespace

TEST_CASE("Embedded text catalogs agree with the JSON parser") {
  compare_json(prompts, prompt_source);
  compare_json(cli, cli_source);
  const auto copy = prompts;
  CHECK(copy.get("system_intro") == prompts.get("system_intro"));
  CHECK_THROWS_AS(copy.get("absent"), std::invalid_argument);
}

TEST_CASE("Text catalog strings decode JSON escapes and UTF-8") {
  for (const std::string_view json : {
           R"({"a":"\"\\\/\b\f\n\r\t\u0000"})",
           R"({"a":"\u00e9\u20ac\ud83d\udc37"})",
           R"({"a":"é€🐷"})",
           R"({"a": ["", "one", " two"], "b": ""})",
           " \n\r\t{} \n",
       }) {
    const auto fields = pigpen::text::detail::Reader{json}.read();
    const auto expected = nlohmann::json::parse(json);
    for (const auto &[key, value] : fields) {
      std::string decoded;
      if (expected.at(key).is_array()) {
        for (const auto &fragment : expected.at(key)) {
          decoded += fragment.get<std::string>();
        }
      } else {
        decoded = expected.at(key).get<std::string>();
      }
      CHECK(value == decoded);
    }
  }
}

TEST_CASE("Text catalogs reject malformed JSON and unsupported value types") {
  for (const std::string_view json : {
           "",
           "{",
           "[]",
           R"({"a":null})",
           R"({"a":42})",
           R"({"a":true})",
           R"({"a":{}})",
           R"({"a":[[]]})",
           R"({"a":["x",]})",
           R"({"a":"x",})",
           R"({"a":"x"} trailing)",
           R"({"a":"x","a":"y"})",
           R"({"a":"\q"})",
           R"({"a":"\u123"})",
           R"({"a":"\uxxxx"})",
           R"({"a":"\ud800"})",
           R"({"a":"\udc00"})",
           R"({"a":"\ud800\u0041"})",
           R"({"a":"unfinished})",
           "{\"a\":\"\n\"}",
           "{\"a\":\"\xc0\x80\"}",
           "{\"a\":\"\xe0\x80\x80\"}",
           "{\"a\":\"\xed\xa0\x80\"}",
           "{\"a\":\"\xf4\x90\x80\x80\"}",
           "{\"a\":\"\xc2x\"}",
       }) {
    INFO(json);
    CHECK_THROWS_AS(pigpen::text::detail::Reader{json}.read(),
                    std::invalid_argument);
  }
}
