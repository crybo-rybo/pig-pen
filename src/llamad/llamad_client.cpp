/// @file llamad_client.cpp
/// @brief LlamadChatClient implementation; the contract is in the header.
#include "llamad/llamad_client.hpp"

#include <grpcpp/grpcpp.h>
#include <llamad/v1/llamad.grpc.pb.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace pigpen::llamad {
namespace {

namespace wire = ::llamad::v1;

using Terminal = std::expected<core::ChatFinish, core::ChatError>;

/// @brief The canonical gRPC spelling of @p code, as llamad's docs use it.
[[nodiscard]] std::string_view status_name(const grpc::StatusCode code) {
  switch (code) {
  case grpc::StatusCode::OK:
    return "OK";
  case grpc::StatusCode::CANCELLED:
    return "CANCELLED";
  case grpc::StatusCode::UNKNOWN:
    return "UNKNOWN";
  case grpc::StatusCode::INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case grpc::StatusCode::DEADLINE_EXCEEDED:
    return "DEADLINE_EXCEEDED";
  case grpc::StatusCode::NOT_FOUND:
    return "NOT_FOUND";
  case grpc::StatusCode::ALREADY_EXISTS:
    return "ALREADY_EXISTS";
  case grpc::StatusCode::PERMISSION_DENIED:
    return "PERMISSION_DENIED";
  case grpc::StatusCode::RESOURCE_EXHAUSTED:
    return "RESOURCE_EXHAUSTED";
  case grpc::StatusCode::FAILED_PRECONDITION:
    return "FAILED_PRECONDITION";
  case grpc::StatusCode::ABORTED:
    return "ABORTED";
  case grpc::StatusCode::OUT_OF_RANGE:
    return "OUT_OF_RANGE";
  case grpc::StatusCode::UNIMPLEMENTED:
    return "UNIMPLEMENTED";
  case grpc::StatusCode::INTERNAL:
    return "INTERNAL";
  case grpc::StatusCode::UNAVAILABLE:
    return "UNAVAILABLE";
  case grpc::StatusCode::DATA_LOSS:
    return "DATA_LOSS";
  case grpc::StatusCode::UNAUTHENTICATED:
    return "UNAUTHENTICATED";
  default:
    break;
  }
  return "UNKNOWN";
}

/// @brief "llamad <STATUS_NAME>: <message>".
[[nodiscard]] std::string describe(const grpc::Status &status) {
  auto text = "llamad " + std::string{status_name(status.error_code())};
  if (!status.error_message().empty()) {
    text += ": " + status.error_message();
  }
  return text;
}

[[nodiscard]] std::uint64_t count(const std::int32_t value) noexcept {
  return static_cast<std::uint64_t>(std::max(value, std::int32_t{0}));
}

[[nodiscard]] core::ChatToolCall from_wire(const wire::ToolCall &call) {
  return {.id = call.id(),
          .name = call.name(),
          .arguments_json = call.arguments_json()};
}

[[nodiscard]] Terminal from_wire(const wire::Finish &finish) {
  core::ChatFinish result{
      .stats = {.prompt_tokens = count(finish.stats().prompt_tokens()),
                .completion_tokens = count(finish.stats().completion_tokens()),
                .cached_prompt_tokens =
                    count(finish.stats().cached_prompt_tokens())},
  };
  switch (finish.reason()) {
  case wire::FINISH_REASON_EOG:
    result.reason = core::ChatFinishReason::end_of_generation;
    break;
  case wire::FINISH_REASON_LENGTH:
    result.reason = core::ChatFinishReason::length;
    break;
  case wire::FINISH_REASON_STOP:
    result.reason = core::ChatFinishReason::stop;
    break;
  case wire::FINISH_REASON_CANCELLED:
    result.reason = core::ChatFinishReason::cancelled;
    break;
  case wire::FINISH_REASON_TOOL_CALLS:
    result.reason = core::ChatFinishReason::tool_calls;
    break;
  default:
    return std::unexpected(core::ChatError{
        .message = "llamad sent an unknown finish reason " +
                   std::to_string(static_cast<int>(finish.reason()))});
  }
  result.tool_calls.reserve(static_cast<std::size_t>(finish.tool_calls_size()));
  for (const auto &call : finish.tool_calls()) {
    result.tool_calls.push_back(from_wire(call));
  }
  return result;
}

[[nodiscard]] wire::ChatRequest to_wire(const core::ChatRequest &request) {
  wire::ChatRequest result;
  for (const auto &message : request.messages) {
    auto &sent = *result.add_messages();
    sent.set_role(std::string{core::chat_role_name(message.role)});
    sent.set_content(message.content);
    for (const auto &call : message.tool_calls) {
      auto &sent_call = *sent.add_tool_calls();
      sent_call.set_id(call.id);
      sent_call.set_name(call.name);
      sent_call.set_arguments_json(call.arguments_json);
    }
    sent.set_tool_call_id(message.tool_call_id);
  }
  for (const auto &tool : request.tools) {
    auto &sent = *result.add_tools();
    sent.set_name(tool.name);
    sent.set_description(tool.description);
    sent.set_parameters_json_schema(tool.parameters_json_schema);
  }
  auto &sampling = *result.mutable_sampling();
  sampling.set_temperature(static_cast<float>(request.sampling.temperature));
  sampling.set_max_tokens(static_cast<std::int32_t>(
      std::min<std::uint32_t>(request.sampling.max_tokens, INT32_MAX)));
  if (request.sampling.seed) {
    sampling.set_seed(*request.sampling.seed);
  }
  return result;
}

} // namespace

