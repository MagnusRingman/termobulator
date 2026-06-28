// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "mcp_server.h"

#include <unistd.h>

#include <chrono>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <lua.hpp>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "app_utils.h"
#include "inlined_docs.h"
#include "terminal_levels.h"
#include "termobulator_config.h"

namespace termobulator {

using json = nlohmann::json;
using termobulator::unstable::CreateSubprocessTerminal;
using termobulator::unstable::Terminal;

bool MatchLuaPatternBackground(const std::string& text,
                               const std::string& pattern,
                               std::string& matched_out) {
    lua_State* L = luaL_newstate();
    if (!L) return false;
    std::unique_ptr<lua_State, void (*)(lua_State*)> L_guard(L, lua_close);

    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);

    lua_getglobal(L, "string");
    lua_getfield(L, -1, "match");
    lua_remove(L, -2);

    lua_pushlstring(L, text.data(), text.size());
    lua_pushlstring(L, pattern.data(), pattern.size());

    bool matched = false;
    if (lua_pcall(L, 2, 1, 0) == LUA_OK) {
        if (!lua_isnil(L, -1)) {
            size_t len = 0;
            const char* m = lua_tolstring(L, -1, &len);
            if (m) {
                matched_out = std::string(m, len);
            }
            matched = true;
        }
    }
    return matched;
}

void McpServer::RunSessionWatcher(
    McpServer* server, std::string sess_id,
    std::shared_ptr<termobulator::unstable::Terminal> term,
    std::shared_ptr<std::atomic<bool>> stop_flag,
    std::shared_ptr<std::condition_variable> cv) {
    while (!stop_flag->load()) {
        termobulator::unstable::WaitResult res = term->WaitIdle(5, 500);
        if (stop_flag->load()) break;

        std::lock_guard<std::mutex> lock(server->sessions_mutex_);
        auto it = server->session_watchers_.find(sess_id);
        if (it == server->session_watchers_.end()) break;

        auto& state = it->second;
        bool any_fired = false;

        termobulator::unstable::ScreenSnapshot snap = term->GetSnapshot(-1);

        for (auto& watcher : state.watchers) {
            bool condition_met = false;
            std::string matched_text;

            if (watcher.type == "text") {
                std::string escaped_pattern =
                    termobulator::unstable::Terminal::ParseEscapes(
                        watcher.pattern);
                for (unsigned int y = 0; y < snap.height; ++y) {
                    std::string row_str =
                        termobulator::app_utils::GetRow(snap, y);
                    if (row_str.find(escaped_pattern) != std::string::npos) {
                        condition_met = true;
                        matched_text = escaped_pattern;
                        break;
                    }
                }
            } else if (watcher.type == "pattern") {
                for (unsigned int y = 0; y < snap.height; ++y) {
                    std::string row_str =
                        termobulator::app_utils::GetRow(snap, y);
                    std::string matched;
                    if (MatchLuaPatternBackground(row_str, watcher.pattern,
                                                  matched)) {
                        condition_met = true;
                        matched_text = matched;
                        break;
                    }
                }
            }

            if (condition_met) {
                if (!watcher.has_fired ||
                    watcher.last_matched_text != matched_text) {
                    watcher.has_fired = true;
                    watcher.last_matched_text = matched_text;

                    McpServer::FiredEvent event;
                    event.watcher_id = watcher.watcher_id;
                    event.fired_at_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now()
                                .time_since_epoch())
                            .count();
                    event.matched_text = matched_text;

                    state.events.push_back(event);
                    if (state.events.size() > 1000) {
                        state.events.pop_front();
                    }
                    any_fired = true;
                }
            } else {
                watcher.has_fired = false;
                watcher.last_matched_text.clear();
            }
        }

        if (any_fired) {
            cv->notify_all();
        }
    }
}

McpServer::McpServer(unsigned int width, unsigned int height,
                     const std::string& term_type, const std::string& locale,
                     unsigned int idle_timeout_sec, bool do_log)
        : width_(width),
          height_(height),
          term_type_(term_type),
          locale_(locale),
          do_log_(do_log),
          idle_timeout_sec_(idle_timeout_sec) {}

