# `termobulator`

A headless terminal emulator designed as a **Model Context Protocol (MCP) server** for AI/LLM agents, and a C++ library. Run any TUI program behind it, and programmatically spawn sessions, control input, read screen contents, capture scrollback, and query terminal attributes.

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
- `termobulator` — the MCP server binary

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
| `execute_dsl` | `script` (string, req), `session_id` (string, opt) | Execute a sandboxed Lua 5.4 script on the session's persistent terminal and state. |
| `register_watcher` | `watcher_id` (string, req), `condition` (object, req), `session_id` (string, opt) | Register a persistent watcher on the targeted session. |
| `check_watchers` | `session_id` (string, opt) | Poll and retrieve any fired watcher events for the targeted session. |
| `await_watchers` | `timeout_ms` (integer, req), `watcher_ids` (array, opt), `session_id` (string, opt) | Block until a persistent watcher fires on the targeted session. |

---

## Lua Scripting & Execution

All terminal interaction, assertions, and state queries are executed via sandboxed Lua 5.4 scripts through the `execute_dsl` tool.

### Features

- **Sandboxed Environment**: Only safe standard Lua libraries (`base`, `string`, `table`, `math`, `utf8`) are loaded.
- **Global `vars` Table**: Persists across calls. Only values that serialize cleanly to JSON (numbers, strings, booleans, arrays, and string-keyed tables) are preserved.
- **Global `log(...)`**: Appends diagnostic messages to a log buffer returned to the caller.
- **Terminal Namespace `term.*`**: Exposes APIs for terminal input (`send_key`, `send_special_key`), state inspection (`dump_screen_html`, `get_cursor`, `get_cell`), and transient/asynchronous waiting (`wait_idle`, `wait_for_text`, `wait_any`).

For the complete Lua scripting model and a full reference of the `term.*` namespace APIs, see [MCP Server Instructions](file:///home/bmr/src/termobulator/inlined-doc/mcp_instructions.md).

> [!CAUTION]
> Sandboxing is enforced strictly at the Lua language level by disabling standard libraries (like `io`, `os`, and `package`) and pruning dangerous globals. It does not (yet) use OS-level containment (such as `bubblewrap` or `firejail`).

---

## C++ API Architecture

> [!WARNING]
> `termobulator` does not currently provide a stable API or ABI. All public types, interfaces, and factory functions are nested within the `termobulator::unstable` namespace (e.g., `termobulator::unstable::Terminal`).

`libtermobulator` decouples client code from `libtsm` header dependencies by exposing a pure virtual `Terminal` interface.

### Structs (`termobulator.h`)

- **`CellAttr`**: Style attributes (foreground/background color codes, RGB, bold, italic, underline, inverse, etc.).
- **`Cell`**: Contains a UTF-8 character string (`ch`) and its `CellAttr`.
- **`ScreenSnapshot`**: Saved grid state containing dimension (`width`, `height`), cursor info, and a flat `std::vector<Cell>`.
- **`WatcherDescriptor`**: Conditions for screen-matching or timing out.
- **`WatchResult`**: Result containing the index of the triggered watcher.

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
    virtual WatchResult WaitAny(
        const std::vector<WatcherDescriptor> &conditions) = 0;

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
