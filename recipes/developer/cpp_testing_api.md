---
id: cpp_testing_api
title: C++ Testing Affordances API
description: Detailed manual for using C++ test helpers and Google Test matchers to test TUI applications.
persona: developer
---

## C++ Testing Affordances for TUIs

When developing C++ unit tests for console/TUI programs, you can link against `libtermobulator-testing` (header `<termobulator_testing.h>`) to leverage declarative assertions, Google Mock matchers, and convenient waiting/extraction helpers.

---

## 1. Google Mock Matchers

These matchers provide a highly declarative syntax for checking terminal screens, automatically printing the full terminal screen layout in a clean box format when assertions fail.

They match polymorphic types: `termobulator::unstable::ScreenSnapshot`, `termobulator::unstable::Terminal*`, or `termobulator::unstable::Terminal&`.

### `ScreenContains(text)`
Asserts that the screen contains the specified text anywhere.
```cpp
EXPECT_THAT(term.get(), ScreenContains("Welcome to MyApp"));
```

### `RowContains(row_index, text)`
Asserts that the specified 0-indexed row of the screen contains the text (stripped of trailing whitespace).
```cpp
EXPECT_THAT(term.get(), RowContains(0, "MY TUI HEADER"));
```

### `ScreenMatchesPattern(pattern)`
Asserts that the entire screen content matches a regular expression.
```cpp
EXPECT_THAT(term.get(), ScreenMatchesPattern("Version: [0-9]+\\.[0-9]+"));
```

### `HasCursorAt(x, y, visible)`
Asserts the coordinates and visibility of the cursor.
```cpp
// Check if the cursor is at column 10, row 5 and is visible
EXPECT_THAT(term.get(), HasCursorAt(10, 5, true));
```

---

## 2. Coordination and Polling Utilities

Because TUI applications run asynchronously inside terminal sessions, static assertions can fire too early. Use polling helpers to synchronize your test driver with the application state.

### `WaitForText(terminal, query, deadline_ms)`
Spins and polls until the specified text is found on the screen, or the deadline is reached.
* **Return**: `"found"` if matching, `"exited"` if the subprocess exits, or `"timeout"`.
```cpp
std::string status = WaitForText(term.get(), "Press ANY key to continue", 2000);
ASSERT_EQ(status, "found");
```

### `WaitForScreenChange(terminal, baseline_snapshot_id, deadline_ms)`
Captures a baseline snapshot ID and waits until the screen content changes.
* **Return**: `"changed"`, `"exited"`, or `"timeout"`.
```cpp
int baseline = term->Snapshot();
term->SendKey('\n');
std::string status = WaitForScreenChange(term.get(), baseline, 2000);
ASSERT_EQ(status, "changed");
```

---

## 3. Screen Inspection and Extraction

For fine-grained or manual assertions on colors, attributes, or text content:

### `GetScreen(terminal, snapshot_id)`
Extracts the screen content as a single newline-separated string.

### `GetRow(terminal, row_index, snapshot_id)`
Extracts a single row's string content (rtrimmed).

### `GetRows(terminal, start, end, snapshot_id)`
Extracts a range of rows as a vector of strings.

### `DumpScreenHtml(terminal, snapshot_id)`
Generates HTML of the screen, wrapping characters in colored styled `<span>` elements. Useful for writing visual snapshots to test logs.

---

## 4. CMake Integration

Link your test suite target to the shared testing library:

```cmake
find_package(termobulator REQUIRED)

add_executable(my_tui_test my_tui_test.cc)
target_link_libraries(my_tui_test PRIVATE
    termobulator::termobulator
    termobulator::termobulator-testing
    GTest::gmock
    GTest::gtest_main
)
```
