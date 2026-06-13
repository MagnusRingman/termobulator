// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "dsl_interpreter.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "app_utils.h"

namespace termobulator {
namespace unstable {

nlohmann::json ParseTextToDsl(std::string_view text) {
    std::vector<std::vector<nlohmann::json>> stack_of_arrays;
    stack_of_arrays.push_back({});  // root level

    size_t i = 0;
    while (i < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[i]))) {
            i++;
            continue;
        }

        if (text[i] == '[') {
            stack_of_arrays.push_back({});
            i++;
            continue;
        }

        if (text[i] == ']') {
            if (stack_of_arrays.size() <= 1) {
                throw std::runtime_error(
                    "Unmatched closing bracket ']' in text DSL");
            }
            auto completed_quote = stack_of_arrays.back();
            stack_of_arrays.pop_back();
            stack_of_arrays.back().push_back(nlohmann::json(completed_quote));
            i++;
            continue;
        }

        if (text[i] == '"') {
            i++;  // consume opening quote
            std::string content;
            bool escaped = false;
            while (i < text.size()) {
                if (escaped) {
                    if (text[i] == 'n')
                        content += '\n';
                    else if (text[i] == 'r')
                        content += '\r';
                    else if (text[i] == 't')
                        content += '\t';
                    else
                        content += text[i];
                    escaped = false;
                } else if (text[i] == '\\') {
                    escaped = true;
                } else if (text[i] == '"') {
                    break;
                } else {
                    content += text[i];
                }
                i++;
            }
            if (i >= text.size()) {
                throw std::runtime_error(
                    "Unterminated string literal in text DSL");
            }
            i++;  // consume closing quote
            stack_of_arrays.back().push_back("$" + content);
            continue;
        }

        size_t start = i;
        while (i < text.size() &&
               !std::isspace(static_cast<unsigned char>(text[i])) &&
               text[i] != '[' && text[i] != ']' && text[i] != '"') {
            i++;
        }
        std::string token(text.substr(start, i - start));
        if (token == "true") {
            stack_of_arrays.back().push_back(true);
        } else if (token == "false") {
            stack_of_arrays.back().push_back(false);
        } else {
            bool is_num = !token.empty();
            size_t start_idx = 0;
            if (token[0] == '-' && token.size() > 1) {
                start_idx = 1;
            }
            for (size_t k = start_idx; k < token.size(); ++k) {
                if (!std::isdigit(static_cast<unsigned char>(token[k]))) {
                    is_num = false;
                    break;
                }
            }
            if (is_num) {
                stack_of_arrays.back().push_back(std::stoi(token));
            } else {
                stack_of_arrays.back().push_back(token);
            }
        }
    }

    if (stack_of_arrays.size() != 1) {
        throw std::runtime_error("Unmatched opening bracket '[' in text DSL");
    }

    return nlohmann::json(stack_of_arrays.back());
}

Interpreter::Interpreter(Terminal* term) : term_(term) {}

nlohmann::json Interpreter::Execute(const nlohmann::json& instructions) {
    if (!instructions.is_array()) {
        throw std::runtime_error(
            "DSL execution error: instructions must be a JSON array");
    }
    size_t idx = 0;
    try {
        for (; idx < instructions.size(); ++idx) {
            ExecuteToken(instructions[idx]);
        }
    } catch (const std::exception& e) {
        nlohmann::json stack_snapshot = stack_;
        ClearStack();
        std::string err_msg = e.what();
        std::string enriched_msg =
            "DSL execution failed at instruction " + std::to_string(idx) +
            " (" + instructions[idx].dump() + "): " + err_msg +
            ". Stack at failure: " + stack_snapshot.dump();
        throw std::runtime_error(enriched_msg);
    }
    return nlohmann::json(stack_);
}

nlohmann::json Interpreter::ExecuteText(std::string_view text) {
    return Execute(ParseTextToDsl(text));
}

void Interpreter::Push(nlohmann::json val) {
    stack_.push_back(std::move(val));
}

nlohmann::json Interpreter::Pop() {
    if (stack_.empty()) {
        throw std::runtime_error("Stack underflow");
    }
    nlohmann::json val = std::move(stack_.back());
    stack_.pop_back();
    return val;
}