int McpServer::Run() {
    if (do_log_) {
        std::string log_path =
            "/tmp/termobulator-" + std::to_string(getpid()) + ".log";
        log_file_.open(log_path);
    }
    stop_idle_check_ = false;
    idle_check_thread_ = std::thread([this]() {
        while (!stop_idle_check_) {
            std::unique_lock<std::mutex> lock(sessions_mutex_);
            // Wait up to 1 second or until stopped for faster responsiveness
            // and test speed
            idle_check_cv_.wait_for(lock, std::chrono::seconds(1), [this]() {
                return stop_idle_check_.load();
            });
            if (stop_idle_check_) break;

            auto now = std::chrono::steady_clock::now();
            std::vector<std::string> sessions_to_close;
            for (const auto& pair : sessions_last_activity_) {
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        now - pair.second)
                        .count();
                if (elapsed >= idle_timeout_sec_) {
                    sessions_to_close.push_back(pair.first);
                }
            }

            for (const auto& sess_id : sessions_to_close) {
                CloseSessionInternal(sess_id);
            }
        }
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        WriteLog("RECV", line);
        try {
            json req = json::parse(line);
            ProcessMcpMessage(req);
        } catch (const std::exception& e) {
            SendJsonRpcError(nullptr, -32700,
                             std::string("Parse error: ") + e.what());
        }
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& pair : session_watchers_) {
            if (pair.second.stop_watcher) {
                pair.second.stop_watcher->store(true);
            }
            if (pair.second.watcher_cv) {
                pair.second.watcher_cv->notify_all();
            }
        }
    }
    for (auto& pair : session_watchers_) {
        if (pair.second.watcher_thread &&
            pair.second.watcher_thread->joinable()) {
            pair.second.watcher_thread->join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        session_watchers_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        stop_idle_check_ = true;
        idle_check_cv_.notify_all();
    }
    if (idle_check_thread_.joinable()) {
        idle_check_thread_.join();
    }

    if (log_file_.is_open()) {
        log_file_.close();
    }

    // Return the exit status of the first created session, if any.
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    if (!first_session_id_.empty()) {
        auto it = sessions_.find(first_session_id_);
        if (it != sessions_.end()) {
            return it->second->ExitStatus();
        }
        return first_session_exit_code_;
    }
    return 0;
}

void McpServer::SendJson(const json& resp) {
    std::string s = resp.dump();
    std::cout << s << std::endl;
    WriteLog("SEND", s);
}

void McpServer::WriteLog(std::string_view direction,
                         const std::string& message) {
    if (do_log_ && log_file_.is_open()) {
        log_file_ << "[" << direction << "] " << message << "\n";
        log_file_.flush();
    }
}

void McpServer::SendJsonRpcError(const json& id, int code,
                                 const std::string& message) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id.is_null() ? json(nullptr) : id;
    resp["error"]["code"] = code;
    resp["error"]["message"] = message;
    SendJson(resp);
}

void McpServer::ProcessMcpMessage(const json& msg) {
    if (!msg.contains("jsonrpc") || msg["jsonrpc"] != "2.0") {
        SendJsonRpcError(
            msg.contains("id") ? msg["id"] : nullptr, -32600,
            "Invalid Request: missing or invalid jsonrpc version");
        return;
    }

    if (!msg.contains("method")) {
        if (msg.contains("id") && msg["id"] == "get_roots") {
            if (msg.contains("result") && msg["result"].contains("roots")) {
                ParseRoots(msg["result"]["roots"]);
            } else if (msg.contains("error")) {
                WriteLog("ERROR",
                         "Failed to retrieve roots: " + msg["error"].dump());
            }
            return;
        }
        SendJsonRpcError(msg.contains("id") ? msg["id"] : nullptr, -32600,
                         "Invalid Request: missing method");
        return;
    }

    std::string method = msg["method"];
    bool has_id = msg.contains("id");
    json id = has_id ? msg["id"] : json(nullptr);

    if (has_id) {
        if (!id.is_string() && !id.is_number() && !id.is_null()) {
            SendJsonRpcError(
                json(nullptr), -32600,
                "Invalid Request: id must be string, number, or null");
            return;
        }
    }

    if (method == "initialize") {
        HandleInitialize(id, msg);
    } else if (method == "notifications/initialized") {
        HandleInitialized(msg);
    } else if (method == "notifications/roots/list_changed") {
        HandleRootsListChanged(msg);
    } else if (!initialized_) {
        if (has_id) {
            SendJsonRpcError(id, -32002, "Server not initialized");
        }
    } else if (method == "tools/list") {
        HandleToolsList(id, msg);
    } else if (method == "tools/call") {
        HandleToolsCall(id, msg);
    } else if (method == "resources/list") {
        HandleResourcesList(id, msg);
    } else if (method == "resources/read") {
        HandleResourcesRead(id, msg);
    } else {
        if (has_id) {
            SendJsonRpcError(id, -32601, "Method not found: " + method);
        }
    }
}

