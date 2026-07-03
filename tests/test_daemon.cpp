#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "app_metadata.hpp"
#include "daemon_backend.hpp"
#include "daemon_controller.hpp"
#include "daemon_ipc.hpp"
#include "logging.hpp"

namespace {

int g_failures = 0;
constexpr size_t kMaxTestPayloadBytes = 64 * 1024;

struct TempDir {
  std::filesystem::path path;
  ~TempDir() {
    if (!path.empty()) {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  }
  auto is_valid() const -> bool { return !path.empty(); }
};

struct ScopedEnv {
  std::string name;
  std::optional<std::string> previous;

  ScopedEnv(std::string env_name, const std::string& value) : name(std::move(env_name)) {
    if (const char* current = std::getenv(name.c_str()); current != nullptr) {
      previous = current;
    }
    setenv(name.c_str(), value.c_str(), 1);
  }

  ~ScopedEnv() {
    if (previous.has_value()) {
      setenv(name.c_str(), previous->c_str(), 1);
    } else {
      unsetenv(name.c_str());
    }
  }
};

void expect(bool condition, std::string_view message) {
  if (!condition) {
    ee::log::error(std::string("FAIL: ") + std::string(message));
    ++g_failures;
  }
}

auto fixture_path(std::string_view file_name) -> std::string {
  return std::string(EQ_CLI_TEST_FIXTURE_DIR) + "/" + std::string(file_name);
}

auto make_temp_dir() -> TempDir {
  std::array<char, 64> pattern{};
  std::snprintf(pattern.data(), pattern.size(), "/tmp/eq-cli-daemon-XXXXXX");
  if (char* created = mkdtemp(pattern.data()); created != nullptr) {
    return TempDir{.path = created};
  }
  return {};
}

struct FakeBackendState {
  std::mutex mutex;
  bool session_active = false;
  bool fail_next_start = false;
  int fail_start_count = 0;
  std::string fail_message = "forced runtime failure";
  std::string preset_origin;
  std::string sink_name = "fake_sink";
  uint64_t sink_serial = 7;
  std::vector<std::string> active_plugins = {"equalizer"};
  std::vector<std::string> sinks = {"fake_sink [serial 7]"};
  bool bypass = false;
  float volume = 1.0F;
};

class FakeBackend final : public ee::SessionBackend {
 public:
  explicit FakeBackend(std::shared_ptr<FakeBackendState> state) : state_(std::move(state)) {}

  auto start_session([[maybe_unused]] const ee::ParsedPreset& preset,
                     std::string preset_origin,
                     [[maybe_unused]] std::string sink_selector,
                     std::string& error) -> bool override {
    std::scoped_lock lock(state_->mutex);
    if (state_->fail_start_count > 0) {
      --state_->fail_start_count;
      error = state_->fail_message;
      return false;
    }
    if (state_->fail_next_start) {
      state_->fail_next_start = false;
      error = state_->fail_message;
      return false;
    }
    state_->session_active = true;
    state_->preset_origin = std::move(preset_origin);
    return true;
  }

  void stop_session() override {
    std::scoped_lock lock(state_->mutex);
    state_->session_active = false;
  }

  void set_bypass(bool bypass) override {
    std::scoped_lock lock(state_->mutex);
    state_->bypass = bypass;
  }

  void set_volume(float volume) override {
    std::scoped_lock lock(state_->mutex);
    state_->volume = volume;
  }

  auto list_sinks(std::string& error) -> std::vector<std::string> override {
    std::scoped_lock lock(state_->mutex);
    error.clear();
    return state_->sinks;
  }

  auto snapshot() const -> ee::RuntimeSnapshot override {
    std::scoped_lock lock(state_->mutex);
    ee::RuntimeSnapshot snapshot;
    snapshot.session_active = state_->session_active;
    snapshot.preset_origin = state_->preset_origin;
    snapshot.sink_name = state_->session_active ? state_->sink_name : "";
    snapshot.sink_serial = state_->session_active ? state_->sink_serial : 0;
    snapshot.active_plugins = state_->session_active ? state_->active_plugins : std::vector<std::string>{};
    snapshot.bypass = state_->bypass;
    snapshot.volume = state_->volume;
    return snapshot;
  }

