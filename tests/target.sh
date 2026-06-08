#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The Termobulator Authors.
# interactive test target
echo -n "READY"
read -n 1 -r -s key
if [ "$key" = "q" ]; then
    echo -n "BYE"
    exit 0
fi
exit 1
