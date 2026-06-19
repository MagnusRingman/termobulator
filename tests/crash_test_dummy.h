// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_TESTS_CRASH_TEST_DUMMY_H
#define TERMOBULATOR_TESTS_CRASH_TEST_DUMMY_H

namespace crash_test_dummy {

// Unified entry point for both standalone execution and in-process thread
// execution. Throws std::runtime_error if single-instance constraint is
// violated or initialization fails.
void Run(int argc, char* argv[]);

}  // namespace crash_test_dummy

#endif  // TERMOBULATOR_TESTS_CRASH_TEST_DUMMY_H