struct LlamadChatClient::Impl {
  struct Event {
    std::uint64_t request{};
    std::variant<std::string, Terminal> payload{};
  };

  std::string socket_path{};
  core::ModelInfo info{};
  std::shared_ptr<grpc::Channel> channel{};
  std::unique_ptr<wire::Llama::Stub> stub{};

  // Shared with the worker thread.
  std::mutex mutex{};
  std::deque<Event> events{};
  std::shared_ptr<grpc::ClientContext> context{};
  bool cancel_requested{};

  // Pump-thread state.
  std::uint64_t request{};
  bool active{};
  core::ChatCallbacks callbacks{};
  std::jthread worker{};

  void push(const std::uint64_t id, std::variant<std::string, Terminal> event) {
    const std::scoped_lock lock{mutex};
    events.push_back({.request = id, .payload = std::move(event)});
  }

  /// @brief The body of one worker thread: read the stream, queue events.
  void read_stream(const std::uint64_t id, grpc::ClientContext &call_context,
                   const wire::ChatRequest &request_message) {
    auto reader = stub->Chat(&call_context, request_message);
    wire::GenerateChunk chunk;
    std::optional<Terminal> finish;
    while (reader->Read(&chunk)) {
      if (chunk.has_text()) {
        push(id, chunk.text());
      } else if (chunk.has_finish() && !finish) {
        finish = from_wire(chunk.finish());
      }
    }
    const auto status = reader->Finish();
    Terminal terminal;
    if (status.ok() && finish) {
      terminal = std::move(*finish);
    } else if (status.ok()) {
      terminal = std::unexpected(core::ChatError{
          .message = "llamad ended the stream without a finish chunk"});
    } else {
      bool cancelled{};
      {
        const std::scoped_lock lock{mutex};
        cancelled = cancel_requested &&
                    status.error_code() == grpc::StatusCode::CANCELLED;
      }
      terminal = std::unexpected(core::ChatError{
          .cancelled = cancelled,
          .message = cancelled ? "the model request was cancelled"
                               : describe(status)});
    }
    // Queued last, after Finish() returned, so the next start() only waits
    // for this thread to exit.
    push(id, std::move(terminal));
  }

  void cancel_active() noexcept {
    const std::scoped_lock lock{mutex};
    if (context) {
      cancel_requested = true;
      context->TryCancel();
    }
  }
};

