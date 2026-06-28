// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "../termobulator.h"

using termobulator::unstable::CreateSubprocessTerminal;
using termobulator::unstable::ScreenSnapshot;

void WaitForExit(
    const std::unique_ptr<termobulator::unstable::Terminal>& term) {
    auto start = std::chrono::steady_clock::now();
    while (!term->IsExited() && std::chrono::steady_clock::now() - start <
                                    std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(term->IsExited());
}

void TestBasicRendering() {
    auto term = CreateSubprocessTerminal(80, 24, "sh",
                                         {"-c", "printf Hello; sleep 0.05"});
    term->WaitIdle(20, 200);
    ScreenSnapshot snap = term->GetSnapshot();
    assert(snap.width == 80);
    assert(snap.height == 24);
    assert(snap.cells[0].ch == "H");
    assert(snap.cells[4].ch == "o");
    assert(snap.cells[5].ch == " ");
    assert(snap.cursor_x == 5);
    assert(snap.cursor_y == 0);
    std::cout << "test_basic_rendering passed\n";
}

void TestScreenAttributes() {
    auto term = CreateSubprocessTerminal(
        80, 24, "sh",
        {"-c", "printf '\\033[1;31mRedBold\\033[0m'; sleep 0.05"});
    term->WaitIdle(20, 200);
    ScreenSnapshot snap = term->GetSnapshot();
    assert(snap.cells[0].ch == "R");
    assert(snap.cells[0].attr.bold);
    assert(snap.cells[0].attr.fccode == 1);

    assert(snap.cells[7].ch == " ");
    assert(snap.cells[7].attr.bold == 0);
    std::cout << "test_screen_attributes passed\n";
}

void TestPtyWrite() {
    auto term =
        CreateSubprocessTerminal(80, 24, "python3",
                                 {"-c",
                                  "import os, sys; data = os.read(0, 10); "
                                  "sys.exit(0 if b'x' in data else 1)"});

    term->SendKey('x');
    term->SendKey('\n');

    WaitForExit(term);
    assert(term->ExitStatus() == 0);
    std::cout << "test_pty_write passed\n";
}

void TestSnapshots() {
    auto term = CreateSubprocessTerminal(
        80, 24, "python3",
        {"-c",
         "import sys, time; sys.stdout.write('First'); sys.stdout.flush(); "
         "time.sleep(0.3); sys.stdout.write(' Second'); sys.stdout.flush(); "
         "time.sleep(0.3)"});

    term->WaitIdle(20, 200);

    int id1 = term->Snapshot();
    assert(id1 == 0);

    // Wait for output change
    auto start_t = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_t <
           std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ScreenSnapshot current = term->GetSnapshot();
        if (current.cells[6].ch == "S") {
            break;
        }
    }

    term->WaitIdle(20, 200);

    int id2 = term->Snapshot();
    assert(id2 == 1);

    ScreenSnapshot snap1 = term->GetSnapshot(id1);
    assert(snap1.cells[0].ch == "F");
    assert(snap1.cells[5].ch == " ");

    ScreenSnapshot snap2 = term->GetSnapshot(id2);
    assert(snap2.cells[0].ch == "F");
    assert(snap2.cells[5].ch == " ");
    assert(snap2.cells[6].ch == "S");
    std::cout << "test_snapshots passed\n";
}

