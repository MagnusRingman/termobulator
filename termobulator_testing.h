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
/**
 * @namespace termobulator::unstable::testing
 * @brief Contains the C++ testing API, including helper utilities and Google
 * Mock matchers for automating TUI visual regression and interactive tests.
 */
namespace testing {

/**
 * @struct TextMatch
 * @brief Represents a coordinate match for text segments found on the screen.
 */
struct TextMatch {
    unsigned int row;        ///< 0-indexed row (y-coordinate) of the match.
    unsigned int col_start;  ///< 0-indexed starting column (x-coordinate).
    unsigned int col_end;    ///< 0-indexed ending column (inclusive).
};

/**
 * @struct DiffStructuredEntry
 * @brief Represents a detailed difference between two screen snapshots at a
 * specific region.
 */
struct DiffStructuredEntry {
    unsigned int row;  ///< Row index where the difference was detected.
    unsigned int
        col_start;  ///< Starting column of the contiguous difference block.
    unsigned int
        col_end;  ///< Ending column of the contiguous difference block.
    std::string old_val;  ///< Original character sequence.
    std::string new_val;  ///< Modified character sequence.
};

/**
 * @struct CursorInfo
 * @brief Represents the coordinate and visibility state of the terminal
 * cursor.
 */
struct CursorInfo {
    unsigned int x;  ///< 0-indexed x-coordinate (column).
    unsigned int y;  ///< 0-indexed y-coordinate (row).
    bool visible;    ///< True if the cursor is visible, false if hidden.
};

/**
 * @brief Blocks the calling thread until the specified text is found on the
 * screen, or the deadline is reached.
 * @param term Pointer to the Terminal instance.
 * @param text Text string to search for (supports escape sequence resolution).
 * @param deadline_ms Maximum time to wait in milliseconds.
 * @return "found" if the text appears, "exited" if the subprocess terminates,
 *         or "timeout" if the deadline expires.
 */
std::string WaitForText(Terminal* term, std::string_view text,
                        unsigned int deadline_ms);

/**
 * @brief Blocks the calling thread until the screen content changes from the
 * baseline snapshot, or the deadline is reached.
 * @param term Pointer to the Terminal instance.
 * @param baseline_id Snapshot ID representing the baseline state.
 * @param deadline_ms Maximum time to wait in milliseconds.
 * @return "changed" if the screen changes, "exited" if the subprocess
 * terminates, or "timeout" if the deadline expires.
 */
std::string WaitForScreenChange(Terminal* term, int baseline_id,
                                unsigned int deadline_ms);

/**
 * @brief Creates a watcher descriptor configured to trigger when specific text
 * appears on screen.
 * @param text The text query to match.
 */
WatcherDescriptor WatchText(std::string_view text);

/**
 * @brief Creates a watcher descriptor configured to trigger after a timeout.
 * @param ms Timeout duration in milliseconds.
 */
WatcherDescriptor WatchTimeout(unsigned int ms);

/**
 * @brief Creates a watcher descriptor configured to trigger when a regular
 * expression matches.
 * @param pattern The std::regex-compatible pattern.
 */
WatcherDescriptor WatchPattern(std::string_view pattern);

/**
 * @brief Creates a watcher descriptor configured to trigger when any of the
 * specified text items appear.
 * @param texts Vector of text strings.
 */
WatcherDescriptor WatchAnyText(const std::vector<std::string>& texts);

/**
 * @brief Creates a watcher descriptor configured to trigger when any of the
 * specified regex patterns match.
 * @param patterns Vector of regex pattern strings.
 */
WatcherDescriptor WatchAnyPattern(const std::vector<std::string>& patterns);

/**
 * @brief Retrieves the entire screen content as a single string.
 * @param term Pointer to the Terminal instance.
 * @param snap_id Optional snapshot ID (defaults to -1 for current screen).
 * @return Text representation of the screen (rows separated by newlines).
 */
std::string GetScreen(Terminal* term, int snap_id = -1);

/**
 * @brief Retrieves the text content of a single screen row.
 * @param term Pointer to the Terminal instance.
 * @param row 0-indexed row number.
 * @param snap_id Optional snapshot ID.
 * @return The row's text content, stripped of trailing whitespace.
 */
std::string GetRow(Terminal* term, unsigned int row, int snap_id = -1);

/**
 * @brief Retrieves a contiguous block of rows.
 * @param term Pointer to the Terminal instance.
 * @param start_row Starting row index (inclusive).
 * @param end_row Ending row index (inclusive).
 * @param snap_id Optional snapshot ID.
 * @return Vector of strings representing the rows.
 */
std::vector<std::string> GetRows(Terminal* term, unsigned int start_row,
                                 unsigned int end_row, int snap_id = -1);

/**
 * @brief Generates an HTML representation of the screen snapshot with inline
 * styling representing character colors and font attributes.
 * @param term Pointer to the Terminal instance.
 * @param snap_id Optional snapshot ID.
 * @return Styled HTML screen dump wrapped in a <pre> block.
 */
std::string DumpScreenHtml(Terminal* term, int snap_id = -1);

/**
 * @brief Extracts the current cursor information.
 * @param term Pointer to the Terminal instance.
 * @param snap_id Optional snapshot ID.
 */
CursorInfo GetCursor(Terminal* term, int snap_id = -1);

/**
 * @brief Searches the screen for a specific text sequence.
 * @param term Pointer to the Terminal instance.
 * @param text The search query.
 * @param snap_id Optional snapshot ID.
 * @return Vector of coordinates matching the search query.
 */
std::vector<TextMatch> FindText(Terminal* term, std::string_view text,
                                int snap_id = -1);

/**
 * @brief Searches the screen using a regular expression pattern.
 * @param term Pointer to the Terminal instance.
 * @param pattern The std::regex-compatible pattern.
 * @param snap_id Optional snapshot ID.
 * @return Vector of coordinates matching the search pattern.
 */
std::vector<TextMatch> FindPattern(Terminal* term, std::string_view pattern,
                                   int snap_id = -1);

/**
 * @brief Generates a line-by-line text description of changes between two
 * snapshots.
 * @param term Pointer to the Terminal instance.
 * @param snap_a Snapshot ID of baseline.
 * @param snap_b Snapshot ID of modified screen.
 */
std::string GetDiff(Terminal* term, int snap_a, int snap_b);

/**
 * @brief Generates a structured vector of differences between two snapshots.
 * @param term Pointer to the Terminal instance.
 * @param snap_a Snapshot ID of baseline.
 * @param snap_b Snapshot ID of modified screen.
 */
std::vector<DiffStructuredEntry> GetDiffStructured(Terminal* term, int snap_a,
                                                   int snap_b);

// Google Test / Google Mock Matchers

/**
 * @class ScreenContainsMatcher
 * @brief Custom GMock matcher class that asserts that the screen contains a
 * given text. Provides formatted diagnostic output showing the actual screen
 * dump on mismatch.
 */
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

/**
 * @brief GMock matcher checking if the terminal screen contains the specified
 * text.
 * @param text Text segment to search for.
 * @note Usage: EXPECT_THAT(term.get(), ScreenContains("Welcome"));
 */
ScreenContainsMatcher ScreenContains(std::string_view text);

/**
 * @class RowContainsMatcher
 * @brief Custom GMock matcher class that asserts that a specific row contains
 * the text.
 */
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

/**
 * @brief GMock matcher checking if a specific terminal row contains the
 * specified text.
 * @param row 0-indexed row number.
 * @param text Text segment to search for.
 * @note Usage: EXPECT_THAT(term.get(), RowContains(1, "Title"));
 */
RowContainsMatcher RowContains(unsigned int row, std::string_view text);

/**
 * @class ScreenMatchesPatternMatcher
 * @brief Custom GMock matcher class that asserts that the screen matches a
 * regular expression.
 */
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

/**
 * @brief GMock matcher checking if the terminal screen matches a regex
 * pattern.
 * @param pattern The std::regex-compatible pattern.
 * @note Usage: EXPECT_THAT(term.get(), ScreenMatchesPattern("Version:
 * [0-9]+"));
 */
ScreenMatchesPatternMatcher ScreenMatchesPattern(std::string_view pattern);

/**
 * @class HasCursorAtMatcher
 * @brief Custom GMock matcher class that asserts the cursor coordinate and
 * visibility.
 */
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

/**
 * @brief GMock matcher checking if the cursor is at the specified location and
 * visibility state.
 * @param x 0-indexed column.
 * @param y 0-indexed row.
 * @param visible Visibility state of the cursor (defaults to true).
 * @note Usage: EXPECT_THAT(term.get(), HasCursorAt(0, 0, false));
 */
HasCursorAtMatcher HasCursorAt(unsigned int x, unsigned int y,
                               bool visible = true);

}  // namespace testing
}  // namespace unstable
}  // namespace termobulator

#endif  // TERMOBULATOR_TESTING_H
