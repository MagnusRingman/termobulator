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

using namespace termobulator::unstable;
using namespace termobulator::unstable::testing;

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

TEST(TestingApiTest, HelperFunctionsAndMatchers) {
    auto term =
        CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                 {"--exhibit", "input"}, "xterm-256color");
    ASSERT_NE(term, nullptr);

    // Wait for text
    std::string wait_res =
        WaitForText(term.get(), "EXHIBIT: Input Keystroke Echo", 2000);
    EXPECT_EQ(wait_res, "found");

    // Static snapshot matches
    ScreenSnapshot snap = term->GetSnapshot(-1);
    EXPECT_THAT(snap, ScreenContains("EXHIBIT: Input Keystroke Echo"));
    EXPECT_THAT(snap, RowContains(1, "EXHIBIT: Input Keystroke Echo"));
    EXPECT_THAT(snap, ScreenMatchesPattern("Input.*Echo"));
    EXPECT_THAT(snap, HasCursorAt(31, 21, false));

    // Pointer-based matches
    EXPECT_THAT(term.get(), ScreenContains("EXHIBIT: Input Keystroke Echo"));
    EXPECT_THAT(term.get(), RowContains(1, "EXHIBIT: Input Keystroke Echo"));
    EXPECT_THAT(term.get(), ScreenMatchesPattern("Input.*Echo"));

    // Reference matches
    EXPECT_THAT(*term, ScreenContains("EXHIBIT: Input Keystroke Echo"));
    EXPECT_THAT(*term, RowContains(1, "EXHIBIT: Input Keystroke Echo"));
    EXPECT_THAT(*term, ScreenMatchesPattern("Input.*Echo"));

    // GetScreen, GetRow, GetRows, GetCursor
    std::string screen = GetScreen(term.get(), -1);
    EXPECT_NE(screen.find("EXHIBIT: Input Keystroke Echo"), std::string::npos);

    std::string row = GetRow(term.get(), 1, -1);
    EXPECT_NE(row.find("Input Keystroke Echo"), std::string::npos);

    auto rows = GetRows(term.get(), 1, 2, -1);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_NE(rows[0].find("Input Keystroke Echo"), std::string::npos);

    CursorInfo cursor = GetCursor(term.get(), -1);
    EXPECT_FALSE(cursor.visible);

    // Send a key and verify screen change
    int snap_before = term->Snapshot();
    term->SendKey('a');
    std::string change_res =
        WaitForScreenChange(term.get(), snap_before, 2000);
    EXPECT_EQ(change_res, "changed");

    int snap_after = term->Snapshot();
    std::string diff = GetDiff(term.get(), snap_before, snap_after);
    EXPECT_NE(diff.find("'a'"), std::string::npos);

    auto diffs = GetDiffStructured(term.get(), snap_before, snap_after);
    ASSERT_GE(diffs.size(), 2);
    EXPECT_EQ(diffs[0].new_val, " 1:");
    EXPECT_NE(diffs[1].new_val.find("'a'"), std::string::npos);

    // FindText / FindPattern
    auto text_matches = FindText(term.get(), "Last 10", -1);
    EXPECT_GE(text_matches.size(), 1);

    auto pattern_matches = FindPattern(term.get(), "Last.*10", -1);
    EXPECT_GE(pattern_matches.size(), 1);

    // Watchers
    WatcherDescriptor w1 = WatchText("'a'");
    WatcherDescriptor w2 = WatchPattern(".*'a'.*");
    WatcherDescriptor w3 = WatchAnyText({"unknown", "'a'"});
    WatcherDescriptor w4 = WatchAnyPattern({"xyz", ".*'a'.*"});

    WatchResult wr = term->WaitAny({w1, w2, w3, w4});
    EXPECT_GE(wr.fired_index, 0);

    term->SendRawBytes("q");
    // Wait for exit
    auto start = std::chrono::steady_clock::now();
    while (!term->IsExited() && std::chrono::steady_clock::now() - start <
                                    std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(term->IsExited());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