 private:
  std::shared_ptr<FakeBackendState> state_;
};

auto wait_for_socket(const std::filesystem::path& path) -> bool {
  for (int i = 0; i < 200; ++i) {
    if (std::filesystem::exists(path)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

auto wait_for_status_ready(std::string& last_error) -> bool {
  for (int i = 0; i < 200; ++i) {
    ee::DaemonResponse response;
    std::string error;
    if (ee::send_daemon_request(ee::DaemonRequest{.command = "status"}, response, error) && response.ok) {
      return true;
    }
    last_error = !error.empty() ? error : response.error;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

auto make_test_socket_path(const std::filesystem::path& runtime_dir) -> std::filesystem::path {
  return runtime_dir / "eq-cli" / "daemon.sock";
}

void run_test_server_at_path(ee::DaemonController& controller, const std::filesystem::path& socket_path, std::string& error) {
  std::filesystem::create_directories(socket_path.parent_path());

  const int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd == -1) {
    error = "failed to create test daemon socket";
    return;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
  if (bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    error = "failed to bind test daemon socket";
    close(listen_fd);
    return;
  }
  if (listen(listen_fd, 8) != 0) {
    error = "failed to listen on test daemon socket";
    close(listen_fd);
    return;
  }
  ee::log::info(std::format("daemon-test server listening on {}", socket_path.string()));

  bool done = false;
  controller.set_shutdown_callback([&done]() { done = true; });

  while (!done) {
    const int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client_fd == -1) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    std::string payload;
    std::array<char, 4096> buffer{};
    while (true) {
      const auto bytes = read(client_fd, buffer.data(), buffer.size());
      if (bytes == 0) {
        break;
      }
      if (bytes < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      payload.append(buffer.data(), static_cast<size_t>(bytes));
      if (payload.size() > kMaxTestPayloadBytes) {
        break;
      }
    }

    ee::DaemonResponse response;
    try {
      const auto request = nlohmann::json::parse(payload).get<ee::DaemonRequest>();
      response = controller.handle_request(request);
    } catch (const std::exception& ex) {
      response.ok = false;
      response.error = ex.what();
    }

    const auto encoded = nlohmann::json(response).dump();
    const char* data = encoded.data();
    size_t remaining = encoded.size();
    while (remaining > 0) {
      const auto bytes = send(client_fd, data, remaining, MSG_NOSIGNAL);
      if (bytes < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      data += bytes;
      remaining -= static_cast<size_t>(bytes);
    }
    close(client_fd);
  }

  close(listen_fd);
  std::filesystem::remove(socket_path);
}

void run_test_server(ee::DaemonController& controller, const std::filesystem::path& runtime_dir, std::string& error) {
  run_test_server_at_path(controller, make_test_socket_path(runtime_dir), error);
}

struct DaemonHarness {
  TempDir temp_dir;
  std::unique_ptr<ScopedEnv> runtime_dir_env;
  std::shared_ptr<FakeBackendState> state;
  std::thread thread;
  std::shared_ptr<std::string> server_error = std::make_shared<std::string>();

  DaemonHarness() = default;
  DaemonHarness(DaemonHarness&&) noexcept = default;
  auto operator=(DaemonHarness&&) noexcept -> DaemonHarness& = default;
  DaemonHarness(const DaemonHarness&) = delete;
  auto operator=(const DaemonHarness&) -> DaemonHarness& = delete;

  ~DaemonHarness() {
    if (thread.joinable()) {
      ee::DaemonResponse ignored;
      std::string error;
      if (ee::send_daemon_request(ee::DaemonRequest{.command = "shutdown"}, ignored, error)) {
        thread.join();
      } else {
        thread.detach();
      }
    }
  }
};

auto start_daemon_harness() -> std::unique_ptr<DaemonHarness> {
  auto harness = std::make_unique<DaemonHarness>();
  harness->temp_dir = make_temp_dir();
  harness->state = std::make_shared<FakeBackendState>();
  harness->runtime_dir_env = std::make_unique<ScopedEnv>("XDG_RUNTIME_DIR", harness->temp_dir.path.string());

  harness->thread = std::thread([state = harness->state,
                                 path = harness->temp_dir.path.string(),
                                 error = harness->server_error]() {
    ee::DaemonController controller(std::make_unique<FakeBackend>(state), ee::kApplicationVersion, 1234, "2026-04-16T00:00:00Z");
    error->clear();
    run_test_server(controller, path, *error);
  });

  const auto socket_path = make_test_socket_path(harness->temp_dir.path);
  if (!wait_for_socket(socket_path)) {
    ee::log::error("FAIL: daemon socket did not appear");
    ++g_failures;
  } else {
    std::string ready_error;
    if (!wait_for_status_ready(ready_error)) {
      ee::log::error(std::string("FAIL: daemon did not become ready: ") + ready_error);
      ++g_failures;
    }
  }
  return harness;
}

void test_daemon_status_idle() {
  ee::log::info("daemon-test: status_idle");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;
  const auto status_ok = ee::send_daemon_request(ee::DaemonRequest{.command = "status"}, response, error);
  if (!status_ok) {
    ee::log::error(std::format("daemon-test status error: {} | server_error={}", error, *harness->server_error));
  } else if (!response.ok) {
    ee::log::error(std::format("daemon-test status response error: {} | server_error={}",
                               response.error,
                               *harness->server_error));
  }
  expect(status_ok, "status request should succeed");
  expect(response.ok, "status response should be ok");
  expect(response.status.daemon_state == ee::DaemonProcessState::Ready, "daemon should be ready");
  expect(response.status.session_state == ee::SessionLifecycleState::Disabled, "session should start disabled");
  expect(response.status.health == ee::HealthState::Ok, "idle daemon health should be ok");
}

void test_apply_disable_enable_cycle() {
  ee::log::info("daemon-test: apply_disable_enable_cycle");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  const auto apply_ok = ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response,
             error);
  if (!apply_ok) {
    ee::log::error(std::format("daemon-test apply error: {} | server_error={}", error, *harness->server_error));
  } else if (!response.ok) {
    ee::log::error(std::format("daemon-test apply response error: {} | server_error={}",
                               response.error,
                               *harness->server_error));
  }
  expect(apply_ok, "apply request should succeed");
  expect(response.ok, "apply response should be ok");
  expect(response.status.session_state == ee::SessionLifecycleState::Enabled, "apply should enable session");

  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "disable"}, response, error), "disable request should succeed");
  expect(response.ok, "disable response should be ok");
  expect(response.status.session_state == ee::SessionLifecycleState::Disabled, "disable should stop session");

  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "enable"}, response, error), "enable request should succeed");
  expect(response.ok, "enable response should be ok");
  expect(response.status.session_state == ee::SessionLifecycleState::Enabled, "enable should reuse desired config");
}

void test_runtime_apply_failure_rolls_back() {
  ee::log::info("daemon-test: runtime_apply_failure_rolls_back");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response,
             error),
         "initial apply should succeed");
  expect(response.ok, "initial apply response should be ok");

  {
    std::scoped_lock lock(harness->state->mutex);
    harness->state->fail_next_start = true;
    harness->state->fail_message = "forced runtime failure";
  }

  expect(ee::send_daemon_request(
             ee::DaemonRequest{
                 .command = "apply",
                 .preset_path = fixture_path("Bass Enhancing + Perfect EQ - Low Latency.json"),
             },
             response,
             error),
         "runtime failure apply request should return a response");
  expect(!response.ok, "runtime failure apply should fail");
  expect(response.status.health == ee::HealthState::Degraded, "rollback success should leave health degraded");
  expect(response.status.session_state == ee::SessionLifecycleState::Enabled, "rollback success should preserve enabled session");
  expect(response.status.effective.preset_origin.ends_with("Boosted.json"),
         "rollback success should preserve previous effective preset");
}

