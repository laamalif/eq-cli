#include "daemon_ipc.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "app_metadata.hpp"
#include "logging.hpp"

namespace ee {

namespace {

constexpr size_t kMaxDaemonPayloadBytes = 64 * 1024;

auto write_all(int fd, std::string_view payload, std::string& error) -> bool {
  const char* data = payload.data();
  size_t remaining = payload.size();
  while (remaining > 0) {
    const auto bytes = send(fd, data, remaining, MSG_NOSIGNAL);
    if (bytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = std::format("socket write failed: {}", std::strerror(errno));
      return false;
    }
    data += bytes;
    remaining -= static_cast<size_t>(bytes);
  }
  return true;
}

auto daemon_socket_path_for_dir(std::string_view dir_name, std::string& error, const bool create_directory) -> std::string {
  const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  if (runtime_dir == nullptr || *runtime_dir == '\0') {
    error = "XDG_RUNTIME_DIR is required for daemon mode";
    return {};
  }

  const auto dir = std::filesystem::path(runtime_dir) / dir_name;
  if (create_directory) {
    std::error_code fs_error;
    std::filesystem::create_directories(dir, fs_error);
    if (fs_error) {
      error = std::format("failed to create daemon runtime directory: {}", fs_error.message());
      return {};
    }
  }

  return (dir / "daemon.sock").string();
}

auto bind_server_socket(const std::string& path, std::string& error) -> int {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    error = std::format("failed to create daemon socket: {}", std::strerror(errno));
    return -1;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    close(fd);
    error = "daemon socket path is too long";
    return -1;
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

  if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
    return fd;
  }

  if (errno != EADDRINUSE) {
    error = std::format("failed to bind daemon socket: {}", std::strerror(errno));
    close(fd);
    return -1;
  }

  const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (probe != -1 && connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
    close(probe);
    close(fd);
    error = "daemon already running";
    return -1;
  }
  if (probe != -1) {
    close(probe);
  }

  std::error_code fs_error;
  std::filesystem::remove(path, fs_error);
  if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    error = std::format("failed to rebind daemon socket: {}", std::strerror(errno));
    close(fd);
    return -1;
  }

  return fd;
}

auto read_request_json(int fd,
                       int signal_fd,
                       std::chrono::milliseconds timeout,
                       DaemonRequest& request,
                       std::string& error) -> bool {
  std::string payload;
  std::array<char, 4096> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    // The daemon is single-threaded: a client that connects but never
    // finishes its request must not block the accept loop (or shutdown
    // signals) forever, so the read is bounded by a deadline and also
    // watches the signalfd.
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
      error = "timed out reading daemon request";
      return false;
    }

    std::array<pollfd, 2> poll_fds{};
    nfds_t poll_count = 1;
    poll_fds[0] = {.fd = fd, .events = POLLIN, .revents = 0};
    if (signal_fd != -1) {
      poll_fds[1] = {.fd = signal_fd, .events = POLLIN, .revents = 0};
      poll_count = 2;
    }

    const int poll_result = poll(poll_fds.data(), poll_count, static_cast<int>(remaining.count()));
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = std::format("failed to poll daemon request: {}", std::strerror(errno));
      return false;
    }
    if (poll_result == 0) {
      error = "timed out reading daemon request";
      return false;
    }
    if (signal_fd != -1 && (poll_fds[1].revents & POLLIN) != 0) {
      // Leave the signal unread; the accept loop consumes it and shuts down.
      error = "daemon is shutting down";
      return false;
    }
    if (poll_fds[0].revents == 0) {
      continue;
    }

    const auto bytes = read(fd, buffer.data(), buffer.size());
    if (bytes == 0) {
      break;
    }
    if (bytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = std::format("failed to read daemon request: {}", std::strerror(errno));
      return false;
    }
    payload.append(buffer.data(), static_cast<size_t>(bytes));
    if (payload.size() > kMaxDaemonPayloadBytes) {
      error = "daemon request exceeds maximum payload size";
      return false;
    }
  }

  try {
    request = nlohmann::json::parse(payload).get<DaemonRequest>();
    return true;
  } catch (const std::exception& ex) {
    error = std::format("invalid daemon request: {}", ex.what());
    return false;
  }
}

void write_response_json(int fd, const DaemonResponse& response) {
  const auto encoded = nlohmann::json(response).dump();
  std::string error;
  if (!write_all(fd, encoded, error)) {
    log::warn(error);
  }
}

}  // namespace

auto daemon_socket_path(std::string& error) -> std::string {
  return daemon_socket_path_for_dir(kRuntimeDirName, error, true);
}

