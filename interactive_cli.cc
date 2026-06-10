// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "interactive_cli.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "app_utils.h"

namespace termobulator {

using termobulator::unstable::CellAttr;
using termobulator::unstable::CreateSubprocessTerminal;
using termobulator::unstable::ScreenSnapshot;
using termobulator::unstable::Terminal;

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
    SetupCommands();

    std::string line;
    while (std::getline(std::cin, line)) {
        auto [cmd, arg] = app_utils::SplitCommand(line);
        if (auto it = commands_.find(cmd); it != commands_.end()) {
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

void InteractiveCli::SetupCommands() {
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
    commands_["scrollback"] = {
        [this](const std::string& arg) { return HandleScrollback(arg); },
        "  scrollback <lines>\n    Retrieve the last <lines> of scrollback."};
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

bool InteractiveCli::HandleAttrMap(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    if (args.empty()) {
        std::cerr << "Usage: attr-map <attr_id|attr_desc> [snapshot_id]\n";
        return true;
    }
    int snap_id =
        args.size() > 1 ? app_utils::ParseInt(args[1], "snapshot ID") : -1;
    ScreenSnapshot snap = term_->GetSnapshot(snap_id);

    CellAttr target_attr;
    bool is_numeric = !args[0].empty() &&
                      std::all_of(args[0].begin(), args[0].end(), ::isdigit);
    if (is_numeric) {
        int attr_id = app_utils::ParseInt(args[0], "attribute ID");
        auto unique_attrs = app_utils::GetUniqueAttrs(snap);
        if (attr_id < 0 ||
            static_cast<size_t>(attr_id) >= unique_attrs.size()) {
            std::cerr << "Invalid attribute ID: " << attr_id << "\n";
            return true;
        }
        target_attr = unique_attrs[attr_id];
    } else {
        target_attr = app_utils::ParseAttr(args[0]);
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

bool InteractiveCli::HandleAttributes(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    int snap_id =
        !args.empty() ? app_utils::ParseInt(args[0], "snapshot ID") : -1;
    ScreenSnapshot snap = term_->GetSnapshot(snap_id);
    auto unique_attrs = app_utils::GetUniqueAttrs(snap);
    for (size_t i = 0; i < unique_attrs.size(); ++i) {
        std::cout << i << ": " << app_utils::FormatAttr(unique_attrs[i])
                  << "\n";
    }
    return true;
}

bool InteractiveCli::HandleCell(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    if (args.size() < 2) {
        std::cerr << "Usage: cell <y> <x> [snapshot_id]\n";
        return true;
    }
    int y = app_utils::ParseInt(args[0], "coordinate y");
    int x = app_utils::ParseInt(args[1], "coordinate x");
    int snap_id =
        args.size() > 2 ? app_utils::ParseInt(args[2], "snapshot ID") : -1;

    ScreenSnapshot snap = term_->GetSnapshot(snap_id);
    if (y >= static_cast<int>(snap.height) ||
        x >= static_cast<int>(snap.width) || y < 0 || x < 0) {
        std::cerr << "Coordinates out of range: " << x << ", " << y << "\n";
        return true;
    }
    std::cout << snap.cells[y * snap.width + x].ch << "\n";
    return true;
}

bool InteractiveCli::HandleCursor(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    int snap_id =
        !args.empty() ? app_utils::ParseInt(args[0], "snapshot ID") : -1;
    ScreenSnapshot snap = term_->GetSnapshot(snap_id);
    std::cout << "col=" << snap.cursor_x << " row=" << snap.cursor_y
              << " visible=" << (snap.cursor_hidden ? 0 : 1) << "\n";
    return true;
}

bool InteractiveCli::HandleDiff(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    if (args.empty()) {
        std::cerr << "Usage: diff <snapshot_id_a> [snapshot_id_b]\n";
        return true;
    }
    int snap_a = app_utils::ParseInt(args[0], "snapshot ID A");
    int snap_b =
        args.size() > 1 ? app_utils::ParseInt(args[1], "snapshot ID B") : -1;

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

bool InteractiveCli::HandleExit(const std::string&) { return false; }

bool InteractiveCli::HandleFind(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    if (args.empty()) {
        std::cerr << "Usage: find <string> [snapshot_id]\n";
        return true;
    }
    std::string query = Terminal::ParseEscapes(args[0]);
    if (query.empty()) {
        std::cout << "empty query\n";
        return true;
    }
    int snap_id =
        args.size() > 1 ? app_utils::ParseInt(args[1], "snapshot ID") : -1;

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

bool InteractiveCli::HandleHelp(const std::string&) {
    std::cout << "Commands:\n";
    for (const auto& [name, cmd] : commands_) {
        std::cout << cmd.help_text << "\n";
    }
    return true;
}

bool InteractiveCli::HandleKeysyms(const std::string&) {
    auto keysyms = Terminal::GetKeysyms();
    for (size_t i = 0; i < keysyms.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << keysyms[i];
    }
    std::cout << "\n";
    return true;
}

bool InteractiveCli::HandleKey(const std::string& arg) {
    term_->SendRawBytes(Terminal::ParseEscapes(arg));
    return true;
}

bool InteractiveCli::HandleKill(const std::string& arg) {
    int sig = 15;
    if (!arg.empty()) {
        sig = app_utils::ParseInt(arg, "signal");
    }
    if (term_->IsExited()) {
        std::cout << "Process already exited.\n";
    } else {
        term_->SendSignal(sig);
    }
    return true;
}

bool InteractiveCli::HandleRange(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    if (args.size() < 3) {
        std::cerr << "Usage: range <y> <col_start> <col_end> [snapshot_id]\n";
        return true;
    }
    int y = app_utils::ParseInt(args[0], "coordinate y");
    int col_start = app_utils::ParseInt(args[1], "column start");
    int col_end = app_utils::ParseInt(args[2], "column end");
    int snap_id =
        args.size() > 3 ? app_utils::ParseInt(args[3], "snapshot ID") : -1;

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

bool InteractiveCli::HandleRow(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    if (args.empty()) {
        std::cerr << "Usage: row <y> [snapshot_id]\n";
        return true;
    }
    int y = app_utils::ParseInt(args[0], "coordinate y");
    int snap_id =
        args.size() > 1 ? app_utils::ParseInt(args[1], "snapshot ID") : -1;

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

bool InteractiveCli::HandleScreen(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    int snap_id =
        !args.empty() ? app_utils::ParseInt(args[0], "snapshot ID") : -1;
    std::cout << term_->DumpScreen(snap_id);
    return true;
}

bool InteractiveCli::HandleSnapshot(const std::string&) {
    int id = term_->Snapshot();
    std::cout << "snapshot " << id << "\n";
    return true;
}

bool InteractiveCli::HandleSpecial(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
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

bool InteractiveCli::HandleStatus(const std::string&) {
    if (term_->IsExited()) {
        std::cout << "exited " << term_->ExitStatus() << "\n";
    } else {
        std::cout << "running\n";
    }
    return true;
}

bool InteractiveCli::HandleWait(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    if (args.size() < 2) {
        std::cerr << "Usage: wait <quiet-time-ms> <deadline-ms>\n";
        return true;
    }
    int quiet_ms = app_utils::ParseInt(args[0], "quiet-time-ms");
    int deadline_ms = app_utils::ParseInt(args[1], "deadline-ms");
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

bool InteractiveCli::HandleWaitForText(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    if (args.size() < 2) {
        std::cerr << "Usage: wait-for-text <string> <deadline-ms>\n";
        return true;
    }
    std::string query = Terminal::ParseEscapes(args[0]);
    int deadline_ms = app_utils::ParseInt(args[1], "deadline-ms");
    if (deadline_ms < 0) {
        std::cerr << "Deadline must be non-negative.\n";
        return true;
    }

    auto start_time = std::chrono::steady_clock::now();
    auto d_dur = std::chrono::milliseconds(deadline_ms);

    while (true) {
        if (app_utils::IsTextOnScreen(term_.get(), query)) {
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

bool InteractiveCli::HandleScreenRaw(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    int snap_id =
        !args.empty() ? app_utils::ParseInt(args[0], "snapshot ID") : -1;
    ScreenSnapshot snap = term_->GetSnapshot(snap_id);
    for (unsigned int y = 0; y < snap.height; ++y) {
        std::cout << app_utils::GetRow(snap, y) << "\n";
    }
    return true;
}

bool InteractiveCli::HandleResize(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    if (args.size() < 2) {
        std::cerr << "Usage: resize <width> <height>\n";
        return true;
    }
    int w = app_utils::ParseInt(args[0], "width");
    int h = app_utils::ParseInt(args[1], "height");
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

bool InteractiveCli::HandleScrollback(const std::string& arg) {
    auto args = app_utils::ParseArgs(arg);
    if (args.empty()) {
        std::cerr << "Usage: scrollback <lines>\n";
        return true;
    }
    int lines = app_utils::ParseInt(args[0], "lines");
    if (lines <= 0) {
        std::cerr << "Lines must be positive.\n";
        return true;
    }
    std::vector<std::string> sb = term_->GetScrollback(lines);
    for (const auto& line : sb) {
        std::cout << line << "\n";
    }
    return true;
}

}  // namespace termobulator