int Interpreter::PopInt() {
    nlohmann::json val = Pop();
    if (!val.is_number_integer()) {
        throw std::runtime_error("Type error: expected integer");
    }
    return val.get<int>();
}

bool Interpreter::PopBool() {
    nlohmann::json val = Pop();
    if (!val.is_boolean()) {
        throw std::runtime_error("Type error: expected boolean");
    }
    return val.get<bool>();
}

std::string Interpreter::PopString() {
    nlohmann::json val = Pop();
    if (!val.is_string()) {
        throw std::runtime_error("Type error: expected string");
    }
    return val.get<std::string>();
}

nlohmann::json Interpreter::PopArray() {
    nlohmann::json val = Pop();
    if (!val.is_array()) {
        throw std::runtime_error("Type error: expected array (quotation)");
    }
    return val;
}

void Interpreter::ExecuteToken(const nlohmann::json& token) {
    if (token.is_number_integer() || token.is_boolean() || token.is_array()) {
        Push(token);
        return;
    }
    if (token.is_object()) {
        if (token.contains("lit")) {
            Push(token.at("lit"));
            return;
        }
        if (token.contains("op")) {
            std::string op_name = token.at("op").get<std::string>();
            if (token.contains("args")) {
                const auto& args = token.at("args");
                if (!args.is_array()) {
                    throw std::runtime_error("args must be a JSON array");
                }
                for (const auto& arg : args) {
                    Push(arg);
                }
            }
            ExecuteToken(op_name);
            return;
        }
        Push(token);
        return;
    }
    if (token.is_string()) {
        std::string s = token.get<std::string>();
        if (!s.empty() && s[0] == '$') {
            Push(s.substr(1));
            return;
        }

        // Handle operations
        if (s == "dup") {
            nlohmann::json val = Pop();
            Push(val);
            Push(std::move(val));
        } else if (s == "dup2") {
            nlohmann::json y = Pop();
            nlohmann::json x = Pop();
            Push(x);
            Push(y);
            Push(x);
            Push(y);
        } else if (s == "drop") {
            Pop();
        } else if (s == "swap") {
            nlohmann::json y = Pop();
            nlohmann::json x = Pop();
            Push(std::move(y));
            Push(std::move(x));
        } else if (s == "over") {
            nlohmann::json y = Pop();
            nlohmann::json x = Pop();
            Push(x);
            Push(y);
            Push(std::move(x));
        } else if (s == "rot") {
            nlohmann::json z = Pop();
            nlohmann::json y = Pop();
            nlohmann::json x = Pop();
            Push(std::move(y));
            Push(std::move(z));
            Push(std::move(x));
        } else if (s == "clear") {
            stack_.clear();
        } else if (s == "exec") {
            nlohmann::json q = PopArray();
            for (const auto& tok : q) {
                ExecuteToken(tok);
            }
        } else if (s == "if") {
            nlohmann::json false_q = PopArray();
            nlohmann::json true_q = PopArray();
            nlohmann::json cond = Pop();
            bool cond_val = false;
            if (cond.is_boolean()) {
                cond_val = cond.get<bool>();
            } else if (cond.is_number()) {
                cond_val = cond.get<double>() != 0;
            } else {
                throw std::runtime_error(
                    "Type error: expected boolean or number for condition");
            }
            const auto& selected_q = cond_val ? true_q : false_q;
            for (const auto& tok : selected_q) {
                ExecuteToken(tok);
            }
        } else if (s == "dip") {
            nlohmann::json q = PopArray();
            nlohmann::json x = Pop();
            for (const auto& tok : q) {
                ExecuteToken(tok);
            }
            Push(std::move(x));
        } else if (s == "while") {
            nlohmann::json body_q = PopArray();
            nlohmann::json cond_q = PopArray();
            while (true) {
                for (const auto& tok : cond_q) {
                    ExecuteToken(tok);
                }
                nlohmann::json cond = Pop();
                bool cond_val = false;
                if (cond.is_boolean()) {
                    cond_val = cond.get<bool>();
                } else if (cond.is_number()) {
                    cond_val = cond.get<double>() != 0;
                } else {
                    throw std::runtime_error(
                        "Type error: expected boolean or number for while "
                        "condition");
                }
                if (!cond_val) {
                    break;
                }
                for (const auto& tok : body_q) {
                    ExecuteToken(tok);
                }
            }
        } else if (s == "store") {
            std::string name = PopString();
            nlohmann::json val = Pop();
            variables_[name] = std::move(val);
        } else if (s == "load") {
            std::string name = PopString();
            auto it = variables_.find(name);
            if (it == variables_.end()) {
                throw std::runtime_error("Variable not found: " + name);
            }
            Push(it->second);
        } else if (s == "not") {
            nlohmann::json val = Pop();
            if (val.is_boolean()) {
                Push(!val.get<bool>());
            } else if (val.is_number()) {
                Push(val.get<double>() == 0);
            } else {
                throw std::runtime_error(
                    "Type error: expected boolean or number for negation");
            }
        } else if (s == "equal") {
            nlohmann::json y = Pop();
            nlohmann::json x = Pop();
            Push(x == y);
        } else if (s == "+") {
            int y = PopInt();
            int x = PopInt();
            Push(x + y);
        } else if (s == "-") {
            int y = PopInt();
            int x = PopInt();
            Push(x - y);
        } else if (s == "empty") {
            nlohmann::json val = Pop();
            if (val.is_string()) {
                Push(val.get<std::string>().empty());
            } else if (val.is_array()) {
                Push(val.empty());
            } else if (val.is_object()) {
                Push(val.empty());
            } else {
                throw std::runtime_error(
                    "Type error: expected string, array, or object for empty "
                    "check");
            }
        } else if (s == "size") {
            nlohmann::json val = Pop();
            if (val.is_string()) {
                Push(static_cast<int>(val.get<std::string>().size()));
            } else if (val.is_array() || val.is_object()) {
                Push(static_cast<int>(val.size()));
            } else {
                throw std::runtime_error(
                    "Type error: expected string, array, or object for size");
            }
        } else if (s == "sleep_ms") {
            int ms = PopInt();
            if (ms < 0) {
                throw std::runtime_error(
                    "Sleep duration must be non-negative");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        } else {
            // All operations below require terminal access
            if (!term_) {
                throw std::runtime_error(
                    "Terminal is not available for operation: " + s);
            }

            if (s == "send_key") {
                std::string keys = PopString();
                term_->SendRawBytes(
                    termobulator::unstable::Terminal::ParseEscapes(keys));
            } else if (s == "send_special_key") {
                std::string modifiers_str = PopString();
                std::string keyname = PopString();
                uint32_t keysym =
                    termobulator::unstable::Terminal::ParseKeysym(keyname);
                if (keysym == 0) {
                    throw std::runtime_error("Unknown keyname: " + keyname);
                }
                unsigned int mods = 0;
                if (!modifiers_str.empty()) {
                    std::vector<std::string> mod_list;
                    size_t pos = 0;
                    while (true) {
                        size_t next = modifiers_str.find(',', pos);
                        if (next == std::string::npos) {
                            mod_list.push_back(modifiers_str.substr(pos));
                            break;
                        }
                        mod_list.push_back(
                            modifiers_str.substr(pos, next - pos));
                        pos = next + 1;
                    }
                    mods = termobulator::unstable::Terminal::ParseMods(
                        mod_list, 0);
                }
                term_->SendKey(keysym, mods);
            } else if (s == "send_signal") {
                int sig = PopInt();
                if (term_->IsExited()) {
                    throw std::runtime_error("Process already exited");
                }
                term_->SendSignal(sig);
            } else if (s == "get_status") {
                if (term_->IsExited()) {
                    Push("exited " + std::to_string(term_->ExitStatus()));
                } else {
                    Push("running");
                }
            } else if (s == "wait_idle") {
                int deadline_ms = PopInt();
                int quiet_ms = PopInt();
                if (quiet_ms < 0 || deadline_ms < 0) {
                    throw std::runtime_error("Times must be non-negative");
                }
                termobulator::unstable::WaitResult wait_res =
                    term_->WaitIdle(quiet_ms, deadline_ms);
                if (wait_res == termobulator::unstable::WaitResult::kIdle) {
                    Push("wait: idle");
                } else if (wait_res ==
                           termobulator::unstable::WaitResult::kDeadline) {
                    Push("wait: deadline");
                } else if (wait_res ==
                           termobulator::unstable::WaitResult::kExited) {
                    Push("wait: exited");
                }
            } else if (s == "wait_for_text") {
                int deadline_ms = PopInt();
                std::string text = PopString();
                if (deadline_ms < 0) {
                    throw std::runtime_error("Deadline must be non-negative");
                }
                std::string query =
                    termobulator::unstable::Terminal::ParseEscapes(text);
                auto start_time = std::chrono::steady_clock::now();
                auto d_dur = std::chrono::milliseconds(deadline_ms);
                std::string result_str = "wait-for-text: timeout";

                while (true) {
                    if (termobulator::app_utils::IsTextOnScreen(term_,
                                                                query)) {
                        result_str = "wait-for-text: found";
                        break;
                    }
                    if (term_->IsExited()) {
                        result_str = "wait-for-text: exited";
                        break;
                    }
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - start_time);
                    if (elapsed >= d_dur) {
                        result_str = "wait-for-text: timeout";
                        break;
                    }
                    unsigned int remain = deadline_ms - elapsed.count();
                    unsigned int wait_ms = std::min(10u, remain);
                    term_->WaitIdle(std::min(5u, wait_ms), wait_ms);
                }
                Push(std::move(result_str));
            } else if (s == "take_snapshot") {
                int id = term_->Snapshot();
                Push(id);
            } else if (s == "get_screen") {
                int snap_id = PopInt();
                termobulator::unstable::ScreenSnapshot snap =
                    term_->GetSnapshot(snap_id);
                std::string screen_str;
                for (unsigned int y = 0; y < snap.height; ++y) {
                    if (y > 0) screen_str += "\n";
                    std::string row_str =
                        termobulator::app_utils::GetRow(snap, y);
                    size_t endpos = row_str.find_last_not_of(" \t\r\n");
                    if (endpos != std::string::npos) {
                        screen_str += row_str.substr(0, endpos + 1);
                    }
                }
                Push(std::move(screen_str));
            } else if (s == "get_cursor") {
                int snap_id = PopInt();
                termobulator::unstable::ScreenSnapshot snap =
                    term_->GetSnapshot(snap_id);
                Push(static_cast<int>(snap.cursor_x));
                Push(static_cast<int>(snap.cursor_y));
                Push(!snap.cursor_hidden);
            } else if (s == "get_cell") {
                int snap_id = PopInt();
                int y = PopInt();
                int x = PopInt();
                termobulator::unstable::ScreenSnapshot snap =
                    term_->GetSnapshot(snap_id);
                if (y >= static_cast<int>(snap.height) ||
                    x >= static_cast<int>(snap.width) || y < 0 || x < 0) {
                    throw std::runtime_error("Coordinates out of range");
                }
                const auto& cell = snap.cells[y * snap.width + x];
                termobulator::unstable::CellAttr attr = cell.attr;
                nlohmann::json cell_json = {
                    {"char", cell.ch},
                    {"fg", termobulator::app_utils::FormatColor(
                               attr.fccode, attr.fr, attr.fg, attr.fb)},
                    {"bg", termobulator::app_utils::FormatColor(
                               attr.bccode, attr.br, attr.bg, attr.bb)},
                    {"bold", attr.bold},
                    {"italic", attr.italic},
                    {"underline", attr.underline},
                    {"inverse", attr.inverse},
                    {"protect", attr.protect},
                    {"blink", attr.blink}};
                Push(std::move(cell_json));
            } else if (s == "get_row") {
                int snap_id = PopInt();
                int row = PopInt();
                termobulator::unstable::ScreenSnapshot snap =
                    term_->GetSnapshot(snap_id);
                if (row >= static_cast<int>(snap.height) || row < 0) {
                    throw std::runtime_error("Row index out of range: " +
                                             std::to_string(row));
                }
                std::string row_str =
                    termobulator::app_utils::GetRow(snap, row);
                size_t endpos = row_str.find_last_not_of(" \t\r\n");
                if (endpos != std::string::npos) {
                    row_str = row_str.substr(0, endpos + 1);
                } else {
                    row_str.clear();
                }
                Push(std::move(row_str));
            } else if (s == "get_attributes") {
                int snap_id = PopInt();
                termobulator::unstable::ScreenSnapshot snap =
                    term_->GetSnapshot(snap_id);
                auto unique_attrs =
                    termobulator::app_utils::GetUniqueAttrs(snap);
                std::string out;
                for (size_t i = 0; i < unique_attrs.size(); ++i) {
                    out +=
                        std::to_string(i) + ": " +
                        termobulator::app_utils::FormatAttr(unique_attrs[i]) +
                        "\n";
                }
                Push(std::move(out));
            } else if (s == "find_text") {
                int snap_id = PopInt();
                std::string text = PopString();
                std::string query =
                    termobulator::unstable::Terminal::ParseEscapes(text);
                if (query.empty()) {
                    throw std::runtime_error("Empty search query");
                }
                termobulator::unstable::ScreenSnapshot snap =
                    term_->GetSnapshot(snap_id);
                nlohmann::json results = nlohmann::json::array();
                for (unsigned int y = 0; y < snap.height; ++y) {
                    std::string row_str;
                    std::vector<unsigned int> char_to_cell;
                    for (unsigned int x = 0; x < snap.width; ++x) {
                        const std::string& ch =
                            snap.cells[y * snap.width + x].ch;
                        for (size_t i = 0; i < ch.size(); ++i) {
                            char_to_cell.push_back(x);
                        }
                        row_str += ch;
                    }
                    size_t pos = row_str.find(query, 0);
                    while (pos != std::string::npos) {
                        unsigned int col_start = char_to_cell[pos];
                        unsigned int col_end =
                            char_to_cell[pos + query.size() - 1];
                        nlohmann::json item;
                        item["row"] = y;
                        item["col_start"] = col_start;
                        item["col_end"] = col_end;
                        results.push_back(item);
                        pos = row_str.find(query, pos + 1);
                    }
                }
                Push(std::move(results));
            } else if (s == "get_diff") {
                int snap_a = PopInt();
                int snap_b = PopInt();
                termobulator::unstable::ScreenSnapshot prev =
                    term_->GetSnapshot(snap_a);
                termobulator::unstable::ScreenSnapshot curr =
                    term_->GetSnapshot(snap_b);

                if (curr.width != prev.width || curr.height != prev.height) {
                    throw std::runtime_error(
                        "Snapshot dimensions mismatch. Snapshots before and "
                        "after resize are incompatible.");
                }

                std::string out;
                for (unsigned int y = 0; y < curr.height; ++y) {
                    unsigned int cx = 0;
                    while (cx < curr.width) {
                        if (curr.cells[y * curr.width + cx].ch !=
                            prev.cells[y * prev.width + cx].ch) {
                            unsigned int x_start = cx;
                            std::string old_str;
                            std::string new_str;
                            while (cx < curr.width && cx < prev.width &&
                                   curr.cells[y * curr.width + cx].ch !=
                                       prev.cells[y * prev.width + cx].ch) {
                                old_str += prev.cells[y * prev.width + cx].ch;
                                new_str += curr.cells[y * curr.width + cx].ch;
                                cx++;
                            }
                            out += "row " + std::to_string(y) + " col " +
                                   std::to_string(x_start) + "-" +
                                   std::to_string(cx - 1) + ": \"" + old_str +
                                   "\" -> \"" + new_str + "\"\n";
                        } else {
                            cx++;
                        }
                    }
                }
                Push(std::move(out));
            } else {
                throw std::runtime_error("Unknown operation: " + s);
            }
        }
        return;
    }
    throw std::runtime_error("DSL execution error: invalid token type");
}

}  // namespace unstable
}  // namespace termobulator
