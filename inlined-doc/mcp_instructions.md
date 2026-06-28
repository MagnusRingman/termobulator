# MCP Server Instructions

This server manages terminal sessions and provides tools to interact with them.
The `execute_dsl` tool executes a Lua 5.4 script on a session's persistent terminal and state.

## Lua Conceptual Model

- **Fresh State Per Call**: A new `lua_State` is created for every `execute_dsl` call. Uncaught runtime errors abort execution immediately, leaving the persistent variable store untouched (perfect rollback).
- **Sandboxed Environment**: To prevent host access, only safe standard libraries are opened: `base`, `string`, `table`, `math`, and `utf8`.
- **Pruned Globals**: Banned functions like `print`, `warn`, `load`, `loadfile`, `dofile`, `require`, and `collectgarbage` are removed from the global environment. `string.dump` is also removed.
- **Variable Store (`vars`)**: Variables are persisted across calls by storing them in the global `vars` table (e.g. `vars.counter = (vars.counter or 0) + 1`). Surviving entries are serialized to/from the C++ session store between calls.
- **JSON Serialization**: Only values that round-trip cleanly to JSON are serialized in `vars` (numbers, strings, booleans, arrays, and tables with string keys). Other types (e.g. functions, userdata) are skipped with a warning logged.
- **Diagnostic Logging**: The global `log(...)` function accepts any number of arguments, converts them to strings, joins them with tabs, and appends them to a diagnostics log buffer returned to the caller.
- **Structured Response**: The tool returns a JSON payload containing:
  - `result`: The value(s) returned by the Lua chunk (0 returns = `null`, 1 return = value, >1 returns = JSON array).
  - `log`: An array of diagnostic strings collected via `log(...)`.
  - `variables`: A snapshot of the updated `vars` table.

### Global Functions

- `log(...)` `( ... -- )`: Appends formatted string to the diagnostic log buffer.

### Terminal Namespace (`term.*`)

All terminal interactions reside under the global `term` table namespace:

#### Input

- `term.send_key(text)` `( string -- )`: Sends a key sequence to the terminal. Parses escape sequences like `\n`, `\t`, or `\xNN`.
- `term.send_special_key(name [, mods])` `( string [, string] -- )`: Sends a special key. `mods` is a comma-separated string of modifiers (e.g. `"ctrl,shift"`).
  - **Supported Key Names**: `up`, `down`, `left`, `right`, `f1` to `f20`, `backspace`, `tab`, `enter`/`return`, `escape`/`esc`, `insert`, `delete`/`del`, `home`, `end`, `pageup`/`pgup`, `pagedown`/`pgdn`, `space`, and keypad keys (e.g. `kp_enter`).
  - **Supported Modifiers**: `shift`, `ctrl`/`control`, `alt`/`meta`.
  - **Note on Modifiers**: Compound keys with modifiers (like `"ctrl,left"`) are delivered as raw escape sequences (e.g. `\e[1;5D` for `Ctrl+Left`) unless explicitly bound by the active terminfo file and keyboard/keypad translation is enabled.
- `term.send_signal(sig)` `( integer -- )`: Sends a POSIX signal to the child process.
- `term.sleep_ms(ms)` `( integer -- )`: Blocks script execution for `ms` milliseconds.
- `term.set_disable_alternate_screen(disable)` `( boolean -- )`: Disables switching to the alternate screen buffer.

#### Waiting

- `term.wait_idle(quiet_ms, deadline_ms)` `( integer, integer -- string )`: Waits for terminal to be idle (no updates for `quiet_ms` or `deadline_ms` elapsed). Returns `"idle"`, `"deadline"`, or `"exited"`.
- `term.wait_for_text(text, deadline_ms)` `( string, integer -- string )`: Waits for the given text to appear on the screen. Parses escape sequences in `text`. Returns `"found"`, `"timeout"`, or `"exited"`.
- `term.wait_for_screen_change(baseline_snap_id, deadline_ms)` `( integer, integer -- string )`: Blocks until the screen state differs from the baseline snapshot. Returns `"changed"`, `"timeout"`, or `"exited"`.

