/// @file fake_llamad.cpp
/// @brief FakeLlamad implementation over a real gRPC server.
#include "support/fake_llamad.hpp"

#include <grpcpp/grpcpp.h>
#include <llamad/v1/llamad.grpc.pb.h>

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <stdlib.h>
#include <system_error>
#include <thread>
#include <utility>

namespace pigpen::testing {
namespace {

namespace wire = ::llamad::v1;

[[nodiscard]] wire::FinishReason to_wire(const FakeFinishReason reason) {
  switch (reason) {
  case FakeFinishReason::eog:
    return wire::FINISH_REASON_EOG;
  case FakeFinishReason::length:
    return wire::FINISH_REASON_LENGTH;
  case FakeFinishReason::stop:
    return wire::FINISH_REASON_STOP;
  case FakeFinishReason::cancelled:
    return wire::FINISH_REASON_CANCELLED;
  case FakeFinishReason::tool_calls:
    return wire::FINISH_REASON_TOOL_CALLS;
  case FakeFinishReason::unspecified:
    break;
  }
  return wire::FINISH_REASON_UNSPECIFIED;
}

[[nodiscard]] std::vector<FakeToolCall>
from_wire(const google::protobuf::RepeatedPtrField<wire::ToolCall> &calls) {
  std::vector<FakeToolCall> result;
  for (const auto &call : calls) {
    result.push_back({.id = call.id(),
                      .name = call.name(),
                      .arguments_json = call.arguments_json()});
  }
  return result;
}

[[nodiscard]] ReceivedRequest from_wire(const wire::ChatRequest &request) {
  ReceivedRequest result;
  for (const auto &message : request.messages()) {
    result.messages.push_back({.role = message.role(),
                               .content = message.content(),
                               .tool_calls = from_wire(message.tool_calls()),
                               .tool_call_id = message.tool_call_id()});
  }
  for (const auto &tool : request.tools()) {
    result.tools.push_back(
        {.name = tool.name(),
         .description = tool.description(),
         .parameters_json_schema = tool.parameters_json_schema()});
  }
  const auto &sampling = request.sampling();
  if (sampling.has_temperature()) {
    result.temperature = sampling.temperature();
  }
  if (sampling.has_seed()) {
    result.seed = sampling.seed();
  }
  if (sampling.has_max_tokens()) {
    result.max_tokens = sampling.max_tokens();
  }
  if (sampling.has_top_k()) {
    result.top_k = sampling.top_k();
  }
  result.response_json_schema = request.response_json_schema();
  return result;
}

class Service final : public wire::Llama::Service {
public:
  Service(FakeModelInfo info,
          std::function<void(const ReceivedRequest &)> on_request)
      : info_(std::move(info)), on_request_(std::move(on_request)) {}

  grpc::Status GetModelInfo(grpc::ServerContext *,
                            const wire::GetModelInfoRequest *,
                            wire::ModelInfo *info) override {
    info->set_description(info_.description);
    info->set_n_params(info_.n_params);
    info->set_size_bytes(info_.size_bytes);
    info->set_n_ctx(info_.n_ctx);
    info->set_n_ctx_train(info_.n_ctx_train);
    info->set_has_chat_template(info_.has_chat_template);
    info->set_serves_embeddings(info_.serves_embeddings);
    info->set_n_embd(info_.serves_embeddings ? 384U : 0U);
    return grpc::Status::OK;
  }

  grpc::Status Chat(grpc::ServerContext *context,
                    const wire::ChatRequest *request,
                    grpc::ServerWriter<wire::GenerateChunk> *writer) override {
    auto received = from_wire(*request);
    if (on_request_) {
      on_request_(received);
    }
    std::optional<FakeReply> reply;
    {
      const std::scoped_lock lock{mutex_};
      requests_.push_back(std::move(received));
      if (!replies_.empty()) {
        reply = std::move(replies_.front());
        replies_.pop_front();
      }
      ++active_;
    }
    changed_.notify_all();
    const auto status = respond(*context, reply, *writer);
    {
      const std::scoped_lock lock{mutex_};
      --active_;
    }
    changed_.notify_all();
    return status;
  }

  void enqueue(FakeReply reply) {
    const std::scoped_lock lock{mutex_};
    replies_.push_back(std::move(reply));
  }

  [[nodiscard]] std::vector<ReceivedRequest> requests() const {
    const std::scoped_lock lock{mutex_};
    return requests_;
  }

  [[nodiscard]] bool
  wait_for_requests(const std::size_t count,
                    const std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [&] { return requests_.size() >= count; });
  }