void McpServer::HandleInitialize(const json& id, const json& req) {
    initialize_received_ = true;
    if (req.contains("params") && req["params"].contains("capabilities")) {
        const auto& caps = req["params"]["capabilities"];
        if (caps.contains("roots")) {
            client_supports_roots_ = true;
        }
    }
    if (req.contains("params") && req["params"].contains("roots")) {
        ParseRoots(req["params"]["roots"]);
    }
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"]["protocolVersion"] = "2024-11-05";
    resp["result"]["capabilities"]["tools"] = json::object();
    resp["result"]["capabilities"]["resources"] = json::object();
    resp["result"]["serverInfo"]["name"] = "termobulator";
    resp["result"]["serverInfo"]["version"] = TERMOBULATOR_VERSION;
    resp["result"]["instructions"] = termobulator::doc::GetMcpInstructions();
    SendJson(resp);
}

void McpServer::HandleInitialized(const json& req) {
    if (initialize_received_) {
        initialized_ = true;
        bool should_request = false;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            should_request =
                client_supports_roots_ && workspace_roots_.empty();
        }
        if (should_request) {
            RequestRoots();
        }
    }
}

std::shared_ptr<termobulator::unstable::Terminal> McpServer::GetTargetSession(
    const json& args) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::string sess_id;
    if (args.contains("session_id")) {
        sess_id = args.at("session_id").get<std::string>();
    } else {
        sess_id = active_session_id_;
    }
    if (sess_id.empty()) {
        throw std::runtime_error(
            "No active session. Please create a session using create_session "
            "first.");
    }
    if (auto it = sessions_.find(sess_id); it == sessions_.end()) {
        throw std::runtime_error("Session not found: " + sess_id);
    } else {
        sessions_last_activity_[sess_id] = std::chrono::steady_clock::now();
        return it->second;
    }
}

std::shared_ptr<termobulator::unstable::LuaExecutor> McpServer::GetExecutor(
    const json& args) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::string sess_id;
    if (args.contains("session_id")) {
        sess_id = args.at("session_id").get<std::string>();
    } else {
        sess_id = active_session_id_;
    }
    if (sess_id.empty()) {
        throw std::runtime_error(
            "No active session. Please create a session using create_session "
            "first.");
    }
    auto it_term = sessions_.find(sess_id);
    if (it_term == sessions_.end()) {
        throw std::runtime_error("Session not found: " + sess_id);
    }
    sessions_last_activity_[sess_id] = std::chrono::steady_clock::now();

    auto it_exec = executors_.find(sess_id);
    if (it_exec == executors_.end()) {
        auto exec = std::make_shared<termobulator::unstable::LuaExecutor>(
            it_term->second.get());
        executors_[sess_id] = exec;
        return exec;
    }
    return it_exec->second;
}

void McpServer::CloseSessionInternal(const std::string& sess_id) {
    std::shared_ptr<std::thread> thread_to_join;
    auto it_watch = session_watchers_.find(sess_id);
    if (it_watch != session_watchers_.end()) {
        if (it_watch->second.stop_watcher) {
            it_watch->second.stop_watcher->store(true);
        }
        if (it_watch->second.watcher_cv) {
            it_watch->second.watcher_cv->notify_all();
        }
        thread_to_join = it_watch->second.watcher_thread;
        session_watchers_.erase(it_watch);
    }

    auto it = sessions_.find(sess_id);
    if (it != sessions_.end()) {
        if (sess_id == first_session_id_) {
            first_session_exit_code_ = it->second->ExitStatus();
        }
        sessions_.erase(it);
        sessions_last_activity_.erase(sess_id);
    }
    auto it_exec = executors_.find(sess_id);
    if (it_exec != executors_.end()) {
        executors_.erase(it_exec);
    }
    if (active_session_id_ == sess_id) {
        if (!sessions_.empty()) {
            active_session_id_ = sessions_.begin()->first;
        } else {
            active_session_id_ = "";
        }
    }

    if (thread_to_join && thread_to_join->joinable()) {
        sessions_mutex_.unlock();
        thread_to_join->join();
        sessions_mutex_.lock();
    }
}

