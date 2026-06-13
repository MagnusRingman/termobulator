# `termobulator`

A headless terminal emulator designed primarily as a **Model Context Protocol (MCP) server** for AI/LLM agents, with an interactive command-line interface (CLI) for scripting and testing, and a C++ library. Run any TUI program behind it, and programmatically spawn sessions, control input, read screen contents, capture scrollback, and query terminal attributes.

`termobulator` spawns a client (either as a subprocess or inside a dedicated thread) on the slave side of a virtual PTY, feeds its output into a `libtsm`-based terminal state machine on the master side, and exposes an interactive interface. This enables automated headless testing, event playback, assertions, and LLM-agent-driven control of curses/terminal-based interfaces.

---

## Building

```sh
mkdir build && cd build
cmake ..
cmake --build .
```

**Requirements**: `libtsm` (header at `/usr/include/libtsm.h`, links as `-ltsm`), a C++17 compiler, POSIX pthreads.

The CMake build produces:

- `libtermobulator` — shared library (`libtermobulator.so`)
- `termobulator` — the main command-line driver

---

## Model Context Protocol (MCP) Mode

`termobulator` runs as an MCP server, allowing LLMs and AI agents to interact with terminal processes programmatically using standard JSON-RPC tool calls.

To run in MCP mode:

```sh
termobulator --mcp [options]
```

### Options

| Option | Argument | Description |
| --- | --- | --- |
| `--mcp`, `-m` | None | Run in MCP server mode. |
| `--width` | `<width>` | Default terminal width (columns, default: 80). |
| `--height` | `<height>` | Default terminal height (rows, default: 24). |
| `--terminal` | `<term>` | Default terminal type (default: `tmux-256color`). |
| `--locale` | `<locale>` | Default locale setting. |
| `--idle-timeout`, `-i` | `<minutes>` | Idle timeout in minutes after which inactive sessions are closed (default: 300). |
| `--idle-timeout-sec` | `<seconds>` | Idle timeout in seconds after which inactive sessions are closed. |
| `--do_log` | None | Enable JSON-RPC traffic logging. Logs are written to `/tmp/termobulator-<pid>.log`. |

> [!NOTE]
> In MCP mode, target binaries and arguments are not specified at startup. Instead, terminal sessions are created and managed dynamically by the client agent via MCP tool calls.

### Exposed MCP Tools

| Tool | Parameters | Description |
| --- | --- | --- |
| `create_session` | `binary` (string, req), `arguments` (array, opt), `session_id` (string, opt), `width` (int, opt), `height` (int, opt), `terminal` (string, opt), `locale` (string, opt), `disable_alternate_screen` (bool, opt), `scrollback_size` (int, opt) | Spawn a new terminal session running the specified binary. |
| `close_session` | `session_id` (string, req) | Terminate and close a terminal session. |
| `list_sessions` | None | List all active sessions and their status/exit status. |
| `set_active_session` | `session_id` (string, req) | Set the default active session for subsequent tool calls. |
| `execute_dsl` | `instructions` (array/string, req), `session_id` (string, opt) | Execute a series of DSL instructions on the session's persistent stack machine. |

---

## Domain Specific Language (DSL) Specification

All terminal interaction and state queries are unified under a stack-based DSL execution engine.

### Conceptual Model
- **Persistent State**: Each terminal session maintains a stack and a variable dictionary between `execute_dsl` calls.
- **Literals**: Values of types integer, boolean, array, and object are pushed directly onto the stack.
  - **JSON Array format**: Prefix a literal string with `$` (e.g. `"$myvar"`) to push it as a literal string.
  - **Structured Literals**: Pushing `{"lit": <value>}` pushes `<value>` directly onto the stack (e.g. `{"lit": "f4"}`).
  - **Structured Operations**: Pushing `{"op": "<op_name>", "args": [<args>...]}` executes the operation with the specified arguments in order, avoiding manual stack-shuffling.
- **Operations**: Unprefixed/unquoted strings are executed as operations (verbs) that pop arguments and push results.
- **Fail-Fast & Auto-Clear**: Any execution error (stack underflow, type mismatch, invalid operation) aborts execution immediately, returning detailed diagnostics (instruction index, failing token, stack state at failure), and automatically clears the session stack to prevent state pollution.
- **Opaque Snapshots**: A snapshot is an immutable entity stored in the session registry and referenced on the stack as an integer snapshot ID.

### Available Operations