std::expected<std::unique_ptr<LlamadChatClient>, std::string>
LlamadChatClient::connect(std::string socket_path,
                          const std::chrono::milliseconds deadline) {
  if (socket_path.empty()) {
    return std::unexpected("the llamad socket path is empty");
  }
  auto channel = grpc::CreateChannel("unix:" + socket_path,
                                     grpc::InsecureChannelCredentials());
  auto stub = wire::Llama::NewStub(channel);

  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + deadline);
  wire::ModelInfo info;
  if (const auto status = stub->GetModelInfo(&context, {}, &info);
      !status.ok()) {
    if (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
        status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
      return std::unexpected("could not reach llamad at " + socket_path + " (" +
                             describe(status) +
                             "). Start the daemon with: llamad --model " +
                             "MODEL.gguf --socket " + socket_path);
    }
    return std::unexpected("llamad at " + socket_path +
                           " did not report its model (" + describe(status) +
                           ")");
  }
  if (info.serves_embeddings()) {
    return std::unexpected("llamad at " + socket_path +
                           " serves an embedding model (" + info.description() +
                           "); Pig Pen needs a chat model");
  }
  if (!info.has_chat_template()) {
    return std::unexpected("the model that llamad serves at " + socket_path +
                           " (" + info.description() +
                           ") has no chat template; Pig Pen needs one");
  }

  auto impl = std::make_unique<Impl>();
  impl->socket_path = std::move(socket_path);
  impl->info = {.description = info.description(),
                .n_params = info.n_params(),
                .size_bytes = info.size_bytes(),
                .n_ctx = info.n_ctx(),
                .n_ctx_train = info.n_ctx_train()};
  impl->channel = std::move(channel);
  impl->stub = std::move(stub);
  return std::unique_ptr<LlamadChatClient>{
      new LlamadChatClient{std::move(impl)}};
}

LlamadChatClient::LlamadChatClient(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

LlamadChatClient::~LlamadChatClient() {
  impl_->cancel_active();
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  const std::scoped_lock lock{impl_->mutex};
  impl_->events.clear();
}

const core::ModelInfo &LlamadChatClient::model_info() const noexcept {
  return impl_->info;
}

const std::string &LlamadChatClient::socket_path() const noexcept {
  return impl_->socket_path;
}

std::expected<void, std::string>
LlamadChatClient::start(core::ChatRequest request,
                        core::ChatCallbacks callbacks) {
  auto &impl = *impl_;
  if (impl.active) {
    return std::unexpected("a llamad request is already active");
  }
  if (request.messages.empty()) {
    return std::unexpected("a chat request needs at least one message");
  }
  if (impl.worker.joinable()) {
    impl.worker.join();
  }
  auto context = std::make_shared<grpc::ClientContext>();
  {
    const std::scoped_lock lock{impl.mutex};
    impl.context = context;
    impl.cancel_requested = false;
  }
  const auto id = ++impl.request;
  impl.active = true;
  impl.callbacks = std::move(callbacks);
  impl.worker = std::jthread{
      [&impl, id, context = std::move(context), message = to_wire(request)] {
        impl.read_stream(id, *context, message);
      }};
  return {};
}

bool LlamadChatClient::cancel() noexcept {
  if (!impl_->active) {
    return false;
  }
  impl_->cancel_active();
  return true;
}

DeliveryStats LlamadChatClient::pump(const std::size_t max_callbacks) {
  auto &impl = *impl_;
  DeliveryStats stats;
  while (stats.callbacks_delivered < max_callbacks) {
    Impl::Event event;
    {
      const std::scoped_lock lock{impl.mutex};
      if (impl.events.empty()) {
        break;
      }
      event = std::move(impl.events.front());
      impl.events.pop_front();
      if (std::holds_alternative<Terminal>(event.payload)) {
        impl.context.reset();
      }
    }
    if (!impl.active || event.request != impl.request) {
      continue;
    }
    if (auto *text = std::get_if<std::string>(&event.payload)) {
      if (impl.callbacks.on_text) {
        impl.callbacks.on_text(*text);
      }
    } else {
      // Clear the request before the callback, which can start the next one.
      auto callbacks = std::move(impl.callbacks);
      impl.callbacks = {};
      impl.active = false;
      if (callbacks.on_finished) {
        callbacks.on_finished(std::move(std::get<Terminal>(event.payload)));
      }
    }
    ++stats.callbacks_delivered;
  }
  {
    const std::scoped_lock lock{impl.mutex};
    stats.events_remaining = impl.events.size();
  }
  return stats;
}

} // namespace pigpen::llamad
