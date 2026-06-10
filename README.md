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
| `get_screen` | `snapshot_id` (int, opt), `session_id` (string, opt) | Get the raw text contents of the screen. |
| `get_scrollback` | `lines` (int, req), `format` (string, opt: `"text"` or `"lines"`), `session_id` (string, opt) | Retrieve the scrollback history buffer for a terminal session. |
| `get_cursor` | `snapshot_id` (int, opt), `session_id` (string, opt) | Get cursor position and visibility state. |
| `get_cell` | `x` (int, req), `y` (int, req), `snapshot_id` (int, opt), `session_id` (string, opt) | Get character and attributes at specific coordinates. |
| `get_row` | `row` (int, req), `snapshot_id` (int, opt), `session_id` (string, opt) | Get text contents of a single screen row. |
| `get_area` | `x` (int, req), `y` (int, req), `width` (int, req), `height` (int, req), `format` (string, opt: `"text"` or `"lines"`), `include_text` (bool, opt), `include_attrs` (bool, opt), `snapshot_id` (int, opt), `session_id` (string, opt) | Retrieve characters and/or attributes from a rectangular area on the screen. |
| `get_attributes` | `snapshot_id` (int, opt), `attribute_id` (int, opt), `include_ranges` (bool, opt), `session_id` (string, opt) | List unique cell style attributes present in the snapshot, or get ranges where an attribute is active. |
| `send_key` | `keys` (string, req), `session_id` (string, opt) | Send text/escaped characters to the terminal PTY. |
| `send_special_key` | `keyname` (string, req), `modifiers` (array, opt), `session_id` (string, opt) | Send a special key (e.g. arrow, enter) with optional modifiers. |
| `send_signal` | `signal` (int, opt), `session_id` (string, opt) | Send a POSIX signal to the terminal subprocess. |
| `get_status` | `session_id` (string, opt) | Check whether the child subprocess is running or exited. |
| `wait_idle` | `quiet_ms` (int, req), `deadline_ms` (int, req), `session_id` (string, opt) | Wait until the screen is idle for `quiet_ms`. |
| `wait_for_text` | `text` (string, req), `deadline_ms` (int, req), `session_id` (string, opt) | Wait until a specific string is found on the screen. |
| `find_text` | `text` (string, req), `snapshot_id` (int, opt), `session_id` (string, opt) | Locate all occurrences of a string on the screen. |
| `take_snapshot` | `session_id` (string, opt) | Capture a snapshot of the screen and return a snapshot ID. |
| `get_diff` | `snapshot_id_a` (int, req), `snapshot_id_b` (int, opt), `session_id` (string, opt) | Compare two snapshots or compare a snapshot to the current screen. |
| `resize_terminal` | `width` (int, req), `height` (int, req), `session_id` (string, opt) | Resize the terminal emulator width and height. |

---

## Command Line Interface (CLI) Mode

If target binary and arguments are specified at startup without `--mcp`, `termobulator` runs in interactive CLI mode:

```sh
termobulator [--width <width>] [--height <height>] [--terminal <term>] [--locale <locale>] <binary> [args...]
```

The driver spawns the target binary inside a default 80×24 PTY, reads line-oriented commands from stdin, and writes responses to stdout.

### CLI Commands

| Command | Usage | Description |
| --- | --- | --- |
| `key` | `key <escaped_str>` | Send characters to PTY. Supports standard backslash escapes (e.g. `\n`, `\t`, `\x1b`). |
| `special` | `special <keyname> [mods...]` | Send non-printable key with optional modifiers (e.g. `special up ctrl shift`). |
| `keysyms` | `keysyms` | List available special keysym names. |
| `screen` | `screen [snapshot_id]` | Dump the formatted text screen (bordered). |
| `screen-raw` | `screen-raw [snapshot_id]` | Dump raw screen contents row by row without borders or cursor info. |
| `scrollback` | `scrollback <lines>` | Retrieve the last `<lines>` of scrollback history. |
| `snapshot` | `snapshot` | Capture the current screen state and return a unique snapshot ID. |
| `cell` | `cell <y> <x> [snapshot_id]` | Get character at coordinates (x, y). |
| `row` | `row <y> [snapshot_id]` | Get characters on row `<y>`. |
| `range` | `range <y> <col_start> <col_end> [snapshot_id]` | Get characters on row `<y>` from column `<col_start>` to `<col_end>`. |
| `attributes` | `attributes [snapshot_id]` | List unique cell style attributes present in the snapshot. |
| `attr-map` | `attr-map <attr_id> [snapshot_id]` | Show column ranges on each row where attribute `<attr_id>` is active. |
| `cursor` | `cursor [snapshot_id]` | Get cursor location (x, y) and visibility status. |
| `diff` | `diff <snapshot_id>` | Show a text diff of the current screen against snapshot `<snapshot_id>`. |
| `find` | `find <string> [snapshot_id]` | Locate occurrences of a string in a snapshot or the current display. Supports standard backslash escapes (e.g. `\x20` for space). |
| `resize` | `resize <width> <height>` | Resize the terminal window. Warns that pre-resize snapshots are incompatible. |
| `status` | `status` | Get exit/running status of the subprocess. |
| `wait` | `wait <quiet_ms> <deadline_ms>` | Wait until the PTY is idle (no writes) for `<quiet_ms>` or the `<deadline_ms>` is reached. |
| `wait-for-text` | `wait-for-text <string> <deadline_ms>` | Wait until the string query appears on the screen or `<deadline_ms>` elapsed. |
| `kill` | `kill [signal]` | Send a signal (default SIGTERM=15) to the subprocess. |
| `exit` | `exit` | Exit the CLI driver. |

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