const std::vector<McpServer::Tool>& McpServer::GetTools() {
    static const std::vector<Tool> kTools = {
        {"create_session",
         "Spawn a new terminal session running the specified binary.",
         {{"binary",
           {{"type", "string"},
            {"description", "Path to the executable binary to run."}}},
          {"arguments",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "Optional list of command line arguments."}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional unique identifier for the session. If not provided, "
             "one will be generated."}}},
          {"width",
           {{"type", "integer"},
            {"description",
             "Optional terminal width (default: width specified at server "
             "start)."}}},
          {"height",
           {{"type", "integer"},
            {"description",
             "Optional terminal height (default: height specified at server "
             "start)."}}},
          {"terminal",
           {{"type", "string"},
            {"description",
             "Optional terminal type. Accepts symbolic level names "
             "(\"minimal\", \"basic\", \"extended\", \"full\") with optional "
             "color suffix (e.g. \"basic-8color\", \"full-16color\"), or any "
             "raw terminfo name. Default: terminal type specified at server "
             "start (\"full\" = xterm-256color)."}}},
          {"locale",
           {{"type", "string"},
            {"description",
             "Optional locale setting (e.g. \"en_US.UTF-8\", default: locale "
             "specified at server start)."}}},
          {"disable_alternate_screen",
           {{"type", "boolean"},
            {"description",
             "Optional boolean to disable switching to the terminal's "
             "alternate "
             "screen buffer. This helps capture scrollback history for "
             "curses/TUI "
             "applications."}}},
          {"scrollback_size",
           {{"type", "integer"},
            {"description",
             "Optional maximum number of scrollback lines to retain "
             "(default: "
             "200)."}}}},
         {"binary"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("binary"))
                 throw std::runtime_error(
                     "Missing required parameter: binary");
             std::string binary = args.at("binary").get<std::string>();
             std::string resolved_bin = server->ResolveBinaryPath(binary);
             if (!server->IsPathInWorkspace(resolved_bin)) {
                 throw std::runtime_error("Access denied: target binary '" +
                                          binary + "' (resolved: '" +
                                          resolved_bin +
                                          "') lies outside the workspace.");
             }
             std::vector<std::string> cmd_args;
             if (args.contains("arguments")) {
                 cmd_args = args["arguments"].get<std::vector<std::string>>();
             }
             unsigned int w = args.contains("width")
                                  ? args["width"].get<unsigned int>()
                                  : server->width_;
             unsigned int h = args.contains("height")
                                  ? args["height"].get<unsigned int>()
                                  : server->height_;
             std::string sess_id;
             unsigned int sb_size =
                 args.contains("scrollback_size")
                     ? args["scrollback_size"].get<unsigned int>()
                     : 200;
             bool disable_alt =
                 args.contains("disable_alternate_screen")
                     ? args["disable_alternate_screen"].get<bool>()
                     : false;
             std::string term = args.contains("terminal")
                                    ? args["terminal"].get<std::string>()
                                    : server->term_type_;
             std::string resolved_term = ResolveTerminalType(term);
             std::string warning;
             if (!IsKnownTerminalType(resolved_term)) {
                 warning =
                     "Terminal type '" + resolved_term +
                     "' is not in the set of known-good terminal types. "
                     "The session will use it as-is, but the DUT may fail "
                     "if the corresponding terminfo entry is not installed "
                     "on the host.";
             }
             std::string loc = args.contains("locale")
                                   ? args["locale"].get<std::string>()
                                   : server->locale_;
             std::shared_ptr<termobulator::unstable::Terminal> term_obj =
                 CreateSubprocessTerminal(w, h, binary, cmd_args,
                                          resolved_term, loc, sb_size);
             term_obj->SetDisableAlternateScreen(disable_alt);

             {
                 std::lock_guard<std::mutex> lock(server->sessions_mutex_);
                 if (args.contains("session_id")) {
                     sess_id = args["session_id"].get<std::string>();
                     if (server->sessions_.count(sess_id) > 0) {
                         throw std::runtime_error(
                             "Session ID already exists: " + sess_id);
                     }
                 } else {
                     sess_id = "session_" +
                               std::to_string(server->next_session_number_++);
                     while (server->sessions_.count(sess_id) > 0) {
                         sess_id =
                             "session_" +
                             std::to_string(server->next_session_number_++);
                     }
                 }
                 server->sessions_[sess_id] = std::move(term_obj);
                 server->sessions_last_activity_[sess_id] =
                     std::chrono::steady_clock::now();
                 if (server->first_session_id_.empty()) {
                     server->first_session_id_ = sess_id;
                 }
                 if (server->active_session_id_.empty()) {
                     server->active_session_id_ = sess_id;
                 }
             }
             json response_json = {{"session_id", sess_id},
                                   {"terminal", resolved_term}};
             if (!warning.empty()) {
                 response_json["warning"] = warning;
             }
             return {
                 {"content", json::array({{{"type", "text"},
                                           {"text", response_json.dump()}}})}};
         }},
        {"close_session",
         "Terminate and close a terminal session.",
         {{"session_id",
           {{"type", "string"},
            {"description", "The ID of the session to close."}}}},
         {"session_id"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("session_id")) {
                 throw std::runtime_error(
                     "Missing required parameter: session_id");
             }
             std::string sess_id = args.at("session_id").get<std::string>();
             std::lock_guard<std::mutex> lock(server->sessions_mutex_);
             if (server->sessions_.count(sess_id) == 0) {
                 throw std::runtime_error("Session not found: " + sess_id);
             }
             server->CloseSessionInternal(sess_id);
             return {{"content",
                      json::array(
                          {{{"type", "text"}, {"text", "Session closed."}}})}};
         }},
        {"list_sessions",
         "List all active terminal sessions and their states.",
         {},
         {},
         [](McpServer* server, const json& args) -> json {
             std::lock_guard<std::mutex> lock(server->sessions_mutex_);
             json session_list = json::array();
             for (const auto& pair : server->sessions_) {
                 json item;
                 item["session_id"] = pair.first;
                 item["status"] =
                     pair.second->IsExited() ? "exited" : "running";
                 item["exit_code"] = pair.second->ExitStatus();
                 item["active"] = (pair.first == server->active_session_id_);
                 session_list.push_back(item);
             }
             return {
                 {"content", json::array({{{"type", "text"},
                                           {"text", session_list.dump()}}})}};
         }},
        {"set_active_session",
         "Set the default active session for subsequent tool calls.",
         {{"session_id",
           {{"type", "string"},
            {"description", "The ID of the session to activate."}}}},
         {"session_id"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("session_id")) {
                 throw std::runtime_error(
                     "Missing required parameter: session_id");
             }
             std::string sess_id = args.at("session_id").get<std::string>();
             std::lock_guard<std::mutex> lock(server->sessions_mutex_);
             if (server->sessions_.count(sess_id) == 0) {
                 throw std::runtime_error("Session not found: " + sess_id);
             }
             server->active_session_id_ = sess_id;
             server->sessions_last_activity_[sess_id] =
                 std::chrono::steady_clock::now();
             return {{"content",
                      json::array(
                          {{{"type", "text"},
                            {"text", "Active session set to " + sess_id}}})}};
         }},
        {"execute_dsl",
         "Execute a Lua script on the targeted session. Sandboxed standard "
         "libraries (base, string, table, math, utf8) and term.* functions "
         "are available. Variables persist in the global 'vars' table.",
         {{"script",
           {{"type", "string"},
            {"description", "The Lua script string to execute."}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {"script"},
         [](McpServer* server, const json& args) -> json {
             auto executor = server->GetExecutor(args);
             std::string script = args.at("script").get<std::string>();
             auto res = executor->Execute(script);
             json resp_payload = {{"result", res.result},
                                  {"log", res.log},
                                  {"variables", res.variables}};
             return {
                 {"content", json::array({{{"type", "text"},
                                           {"text", resp_payload.dump()}}})}};
         }},
        {"register_watcher",
         "Register a persistent watcher on the targeted session.",
         {{"watcher_id",
           {{"type", "string"},
            {"description", "Unique identifier for the watcher."}}},
          {"condition",
           {{"type", "object"},
            {"properties",
             {{"type",
               {{"type", "string"},
                {"enum", {"text", "pattern"}},
                {"description", "Match type."}}},
              {"pattern",
               {{"type", "string"},
                {"description", "The text or Lua pattern to match."}}}}},
            {"required", {"type", "pattern"}}}},
          {"session_id",
           {{"type", "string"}, {"description", "Optional session ID."}}}},
         {"watcher_id", "condition"},
         [](McpServer* server, const json& args) -> json {
             std::string sess_id = args.contains("session_id")
                                       ? args["session_id"].get<std::string>()
                                       : server->active_session_id_;
             if (sess_id.empty()) {
                 throw std::runtime_error("No active session.");
             }
             std::string watcher_id = args["watcher_id"].get<std::string>();
             json condition = args["condition"];
             std::string type = condition["type"].get<std::string>();
             std::string pattern = condition["pattern"].get<std::string>();

             std::lock_guard<std::mutex> lock(server->sessions_mutex_);
             auto it_term = server->sessions_.find(sess_id);
             if (it_term == server->sessions_.end()) {
                 throw std::runtime_error("Session not found: " + sess_id);
             }

             auto& state = server->session_watchers_[sess_id];
             bool found = false;
             for (auto& watcher : state.watchers) {
                 if (watcher.watcher_id == watcher_id) {
                     watcher.type = type;
                     watcher.pattern = pattern;
                     watcher.has_fired = false;
                     watcher.last_matched_text.clear();
                     found = true;
                     break;
                 }
             }
             if (!found) {
                 PersistentWatcher w;
                 w.watcher_id = watcher_id;
                 w.type = type;
                 w.pattern = pattern;
                 state.watchers.push_back(w);
             }

             if (!state.watcher_thread) {
                 state.stop_watcher =
                     std::make_shared<std::atomic<bool>>(false);
                 state.watcher_cv =
                     std::make_shared<std::condition_variable>();
                 auto term = it_term->second;
                 auto stop_flag = state.stop_watcher;
                 auto cv = state.watcher_cv;
                 state.watcher_thread = std::make_shared<std::thread>(
                     [server, sess_id, term, stop_flag, cv]() {
                         RunSessionWatcher(server, sess_id, term, stop_flag,
                                           cv);
                     });
             }

             return {{"content",
                      json::array({{{"type", "text"},
                                    {"text", "Watcher registered."}}})}};
         }},
        {"check_watchers",
         "Poll and retrieve any fired watcher events for the targeted "
         "session.",
         {{"session_id",
           {{"type", "string"}, {"description", "Optional session ID."}}}},
         {},
         [](McpServer* server, const json& args) -> json {
             std::string sess_id = args.contains("session_id")
                                       ? args["session_id"].get<std::string>()
                                       : server->active_session_id_;
             if (sess_id.empty()) {
                 throw std::runtime_error("No active session.");
             }

             std::lock_guard<std::mutex> lock(server->sessions_mutex_);
             auto it = server->session_watchers_.find(sess_id);
             json events_json = json::array();
             if (it != server->session_watchers_.end()) {
                 auto& state = it->second;
                 while (!state.events.empty()) {
                     auto event = state.events.front();
                     state.events.pop_front();
                     events_json.push_back(
                         {{"watcher_id", event.watcher_id},
                          {"fired_at_ms", event.fired_at_ms},
                          {"matched_text", event.matched_text}});
                 }
             }
             return {
                 {"content", json::array({{{"type", "text"},
                                           {"text", events_json.dump()}}})}};
         }},
        {"await_watchers",
         "Block until a persistent watcher fires on the targeted session.",
         {{"timeout_ms",
           {{"type", "integer"},
            {"description", "Max block time in milliseconds."}}},
          {"watcher_ids",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "Optional list of watcher IDs to wait for."}}},
          {"session_id",
           {{"type", "string"}, {"description", "Optional session ID."}}}},
         {"timeout_ms"},
         [](McpServer* server, const json& args) -> json {
             std::string sess_id = args.contains("session_id")
                                       ? args["session_id"].get<std::string>()
                                       : server->active_session_id_;
             if (sess_id.empty()) {
                 throw std::runtime_error("No active session.");
             }
             int timeout_ms = args["timeout_ms"].get<int>();
             std::vector<std::string> filter_ids;
             if (args.contains("watcher_ids")) {
                 filter_ids =
                     args["watcher_ids"].get<std::vector<std::string>>();
             }

             std::unique_lock<std::mutex> lock(server->sessions_mutex_);
             auto it = server->session_watchers_.find(sess_id);
             if (it == server->session_watchers_.end()) {
                 std::condition_variable cv;
                 cv.wait_for(lock, std::chrono::milliseconds(timeout_ms));
                 return {{"content",
                          json::array({{{"type", "text"}, {"text", "[]"}}})}};
             }

             auto& state = it->second;
             auto cv = state.watcher_cv;
             auto stop_flag = state.stop_watcher;

             auto filter_match = [&](const FiredEvent& event) {
                 if (filter_ids.empty()) return true;
                 for (const auto& fid : filter_ids) {
                     if (fid == event.watcher_id) return true;
                 }
                 return false;
             };

             auto has_matching_events = [&]() {
                 for (const auto& ev : state.events) {
                     if (filter_match(ev)) return true;
                 }
                 return false;
             };

             if (!has_matching_events()) {
                 cv->wait_for(
                     lock, std::chrono::milliseconds(timeout_ms), [&]() {
                         return has_matching_events() || stop_flag->load();
                     });
             }

             json events_json = json::array();
             std::deque<FiredEvent> remaining;
             for (const auto& event : state.events) {
                 if (filter_match(event)) {
                     events_json.push_back(
                         {{"watcher_id", event.watcher_id},
                          {"fired_at_ms", event.fired_at_ms},
                          {"matched_text", event.matched_text}});
                 } else {
                     remaining.push_back(event);
                 }
             }
             state.events = std::move(remaining);

             return {
                 {"content", json::array({{{"type", "text"},
                                           {"text", events_json.dump()}}})}};
         }}};
    return kTools;
}

