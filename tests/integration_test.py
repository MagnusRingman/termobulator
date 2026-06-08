#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The Termobulator Authors.

import subprocess
import time
import traceback
import sys
import os

def run_integration_test():
    # Resolve paths relative to script location to support running both locally and from CTest
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    
    if os.path.exists("./termobulator"):
        binary_path = "./termobulator"
    elif os.path.exists(os.path.join(project_root, "build", "termobulator")):
        binary_path = os.path.join(project_root, "build", "termobulator")
    else:
        binary_path = "./build/termobulator"
        
    target_path = os.path.join(script_dir, "target.sh")
    
    if not os.path.exists(binary_path):
        print(f"Error: Executable {binary_path} not found. Please compile the project first.")
        sys.exit(1)
        
    os.chmod(target_path, 0o755)

    print("Running integration test for new CLI size option...")
    proc_size = subprocess.Popen(
        [binary_path, "--width", "40", "-h", "10", target_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True
    )
    try:
        proc_size.stdin.write("wait 20 200\n")
        proc_size.stdin.write("screen\n")
        proc_size.stdin.flush()
        wait_out = proc_size.stdout.readline().strip()
        assert wait_out == "wait: idle", f"Expected 'wait: idle', got '{wait_out}'"
        lines = []
        expected_height = 10
        for _ in range(expected_height + 2):
            lines.append(proc_size.stdout.readline().strip())
        assert len(lines[0]) == 42, f"Expected border width 42, got {len(lines[0])}"
        assert lines[0] == "+" + "-"*40 + "+", f"Invalid border line: {lines[0]}"
        assert len(lines) == expected_height + 2, f"Expected {expected_height + 2} lines for screen output, got {len(lines)}"
        proc_size.stdin.write("exit\n")
        proc_size.stdin.flush()
        proc_size.wait(timeout=2.0)
    except Exception as e:
        proc_size.kill()
        traceback.print_exc()
        print("CLI size option test failed:", e)
        sys.exit(1)

    print("Running main integration test...")
    proc = subprocess.Popen(
        [binary_path, target_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True
    )

    def read_until(proc, marker, timeout=2.0):
        lines = []
        start_time = time.time()
        while time.time() - start_time < timeout:
            line = proc.stdout.readline()
            if not line:
                break
            lines.append(line.strip())
            if marker in line:
                return lines
        return lines

    try:

        # 1. Wait for target to become idle
        proc.stdin.write("wait 20 200\n")
        proc.stdin.flush()
        wait_line1 = proc.stdout.readline().strip()
        assert wait_line1 == "wait: idle", f"Expected 'wait: idle', got '{wait_line1}'"
        
        # Test screen-raw
        proc.stdin.write("screen-raw\n")
        proc.stdin.flush()
        raw_lines = []
        for _ in range(24):
            raw_lines.append(proc.stdout.readline().strip())
        assert raw_lines[0].startswith("READY"), f"Expected line to start with READY, got: {raw_lines[0]}"
        assert not any(l.startswith("|") for l in raw_lines), "screen-raw output should not contain borders"

        # Test wait-for-text found
        proc.stdin.write("wait-for-text READY 500\n")
        proc.stdin.flush()
        wft_found = proc.stdout.readline().strip()
        assert wft_found == "wait-for-text: found", f"Expected 'wait-for-text: found', got '{wft_found}'"

        # Test wait-for-text timeout
        proc.stdin.write("wait-for-text NOTPRESENT 50\n")
        proc.stdin.flush()
        wft_timeout = proc.stdout.readline().strip()
        assert wft_timeout == "wait-for-text: timeout", f"Expected 'wait-for-text: timeout', got '{wft_timeout}'"

        # Test find command
        proc.stdin.write("find READY\n")
        proc.stdin.write("status\n")
        proc.stdin.flush()
        find_lines = read_until(proc, "running")
        print("Find output:", find_lines)
        assert "row 0 col 0-4" in find_lines, "Expected 'row 0 col 0-4' in find output"

        # Test find with snapshot
        proc.stdin.write("snapshot\n")
        proc.stdin.flush()
        snap_line = proc.stdout.readline().strip()
        print("Snapshot line:", snap_line)
        assert snap_line == "snapshot 0", f"Expected 'snapshot 0', got '{snap_line}'"

        proc.stdin.write("find READY 0\n")
        proc.stdin.write("status\n")
        proc.stdin.flush()
        find_snap_lines = read_until(proc, "running")
        print("Find snap output:", find_snap_lines)
        assert "row 0 col 0-4" in find_snap_lines, "Expected 'row 0 col 0-4' in find snap output"

        # Test find with non-existent string
        proc.stdin.write("find NONEXISTENT\n")
        proc.stdin.write("status\n")
        proc.stdin.flush()
        find_empty_lines = read_until(proc, "running")
        print("Find empty output:", find_empty_lines)
        assert "not found" in find_empty_lines, "Expected 'not found' in find empty output"

        # Test find with empty string
        proc.stdin.write("find \"\"\n")
        proc.stdin.write("status\n")
        proc.stdin.flush()
        find_empty_q_lines = read_until(proc, "running")
        print("Find empty query output:", find_empty_q_lines)
        assert "empty query" in find_empty_q_lines, "Expected 'empty query' in find empty query output"

        # Test keysyms command
        proc.stdin.write("keysyms\n")
        proc.stdin.write("status\n")
        proc.stdin.flush()
        keysyms_lines = read_until(proc, "running")
        print("Keysyms output:", keysyms_lines)
        assert len(keysyms_lines) >= 2, "Expected output from keysyms command"
        assert "space" in keysyms_lines[0], "Expected 'space' keysym in keysyms output"

        # Test attr-map with description
        proc.stdin.write("attr-map \"fg=16 bg=17\" 0\n")
        proc.stdin.write("status\n")
        proc.stdin.flush()
        attr_map_lines = read_until(proc, "running")
        print("Attr map output:", attr_map_lines)
        assert any("row 0: col" in l for l in attr_map_lines), "Expected row 0 ranges in attr-map output"

        # Send key 'q' followed by a newline to flush canonical mode input
        proc.stdin.write("key q\\n\n")
        proc.stdin.flush()

        # Wait for exit and dump status
        proc.stdin.write("wait 100 1000\n")
        proc.stdin.flush()
        wait_line2 = proc.stdout.readline().strip()
        assert wait_line2 == "wait: exited", f"Expected 'wait: exited', got '{wait_line2}'"

        # Take snapshot 1 after exit
        proc.stdin.write("snapshot\n")
        proc.stdin.flush()
        snap_line1 = proc.stdout.readline().strip()
        assert snap_line1 == "snapshot 1", f"Expected 'snapshot 1', got '{snap_line1}'"

        # Test snapshot-to-snapshot diff
        proc.stdin.write("diff 0 1\n")
        proc.stdin.write("status\n")
        proc.stdin.flush()
        diff_lines = read_until(proc, "exited 0")
        print("Diff output:", diff_lines)
        assert any("row 0" in l for l in diff_lines), "Expected row 0 change in diff output"

        proc.stdin.write("status\n")
        proc.stdin.flush()
        status_line = proc.stdout.readline().strip()
        print("Status output:", status_line)
        assert status_line == "exited 0", f"Expected 'exited 0', got '{status_line}'"

        # 5. Tell termobulator to exit
        proc.stdin.write("exit\n")
        proc.stdin.flush()

        # Wait for the program to terminate
        proc.wait(timeout=2.0)
        
    except Exception as e:
        proc.kill()
        traceback.print_exc()
        print("Main integration test failed:", e)
        sys.exit(1)

    print("Running integration test for keysym space...")
    proc_space = subprocess.Popen(
        [binary_path, target_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True
    )
    try:
        proc_space.stdin.write("wait 20 200\n")
        proc_space.stdin.flush()
        wait_idle = proc_space.stdout.readline().strip()
        assert wait_idle == "wait: idle", f"Expected 'wait: idle', got '{wait_idle}'"
        
        proc_space.stdin.write("special space\n")
        proc_space.stdin.write("key \\n\n")
        proc_space.stdin.write("wait 100 1000\n")
        proc_space.stdin.write("status\n")
        proc_space.stdin.flush()
        
        wait_result = proc_space.stdout.readline().strip()
        assert wait_result in ("wait: exited", "wait: idle"), f"Expected wait result, got '{wait_result}'"
        status_line = proc_space.stdout.readline().strip()
        print("Space test status output:", status_line)
        assert status_line == "exited 1", f"Expected 'exited 1', got '{status_line}'"
        
        proc_space.stdin.write("exit\n")
        proc_space.stdin.flush()
        proc_space.wait(timeout=2.0)
    except Exception as e:
        proc_space.kill()
        traceback.print_exc()
        print("Keysym space test failed:", e)
        sys.exit(1)

    print("All integration tests passed successfully!")

if __name__ == "__main__":
    run_integration_test()
