// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "termobulator.h"
using termobulator::unstable::CellAttr;
using termobulator::unstable::CreateSubprocessTerminal;
using termobulator::unstable::ScreenSnapshot;
using termobulator::unstable::Terminal;

class TerminalApp {
  public:
    TerminalApp(unsigned int width, unsigned int height,
                const std::string& cmd, const std::vector<std::string>& args,
                const std::string& term_type = "tmux-256color",
                const std::string& locale = "")
            : width_(width),
              height_(height),
              cmd_(cmd),
              args_(args),
              term_type_(term_type),
              locale_(locale) {}

    int Run() {
        term_ = CreateSubprocessTerminal(width_, height_, cmd_, args_,
                                         term_type_, locale_);
        SetupCommands();

        std::string line;
        while (std::getline(std::cin, line)) {
            auto [cmd, arg] = SplitCommand(line);
            auto it = commands_.find(cmd);
            if (it != commands_.end()) {
                try {
                    if (!it->second.handler(arg)) {
                        break;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error: " << e.what() << "\n";
                }
            } else {
                std::cout << "Unknown command. Use 'help' to list available "
                             "commands.\n";
            }
        }

        return term_->ExitStatus();
    }

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
    bool IsTextOnScreen(const std::string& query);

    static std::pair<std::string, std::string> SplitCommand(
        const std::string& line) {
        size_t space = line.find(' ');
        if (space == std::string::npos) {
            return {line, ""};
        }
        return {line.substr(0, space), line.substr(space + 1)};
    }

    static std::vector<std::string> ParseArgs(const std::string& arg_str) {
        std::vector<std::string> args;
        std::string current;
        bool in_quotes = false;
        bool has_seen_quotes = false;
        for (size_t i = 0; i < arg_str.size(); ++i) {
            char c = arg_str[i];
            if (c == '"') {
                in_quotes = !in_quotes;
                has_seen_quotes = true;
            } else if (c == ' ' && !in_quotes) {
                if (!current.empty() || has_seen_quotes) {
                    args.push_back(current);
                    current.clear();
                    has_seen_quotes = false;
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty() || has_seen_quotes) {
            args.push_back(current);
        }
        return args;
    }

    static int ParseInt(const std::string& str, const std::string& name) {
        try {
            size_t pos = 0;
            int val = std::stoi(str, &pos);
            if (pos != str.size()) {
                throw std::runtime_error("invalid " + name + ": " + str);
            }
            return val;
        } catch (...) {
            throw std::runtime_error("invalid " + name + ": " + str);
        }
    }

    static std::string FormatColor(int code, uint8_t r, uint8_t g, uint8_t b) {
        if (code >= 0) {
            return std::to_string(code);
        }
        return "rgb(" + std::to_string(static_cast<int>(r)) + "," +
               std::to_string(static_cast<int>(g)) + "," +
               std::to_string(static_cast<int>(b)) + ")";
    }

    static std::string FormatAttr(const CellAttr& attr) {
        std::string s =
            "fg=" + FormatColor(attr.fccode, attr.fr, attr.fg, attr.fb) +
            " bg=" + FormatColor(attr.bccode, attr.br, attr.bg, attr.bb);
        if (attr.bold) s += " bold";
        if (attr.italic) s += " italic";
        if (attr.underline) s += " underline";
        if (attr.inverse) s += " inverse";
        if (attr.blink) s += " blink";
        if (attr.protect) s += " protect";
        return s;
    }

    static bool ParseRgb(const std::string& s, uint8_t& r, uint8_t& g,
                         uint8_t& b) {
        if (s.rfind("rgb(", 0) != 0 || s.back() != ')') {
            return false;
        }
        std::string inner = s.substr(4, s.size() - 5);
        size_t first_comma = inner.find(',');
        size_t second_comma = inner.find(',', first_comma + 1);
        if (first_comma == std::string::npos ||
            second_comma == std::string::npos) {
            return false;
        }
        try {
            r = static_cast<uint8_t>(std::stoi(inner.substr(0, first_comma)));
            g = static_cast<uint8_t>(std::stoi(inner.substr(
                first_comma + 1, second_comma - first_comma - 1)));
            b = static_cast<uint8_t>(
                std::stoi(inner.substr(second_comma + 1)));
            return true;
        } catch (...) {
            return false;
        }
    }

    static CellAttr ParseAttr(const std::string& desc) {
        CellAttr attr{};
        std::vector<std::string> tokens;
        std::string current;
        for (char c : desc) {
            if (c == ' ') {
                if (!current.empty()) {
                    tokens.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) {
            tokens.push_back(current);
        }

        for (const auto& token : tokens) {
            if (token.rfind("fg=", 0) == 0) {
                std::string val = token.substr(3);
                uint8_t r, g, b;
                if (ParseRgb(val, r, g, b)) {
                    attr.fccode = -1;
                    attr.fr = r;
                    attr.fg = g;
                    attr.fb = b;
                } else {
                    attr.fccode = std::stoi(val);
                }
            } else if (token.rfind("bg=", 0) == 0) {
                std::string val = token.substr(3);
                uint8_t r, g, b;
                if (ParseRgb(val, r, g, b)) {
                    attr.bccode = -1;
                    attr.br = r;
                    attr.bg = g;
                    attr.bb = b;
                } else {
                    attr.bccode = std::stoi(val);
                }
            } else if (token == "bold") {
                attr.bold = true;
            } else if (token == "italic") {
                attr.italic = true;
            } else if (token == "underline") {
                attr.underline = true;
            } else if (token == "inverse") {
                attr.inverse = true;
            } else if (token == "blink") {
                attr.blink = true;
            } else if (token == "protect") {
                attr.protect = true;
            } else {
                std::cerr << "Warning: unknown attribute token: " << token
                          << "\n";
            }
        }
        return attr;
    }

    static std::vector<CellAttr> GetUniqueAttrs(const ScreenSnapshot& snap) {
        std::vector<CellAttr> attrs;
        attrs.reserve(snap.cells.size());
        for (const auto& cell : snap.cells) {
            attrs.push_back(cell.attr);
        }
        std::sort(attrs.begin(), attrs.end());
        attrs.erase(std::unique(attrs.begin(), attrs.end()), attrs.end());
        return attrs;
    }

    static std::string GetRow(const ScreenSnapshot& snap, unsigned int y) {
        std::string row;
        row.reserve(snap.width);
        for (unsigned int x = 0; x < snap.width; ++x) {
            row += snap.cells[y * snap.width + x].ch;
        }
        return row;
    }

    unsigned int width_;
    unsigned int height_;
    std::string cmd_;
    std::vector<std::string> args_;
    std::string term_type_;
    std::string locale_;
    std::unique_ptr<Terminal> term_;
    std::map<std::string, Command> commands_;
};

void TerminalApp::SetupCommands() {
    commands_["attr-map"] = {
        [this](const std::string& arg) { return HandleAttrMap(arg); },
        "  attr-map <attr_id|attr_desc> [snapshot_id]\n    Show column ranges "
        "on each "
        "row where the attribute is active."};
    commands_["attributes"] = {
        [this](const std::string& arg) { return HandleAttributes(arg); },
        "  attributes [snapshot_id]\n    List unique attributes present in "
        "the snapshot."};
    commands_["cell"] = {
        [this](const std::string& arg) { return HandleCell(arg); },
        "  cell <y> <x> [snapshot_id]\n    Get character at coordinates (x, "
        "y)."};
    commands_["cursor"] = {
        [this](const std::string& arg) { return HandleCursor(arg); },
        "  cursor [snapshot_id]\n    Get cursor location and visibility "
        "status."};
    commands_["diff"] = {
        [this](const std::string& arg) { return HandleDiff(arg); },
        "  diff <snapshot_id_a> [snapshot_id_b]\n    Show diff of snapshot "
        "<snapshot_id_b> (or current screen if omitted) against snapshot "
        "<snapshot_id_a>."};
    commands_["exit"] = {
        [this](const std::string& arg) { return HandleExit(arg); },
        "  exit\n    Exit the program."};
    commands_["find"] = {
        [this](const std::string& arg) { return HandleFind(arg); },
        "  find <string> [snapshot_id]\n    Locate occurrences of a string "
        "in a snapshot or the current display."};
    commands_["help"] = {
        [this](const std::string& arg) { return HandleHelp(arg); },
        "  help\n    List available commands."};
    commands_["keysyms"] = {
        [this](const std::string& arg) { return HandleKeysyms(arg); },
        "  keysyms\n    List available keysym names."};
    commands_["key"] = {
        [this](const std::string& arg) { return HandleKey(arg); },
        "  key <escaped_str>\n    Send characters to PTY."};
    commands_["kill"] = {
        [this](const std::string& arg) { return HandleKill(arg); },
        "  kill [signal]\n    Send signal (default SIGTERM=15) to "
        "subprocess."};
    commands_["range"] = {
        [this](const std::string& arg) { return HandleRange(arg); },
        "  range <y> <col_start> <col_end> [snapshot_id]\n    Get characters "
        "on row <y> from column <col_start> to <col_end>."};
    commands_["row"] = {
        [this](const std::string& arg) { return HandleRow(arg); },
        "  row <y> [snapshot_id]\n    Get characters on row <y>."};
    commands_["screen"] = {
        [this](const std::string& arg) { return HandleScreen(arg); },
        "  screen [snapshot_id]\n    Dump full snapshot contents to stdout."};
    commands_["screen-raw"] = {
        [this](const std::string& arg) { return HandleScreenRaw(arg); },
        "  screen-raw [snapshot_id]\n    Dump screen contents row by row "
        "without borders or cursor info."};
    commands_["resize"] = {
        [this](const std::string& arg) { return HandleResize(arg); },
        "  resize <width> <height>\n    Resize the terminal window and notify "
        "the process of size changes."};
    commands_["snapshot"] = {
        [this](const std::string& arg) { return HandleSnapshot(arg); },
        "  snapshot\n    Take snapshot of the screen and return its ID."};
    commands_["special"] = {
        [this](const std::string& arg) { return HandleSpecial(arg); },
        "  special <keyname> [mods...]\n    Send special key with optional "
        "modifiers (e.g. up ctrl shift)."};
    commands_["status"] = {
        [this](const std::string& arg) { return HandleStatus(arg); },
        "  status\n    Get status of child subprocess (exited <exit_code> or "
        "running)."};
    commands_["wait"] = {
        [this](const std::string& arg) { return HandleWait(arg); },
        "  wait <quiet-time-ms> <deadline-ms>\n    Wait until screen is idle "
        "for <quiet-time-ms> or <deadline-ms> elapsed."};
    commands_["wait-for-text"] = {
        [this](const std::string& arg) { return HandleWaitForText(arg); },
        "  wait-for-text <string> <deadline-ms>\n    Wait until <string> "
        "appears "
        "on screen or <deadline-ms> elapsed."};
}

bool TerminalApp::HandleAttrMap(const std::string& arg) {
    auto args = ParseArgs(arg);
    if (args.empty()) {
        std::cerr << "Usage: attr-map <attr_id|attr_desc> [snapshot_id]\n";
        return true;
    }
    int snap_id = args.size() > 1 ? ParseInt(args[1], "snapshot ID") : -1;
    ScreenSnapshot snap = term_->GetSnapshot(snap_id);

    CellAttr target_attr;
    bool is_numeric = !args[0].empty() &&
                      std::all_of(args[0].begin(), args[0].end(), ::isdigit);
    if (is_numeric) {
        int attr_id = ParseInt(args[0], "attribute ID");
        auto unique_attrs = GetUniqueAttrs(snap);
        if (attr_id < 0 ||
            static_cast<size_t>(attr_id) >= unique_attrs.size()) {
            std::cerr << "Invalid attribute ID: " << attr_id << "\n";
            return true;
        }
        target_attr = unique_attrs[attr_id];
    } else {
        target_attr = ParseAttr(args[0]);
    }

    for (unsigned int y = 0; y < snap.height; ++y) {
        std::vector<std::string> ranges;
        unsigned int x = 0;
        while (x < snap.width) {
            if (snap.cells[y * snap.width + x].attr == target_attr) {
                unsigned int x_start = x;
                while (x < snap.width &&
                       snap.cells[y * snap.width + x].attr == target_attr) {
                    x++;
                }
                ranges.push_back(std::to_string(x_start) + "-" +
                                 std::to_string(x - 1));
            } else {
                x++;
            }
        }
        if (!ranges.empty()) {
            std::cout << "row " << y << ": col ";
            for (size_t i = 0; i < ranges.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << ranges[i];
            }
            std::cout << "\n";
        }
    }
    return true;
}

bool TerminalApp::HandleAttributes(const std::string& arg) {
    auto args = ParseArgs(arg);
    int snap_id = !args.empty() ? ParseInt(args[0], "snapshot ID") : -1;
    ScreenSnapshot snap = term_->GetSnapshot(snap_id);
    auto unique_attrs = GetUniqueAttrs(snap);
    for (size_t i = 0; i < unique_attrs.size(); ++i) {
        std::cout << i << ": " << FormatAttr(unique_attrs[i]) << "\n";
    }
    return true;
}

bool TerminalApp::HandleCell(const std::string& arg) {
    auto args = ParseArgs(arg);
    if (args.size() < 2) {
        std::cerr << "Usage: cell <y> <x> [snapshot_id]\n";
        return true;
    }
    int y = ParseInt(args[0], "coordinate y");
    int x = ParseInt(args[1], "coordinate x");
    int snap_id = args.size() > 2 ? ParseInt(args[2], "snapshot ID") : -1;

    ScreenSnapshot snap = term_->GetSnapshot(snap_id);
    if (y >= static_cast<int>(snap.height) ||
        x >= static_cast<int>(snap.width) || y < 0 || x < 0) {
        std::cerr << "Coordinates out of range: " << x << ", " << y << "\n";
        return true;
    }
    std::cout << snap.cells[y * snap.width + x].ch << "\n";
    return true;
}

bool TerminalApp::HandleCursor(const std::string& arg) {
    auto args = ParseArgs(arg);
    int snap_id = !args.empty() ? ParseInt(args[0], "snapshot ID") : -1;
    ScreenSnapshot snap = term_->GetSnapshot(snap_id);
    std::cout << "col=" << snap.cursor_x << " row=" << snap.cursor_y
              << " visible=" << (snap.cursor_hidden ? 0 : 1) << "\n";
    return true;
}

bool TerminalApp::HandleDiff(const std::string& arg) {
    auto args = ParseArgs(arg);
    if (args.empty()) {
        std::cerr << "Usage: diff <snapshot_id_a> [snapshot_id_b]\n";
        return true;
    }
    int snap_a = ParseInt(args[0], "snapshot ID A");
    int snap_b = args.size() > 1 ? ParseInt(args[1], "snapshot ID B") : -1;

    ScreenSnapshot curr = term_->GetSnapshot(snap_b);
    ScreenSnapshot prev = term_->GetSnapshot(snap_a);

    if (curr.width != prev.width || curr.height != prev.height) {
        std::cout
            << "Warning: Snapshot dimensions mismatch (snapshot "
            << (snap_b == -1 ? "current" : std::to_string(snap_b)) << " "
            << curr.width << "x" << curr.height << " vs snapshot " << snap_a
            << " " << prev.width << "x" << prev.height
            << "). Snapshots before and after resize are incompatible.\n";
        return true;
    }

    for (unsigned int y = 0; y < curr.height; ++y) {
        unsigned int x = 0;
        while (x < curr.width) {
            if (curr.cells[y * curr.width + x].ch !=
                prev.cells[y * prev.width + x].ch) {
                unsigned int x_start = x;
                std::string old_str;
                std::string new_str;
                while (x < curr.width && x < prev.width &&
                       curr.cells[y * curr.width + x].ch !=
                           prev.cells[y * prev.width + x].ch) {
                    old_str += prev.cells[y * prev.width + x].ch;
                    new_str += curr.cells[y * curr.width + x].ch;
                    x++;
                }
                std::cout << "row " << y << " col " << x_start << "-"
                          << (x - 1) << ": \"" << old_str << "\" -> \""
                          << new_str << "\"\n";
            } else {
                x++;
            }
        }
    }
    return true;
}

bool TerminalApp::HandleExit(const std::string&) { return false; }

bool TerminalApp::HandleFind(const std::string& arg) {
    auto args = ParseArgs(arg);
    if (args.empty()) {
        std::cerr << "Usage: find <string> [snapshot_id]\n";
        return true;
    }
    std::string query = Terminal::ParseEscapes(args[0]);
    if (query.empty()) {
        std::cout << "empty query\n";
        return true;
    }
    int snap_id = args.size() > 1 ? ParseInt(args[1], "snapshot ID") : -1;

    ScreenSnapshot snap = term_->GetSnapshot(snap_id);
    bool found = false;

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
            unsigned int col_end = char_to_cell[pos + query.size() - 1];
            std::cout << "row " << y << " col " << col_start << "-" << col_end
                      << "\n";
            found = true;
            pos = row_str.find(query, pos + 1);
        }
    }
    if (!found) {
        std::cout << "not found\n";
    }
    return true;
}

bool TerminalApp::HandleHelp(const std::string&) {
    std::cout << "Commands:\n";
    for (const auto& [name, cmd] : commands_) {
        std::cout << cmd.help_text << "\n";
    }
    return true;
}

bool TerminalApp::HandleKeysyms(const std::string&) {
    auto keysyms = Terminal::GetKeysyms();
    for (size_t i = 0; i < keysyms.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << keysyms[i];
    }
    std::cout << "\n";
    return true;
}

bool TerminalApp::HandleKey(const std::string& arg) {
    term_->SendRawBytes(Terminal::ParseEscapes(arg));
    return true;
}

bool TerminalApp::HandleKill(const std::string& arg) {
    int sig = 15;
    if (!arg.empty()) {
        sig = ParseInt(arg, "signal");
    }
    if (term_->IsExited()) {
        std::cout << "Process already exited.\n";
    } else {
        term_->SendSignal(sig);
    }
    return true;
}

bool TerminalApp::HandleRange(const std::string& arg) {
    auto args = ParseArgs(arg);
    if (args.size() < 3) {
        std::cerr << "Usage: range <y> <col_start> <col_end> [snapshot_id]\n";
        return true;
    }
    int y = ParseInt(args[0], "coordinate y");
    int col_start = ParseInt(args[1], "column start");
    int col_end = ParseInt(args[2], "column end");
    int snap_id = args.size() > 3 ? ParseInt(args[3], "snapshot ID") : -1;

    ScreenSnapshot snap = term_->GetSnapshot(snap_id);
    if (y >= static_cast<int>(snap.height) || y < 0) {
        std::cerr << "Row index out of range: " << y << "\n";
        return true;
    }
    if (col_start >= static_cast<int>(snap.width) ||
        col_end >= static_cast<int>(snap.width) || col_start > col_end ||
        col_start < 0 || col_end < 0) {
        std::cerr << "Invalid column ranges: " << col_start << ", " << col_end
                  << "\n";
        return true;
    }
    for (int col = col_start; col <= col_end; ++col) {
        std::cout << snap.cells[y * snap.width + col].ch;
    }
    std::cout << "\n";
    return true;
}

bool TerminalApp::HandleRow(const std::string& arg) {
    auto args = ParseArgs(arg);
    if (args.empty()) {
        std::cerr << "Usage: row <y> [snapshot_id]\n";
        return true;
    }
    int y = ParseInt(args[0], "coordinate y");
    int snap_id = args.size() > 1 ? ParseInt(args[1], "snapshot ID") : -1;

    ScreenSnapshot snap = term_->GetSnapshot(snap_id);
    if (y >= static_cast<int>(snap.height) || y < 0) {
        std::cerr << "Row index out of range: " << y << "\n";
        return true;
    }
    for (unsigned int x = 0; x < snap.width; ++x) {
        std::cout << snap.cells[y * snap.width + x].ch;
    }
    std::cout << "\n";
    return true;
}

bool TerminalApp::HandleScreen(const std::string& arg) {
    auto args = ParseArgs(arg);
    int snap_id = !args.empty() ? ParseInt(args[0], "snapshot ID") : -1;
    std::cout << term_->DumpScreen(snap_id);
    return true;
}

bool TerminalApp::HandleSnapshot(const std::string&) {
    int id = term_->Snapshot();
    std::cout << "snapshot " << id << "\n";
    return true;
}

bool TerminalApp::HandleSpecial(const std::string& arg) {
    auto args = ParseArgs(arg);
    if (args.empty()) {
        std::cerr << "Usage: special <keyname> [mods...]\n";
        return true;
    }
    uint32_t keysym = Terminal::ParseKeysym(args[0]);
    if (keysym == 0) {
        std::cerr << "Unknown keyname: " << args[0] << "\n";
        return true;
    }
    unsigned int mods = Terminal::ParseMods(args, 1);
    term_->SendKey(keysym, mods);
    return true;
}

bool TerminalApp::HandleStatus(const std::string&) {
    if (term_->IsExited()) {
        std::cout << "exited " << term_->ExitStatus() << "\n";
    } else {
        std::cout << "running\n";
    }
    return true;
}

bool TerminalApp::HandleWait(const std::string& arg) {
    auto args = ParseArgs(arg);
    if (args.size() < 2) {
        std::cerr << "Usage: wait <quiet-time-ms> <deadline-ms>\n";
        return true;
    }
    int quiet_ms = ParseInt(args[0], "quiet-time-ms");
    int deadline_ms = ParseInt(args[1], "deadline-ms");
    if (quiet_ms < 0 || deadline_ms < 0) {
        std::cerr << "Times must be non-negative.\n";
        return true;
    }
    termobulator::unstable::WaitResult res =
        term_->WaitIdle(quiet_ms, deadline_ms);
    if (res == termobulator::unstable::WaitResult::kIdle) {
        std::cout << "wait: idle\n";
    } else if (res == termobulator::unstable::WaitResult::kDeadline) {
        std::cout << "wait: deadline\n";
    } else if (res == termobulator::unstable::WaitResult::kExited) {
        std::cout << "wait: exited\n";
    }
    return true;
}

bool TerminalApp::IsTextOnScreen(const std::string& query) {
    ScreenSnapshot snap = term_->GetSnapshot(-1);
    for (unsigned int y = 0; y < snap.height; ++y) {
        if (GetRow(snap, y).find(query) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool TerminalApp::HandleWaitForText(const std::string& arg) {
    auto args = ParseArgs(arg);
    if (args.size() < 2) {
        std::cerr << "Usage: wait-for-text <string> <deadline-ms>\n";
        return true;
    }
    std::string query = Terminal::ParseEscapes(args[0]);
    int deadline_ms = ParseInt(args[1], "deadline-ms");
    if (deadline_ms < 0) {
        std::cerr << "Deadline must be non-negative.\n";
        return true;
    }

    auto start_time = std::chrono::steady_clock::now();
    auto d_dur = std::chrono::milliseconds(deadline_ms);

    while (true) {
        if (IsTextOnScreen(query)) {
            std::cout << "wait-for-text: found\n";
            return true;
        }
        if (term_->IsExited()) {
            std::cout << "wait-for-text: exited\n";
            return true;
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time);
        if (elapsed >= d_dur) {
            std::cout << "wait-for-text: timeout\n";
            return true;
        }

        unsigned int remain = deadline_ms - elapsed.count();
        unsigned int wait_ms = std::min(10u, remain);
        term_->WaitIdle(5, wait_ms);
    }
}

bool TerminalApp::HandleScreenRaw(const std::string& arg) {
    auto args = ParseArgs(arg);
    int snap_id = !args.empty() ? ParseInt(args[0], "snapshot ID") : -1;
    ScreenSnapshot snap = term_->GetSnapshot(snap_id);
    for (unsigned int y = 0; y < snap.height; ++y) {
        std::cout << GetRow(snap, y) << "\n";
    }
    return true;
}

bool TerminalApp::HandleResize(const std::string& arg) {
    auto args = ParseArgs(arg);
    if (args.size() < 2) {
        std::cerr << "Usage: resize <width> <height>\n";
        return true;
    }
    int w = ParseInt(args[0], "width");
    int h = ParseInt(args[1], "height");
    if (w <= 0 || h <= 0) {
        std::cerr << "Dimensions must be positive.\n";
        return true;
    }
    term_->Resize(w, h);
    std::cout << "Resized to " << w << "x" << h << ".\n";
    std::cout << "Warning: Existing snapshots taken before this resize have "
                 "incompatible dimensions.\n";
    return true;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    // Reuse TerminalApp::ParseInt logic via a local wrapper.
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
        } else {
            break;
        }
    }

    if (arg_idx >= argc) {
        std::cerr << "Usage: " << argv[0]
                  << " [--width <width>] [--height <height>] [--terminal "
                     "<term>] [--locale <locale>] <binary> [args...]\n";
        return 1;
    }

    std::string cmd = argv[arg_idx];
    std::vector<std::string> args;
    for (int i = arg_idx + 1; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    TerminalApp app(width, height, cmd, args, term_type, locale);
    return app.Run();
}