#### Watchers & WaitAny

- `term.watch_text(text)` `( string -- userdata )`: Creates a text watcher descriptor. Parses escape sequences.
- `term.watch_timeout(ms)` `( integer -- userdata )`: Creates a timeout watcher descriptor.
- `term.watch_pattern(lua_pattern)` `( string -- userdata )`: Creates a watcher that matches using Lua's built-in pattern matching engine.
- `term.watch_any_text(texts)` `( table -- userdata )`: Creates a custom watcher that matches if any string in the `texts` array is found on the screen.
- `term.watch_any_pattern(patterns)` `( table -- userdata )`: Creates a custom watcher that matches if any Lua pattern in the `patterns` array is found on the screen.
- `term.wait_any(w1, w2, ...)` `( userdata... -- userdata | string )`: Blocks until one watcher fires, returning that watcher userdata. If the terminal exits during wait, returns `"exited"`.

#### Queries

- `term.get_status()` `( -- string )`: Returns current status (`"running"` or `"exited <code_int>"`).
- `term.get_terminal_size()` `( -- integer, integer )`: Returns the terminal width and height.
- `term.get_keysyms()` `( -- table )`: Returns an array of all registered key name strings.
- `term.take_snapshot()` `( -- integer )`: Captures the current screen state, returning a snapshot ID.
- `term.dump_screen([snapshot_id])` `( [integer] -- string )`: Returns formatted text representation of screen (default: current screen / `-1`).
- `term.dump_screen_html([snapshot_id])` `( [integer] -- string )`: Returns formatted HTML representation of screen with colors and styles preserved (default: current screen / `-1`).
- `term.get_screen([snapshot_id])` `( [integer] -- string )`: Returns screen text content with trailing whitespace on lines trimmed (default: current / `-1`).
- `term.get_cursor([snapshot_id])` `( [integer] -- integer, integer, boolean )`: Returns cursor column `x` (0-indexed), row `y` (0-indexed), and visibility.
- `term.get_cell(x, y [, snapshot_id])` `( integer, integer [, integer] -- table )`: Returns a table describing the cell at `(x, y)`: `{char, fg, bg, bold, italic, underline, inverse, protect, blink}`.
- `term.get_row(row [, snapshot_id])` `( integer [, integer] -- string )`: Returns the text of a single row with trailing whitespace trimmed.
- `term.get_rows(start_row, end_row [, snapshot_id])` `( integer, integer [, integer] -- table )`: Fetches a range of rows as an array of strings in a single call.
- `term.get_attributes([snapshot_id])` `( [integer] -- string )`: Returns a formatted list of unique formatting attributes.
- `term.find_text(text [, snapshot_id])` `( string [, integer] -- table )`: Searches the screen, returning a list of match coordinates: `{ {row, col_start, col_end}, ... }`.
- `term.find_pattern(lua_pattern [, snapshot_id])` `( string [, integer] -- table )`: Searches the screen using a Lua pattern, returning a list of match coordinates: `{ {row, col_start, col_end}, ... }`.
- `term.get_diff(snap_a, snap_b)` `( integer, integer -- string )`: Returns a text summary comparing differences between two snapshot IDs.
- `term.get_diff_structured(snap_a, snap_b)` `( integer, integer -- table )`: Programmatic diff returning a table of difference objects: `{ {row, col_start, col_end, old, new}, ... }`.
- `term.get_scrollback(lines)` `( integer -- table )`: Returns an array of the last `lines` of scrollback.
- `term.resize(width, height)` `( integer, integer -- )`: Resizes the terminal columns and rows.

### Terminal Capability Levels

The `terminal` parameter on `create_session` controls what terminal capabilities the launched application sees. The default is `"full"` (xterm-256color). Use lower levels like `"basic"` (vt100) or `"extended"` (vt220) to test degraded-terminal behaviour. Color depth can be controlled independently by appending a suffix: `"full-8color"`, `"basic-16color"`, etc. Read the `termobulator://docs/terminal-levels` resource for the full reference.
