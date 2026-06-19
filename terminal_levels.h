// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_TERMINAL_LEVELS_H
#define TERMOBULATOR_TERMINAL_LEVELS_H

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

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
    return
        R"(# Terminal Capability Levels

The `terminal` parameter on `create_session` controls what terminal capabilities
the launched application sees. Termobulator's VTE always emulates the same full
feature set; what changes is the `terminfo` entry the application looks up via
`$TERM`, which determines which features it will try to use.

## Base capability levels

| Symbolic name | `$TERM` value    | Description                                                            |
|---------------|------------------|------------------------------------------------------------------------|
| `minimal`     | `dumb`           | No cursor addressing, no color, no styling. Raw line-mode only.        |
| `basic`       | `vt100`          | Cursor addressing, bold, underline, reverse video. No color.           |
| `extended`    | `vt220`          | Adds insert/delete line, full function-key set (F1-F20), cursor visibility control. |
| `full`        | `xterm-256color` | 256-color palette, mouse tracking, bracketed paste, italic, strikethrough. **Default.** |

## Color depth suffixes

Color depth can be specified independently by appending a suffix to the base level name:

| Suffix       | Colors | Description                                      |
|--------------|--------|--------------------------------------------------|
| `-mono`      | 0      | No color capabilities at all.                    |
| `-8color`    | 8      | Standard ANSI 8-color palette (SGR 30-37/40-47). |
| `-16color`   | 16     | 8 normal + 8 bright (aixterm-style extensions).  |
| `-256color`  | 256    | Full 256-color indexed palette.                  |
| *(no suffix)*|        | Level default: mono for basic/extended, 256color for full. |

## Full combinatorial matrix

| Symbolic name       | `$TERM` value                     |
|---------------------|-----------------------------------|
| `minimal`           | `dumb`                            |
| `basic` / `basic-mono` | `vt100`                        |
| `basic-8color`      | `termobulator-basic-8color`       |
| `basic-16color`     | `termobulator-basic-16color`      |
| `basic-256color`    | `termobulator-basic-256color`     |
| `extended` / `extended-mono` | `vt220`                  |
| `extended-8color`   | `termobulator-extended-8color`    |
| `extended-16color`  | `termobulator-extended-16color`   |
| `extended-256color` | `termobulator-extended-256color`  |
| `full` / `full-256color` | `xterm-256color`             |
| `full-16color`      | `xterm-16color`                   |
| `full-8color`       | `termobulator-full-8color`        |
| `full-mono`         | `termobulator-full-mono`          |

`minimal` has no color variants: `dumb` lacks cursor addressing and SGR support
entirely. Strings like `minimal-8color` are treated as raw terminfo names and
will trigger an unknown-type warning.

## Guidance

- **Compatibility testing**: Use `basic` or `extended` to verify the TUI works on
  legacy terminals without color. Use `minimal` to test graceful degradation.
- **Color regression testing**: Use `full-8color` or `full-16color` to reproduce
  the "angry fruit salad" failure mode where a TUI designed for 256 colors has
  indices clamped to a smaller palette.
- **Raw terminfo names**: Any string not matching a symbolic name is passed through
  as a raw `$TERM` value. If it is not in the known-good set, a `warning` field
  will appear in the `create_session` response.
- **Case-insensitive**: Symbolic names are matched case-insensitively
  (`"FULL-MONO"` and `"full-mono"` are equivalent).
)";
}

}  // namespace termobulator

#endif  // TERMOBULATOR_TERMINAL_LEVELS_H
