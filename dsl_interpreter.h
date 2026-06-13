// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_DSL_INTERPRETER_H
#define TERMOBULATOR_DSL_INTERPRETER_H

#include <nlohmann/json.hpp>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "termobulator.h"

namespace termobulator {
namespace unstable {

class Interpreter {
  public:
    explicit Interpreter(Terminal* term);

    // Executes a JSON array of instructions/literals.
    // Returns the serialized final stack state upon success.
    // Throws std::runtime_error on any VM error (halting execution).
    nlohmann::json Execute(const nlohmann::json& instructions);

    // Helper to execute text input (CLI), parsing it to JSON internally.
    nlohmann::json ExecuteText(std::string_view text);

    // Accessors for debugging and state management.
    const std::vector<nlohmann::json>& GetStack() const { return stack_; }
    const std::unordered_map<std::string, nlohmann::json>& GetVariables()
        const {
        return variables_;
    }
    void ClearStack() { stack_.clear(); }
    void SetStack(const std::vector<nlohmann::json>& stack) { stack_ = stack; }

    // Stack helper methods
    void Push(nlohmann::json val);
    nlohmann::json Pop();

  private:
    void ExecuteToken(const nlohmann::json& token);

    int PopInt();
    bool PopBool();
    std::string PopString();
    nlohmann::json PopArray();

    Terminal* term_;
    std::vector<nlohmann::json> stack_;
    std::unordered_map<std::string, nlohmann::json> variables_;
};

nlohmann::json ParseTextToDsl(std::string_view text);

}  // namespace unstable
}  // namespace termobulator

#endif  // TERMOBULATOR_DSL_INTERPRETER_H