void McpServer::HandleToolsList(const json& id, const json& req) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;

    json tools = json::array();
    for (const auto& tool : GetTools()) {
        json t;
        t["name"] = std::string(tool.name);
        t["description"] = std::string(tool.description);
        t["inputSchema"]["type"] = "object";
        t["inputSchema"]["properties"] =
            tool.properties.is_object() ? tool.properties : json::object();
        if (!tool.required.empty()) {
            t["inputSchema"]["required"] = tool.required;
        }

        tools.push_back(t);
    }

    resp["result"]["tools"] = tools;
    SendJson(resp);
}

void McpServer::HandleToolsCall(const json& id, const json& req) {
    if (!req.contains("params") || !req["params"].contains("name")) {
        SendJsonRpcError(id, -32602, "Invalid params: missing tool name");
        return;
    }
    std::string name = req["params"]["name"];
    json args = req["params"].contains("arguments")
                    ? req["params"]["arguments"]
                    : json::object();

    try {
        json tool_result = ExecuteTool(name, args);
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["result"] = tool_result;
        SendJson(resp);
    } catch (const std::exception& e) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["result"]["isError"] = true;
        resp["result"]["content"] =
            json::array({{{"type", "text"},
                          {"text", std::string("Tool error: ") + e.what()}}});
        SendJson(resp);
    }
}

