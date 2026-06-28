// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_MCP_SERVER_H
#define TERMOBULATOR_MCP_SERVER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lua_executor.h"
#include "termobulator.h"

namespace termobulator {

class McpServer {
  public:
    McpServer(unsigned int width, unsigned int height,
              const std::string& term_type = "xterm-256color",
              const std::string& locale = "",
              unsigned int idle_timeout_sec = 300, bool do_log = false);

    int Run();

  private:
    using json = nlohmann::json;

    struct Tool {
        std::string_view name;
        std::string_view description;
        json properties;
        std::vector<std::string_view> required;
        json (*handler)(McpServer* server, const json& args);
    };

    static const std::vector<Tool>& GetTools();

    void ProcessMcpMessage(const json& msg);
    void HandleInitialize(const json& id, const json& req);
    void HandleInitialized(const json& req);
    void HandleToolsList(const json& id, const json& req);
    void HandleToolsCall(const json& id, const json& req);
    void HandleResourcesList(const json& id, const json& req);
    void HandleResourcesRead(const json& id, const json& req);
    json ExecuteTool(const std::string& name, const json& args);
    void SendJson(const json& resp);
    void WriteLog(std::string_view direction, const std::string& message);
    void SendJsonRpcError(const json& id, int code,
                          const std::string& message);
    std::shared_ptr<termobulator::unstable::Terminal> GetTargetSession(
        const json& args);
    std::shared_ptr<termobulator::unstable::LuaExecutor> GetExecutor(
        const json& args);
    void CloseSessionInternal(const std::string& sess_id);

    void ParseRoots(const json& roots_json);
    void RequestRoots();
    void HandleRootsListChanged(const json& req);
    bool IsPathInWorkspace(const std::string& binary_path);
    std::string ResolveBinaryPath(const std::string& binary);

    unsigned int width_;
    unsigned int height_;
    std::string term_type_;
    std::string locale_;
    bool do_log_;

    bool initialized_ = false;
    bool initialize_received_ = false;
    bool client_supports_roots_ = false;
    std::vector<std::string> workspace_roots_;
    std::unordered_map<std::string,
                       std::shared_ptr<termobulator::unstable::Terminal>>
        sessions_;
    std::unordered_map<std::string,
                       std::shared_ptr<termobulator::unstable::LuaExecutor>>
        executors_;
    std::string active_session_id_;
    std::string first_session_id_;
    int first_session_exit_code_ = 0;
    int next_session_number_ = 1;

    unsigned int idle_timeout_sec_;
    std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        sessions_last_activity_;
    std::thread idle_check_thread_;
    std::atomic<bool> stop_idle_check_{false};
    std::condition_variable idle_check_cv_;

    static void RunSessionWatcher(
        McpServer* server, std::string sess_id,
        std::shared_ptr<termobulator::unstable::Terminal> term,
        std::shared_ptr<std::atomic<bool>> stop_flag,
        std::shared_ptr<std::condition_variable> cv);

    struct FiredEvent {
        std::string watcher_id;
        uint64_t fired_at_ms;
        std::string matched_text;
    };

    struct PersistentWatcher {
        std::string watcher_id;
        std::string type;  // "text" or "pattern"
        std::string pattern;
        bool has_fired = false;
        std::string last_matched_text;
    };

    struct SessionWatcherState {
        std::vector<PersistentWatcher> watchers;
        std::deque<FiredEvent> events;
        std::shared_ptr<std::thread> watcher_thread;
        std::shared_ptr<std::atomic<bool>> stop_watcher;
        std::shared_ptr<std::condition_variable> watcher_cv;
    };

    std::unordered_map<std::string, SessionWatcherState> session_watchers_;

    std::ofstream log_file_;
};

}  // namespace termobulator

#endif  // TERMOBULATOR_MCP_SERVER_H