| Operation | Stack Notation | Description |
| --- | --- | --- |
| **Stack Manipulation** | | |
| `dup` | `( x -- x x )` | Duplicate the top item. |
| `dup2` | `( x y -- x y x y )` | Duplicate the top two items. |
| `drop` | `( x -- )` | Discard the top item. |
| `swap` | `( x y -- y x )` | Swap the top two items. |
| `over` | `( x y -- x y x )` | Duplicate the second item to the top. |
| `rot` | `( x y z -- y z x )` | Rotate the top three items. |
| `clear` | `( ... -- )` | Discard all stack elements. |
| **Control Flow & Execution** | | |
| `exec` | `( q -- ... )` | Execute the quotation/array `q`. |
| `if` | `( cond true_q false_q -- ... )` | If `cond` is non-zero/true, execute `true_q`, else execute `false_q`. |
| `dip` | `( x q -- ... x )` | Pop `x`, execute `q`, and then restore `x`. |
| `while` | `( cond_q body_q -- ... )` | Loop `body_q` while `cond_q` returns non-zero/true. |
| **Dictionary (Variables)** | | |
| `store` | `( val name_str -- )` | Bind `val` to variable name `name_str`. |
| `load` | `( name_str -- val )` | Retrieve the value of variable `name_str`. |
| **Logic, Comparison & Math** | | |
| `not` | `( x -- bool )` | Logical negation of boolean or number. |
| `equal` | `( x y -- bool )` | Check equality of top two items. |
| `empty` | `( val -- bool )` | Check if a string, array, or object is empty. |
| `size` | `( val -- int )` | Length/size of a string, array, or object. |
| `+` | `( x y -- sum )` | Integer addition. |
| `-` | `( x y -- diff )` | Integer subtraction. |
| **Terminal Interaction** | | |
| `sleep_ms` | `( ms_int -- )` | Delay execution for `ms_int` milliseconds. |
| `send_key` | `( keys_str -- )` | Send key/character string (supports escapes like `\n`, `\t`, `\xNN`). |
| `send_special_key` | `( keyname_str modifiers_str -- )` | Send special key (e.g. `"up"`, `"enter"`) with comma-separated modifiers (e.g. `"ctrl,alt"` or `""`). |
| `send_signal` | `( sig_int -- )` | Send POSIX signal `sig_int` to the child process. |
| `get_status` | `( -- status_str )` | Push process status (`"running"` or `"exited <code_int>"`). |
| `wait_idle` | `( quiet_ms deadline_ms -- result_str )` | Wait for terminal idleness. Pushes `"wait: idle"`, `"wait: deadline"`, or `"wait: exited"`. |
| `wait_for_text` | `( text_str deadline_ms -- result_str )` | Wait for text to appear on screen. Pushes `"wait-for-text: found"`, `"wait-for-text: timeout"`, or `"wait-for-text: exited"`. |
| **Snapshots & Queries** | | |
| `take_snapshot` | `( -- snapshot_id )` | Capture screen state, push integer ID. |
| `get_screen` | `( snapshot_id -- screen_str )` | Get text representation of a snapshot. |
| `get_cursor` | `( snapshot_id -- col_int row_int visible_bool )` | Push cursor column, row, and visibility. |
| `get_cell` | `( x_int y_int snapshot_id -- cell_obj )` | Push cell JSON object (char, fg, bg, and attributes). |
| `get_row` | `( row_int snapshot_id -- row_str )` | Push row text. |
| `get_attributes` | `( snapshot_id -- attr_list_str )` | Push unique style attributes summary. |
| `find_text` | `( query_str snapshot_id -- results_arr )` | Search for `query_str`, push array of `{"row": y, "col_start": x, "col_end": x}`. |
| `get_diff` | `( snapshot_id_b snapshot_id_a -- diff_str )` | Diff two snapshots, older first. Pushes text diff summary. |

---

## Command Line Interface (CLI) Mode

If a target binary and arguments are specified at startup without `--mcp`, `termobulator` runs in interactive CLI mode:

```sh
termobulator [--width <width>] [--height <height>] [--terminal <term>] [--locale <locale>] <binary> [args...]
```

The driver spawns the target binary inside a default 80×24 PTY, runs a DSL execution loop (REPL) on standard input, and outputs the resulting stack state as a JSON array to stdout.

### CLI Syntax
- Whitespace separates tokens.
- `[` and `]`: Form nested quotations/arrays.
- `"..."`: Parsed as literal strings. Standard backslash escapes (e.g. `\n`, `\t`) are supported.
- Numeric digits (e.g. `123`, `-5`): Parsed as literal integers.
- `true` or `false`: Parsed as literal booleans.
- Unquoted words: Executed as operations.
- `exit` or `quit`: Exits the CLI REPL.

#### Example Session:
```
> 20 200 wait_idle
["wait: idle"]
> clear "q\n" send_key
[]
> clear get_status
["exited 0"]
> exit
```


---

## C++ API Architecture

> [!WARNING]
> `termobulator` does not currently provide a stable API or ABI. All public types, interfaces, and factory functions are nested within the `termobulator::unstable` namespace (e.g., `termobulator::unstable::Terminal`).

