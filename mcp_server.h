// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_MCP_SERVER_H
#define TERMOBULATOR_MCP_SERVER_H

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "termobulator.h"

namespace termobulator {

class McpServer {
  public:
    McpServer(unsigned int width, unsigned int height,
              const std::string& term_type = "tmux-256color",
              const std::string& locale = "");

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
    json ExecuteTool(const std::string& name, const json& args);
    void SendJsonRpcError(const json& id, int code,
                          const std::string& message);
    termobulator::unstable::Terminal* GetTargetSession(const json& args);

    unsigned int width_;
    unsigned int height_;
    std::string term_type_;
    std::string locale_;

    bool initialized_ = false;
    bool initialize_received_ = false;
    std::unordered_map<std::string,
                       std::unique_ptr<termobulator::unstable::Terminal>>
        sessions_;
    std::string active_session_id_;
    std::string first_session_id_;
    int first_session_exit_code_ = 0;
    int next_session_number_ = 1;
};

}  // namespace termobulator

#endif  // TERMOBULATOR_MCP_SERVER_H
