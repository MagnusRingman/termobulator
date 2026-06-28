// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "termobulator_testing.h"

#include <chrono>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "app_utils.h"

namespace termobulator {
namespace unstable {
namespace testing {

namespace {

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

std::string DumpSnapshotText(const ScreenSnapshot& snap) {
    std::string out;
    for (unsigned int y = 0; y < snap.height; ++y) {
        std::string row_str = app_utils::GetRow(snap, y);
        size_t endpos = row_str.find_last_not_of(" \t\r\n");
        if (endpos != std::string::npos) {
            out += row_str.substr(0, endpos + 1);
        }
        out += "\n";
    }
    return out;
}

}  // namespace

// Coordination / Waiting
std::string WaitForText(Terminal* term, std::string_view text,
                        unsigned int deadline_ms) {
    std::string query = Terminal::ParseEscapes(text);
    auto start_time = std::chrono::steady_clock::now();
    auto d_dur = std::chrono::milliseconds(deadline_ms);

    while (true) {
        if (app_utils::IsTextOnScreen(term, query)) {
            return "found";
        }
        if (term->IsExited()) {
            return "exited";
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time);
        if (elapsed >= d_dur) {
            return "timeout";
        }
        unsigned int remain = deadline_ms - elapsed.count();
        unsigned int wait_ms = std::min(10u, remain);
        term->WaitIdle(std::min(5u, wait_ms), wait_ms);
    }
}

std::string WaitForScreenChange(Terminal* term, int baseline_id,
                                unsigned int deadline_ms) {
    ScreenSnapshot baseline = term->GetSnapshot(baseline_id);
    auto start_time = std::chrono::steady_clock::now();
    auto d_dur = std::chrono::milliseconds(deadline_ms);

    while (true) {
        ScreenSnapshot current = term->GetSnapshot(-1);
        if (!SnapshotsEqual(baseline, current)) {
            return "changed";
        }
        if (term->IsExited()) {
            return "exited";
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time);
        if (elapsed >= d_dur) {
            return "timeout";
        }
        unsigned int remain = deadline_ms - elapsed.count();
        unsigned int wait_ms = std::min(10u, remain);
        term->WaitIdle(std::min(5u, wait_ms), wait_ms);
    }
}

// Watcher Factories
WatcherDescriptor WatchText(std::string_view text) {
    WatcherDescriptor udata;
    udata.type = WatcherDescriptor::Type::kText;
    udata.text = Terminal::ParseEscapes(text);
    return udata;
}

WatcherDescriptor WatchTimeout(unsigned int ms) {
    WatcherDescriptor udata;
    udata.type = WatcherDescriptor::Type::kTimeout;
    udata.deadline_ms = ms;
    udata.absolute_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    return udata;
}

WatcherDescriptor WatchPattern(std::string_view pattern) {
    WatcherDescriptor udata;
    udata.type = WatcherDescriptor::Type::kCustom;
    udata.text = "pattern: " + std::string(pattern);
    udata.deadline_ms = 0;
    std::regex re{std::string(pattern)};
    udata.predicate = [re](const ScreenSnapshot& snap) -> bool {
        for (unsigned int y = 0; y < snap.height; ++y) {
            std::string row_str = app_utils::GetRow(snap, y);
            if (std::regex_search(row_str, re)) {
                return true;
            }
        }
        return false;
    };
    return udata;
}

WatcherDescriptor WatchAnyText(const std::vector<std::string>& texts) {
    std::vector<std::string> parsed_texts;
    parsed_texts.reserve(texts.size());
    for (const auto& t : texts) {
        parsed_texts.push_back(Terminal::ParseEscapes(t));
    }
    WatcherDescriptor udata;
    udata.type = WatcherDescriptor::Type::kCustom;
    udata.text = "any_text (" + std::to_string(texts.size()) + " items)";
    udata.deadline_ms = 0;
    udata.predicate = [parsed_texts](const ScreenSnapshot& snap) -> bool {
        for (unsigned int y = 0; y < snap.height; ++y) {
            std::string row_str = app_utils::GetRow(snap, y);
            for (const auto& txt : parsed_texts) {
                if (row_str.find(txt) != std::string::npos) {
                    return true;
                }
            }
        }
        return false;
    };
    return udata;
}

WatcherDescriptor WatchAnyPattern(const std::vector<std::string>& patterns) {
    std::vector<std::regex> regexes;
    regexes.reserve(patterns.size());
    for (const auto& p : patterns) {
        regexes.emplace_back(p);
    }
    WatcherDescriptor udata;
    udata.type = WatcherDescriptor::Type::kCustom;
    udata.text = "any_pattern (" + std::to_string(patterns.size()) + " items)";
    udata.deadline_ms = 0;
    udata.predicate = [regexes](const ScreenSnapshot& snap) -> bool {
        for (unsigned int y = 0; y < snap.height; ++y) {
            std::string row_str = app_utils::GetRow(snap, y);
            for (const auto& re : regexes) {
                if (std::regex_search(row_str, re)) {
                    return true;
                }
            }
        }
        return false;
    };
    return udata;
}

// Inspection / Extraction
std::string GetScreen(Terminal* term, int snap_id) {
    ScreenSnapshot snap = term->GetSnapshot(snap_id);
    std::string screen_str;
    for (unsigned int y = 0; y < snap.height; ++y) {
        if (y > 0) screen_str += "\n";
        std::string row_str = app_utils::GetRow(snap, y);
        size_t endpos = row_str.find_last_not_of(" \t\r\n");
        if (endpos != std::string::npos) {
            screen_str += row_str.substr(0, endpos + 1);
        }
    }
    return screen_str;
}

std::string GetRow(Terminal* term, unsigned int row, int snap_id) {
    ScreenSnapshot snap = term->GetSnapshot(snap_id);
    if (row >= snap.height) {
        throw std::out_of_range("Row index out of range");
    }
    std::string row_str = app_utils::GetRow(snap, row);
    size_t endpos = row_str.find_last_not_of(" \t\r\n");
    if (endpos != std::string::npos) {
        row_str = row_str.substr(0, endpos + 1);
    } else {
        row_str.clear();
    }
    return row_str;
}

std::vector<std::string> GetRows(Terminal* term, unsigned int start_row,
                                 unsigned int end_row, int snap_id) {
    ScreenSnapshot snap = term->GetSnapshot(snap_id);
    if (start_row >= snap.height || end_row >= snap.height ||
        start_row > end_row) {
        throw std::out_of_range("Invalid row range");
    }
    std::vector<std::string> res;
    for (unsigned int r = start_row; r <= end_row; ++r) {
        std::string row_str = app_utils::GetRow(snap, r);
        size_t endpos = row_str.find_last_not_of(" \t\r\n");
        if (endpos != std::string::npos) {
            row_str = row_str.substr(0, endpos + 1);
        } else {
            row_str.clear();
        }
        res.push_back(row_str);
    }
    return res;
}

std::string DumpScreenHtml(Terminal* term, int snap_id) {
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
                out += "<span style=\"" +
                       get_style_str(cell_state.attr, cell_state.is_cursor) +
                       "\">";
                current_state = cell_state;
                has_open_span = true;
            } else if (cell_state != current_state) {
                out += "</span>";
                out += "<span style=\"" +
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
    return out;
}

CursorInfo GetCursor(Terminal* term, int snap_id) {
    ScreenSnapshot snap = term->GetSnapshot(snap_id);
    return {snap.cursor_x, snap.cursor_y, !snap.cursor_hidden};
}

// Search & Diffing
std::vector<TextMatch> FindText(Terminal* term, std::string_view text,
                                int snap_id) {
    std::string query = Terminal::ParseEscapes(text);
    if (query.empty()) {
        throw std::invalid_argument("Empty search query");
    }
    ScreenSnapshot snap = term->GetSnapshot(snap_id);
    std::vector<TextMatch> matches;
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
            matches.push_back({y, col_start, col_end});
            pos = row_str.find(query, pos + 1);
        }
    }
    return matches;
}

std::vector<TextMatch> FindPattern(Terminal* term, std::string_view pattern,
                                   int snap_id) {
    if (pattern.empty()) {
        throw std::invalid_argument("Empty search pattern");
    }
    std::regex re{std::string(pattern)};
    ScreenSnapshot snap = term->GetSnapshot(snap_id);
    std::vector<TextMatch> matches;
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
        auto words_begin =
            std::sregex_iterator(row_str.cbegin(), row_str.cend(), re);
        auto words_end = std::sregex_iterator();
        for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
            std::smatch match = *i;
            size_t pos = match.position();
            size_t len = match.length();
            if (len > 0) {
                unsigned int col_start = char_to_cell[pos];
                unsigned int col_end = char_to_cell[pos + len - 1];
                matches.push_back({y, col_start, col_end});
            }
        }
    }
    return matches;
}

