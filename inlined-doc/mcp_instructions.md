# MCP Server Instructions

This server manages terminal sessions and provides tools to interact with them.
The `execute_dsl` tool executes a series of instructions on a session's persistent stack machine.

### DSL Conceptual Model

- **Persistent State**: Each terminal session maintains a stack and a variable dictionary between `execute_dsl` calls.
- **Literals**: Values of types integer, boolean, array, and object are pushed directly onto the stack.
- **Structured Literals**: Pushing `{"lit": value}` directly pushes `value` (e.g. `{"lit": "f4"}` or `{"lit": ""}`) without evaluating it or needing escapes.
- **Structured Operations**: Pushing `{"op": "op_name", "args": [lit_args...]}` evaluates the operation with the given literal arguments in order, avoiding stack-shuffling.
- **Escape Prefix**: In a JSON array, prefixing a string with '$' (e.g. '$myvar') pushes it as a literal string. In a text string, wrap it in double quotes (e.g. \"myvar\").
- **Operations**: Unprefixed/unquoted strings are executed as operations (verbs) that pop arguments and push results.
- **Fail-Fast & Auto-Clear**: Any error (stack underflow, type mismatch, invalid operation) aborts execution immediately, returning a detailed error message, and automatically clears the stack of the session.
- **Opaque Snapshots**: A snapshot is an immutable entity stored in the session registry and referenced on the stack as an integer snapshot ID.

### DSL Available Operations

- `dup` `( x -- x x )`: Duplicate top item.
- `dup2` `( x y -- x y x y )`: Duplicate top two items.
- `drop` `( x -- )`: Discard top item.
- `swap` `( x y -- y x )`: Swap top two items.
- `over` `( x y -- x y x )`: Duplicate second item to top.
- `rot` `( x y z -- y z x )`: Rotate top three items.
- `clear` `( ... -- )`: Discard all stack elements.
- `store` `( val name_str -- )`: Bind value to variable name.
- `load` `( name_str -- val )`: Retrieve value of variable name.
- `exec` `( q -- ... )`: Execute quotation/array `q`.
- `if` `( cond true_q false_q -- ... )`: If cond is non-zero/true execute true_q, else false_q.
- `dip` `( x q -- ... x )`: Pop x, execute q, restore x.
- `while` `( cond_q body_q -- ... )`: Loop body_q while cond_q returns true/non-zero.
- `not` `( x -- bool )`: Logical negation.
- `equal` `( x y -- bool )`: Check equality.
- `empty` `( val -- bool )`: Check if string/array/object is empty.
- `size` `( val -- int )`: Length of string/array/object.
- `sleep_ms` `( ms_int -- )`: Sleep/delay execution for ms_int milliseconds.
- `send_key` `( keys_str -- )`: Send key string (supports escapes like \n, \t, \xNN).
- `send_special_key` `( keyname_str modifiers_str -- )`: Send special key with comma-separated modifiers (e.g. 'ctrl,alt' or '').
  - **Supported Key Names**: `up`, `down`, `left`, `right`, `f1` to `f20`, `backspace`, `tab`, `enter`/`return`, `escape`/`esc`, `insert`, `delete`/`del`, `home`, `end`, `pageup`/`pgup`, `pagedown`/`pgdn`, `space`, and keypad keys (e.g. `kp_enter`).
  - **Supported Modifiers**: `shift`, `ctrl`/`control`, `alt`/`meta`.
- `send_signal` `( sig_int -- )`: Send POSIX signal to child process.
- `get_status` `( -- status_str )`: Push status ('running' or 'exited <code_int>').
- `wait_idle` `( quiet_ms deadline_ms -- result_str )`: Wait for terminal to be idle. Pushes 'wait: idle', 'wait: deadline', or 'wait: exited'.
- `wait_for_text` `( text_str deadline_ms -- result_str )`: Wait for text to appear. Pushes 'wait-for-text: found', 'wait-for-text: timeout', or 'wait-for-text: exited'.
- `take_snapshot` `( -- snapshot_id )`: Capture screen state, push ID.
- `get_screen` `( snapshot_id -- screen_str )`: Get screen text of snapshot.
- `get_cursor` `( snapshot_id -- col_int row_int visible_bool )`: Push cursor column, row, and visibility.
- `get_cell` `( x_int y_int snapshot_id -- cell_obj )`: Push cell JSON object (char, fg, bg, and attributes).
- `get_row` `( row_int snapshot_id -- row_str )`: Push text of single row.
- `get_attributes` `( snapshot_id -- attr_list_str )`: Push list of unique formatting attributes.
- `find_text` `( query_str snapshot_id -- results_arr )`: Find query, push array of `{"row": y, "col_start": x, "col_end": x}`.
- `get_diff` `( snapshot_id_b snapshot_id_a -- diff_str )`: Diff snapshots, older first. Pushes text summary of difference.

### Terminal Capability Levels

The `terminal` parameter on `create_session` controls what terminal capabilities the launched application sees. The default is `"full"` (xterm-256color). Use lower levels like `"basic"` (vt100) or `"extended"` (vt220) to test degraded-terminal behaviour. Color depth can be controlled independently by appending a suffix: `"full-8color"`, `"basic-16color"`, etc. Read the `termobulator://docs/terminal-levels` resource for the full reference.
