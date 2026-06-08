// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "app_utils.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace termobulator {
namespace app_utils {

using termobulator::unstable::CellAttr;
using termobulator::unstable::ScreenSnapshot;
using termobulator::unstable::Terminal;

std::pair<std::string, std::string> SplitCommand(const std::string& line) {
    size_t space = line.find(' ');
    if (space == std::string::npos) {
        return {line, ""};
    }
    return {line.substr(0, space), line.substr(space + 1)};
}

std::vector<std::string> ParseArgs(const std::string& arg_str) {
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

int ParseInt(const std::string& str, const std::string& name) {
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

std::string FormatColor(int code, uint8_t r, uint8_t g, uint8_t b) {
    if (code >= 0) {
        return std::to_string(code);
    }
    return "rgb(" + std::to_string(static_cast<int>(r)) + "," +
           std::to_string(static_cast<int>(g)) + "," +
           std::to_string(static_cast<int>(b)) + ")";
}

std::string FormatAttr(const CellAttr& attr) {
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

std::optional<RgbColor> ParseRgb(std::string_view s) {
    if (s.rfind("rgb(", 0) != 0 || s.back() != ')') {
        return std::nullopt;
    }
    std::string inner(s.substr(4, s.size() - 5));
    size_t first_comma = inner.find(',');
    size_t second_comma = inner.find(',', first_comma + 1);
    if (first_comma == std::string::npos ||
        second_comma == std::string::npos) {
        return std::nullopt;
    }
    try {
        RgbColor color;
        color.r =
            static_cast<uint8_t>(std::stoi(inner.substr(0, first_comma)));
        color.g = static_cast<uint8_t>(std::stoi(
            inner.substr(first_comma + 1, second_comma - first_comma - 1)));
        color.b =
            static_cast<uint8_t>(std::stoi(inner.substr(second_comma + 1)));
        return color;
    } catch (...) {
        return std::nullopt;
    }
}

CellAttr ParseAttr(std::string_view desc) {
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
            if (auto color = ParseRgb(val)) {
                attr.fccode = -1;
                attr.fr = color->r;
                attr.fg = color->g;
                attr.fb = color->b;
            } else {
                attr.fccode = std::stoi(val);
            }
        } else if (token.rfind("bg=", 0) == 0) {
            std::string val = token.substr(3);
            if (auto color = ParseRgb(val)) {
                attr.bccode = -1;
                attr.br = color->r;
                attr.bg = color->g;
                attr.bb = color->b;
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
            std::cerr << "Warning: unknown attribute token: " << token << "\n";
        }
    }
    return attr;
}

std::vector<CellAttr> GetUniqueAttrs(const ScreenSnapshot& snap) {
    std::vector<CellAttr> attrs;
    attrs.reserve(snap.cells.size());
    for (const auto& cell : snap.cells) {
        attrs.push_back(cell.attr);
    }
    std::sort(attrs.begin(), attrs.end());
    attrs.erase(std::unique(attrs.begin(), attrs.end()), attrs.end());
    return attrs;
}

std::string GetRow(const ScreenSnapshot& snap, unsigned int y) {
    std::string row;
    row.reserve(snap.width);
    for (unsigned int x = 0; x < snap.width; ++x) {
        row += snap.cells[y * snap.width + x].ch;
    }
    return row;
}

bool IsTextOnScreen(Terminal* term, std::string_view query) {
    ScreenSnapshot snap = term->GetSnapshot(-1);
    for (unsigned int y = 0; y < snap.height; ++y) {
        if (GetRow(snap, y).find(query) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace app_utils
}  // namespace termobulator