std::string GetDiff(Terminal* term, int snap_a, int snap_b) {
    ScreenSnapshot prev = term->GetSnapshot(snap_a);
    ScreenSnapshot curr = term->GetSnapshot(snap_b);
    if (curr.width != prev.width || curr.height != prev.height) {
        throw std::invalid_argument("Snapshot dimensions mismatch");
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
                       std::to_string(x_start) + "-" + std::to_string(cx - 1) +
                       ": \"" + old_str + "\" -> \"" + new_str + "\"\n";
            } else {
                cx++;
            }
        }
    }
    return out;
}

std::vector<DiffStructuredEntry> GetDiffStructured(Terminal* term, int snap_a,
                                                   int snap_b) {
    ScreenSnapshot prev = term->GetSnapshot(snap_a);
    ScreenSnapshot curr = term->GetSnapshot(snap_b);
    if (curr.width != prev.width || curr.height != prev.height) {
        throw std::invalid_argument("Snapshot dimensions mismatch");
    }
    std::vector<DiffStructuredEntry> entries;
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
                entries.push_back({y, x_start, cx - 1, old_str, new_str});
            } else {
                cx++;
            }
        }
    }
    return entries;
}

// Google Test / Google Mock Matchers Implementation

ScreenContainsMatcher::ScreenContainsMatcher(std::string text)
        : text_(std::move(text)) {}

