// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "mcp_server.h"

#include <unistd.h>

#include <chrono>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "terminal_levels.h"

namespace termobulator {

using json = nlohmann::json;
using termobulator::unstable::CreateSubprocessTerminal;
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
    resp["result"]["serverInfo"]["version"] = "1.1.0";
    resp["result"]["instructions"] =
        "This server manages terminal sessions and provides tools to interact "
        "with them.\n"
        "The `execute_dsl` tool executes a series of instructions on a "
        "session's persistent stack machine.\n\n"
        "### DSL Conceptual Model\n"
        "- **Persistent State**: Each terminal session maintains a stack and "
        "a variable dictionary between `execute_dsl` calls.\n"
        "- **Literals**: Values of types integer, boolean, array, and object "
        "are pushed directly onto the stack.\n"
        "- **Structured Literals**: Pushing {\"lit\": <value>} directly "
        "pushes <value> (e.g. {\"lit\": \"f4\"} or {\"lit\": \"\"}) without "
        "evaluating it or needing escapes.\n"
        "- **Structured Operations**: Pushing {\"op\": \"<op_name>\", "
        "\"args\": [<lit_args>...]} evaluates the operation with the given "
        "literal arguments in order, avoiding stack-shuffling.\n"
        "- **Escape Prefix**: In a JSON array, prefixing a string with '$' "
        "(e.g. '$myvar') pushes it as a literal string. In a text string, "
        "wrap it in double quotes (e.g. \\\"myvar\\\").\n"
        "- **Operations**: Unprefixed/unquoted strings are executed as "
        "operations (verbs) that pop arguments and push results.\n"
        "- **Fail-Fast & Auto-Clear**: Any error (stack underflow, type "
        "mismatch, invalid operation) aborts execution immediately, returning "
        "a detailed error message, and automatically clears the stack of the "
        "session.\n"
        "- **Opaque Snapshots**: A snapshot is an immutable entity stored in "
        "the session registry and referenced on the stack as an integer "
        "snapshot ID.\n\n"
        "### DSL Available Operations\n"
        "- `dup` `( x -- x x )`: Duplicate top item.\n"
        "- `dup2` `( x y -- x y x y )`: Duplicate top two items.\n"
        "- `drop` `( x -- )`: Discard top item.\n"
        "- `swap` `( x y -- y x )`: Swap top two items.\n"
        "- `over` `( x y -- x y x )`: Duplicate second item to top.\n"
        "- `rot` `( x y z -- y z x )`: Rotate top three items.\n"
        "- `clear` `( ... -- )`: Discard all stack elements.\n"
        "- `store` `( val name_str -- )`: Bind value to variable name.\n"
        "- `load` `( name_str -- val )`: Retrieve value of variable name.\n"
        "- `exec` `( q -- ... )`: Execute quotation/array `q`.\n"
        "- `if` `( cond true_q false_q -- ... )`: If cond is non-zero/true "
        "execute true_q, else false_q.\n"
        "- `dip` `( x q -- ... x )`: Pop x, execute q, restore x.\n"
        "- `while` `( cond_q body_q -- ... )`: Loop body_q while cond_q "
        "returns true/non-zero.\n"
        "- `not` `( x -- bool )`: Logical negation.\n"
        "- `equal` `( x y -- bool )`: Check equality.\n"
        "- `empty` `( val -- bool )`: Check if string/array/object is empty.\n"
        "- `size` `( val -- int )`: Length of string/array/object.\n"
        "- `sleep_ms` `( ms_int -- )`: Sleep/delay execution for ms_int "
        "milliseconds.\n"
        "- `send_key` `( keys_str -- )`: Send key string (supports escapes "
        "like \\n, \\t, \\xNN).\n"
        "- `send_special_key` `( keyname_str modifiers_str -- )`: Send "
        "special key with comma-separated modifiers (e.g. 'ctrl,alt' or '').\n"
        "  * **Supported Key Names**: `up`, `down`, `left`, `right`, `f1` to "
        "`f20`, `backspace`, `tab`, `enter`/`return`, `escape`/`esc`, "
        "`insert`, `delete`/`del`, `home`, `end`, `pageup`/`pgup`, "
        "`pagedown`/`pgdn`, `space`, and keypad keys (e.g. `kp_enter`).\n"
        "  * **Supported Modifiers**: `shift`, `ctrl`/`control`, "
        "`alt`/`meta`.\n"
        "- `send_signal` `( sig_int -- )`: Send POSIX signal to child "
        "process.\n"
        "- `get_status` `( -- status_str )`: Push status ('running' or "
        "'exited <code_int>').\n"
        "- `wait_idle` `( quiet_ms deadline_ms -- result_str )`: Wait for "
        "terminal to be idle. Pushes 'wait: idle', 'wait: deadline', or "
        "'wait: exited'.\n"
        "- `wait_for_text` `( text_str deadline_ms -- result_str )`: Wait for "
        "text to appear. Pushes 'wait-for-text: found', 'wait-for-text: "
        "timeout', or 'wait-for-text: exited'.\n"
        "- `take_snapshot` `( -- snapshot_id )`: Capture screen state, push "
        "ID.\n"
        "- `get_screen` `( snapshot_id -- screen_str )`: Get screen text of "
        "snapshot.\n"
        "- `get_cursor` `( snapshot_id -- col_int row_int visible_bool )`: "
        "Push cursor column, row, and visibility.\n"
        "- `get_cell` `( x_int y_int snapshot_id -- cell_obj )`: Push cell "
        "JSON object (char, fg, bg, and attributes).\n"
        "- `get_row` `( row_int snapshot_id -- row_str )`: Push text of "
        "single row.\n"
        "- `get_attributes` `( snapshot_id -- attr_list_str )`: Push list of "
        "unique formatting attributes.\n"
        "- `find_text` `( query_str snapshot_id -- results_arr )`: Find "
        "query, push array of `{\"row\": y, \"col_start\": x, \"col_end\": "
        "x}`.\n"
        "- `get_diff` `( snapshot_id_b snapshot_id_a -- diff_str )`: Diff "
        "snapshots, older first. Pushes text summary of difference.\n\n"
        "### Terminal Capability Levels\n"
        "The `terminal` parameter on `create_session` controls what terminal "
        "capabilities the launched application sees. The default is "
        "`\"full\"` "
        "(xterm-256color). Use lower levels like `\"basic\"` (vt100) or "
        "`\"extended\"` (vt220) to test degraded-terminal behaviour. Color "
        "depth can be controlled independently by appending a suffix: "
        "`\"full-8color\"`, `\"basic-16color\"`, etc. Read the "
        "`termobulator://docs/terminal-levels` resource for the full "
        "reference.";
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

std::shared_ptr<termobulator::unstable::Interpreter> McpServer::GetInterpreter(
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

    auto it_interp = interpreters_.find(sess_id);
    if (it_interp == interpreters_.end()) {
        auto interp = std::make_shared<termobulator::unstable::Interpreter>(
            it_term->second.get());
        interpreters_[sess_id] = interp;
        return interp;
    }
    return it_interp->second;
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
    auto it_interp = interpreters_.find(sess_id);
    if (it_interp != interpreters_.end()) {
        interpreters_.erase(it_interp);
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
         "Execute a series of DSL instructions on the session's persistent "
         "stack machine to interact with or inspect the terminal. See "
         "initialize "
         "instructions for the full language syntax and operators list.",
         {{"instructions",
           {{"type", "array"},
            {"items", json::object()},
            {"description",
             "A JSON array of DSL instructions and/or literals, or a "
             "space-separated string of instructions."}}},
          {"session_id",
           {{"type", "string"},
            {"description",
             "Optional session ID to target. If omitted, targets the "
             "currently active session."}}}},
         {"instructions"},
         [](McpServer* server, const json& args) -> json {
             auto interp = server->GetInterpreter(args);
             json instrs = args.at("instructions");
             json final_stack;
             if (instrs.is_string()) {
                 final_stack = interp->ExecuteText(instrs.get<std::string>());
             } else if (instrs.is_array()) {
                 final_stack = interp->Execute(instrs);
             } else {
                 throw std::runtime_error(
                     "instructions must be either a JSON array or a string");
             }
             return {
                 {"content", json::array({{{"type", "text"},
                                           {"text", final_stack.dump()}}})}};
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
    resp["result"]["resources"] =
        json::array({{{"uri", "termobulator://docs/terminal-levels"},
                      {"name", "Terminal Capability Levels"},
                      {"mimeType", "text/markdown"}}});
    SendJson(resp);
}

void McpServer::HandleResourcesRead(const json& id, const json& req) {
    if (!req.contains("params") || !req["params"].contains("uri")) {
        SendJsonRpcError(id, -32602, "Invalid params: missing uri");
        return;
    }
    std::string uri = req["params"]["uri"].get<std::string>();
    if (uri != "termobulator://docs/terminal-levels") {
        SendJsonRpcError(id, -32002, "Resource not found: " + uri);
        return;
    }
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"]["contents"] =
        json::array({{{"uri", uri},
                      {"mimeType", "text/markdown"},
                      {"text", GetTerminalLevelsDocumentation()}}});
    SendJson(resp);
}

}  // namespace termobulator