void test_status_refreshes_effective_sink() {
  ee::log::info("daemon-test: status_refreshes_effective_sink");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response,
             error),
         "apply should succeed before sink refresh check");
  expect(response.ok, "apply response should be ok before sink refresh check");

  {
    std::scoped_lock lock(harness->state->mutex);
    harness->state->sink_name = "moved_sink";
    harness->state->sink_serial = 42;
  }

  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "status"}, response, error),
         "status request should succeed after sink change");
  expect(response.ok, "status response should be ok after sink change");
  expect(response.status.effective.sink_name == "moved_sink",
         "status should refresh effective sink name from backend snapshot");
  expect(response.status.effective.sink_serial == 42,
         "status should refresh effective sink serial from backend snapshot");
}

void test_daemon_single_instance_refusal() {
  ee::log::info("daemon-test: single_instance_refusal");
  auto harness = start_daemon_harness();
  setenv("XDG_RUNTIME_DIR", harness->temp_dir.path.c_str(), 1);
  auto backend = std::make_unique<FakeBackend>(harness->state);
  ee::DaemonController controller(std::move(backend), ee::kApplicationVersion, 9999, "2026-04-16T00:00:00Z");
  std::string error;
  expect(ee::run_daemon_ipc_server(controller, error) == EXIT_FAILURE, "second daemon start should fail");
  expect(error == "daemon already running", "second daemon start should report already running");
}