auto run_daemon_ipc_server(DaemonController& controller,
                           std::string& error,
                           const bool watch_signals,
                           const std::chrono::milliseconds request_read_timeout) -> int {
  const auto socket_path = daemon_socket_path(error);
  if (!error.empty()) {
    return EXIT_FAILURE;
  }

  const int listen_fd = bind_server_socket(socket_path, error);
  if (listen_fd == -1) {
    return EXIT_FAILURE;
  }

  if (listen(listen_fd, 8) != 0) {
    error = std::format("failed to listen on daemon socket: {}", std::strerror(errno));
    close(listen_fd);
    std::filesystem::remove(socket_path);
    return EXIT_FAILURE;
  }

  {
    const auto status = controller.handle_request(DaemonRequest{.command = "status"});
    ee::log::info("daemon ready");
    ee::log::info(std::format("socket: {}", socket_path));
    ee::log::info(std::format("session: {}",
        status.status.session_state == SessionLifecycleState::Enabled ? "enabled" : "disabled"));
    ee::log::info(std::format("pid: {}", status.status.pid));
  }

  int signal_fd = -1;
  if (watch_signals) {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) {
      error = std::format("sigprocmask failed: {}", std::strerror(errno));
      close(listen_fd);
      std::filesystem::remove(socket_path);
      return EXIT_FAILURE;
    }

    signal_fd = signalfd(-1, &signals, SFD_CLOEXEC);
    if (signal_fd == -1) {
      error = std::format("signalfd failed: {}", std::strerror(errno));
      close(listen_fd);
      std::filesystem::remove(socket_path);
      return EXIT_FAILURE;
    }
  }

  bool done = false;
  controller.set_shutdown_callback([&done]() { done = true; });

  while (!done) {
    std::array<pollfd, 2> poll_fds{};
    nfds_t poll_count = 1;
    poll_fds[0] = {.fd = listen_fd, .events = POLLIN, .revents = 0};
    if (watch_signals) {
      poll_fds[1] = {.fd = signal_fd, .events = POLLIN, .revents = 0};
      poll_count = 2;
    }

    const int poll_result = poll(poll_fds.data(), poll_count, -1);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = std::format("daemon poll failed: {}", std::strerror(errno));
      break;
    }

    if (watch_signals && (poll_fds[1].revents & POLLIN) != 0) {
      done = true;
      break;
    }

    if ((poll_fds[0].revents & POLLIN) != 0) {
      const int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
      if (client_fd == -1) {
        continue;
      }

      DaemonRequest request;
      std::string request_error;
      DaemonResponse response;
      if (!read_request_json(client_fd, signal_fd, request_read_timeout, request, request_error)) {
        response.ok = false;
        response.error = request_error;
      } else {
        std::string log_line = std::format("request: {}", request.command);
        if (!request.preset_path.empty()) {
          log_line += std::format(" preset={}", request.preset_path);
        }
        if (!request.sink_selector.empty()) {
          log_line += std::format(" sink={}", request.sink_selector);
        }
        log::info(log_line);
        response = controller.handle_request(request);
      }

      write_response_json(client_fd, response);
      close(client_fd);
    }
  }

  if (signal_fd != -1) {
    close(signal_fd);
  }
  close(listen_fd);
  std::filesystem::remove(socket_path);
  return error.empty() ? EXIT_SUCCESS : EXIT_FAILURE;
}

auto send_daemon_request(const DaemonRequest& request, DaemonResponse& response, std::string& error) -> bool {
  const auto socket_path = daemon_socket_path_for_dir(kRuntimeDirName, error, false);
  if (!error.empty()) {
    return false;
  }

  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    error = std::format("failed to create daemon client socket: {}", std::strerror(errno));
    return false;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    error = std::format("daemon not running; start it with 'eq-cli daemon start' ({})", std::strerror(errno));
    close(fd);
    return false;
  }

  const auto encoded = nlohmann::json(request).dump();
  if (!write_all(fd, encoded, error)) {
    close(fd);
    return false;
  }
  shutdown(fd, SHUT_WR);

  std::string payload;
  std::array<char, 4096> buffer{};
  while (true) {
    const auto bytes = read(fd, buffer.data(), buffer.size());
    if (bytes == 0) {
      break;
    }
    if (bytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = std::format("failed to read daemon response: {}", std::strerror(errno));
      close(fd);
      return false;
    }
    payload.append(buffer.data(), static_cast<size_t>(bytes));
  }
  close(fd);

  try {
    response = nlohmann::json::parse(payload).get<DaemonResponse>();
    return true;
  } catch (const std::exception& ex) {
    error = std::format("invalid daemon response: {}", ex.what());
    return false;
  }
}

}  // namespace ee