  [[nodiscard]] bool wait_until_idle(const std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [&] { return active_ == 0; });
  }

private:
  static grpc::Status respond(grpc::ServerContext &context,
                              const std::optional<FakeReply> &reply,
                              grpc::ServerWriter<wire::GenerateChunk> &writer) {
    if (!reply) {
      return {grpc::StatusCode::INTERNAL, "no scripted reply"};
    }
    for (const auto &text : reply->text) {
      wire::GenerateChunk chunk;
      chunk.set_text(text);
      writer.Write(chunk);
    }
    if (reply->hang) {
      while (!context.IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
      return {grpc::StatusCode::CANCELLED, "cancelled by the client"};
    }
    if (reply->status_code != 0) {
      return {static_cast<grpc::StatusCode>(reply->status_code),
              reply->status_message};
    }
    if (reply->finish) {
      wire::GenerateChunk chunk;
      auto &finish = *chunk.mutable_finish();
      finish.set_reason(to_wire(*reply->finish));
      finish.mutable_stats()->set_prompt_tokens(reply->prompt_tokens);
      finish.mutable_stats()->set_completion_tokens(reply->completion_tokens);
      finish.mutable_stats()->set_cached_prompt_tokens(
          reply->cached_prompt_tokens);
      for (const auto &call : reply->tool_calls) {
        auto &sent = *finish.add_tool_calls();
        sent.set_id(call.id);
        sent.set_name(call.name);
        sent.set_arguments_json(call.arguments_json);
      }
      writer.Write(chunk);
    }
    return grpc::Status::OK;
  }

  FakeModelInfo info_;
  std::function<void(const ReceivedRequest &)> on_request_;
  mutable std::mutex mutex_{};
  std::condition_variable changed_{};
  std::deque<FakeReply> replies_{};
  std::vector<ReceivedRequest> requests_{};
  std::size_t active_{};
};

} // namespace

TempSocketDirectory::TempSocketDirectory() {
  std::string pattern{"/tmp/pigpen-XXXXXX"};
  if (::mkdtemp(pattern.data()) == nullptr) {
    throw std::system_error{errno, std::generic_category(), "mkdtemp"};
  }
  path_ = pattern;
}

TempSocketDirectory::~TempSocketDirectory() {
  std::error_code ignored;
  std::filesystem::remove_all(path_, ignored);
}

std::string TempSocketDirectory::socket() const {
  return (path_ / "s.sock").string();
}

FakeReply FakeReply::calls(std::vector<FakeToolCall> tool_calls,
                           std::vector<std::string> text) {
  return {.text = std::move(text),
          .finish = FakeFinishReason::tool_calls,
          .tool_calls = std::move(tool_calls)};
}

FakeReply FakeReply::say(std::string text) {
  return {.text = {std::move(text)}};
}

struct FakeLlamad::Impl {
  Impl(FakeModelInfo info,
       std::function<void(const ReceivedRequest &)> on_request)
      : service(std::move(info), std::move(on_request)) {}

  Service service;
  std::unique_ptr<grpc::Server> server{};
};

std::unique_ptr<FakeLlamad>
FakeLlamad::start(const std::string &socket_path, FakeModelInfo info,
                  std::function<void(const ReceivedRequest &)> on_request) {
  auto impl = std::make_unique<Impl>(std::move(info), std::move(on_request));
  grpc::ServerBuilder builder;
  builder.AddListeningPort("unix:" + socket_path,
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&impl->service);
  impl->server = builder.BuildAndStart();
  if (!impl->server) {
    return nullptr;
  }
  return std::unique_ptr<FakeLlamad>{new FakeLlamad{std::move(impl)}};
}

FakeLlamad::FakeLlamad(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FakeLlamad::~FakeLlamad() {
  impl_->server->Shutdown(std::chrono::system_clock::now() +
                          std::chrono::milliseconds{200});
  impl_->server->Wait();
}

void FakeLlamad::enqueue(FakeReply reply) {
  impl_->service.enqueue(std::move(reply));
}

std::vector<ReceivedRequest> FakeLlamad::requests() const {
  return impl_->service.requests();
}

bool FakeLlamad::wait_for_requests(
    const std::size_t count, const std::chrono::milliseconds timeout) const {
  return impl_->service.wait_for_requests(count, timeout);
}

bool FakeLlamad::wait_until_idle(
    const std::chrono::milliseconds timeout) const {
  return impl_->service.wait_until_idle(timeout);
}

} // namespace pigpen::testing