void test_stale_socket_recovery() {
  ee::log::info("daemon-test: stale_socket_recovery");
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temporary runtime dir should be created");
  if (!temp_dir.is_valid()) {
    return;
  }
  auto runtime_dir_env = ScopedEnv("XDG_RUNTIME_DIR", temp_dir.path.string());
  std::filesystem::create_directories(temp_dir.path / "eq-cli");
  const auto socket_path = temp_dir.path / "eq-cli" / "daemon.sock";

  const int stale_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  expect(stale_fd != -1, "stale socket fd should be created");
  if (stale_fd == -1) {
    return;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
  expect(bind(stale_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "stale socket should bind");
  close(stale_fd);

  auto state = std::make_shared<FakeBackendState>();
  std::string server_error;
  std::thread server([&]() {
    ee::DaemonController controller(std::make_unique<FakeBackend>(state), ee::kApplicationVersion, 1234, "2026-04-16T00:00:00Z");
    ee::run_daemon_ipc_server(controller, server_error, false);
  });

  expect(wait_for_socket(socket_path), "daemon should recover stale socket and bind");
  std::string ready_error;
  expect(wait_for_status_ready(ready_error), "daemon should answer requests after stale-socket recovery");
  ee::DaemonResponse ignored;
  std::string error;
  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "shutdown"}, ignored, error),
         "shutdown should succeed after stale-socket recovery");
  server.join();
}

void test_stalled_client_does_not_block_daemon() {
  ee::log::info("daemon-test: stalled_client_timeout");
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temporary runtime dir should be created");
  if (!temp_dir.is_valid()) {
    return;
  }
  auto runtime_dir_env = ScopedEnv("XDG_RUNTIME_DIR", temp_dir.path.string());

  auto state = std::make_shared<FakeBackendState>();
  std::string server_error;
  std::thread server([&]() {
    ee::DaemonController controller(std::make_unique<FakeBackend>(state), ee::kApplicationVersion, 1234, "2026-04-16T00:00:00Z");
    ee::run_daemon_ipc_server(controller, server_error, false, std::chrono::milliseconds(200));
  });

  const auto socket_path = temp_dir.path / "eq-cli" / "daemon.sock";
  expect(wait_for_socket(socket_path), "daemon socket should appear");
  std::string ready_error;
  expect(wait_for_status_ready(ready_error), "daemon should become ready");

  // A client that connects but never sends its request.
  const int stalled_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  expect(stalled_fd != -1, "stalled client socket should be created");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
  expect(connect(stalled_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
         "stalled client should connect");

  ee::DaemonResponse response;
  std::string error;
  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "status"}, response, error) && response.ok,
         "status should succeed while another client stalls");

  // The stalled client must have been answered with a timeout error and closed.
  std::string stalled_payload;
  std::array<char, 4096> buffer{};
  while (true) {
    const auto bytes = read(stalled_fd, buffer.data(), buffer.size());
    if (bytes <= 0) {
      break;
    }
    stalled_payload.append(buffer.data(), static_cast<size_t>(bytes));
  }
  close(stalled_fd);
  expect(stalled_payload.find("timed out") != std::string::npos,
         "stalled client should receive a timeout error response");

  ee::DaemonResponse ignored;
  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "shutdown"}, ignored, error),
         "shutdown should succeed after a stalled client");
  server.join();
}

