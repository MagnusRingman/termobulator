// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_INTERACTIVE_CLI_H
#define TERMOBULATOR_INTERACTIVE_CLI_H

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "termobulator.h"

namespace termobulator {

class InteractiveCli {
  public:
    InteractiveCli(unsigned int width, unsigned int height,
                   const std::string& cmd,
                   const std::vector<std::string>& args,
                   const std::string& term_type = "tmux-256color",
                   const std::string& locale = "");

    int Run();

  private:
    struct Command {
        std::function<bool(const std::string&)> handler;
        std::string help_text;
    };

    void SetupCommands();

    bool HandleAttrMap(const std::string& arg);
    bool HandleAttributes(const std::string& arg);
    bool HandleCell(const std::string& arg);
    bool HandleCursor(const std::string& arg);
    bool HandleDiff(const std::string& arg);
    bool HandleExit(const std::string& arg);
    bool HandleFind(const std::string& arg);
    bool HandleHelp(const std::string& arg);
    bool HandleKeysyms(const std::string& arg);
    bool HandleKey(const std::string& arg);
    bool HandleKill(const std::string& arg);
    bool HandleRange(const std::string& arg);
    bool HandleResize(const std::string& arg);
    bool HandleRow(const std::string& arg);
    bool HandleScreen(const std::string& arg);
    bool HandleSnapshot(const std::string& arg);
    bool HandleSpecial(const std::string& arg);
    bool HandleStatus(const std::string& arg);
    bool HandleWait(const std::string& arg);
    bool HandleWaitForText(const std::string& arg);
    bool HandleScreenRaw(const std::string& arg);
    bool HandleScrollback(const std::string& arg);

    unsigned int width_;
    unsigned int height_;
    std::string cmd_;
    std::vector<std::string> args_;
    std::string term_type_;
    std::string locale_;
    std::unique_ptr<termobulator::unstable::Terminal> term_;
    std::map<std::string, Command> commands_;
};

}  // namespace termobulator

#endif  // TERMOBULATOR_INTERACTIVE_CLI_H
