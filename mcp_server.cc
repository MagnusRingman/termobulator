// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "mcp_server.h"

#include <unistd.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "app_utils.h"

namespace termobulator {

using json = nlohmann::json;
using termobulator::unstable::CellAttr;
using termobulator::unstable::CreateSubprocessTerminal;
using termobulator::unstable::ScreenSnapshot;
using termobulator::unstable::Terminal;

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
    } else if (!initialized_) {
        if (has_id) {
            SendJsonRpcError(id, -32002, "Server not initialized");
        }
    } else if (method == "tools/list") {
        HandleToolsList(id, msg);
    } else if (method == "tools/call") {
        HandleToolsCall(id, msg);
    } else {
        if (has_id) {
            SendJsonRpcError(id, -32601, "Method not found: " + method);
        }
    }
}

void McpServer::HandleInitialize(const json& id, const json& req) {
    initialize_received_ = true;
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"]["protocolVersion"] = "2024-11-05";
    resp["result"]["capabilities"]["tools"] = json::object();
    resp["result"]["serverInfo"]["name"] = "termobulator";
    resp["result"]["serverInfo"]["version"] = "1.1.0";
    SendJson(resp);
}

void McpServer::HandleInitialized(const json& req) {
    if (initialize_received_) {
        initialized_ = true;
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

void McpServer::CloseSessionInternal(const std::string& sess_id) {
    auto it = sessions_.find(sess_id);
    if (it != sessions_.end()) {
        if (sess_id == first_session_id_) {
            first_session_exit_code_ = it->second->ExitStatus();
        }
        sessions_.erase(it);
        sessions_last_activity_.erase(sess_id);
    }
    if (active_session_id_ == sess_id) {
        if (!sessions_.empty()) {
            active_session_id_ = sessions_.begin()->first;
        } else {
            active_session_id_ = "";
        }
    }
}

const std::vector<McpServer::Tool>& McpServer::GetTools() {
    static const std::vector<Tool> kTools = {
        {"get_screen",
         "Get the raw text contents of the screen.",
         {{"snapshot_id",
           {{"type", "integer"},
            {"description",
             "Optional ID of a snapshot to read from. Use -1 or omit for the "
             "current active screen."}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {},
         [](McpServer* server, const json& args) -> json {
             int snap_id = args.contains("snapshot_id")
                               ? args["snapshot_id"].get<int>()
                               : -1;
             ScreenSnapshot snap =
                 server->GetTargetSession(args)->GetSnapshot(snap_id);
             std::string screen_str;
             for (unsigned int y = 0; y < snap.height; ++y) {
                 if (y > 0) screen_str += "\n";
                 std::string row_str = app_utils::GetRow(snap, y);
                 size_t endpos = row_str.find_last_not_of(" \t\r\n");
                 if (endpos != std::string::npos) {
                     screen_str += row_str.substr(0, endpos + 1);
                 }
             }
             return {{"content", json::array({{{"type", "text"},
                                               {"text", screen_str}}})}};
         }},
        {"get_cursor",
         "Get the cursor position and visibility state.",
         {{"snapshot_id",
           {{"type", "integer"},
            {"description",
             "Optional ID of a snapshot to read from. Use -1 or omit for the "
             "current active screen."}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {},
         [](McpServer* server, const json& args) -> json {
             int snap_id = args.contains("snapshot_id")
                               ? args["snapshot_id"].get<int>()
                               : -1;
             ScreenSnapshot snap =
                 server->GetTargetSession(args)->GetSnapshot(snap_id);
             json cursor_json = {{"col", snap.cursor_x},
                                 {"row", snap.cursor_y},
                                 {"visible", !snap.cursor_hidden}};
             return {
                 {"content", json::array({{{"type", "text"},
                                           {"text", cursor_json.dump()}}})}};
         }},
        {"get_cell",
         "Get the character and attributes at specific cell coordinates.",
         {{"x",
           {{"type", "integer"},
            {"description", "X coordinate (column, 0-indexed)"}}},
          {"y",
           {{"type", "integer"},
            {"description", "Y coordinate (row, 0-indexed)"}}},
          {"snapshot_id",
           {{"type", "integer"},
            {"description", "Optional snapshot ID to query."}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {"x", "y"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("x"))
                 throw std::runtime_error("Missing required parameter: x");
             if (!args.contains("y"))
                 throw std::runtime_error("Missing required parameter: y");
             int x = args.at("x").get<int>();
             int y = args.at("y").get<int>();
             int snap_id = args.contains("snapshot_id")
                               ? args["snapshot_id"].get<int>()
                               : -1;
             ScreenSnapshot snap =
                 server->GetTargetSession(args)->GetSnapshot(snap_id);
             if (y >= static_cast<int>(snap.height) ||
                 x >= static_cast<int>(snap.width) || y < 0 || x < 0) {
                 throw std::runtime_error("Coordinates out of range");
             }
             const auto& cell = snap.cells[y * snap.width + x];
             CellAttr attr = cell.attr;
             json cell_json = {
                 {"char", cell.ch},
                 {"fg", app_utils::FormatColor(attr.fccode, attr.fr, attr.fg,
                                               attr.fb)},
                 {"bg", app_utils::FormatColor(attr.bccode, attr.br, attr.bg,
                                               attr.bb)},
                 {"bold", attr.bold},
                 {"italic", attr.italic},
                 {"underline", attr.underline},
                 {"inverse", attr.inverse},
                 {"protect", attr.protect},
                 {"blink", attr.blink}};
             return {{"content", json::array({{{"type", "text"},
                                               {"text", cell_json.dump()}}})}};
         }},
        {"get_row",
         "Get the text contents of a single screen row.",
         {{"row",
           {{"type", "integer"}, {"description", "Row index (0-indexed)"}}},
          {"snapshot_id",
           {{"type", "integer"},
            {"description", "Optional ID of a snapshot to read from."}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {"row"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("row"))
                 throw std::runtime_error("Missing required parameter: row");
             int y = args.at("row").get<int>();
             int snap_id = args.contains("snapshot_id")
                               ? args["snapshot_id"].get<int>()
                               : -1;
             ScreenSnapshot snap =
                 server->GetTargetSession(args)->GetSnapshot(snap_id);
             if (y >= static_cast<int>(snap.height) || y < 0) {
                 throw std::runtime_error("Row index out of range: " +
                                          std::to_string(y));
             }
             std::string row_str = app_utils::GetRow(snap, y);
             size_t endpos = row_str.find_last_not_of(" \t\r\n");
             if (endpos != std::string::npos) {
                 row_str = row_str.substr(0, endpos + 1);
             } else {
                 row_str.clear();
             }
             return {{"content",
                      json::array({{{"type", "text"}, {"text", row_str}}})}};
         }},
        {"get_attributes",
         "Get unique terminal attributes present on the screen or show row "
         "column ranges for a specific attribute.",
         {{"snapshot_id",
           {{"type", "integer"}, {"description", "Optional snapshot ID."}}},
          {"attribute_id",
           {{"type", "integer"},
            {"description",
             "Optional attribute ID to show active ranges on each row. "
             "Attribute IDs returned by this tool are only stable for a fixed "
             "snapshot. Always use `take_snapshot` first and pass the same "
             "`snapshot_id` to both the listing call and the range-query "
             "call."}}},
          {"include_ranges",
           {{"type", "boolean"},
            {"description",
             "Optional boolean (default false). If true, include the per-row "
             "ranges where each attribute is active on the screen."}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {},
         [](McpServer* server, const json& args) -> json {
             if (args.contains("attribute_id") &&
                 !args.contains("snapshot_id")) {
                 throw std::runtime_error(
                     "snapshot_id is required when attribute_id is supplied.");
             }
             int snap_id = args.contains("snapshot_id")
                               ? args["snapshot_id"].get<int>()
                               : -1;
             bool include_ranges = args.contains("include_ranges")
                                       ? args["include_ranges"].get<bool>()
                                       : false;
             ScreenSnapshot snap =
                 server->GetTargetSession(args)->GetSnapshot(snap_id);
             std::string out;
             if (args.contains("attribute_id")) {
                 int attr_id = args["attribute_id"].get<int>();
                 auto unique_attrs = app_utils::GetUniqueAttrs(snap);
                 if (attr_id < 0 ||
                     static_cast<size_t>(attr_id) >= unique_attrs.size()) {
                     throw std::runtime_error("Invalid attribute ID");
                 }
                 CellAttr target_attr = unique_attrs[attr_id];
                 for (unsigned int y = 0; y < snap.height; ++y) {
                     std::vector<std::string> ranges;
                     unsigned int cx = 0;
                     while (cx < snap.width) {
                         if (snap.cells[y * snap.width + cx].attr ==
                             target_attr) {
                             unsigned int x_start = cx;
                             while (cx < snap.width &&
                                    snap.cells[y * snap.width + cx].attr ==
                                        target_attr) {
                                 cx++;
                             }
                             ranges.push_back(std::to_string(x_start) + "-" +
                                              std::to_string(cx - 1));
                         } else {
                             cx++;
                         }
                     }
                     if (!ranges.empty()) {
                         out += "row " + std::to_string(y) + ": col ";
                         for (size_t i = 0; i < ranges.size(); ++i) {
                             if (i > 0) out += ", ";
                             out += ranges[i];
                         }
                         out += "\n";
                     }
                 }
             } else {
                 auto unique_attrs = app_utils::GetUniqueAttrs(snap);
                 for (size_t i = 0; i < unique_attrs.size(); ++i) {
                     out += std::to_string(i) + ": " +
                            app_utils::FormatAttr(unique_attrs[i]) + "\n";
                     if (include_ranges) {
                         CellAttr target_attr = unique_attrs[i];
                         for (unsigned int y = 0; y < snap.height; ++y) {
                             std::vector<std::string> ranges;
                             unsigned int cx = 0;
                             while (cx < snap.width) {
                                 if (snap.cells[y * snap.width + cx].attr ==
                                     target_attr) {
                                     unsigned int x_start = cx;
                                     while (cx < snap.width &&
                                            snap.cells[y * snap.width + cx]
                                                    .attr == target_attr) {
                                         cx++;
                                     }
                                     ranges.push_back(std::to_string(x_start) +
                                                      "-" +
                                                      std::to_string(cx - 1));
                                 } else {
                                     cx++;
                                 }
                             }
                             if (!ranges.empty()) {
                                 out +=
                                     "  row " + std::to_string(y) + ": col ";
                                 for (size_t r = 0; r < ranges.size(); ++r) {
                                     if (r > 0) out += ", ";
                                     out += ranges[r];
                                 }
                                 out += "\n";
                             }
                         }
                     }
                 }
             }
             return {{"content",
                      json::array({{{"type", "text"}, {"text", out}}})}};
         }},
        {"send_key",
         "Send text/characters (including escaped sequences) to the terminal "
         "process.",
         {{"keys",
           {{"type", "string"},
            {"description",
             "The text to send to PTY. Supports backslash escape sequences "
             "like \\n, \\r, \\t, \\\\, \\xNN (hex), and \\NNN (octal)."}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {"keys"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("keys"))
                 throw std::runtime_error("Missing required parameter: keys");
             std::string keys = args.at("keys").get<std::string>();
             server->GetTargetSession(args)->SendRawBytes(
                 Terminal::ParseEscapes(keys));
             return {{"content", json::array({{{"type", "text"},
                                               {"text", "Keys sent."}}})}};
         }},
        {"send_special_key",
         "Send a special key (e.g. arrows, enter, escape, backspace) with "
         "optional modifiers.",
         {{"keyname",
           {{"type", "string"},
            {"description",
             "The key name (e.g. \"up\", \"down\", \"left\", \"right\", "
             "\"enter\", \"escape\", \"backspace\", \"tab\")"}}},
          {"modifiers",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             "Optional list of modifier keys (e.g. \"ctrl\", \"shift\", "
             "\"alt\")"}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {"keyname"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("keyname"))
                 throw std::runtime_error(
                     "Missing required parameter: keyname");
             std::string keyname = args.at("keyname").get<std::string>();
             uint32_t keysym = Terminal::ParseKeysym(keyname);
             if (keysym == 0) {
                 throw std::runtime_error("Unknown keyname: " + keyname);
             }
             unsigned int mods = 0;
             if (args.contains("modifiers")) {
                 std::vector<std::string> mod_list =
                     args["modifiers"].get<std::vector<std::string>>();
                 mods = Terminal::ParseMods(mod_list, 0);
             }
             server->GetTargetSession(args)->SendKey(keysym, mods);
             return {
                 {"content", json::array({{{"type", "text"},
                                           {"text", "Special key sent."}}})}};
         }},
        {"send_signal",
         "Send a POSIX signal to the child terminal process.",
         {{"signal",
           {{"type", "integer"},
            {"description", "POSIX signal number (default: 15 = SIGTERM)"}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {},
         [](McpServer* server, const json& args) -> json {
             int sig =
                 args.contains("signal") ? args["signal"].get<int>() : 15;
             auto t = server->GetTargetSession(args);
             if (t->IsExited()) {
                 throw std::runtime_error("Process already exited.");
             } else {
                 t->SendSignal(sig);
             }
             return {{"content",
                      json::array({{{"type", "text"},
                                    {"text", "Signal " + std::to_string(sig) +
                                                 " sent."}}})}};
         }},
        {"get_status",
         "Check whether the child subprocess is running or exited.",
         {{"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {},
         [](McpServer* server, const json& args) -> json {
             std::string status_str;
             auto t = server->GetTargetSession(args);
             if (t->IsExited()) {
                 status_str = "exited " + std::to_string(t->ExitStatus());
             } else {
                 status_str = "running";
             }
             return {{"content", json::array({{{"type", "text"},
                                               {"text", status_str}}})}};
         }},
        {"wait_idle",
         "Wait until the screen is idle (quiet) for a specific duration or "
         "until a deadline is reached.",
         {{"quiet_ms",
           {{"type", "integer"},
            {"description",
             "Minimum duration in milliseconds the terminal must be idle"}}},
          {"deadline_ms",
           {{"type", "integer"},
            {"description", "Maximum milliseconds to wait overall"}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {"quiet_ms", "deadline_ms"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("quiet_ms"))
                 throw std::runtime_error(
                     "Missing required parameter: quiet_ms");
             if (!args.contains("deadline_ms"))
                 throw std::runtime_error(
                     "Missing required parameter: deadline_ms");
             int quiet_ms = args.at("quiet_ms").get<int>();
             int deadline_ms = args.at("deadline_ms").get<int>();
             if (quiet_ms < 0 || deadline_ms < 0) {
                 throw std::runtime_error("Times must be non-negative.");
             }
             auto t = server->GetTargetSession(args);
             termobulator::unstable::WaitResult wait_res =
                 t->WaitIdle(quiet_ms, deadline_ms);
             std::string result_str;
             if (wait_res == termobulator::unstable::WaitResult::kIdle) {
                 result_str = "wait: idle";
             } else if (wait_res ==
                        termobulator::unstable::WaitResult::kDeadline) {
                 result_str = "wait: deadline";
             } else if (wait_res ==
                        termobulator::unstable::WaitResult::kExited) {
                 result_str = "wait: exited";
             }
             return {{"content", json::array({{{"type", "text"},
                                               {"text", result_str}}})}};
         }},
        {"wait_for_text",
         "Wait until a specific string is found on the screen, up to a "
         "deadline.",
         {{"text",
           {{"type", "string"},
            {"description", "The text query string to wait for"}}},
          {"deadline_ms",
           {{"type", "integer"},
            {"description", "Maximum milliseconds to wait"}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {"text", "deadline_ms"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("text"))
                 throw std::runtime_error("Missing required parameter: text");
             if (!args.contains("deadline_ms"))
                 throw std::runtime_error(
                     "Missing required parameter: deadline_ms");
             std::string text = args.at("text").get<std::string>();
             std::string query = Terminal::ParseEscapes(text);
             int deadline_ms = args.at("deadline_ms").get<int>();
             if (deadline_ms < 0) {
                 throw std::runtime_error("Deadline must be non-negative.");
             }

             auto t = server->GetTargetSession(args);
             auto start_time = std::chrono::steady_clock::now();
             auto d_dur = std::chrono::milliseconds(deadline_ms);
             std::string result_str = "wait-for-text: timeout";

             while (true) {
                 if (app_utils::IsTextOnScreen(t.get(), query)) {
                     result_str = "wait-for-text: found";
                     break;
                 }
                 if (t->IsExited()) {
                     result_str = "wait-for-text: exited";
                     break;
                 }
                 auto now = std::chrono::steady_clock::now();
                 auto elapsed =
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - start_time);
                 if (elapsed >= d_dur) {
                     result_str = "wait-for-text: timeout";
                     break;
                 }
                 unsigned int remain = deadline_ms - elapsed.count();
                 unsigned int wait_ms = std::min(10u, remain);
                 t->WaitIdle(std::min(5u, wait_ms), wait_ms);
             }
             return {{"content", json::array({{{"type", "text"},
                                               {"text", result_str}}})}};
         }},
        {"find_text",
         "Locate all occurrences of a string on the screen. Note: search is "
         "performed per-row. Text that wraps across multiple rows will not be "
         "matched.",
         {{"text",
           {{"type", "string"},
            {"description", "The text query string to search for"}}},
          {"snapshot_id",
           {{"type", "integer"},
            {"description", "Optional snapshot ID to query."}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {"text"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("text")) {
                 throw std::runtime_error("Missing required parameter: text");
             }
             std::string query_raw = args.at("text").get<std::string>();
             std::string query = Terminal::ParseEscapes(query_raw);
             if (query.empty()) {
                 throw std::runtime_error("Empty search query");
             }
             int snap_id = args.contains("snapshot_id")
                               ? args["snapshot_id"].get<int>()
                               : -1;
             ScreenSnapshot snap =
                 server->GetTargetSession(args)->GetSnapshot(snap_id);

             json results = json::array();
             for (unsigned int y = 0; y < snap.height; ++y) {
                 std::string row_str;
                 std::vector<unsigned int> char_to_cell;
                 for (unsigned int x = 0; x < snap.width; ++x) {
                     const std::string& ch = snap.cells[y * snap.width + x].ch;
                     for (size_t i = 0; i < ch.size(); ++i) {
                         char_to_cell.push_back(x);
                     }
                     row_str += ch;
                 }

                 size_t pos = row_str.find(query, 0);
                 while (pos != std::string::npos) {
                     unsigned int col_start = char_to_cell[pos];
                     unsigned int col_end =
                         char_to_cell[pos + query.size() - 1];

                     json item;
                     item["row"] = y;
                     item["col_start"] = col_start;
                     item["col_end"] = col_end;
                     results.push_back(item);

                     pos = row_str.find(query, pos + 1);
                 }
             }
             return {{"content", json::array({{{"type", "text"},
                                               {"text", results.dump()}}})}};
         }},
        {"take_snapshot",
         "Capture a snapshot of the current screen state and return a "
         "snapshot ID.",
         {{"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {},
         [](McpServer* server, const json& args) -> json {
             int id = server->GetTargetSession(args)->Snapshot();
             json snap_json = {{"snapshot_id", id}};
             return {{"content", json::array({{{"type", "text"},
                                               {"text", snap_json.dump()}}})}};
         }},
        {"get_diff",
         "Compare two screen snapshots or compare a snapshot to the current "
         "screen.",
         {{"snapshot_id_a",
           {{"type", "integer"}, {"description", "First snapshot ID"}}},
          {"snapshot_id_b",
           {{"type", "integer"},
            {"description",
             "Optional second snapshot ID. Use -1 or omit to compare with the "
             "current screen."}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {"snapshot_id_a"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("snapshot_id_a"))
                 throw std::runtime_error(
                     "Missing required parameter: snapshot_id_a");
             int snap_a = args.at("snapshot_id_a").get<int>();
             int snap_b = args.contains("snapshot_id_b")
                              ? args["snapshot_id_b"].get<int>()
                              : -1;
             auto t = server->GetTargetSession(args);
             ScreenSnapshot curr = t->GetSnapshot(snap_b);
             ScreenSnapshot prev = t->GetSnapshot(snap_a);

             if (curr.width != prev.width || curr.height != prev.height) {
                 throw std::runtime_error(
                     "Snapshot dimensions mismatch. Snapshots before and "
                     "after resize are incompatible.");
             }

             std::string out;
             for (unsigned int y = 0; y < curr.height; ++y) {
                 unsigned int cx = 0;
                 while (cx < curr.width) {
                     if (curr.cells[y * curr.width + cx].ch !=
                         prev.cells[y * prev.width + cx].ch) {
                         unsigned int x_start = cx;
                         std::string old_str;
                         std::string new_str;
                         while (cx < curr.width && cx < prev.width &&
                                curr.cells[y * curr.width + cx].ch !=
                                    prev.cells[y * prev.width + cx].ch) {
                             old_str += prev.cells[y * prev.width + cx].ch;
                             new_str += curr.cells[y * curr.width + cx].ch;
                             cx++;
                         }
                         out += "row " + std::to_string(y) + " col " +
                                std::to_string(x_start) + "-" +
                                std::to_string(cx - 1) + ": \"" + old_str +
                                "\" -> \"" + new_str + "\"\n";
                     } else {
                         cx++;
                     }
                 }
             }
             return {{"content",
                      json::array({{{"type", "text"}, {"text", out}}})}};
         }},
        {"resize_terminal",
         "Resize the terminal emulator width and height.",
         {{"width",
           {{"type", "integer"},
            {"description", "New terminal width (columns)"}}},
          {"height",
           {{"type", "integer"},
            {"description", "New terminal height (rows)"}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {"width", "height"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("width"))
                 throw std::runtime_error("Missing required parameter: width");
             if (!args.contains("height"))
                 throw std::runtime_error(
                     "Missing required parameter: height");
             int w = args.at("width").get<int>();
             int h = args.at("height").get<int>();
             if (w <= 0 || h <= 0) {
                 throw std::runtime_error("Dimensions must be positive.");
             }
             server->GetTargetSession(args)->Resize(w, h);
             return {{"content",
                      json::array(
                          {{{"type", "text"},
                            {"text", "Resized to " + std::to_string(w) + "x" +
                                         std::to_string(h) + "."}}})}};
         }},
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
             "Optional terminal type (e.g. \"xterm-256color\", default: "
             "terminal type specified at server start)."}}},
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
             "Optional maximum number of scrollback lines to retain (default: "
             "200)."}}}},
         {"binary"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("binary"))
                 throw std::runtime_error(
                     "Missing required parameter: binary");
             std::string binary = args.at("binary").get<std::string>();
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
             std::string loc = args.contains("locale")
                                   ? args["locale"].get<std::string>()
                                   : server->locale_;
             std::shared_ptr<termobulator::unstable::Terminal> term_obj =
                 CreateSubprocessTerminal(w, h, binary, cmd_args, term, loc,
                                          sb_size);
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
             json response_json = {{"session_id", sess_id}};
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
        {"get_scrollback",
         "Retrieve the scrollback history buffer for a terminal session.",
         {{"lines",
           {{"type", "integer"},
            {"description", "The number of scrollback lines to retrieve."}}},
          {"format",
           {{"type", "string"},
            {"enum", {"text", "lines"}},
            {"description",
             "Output format, either 'text' (default) or 'lines'."}}},
          {"session_id",
           {{"type", "string"},
            {"description", "Optional session ID to target."}}}},
         {"lines"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("lines")) {
                 throw std::runtime_error("Missing required parameter: lines");
             }
             int lines = args.at("lines").get<int>();
             if (lines <= 0) {
                 throw std::runtime_error("Lines must be positive.");
             }
             std::string format = "text";
             if (args.contains("format")) {
                 format = args.at("format").get<std::string>();
             }
             auto term = server->GetTargetSession(args);
             std::vector<std::string> sb = term->GetScrollback(lines);

             if (format == "lines") {
                 return {
                     {"content", json::array({{{"type", "text"},
                                               {"text", json(sb).dump()}}})}};
             } else {
                 std::string text;
                 for (const auto& line : sb) {
                     text += line + "\n";
                 }
                 return {{"content",
                          json::array({{{"type", "text"}, {"text", text}}})}};
             }
         }},
        {"get_area",
         "Retrieve characters and/or attributes from a rectangular area on "
         "the screen.",
         {{"x",
           {{"type", "integer"},
            {"description",
             "Column index (0-based) of the top-left corner."}}},
          {"y",
           {{"type", "integer"},
            {"description", "Row index (0-based) of the top-left corner."}}},
          {"width",
           {{"type", "integer"},
            {"description", "Width of the area in characters."}}},
          {"height",
           {{"type", "integer"},
            {"description", "Height of the area in characters."}}},
          {"format",
           {{"type", "string"},
            {"enum", {"text", "lines"}},
            {"description",
             "Output format for text, either 'text' (default) or 'lines'."}}},
          {"include_text",
           {{"type", "boolean"},
            {"description",
             "Include the text content of the area (default: true)."}}},
          {"include_attrs",
           {{"type", "boolean"},
            {"description",
             "Include the attribute map of the area (default: false)."}}},
          {"snapshot_id",
           {{"type", "integer"},
            {"description",
             "Optional ID of a snapshot to read from. Use -1 or omit for the "
             "current active screen."}}},
          {"session_id",
           {{"type", "string"},
            {"description", "Optional session ID to target."}}}},
         {"x", "y", "width", "height"},
         [](McpServer* server, const json& args) -> json {
             if (!args.contains("x") || !args.contains("y") ||
                 !args.contains("width") || !args.contains("height")) {
                 throw std::runtime_error(
                     "Missing required parameters: x, y, width, height");
             }
             int x = args.at("x").get<int>();
             int y = args.at("y").get<int>();
             int w = args.at("width").get<int>();
             int h = args.at("height").get<int>();
             if (w <= 0 || h <= 0) {
                 throw std::runtime_error(
                     "Width and height must be positive.");
             }

             std::string format = "text";
             if (args.contains("format")) {
                 format = args.at("format").get<std::string>();
             }
             bool include_text = true;
             if (args.contains("include_text")) {
                 include_text = args.at("include_text").get<bool>();
             }
             bool include_attrs = false;
             if (args.contains("include_attrs")) {
                 include_attrs = args.at("include_attrs").get<bool>();
             }
             int snap_id = args.contains("snapshot_id")
                               ? args.at("snapshot_id").get<int>()
                               : -1;

             ScreenSnapshot snap =
                 server->GetTargetSession(args)->GetSnapshot(snap_id);

             bool clipped = false;
             if (x < 0 || y < 0 || x + w > static_cast<int>(snap.width) ||
                 y + h > static_cast<int>(snap.height)) {
                 clipped = true;
             }

             int x1 = std::max(0, x);
             int y1 = std::max(0, y);
             int x2 = std::min(static_cast<int>(snap.width) - 1, x + w - 1);
             int y2 = std::min(static_cast<int>(snap.height) - 1, y + h - 1);

             json response_obj = json::object();

             if (include_text) {
                 if (x1 > x2 || y1 > y2) {
                     if (format == "lines") {
                         response_obj["text"] = json::array();
                     } else {
                         response_obj["text"] = "";
                     }
                 } else {
                     if (format == "lines") {
                         json lines_arr = json::array();
                         for (int row = y1; row <= y2; ++row) {
                             std::string row_str;
                             for (int col = x1; col <= x2; ++col) {
                                 row_str +=
                                     snap.cells[row * snap.width + col].ch;
                             }
                             lines_arr.push_back(row_str);
                         }
                         response_obj["text"] = lines_arr;
                     } else {
                         std::string text_str;
                         for (int row = y1; row <= y2; ++row) {
                             if (row > y1) text_str += "\n";
                             for (int col = x1; col <= x2; ++col) {
                                 text_str +=
                                     snap.cells[row * snap.width + col].ch;
                             }
                         }
                         response_obj["text"] = text_str;
                     }
                 }
             }

             if (include_attrs) {
                 json attrs_arr = json::array();
                 if (!(x1 > x2 || y1 > y2)) {
                     for (int row = y1; row <= y2; ++row) {
                         json row_attrs = json::array();
                         for (int col = x1; col <= x2; ++col) {
                             row_attrs.push_back(app_utils::FormatAttr(
                                 snap.cells[row * snap.width + col].attr));
                         }
                         attrs_arr.push_back(row_attrs);
                     }
                 }
                 response_obj["attributes"] = attrs_arr;
             }

             if (clipped) {
                 response_obj["warning"] =
                     "Requested area was clipped by the actual screen "
                     "dimensions (" +
                     std::to_string(snap.width) + "x" +
                     std::to_string(snap.height) + ").";
             }

             return {
                 {"content", json::array({{{"type", "text"},
                                           {"text", response_obj.dump()}}})}};
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

}  // namespace termobulator
