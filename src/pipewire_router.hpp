#pragma once

#include <atomic>
#include <memory>
#include <cstdint>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spa/utils/hook.h>

#include "ee_eq_preset_parser.hpp"

struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_thread_loop;
struct pw_proxy;
struct pw_metadata;
struct pw_filter;
struct spa_dict;
struct pw_node_info;
struct pw_core_info;
struct pw_link_info;
struct spa_io_position;

namespace ee {

class PipeWireRouter {
 public:
  struct RuntimeSnapshot {
    std::string sink_name;
    uint64_t sink_serial = 0;
    std::vector<std::string> active_plugins;
    std::string init_error;
    bool bypass = false;
    float volume = 1.0F;
  };

  struct NodeData;
  struct LinkState {
    pw_proxy* proxy = nullptr;
    spa_hook listener{};
    std::atomic<int> state{0};  // PW_LINK_STATE_INIT == 0; atomic for cross-thread access
  };
  class EqFilterNode;
  class LimiterFilterNode;
  class ConvolverFilterNode;

  explicit PipeWireRouter(ParsedPreset preset, std::string sink_selector = {});
  ~PipeWireRouter();

  PipeWireRouter(const PipeWireRouter&) = delete;
  auto operator=(const PipeWireRouter&) -> PipeWireRouter& = delete;

  auto start(std::string& error) -> bool;
  void stop();
  auto list_sinks(std::string& error) -> std::vector<std::string>;
  auto runtime_snapshot() const -> RuntimeSnapshot;
  void set_bypass(bool bypass);
  void set_volume(float volume);
  auto wake_fd() const -> int;
  auto next_task_timeout() const -> std::optional<std::chrono::milliseconds>;
  void run_due_tasks();

  // Callback targets used by the PipeWire listener shims in pipewire_router.cpp.
  void on_registry_global(uint32_t id, const char* type, const spa_dict* props);
  void on_node_info(NodeData* data, const pw_node_info* info);
  void on_node_proxy_destroy(NodeData* data);
  void on_node_proxy_removed(NodeData* data);
  void on_registry_global_remove(uint32_t id);
  void on_metadata_property(const char* key, const char* value);
  void on_core_done();
  void on_core_error(int res, const char* message);

 private:
  struct PortInfo {
    uint32_t id = 0;
    uint32_t node_id = 0;
    uint32_t port_id = 0;
    uint64_t serial = 0;
    std::string direction;
    std::string audio_channel;
    std::string name;
  };

  struct NodeInfo {
    uint32_t id = 0;
    uint64_t serial = 0;
    std::string name;
    std::string media_class;
    std::string target_object;
  };

  static void free_node_info(NodeData* node);

  struct PatchAttemptInfo {
    std::string name;
    std::chrono::steady_clock::time_point patched_at{};
  };

  struct LoopGuardEntry {
    int rapid_removals = 0;
    std::chrono::steady_clock::time_point last_removed_at{};
    std::chrono::steady_clock::time_point suppress_until{};
  };

  ParsedPreset preset_;
  std::string sink_selector_;
  std::atomic<float> volume_{1.0F};
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> startup_sink_locked_{false};

  pw_thread_loop* thread_loop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  pw_registry* registry_ = nullptr;
  pw_metadata* metadata_ = nullptr;
  pw_proxy* virtual_sink_proxy_ = nullptr;

  std::vector<NodeData*> bound_nodes_;
  std::vector<PortInfo> ports_;
  std::vector<std::unique_ptr<LinkState>> created_links_;
  std::unordered_set<uint64_t> patched_streams_;
  std::unordered_set<uint64_t> pending_patch_streams_;
  std::unordered_map<uint64_t, PatchAttemptInfo> recent_patch_attempts_;
  std::unordered_map<std::string, LoopGuardEntry> loop_guard_;
  std::unique_ptr<EqFilterNode> eq_filter_;
  std::unique_ptr<LimiterFilterNode> limiter_filter_;
  std::unique_ptr<ConvolverFilterNode> convolver_filter_;

  NodeInfo selected_sink_{};
  NodeInfo virtual_sink_{};
  mutable std::recursive_mutex state_mutex_;
  mutable std::mutex task_mutex_;
  std::multimap<std::chrono::steady_clock::time_point, std::function<void()>> scheduled_tasks_;
  int wake_fd_ = -1;

  std::unique_ptr<spa_hook> core_listener_;
  std::unique_ptr<spa_hook> registry_listener_;
  std::unique_ptr<spa_hook> metadata_listener_;

  auto setup_core(std::string& error) -> bool;
  auto wait_for_startup_sink(std::string& error) -> bool;
  auto wait_for_virtual_sink(std::string& error) -> bool;
  auto wait_for_node_ports(uint32_t node_id, uint32_t minimum_ports, std::string& error) -> bool;
  void create_virtual_sink(std::string& error);
  auto connect_chain(std::string& error) -> bool;
  void patch_existing_streams();
  void queue_patch_stream(uint64_t node_serial);
  void patch_stream(uint64_t node_serial);
  void clear_patched_streams();
  void destroy_links();
  void destroy_proxy_sync(pw_proxy* proxy, spa_hook* hook);
  auto wait_for_links_ready(std::string& error) -> bool;
  void reconnect_to_sink(const std::string& new_sink_name);

  auto snapshot_nodes() const -> std::vector<NodeInfo>;
  auto snapshot_ports() const -> std::vector<PortInfo>;
  auto snapshot_selected_sink() const -> NodeInfo;
  auto snapshot_virtual_sink() const -> NodeInfo;
  auto stream_targets_selected_sink(const NodeInfo& node) const -> bool;
  auto find_node_by_name(std::string_view name) -> std::optional<NodeInfo>;
  auto find_node_by_id(uint32_t id) const -> std::optional<NodeInfo>;
  auto find_node_by_serial(uint64_t serial) const -> std::optional<NodeInfo>;
  auto count_node_ports(uint32_t node_id) const -> uint32_t;
  auto get_node_ports(uint32_t node_id, const std::string& direction) const -> std::vector<PortInfo>;
  auto link_nodes(uint32_t output_node_id, uint32_t input_node_id, std::string& error) -> bool;

  void set_metadata_target_node(uint32_t origin_id, uint32_t target_id, uint64_t target_serial);
  void clear_metadata_target_node(uint32_t origin_id);
  void schedule_task(std::chrono::milliseconds delay, std::function<void()> action);

};

}  // namespace ee