`libtermobulator` decouples client code from `libtsm` header dependencies by exposing a pure virtual `Terminal` interface.

### Structs (`termobulator.h`)

- **`CellAttr`**: Style attributes (foreground/background color codes, RGB, bold, italic, underline, inverse, etc.).
- **`Cell`**: Contains a UTF-8 character string (`ch`) and its `CellAttr`.
- **`ScreenSnapshot`**: Saved grid state containing dimension (`width`, `height`), cursor info, and a flat `std::vector<Cell>`.

### The `Terminal` Interface (`termobulator.h`)

```cpp
enum class WaitResult { kIdle, kDeadline, kExited };

class Terminal {
  public:
    virtual ~Terminal() = default;

    virtual void SendRawBytes(std::string_view bytes) = 0;
    virtual std::string DumpScreen(int snapshot_id) = 0;
    std::string DumpScreen() { return DumpScreen(-1); }
    virtual int Snapshot() = 0;
    virtual ScreenSnapshot GetSnapshot(int snapshot_id) = 0;
    ScreenSnapshot GetSnapshot() { return GetSnapshot(-1); }
    virtual void SendKey(uint32_t keysym, unsigned int mods) = 0;
    void SendKey(uint32_t keysym) { SendKey(keysym, 0); }

    virtual unsigned int Width() const = 0;
    virtual unsigned int Height() const = 0;

    virtual bool IsExited() const = 0;
    virtual int ExitStatus() const = 0;
    virtual void SendSignal(int sig) = 0;
    virtual void Resize(unsigned int width, unsigned int height) = 0;
    virtual WaitResult WaitIdle(unsigned int quiet_ms,
                                unsigned int deadline_ms) = 0;

    virtual void SetDisableAlternateScreen(bool disable) = 0;
    virtual std::vector<std::string> GetScrollback(unsigned int lines) = 0;
};
```

### Factories

Clients instantiate terminals via two factory functions:

1. **Subprocess-based**:

   ```cpp
   std::unique_ptr<Terminal> CreateSubprocessTerminal(
       unsigned int width, unsigned int height,
       const std::string& cmd, const std::vector<std::string>& args,
       const std::string& term_type = "tmux-256color",
       const std::string& locale = "",
       unsigned int scrollback_size = 200);
   ```

   Spawns the client application in a separate child process. Before `exec`, the child's environment is adjusted: `TERM` is set to `tmux-256color` and `TERMCAP` is cleared, so the subprocess always sees a well-known terminal type regardless of the parent process environment.

2. **Thread-based (In-process)**:

   ```cpp
   std::unique_ptr<Terminal> CreateThreadTerminal(
       unsigned int width, unsigned int height,
       std::function<void()> client_func,
       unsigned int scrollback_size = 200);
   ```

   Runs `client_func` inside a dedicated thread within the host process, redirecting its stdio descriptors to the PTY slave. Ideal for unit testing framework integrations (e.g. `gtest`). Unlike the subprocess variant, the thread shares the host process environment, so `TERM`, `TERMCAP`, and all other environment variables are inherited as-is; it is the caller's responsibility to set them appropriately before invoking the factory.

---

## Implementation Details

### Signal Handling & Thread Isolation

For the thread-based implementation (`ThreadTerminalImpl`), we route PTY window-change events carefully:

- Because the client runs in a thread under the main process, `ioctl(TIOCSWINSZ)` on the master PTY does not automatically trigger `SIGWINCH` delivery to the client thread.
- During initialization, `ThreadTerminalImpl` temporarily blocks `SIGWINCH` on the parent thread so that spawned helper threads (like the reader) inherit a blocked signal mask.
- The client thread explicitly unblocks `SIGWINCH` when starting up.
- On calling `Resize(width, height)`, `ThreadTerminalImpl` performs the PTY size change and sends `SIGWINCH` directly to the client thread using `pthread_kill(client_thread_.native_handle(), SIGWINCH)`.

---

## Licensing and Dependencies

`termobulator` is licensed under the Apache License 2.0. However, it relies on `libtsm` (Terminal-emulator State Machine), which is dually licensed under the MIT License and the GNU Lesser General Public License, version 2.1 (LGPLv2.1).

To maintain legal compatibility between the Apache 2.0 and LGPLv2.1 licenses, our build system (`CMakeLists.txt`) is configured to use **dynamic linking** to `libtsm` by default. This ensures that downstream users can modify or replace the `libtsm` library independently, in compliance with LGPLv2.1.

> [!WARNING]
> Downstream package maintainers: Forcing a static build of `libtsm` into the final `termobulator` binary may result in an incompatible license combination. If you modify the build matrix or link settings to compile `libtsm` statically, you assume full responsibility for any licensing issues that arise.
