// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_APP_UTILS_H
#define TERMOBULATOR_APP_UTILS_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "termobulator.h"

namespace termobulator {
namespace app_utils {

struct RgbColor {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

std::pair<std::string, std::string> SplitCommand(const std::string& line);

std::vector<std::string> ParseArgs(const std::string& arg_str);

int ParseInt(const std::string& str, const std::string& name);

std::string FormatColor(int code, uint8_t r, uint8_t g, uint8_t b);

std::string FormatAttr(const termobulator::unstable::CellAttr& attr);

std::optional<RgbColor> ParseRgb(std::string_view s);

termobulator::unstable::CellAttr ParseAttr(std::string_view desc);

std::vector<termobulator::unstable::CellAttr> GetUniqueAttrs(
    const termobulator::unstable::ScreenSnapshot& snap);

std::string GetRow(const termobulator::unstable::ScreenSnapshot& snap,
                   unsigned int y);

bool IsTextOnScreen(termobulator::unstable::Terminal* term,
                    std::string_view query);

}  // namespace app_utils
}  // namespace termobulator

#endif  // TERMOBULATOR_APP_UTILS_H
