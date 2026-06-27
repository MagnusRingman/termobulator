---
id: first_contact
title: First Contact Interaction Loop
description: A systematic approach for an agent's first interaction with an unknown TUI.
persona: incidental
---

## First Contact: Interacting with an Unknown TUI

When you encounter a Terminal User Interface (TUI) for the first time, do not blindly send keystrokes. Follow this systematic "First Contact" recipe to discover its controls and interact with it safely.

## The Interaction Loop

Always follow this cycle:

1. **Act**: Send a keystroke or command.
2. **Wait**: Use `wait_idle` to let the application finish rendering.
3. **Snapshot**: Capture the screen state.
4. **Inspect**: Read the screen text and cursor position to assess the state.
5. **Decide**: Plan your next action based on the visible UI.

## Step-by-Step Procedure

### 1. Take an Initial Snapshot

Immediately after launching the TUI, take a snapshot and read the screen. Do not send any input yet.

* **Goal**: Identify what program is running and locate its layout (headers, status bars, menus).

### 2. Classify the UI Paradigm

Analyze the layout to determine which interaction family it belongs to:

* **Menu-driven** (e.g., `mc`, `nnn`): Shows lists of items. Uses arrow keys to navigate, `Enter` to select, and `Esc` or `q` to go back.
* **Vim-style modal** (e.g., `vim`, `less`): Uses modes. Expect normal mode by default. Keypresses like `:` start command mode. `Esc` returns to normal mode.
* **Dialog-based** (e.g., `whiptail`, `dialog`): Shows pop-up boxes. Uses `Tab` or arrows to move between buttons/fields, `Space` to toggle checkboxes, and `Enter` to select.
* **Readline prompt** (e.g., `gdb`, interactive shells): Shows a simple text prompt. Enter commands as text followed by `Enter`.

### 3. Find the Help System

Look for a help option on the screen. Common help triggers:

* Pressing `F1` or `?`.
* Typing `h`, `help`, or `:help`.
* Status bar hints at the top or bottom of the screen.

### 4. Perform the Desired Action

Once you understand the controls, navigate to the target field or menu:

* Send keys slowly. Do not buffer multiple key sequences unless necessary.
* Call `wait_idle` after each keypress to ensure the UI updates before you read it.

### 5. Graceful Exit

Always exit the TUI cleanly. Do not leave orphaned background processes.

* Try standard quit keys: `q`, `Ctrl-C`, `Ctrl-D`, `:q!`, or selecting an "Exit" menu item.
* Verify the process has exited using `wait_idle` or by checking if the process status is exited.
