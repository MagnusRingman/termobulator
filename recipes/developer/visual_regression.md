---
id: visual_regression
title: Visual Regression Testing
description: Patterns for capturing snapshots and asserting visual correctness of a TUI.
persona: developer
---

## Visual Regression Testing for TUIs

When developing or testing a TUI, you need systematic ways to verify that changes to the code do not break the visual layout, colors, alignment, or keyboard navigation.

## The Testing Loop

For each test case:

1. **Launch**: Start your TUI program with specified dimensions (e.g., 80x24).
2. **Interact**: Send a sequence of keys to navigate to a target state.
3. **Assert**: Retrieve a screen snapshot and assert visual correctness (text, colors, cell attributes).
4. **Compare**: Compare the output against a known baseline (snapshot testing).

## Key Patterns

### 1. Verification of Layout and Content

To check specific regions of the screen without doing full snapshot comparisons:

- Retrieve the current screen contents or snapshot ID using `term.take_snapshot()`, `term.get_screen()`, or `term.get_row()`.
- Check or assert that the extracted text matches expected values (e.g., headers, menu labels, or dialog contents).

### 2. Color and Attribute Verification

Do not just verify text; verify styling to prevent rendering degradation under different terminal capability levels:

- Inspect individual cell styling attributes (`bold`, `italic`, `underline`, `inverse`) using `term.get_cell(x, y)`.
- Inspect foreground and background colors (`fg` and `bg` hex/formatted strings returned by `term.get_cell(x, y)`).
- Ensure critical UI components (like selected menu items or error prompts) are visually distinct.

### 3. The Ephemeral Screen Capture Pattern

Standard full-screen TUIs clean up after themselves by switching back to the host terminal's primary screen buffer when exiting (sending the `rmcup` escape sequence). This restores the shell prompt and completely wipes the TUI from the terminal's visible state.

- **The Pitfall**: If you wait for the process to exit before taking a snapshot, the screen will be empty or show the restored shell.
- **The Solution**: Always capture your final screen snapshot or read screen contents *before* sending the exit command (`q`, `:q!`, etc.) and waiting for the process to terminate.

## Automated Verification Checklist

- Set fixed dimensions (e.g., 80x24) via `term.resize(80, 24)` or initial session config to ensure layout stability across test runs.
- Use `term.wait_idle(quiet_ms, deadline_ms)` after every keystroke sequence to allow the application to process inputs and render the screen fully.
- When testing terminal capabilities (e.g., monochrome vs 8-color vs 256-color), run the exact same test sequence under different `TERM` environments and assert appropriate fallback behavior.
