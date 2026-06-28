// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "lua_executor.h"

#include <lua.hpp>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app_utils.h"

namespace termobulator {
namespace unstable {

namespace {

struct LuaStateDeleter {
    void operator()(lua_State* L) const {
        if (L) {
            lua_close(L);
        }
    }
};
using UniqueLuaState = std::unique_ptr<lua_State, LuaStateDeleter>;

int ErrorHandler(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) {
        msg = "";
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

int LogFunc(lua_State* L) {
    auto* log_ptr = static_cast<std::vector<std::string>*>(
        lua_touserdata(L, lua_upvalueindex(1)));
    if (!log_ptr) {
        return 0;
    }
    int nargs = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= nargs; ++i) {
        if (i > 1) {
            line += "\t";
        }
        size_t len;
        const char* s = luaL_tolstring(L, i, &len);
        line.append(s, len);
        lua_pop(L, 1);
    }
    log_ptr->push_back(line);
    return 0;
}

Terminal* GetTerm(lua_State* L) {
    Terminal* term =
        static_cast<Terminal*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!term) {
        luaL_error(L, "Terminal is not available (null)");
    }
    return term;
}

int TermSendKey(lua_State* L) {
    Terminal* term = GetTerm(L);
    size_t len;
    const char* text = luaL_checklstring(L, 1, &len);
    term->SendRawBytes(Terminal::ParseEscapes(std::string_view(text, len)));
    return 0;
}

int TermSendSpecialKey(lua_State* L) {
    Terminal* term = GetTerm(L);
    std::string keyname = luaL_checkstring(L, 1);
    std::string modifiers_str = luaL_optstring(L, 2, "");

    uint32_t keysym = Terminal::ParseKeysym(keyname);
    if (keysym == 0) {
        return luaL_error(L, "Unknown keyname: %s", keyname.c_str());
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
            mod_list.push_back(modifiers_str.substr(pos, next - pos));
            pos = next + 1;
        }
        mods = Terminal::ParseMods(mod_list, 0);
    }
    term->SendKey(keysym, mods);
    return 0;
}

int TermSendSignal(lua_State* L) {
    Terminal* term = GetTerm(L);
    int sig = luaL_checkinteger(L, 1);
    if (term->IsExited()) {
        return luaL_error(L, "Process already exited");
    }
    term->SendSignal(sig);
    return 0;
}

int TermSleepMs(lua_State* L) {
    int ms = luaL_checkinteger(L, 1);
    if (ms < 0) {
        return luaL_error(L, "Sleep duration must be non-negative");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return 0;
}

int TermWaitIdle(lua_State* L) {
    Terminal* term = GetTerm(L);
    int quiet_ms = luaL_checkinteger(L, 1);
    int deadline_ms = luaL_checkinteger(L, 2);
    if (quiet_ms < 0 || deadline_ms < 0) {
        return luaL_error(L, "Times must be non-negative");
    }
    WaitResult res = term->WaitIdle(quiet_ms, deadline_ms);
    if (res == WaitResult::kIdle) {
        lua_pushstring(L, "idle");
    } else if (res == WaitResult::kExited) {
        lua_pushstring(L, "exited");
    } else {
        lua_pushstring(L, "deadline");
    }
    return 1;
}

int TermWaitForText(lua_State* L) {
    Terminal* term = GetTerm(L);
    std::string text = luaL_checkstring(L, 1);
    int deadline_ms = luaL_checkinteger(L, 2);
    if (deadline_ms < 0) {
        return luaL_error(L, "Deadline must be non-negative");
    }
    std::string query = Terminal::ParseEscapes(text);
    auto start_time = std::chrono::steady_clock::now();
    auto d_dur = std::chrono::milliseconds(deadline_ms);

    while (true) {
        if (app_utils::IsTextOnScreen(term, query)) {
            lua_pushstring(L, "found");
            return 1;
        }
        if (term->IsExited()) {
            lua_pushstring(L, "exited");
            return 1;
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time);
        if (elapsed >= d_dur) {
            lua_pushstring(L, "timeout");
            return 1;
        }
        unsigned int remain = deadline_ms - elapsed.count();
        unsigned int wait_ms = std::min(10u, remain);
        term->WaitIdle(std::min(5u, wait_ms), wait_ms);
    }
}

int TermWatchText(lua_State* L) {
    std::string text = luaL_checkstring(L, 1);
    auto* udata = static_cast<WatcherDescriptor*>(
        lua_newuserdatauv(L, sizeof(WatcherDescriptor), 0));
    new (udata) WatcherDescriptor();
    udata->type = WatcherDescriptor::Type::kText;
    udata->text = Terminal::ParseEscapes(text);

    luaL_getmetatable(L, "Termobulator.Watcher");
    lua_setmetatable(L, -2);
    return 1;
}

int TermWatchTimeout(lua_State* L) {
    int ms = luaL_checkinteger(L, 1);
    if (ms < 0) {
        return luaL_error(L, "Timeout must be non-negative");
    }
    auto* udata = static_cast<WatcherDescriptor*>(
        lua_newuserdatauv(L, sizeof(WatcherDescriptor), 0));
    new (udata) WatcherDescriptor();
    udata->type = WatcherDescriptor::Type::kTimeout;
    udata->deadline_ms = ms;
    udata->absolute_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

    luaL_getmetatable(L, "Termobulator.Watcher");
    lua_setmetatable(L, -2);
    return 1;
}

int WatcherToString(lua_State* L) {
    auto* udata = static_cast<WatcherDescriptor*>(
        luaL_checkudata(L, 1, "Termobulator.Watcher"));
    if (udata->type == WatcherDescriptor::Type::kText) {
        lua_pushfstring(L, "watcher: text(%q)", udata->text.c_str());
    } else if (udata->type == WatcherDescriptor::Type::kTimeout) {
        lua_pushfstring(L, "watcher: timeout(%d)", udata->deadline_ms);
    } else {
        lua_pushfstring(L, "watcher: custom(%s)", udata->text.c_str());
    }
    return 1;
}

int WatcherGc(lua_State* L) {
    auto* udata = static_cast<WatcherDescriptor*>(
        luaL_checkudata(L, 1, "Termobulator.Watcher"));
    udata->~WatcherDescriptor();
    return 0;
}

int TermWaitAny(lua_State* L) {
    Terminal* term = GetTerm(L);
    int nargs = lua_gettop(L);
    if (nargs == 0) {
        return luaL_error(L, "wait_any requires at least one watcher");
    }
    std::vector<WatcherDescriptor> conditions;
    std::vector<int> stack_indices;
    conditions.reserve(nargs);
    stack_indices.reserve(nargs);

    for (int i = 1; i <= nargs; ++i) {
        auto* udata = static_cast<WatcherDescriptor*>(
            luaL_testudata(L, i, "Termobulator.Watcher"));
        if (!udata) {
            return luaL_argerror(L, i, "expected watcher userdata");
        }
        conditions.push_back(*udata);
        stack_indices.push_back(i);
    }

    WatchResult res = term->WaitAny(conditions);
    if (res.fired_index == -1) {
        lua_pushstring(L, "exited");
        return 1;
    }

    lua_pushvalue(L, stack_indices[res.fired_index]);
    return 1;
}

int TermGetStatus(lua_State* L) {
    Terminal* term = GetTerm(L);
    if (term->IsExited()) {
        lua_pushfstring(L, "exited %d", term->ExitStatus());
    } else {
        lua_pushstring(L, "running");
    }
    return 1;
}

int TermDumpScreen(lua_State* L) {
    Terminal* term = GetTerm(L);
    int snap_id = luaL_optinteger(L, 1, -1);
    try {
        lua_pushstring(L, term->DumpScreen(snap_id).c_str());
        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermDumpScreenHtml(lua_State* L) {
    Terminal* term = GetTerm(L);
    int snap_id = luaL_optinteger(L, 1, -1);
    try {
        ScreenSnapshot snap = term->GetSnapshot(snap_id);
        std::string out;

        out +=
            "<pre style=\"font-family: "
            "ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,'Liberation "
            "Mono','Courier New',monospace; "
            "background-color: rgb(0,0,0); color: rgb(229,229,229); padding: "
            "10px; border-radius: 4px; "
            "line-height: 1.2; overflow-x: auto; white-space: pre; margin: "
            "0;\">\n";

        struct CellVisualState {
            CellAttr attr;
            bool is_cursor = false;

            bool operator==(const CellVisualState& o) const {
                return attr == o.attr && is_cursor == o.is_cursor;
            }
            bool operator!=(const CellVisualState& o) const {
                return !(*this == o);
            }
        };

        auto escape_html = [](const std::string& str) {
            std::string escaped;
            escaped.reserve(str.size());
            for (char c : str) {
                switch (c) {
                case '&':
                    escaped += "&amp;";
                    break;
                case '<':
                    escaped += "&lt;";
                    break;
                case '>':
                    escaped += "&gt;";
                    break;
                case '\"':
                    escaped += "&quot;";
                    break;
                case '\'':
                    escaped += "&#39;";
                    break;
                default:
                    escaped += c;
                    break;
                }
            }
            return escaped;
        };

        auto get_style_str = [](const CellAttr& attr, bool is_cursor) {
            std::string style;
            bool invert = attr.inverse ^ is_cursor;
            uint8_t fg_r = invert ? attr.br : attr.fr;
            uint8_t fg_g = invert ? attr.bg : attr.fg;
            uint8_t fg_b = invert ? attr.bb : attr.fb;
            uint8_t bg_r = invert ? attr.fr : attr.br;
            uint8_t bg_g = invert ? attr.fg : attr.bg;
            uint8_t bg_b = invert ? attr.fb : attr.bb;

            style += "color: rgb(" + std::to_string(fg_r) + "," +
                     std::to_string(fg_g) + "," + std::to_string(fg_b) + ");";
            style += "background-color: rgb(" + std::to_string(bg_r) + "," +
                     std::to_string(bg_g) + "," + std::to_string(bg_b) + ");";

            if (attr.bold) style += "font-weight: bold;";
            if (attr.italic) style += "font-style: italic;";
            if (attr.underline) style += "text-decoration: underline;";
            if (attr.blink) style += "text-decoration: blink;";
            return style;
        };

        for (unsigned int y = 0; y < snap.height; ++y) {
            CellVisualState current_state;
            bool has_open_span = false;

            for (unsigned int x = 0; x < snap.width; ++x) {
                const auto& cell = snap.cells[y * snap.width + x];
                bool is_cursor = (!snap.cursor_hidden && y == snap.cursor_y &&
                                  x == snap.cursor_x);
                CellVisualState cell_state{cell.attr, is_cursor};

                if (!has_open_span) {
                    out +=
                        "<span style=\"" +
                        get_style_str(cell_state.attr, cell_state.is_cursor) +
                        "\">";
                    current_state = cell_state;
                    has_open_span = true;
                } else if (cell_state != current_state) {
                    out += "</span>";
                    out +=
                        "<span style=\"" +
                        get_style_str(cell_state.attr, cell_state.is_cursor) +
                        "\">";
                    current_state = cell_state;
                }
                out += escape_html(cell.ch);
            }
            if (has_open_span) {
                out += "</span>";
            }
            out += "\n";
        }
        out += "</pre>";

        lua_pushstring(L, out.c_str());
        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermTakeSnapshot(lua_State* L) {
    Terminal* term = GetTerm(L);
    lua_pushinteger(L, term->Snapshot());
    return 1;
}

int TermGetScreen(lua_State* L) {
    Terminal* term = GetTerm(L);
    int snap_id = luaL_optinteger(L, 1, -1);
    try {
        ScreenSnapshot snap = term->GetSnapshot(snap_id);
        std::string screen_str;
        for (unsigned int y = 0; y < snap.height; ++y) {
            if (y > 0) screen_str += "\n";
            std::string row_str;
            row_str.reserve(snap.width);
            for (unsigned int x = 0; x < snap.width; ++x) {
                row_str += snap.cells[y * snap.width + x].ch;
            }
            size_t endpos = row_str.find_last_not_of(" \t\r\n");
            if (endpos != std::string::npos) {
                screen_str += row_str.substr(0, endpos + 1);
            }
        }
        lua_pushlstring(L, screen_str.data(), screen_str.size());
        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermGetCursor(lua_State* L) {
    Terminal* term = GetTerm(L);
    int snap_id = luaL_optinteger(L, 1, -1);
    try {
        ScreenSnapshot snap = term->GetSnapshot(snap_id);
        lua_pushinteger(L, snap.cursor_x);
        lua_pushinteger(L, snap.cursor_y);
        lua_pushboolean(L, !snap.cursor_hidden);
        return 3;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermGetCell(lua_State* L) {
    Terminal* term = GetTerm(L);
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int snap_id = luaL_optinteger(L, 3, -1);
    try {
        ScreenSnapshot snap = term->GetSnapshot(snap_id);
        if (y >= static_cast<int>(snap.height) ||
            x >= static_cast<int>(snap.width) || y < 0 || x < 0) {
            return luaL_error(L, "Coordinates out of range");
        }
        const auto& cell = snap.cells[y * snap.width + x];
        CellAttr attr = cell.attr;

        lua_newtable(L);
        lua_pushstring(L, cell.ch.c_str());
        lua_setfield(L, -2, "char");

        lua_pushstring(
            L, app_utils::FormatColor(attr.fccode, attr.fr, attr.fg, attr.fb)
                   .c_str());
        lua_setfield(L, -2, "fg");
        lua_pushstring(
            L, app_utils::FormatColor(attr.bccode, attr.br, attr.bg, attr.bb)
                   .c_str());
        lua_setfield(L, -2, "bg");

        lua_pushboolean(L, attr.bold);
        lua_setfield(L, -2, "bold");
        lua_pushboolean(L, attr.italic);
        lua_setfield(L, -2, "italic");
        lua_pushboolean(L, attr.underline);
        lua_setfield(L, -2, "underline");
        lua_pushboolean(L, attr.inverse);
        lua_setfield(L, -2, "inverse");
        lua_pushboolean(L, attr.protect);
        lua_setfield(L, -2, "protect");
        lua_pushboolean(L, attr.blink);
        lua_setfield(L, -2, "blink");

        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermGetRow(lua_State* L) {
    Terminal* term = GetTerm(L);
    int row = luaL_checkinteger(L, 1);
    int snap_id = luaL_optinteger(L, 2, -1);
    try {
        ScreenSnapshot snap = term->GetSnapshot(snap_id);
        if (row >= static_cast<int>(snap.height) || row < 0) {
            return luaL_error(L, "Row index out of range: %d", row);
        }
        std::string row_str = app_utils::GetRow(snap, row);
        size_t endpos = row_str.find_last_not_of(" \t\r\n");
        if (endpos != std::string::npos) {
            row_str = row_str.substr(0, endpos + 1);
        } else {
            row_str.clear();
        }
        lua_pushlstring(L, row_str.data(), row_str.size());
        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermGetAttributes(lua_State* L) {
    Terminal* term = GetTerm(L);
    int snap_id = luaL_optinteger(L, 1, -1);
    try {
        ScreenSnapshot snap = term->GetSnapshot(snap_id);
        auto unique_attrs = app_utils::GetUniqueAttrs(snap);
        std::string out;
        for (size_t i = 0; i < unique_attrs.size(); ++i) {
            out += std::to_string(i) + ": " +
                   app_utils::FormatAttr(unique_attrs[i]) + "\n";
        }
        lua_pushstring(L, out.c_str());
        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermFindText(lua_State* L) {
    Terminal* term = GetTerm(L);
    std::string text = luaL_checkstring(L, 1);
    int snap_id = luaL_optinteger(L, 2, -1);
    std::string query = Terminal::ParseEscapes(text);
    if (query.empty()) {
        return luaL_error(L, "Empty search query");
    }
    try {
        ScreenSnapshot snap = term->GetSnapshot(snap_id);
        lua_newtable(L);
        int idx = 1;
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

                lua_newtable(L);
                lua_pushinteger(L, y);
                lua_setfield(L, -2, "row");
                lua_pushinteger(L, col_start);
                lua_setfield(L, -2, "col_start");
                lua_pushinteger(L, col_end);
                lua_setfield(L, -2, "col_end");

                lua_rawseti(L, -2, idx++);
                pos = row_str.find(query, pos + 1);
            }
        }
        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermGetDiff(lua_State* L) {
    Terminal* term = GetTerm(L);
    int snap_a = luaL_checkinteger(L, 1);
    int snap_b = luaL_checkinteger(L, 2);
    try {
        ScreenSnapshot prev = term->GetSnapshot(snap_a);
        ScreenSnapshot curr = term->GetSnapshot(snap_b);
        if (curr.width != prev.width || curr.height != prev.height) {
            return luaL_error(L,
                              "Snapshot dimensions mismatch. Snapshots before "
                              "and after resize are incompatible.");
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
        lua_pushstring(L, out.c_str());
        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermGetScrollback(lua_State* L) {
    Terminal* term = GetTerm(L);
    int lines = luaL_checkinteger(L, 1);
    if (lines < 0) {
        return luaL_error(L, "Lines count must be non-negative");
    }
    try {
        std::vector<std::string> sb = term->GetScrollback(lines);
        lua_newtable(L);
        int idx = 1;
        for (const auto& line : sb) {
            lua_pushlstring(L, line.data(), line.size());
            lua_rawseti(L, -2, idx++);
        }
        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermResize(lua_State* L) {
    Terminal* term = GetTerm(L);
    int width = luaL_checkinteger(L, 1);
    int height = luaL_checkinteger(L, 2);
    if (width <= 0 || height <= 0) {
        return luaL_error(L, "Dimensions must be positive");
    }
    try {
        term->Resize(width, height);
        return 0;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermGetTerminalSize(lua_State* L) {
    Terminal* term = GetTerm(L);
    lua_pushinteger(L, term->Width());
    lua_pushinteger(L, term->Height());
    return 2;
}

int TermSetDisableAlternateScreen(lua_State* L) {
    Terminal* term = GetTerm(L);
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    bool disable = lua_toboolean(L, 1);
    term->SetDisableAlternateScreen(disable);
    return 0;
}

int TermGetKeysyms(lua_State* L) {
    auto keysyms = Terminal::GetKeysyms();
    lua_newtable(L);
    int idx = 1;
    for (const auto& name : keysyms) {
        lua_pushlstring(L, name.data(), name.size());
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

int TermGetRows(lua_State* L) {
    Terminal* term = GetTerm(L);
    int start_row = luaL_checkinteger(L, 1);
    int end_row = luaL_checkinteger(L, 2);
    int snap_id = luaL_optinteger(L, 3, -1);
    try {
        ScreenSnapshot snap = term->GetSnapshot(snap_id);
        if (start_row < 0 || start_row >= static_cast<int>(snap.height) ||
            end_row < 0 || end_row >= static_cast<int>(snap.height) ||
            start_row > end_row) {
            return luaL_error(L, "Invalid row range: %d to %d (height %d)",
                              start_row, end_row, snap.height);
        }
        lua_newtable(L);
        int idx = 1;
        for (int row = start_row; row <= end_row; ++row) {
            std::string row_str = app_utils::GetRow(snap, row);
            size_t endpos = row_str.find_last_not_of(" \t\r\n");
            if (endpos != std::string::npos) {
                row_str = row_str.substr(0, endpos + 1);
            } else {
                row_str.clear();
            }
            lua_pushlstring(L, row_str.data(), row_str.size());
            lua_rawseti(L, -2, idx++);
        }
        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermGetDiffStructured(lua_State* L) {
    Terminal* term = GetTerm(L);
    int snap_a = luaL_checkinteger(L, 1);
    int snap_b = luaL_checkinteger(L, 2);
    try {
        ScreenSnapshot prev = term->GetSnapshot(snap_a);
        ScreenSnapshot curr = term->GetSnapshot(snap_b);
        if (curr.width != prev.width || curr.height != prev.height) {
            return luaL_error(L,
                              "Snapshot dimensions mismatch. Snapshots before "
                              "and after resize are incompatible.");
        }
        lua_newtable(L);
        int idx = 1;
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
                    lua_newtable(L);

                    lua_pushinteger(L, y);
                    lua_setfield(L, -2, "row");

                    lua_pushinteger(L, x_start);
                    lua_setfield(L, -2, "col_start");

                    lua_pushinteger(L, cx - 1);
                    lua_setfield(L, -2, "col_end");

                    lua_pushlstring(L, old_str.data(), old_str.size());
                    lua_setfield(L, -2, "old");

                    lua_pushlstring(L, new_str.data(), new_str.size());
                    lua_setfield(L, -2, "new");

                    lua_rawseti(L, -2, idx++);
                } else {
                    cx++;
                }
            }
        }
        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

bool SnapshotsEqual(const ScreenSnapshot& a, const ScreenSnapshot& b) {
    if (a.width != b.width || a.height != b.height) return false;
    if (a.cursor_x != b.cursor_x || a.cursor_y != b.cursor_y ||
        a.cursor_hidden != b.cursor_hidden)
        return false;
    if (a.cells.size() != b.cells.size()) return false;
    for (size_t i = 0; i < a.cells.size(); ++i) {
        if (a.cells[i].ch != b.cells[i].ch ||
            a.cells[i].attr != b.cells[i].attr) {
            return false;
        }
    }
    return true;
}

int TermWaitForScreenChange(lua_State* L) {
    Terminal* term = GetTerm(L);
    int baseline_id = luaL_checkinteger(L, 1);
    int deadline_ms = luaL_checkinteger(L, 2);
    if (deadline_ms < 0) {
        return luaL_error(L, "Deadline must be non-negative");
    }
    try {
        ScreenSnapshot baseline = term->GetSnapshot(baseline_id);
        auto start_time = std::chrono::steady_clock::now();
        auto d_dur = std::chrono::milliseconds(deadline_ms);

        while (true) {
            ScreenSnapshot current = term->GetSnapshot(-1);
            if (!SnapshotsEqual(baseline, current)) {
                lua_pushstring(L, "changed");
                return 1;
            }
            if (term->IsExited()) {
                lua_pushstring(L, "exited");
                return 1;
            }
            auto now = std::chrono::steady_clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - start_time);
            if (elapsed >= d_dur) {
                lua_pushstring(L, "timeout");
                return 1;
            }
            unsigned int remain = deadline_ms - elapsed.count();
            unsigned int wait_ms = std::min(10u, remain);
            term->WaitIdle(std::min(5u, wait_ms), wait_ms);
        }
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermFindPattern(lua_State* L) {
    Terminal* term = GetTerm(L);
    std::string pattern = luaL_checkstring(L, 1);
    int snap_id = luaL_optinteger(L, 2, -1);
    if (pattern.empty()) {
        return luaL_error(L, "Empty search pattern");
    }
    try {
        ScreenSnapshot snap = term->GetSnapshot(snap_id);
        lua_newtable(L);
        int idx = 1;
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
            size_t start_pos = 0;
            size_t end_pos = 0;
            int init = 1;
            while (true) {
                int top = lua_gettop(L);
                lua_getglobal(L, "string");
                lua_getfield(L, -1, "find");
                lua_remove(L, -2);
                lua_pushlstring(L, row_str.data(), row_str.size());
                lua_pushlstring(L, pattern.data(), pattern.size());
                lua_pushinteger(L, init);
                if (lua_pcall(L, 3, 2, 0) == LUA_OK) {
                    if (lua_isnil(L, -2)) {
                        lua_settop(L, top);
                        break;
                    }
                    int start_idx = lua_tointeger(L, -2);
                    int end_idx = lua_tointeger(L, -1);
                    lua_settop(L, top);

                    unsigned int col_start = char_to_cell[start_idx - 1];
                    unsigned int col_end = char_to_cell[end_idx - 1];

                    lua_newtable(L);
                    lua_pushinteger(L, y);
                    lua_setfield(L, -2, "row");
                    lua_pushinteger(L, col_start);
                    lua_setfield(L, -2, "col_start");
                    lua_pushinteger(L, col_end);
                    lua_setfield(L, -2, "col_end");

                    lua_rawseti(L, -2, idx++);

                    if (end_idx >= start_idx) {
                        init = end_idx + 1;
                    } else {
                        init = start_idx + 1;
                    }
                    if (init > static_cast<int>(row_str.size())) {
                        break;
                    }
                } else {
                    std::string err = lua_tostring(L, -1);
                    lua_settop(L, top);
                    return luaL_error(L, "Lua pattern find error: %s",
                                      err.c_str());
                }
            }
        }
        return 1;
    } catch (const std::exception& e) {
        return luaL_error(L, e.what());
    }
}

int TermWatchPattern(lua_State* L) {
    std::string pattern = luaL_checkstring(L, 1);
    auto* udata = static_cast<WatcherDescriptor*>(
        lua_newuserdatauv(L, sizeof(WatcherDescriptor), 0));
    new (udata) WatcherDescriptor();
    udata->type = WatcherDescriptor::Type::kCustom;
    udata->text = "pattern: " + pattern;
    udata->deadline_ms = 0;

    // WARNING: This predicate lambda captures the lua_State* L by value.
    // This is safe only for in-script waits (e.g. wait_any) since L is
    // guaranteed to be alive during execution. Storing this WatcherDescriptor
    // in any long-lived, session-level, or asynchronous context outside this
    // execution call will result in a dangling pointer when L is closed.
    udata->predicate = [L, pattern](const ScreenSnapshot& snap) -> bool {
        int top = lua_gettop(L);
        lua_getglobal(L, "string");
        if (!lua_istable(L, -1)) {
            lua_settop(L, top);
            return false;
        }
        lua_getfield(L, -1, "find");
        if (!lua_isfunction(L, -1)) {
            lua_settop(L, top);
            return false;
        }
        lua_remove(L, -2);

        bool matched = false;
        for (unsigned int y = 0; y < snap.height; ++y) {
            std::string row_str = app_utils::GetRow(snap, y);
            lua_pushvalue(L, -1);
            lua_pushlstring(L, row_str.data(), row_str.size());
            lua_pushlstring(L, pattern.data(), pattern.size());
            if (lua_pcall(L, 2, 1, 0) == LUA_OK) {
                if (!lua_isnil(L, -1)) {
                    matched = true;
                    lua_pop(L, 1);
                    break;
                }
                lua_pop(L, 1);
            } else {
                lua_pop(L, 1);
            }
        }
        lua_settop(L, top);
        return matched;
    };

    luaL_getmetatable(L, "Termobulator.Watcher");
    lua_setmetatable(L, -2);
    return 1;
}

std::vector<std::string> ParseLuaTableOfStrings(lua_State* L, int arg_idx) {
    luaL_checktype(L, arg_idx, LUA_TTABLE);
    std::vector<std::string> res;
    int table_idx = arg_idx < 0 ? lua_gettop(L) + arg_idx + 1 : arg_idx;
    size_t len = lua_rawlen(L, table_idx);
    for (size_t i = 1; i <= len; ++i) {
        lua_rawgeti(L, table_idx, i);
        if (lua_isstring(L, -1)) {
            res.push_back(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }
    return res;
}

int TermWatchAnyText(lua_State* L) {
    auto texts = ParseLuaTableOfStrings(L, 1);
    if (texts.empty()) {
        return luaL_error(
            L, "watch_any_text requires a non-empty table of strings");
    }

    for (auto& txt : texts) {
        txt = Terminal::ParseEscapes(txt);
    }

    auto* udata = static_cast<WatcherDescriptor*>(
        lua_newuserdatauv(L, sizeof(WatcherDescriptor), 0));
    new (udata) WatcherDescriptor();
    udata->type = WatcherDescriptor::Type::kCustom;
    udata->text = "any_text (" + std::to_string(texts.size()) + " items)";
    udata->deadline_ms = 0;

    udata->predicate = [texts](const ScreenSnapshot& snap) -> bool {
        for (unsigned int y = 0; y < snap.height; ++y) {
            std::string row_str = app_utils::GetRow(snap, y);
            for (const auto& text : texts) {
                if (row_str.find(text) != std::string::npos) {
                    return true;
                }
            }
        }
        return false;
    };

    luaL_getmetatable(L, "Termobulator.Watcher");
    lua_setmetatable(L, -2);
    return 1;
}

int TermWatchAnyPattern(lua_State* L) {
    auto patterns = ParseLuaTableOfStrings(L, 1);
    if (patterns.empty()) {
        return luaL_error(
            L, "watch_any_pattern requires a non-empty table of patterns");
    }

    auto* udata = static_cast<WatcherDescriptor*>(
        lua_newuserdatauv(L, sizeof(WatcherDescriptor), 0));
    new (udata) WatcherDescriptor();
    udata->type = WatcherDescriptor::Type::kCustom;
    udata->text =
        "any_pattern (" + std::to_string(patterns.size()) + " items)";
    udata->deadline_ms = 0;

    // WARNING: This predicate lambda captures the lua_State* L by value.
    // This is safe only for in-script waits (e.g. wait_any) since L is
    // guaranteed to be alive during execution. Storing this WatcherDescriptor
    // in any long-lived, session-level, or asynchronous context outside this
    // execution call will result in a dangling pointer when L is closed.
    udata->predicate = [L, patterns](const ScreenSnapshot& snap) -> bool {
        int top = lua_gettop(L);
        lua_getglobal(L, "string");
        if (!lua_istable(L, -1)) {
            lua_settop(L, top);
            return false;
        }
        lua_getfield(L, -1, "find");
        if (!lua_isfunction(L, -1)) {
            lua_settop(L, top);
            return false;
        }
        lua_remove(L, -2);

        bool matched = false;
        for (unsigned int y = 0; y < snap.height; ++y) {
            std::string row_str = app_utils::GetRow(snap, y);
            for (const auto& pattern : patterns) {
                lua_pushvalue(L, -1);
                lua_pushlstring(L, row_str.data(), row_str.size());
                lua_pushlstring(L, pattern.data(), pattern.size());
                if (lua_pcall(L, 2, 1, 0) == LUA_OK) {
                    if (!lua_isnil(L, -1)) {
                        matched = true;
                        lua_pop(L, 1);
                        break;
                    }
                    lua_pop(L, 1);
                } else {
                    lua_pop(L, 1);
                }
            }
            if (matched) break;
        }
        lua_settop(L, top);
        return matched;
    };

    luaL_getmetatable(L, "Termobulator.Watcher");
    lua_setmetatable(L, -2);
    return 1;
}

void SandboxState(lua_State* L) {
    // Open allowed libraries
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "utf8", luaopen_utf8, 1);
    lua_pop(L, 1);

    // Prune base globals
    const char* banned_globals[] = {"print",         "warn",   "load",
                                    "loadfile",      "dofile", "require",
                                    "collectgarbage"};
    for (const char* name : banned_globals) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }

    // Prune string.dump
    lua_getglobal(L, "string");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
    }
    lua_pop(L, 1);
}

void PushJsonValue(lua_State* L, const nlohmann::json& val) {
    if (val.is_null()) {
        lua_pushnil(L);
    } else if (val.is_boolean()) {
        lua_pushboolean(L, val.get<bool>());
    } else if (val.is_number_integer()) {
        lua_pushinteger(L, val.get<int64_t>());
    } else if (val.is_number_float()) {
        lua_pushnumber(L, val.get<double>());
    } else if (val.is_string()) {
        std::string s = val.get<std::string>();
        lua_pushlstring(L, s.data(), s.size());
    } else if (val.is_array()) {
        lua_newtable(L);
        int i = 1;
        for (const auto& item : val) {
            PushJsonValue(L, item);
            lua_rawseti(L, -2, i++);
        }
    } else if (val.is_object()) {
        lua_newtable(L);
        for (auto it = val.begin(); it != val.end(); ++it) {
            lua_pushlstring(L, it.key().data(), it.key().size());
            PushJsonValue(L, it.value());
            lua_rawset(L, -3);
        }
    }
}

std::optional<nlohmann::json> LuaToJson(lua_State* L, int idx,
                                        std::vector<std::string>& warnings,
                                        std::string_view path, int depth = 0) {
    if (depth > 16) {
        warnings.push_back("Max serialization depth exceeded at '" +
                           std::string(path) + "'");
        return std::nullopt;
    }
    int type = lua_type(L, idx);
    switch (type) {
    case LUA_TNIL:
        return nlohmann::json(nullptr);
    case LUA_TBOOLEAN:
        return nlohmann::json(static_cast<bool>(lua_toboolean(L, idx)));
    case LUA_TNUMBER: {
        if (lua_isinteger(L, idx)) {
            return nlohmann::json(static_cast<int64_t>(lua_tointeger(L, idx)));
        } else {
            return nlohmann::json(static_cast<double>(lua_tonumber(L, idx)));
        }
    }
    case LUA_TSTRING: {
        size_t len;
        const char* s = lua_tolstring(L, idx, &len);
        return nlohmann::json(std::string(s, len));
    }
    case LUA_TTABLE: {
        bool is_array = true;
        size_t max_key = 0;
        size_t count = 0;

        lua_pushnil(L);
        int table_idx = idx < 0 ? idx - 1 : idx;
        while (lua_next(L, table_idx) != 0) {
            if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
                lua_Integer k = lua_tointeger(L, -2);
                if (k >= 1) {
                    if (static_cast<size_t>(k) > max_key) {
                        max_key = k;
                    }
                } else {
                    is_array = false;
                }
            } else {
                is_array = false;
            }
            count++;
            lua_pop(L, 1);
        }

        if (count > 0 && (max_key != count || !is_array)) {
            is_array = false;
        }

        if (is_array) {
            nlohmann::json arr = nlohmann::json::array();
            for (size_t i = 1; i <= max_key; ++i) {
                lua_rawgeti(L, idx, i);
                std::string subpath =
                    std::string(path) + "[" + std::to_string(i) + "]";
                auto val_opt = LuaToJson(L, -1, warnings, subpath, depth + 1);
                lua_pop(L, 1);
                if (val_opt) {
                    arr.push_back(*val_opt);
                } else {
                    arr.push_back(nullptr);
                }
            }
            return arr;
        } else {
            nlohmann::json obj = nlohmann::json::object();
            lua_pushnil(L);
            while (lua_next(L, table_idx) != 0) {
                std::string key_str;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    key_str = lua_tostring(L, -2);
                } else if (lua_type(L, -2) == LUA_TNUMBER) {
                    lua_pushvalue(L, -2);
                    key_str = lua_tostring(L, -1);
                    lua_pop(L, 1);
                } else {
                    size_t len;
                    lua_pushvalue(L, -2);
                    const char* kt_str = luaL_tolstring(L, -1, &len);
                    std::string kt(kt_str, len);
                    lua_pop(L, 2);
                    warnings.push_back(
                        "Skipped table key of non-string/non-number type '" +
                        kt + "' under '" + std::string(path) + "'");
                    lua_pop(L, 1);
                    continue;
                }
                std::string subpath = std::string(path) + "." + key_str;
                auto val_opt = LuaToJson(L, -1, warnings, subpath, depth + 1);
                if (val_opt) {
                    obj[key_str] = *val_opt;
                } else {
                    warnings.push_back("Dropped value at '" + subpath +
                                       "' because its type (" +
                                       lua_typename(L, lua_type(L, -1)) +
                                       ") cannot be serialized");
                }
                lua_pop(L, 1);
            }
            return obj;
        }
    }
    default:
        return std::nullopt;
    }
}

}  // namespace

LuaExecutor::LuaExecutor(Terminal* term) : term_(term) {}

LuaExecutor::Result LuaExecutor::Execute(std::string_view script) {
    UniqueLuaState L_guard(luaL_newstate());
    lua_State* L = L_guard.get();
    if (!L) {
        throw std::runtime_error("failed to create lua_State");
    }

    SandboxState(L);

    // Register Watcher metatable
    luaL_newmetatable(L, "Termobulator.Watcher");
    lua_pushcfunction(L, WatcherGc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, WatcherToString);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    // Register term table
    if (term_) {
        lua_newtable(L);
        struct Reg {
            const char* name;
            lua_CFunction func;
        };
        Reg term_funcs[] = {
            {"send_key", TermSendKey},
            {"send_special_key", TermSendSpecialKey},
            {"send_signal", TermSendSignal},
            {"sleep_ms", TermSleepMs},
            {"wait_idle", TermWaitIdle},
            {"wait_for_text", TermWaitForText},
            {"watch_text", TermWatchText},
            {"watch_timeout", TermWatchTimeout},
            {"wait_any", TermWaitAny},
            {"get_status", TermGetStatus},
            {"dump_screen", TermDumpScreen},
            {"dump_screen_html", TermDumpScreenHtml},
            {"take_snapshot", TermTakeSnapshot},
            {"get_screen", TermGetScreen},
            {"get_cursor", TermGetCursor},
            {"get_cell", TermGetCell},
            {"get_row", TermGetRow},
            {"get_attributes", TermGetAttributes},
            {"find_text", TermFindText},
            {"get_diff", TermGetDiff},
            {"get_scrollback", TermGetScrollback},
            {"resize", TermResize},
            {"get_terminal_size", TermGetTerminalSize},
            {"set_disable_alternate_screen", TermSetDisableAlternateScreen},
            {"get_keysyms", TermGetKeysyms},
            {"get_rows", TermGetRows},
            {"get_diff_structured", TermGetDiffStructured},
            {"wait_for_screen_change", TermWaitForScreenChange},
            {"find_pattern", TermFindPattern},
            {"watch_pattern", TermWatchPattern},
            {"watch_any_text", TermWatchAnyText},
            {"watch_any_pattern", TermWatchAnyPattern}};
        for (const auto& reg : term_funcs) {
            lua_pushstring(L, reg.name);
            lua_pushlightuserdata(L, term_);
            lua_pushcclosure(L, reg.func, 1);
            lua_rawset(L, -3);
        }
        lua_setglobal(L, "term");
    }

    // Register log() function
    std::vector<std::string> log_buffer;
    lua_pushlightuserdata(L, &log_buffer);
    lua_pushcclosure(L, LogFunc, 1);
    lua_setglobal(L, "log");

    // Inject vars table
    lua_newtable(L);
    for (const auto& [key, val] : store_) {
        lua_pushlstring(L, key.data(), key.size());
        PushJsonValue(L, val);
        lua_rawset(L, -3);
    }
    lua_setglobal(L, "vars");

    // Push error handler
    lua_pushcfunction(L, ErrorHandler);
    int errfunc_idx = lua_gettop(L);

    // Load script
    if (luaL_loadbuffer(L, script.data(), script.size(), "@script") !=
        LUA_OK) {
        std::string err = lua_tostring(L, -1);
        throw std::runtime_error("Lua compile error: " + err);
    }

    // Call script
    if (lua_pcall(L, 0, LUA_MULTRET, errfunc_idx) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        throw std::runtime_error("Lua runtime error: " + err);
    }

    // Capture return values
    int top = lua_gettop(L);
    int num_returns = top - errfunc_idx;
    std::vector<std::string> serialization_warnings;
    nlohmann::json result_val;

    if (num_returns == 0) {
        result_val = nullptr;
    } else if (num_returns == 1) {
        auto val_opt =
            LuaToJson(L, errfunc_idx + 1, serialization_warnings, "result");
        result_val = val_opt ? *val_opt : nullptr;
    } else {
        nlohmann::json arr = nlohmann::json::array();
        for (int i = 0; i < num_returns; ++i) {
            auto val_opt =
                LuaToJson(L, errfunc_idx + 1 + i, serialization_warnings,
                          "result[" + std::to_string(i) + "]");
            arr.push_back(val_opt ? *val_opt : nullptr);
        }
        result_val = arr;
    }

    // Extract vars table
    lua_getglobal(L, "vars");
    if (lua_istable(L, -1)) {
        store_.clear();
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                std::string key = lua_tostring(L, -2);
                auto val_opt =
                    LuaToJson(L, -1, serialization_warnings, "vars." + key);
                if (val_opt) {
                    store_[key] = *val_opt;
                } else {
                    serialization_warnings.push_back(
                        "Dropped var '" + key + "' because its type (" +
                        lua_typename(L, lua_type(L, -1)) +
                        ") cannot be serialized");
                }
            } else {
                size_t len;
                lua_pushvalue(L, -2);
                const char* k_str = luaL_tolstring(L, -1, &len);
                std::string key_repr(k_str, len);
                lua_pop(L, 2);
                serialization_warnings.push_back("Dropped non-string key '" +
                                                 key_repr + "' in vars table");
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // Append warnings to log buffer
    for (const auto& w : serialization_warnings) {
        log_buffer.push_back("[warning] " + w);
    }

    Result res;
    res.result = result_val;
    res.log = log_buffer;
    res.variables = store_;

    return res;
}

}  // namespace unstable
}  // namespace termobulator
