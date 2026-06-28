// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_TESTING_H
#define TERMOBULATOR_TESTING_H

#include <gmock/gmock.h>

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "termobulator.h"

namespace termobulator {
namespace unstable {
namespace testing {

// Search and Match Structs
struct TextMatch {
    unsigned int row;
    unsigned int col_start;
    unsigned int col_end;
};

struct DiffStructuredEntry {
    unsigned int row;
    unsigned int col_start;
    unsigned int col_end;
    std::string old_val;
    std::string new_val;
};

struct CursorInfo {
    unsigned int x;
    unsigned int y;
    bool visible;
};

// Coordination / Waiting (polling under the hood)
std::string WaitForText(Terminal* term, std::string_view text,
                        unsigned int deadline_ms);
std::string WaitForScreenChange(Terminal* term, int baseline_id,
                                unsigned int deadline_ms);

// Watcher Factories
WatcherDescriptor WatchText(std::string_view text);
WatcherDescriptor WatchTimeout(unsigned int ms);
WatcherDescriptor WatchPattern(std::string_view pattern);
WatcherDescriptor WatchAnyText(const std::vector<std::string>& texts);
WatcherDescriptor WatchAnyPattern(const std::vector<std::string>& patterns);

// Inspection / Extraction
std::string GetScreen(Terminal* term, int snap_id = -1);
std::string GetRow(Terminal* term, unsigned int row, int snap_id = -1);
std::vector<std::string> GetRows(Terminal* term, unsigned int start_row,
                                 unsigned int end_row, int snap_id = -1);
std::string DumpScreenHtml(Terminal* term, int snap_id = -1);
CursorInfo GetCursor(Terminal* term, int snap_id = -1);

// Search & Diffing
std::vector<TextMatch> FindText(Terminal* term, std::string_view text,
                                int snap_id = -1);
std::vector<TextMatch> FindPattern(Terminal* term, std::string_view pattern,
                                   int snap_id = -1);
std::string GetDiff(Terminal* term, int snap_a, int snap_b);
std::vector<DiffStructuredEntry> GetDiffStructured(Terminal* term, int snap_a,
                                                   int snap_b);

// Google Test / Google Mock Matchers
class ScreenContainsMatcher {
  public:
    using is_gtest_matcher = void;
    explicit ScreenContainsMatcher(std::string text);
    bool MatchAndExplain(const ScreenSnapshot& snap,
                         ::testing::MatchResultListener* listener) const;
    bool MatchAndExplain(Terminal* term,
                         ::testing::MatchResultListener* listener) const;
    bool MatchAndExplain(const Terminal& term,
                         ::testing::MatchResultListener* listener) const;
    void DescribeTo(std::ostream* os) const;
    void DescribeNegationTo(std::ostream* os) const;

  private:
    std::string text_;
};

ScreenContainsMatcher ScreenContains(std::string_view text);

class RowContainsMatcher {
  public:
    using is_gtest_matcher = void;
    RowContainsMatcher(unsigned int row, std::string text);
    bool MatchAndExplain(const ScreenSnapshot& snap,
                         ::testing::MatchResultListener* listener) const;
    bool MatchAndExplain(Terminal* term,
                         ::testing::MatchResultListener* listener) const;
    bool MatchAndExplain(const Terminal& term,
                         ::testing::MatchResultListener* listener) const;
    void DescribeTo(std::ostream* os) const;
    void DescribeNegationTo(std::ostream* os) const;

  private:
    unsigned int row_;
    std::string text_;
};

RowContainsMatcher RowContains(unsigned int row, std::string_view text);

class ScreenMatchesPatternMatcher {
  public:
    using is_gtest_matcher = void;
    explicit ScreenMatchesPatternMatcher(std::string pattern);
    bool MatchAndExplain(const ScreenSnapshot& snap,
                         ::testing::MatchResultListener* listener) const;
    bool MatchAndExplain(Terminal* term,
                         ::testing::MatchResultListener* listener) const;
    bool MatchAndExplain(const Terminal& term,
                         ::testing::MatchResultListener* listener) const;
    void DescribeTo(std::ostream* os) const;
    void DescribeNegationTo(std::ostream* os) const;

  private:
    std::string pattern_;
};

ScreenMatchesPatternMatcher ScreenMatchesPattern(std::string_view pattern);

class HasCursorAtMatcher {
  public:
    using is_gtest_matcher = void;
    HasCursorAtMatcher(unsigned int x, unsigned int y, bool visible);
    bool MatchAndExplain(const ScreenSnapshot& snap,
                         ::testing::MatchResultListener* listener) const;
    bool MatchAndExplain(Terminal* term,
                         ::testing::MatchResultListener* listener) const;
    bool MatchAndExplain(const Terminal& term,
                         ::testing::MatchResultListener* listener) const;
    void DescribeTo(std::ostream* os) const;
    void DescribeNegationTo(std::ostream* os) const;

  private:
    unsigned int x_;
    unsigned int y_;
    bool visible_;
};

HasCursorAtMatcher HasCursorAt(unsigned int x, unsigned int y,
                               bool visible = true);

}  // namespace testing
}  // namespace unstable
}  // namespace termobulator

#endif  // TERMOBULATOR_TESTING_H
