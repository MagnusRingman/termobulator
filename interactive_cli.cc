// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "interactive_cli.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "dsl_interpreter.h"

namespace termobulator {

using termobulator::unstable::CreateSubprocessTerminal;
using termobulator::unstable::Interpreter;

InteractiveCli::InteractiveCli(unsigned int width, unsigned int height,
                               const std::string& cmd,
                               const std::vector<std::string>& args,
                               const std::string& term_type,
                               const std::string& locale)
        : width_(width),
          height_(height),
          cmd_(cmd),
          args_(args),
          term_type_(term_type),
          locale_(locale) {}

int InteractiveCli::Run() {
    term_ = CreateSubprocessTerminal(width_, height_, cmd_, args_, term_type_,
                                     locale_);
    Interpreter interpreter(term_.get());

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "exit" || line == "quit") {
            break;
        }
        try {
            nlohmann::json stack_state = interpreter.ExecuteText(line);
            std::cout << stack_state.dump() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }

    return term_->ExitStatus();
}

}  // namespace termobulator
