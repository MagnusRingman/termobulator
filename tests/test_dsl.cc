// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../dsl_interpreter.h"
#include "../termobulator.h"

using termobulator::unstable::CreateThreadTerminal;
using termobulator::unstable::Interpreter;
using termobulator::unstable::ParseTextToDsl;

void TestParse() {
    // 1. Basic parsing
    nlohmann::json dsl = ParseTextToDsl("123 true false dup swap");
    assert(dsl.is_array());
    assert(dsl.size() == 5);
    assert(dsl[0].get<int>() == 123);
    assert(dsl[1].get<bool>() == true);
    assert(dsl[2].get<bool>() == false);
    assert(dsl[3].get<std::string>() == "dup");
    assert(dsl[4].get<std::string>() == "swap");

    // 2. Quoted strings (stripped of outer quotes, prepended with '$')
    dsl = ParseTextToDsl("\"hello world\" \"escaped\\nnewline\"");
    assert(dsl.size() == 2);
    assert(dsl[0].get<std::string>() == "$hello world");
    assert(dsl[1].get<std::string>() == "$escaped\nnewline");

    // 3. Quotations (nested arrays)
    dsl = ParseTextToDsl("1 [ dup 2 ] if");
    assert(dsl.size() == 3);
    assert(dsl[0].get<int>() == 1);
    assert(dsl[1].is_array());
    assert(dsl[1].size() == 2);
    assert(dsl[1][0].get<std::string>() == "dup");
    assert(dsl[1][1].get<int>() == 2);
    assert(dsl[2].get<std::string>() == "if");

    // 4. Error handling
    try {
        ParseTextToDsl("[ dup");
        assert(false);
    } catch (const std::runtime_error& e) {
        // expected
    }

    try {
        ParseTextToDsl("dup ]");
        assert(false);
    } catch (const std::runtime_error& e) {
        // expected
    }

    try {
        ParseTextToDsl("\"unterminated");
        assert(false);
    } catch (const std::runtime_error& e) {
        // expected
    }

    std::cout << "TestParse passed\n";
}

void TestStackOps() {
    Interpreter interp(nullptr);

    // dup
    interp.ExecuteText("42 dup");
    auto stack = interp.GetStack();
    assert(stack.size() == 2);
    assert(stack[0].get<int>() == 42);
    assert(stack[1].get<int>() == 42);

    // dup2
    interp.ClearStack();
    interp.ExecuteText("11 22 dup2");
    stack = interp.GetStack();
    assert(stack.size() == 4);
    assert(stack[0].get<int>() == 11);
    assert(stack[1].get<int>() == 22);
    assert(stack[2].get<int>() == 11);
    assert(stack[3].get<int>() == 22);

    // drop
    interp.ClearStack();
    interp.ExecuteText("1 2 drop");
    stack = interp.GetStack();
    assert(stack.size() == 1);
    assert(stack[0].get<int>() == 1);

    // swap
    interp.ClearStack();
    interp.ExecuteText("1 2 swap");
    stack = interp.GetStack();
    assert(stack.size() == 2);
    assert(stack[0].get<int>() == 2);
    assert(stack[1].get<int>() == 1);

    // over
    interp.ClearStack();
    interp.ExecuteText("1 2 over");
    stack = interp.GetStack();
    assert(stack.size() == 3);
    assert(stack[0].get<int>() == 1);
    assert(stack[1].get<int>() == 2);
    assert(stack[2].get<int>() == 1);

    // rot
    interp.ClearStack();
    interp.ExecuteText("1 2 3 rot");
    stack = interp.GetStack();
    assert(stack.size() == 3);
    assert(stack[0].get<int>() == 2);
    assert(stack[1].get<int>() == 3);
    assert(stack[2].get<int>() == 1);

    // clear
    interp.ExecuteText("clear");
    assert(interp.GetStack().empty());

    // Underflow check
    try {
        interp.ExecuteText("drop");
        assert(false);
    } catch (const std::runtime_error& e) {
        // expected
    }

    std::cout << "TestStackOps passed\n";
}

void TestVariables() {
    Interpreter interp(nullptr);

    // store and load
    interp.ExecuteText("100 \"myvar\" store");
    assert(interp.GetStack().empty());
    interp.ExecuteText("\"myvar\" load");
    auto stack = interp.GetStack();
    assert(stack.size() == 1);
    assert(stack[0].get<int>() == 100);

    // error: load non-existent
    try {
        interp.ExecuteText("\"notfound\" load");
        assert(false);
    } catch (const std::runtime_error& e) {
        // expected
    }

    std::cout << "TestVariables passed\n";
}