void test_switch_sink() {
  ee::log::info("daemon-test: switch_sink");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response,
             error),
         "apply should succeed before switch-sink");
  expect(response.ok, "apply response should be ok before switch-sink");
  expect(response.status.effective.sink_name == "fake_sink", "initial sink should be fake_sink");

  {
    std::scoped_lock lock(harness->state->mutex);
    harness->state->sink_name = "other_sink";
    harness->state->sink_serial = 99;
  }

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "switch-sink", .sink_selector = "other_sink"},
             response,
             error),
         "switch-sink request should succeed");
  expect(response.ok, "switch-sink response should be ok");
  expect(response.status.effective.sink_name == "other_sink",
         "switch-sink should update effective sink name");
  expect(response.status.effective.sink_serial == 99,
         "switch-sink should update effective sink serial");
  expect(response.status.desired.sink_selector == "other_sink",
         "switch-sink should update desired sink selector");
}

void test_bypass_on_off() {
  ee::log::info("daemon-test: bypass_on_off");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response,
             error),
         "apply should succeed before bypass test");
  expect(response.ok, "apply response should be ok before bypass test");

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "bypass", .sink_selector = "on"},
             response,
             error),
         "bypass on request should succeed");
  expect(response.ok, "bypass on response should be ok");
  expect(response.status.desired.bypass, "bypass on should set desired bypass true");
  expect(response.status.effective.bypass, "bypass on should set effective bypass true");

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "bypass", .sink_selector = "off"},
             response,
             error),
         "bypass off request should succeed");
  expect(response.ok, "bypass off response should be ok");
  expect(!response.status.desired.bypass, "bypass off should set desired bypass false");
  expect(!response.status.effective.bypass, "bypass off should set effective bypass false");
}

void test_bypass_no_session() {
  ee::log::info("daemon-test: bypass_no_session");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "bypass", .sink_selector = "on"},
             response,
             error),
         "bypass request should return a response");
  expect(!response.ok, "bypass without active session should fail");
}

void test_bypass_rejects_invalid_value() {
  ee::log::info("daemon-test: bypass_rejects_invalid_value");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response,
             error),
         "apply should succeed");
  expect(response.ok, "apply response should be ok");

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "bypass", .sink_selector = "foo"},
             response,
             error),
         "bypass foo request should return a response");
  expect(!response.ok, "bypass foo should fail");
}

void test_bypass_survives_disable_enable() {
  ee::log::info("daemon-test: bypass_survives_disable_enable");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response,
             error),
         "apply should succeed");
  expect(response.ok, "apply response should be ok");

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "bypass", .sink_selector = "on"},
             response,
             error),
         "bypass on should succeed");
  expect(response.ok, "bypass on response should be ok");
  expect(response.status.desired.bypass, "bypass should be on");

  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "disable"}, response, error),
         "disable should succeed");
  expect(response.ok, "disable response should be ok");

  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "enable"}, response, error),
         "enable should succeed");
  expect(response.ok, "enable response should be ok");
  expect(response.status.desired.bypass, "bypass should survive disable/enable cycle");
}

