// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <limits.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../termobulator.h"
#include "../termobulator_testing.h"

// Bring in the unstable testing helpers and custom GMock matchers.
using namespace termobulator::unstable;
using namespace termobulator::unstable::testing;

/**
 * Helper to dynamically locate the crash_test_dummy executable target.
 * Since the test runner is compiled in the same CMake build directory, we
 * locate the target relative to the current test binary's path.
 */
static std::string GetCrashTestDummyPath() {
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        std::string dir(exe_path);
        auto slash = dir.rfind('/');
        if (slash != std::string::npos) {
            return dir.substr(0, slash) + "/crash_test_dummy";
        }
    }
    return "./crash_test_dummy";
}

/**
 * Spin-wait utility to block until the subprocess terminal exits.
 * Sets a safe 2-second timeout to prevent deadlocks in case of unexpected
 * errors.
 */
static void HelperWaitForExit(Terminal* term) {
    auto start = std::chrono::steady_clock::now();
    while (!term->IsExited() && std::chrono::steady_clock::now() - start <
                                    std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/**
 * EXHIBIT 1: Capability Report Exhibit
 *
 * Verifies that the Capability Report renders correctly under a color-capable
 * terminal (e.g. xterm-256color) and displays the expected parameters.
 */
TEST(CrashTestDummyTest, CapabilityReportExhibitInteractive) {
    // Launch the crash test dummy subprocess with a 80x24 terminal in
    // xterm-256color mode.
    auto term = CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                         {"--exhibit", "capability"},
                                         "xterm-256color");
    ASSERT_NE(term, nullptr);

    // Wait for the screen to render the exhibit's unique header.
    // WaitForText polls term->WaitIdle internally.
    std::string wait_res =
        WaitForText(term.get(), "EXHIBIT: Capability Report", 2000);
    EXPECT_EQ(wait_res, "found");

    // Demonstrate ScreenContains matcher: Asserts that a string is present
    // anywhere on the screen.
    EXPECT_THAT(term.get(), ScreenContains("EXHIBIT: Capability Report"));
    EXPECT_THAT(term.get(),
                ScreenContains("TERM environment variable: xterm-256color"));

    // Demonstrate RowContains matcher: Asserts that the specified row index
    // contains the text. Note that row indices are 0-indexed.
    EXPECT_THAT(term.get(), RowContains(1, "EXHIBIT: Capability Report"));

    // Demonstrate ScreenMatchesPattern: Regex pattern-matching on screen
    // content.
    EXPECT_THAT(term.get(), ScreenMatchesPattern("Colors supported.*YES"));
    EXPECT_THAT(term.get(), ScreenMatchesPattern("Maximum colors.*256"));

    // Send 'q' to quit the curses application and verify clean termination.
    term->SendRawBytes("q");
    HelperWaitForExit(term.get());
    EXPECT_TRUE(term->IsExited());
    EXPECT_EQ(term->ExitStatus(), 0);
}

/**
 * EXHIBIT 1 (vt100 Fallback): Capability Report Exhibit
 *
 * Verifies that the Capability Report correctly adapts and shows NO color
 * support when run in a vt100 terminal emulation.
 */
TEST(CrashTestDummyTest, CapabilityReportExhibitVt100) {
    // Launch under vt100 mode, which lacks color capabilities.
    auto term = CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                         {"--exhibit", "capability"}, "vt100");
    ASSERT_NE(term, nullptr);

    std::string wait_res =
        WaitForText(term.get(), "EXHIBIT: Capability Report", 2000);
    EXPECT_EQ(wait_res, "found");

    // Under vt100, curses has_colors() should return false.
    EXPECT_THAT(term.get(), ScreenMatchesPattern("Colors supported.*NO"));
    EXPECT_THAT(term.get(), ScreenMatchesPattern("Maximum colors.*0"));

    term->SendRawBytes("q");
    HelperWaitForExit(term.get());
    EXPECT_TRUE(term->IsExited());
    EXPECT_EQ(term->ExitStatus(), 0);
}

/**
 * EXHIBIT 2: Colors Exhibit
 *
 * Verifies color rendering and terminal color support checks.
 */