void TestPtyTerminalThread() {
    auto pty_term = CreateSubprocessTerminal(
        80, 24, "sh",
        {"-c",
         "stty -echo; printf READY; read -r c; if [ \"$c\" = \"q\" ]; then "
         "printf '\\nBYE'; else printf '\\nERR'; fi; sleep 0.02"});

    pty_term->WaitIdle(20, 200);

    ScreenSnapshot snap = pty_term->GetSnapshot();
    assert(snap.cells[0].ch == "R");
    assert(snap.cells[1].ch == "E");
    assert(snap.cells[2].ch == "A");
    assert(snap.cells[3].ch == "D");
    assert(snap.cells[4].ch == "Y");

    pty_term->SendRawBytes("q\n");

    auto start = std::chrono::steady_clock::now();
    while (!pty_term->IsExited() && std::chrono::steady_clock::now() - start <
                                        std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(pty_term->IsExited());

    pty_term->WaitIdle(10, 100);

    snap = pty_term->GetSnapshot();
    std::cout << "Screen:\n" << pty_term->DumpScreen() << "\n";
    std::cout << "CHARACTER AT (1,0): '" << snap.cells[1 * snap.width + 0].ch
              << "'\n"
              << std::flush;
    assert(snap.cells[1 * snap.width + 0].ch == "B");
    assert(snap.cells[1 * snap.width + 1].ch == "Y");
    assert(snap.cells[1 * snap.width + 2].ch == "E");

    std::cout << "test_pty_terminal_thread passed\n";
}

void TestResize() {
    auto term = CreateSubprocessTerminal(
        80, 24, "python3",
        {"-c",
         "import sys, signal, time; signal.signal(signal.SIGWINCH, lambda s, "
         "f: (print('READY_RESIZED'), sys.stdout.flush(), sys.exit(0))); "
         "print('READY'); sys.stdout.flush(); [time.sleep(0.01) for _ in "
         "range(200)]"});

    term->WaitIdle(20, 200);

    {
        ScreenSnapshot snap = term->GetSnapshot();
        assert(snap.width == 80);
        assert(snap.height == 24);
    }

    term->Resize(100, 30);

    WaitForExit(term);
    assert(term->ExitStatus() == 0);

    std::cout << "test_resize passed\n";
}

void TestKeysymSpace() {
    uint32_t val = termobulator::unstable::Terminal::ParseKeysym("space");
    assert(val == 0x0020);

    auto keysyms = termobulator::unstable::Terminal::GetKeysyms();
    assert(!keysyms.empty());
    for (size_t i = 1; i < keysyms.size(); ++i) {
        assert(keysyms[i - 1] <= keysyms[i]);
    }
    bool has_space = false;
    for (const auto& k : keysyms) {
        if (k == "space") has_space = true;
    }
    assert(has_space);

    std::cout << "test_keysym_space passed\n";
}

void TestWaitIdleResults() {
    // 1. Idle case
    {
        auto term = CreateSubprocessTerminal(80, 24, "sh",
                                             {"-c", "printf READY; sleep 1"});
        termobulator::unstable::WaitResult res = term->WaitIdle(10, 200);
        assert(res == termobulator::unstable::WaitResult::kIdle);
    }
    // 2. Deadline case
    {
        auto term = CreateSubprocessTerminal(
            80, 24, "sh",
            {"-c", "for i in $(seq 1 50); do printf READY; sleep 0.01; done"});
        termobulator::unstable::WaitResult res = term->WaitIdle(50, 100);
        assert(res == termobulator::unstable::WaitResult::kDeadline);
    }
    // 3. Exited case
    {
        auto term =
            CreateSubprocessTerminal(80, 24, "sh", {"-c", "printf READY"});
        termobulator::unstable::WaitResult res = term->WaitIdle(10, 2000);
        assert(res == termobulator::unstable::WaitResult::kExited);
    }
    std::cout << "test_wait_idle_results passed\n";
}

void TestSubprocessEnv() {
    auto term = CreateSubprocessTerminal(
        80, 24, "sh", {"-c", "echo \"$TERM:$LC_ALL:$LANG\""}, "myterm",
        "en_US.UTF-8");
    term->WaitIdle(20, 2000);
    ScreenSnapshot snap = term->GetSnapshot();
    std::string line;
    for (unsigned int x = 0; x < snap.width; ++x) {
        std::string ch = snap.cells[x].ch;
        if (ch == " ") break;
        line += ch;
    }
    std::cout << "Subprocess output: " << line << "\n";
    assert(line.find("myterm") != std::string::npos);
    assert(line.find("en_US.UTF-8") != std::string::npos);
    std::cout << "test_subprocess_env passed\n";
}

#include <limits.h>

// Helper function to extract a string from a line in the snapshot
std::string GetSnapshotLine(const ScreenSnapshot& snap, unsigned int row,
                            unsigned int start_col, unsigned int length) {
    std::string s;
    for (unsigned int c = 0; c < length; ++c) {
        unsigned int idx = row * snap.width + start_col + c;
        if (idx < snap.cells.size()) {
            s += snap.cells[idx].ch;
        }
    }
    return s;
}

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

void TestCrashTestDummyCapability() {
    auto term = CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                         {"--exhibit", "capability"},
                                         "xterm-256color");

    term->WaitIdle(50, 500);
    std::cout << "CAPABILITY REPORT SCREEN:\n"
              << term->DumpScreen() << std::endl;
    ScreenSnapshot snap = term->GetSnapshot();

    std::string title = GetSnapshotLine(snap, 1, 2, 26);
    assert(title == "EXHIBIT: Capability Report");

    std::string term_line = GetSnapshotLine(snap, 4, 2, 50);
    assert(term_line.find("xterm-256color") != std::string::npos);

    std::string max_colors_line = GetSnapshotLine(snap, 8, 2, 35);
    assert(max_colors_line.find("256") != std::string::npos);

    term->SendRawBytes("q");
    WaitForExit(term);
    std::cout << "test_crash_test_dummy_capability passed\n";
}