void test_bypass_survives_switch_sink() {
  ee::log::info("daemon-test: bypass_survives_switch_sink");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response,
             error),
         "apply should succeed");
  expect(response.ok, "apply response should be ok");

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "bypass", .sink_selector = "on"},
             response,
             error),
         "bypass on should succeed");
  expect(response.ok, "bypass on response should be ok");

  {
    std::scoped_lock lock(harness->state->mutex);
    harness->state->sink_name = "other_sink";
    harness->state->sink_serial = 99;
  }

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "switch-sink", .sink_selector = "other_sink"},
             response,
             error),
         "switch-sink should succeed");
  expect(response.ok, "switch-sink response should be ok");
  expect(response.status.desired.bypass, "bypass should survive switch-sink");
}

void test_switch_sink_while_disabled() {
  ee::log::info("daemon-test: switch_sink_while_disabled");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response,
             error),
         "apply should succeed");
  expect(response.ok, "apply response should be ok");

  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "disable"}, response, error),
         "disable should succeed");
  expect(response.ok, "disable response should be ok");
  expect(response.status.session_state == ee::SessionLifecycleState::Disabled, "session should be disabled");

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "switch-sink", .sink_selector = "other_sink"},
             response,
             error),
         "switch-sink while disabled should succeed");
  expect(response.ok, "switch-sink while disabled response should be ok");
  expect(response.status.desired.sink_selector == "other_sink",
         "switch-sink should update desired sink even while disabled");
  expect(response.status.session_state == ee::SessionLifecycleState::Disabled,
         "switch-sink should not re-enable a disabled session");
  expect(!response.status.effective.session_active,
         "switch-sink should not start a session while disabled");
}

void test_switch_sink_no_preset() {
  ee::log::info("daemon-test: switch_sink_no_preset");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "switch-sink", .sink_selector = "some_sink"},
             response,
             error),
         "switch-sink request should return a response");
  expect(!response.ok, "switch-sink without prior apply should fail");
}

void test_full_lifecycle() {
  ee::log::info("daemon-test: full_lifecycle");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  // 1. idle status
  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "status"}, response, error),
         "lifecycle: status should succeed when idle");
  expect(response.ok, "lifecycle: idle status should be ok");
  expect(response.status.session_state == ee::SessionLifecycleState::Disabled, "lifecycle: idle session should be disabled");
  expect(!response.status.effective.session_active, "lifecycle: idle should have no active session");

  // 2. apply
  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response, error),
         "lifecycle: apply should succeed");
  expect(response.ok, "lifecycle: apply response should be ok");
  expect(response.status.session_state == ee::SessionLifecycleState::Enabled, "lifecycle: apply should enable session");
  expect(response.status.effective.session_active, "lifecycle: apply should have active session");
  expect(response.status.desired.enabled, "lifecycle: apply should set desired enabled");

  // 3. bypass on
  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "bypass", .sink_selector = "on"},
             response, error),
         "lifecycle: bypass on should succeed");
  expect(response.ok, "lifecycle: bypass on should be ok");
  expect(response.status.desired.bypass, "lifecycle: desired bypass should be true");
  expect(response.status.effective.bypass, "lifecycle: effective bypass should be true");

  // 4. bypass off
  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "bypass", .sink_selector = "off"},
             response, error),
         "lifecycle: bypass off should succeed");
  expect(response.ok, "lifecycle: bypass off should be ok");
  expect(!response.status.desired.bypass, "lifecycle: desired bypass should be false");
  expect(!response.status.effective.bypass, "lifecycle: effective bypass should be false");

  // 5. disable
  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "disable"}, response, error),
         "lifecycle: disable should succeed");
  expect(response.ok, "lifecycle: disable should be ok");
  expect(response.status.session_state == ee::SessionLifecycleState::Disabled, "lifecycle: disable should set disabled");
  expect(!response.status.effective.session_active, "lifecycle: disable should stop session");

  // 6. switch-sink while disabled — should not re-enable
  {
    std::scoped_lock lock(harness->state->mutex);
    harness->state->sink_name = "switched_sink";
    harness->state->sink_serial = 55;
  }
  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "switch-sink", .sink_selector = "switched_sink"},
             response, error),
         "lifecycle: switch-sink while disabled should succeed");
  expect(response.ok, "lifecycle: switch-sink while disabled should be ok");
  expect(response.status.session_state == ee::SessionLifecycleState::Disabled,
         "lifecycle: switch-sink should preserve disabled state");
  expect(response.status.desired.sink_selector == "switched_sink",
         "lifecycle: switch-sink should update desired sink");
  expect(!response.status.effective.session_active,
         "lifecycle: switch-sink should not start session while disabled");

  // 7. enable — should use the new sink
  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "enable"}, response, error),
         "lifecycle: enable should succeed");
  expect(response.ok, "lifecycle: enable should be ok");
  expect(response.status.session_state == ee::SessionLifecycleState::Enabled, "lifecycle: enable should re-enable session");
  expect(response.status.effective.session_active, "lifecycle: enable should have active session");
  expect(response.status.effective.sink_name == "switched_sink",
         "lifecycle: enable should use the sink set during disable");

  // 8. status — verify consistent state
  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "status"}, response, error),
         "lifecycle: final status should succeed");
  expect(response.ok, "lifecycle: final status should be ok");
  expect(response.status.session_state == ee::SessionLifecycleState::Enabled, "lifecycle: final session should be enabled");
  expect(response.status.health == ee::HealthState::Ok, "lifecycle: final health should be ok");

  // 9. current-sink (via status) — verify sink is correct
  expect(response.status.effective.sink_name == "switched_sink", "lifecycle: current sink should be switched_sink");
  expect(response.status.effective.sink_serial == 55, "lifecycle: current sink serial should be 55");
}