TEST(CrashTestDummyTest, ColorsExhibitInteractive) {
    auto term =
        CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                 {"--exhibit", "colors"}, "xterm-256color");
    ASSERT_NE(term, nullptr);

    std::string wait_res =
        WaitForText(term.get(), "EXHIBIT: Color Rendering", 2000);
    EXPECT_EQ(wait_res, "found");

    // Verify max colors and layout rendering
    EXPECT_THAT(term.get(), ScreenContains("COLORS available: 256"));
    EXPECT_THAT(term.get(), ScreenContains("256-color grid:"));

    // Inspect snapshot structures to verify that cells contain background
    // colors
    ScreenSnapshot snap = term->GetSnapshot();
    // Cell at row 6, col 4 is part of the 256-color block grid.
    // The background color code (bccode) should be configured.
    unsigned int grid_cell_idx = 6 * snap.width + 4;
    ASSERT_LT(grid_cell_idx, snap.cells.size());
    EXPECT_NE(snap.cells[grid_cell_idx].attr.bccode, -1);

    term->SendRawBytes("q");
    HelperWaitForExit(term.get());
    EXPECT_TRUE(term->IsExited());
    EXPECT_EQ(term->ExitStatus(), 0);
}

/**
 * EXHIBIT 2 (vt100 Fallback): Colors Exhibit
 *
 * Verifies that the colors exhibit gracefully degrades on a vt100 terminal.
 */
TEST(CrashTestDummyTest, ColorsExhibitVt100) {
    auto term = CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                         {"--exhibit", "colors"}, "vt100");
    ASSERT_NE(term, nullptr);

    std::string wait_res =
        WaitForText(term.get(), "EXHIBIT: Color Rendering", 2000);
    EXPECT_EQ(wait_res, "found");

    // vt100 should show the fallback block
    EXPECT_THAT(term.get(), ScreenContains("Colors: NOT SUPPORTED"));

    term->SendRawBytes("q");
    HelperWaitForExit(term.get());
    EXPECT_TRUE(term->IsExited());
    EXPECT_EQ(term->ExitStatus(), 0);
}

/**
 * EXHIBIT 3: Text Styles Exhibit
 *
 * Verifies rendering of font style attributes (bold, underline, reverse,
 * blink) by checking cell-specific formatting flags parsed from curses.
 */
TEST(CrashTestDummyTest, TextStylesExhibitInteractive) {
    auto term =
        CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                 {"--exhibit", "styles"}, "xterm-256color");
    ASSERT_NE(term, nullptr);

    std::string wait_res =
        WaitForText(term.get(), "EXHIBIT: Text Styles", 2000);
    EXPECT_EQ(wait_res, "found");

    // Fetch snapshot to directly inspect style attributes of text segments.
    // Style lines print: "%-12s: Sample Text in %s style", where label +
    // padding occupies 14 chars. The sample text starts at index 18 (4
    // starting x + 14 label offset).
    ScreenSnapshot snap = term->GetSnapshot();
    unsigned int sample_x = 18;

    // Row 4: NORMAL (No attributes set)
    unsigned int row_normal = 4 * snap.width + sample_x;
    ASSERT_LT(row_normal, snap.cells.size());
    EXPECT_FALSE(snap.cells[row_normal].attr.bold);
    EXPECT_FALSE(snap.cells[row_normal].attr.underline);
    EXPECT_FALSE(snap.cells[row_normal].attr.inverse);
    EXPECT_FALSE(snap.cells[row_normal].attr.blink);

    // Row 6: BOLD
    unsigned int row_bold = 6 * snap.width + sample_x;
    ASSERT_LT(row_bold, snap.cells.size());
    EXPECT_TRUE(snap.cells[row_bold].attr.bold);

    // Row 8: UNDERLINE
    unsigned int row_underline = 8 * snap.width + sample_x;
    ASSERT_LT(row_underline, snap.cells.size());
    EXPECT_TRUE(snap.cells[row_underline].attr.underline);

    // Row 10: REVERSE (Inverse)
    unsigned int row_reverse = 10 * snap.width + sample_x;
    ASSERT_LT(row_reverse, snap.cells.size());
    EXPECT_TRUE(snap.cells[row_reverse].attr.inverse);

    // Row 14: BLINK
    unsigned int row_blink = 14 * snap.width + sample_x;
    ASSERT_LT(row_blink, snap.cells.size());
    EXPECT_TRUE(snap.cells[row_blink].attr.blink);

    term->SendRawBytes("q");
    HelperWaitForExit(term.get());
    EXPECT_TRUE(term->IsExited());
    EXPECT_EQ(term->ExitStatus(), 0);
}