bool ScreenContainsMatcher::MatchAndExplain(
    const ScreenSnapshot& snap,
    ::testing::MatchResultListener* listener) const {
    std::string query = Terminal::ParseEscapes(text_);
    bool found = false;
    for (unsigned int y = 0; y < snap.height; ++y) {
        std::string row_str = app_utils::GetRow(snap, y);
        if (row_str.find(query) != std::string::npos) {
            found = true;
            break;
        }
    }
    if (!found && listener->IsInterested()) {
        *listener << "\nActual screen content:\n" << DumpSnapshotText(snap);
    }
    return found;
}

bool ScreenContainsMatcher::MatchAndExplain(
    Terminal* term, ::testing::MatchResultListener* listener) const {
    if (!term) {
        *listener << "Terminal is null";
        return false;
    }
    return MatchAndExplain(term->GetSnapshot(-1), listener);
}

bool ScreenContainsMatcher::MatchAndExplain(
    const Terminal& term, ::testing::MatchResultListener* listener) const {
    return MatchAndExplain(const_cast<Terminal&>(term).GetSnapshot(-1),
                           listener);
}

void ScreenContainsMatcher::DescribeTo(std::ostream* os) const {
    *os << "contains text \"" << text_ << "\"";
}

void ScreenContainsMatcher::DescribeNegationTo(std::ostream* os) const {
    *os << "does not contain text \"" << text_ << "\"";
}

ScreenContainsMatcher ScreenContains(std::string_view text) {
    return ScreenContainsMatcher(std::string(text));
}

RowContainsMatcher::RowContainsMatcher(unsigned int row, std::string text)
        : row_(row), text_(std::move(text)) {}

bool RowContainsMatcher::MatchAndExplain(
    const ScreenSnapshot& snap,
    ::testing::MatchResultListener* listener) const {
    if (row_ >= snap.height) {
        *listener << "row index " << row_ << " is out of bounds (height is "
                  << snap.height << ")";
        return false;
    }
    std::string query = Terminal::ParseEscapes(text_);
    std::string row_str = app_utils::GetRow(snap, row_);
    bool found = row_str.find(query) != std::string::npos;
    if (!found && listener->IsInterested()) {
        *listener << "\nActual content of row " << row_ << ": \"" << row_str
                  << "\"\n";
    }
    return found;
}

bool RowContainsMatcher::MatchAndExplain(
    Terminal* term, ::testing::MatchResultListener* listener) const {
    if (!term) {
        *listener << "Terminal is null";
        return false;
    }
    return MatchAndExplain(term->GetSnapshot(-1), listener);
}

