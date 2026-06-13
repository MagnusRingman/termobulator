// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_INTERACTIVE_CLI_H
#define TERMOBULATOR_INTERACTIVE_CLI_H

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
    unsigned int width_;
    unsigned int height_;
    std::string cmd_;
    std::vector<std::string> args_;
    std::string term_type_;
    std::string locale_;
    std::unique_ptr<termobulator::unstable::Terminal> term_;
};

}  // namespace termobulator

#endif  // TERMOBULATOR_INTERACTIVE_CLI_H
