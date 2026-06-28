// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../lua_executor.h"
#include "../termobulator.h"

using termobulator::unstable::CreateSubprocessTerminal;
using termobulator::unstable::LuaExecutor;

void TestBasic() {
    LuaExecutor exec(nullptr);

    // Simple expression return
    {
        auto res = exec.Execute("return 42");
        assert(res.result.get<int>() == 42);
        assert(res.log.empty());
    }

    // Multiple return values -> JSON array
    {
        auto res = exec.Execute("return 1, 2, 'hello'");
        assert(res.result.is_array());
        assert(res.result.size() == 3);
        assert(res.result[0].get<int>() == 1);
        assert(res.result[1].get<int>() == 2);
        assert(res.result[2].get<std::string>() == "hello");
    }

    // No return -> null
    {
        auto res = exec.Execute("local x = 1");
        assert(res.result.is_null());
    }

    std::cout << "TestBasic passed\n";
}

void TestVariables() {
    LuaExecutor exec(nullptr);

    // Variable persistence
    {
        auto res = exec.Execute("vars.x = 100; vars.y = 'foo'");
        assert(res.variables["x"].get<int>() == 100);
        assert(res.variables["y"].get<std::string>() == "foo");
    }

    // Read and modify in next call
    {
        auto res = exec.Execute("vars.x = vars.x + 50; return vars.x, vars.y");
        assert(res.result.is_array());
        assert(res.result[0].get<int>() == 150);
        assert(res.result[1].get<std::string>() == "foo");
        assert(res.variables["x"].get<int>() == 150);
    }

    std::cout << "TestVariables passed\n";
}

void TestSandbox() {
    LuaExecutor exec(nullptr);

    // Attempting load/print
    {
        // load is nil, so pcall should fail / return error
        auto res = exec.Execute(
            "local ok, err = pcall(load, 'return 1'); return ok, err");
        assert(res.result.is_array());
        assert(res.result[0].get<bool>() == false);
        std::string err = res.result[1].get<std::string>();
        assert(err.find("nil") != std::string::npos);
    }

    // io/os is not available
    {
        auto res = exec.Execute("return io, os");
        assert(res.result.is_array());
        assert(res.result[0].is_null());
        assert(res.result[1].is_null());
    }

    std::cout << "TestSandbox passed\n";
}

void TestLog() {
    LuaExecutor exec(nullptr);

    {
        auto res = exec.Execute("log('hello', 'world'); log(42)");
        assert(res.log.size() == 2);
        assert(res.log[0] == "hello\tworld");
        assert(res.log[1] == "42");
    }

    std::cout << "TestLog passed\n";
}

void TestRollback() {
    LuaExecutor exec(nullptr);

    // Set variable
    exec.Execute("vars.x = 42");

    // Failure execution
    try {
        exec.Execute("vars.x = 99; error('some runtime error')");
        assert(false);
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        assert(msg.find("some runtime error") != std::string::npos);
        assert(msg.find("stack traceback:") != std::string::npos);
    }

    // Verify vars.x is still 42 (rollback)
    assert(exec.GetVariables().at("x").get<int>() == 42);

    std::cout << "TestRollback passed\n";
}

void TestTypesRoundTrip() {
    LuaExecutor exec(nullptr);

    // Complex types
    {
        auto res = exec.Execute(
            "vars.arr = {10, 20, 30}; vars.obj = {a = 1, b = true}; vars.fn = "
            "function() end");
        assert(res.variables["arr"].is_array());
        assert(res.variables["arr"].size() == 3);
        assert(res.variables["arr"][0].get<int>() == 10);
        assert(res.variables["obj"]["a"].get<int>() == 1);
        assert(res.variables["obj"]["b"].get<bool>() == true);

        assert(res.variables.find("fn") == res.variables.end());
        bool has_warning = false;
        for (const auto& l : res.log) {
            if (l.find("Dropped var 'fn'") != std::string::npos) {
                has_warning = true;
            }
        }
        assert(has_warning);
    }

    // Test table with integer keys (verify fix for next traversal panic)
    {
        auto res = exec.Execute(
            "vars.obj_num_keys = { [123] = 'foo', [456] = 'bar' }");
        assert(res.variables["obj_num_keys"].is_object());
        assert(res.variables["obj_num_keys"]["123"].get<std::string>() ==
               "foo");
        assert(res.variables["obj_num_keys"]["456"].get<std::string>() ==
               "bar");
    }

    // Test cyclic table (verify recursion limit prevents stack overflow crash)
    {
        auto res = exec.Execute(
            "local t = {}\n"
            "t.self = t\n"
            "vars.cyclic = t\n");
        bool has_depth_warning = false;
        for (const auto& l : res.log) {
            if (l.find("Max serialization depth exceeded") !=
                std::string::npos) {
                has_depth_warning = true;
            }
        }
        assert(has_depth_warning);
    }

    std::cout << "TestTypesRoundTrip passed\n";
}

void TestTerminalOps() {
    auto term = CreateSubprocessTerminal(80, 24, "sh", {"-c", "sleep 5"});

    LuaExecutor exec(term.get());

    // take_snapshot, get_status
    {
        auto res =
            exec.Execute("return term.take_snapshot(), term.get_status()");
        assert(res.result.is_array());
        assert(res.result[0].get<int>() == 0);
        assert(res.result[1].get<std::string>() == "running");
    }

    // wait_idle
    {
        auto res = exec.Execute("return term.wait_idle(20, 200)");
        assert(res.result.get<std::string>() == "idle");
    }

    // resize, get_screen
    {
        auto res =
            exec.Execute("term.resize(40, 10); return term.get_screen()");
        std::string s = res.result.get<std::string>();
        assert(s.find_first_not_of("\n ") == std::string::npos);
    }

    std::cout << "TestTerminalOps passed\n";
}

