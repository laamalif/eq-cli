#pragma once

#include <chrono>
#include <string>

#include "daemon_controller.hpp"

namespace ee {

auto daemon_socket_path(std::string& error) -> std::string;
auto run_daemon_ipc_server(DaemonController& controller,
                           std::string& error,
                           bool watch_signals = true,
                           std::chrono::milliseconds request_read_timeout = std::chrono::seconds(5)) -> int;
auto send_daemon_request(const DaemonRequest& request, DaemonResponse& response, std::string& error) -> bool;

}  // namespace ee