/**
 * EXHIBIT 4: ACS / Alternate Character Set Exhibit
 *
 * Verifies that alternate line-drawing characters (corners, intersections,
 * lines) are rendered correctly by the terminal engine.
 */
TEST(CrashTestDummyTest, AcsExhibitInteractive) {
    auto term =
        CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                 {"--exhibit", "acs"}, "xterm-256color");
    ASSERT_NE(term, nullptr);

    std::string wait_res =
        WaitForText(term.get(), "EXHIBIT: Line Drawing / ACS", 2000);
    EXPECT_EQ(wait_res, "found");

    EXPECT_THAT(term.get(), ScreenContains("EXHIBIT: Line Drawing / ACS"));
    EXPECT_THAT(term.get(), ScreenContains("ACS Manual Border"));

    // Extract snapshot to verify unicode line-drawing characters.
    // The manual border box starts at Row 4, Col 4.
    ScreenSnapshot snap = term->GetSnapshot();
    unsigned int ul_corner_idx = 4 * snap.width + 4;
    unsigned int h_line_idx = 4 * snap.width + 5;

    ASSERT_LT(h_line_idx, snap.cells.size());
    // Under standard unicode capability, ACS_ULCORNER renders as "┌" or "+".
    EXPECT_THAT(snap.cells[ul_corner_idx].ch, ::testing::AnyOf("┌", "+"));
    // ACS_HLINE renders as "─" or "-".
    EXPECT_THAT(snap.cells[h_line_idx].ch, ::testing::AnyOf("─", "-"));

    term->SendRawBytes("q");
    HelperWaitForExit(term.get());
    EXPECT_TRUE(term->IsExited());
    EXPECT_EQ(term->ExitStatus(), 0);
}

/**
 * EXHIBIT 5: Input Keystroke Echo Exhibit
 *
 * Verifies interactive keyboard input translation, tab cycling, and cursor
 * tracking.
 */
TEST(CrashTestDummyTest, InputEchoExhibitInteractive) {
    auto term =
        CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                 {"--exhibit", "input"}, "xterm-256color");
    ASSERT_NE(term, nullptr);

    std::string wait_res =
        WaitForText(term.get(), "EXHIBIT: Input Keystroke Echo", 2000);
    EXPECT_EQ(wait_res, "found");

    // Demonstrate HasCursorAt matcher: verifies cursor coordinate and
    // visibility state. The cursor is expected to be placed at the end of the
    // instructions line (row 21).
    EXPECT_THAT(term.get(), HasCursorAt(31, 21, false));

    // Verify initial screen state
    EXPECT_THAT(term.get(), ScreenContains("(no keys received yet)"));

    // Demonstrate WaitForScreenChange: capture snapshot before event, send
    // key, and wait for terminal parser thread to digest the new frame.
    int snap_before = term->Snapshot();
    term->SendKey('a');
    std::string change_res =
        WaitForScreenChange(term.get(), snap_before, 2000);
    EXPECT_EQ(change_res, "changed");

    // Assert that the keystroke was echoed and logged
    EXPECT_THAT(term.get(), ScreenContains(" 1: 'a'"));

    // Send a special key code (e.g. arrow key) using the standard key code
    // constants. 0xff52 is standard X11 keysym for Arrow Up (KEY_UP).
    snap_before = term->Snapshot();
    term->SendKey(0xff52);
    change_res = WaitForScreenChange(term.get(), snap_before, 2000);
    EXPECT_EQ(change_res, "changed");

    EXPECT_THAT(term.get(), ScreenContains(" 2: KEY_UP"));

    // Demonstrate Tab-cycling: Tab should transition the active exhibit index
    // back to 0.
    snap_before = term->Snapshot();
    term->SendRawBytes("\t");
    change_res = WaitForScreenChange(term.get(), snap_before, 2000);
    EXPECT_EQ(change_res, "changed");

    // The display should wrap around and show Exhibit 1
    EXPECT_THAT(term.get(), ScreenContains("EXHIBIT: Capability Report"));

    term->SendRawBytes("q");
    HelperWaitForExit(term.get());
    EXPECT_TRUE(term->IsExited());
    EXPECT_EQ(term->ExitStatus(), 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
