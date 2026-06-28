// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_LUA_EXECUTOR_H
#define TERMOBULATOR_LUA_EXECUTOR_H

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "termobulator.h"

namespace termobulator {
namespace unstable {

class LuaExecutor {
  public:
    explicit LuaExecutor(Terminal* term);

    struct Result {
        nlohmann::json result;         // return value(s) of the chunk
        std::vector<std::string> log;  // log() messages
        nlohmann::json variables;      // full post-execution store snapshot
    };

    // Compiles and runs `script` in a fresh lua_State.
    // On success: merges surviving globals into the store, returns Result.
    // On error: throws std::runtime_error with traceback; store is untouched.
    Result Execute(std::string_view script);

    // Accessors for testing and state management
    const std::unordered_map<std::string, nlohmann::json>& GetVariables()
        const {
        return store_;
    }

  private:
    Terminal* term_;
    std::unordered_map<std::string, nlohmann::json> store_;
};

}  // namespace unstable
}  // namespace termobulator

#endif  // TERMOBULATOR_LUA_EXECUTOR_H