void test_invalid_preset_no_replace() {
  ee::log::info("daemon-test: invalid_preset_no_replace");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response, error),
         "initial apply should succeed");
  expect(response.ok, "initial apply response should be ok");
  expect(response.status.session_state == ee::SessionLifecycleState::Enabled, "initial session should be enabled");

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = "/nonexistent/bad.json"},
             response, error),
         "invalid apply should return a response");
  expect(!response.ok, "invalid apply should fail");
  expect(response.status.session_state == ee::SessionLifecycleState::Enabled, "invalid apply should preserve active session");
  expect(response.status.effective.preset_origin.ends_with("Boosted.json"),
         "invalid apply should preserve previous effective preset");
}

void test_apply_rollback_failure() {
  ee::log::info("daemon-test: apply_rollback_failure");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response, error),
         "initial apply should succeed");
  expect(response.ok, "initial apply response should be ok");

  {
    std::scoped_lock lock(harness->state->mutex);
    harness->state->fail_start_count = 2;
    harness->state->fail_message = "both starts fail";
  }

  expect(ee::send_daemon_request(
             ee::DaemonRequest{
                 .command = "apply",
                 .preset_path = fixture_path("Bass Enhancing + Perfect EQ - Low Latency.json"),
             },
             response, error),
         "double-fail apply should return a response");
  expect(!response.ok, "double-fail apply should fail");
  expect(response.status.session_state == ee::SessionLifecycleState::Degraded,
         "rollback failure should leave session degraded");
  expect(response.status.health == ee::HealthState::Degraded,
         "rollback failure should leave health degraded");
  expect(!response.status.last_error.empty(),
         "rollback failure should set last_error");
  expect(!response.status.effective.session_active,
         "rollback failure should have no active session");
}

void test_enable_no_desired_config() {
  ee::log::info("daemon-test: enable_no_desired_config");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(ee::DaemonRequest{.command = "enable"}, response, error),
         "enable without config should return a response");
  expect(!response.ok, "enable without config should fail");
  expect(response.error.find("no preset loaded") != std::string::npos,
         "enable error should mention no preset loaded");
  expect(response.status.daemon_state == ee::DaemonProcessState::Ready,
         "daemon should remain ready after failed enable");
  expect(response.status.session_state == ee::SessionLifecycleState::Disabled,
         "session should remain disabled after failed enable");
}