void TestCrashTestDummyColors() {
    {
        auto term = CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                             {"--exhibit", "colors"},
                                             "xterm-256color");

        term->WaitIdle(50, 500);
        ScreenSnapshot snap = term->GetSnapshot();
        std::string title = GetSnapshotLine(snap, 1, 2, 24);
        assert(title == "EXHIBIT: Color Rendering");

        unsigned int cell_idx = 6 * snap.width + 4;
        assert(snap.cells[cell_idx].attr.bccode != -1);

        term->SendRawBytes("q");
        WaitForExit(term);
    }

    {
        auto term = CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                             {"--exhibit", "colors"}, "vt100");

        term->WaitIdle(50, 500);
        ScreenSnapshot snap = term->GetSnapshot();
        std::string fallback = GetSnapshotLine(snap, 4, 4, 21);
        assert(fallback == "Colors: NOT SUPPORTED");

        term->SendRawBytes("q");
        WaitForExit(term);
    }

    std::cout << "test_crash_test_dummy_colors passed\n";
}

void TestCrashTestDummyAcs() {
    auto term =
        CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                 {"--exhibit", "acs"}, "xterm-256color");

    term->WaitIdle(50, 500);
    ScreenSnapshot snap = term->GetSnapshot();
    std::string title = GetSnapshotLine(snap, 1, 2, 27);
    assert(title == "EXHIBIT: Line Drawing / ACS");

    unsigned int ul_idx = 4 * snap.width + 4;
    assert(snap.cells[ul_idx].ch != " ");

    term->SendRawBytes("q");
    WaitForExit(term);
    std::cout << "test_crash_test_dummy_acs passed\n";
}

void TestCrashTestDummyKeystroke() {
    auto term =
        CreateSubprocessTerminal(80, 24, GetCrashTestDummyPath(),
                                 {"--exhibit", "input"}, "xterm-256color");

    term->WaitIdle(50, 500);

    term->SendKey('a');
    term->WaitIdle(20, 200);

    term->SendKey(0xff52);
    term->WaitIdle(20, 200);

    ScreenSnapshot snap = term->GetSnapshot();

    term->SendRawBytes("q");
    WaitForExit(term);

    std::string key1 = GetSnapshotLine(snap, 8, 6, 10);
    assert(key1.find("'a'") != std::string::npos);

    std::string key2 = GetSnapshotLine(snap, 9, 6, 12);
    assert(key2.find("KEY_UP") != std::string::npos);

    std::cout << "test_crash_test_dummy_keystroke passed\n";
}

int main() {
    std::cout << "Starting test_basic_rendering...\n" << std::flush;
    TestBasicRendering();
    std::cout << "Starting test_screen_attributes...\n" << std::flush;
    TestScreenAttributes();
    std::cout << "Starting test_pty_write...\n" << std::flush;
    TestPtyWrite();
    std::cout << "Starting test_snapshots...\n" << std::flush;
    TestSnapshots();
    std::cout << "Starting test_pty_terminal_thread...\n" << std::flush;
    TestPtyTerminalThread();
    std::cout << "Starting test_resize...\n" << std::flush;
    TestResize();
    std::cout << "Starting test_keysym_space...\n" << std::flush;
    TestKeysymSpace();
    std::cout << "Starting test_wait_idle_results...\n" << std::flush;
    TestWaitIdleResults();
    std::cout << "Starting test_subprocess_env...\n" << std::flush;
    TestSubprocessEnv();

    std::cout << "Starting test_crash_test_dummy_capability...\n"
              << std::flush;
    TestCrashTestDummyCapability();
    std::cout << "Starting test_crash_test_dummy_colors...\n" << std::flush;
    TestCrashTestDummyColors();
    std::cout << "Starting test_crash_test_dummy_acs...\n" << std::flush;
    TestCrashTestDummyAcs();
    std::cout << "Starting test_crash_test_dummy_keystroke...\n" << std::flush;
    TestCrashTestDummyKeystroke();

    std::cout << "All Terminal unit tests passed successfully!\n"
              << std::flush;
    return 0;
}