void TestLogicOps() {
    Interpreter interp(nullptr);

    // not
    interp.ExecuteText("true not");
    assert(interp.Pop().get<bool>() == false);
    interp.ExecuteText("false not");
    assert(interp.Pop().get<bool>() == true);
    interp.ExecuteText("0 not");
    assert(interp.Pop().get<bool>() == true);
    interp.ExecuteText("1 not");
    assert(interp.Pop().get<bool>() == false);

    // equal
    interp.ExecuteText("1 1 equal");
    assert(interp.Pop().get<bool>() == true);
    interp.ExecuteText("1 2 equal");
    assert(interp.Pop().get<bool>() == false);
    interp.ExecuteText("\"foo\" \"foo\" equal");
    assert(interp.Pop().get<bool>() == true);

    // empty
    interp.ExecuteText("\"\" empty");
    assert(interp.Pop().get<bool>() == true);
    interp.ExecuteText("\"a\" empty");
    assert(interp.Pop().get<bool>() == false);
    interp.ExecuteText("[ ] empty");
    assert(interp.Pop().get<bool>() == true);
    interp.ExecuteText("[ 1 ] empty");
    assert(interp.Pop().get<bool>() == false);

    // size
    interp.ExecuteText("\"hello\" size");
    assert(interp.Pop().get<int>() == 5);
    interp.ExecuteText("[ 1 2 3 ] size");
    assert(interp.Pop().get<int>() == 3);

    std::cout << "TestLogicOps passed\n";
}

void TestControlFlow() {
    Interpreter interp(nullptr);

    // exec
    interp.ExecuteText("[ 5 dup ] exec");
    auto stack = interp.GetStack();
    assert(stack.size() == 2);
    assert(stack[0].get<int>() == 5);
    assert(stack[1].get<int>() == 5);
    interp.ClearStack();

    // if
    interp.ExecuteText("true [ 10 ] [ 20 ] if");
    assert(interp.Pop().get<int>() == 10);
    interp.ExecuteText("false [ 10 ] [ 20 ] if");
    assert(interp.Pop().get<int>() == 20);

    // dip
    interp.ExecuteText("99 [ 5 ] dip");
    stack = interp.GetStack();
    assert(stack.size() == 2);
    assert(stack[0].get<int>() == 5);
    assert(stack[1].get<int>() == 99);
    interp.ClearStack();

    // while
    interp.ClearStack();
    interp.ExecuteText("0 [ dup 5 equal not ] [ 1 + ] while");
    assert(interp.Pop().get<int>() == 5);

    std::cout << "TestControlFlow passed\n";
}

void TestTerminalOps() {
    auto term = CreateThreadTerminal(80, 24, []() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
    });

    Interpreter interp(term.get());

    // take_snapshot
    interp.ExecuteText("take_snapshot");
    auto stack = interp.GetStack();
    assert(stack.size() == 1);
    assert(stack[0].is_number_integer());
    int snap_id = stack[0].get<int>();

    // get_cursor
    interp.ClearStack();
    interp.ExecuteText(std::to_string(snap_id) + " get_cursor");
    stack = interp.GetStack();
    assert(stack.size() == 3);
    assert(stack[0].get<int>() == 0);      // x
    assert(stack[1].get<int>() == 0);      // y
    assert(stack[2].get<bool>() == true);  // visible

    // get_status
    interp.ClearStack();
    interp.ExecuteText("get_status");
    assert(interp.Pop().get<std::string>() == "running");

    // wait_idle
    interp.ClearStack();
    interp.ExecuteText("20 200 wait_idle");
    assert(interp.Pop().get<std::string>() == "wait: idle");

    std::cout << "TestTerminalOps passed\n";
}

void TestNewFeatures() {
    Interpreter interp(nullptr);

    // 1. Structured JSON literal {"lit": ...}
    nlohmann::json lit_instr = nlohmann::json::array();
    lit_instr.push_back(nlohmann::json{{"lit", "hello"}});
    lit_instr.push_back(nlohmann::json{{"lit", ""}});
    interp.Execute(lit_instr);
    auto stack = interp.GetStack();
    assert(stack.size() == 2);
    assert(stack[0].get<std::string>() == "hello");
    assert(stack[1].get<std::string>() == "");

    // 2. Structured JSON operation {"op": ..., "args": ...}
    interp.ClearStack();
    nlohmann::json op_instr = nlohmann::json::array();
    op_instr.push_back(
        nlohmann::json{{"op", "dup"}, {"args", nlohmann::json::array({42})}});
    interp.Execute(op_instr);
    stack = interp.GetStack();
    assert(stack.size() == 2);
    assert(stack[0].get<int>() == 42);
    assert(stack[1].get<int>() == 42);

    // 3. sleep_ms
    auto t1 = std::chrono::steady_clock::now();
    interp.ExecuteText("100 sleep_ms");
    auto t2 = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    assert(elapsed >= 90);

    // 4. Error diagnostics & stack auto-clearing
    interp.ClearStack();
    interp.ExecuteText("10 20");
    try {
        interp.ExecuteText("drop drop drop");
        assert(false);
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        assert(msg.find("failed at instruction 2") != std::string::npos);
        assert(msg.find("drop") != std::string::npos);
        assert(msg.find("Stack at failure") != std::string::npos);
    }
    // Verify stack was auto-cleared
    assert(interp.GetStack().empty());

    std::cout << "TestNewFeatures passed\n";
}

int main() {
    std::cout << "Running DSL Interpreter tests...\n" << std::flush;
    TestParse();
    TestStackOps();
    TestVariables();
    TestLogicOps();
    TestControlFlow();
    TestTerminalOps();
    TestNewFeatures();
    std::cout << "All DSL Interpreter tests passed successfully!\n"
              << std::flush;
    return 0;
}