json McpServer::ExecuteTool(const std::string& name, const json& args) {
    for (const auto& tool : GetTools()) {
        if (tool.name == name) {
            return tool.handler(this, args);
        }
    }
    throw std::runtime_error("Unknown tool: " + name);
}

void McpServer::ParseRoots(const json& roots_json) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    workspace_roots_.clear();
    if (!roots_json.is_array()) return;
    for (const auto& item : roots_json) {
        if (item.contains("uri")) {
            std::string uri = item["uri"].get<std::string>();
            if (uri.rfind("file://", 0) == 0) {
                std::string path = uri.substr(7);
                std::string decoded;
                for (size_t i = 0; i < path.size(); ++i) {
                    if (path[i] == '%' && i + 2 < path.size()) {
                        int value = 0;
                        std::stringstream ss;
                        ss << std::hex << path.substr(i + 1, 2);
                        ss >> value;
                        decoded.push_back(static_cast<char>(value));
                        i += 2;
                    } else {
                        decoded.push_back(path[i]);
                    }
                }
                char actual_path[PATH_MAX];
                if (realpath(decoded.c_str(), actual_path) != nullptr) {
                    workspace_roots_.push_back(actual_path);
                } else {
                    workspace_roots_.push_back(decoded);
                }
            }
        }
    }
}

