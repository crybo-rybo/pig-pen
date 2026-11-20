/// @file fake_llamad_main.cpp
/// @brief pigpen_fake_llamad: the scripted fake daemon as a process, for the
/// Python CLI tests. It needs no Python gRPC packages.
///
/// Usage: pigpen_fake_llamad --socket PATH --script FILE --requests FILE
///                           [--model-info-calls FILE] [--model-info-gate FILE]
///
/// The script is one JSON object: an optional "model_info" object (see
/// FakeModelInfo; durations are integers with an `_ms` suffix, for example
/// "reply_delay_ms") and a "replies" array, one object per Chat call (see
/// FakeReply; "hang_progress_interval_ms"). Each received request is
/// appended to the requests file as one JSON line before the reply is
/// written. Each GetModelInfo call appends the line "GetModelInfo" to the
/// model-info-calls file, before its reply_delay starts. With a gate file,
/// each GetModelInfo call then waits until that file exists (at most 30 s),
/// so a test can act while the handshake is in progress without a race
/// against a timer. The process prints
/// "ready" when it listens, and stops on SIGINT or SIGTERM.
#include "support/fake_llamad.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <signal.h>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

using nlohmann::json;
using pigpen::testing::FakeFinishReason;

[[nodiscard]] FakeFinishReason finish_reason(const std::string &name) {
  static const std::map<std::string, FakeFinishReason, std::less<>> names{
      {"eog", FakeFinishReason::eog},
      {"length", FakeFinishReason::length},
      {"stop", FakeFinishReason::stop},
      {"tool_calls", FakeFinishReason::tool_calls},
      {"unspecified", FakeFinishReason::unspecified},
  };
  return names.at(name);
}

[[nodiscard]] pigpen::testing::FakeReply reply_from(const json &value) {
  pigpen::testing::FakeReply reply;
  reply.text = value.value("text", std::vector<std::string>{});
  reply.progress = value.value("progress", 0U);
  if (const auto finish = value.find("finish"); finish != value.end()) {
    if (finish->is_null()) {
      reply.finish.reset();
    } else {
      reply.finish = finish_reason(finish->get<std::string>());
    }
  }
  for (const auto &call : value.value("tool_calls", json::array())) {
    reply.tool_calls.push_back({.id = call.at("id"),
                                .name = call.at("name"),
                                .arguments_json = call.at("arguments")});
  }
  reply.prompt_tokens = value.value("prompt_tokens", 0);
  reply.completion_tokens = value.value("completion_tokens", 0);
  reply.cached_prompt_tokens = value.value("cached_prompt_tokens", 0);
  reply.prompt_ms = value.value("prompt_ms", 0.0);
  reply.completion_ms = value.value("completion_ms", 0.0);
  reply.status_code = value.value("status_code", 0);
  reply.status_message = value.value("status_message", std::string{});
  reply.hang = value.value("hang", false);
  reply.hang_progress_interval =
      std::chrono::milliseconds{value.value("hang_progress_interval_ms", 0)};
  return reply;
}

[[nodiscard]] json to_json(const pigpen::testing::ReceivedRequest &request) {
  json messages = json::array();
  for (const auto &message : request.messages) {
    json calls = json::array();
    for (const auto &call : message.tool_calls) {
      calls.push_back({{"id", call.id},
                       {"name", call.name},
                       {"arguments", call.arguments_json}});
    }
    messages.push_back({{"role", message.role},
                        {"content", message.content},
                        {"tool_calls", calls},
                        {"tool_call_id", message.tool_call_id}});
  }
  json tools = json::array();
  for (const auto &tool : request.tools) {
    tools.push_back({{"name", tool.name},
                     {"description", tool.description},
                     {"parameters", json::parse(tool.parameters_json_schema)}});
  }
  json sampling = json::object();
  if (request.temperature) {
    sampling["temperature"] = *request.temperature;
  }
  if (request.seed) {
    sampling["seed"] = *request.seed;
  }
  if (request.max_tokens) {
    sampling["max_tokens"] = *request.max_tokens;
  }
  return {{"messages", messages},
          {"tools", tools},
          {"sampling", sampling},
          {"reasoning", request.reasoning}};
}

} // namespace

int main(const int argc, char **argv) {
  std::map<std::string, std::string, std::less<>> options;
  for (int index = 1; index + 1 < argc; index += 2) {
    options[argv[index]] = argv[index + 1];
  }
  if (!options.contains("--socket") || !options.contains("--script") ||
      !options.contains("--requests")) {
    std::cerr << "usage: pigpen_fake_llamad --socket PATH --script FILE "
                 "--requests FILE\n";
    return 2;
  }

  // Block the stop signals before gRPC starts its threads, then wait for one.
  sigset_t stop_signals;
  sigemptyset(&stop_signals);
  sigaddset(&stop_signals, SIGINT);
  sigaddset(&stop_signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

  std::ifstream script_stream{options.at("--script")};
  const auto script = json::parse(script_stream);
  pigpen::testing::FakeModelInfo info;
  if (const auto model = script.find("model_info"); model != script.end()) {
    info.description = model->value("description", info.description);
    info.n_ctx = model->value("n_ctx", info.n_ctx);
    info.n_ctx_train = model->value("n_ctx_train", info.n_ctx_train);
    info.n_params = model->value("n_params", info.n_params);
    info.size_bytes = model->value("size_bytes", info.size_bytes);
    info.has_chat_template =
        model->value("has_chat_template", info.has_chat_template);
    info.serves_embeddings =
        model->value("serves_embeddings", info.serves_embeddings);
    info.model_name = model->value("model_name", info.model_name);
    info.model_file = model->value("model_file", info.model_file);
    info.llamad_version = model->value("llamad_version", info.llamad_version);
    info.max_request_bytes =
        model->value("max_request_bytes", info.max_request_bytes);
    info.reply_delay =
        std::chrono::milliseconds{model->value("reply_delay_ms", 0)};
    info.status_code = model->value("status_code", 0);
    info.status_message = model->value("status_message", std::string{});
  }

  std::mutex output_mutex;
  std::ofstream requests{options.at("--requests"), std::ios::app};
  std::ofstream model_info_calls;
  std::optional<std::filesystem::path> gate;
  if (const auto path = options.find("--model-info-gate");
      path != options.end()) {
    gate = path->second;
  }
  if (const auto path = options.find("--model-info-calls");
      path != options.end()) {
    model_info_calls.open(path->second, std::ios::app);
  }
  auto server = pigpen::testing::FakeLlamad::start(
      options.at("--socket"), info,
      [&](const pigpen::testing::ReceivedRequest &request) {
        const std::scoped_lock lock{output_mutex};
        requests << to_json(request).dump() << '\n' << std::flush;
      },
      [&] {
        {
          const std::scoped_lock lock{output_mutex};
          if (model_info_calls.is_open()) {
            model_info_calls << "GetModelInfo\n" << std::flush;
          }
        }
        if (gate) {
          const auto until =
              std::chrono::steady_clock::now() + std::chrono::seconds{30};
          std::error_code ignored;
          while (!std::filesystem::exists(*gate, ignored) &&
                 std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
          }
        }
      });
  if (!server) {
    std::cerr << "could not listen on " << options.at("--socket") << '\n';
    return 1;
  }
  for (const auto &reply : script.value("replies", json::array())) {
    server->enqueue(reply_from(reply));
  }
  std::cout << "ready" << std::endl;

  int received{};
  sigwait(&stop_signals, &received);
  server.reset();
  return 0;
}