bool RowContainsMatcher::MatchAndExplain(
    const Terminal& term, ::testing::MatchResultListener* listener) const {
    return MatchAndExplain(const_cast<Terminal&>(term).GetSnapshot(-1),
                           listener);
}

void RowContainsMatcher::DescribeTo(std::ostream* os) const {
    *os << "row " << row_ << " contains text \"" << text_ << "\"";
}

void RowContainsMatcher::DescribeNegationTo(std::ostream* os) const {
    *os << "row " << row_ << " does not contain text \"" << text_ << "\"";
}

RowContainsMatcher RowContains(unsigned int row, std::string_view text) {
    return RowContainsMatcher(row, std::string(text));
}

ScreenMatchesPatternMatcher::ScreenMatchesPatternMatcher(std::string pattern)
        : pattern_(std::move(pattern)) {}

bool ScreenMatchesPatternMatcher::MatchAndExplain(
    const ScreenSnapshot& snap,
    ::testing::MatchResultListener* listener) const {
    std::regex re(pattern_);
    bool found = false;
    for (unsigned int y = 0; y < snap.height; ++y) {
        std::string row_str = app_utils::GetRow(snap, y);
        if (std::regex_search(row_str, re)) {
            found = true;
            break;
        }
    }
    if (!found && listener->IsInterested()) {
        *listener << "\nActual screen content:\n" << DumpSnapshotText(snap);
    }
    return found;
}

bool ScreenMatchesPatternMatcher::MatchAndExplain(
    Terminal* term, ::testing::MatchResultListener* listener) const {
    if (!term) {
        *listener << "Terminal is null";
        return false;
    }
    return MatchAndExplain(term->GetSnapshot(-1), listener);
}

bool ScreenMatchesPatternMatcher::MatchAndExplain(
    const Terminal& term, ::testing::MatchResultListener* listener) const {
    return MatchAndExplain(const_cast<Terminal&>(term).GetSnapshot(-1),
                           listener);
}

void ScreenMatchesPatternMatcher::DescribeTo(std::ostream* os) const {
    *os << "matches pattern \"" << pattern_ << "\"";
}

void ScreenMatchesPatternMatcher::DescribeNegationTo(std::ostream* os) const {
    *os << "does not match pattern \"" << pattern_ << "\"";
}

ScreenMatchesPatternMatcher ScreenMatchesPattern(std::string_view pattern) {
    return ScreenMatchesPatternMatcher(std::string(pattern));
}

HasCursorAtMatcher::HasCursorAtMatcher(unsigned int x, unsigned int y,
                                       bool visible)
        : x_(x), y_(y), visible_(visible) {}

bool HasCursorAtMatcher::MatchAndExplain(
    const ScreenSnapshot& snap,
    ::testing::MatchResultListener* listener) const {
    bool x_match = snap.cursor_x == x_;
    bool y_match = snap.cursor_y == y_;
    bool vis_match = (!snap.cursor_hidden) == visible_;
    bool success = x_match && y_match && vis_match;
    if (!success && listener->IsInterested()) {
        *listener << "cursor is actually at (" << snap.cursor_x << ", "
                  << snap.cursor_y << ") and is "
                  << (snap.cursor_hidden ? "hidden" : "visible");
    }
    return success;
}

bool HasCursorAtMatcher::MatchAndExplain(
    Terminal* term, ::testing::MatchResultListener* listener) const {
    if (!term) {
        *listener << "Terminal is null";
        return false;
    }
    return MatchAndExplain(term->GetSnapshot(-1), listener);
}

bool HasCursorAtMatcher::MatchAndExplain(
    const Terminal& term, ::testing::MatchResultListener* listener) const {
    return MatchAndExplain(const_cast<Terminal&>(term).GetSnapshot(-1),
                           listener);
}

void HasCursorAtMatcher::DescribeTo(std::ostream* os) const {
    *os << "has cursor at (" << x_ << ", " << y_ << ") and is "
        << (visible_ ? "visible" : "hidden");
}

void HasCursorAtMatcher::DescribeNegationTo(std::ostream* os) const {
    *os << "does not have cursor at (" << x_ << ", " << y_ << ") and is "
        << (visible_ ? "visible" : "hidden");
}

HasCursorAtMatcher HasCursorAt(unsigned int x, unsigned int y, bool visible) {
    return HasCursorAtMatcher(x, y, visible);
}

}  // namespace testing
}  // namespace unstable
}  // namespace termobulator
