# Contributing to termobulator

Thank you for your interest in contributing!

## Requirements

- A C++17 compiler (GCC or Clang)
- CMake ≥ 3.12
- `libtsm-dev` (`sudo apt install libtsm-dev` on Debian/Ubuntu)
- Python 3 (for integration tests)

## Building

```sh
mkdir build && cd build
cmake ..
cmake --build .
```

## Running tests

```sh
ctest --test-dir build --output-on-failure
```

## Code style

This project uses `clang-format`. Before submitting, run:

```sh
clang-format -i *.cc *.h
```

The `.clang-format` file at the repository root defines the style.

## Submitting changes

- Open an issue before starting significant work, to avoid duplicated effort.
- Keep pull requests focused: one logical change per PR.
- All new source files must carry the SPDX header:

  ```cpp
  // SPDX-License-Identifier: Apache-2.0
  // Copyright 2026 The Termobulator Authors.
  ```

- CI must pass before a PR will be merged.

## License

By contributing you agree that your contributions will be licensed under the
[Apache License 2.0](LICENSE).
