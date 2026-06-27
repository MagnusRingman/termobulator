// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include <unistd.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../termobulator.h"
#include "inlined_docs.h"

using termobulator::unstable::CreateSubprocessTerminal;
using termobulator::unstable::ScreenSnapshot;
using termobulator::unstable::Terminal;

static std::string GetCrashTestDummyPath() {
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len != -1) {
        buf[len] = '\0';
        std::string dir(buf);
        size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos) {
            return dir.substr(0, slash) + "/crash_test_dummy";
        }
    }
    return "./crash_test_dummy";
}

void WaitForExit(const std::unique_ptr<Terminal>& term) {
    auto start = std::chrono::steady_clock::now();
    while (!term->IsExited() && std::chrono::steady_clock::now() - start <
                                    std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(term->IsExited());
}

std::string GetSnapshotLine(const ScreenSnapshot& snap, unsigned int row,
                            unsigned int start_col, unsigned int len) {
    std::string res;
    if (row >= snap.height) return res;
    for (unsigned int c = start_col; c < start_col + len && c < snap.width;
         ++c) {
        res += snap.cells[row * snap.width + c].ch;
    }
    return res;
}

void TestInlinedRecipesCatalog() {
    const auto& recipes = termobulator::doc::GetRecipes();
    assert(!recipes.empty());

    bool found_first_contact = false;
    bool found_visual_regression = false;

    for (const auto& recipe : recipes) {
        if (recipe.id == "first_contact") {
            found_first_contact = true;
            assert(recipe.title == "First Contact Interaction Loop");
            assert(recipe.persona == "incidental");
            assert(recipe.content.find("Interaction Loop") !=
                   std::string::npos);
        } else if (recipe.id == "visual_regression") {
            found_visual_regression = true;
            assert(recipe.title == "Visual Regression Testing");
            assert(recipe.persona == "developer");
            assert(recipe.content.find("Regression") != std::string::npos);
        }
    }

    assert(found_first_contact);
    assert(found_visual_regression);
    std::cout << "TestInlinedRecipesCatalog passed\n";
}

void TestFirstContactSimulation() {
    auto term =
        CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                 {"--exhibit", "input"}, "xterm-256color");
    term->WaitIdle(50, 500);

    ScreenSnapshot snap = term->GetSnapshot();
    std::string title = GetSnapshotLine(snap, 1, 2, 29);
    assert(title == "EXHIBIT: Input Keystroke Echo");

    // Simulate interaction: send key 'a' and wait
    term->SendKey('a');
    term->WaitIdle(20, 200);

    snap = term->GetSnapshot();
    std::string key_echo = GetSnapshotLine(snap, 8, 6, 15);
    assert(key_echo.find("'a'") != std::string::npos);

    // Graceful exit
    term->SendRawBytes("q");
    WaitForExit(term);
    std::cout << "TestFirstContactSimulation passed\n";
}

void TestVisualRegressionSimulation() {
    auto term =
        CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                 {"--exhibit", "styles"}, "xterm-256color");
    term->WaitIdle(50, 500);

    ScreenSnapshot snap = term->GetSnapshot();

    // Verify specific styling attribute (A_BOLD in row 6, sample text starts
    // around col 18)
    unsigned int bold_cell_idx = 6 * snap.width + 18;
    assert(snap.cells[bold_cell_idx].attr.bold == true);

    // Verify normal text is not bold
    unsigned int normal_cell_idx = 4 * snap.width + 18;
    assert(snap.cells[normal_cell_idx].attr.bold == false);

    // Graceful exit
    term->SendRawBytes("q");
    WaitForExit(term);
    std::cout << "TestVisualRegressionSimulation passed\n";
}

int main() {
    std::cout << "Starting TestInlinedRecipesCatalog...\n" << std::flush;
    TestInlinedRecipesCatalog();

    std::cout << "Starting TestFirstContactSimulation...\n" << std::flush;
    TestFirstContactSimulation();

    std::cout << "Starting TestVisualRegressionSimulation...\n" << std::flush;
    TestVisualRegressionSimulation();

    std::cout << "All recipe integration tests passed successfully.\n";
    return 0;
}
