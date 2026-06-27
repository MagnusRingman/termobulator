# Terminal Capability Levels

The `terminal` parameter on `create_session` controls what terminal capabilities
the launched application sees. Termobulator's VTE always emulates the same full
feature set; what changes is the `terminfo` entry the application looks up via
`$TERM`, which determines which features it will try to use.

## Base capability levels

| Symbolic name | `$TERM` value    | Description                                                                             |
|---------------|------------------|-----------------------------------------------------------------------------------------|
| `minimal`     | `dumb`           | No cursor addressing, no color, no styling. Raw line-mode only.                         |
| `basic`       | `vt100`          | Cursor addressing, bold, underline, reverse video. No color.                            |
| `extended`    | `vt220`          | Adds insert/delete line, full function-key set (F1-F20), cursor visibility control.     |
| `full`        | `xterm-256color` | 256-color palette, mouse tracking, bracketed paste, italic, strikethrough. **Default.** |

## Color depth suffixes

Color depth can be specified independently by appending a suffix to the base level name:

| Suffix         | Colors | Description                                                 |
|----------------|--------|-------------------------------------------------------------|
| `-mono`        | 0      | No color capabilities at all.                               |
| `-8color`      | 8      | Standard ANSI 8-color palette (SGR 30-37/40-47).            |
| `-16color`     | 16     | 8 normal + 8 bright (aixterm-style extensions).             |
| `-256color`    | 256    | Full 256-color indexed palette.                             |
| `*(no suffix)*`|        | Level default: mono for basic/extended, 256color for full.  |

## Full combinatorial matrix

| Symbolic name                 | `$TERM` value                     |
|-------------------------------|-----------------------------------|
| `minimal`                     | `dumb`                            |
| `basic` / `basic-mono`        | `vt100`                           |
| `basic-8color`                | `termobulator-basic-8color`       |
| `basic-16color`               | `termobulator-basic-16color`      |
| `basic-256color`              | `termobulator-basic-256color`     |
| `extended` / `extended-mono`  | `vt220`                           |
| `extended-8color`             | `termobulator-extended-8color`    |
| `extended-16color`            | `termobulator-extended-16color`   |
| `extended-256color`           | `termobulator-extended-256color`  |
| `full` / `full-256color`      | `xterm-256color`                  |
| `full-16color`                | `xterm-16color`                   |
| `full-8color`                 | `termobulator-full-8color`        |
| `full-mono`                   | `termobulator-full-mono`          |

`minimal` has no color variants: `dumb` lacks cursor addressing and SGR support
entirely. Strings like `minimal-8color` are treated as raw terminfo names and
will trigger an unknown-type warning.

## Guidance

- **Compatibility testing**: Use `basic` or `extended` to verify the TUI works on
  legacy terminals without color. Use `minimal` to test graceful degradation.
- **Color regression testing**: Use `full-8color` or `full-16color` to reproduce
  the "angry fruit salad" failure mode where a TUI designed for 256 colors has
  indices clamped to a smaller palette.
- **Raw terminfo names**: Any string not matching a symbolic name is passed through
  as a raw `$TERM` value. If it is not in the known-good set, a `warning` field
  will appear in the `create_session` response.
- **Case-insensitive**: Symbolic names are matched case-insensitively
  (`"FULL-MONO"` and `"full-mono"` are equivalent).