void test_doctor_fields_populated() {
  ee::log::info("daemon-test: doctor_fields_populated");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response, error),
         "apply should succeed");
  expect(response.ok, "apply response should be ok");

  expect(ee::to_string(response.status.daemon_state) == "ready",
         "to_string(daemon_state) should return ready");
  expect(ee::to_string(response.status.session_state) == "enabled",
         "to_string(session_state) should return enabled");
  expect(ee::to_string(response.status.health) == "ok",
         "to_string(health) should return ok");

  expect(!response.status.effective.preset_origin.empty(),
         "effective preset_origin should be set");
  expect(!response.status.effective.sink_name.empty(),
         "effective sink_name should be set");
  expect(!response.status.version.empty(),
         "version should be set");
  expect(response.status.pid != 0,
         "pid should be set");
}

void test_volume_command() {
  ee::log::info("daemon-test: volume_command");
  auto harness = start_daemon_harness();
  ee::DaemonResponse response;
  std::string error;

  // Volume before apply should fail
  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "volume", .sink_selector = "0.5"},
             response, error),
         "volume send should succeed");
  expect(!response.ok, "volume before apply should fail");
  expect(response.error.find("no active session") != std::string::npos,
         "volume error should mention no active session");

  // Apply preset
  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "apply", .preset_path = fixture_path("Boosted.json")},
             response, error),
         "apply should succeed");
  expect(response.ok, "apply response should be ok");

  // Set volume to 0.5
  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "volume", .sink_selector = "0.5"},
             response, error),
         "volume 0.5 send should succeed");
  expect(response.ok, "volume 0.5 should succeed");
  expect(response.status.desired.volume == 0.5F,
         "desired volume should be 0.5");

  // Out of range
  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "volume", .sink_selector = "2.0"},
             response, error),
         "volume 2.0 send should succeed");
  expect(!response.ok, "volume 2.0 should fail");
  expect(response.error.find("out of range") != std::string::npos,
         "volume error should mention out of range");

  // Invalid parse
  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "volume", .sink_selector = "abc"},
             response, error),
         "volume abc send should succeed");
  expect(!response.ok, "volume abc should fail");
  expect(response.error.find("requires a number") != std::string::npos,
         "volume error should mention requires a number");

  // Volume persists through disable/enable
  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "volume", .sink_selector = "0.7"},
             response, error),
         "volume 0.7 send should succeed");
  expect(response.ok, "volume 0.7 should succeed");

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "disable"},
             response, error),
         "disable should succeed");
  expect(response.ok, "disable should be ok");

  expect(ee::send_daemon_request(
             ee::DaemonRequest{.command = "enable"},
             response, error),
         "enable should succeed");
  expect(response.ok, "enable should be ok");
  expect(response.status.desired.volume == 0.7F,
         "volume should persist after disable/enable");
}

}  // namespace

int main() {
  test_daemon_status_idle();
  test_apply_disable_enable_cycle();
  test_runtime_apply_failure_rolls_back();
  test_status_refreshes_effective_sink();
  test_switch_sink();
  test_switch_sink_while_disabled();
  test_switch_sink_no_preset();
  test_bypass_on_off();
  test_bypass_no_session();
  test_bypass_rejects_invalid_value();
  test_bypass_survives_disable_enable();
  test_bypass_survives_switch_sink();
  test_daemon_single_instance_refusal();
  test_stale_socket_recovery();
  test_stalled_client_does_not_block_daemon();
  test_full_lifecycle();
  test_invalid_preset_no_replace();
  test_apply_rollback_failure();
  test_enable_no_desired_config();
  test_doctor_fields_populated();
  test_volume_command();

  if (g_failures != 0) {
    ee::log::error(std::format("{} daemon test assertion(s) failed", g_failures));
    return 1;
  }

  ee::log::info("eq-cli daemon tests passed");
  return 0;
}
