// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "crash_test_dummy.h"

int main(int argc, char* argv[]) {
    try {
        crash_test_dummy::Run(argc, argv);
        return 0;
    } catch (...) {
        return 1;
    }
}