void McpServer::RequestRoots() {
    json req;
    req["jsonrpc"] = "2.0";
    req["method"] = "roots/list";
    req["id"] = "get_roots";
    SendJson(req);
}

void McpServer::HandleRootsListChanged(const json& req) {
    if (initialized_ && client_supports_roots_) {
        RequestRoots();
    }
}

bool McpServer::IsPathInWorkspace(const std::string& binary_path) {
    std::vector<std::string> allowed_roots;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        if (!client_supports_roots_) {
            return true;
        }
        allowed_roots = workspace_roots_;
    }

    if (allowed_roots.empty()) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != nullptr) {
            allowed_roots.push_back(cwd);
        }
    }
    for (const auto& root : allowed_roots) {
        std::string clean_root = root;
        if (!clean_root.empty() && clean_root.back() != '/') {
            clean_root += '/';
        }
        if (binary_path == root) {
            return true;
        }
        if (binary_path.rfind(clean_root, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::string McpServer::ResolveBinaryPath(const std::string& binary) {
    if (binary.empty()) return "";

    if (binary[0] == '/') {
        char abs_path[PATH_MAX];
        if (realpath(binary.c_str(), abs_path) != nullptr) {
            return abs_path;
        }
        return binary;
    }

    if (binary.find('/') != std::string::npos) {
        char abs_path[PATH_MAX];
        if (realpath(binary.c_str(), abs_path) != nullptr) {
            return abs_path;
        }
        return binary;
    }

    char abs_path[PATH_MAX];
    if (realpath(binary.c_str(), abs_path) != nullptr) {
        if (access(abs_path, X_OK) == 0) {
            return abs_path;
        }
    }

    const char* path_env = std::getenv("PATH");
    if (path_env != nullptr) {
        std::string path_str(path_env);
        std::string segment;
        std::stringstream ss(path_str);
        while (std::getline(ss, segment, ':')) {
            if (segment.empty()) segment = ".";
            std::string full_path = segment + "/" + binary;
            if (realpath(full_path.c_str(), abs_path) != nullptr) {
                if (access(abs_path, X_OK) == 0) {
                    return abs_path;
                }
            }
        }
    }

    if (realpath(binary.c_str(), abs_path) != nullptr) {
        return abs_path;
    }
    return binary;
}

void McpServer::HandleResourcesList(const json& id, const json& req) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;

    json resources = json::array();

    // Add default terminal levels documentation
    resources.push_back({{"uri", "termobulator://docs/terminal-levels"},
                         {"name", "Terminal Capability Levels"},
                         {"mimeType", "text/markdown"}});

    // Add recipes catalog
    resources.push_back({{"uri", "termobulator://recipes/catalog"},
                         {"name", "Bootstrapping Recipes Catalog"},
                         {"mimeType", "text/markdown"}});

    // Add individual recipes
    for (const auto& recipe : doc::GetRecipes()) {
        std::string uri =
            "termobulator://recipes/" + recipe.persona + "/" + recipe.id;
        resources.push_back({{"uri", uri},
                             {"name", recipe.title},
                             {"description", recipe.description},
                             {"mimeType", "text/markdown"}});
    }

    resp["result"]["resources"] = resources;
    SendJson(resp);
}

void McpServer::HandleResourcesRead(const json& id, const json& req) {
    if (!req.contains("params") || !req["params"].contains("uri")) {
        SendJsonRpcError(id, -32602, "Invalid params: missing uri");
        return;
    }
    std::string uri = req["params"]["uri"].get<std::string>();

    std::string content_text;
    bool found = false;

    if (uri == "termobulator://docs/terminal-levels") {
        content_text = GetTerminalLevelsDocumentation();
        found = true;
    } else if (uri == "termobulator://recipes/catalog") {
        std::stringstream ss;
        ss << "# Termobulator Bootstrapping Recipes Catalog\n\n";
        ss << "This catalog contains guidelines and recipes to help you "
              "interact with TUIs effectively.\n\n";

        ss << "## Persona A: Incidental TUI Operators\n";
        ss << "Guidelines for when a TUI stands between you and your "
              "task.\n\n";
        for (const auto& recipe : doc::GetRecipes()) {
            if (recipe.persona == "incidental") {
                ss << "* **[" << recipe.title
                   << "](termobulator://recipes/incidental/" << recipe.id
                   << ")**\n";
                ss << "  " << recipe.description << "\n\n";
            }
        }

        ss << "## Persona B: TUI Developers & Testers\n";
        ss << "Guidelines for building and verifying TUI behavior with "
              "Termobulator.\n\n";
        for (const auto& recipe : doc::GetRecipes()) {
            if (recipe.persona == "developer") {
                ss << "* **[" << recipe.title
                   << "](termobulator://recipes/developer/" << recipe.id
                   << ")**\n";
                ss << "  " << recipe.description << "\n\n";
            }
        }

        content_text = ss.str();
        found = true;
    } else {
        for (const auto& recipe : doc::GetRecipes()) {
            std::string expected_uri =
                "termobulator://recipes/" + recipe.persona + "/" + recipe.id;
            if (uri == expected_uri) {
                content_text = recipe.content;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        SendJsonRpcError(id, -32002, "Resource not found: " + uri);
        return;
    }

    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"]["contents"] = json::array({{{"uri", uri},
                                               {"mimeType", "text/markdown"},
                                               {"text", content_text}}});
    SendJson(resp);
}

}  // namespace termobulator
