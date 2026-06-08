// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include <signal.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <vector>

#include "interactive_cli.h"
#include "mcp_server.h"

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    auto parse_positive = [](const char* s, const char* name) -> int {
        try {
            size_t pos = 0;
            int val = std::stoi(s, &pos);
            if (pos != std::string(s).size() || val <= 0) {
                throw std::runtime_error("");
            }
            return val;
        } catch (...) {
            throw std::runtime_error(std::string("invalid ") + name + ": " +
                                     s);
        }
    };

    unsigned int width = 80;
    unsigned int height = 24;
    std::string term_type = "tmux-256color";
    std::string locale = "";
    bool mcp_mode = false;
    int arg_idx = 1;
    while (arg_idx < argc) {
        std::string arg = argv[arg_idx];
        if (arg == "--width" || arg == "-w") {
            if (arg_idx + 1 >= argc) {
                std::cerr << "Error: --width requires an argument\n";
                return 1;
            }
            try {
                width = static_cast<unsigned int>(
                    parse_positive(argv[arg_idx + 1], "width"));
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
                return 1;
            }
            arg_idx += 2;
        } else if (arg == "--height" || arg == "-h") {
            if (arg_idx + 1 >= argc) {
                std::cerr << "Error: --height requires an argument\n";
                return 1;
            }
            try {
                height = static_cast<unsigned int>(
                    parse_positive(argv[arg_idx + 1], "height"));
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
                return 1;
            }
            arg_idx += 2;
        } else if (arg == "--terminal" || arg == "-t") {
            if (arg_idx + 1 >= argc) {
                std::cerr << "Error: --terminal requires an argument\n";
                return 1;
            }
            term_type = argv[arg_idx + 1];
            arg_idx += 2;
        } else if (arg == "--locale" || arg == "-l") {
            if (arg_idx + 1 >= argc) {
                std::cerr << "Error: --locale requires an argument\n";
                return 1;
            }
            locale = argv[arg_idx + 1];
            arg_idx += 2;
        } else if (arg == "--mcp" || arg == "-m") {
            mcp_mode = true;
            arg_idx += 1;
        } else {
            break;
        }
    }

    std::string cmd;
    std::vector<std::string> args;

    if (mcp_mode) {
        if (arg_idx < argc) {
            std::cerr << "Error: Target binary and arguments are not allowed "
                         "in MCP mode.\n";
            std::cerr << "Usage in MCP mode: " << argv[0]
                      << " [--mcp|-m] [--width <width>] [--height <height>] "
                         "[--terminal <term>] [--locale <locale>]\n";
            return 1;
        }
        termobulator::McpServer server(width, height, term_type, locale);
        return server.Run();
    } else {
        if (arg_idx >= argc) {
            std::cerr << "Usage: " << argv[0]
                      << " [--width <width>] [--height <height>] [--terminal "
                         "<term>] [--locale <locale>] <binary> [args...]\n";
            return 1;
        }
        cmd = argv[arg_idx];
        for (int i = arg_idx + 1; i < argc; ++i) {
            args.push_back(argv[i]);
        }
        termobulator::InteractiveCli cli(width, height, cmd, args, term_type,
                                         locale);
        return cli.Run();
    }
}