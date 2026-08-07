#pragma once

#include "agent/config.hpp"
#include "agent/episode_runner.hpp"
#include "agent/events.hpp"
#include "world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace pigpen::agent {

struct PumpStats {
  std::size_t callbacks_delivered{};
  std::size_t events_remaining{};
};

/// One complete episode runtime. Replacing this object atomically resets the
/// world, conversation, additive tool registry, callbacks, and log.
class Session final : public std::enable_shared_from_this<Session> {
public:
  [[nodiscard]] static std::expected<std::shared_ptr<Session>, std::string>
  create(Config config, std::filesystem::path log_directory = "logs",
         std::string prompt_variant = "default");

  ~Session();

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  [[nodiscard]] PumpStats pump();
  [[nodiscard]] bool play();
  [[nodiscard]] bool pause();
  [[nodiscard]] bool stop();
  [[nodiscard]] std::uint64_t queue_user_input(std::string message);
  [[nodiscard]] bool remove_pending_user_input(std::uint64_t id);
  void clear_pending_user_inputs();

  [[nodiscard]] const Config &config() const noexcept;
  [[nodiscard]] const world::World &world() const noexcept;
  [[nodiscard]] const EventFeed &events() const noexcept;
  [[nodiscard]] const EpisodeRunner &runner() const noexcept;
  [[nodiscard]] EpisodeRunner &runner() noexcept;
  [[nodiscard]] std::size_t tool_call_count() const noexcept;
  [[nodiscard]] const std::filesystem::path &metrics_path() const noexcept;
  [[nodiscard]] const std::string &metrics_error() const noexcept;

private:
  class Impl;

  explicit Session(std::unique_ptr<Impl> impl);
  [[nodiscard]] std::expected<void, std::string> register_tools();

  std::unique_ptr<Impl> impl_;
};

} // namespace pigpen::agent