void TestWatcherOps() {
    auto term = CreateSubprocessTerminal(80, 24, "sh", {"-c", "sleep 5"});

    LuaExecutor exec(term.get());

    // watch_timeout, wait_any
    {
        auto res = exec.Execute(
            "local w1 = term.watch_timeout(50)\n"
            "local fired = term.wait_any(w1)\n"
            "return fired == w1, tostring(w1)");
        assert(res.result.is_array());
        assert(res.result[0].get<bool>() == true);
        assert(res.result[1].get<std::string>().find("watcher: timeout(50)") !=
               std::string::npos);
    }

    std::cout << "TestWatcherOps passed\n";
}

void TestDumpScreenHtml() {
    auto term = CreateSubprocessTerminal(10, 5, "sh", {"-c", "sleep 5"});
    LuaExecutor exec(term.get());

    auto res = exec.Execute("return term.dump_screen_html()");
    assert(res.result.is_string());
    std::string html = res.result.get<std::string>();
    assert(html.find("<pre style=") != std::string::npos);
    assert(html.find("</span>") != std::string::npos);

    std::cout << "TestDumpScreenHtml passed\n";
}

void TestVerboseWarnings() {
    auto term = CreateSubprocessTerminal(80, 24, "sh", {"-c", "sleep 5"});
    LuaExecutor exec(term.get());

    auto res = exec.Execute(
        "vars.my_func = function() end; vars.nested = { bad = function() end "
        "}");
    assert(res.log.size() >= 2);

    bool found_my_func = false;
    bool found_nested_bad = false;
    for (const auto& log : res.log) {
        if (log.find("var 'my_func'") != std::string::npos)
            found_my_func = true;
        if (log.find("vars.nested.bad") != std::string::npos)
            found_nested_bad = true;
    }
    assert(found_my_func);
    assert(found_nested_bad);

    std::cout << "TestVerboseWarnings passed\n";
}

void TestNewBindings() {
    auto term = CreateSubprocessTerminal(80, 24, "cat", {});
    LuaExecutor exec(term.get());

    // test get_terminal_size
    {
        auto res = exec.Execute("return term.get_terminal_size()");
        assert(res.result.is_array());
        assert(res.result[0].get<int>() == 80);
        assert(res.result[1].get<int>() == 24);
    }

    // test set_disable_alternate_screen
    {
        auto res = exec.Execute("term.set_disable_alternate_screen(true)");
        assert(res.result.is_null());
    }

    // test get_keysyms
    {
        auto res = exec.Execute(
            "local syms = term.get_keysyms(); return #syms > 0, syms[1]");
        assert(res.result.is_array());
        assert(res.result[0].get<bool>() == true);
        assert(res.result[1].is_string());
    }

    // test get_rows
    {
        auto res =
            exec.Execute("local rows = term.get_rows(0, 2); return #rows");
        assert(res.result.get<int>() == 3);
    }

    // test structured diff
    {
        auto res = exec.Execute(
            "local s1 = term.take_snapshot()\n"
            "term.send_key('a')\n"
            "term.wait_for_screen_change(s1, 1000)\n"
            "local s2 = term.take_snapshot()\n"
            "local diffs = term.get_diff_structured(s1, s2)\n"
            "return #diffs > 0, diffs[1].row, diffs[1].new\n");
        assert(res.result.is_array());
        assert(res.result[0].get<bool>() == true);
        assert(res.result[1].get<int>() == 0);
        assert(res.result[2].get<std::string>() == "a");
    }

    // test find_pattern
    {
        auto res = exec.Execute(
            "term.send_key('bcd123')\n"
            "term.wait_idle(20, 500)\n"
            "return term.find_pattern('%d+')\n");
        assert(res.result.is_array());
        assert(res.result.size() > 0);
        assert(res.result[0]["row"].get<int>() == 0);
        assert(res.result[0]["col_start"].get<int>() > 0);
    }

    // test wait_for_screen_change
    {
        auto res = exec.Execute(
            "local s = term.take_snapshot()\n"
            "term.send_key('e')\n"
            "return term.wait_for_screen_change(s, 1000)\n");
        assert(res.result.get<std::string>() == "changed");
    }

    // test watch_pattern, watch_any_text, watch_any_pattern
    {
        auto res = exec.Execute(
            "local w1 = term.watch_pattern('%d+')\n"
            "local w2 = term.watch_any_text({'xyz', 'abc'})\n"
            "local w3 = term.watch_any_pattern({'%a+','%d+'})\n"
            "return tostring(w1), tostring(w2), tostring(w3)\n");
        assert(res.result.is_array());
        assert(res.result[0].get<std::string>().find("custom") !=
               std::string::npos);
        assert(res.result[1].get<std::string>().find("custom") !=
               std::string::npos);
        assert(res.result[2].get<std::string>().find("custom") !=
               std::string::npos);
    }

    std::cout << "TestNewBindings passed\n";
}

int main() {
    TestBasic();
    TestVariables();
    TestSandbox();
    TestLog();
    TestRollback();
    TestTypesRoundTrip();
    TestTerminalOps();
    TestWatcherOps();
    TestDumpScreenHtml();
    TestVerboseWarnings();
    TestNewBindings();
    std::cout << "All core LuaExecutor tests passed!\n";
    return 0;
}
