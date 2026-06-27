// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_TERMINAL_LEVELS_H
#define TERMOBULATOR_TERMINAL_LEVELS_H

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "inlined_docs.h"

namespace termobulator {

struct TerminalLevel {
    std::string_view name;           // Symbolic name (e.g. "basic-8color")
    std::string_view terminfo_name;  // Resolved terminfo entry name
    std::string_view description;
};

// All 15 entries from the combinatorial matrix, in declaration order.
inline constexpr std::array<TerminalLevel, 15> kTerminalLevels{{
    {"minimal", "dumb",
     "Line-mode only. No cursor addressing, no color, no styling."},
    {"basic", "vt100",
     "Cursor addressing, bold, underline, reverse video. No color."},
    {"basic-mono", "vt100", "Alias for basic (vt100 is already colorless)."},
    {"basic-8color", "termobulator-basic-8color",
     "vt100 base with 8 ANSI colors (SGR 30-37/40-47)."},
    {"basic-16color", "termobulator-basic-16color",
     "vt100 base with 16 colors (8 normal + 8 bright)."},
    {"basic-256color", "termobulator-basic-256color",
     "vt100 base with full 256-color palette."},
    {"extended", "vt220",
     "Adds insert/delete line, full function-key set, cursor visibility "
     "control."},
    {"extended-mono", "vt220",
     "Alias for extended (vt220 has no color capabilities)."},
    {"extended-8color", "termobulator-extended-8color",
     "vt220 base with 8 ANSI colors (SGR 30-37/40-47)."},
    {"extended-16color", "termobulator-extended-16color",
     "vt220 base with 16 colors (8 normal + 8 bright)."},
    {"extended-256color", "termobulator-extended-256color",
     "vt220 base with full 256-color palette."},
    {"full", "xterm-256color",
     "256-color palette, mouse tracking, bracketed paste, italic, "
     "strikethrough. The default."},
    {"full-256color", "xterm-256color", "Alias for full (xterm-256color)."},
    {"full-mono", "termobulator-full-mono",
     "xterm-256color base with all color capabilities removed."},
    {"full-8color", "termobulator-full-8color",
     "xterm-256color base reduced to 8 ANSI colors."},
    // Note: full-16color maps to the standard xterm-16color entry.
}};

// xterm-16color is a standard entry (not synthetic), but not in the table
// above because it has two symbolic names. It is listed separately in the
// known set.

// Case-insensitive lookup. Returns the mapped terminfo name if input matches a
// symbolic name; returns input unchanged if it does not match any symbolic
// name.
inline std::string ResolveTerminalType(const std::string& input) {
    std::string lower = input;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // full-16color handled separately (not a primary row in the table)
    if (lower == "full-16color") return "xterm-16color";

    for (const auto& level : kTerminalLevels) {
        if (lower == level.name) {
            return std::string(level.terminfo_name);
        }
    }
    return input;
}

// Returns true if terminfo_name is in the known-good set: the distinct
// terminfo names from the 15-entry matrix plus full-16color, tmux-256color,
// screen-256color, ansi, and vt420.
inline bool IsKnownTerminalType(const std::string& terminfo_name) {
    static const std::array<std::string_view, 16> kKnownTerminfo{{
        "dumb", "vt100", "vt220", "xterm-256color", "xterm-16color",
        "tmux-256color", "termobulator-basic-8color",
        "termobulator-basic-16color", "termobulator-basic-256color",
        "termobulator-extended-8color", "termobulator-extended-16color",
        "termobulator-extended-256color", "termobulator-full-mono",
        "termobulator-full-8color",
        // Extra well-known entries
        "screen-256color", "ansi",
        // vt420 intentionally omitted from synthetics but accepted without
        // warning
    }};
    // Also accept vt420 explicitly
    if (terminfo_name == "vt420") return true;
    for (const auto& t : kKnownTerminfo) {
        if (terminfo_name == t) return true;
    }
    return false;
}

// Returns the full markdown reference text served by the
// termobulator://docs/terminal-levels MCP resource.
inline std::string GetTerminalLevelsDocumentation() {
    return doc::GetTerminalLevelsDocumentation();
}

}  // namespace termobulator

#endif  // TERMOBULATOR_TERMINAL_LEVELS_H
