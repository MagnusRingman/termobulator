#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The Termobulator Authors.

import subprocess
import time
import traceback
import sys
import os
import json

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
        proc_size.stdin.write("20 200 wait_idle\n")
        proc_size.stdin.write("clear -1 get_screen\n")
        proc_size.stdin.flush()
        
        wait_out = json.loads(proc_size.stdout.readline().strip())
        assert wait_out == ["wait: idle"], f"Expected ['wait: idle'], got '{wait_out}'"
        
        screen_out = json.loads(proc_size.stdout.readline().strip())
        assert len(screen_out) == 1
        screen_str = screen_out[0]
        lines = screen_str.split("\n")
        expected_height = 10
        assert len(lines) == expected_height, f"Expected {expected_height} lines, got {len(lines)}"
        assert all(len(line) <= 40 for line in lines), "Expected all lines to be <= 40 characters"
        
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
        stderr=subprocess.PIPE,
        text=True
    )

    try:
        # 1. Wait for target to become idle
        proc.stdin.write("20 200 wait_idle\n")
        proc.stdin.flush()
        wait_line1 = json.loads(proc.stdout.readline().strip())
        assert wait_line1 == ["wait: idle"], f"Expected ['wait: idle'], got '{wait_line1}'"
        
        # Test get_screen
        proc.stdin.write("clear -1 get_screen\n")
        proc.stdin.flush()
        screen_res = json.loads(proc.stdout.readline().strip())
        assert len(screen_res) == 1
        screen_str = screen_res[0]
        lines = screen_str.split("\n")
        assert len(lines) == 24
        assert lines[0].startswith("READY"), f"Expected line to start with READY, got: {lines[0]}"

        # Test wait_for_text found
        proc.stdin.write("clear \"READY\" 500 wait_for_text\n")
        proc.stdin.flush()
        wft_found = json.loads(proc.stdout.readline().strip())
        assert wft_found == ["wait-for-text: found"], f"Expected ['wait-for-text: found'], got '{wft_found}'"

        # Test wait_for_text timeout
        proc.stdin.write("clear \"NOTPRESENT\" 50 wait_for_text\n")
        proc.stdin.flush()
        wft_timeout = json.loads(proc.stdout.readline().strip())
        assert wft_timeout == ["wait-for-text: timeout"], f"Expected ['wait-for-text: timeout'], got '{wft_timeout}'"

        # Test find command
        proc.stdin.write("clear \"READY\" -1 find_text\n")
        proc.stdin.flush()
        find_res = json.loads(proc.stdout.readline().strip())
        assert len(find_res) == 1
        assert find_res[0][0]["row"] == 0
        assert find_res[0][0]["col_start"] == 0

        # Test take_snapshot
        proc.stdin.write("clear take_snapshot\n")
        proc.stdin.flush()
        snap_res = json.loads(proc.stdout.readline().strip())
        assert snap_res == [0], f"Expected [0], got '{snap_res}'"

        # Test find with snapshot
        proc.stdin.write("clear \"READY\" 0 find_text\n")
        proc.stdin.flush()
        find_snap_res = json.loads(proc.stdout.readline().strip())
        assert find_snap_res[0][0]["row"] == 0
        assert find_snap_res[0][0]["col_start"] == 0

        # Test find with non-existent string
        proc.stdin.write("clear \"NONEXISTENT\" -1 find_text\n")
        proc.stdin.flush()
        find_empty_res = json.loads(proc.stdout.readline().strip())
        assert find_empty_res == [[]], f"Expected [[]], got '{find_empty_res}'"

        # Test find with empty string (errors out)
        proc.stdin.write("clear \"\" -1 find_text\n")
        proc.stdin.flush()
        err_line = proc.stderr.readline().strip()
        assert "Empty search query" in err_line, f"Expected 'Empty search query' in stderr, got '{err_line}'"

        # Send key 'q' followed by a newline to flush canonical mode input
        proc.stdin.write("clear \"q\\n\" send_key\n")
        proc.stdin.flush()
        send_key_res = json.loads(proc.stdout.readline().strip())
        assert send_key_res == [], f"Expected empty stack, got {send_key_res}"

        # Wait for exit and dump status
        proc.stdin.write("clear 100 1000 wait_idle\n")
        proc.stdin.flush()
        wait_line2 = json.loads(proc.stdout.readline().strip())
        assert wait_line2 == ["wait: exited"], f"Expected ['wait: exited'], got '{wait_line2}'"

        # Take snapshot 1 after exit
        proc.stdin.write("clear take_snapshot\n")
        proc.stdin.flush()
        snap_line1 = json.loads(proc.stdout.readline().strip())
        assert snap_line1 == [1], f"Expected [1], got '{snap_line1}'"

        # Test snapshot-to-snapshot diff
        proc.stdin.write("clear 0 1 get_diff\n")
        proc.stdin.flush()
        diff_res = json.loads(proc.stdout.readline().strip())
        assert any("row 0" in l for l in diff_res), "Expected row 0 change in diff output"

        # Test get_status
        proc.stdin.write("clear get_status\n")
        proc.stdin.flush()
        status_line = json.loads(proc.stdout.readline().strip())
        assert status_line == ["exited 0"], f"Expected ['exited 0'], got '{status_line}'"

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
        proc_space.stdin.write("20 200 wait_idle\n")
        proc_space.stdin.flush()
        wait_idle = json.loads(proc_space.stdout.readline().strip())
        assert wait_idle == ["wait: idle"], f"Expected ['wait: idle'], got '{wait_idle}'"
        
        proc_space.stdin.write("clear \"space\" \"\" send_special_key\n")
        proc_space.stdin.flush()
        res1 = json.loads(proc_space.stdout.readline().strip())
        assert res1 == []
        
        proc_space.stdin.write("clear \"\\n\" send_key\n")
        proc_space.stdin.flush()
        res2 = json.loads(proc_space.stdout.readline().strip())
        assert res2 == []
        
        proc_space.stdin.write("clear 100 1000 wait_idle\n")
        proc_space.stdin.flush()
        wait_result = json.loads(proc_space.stdout.readline().strip())
        assert wait_result in (["wait: exited"], ["wait: idle"]), f"Expected wait result, got '{wait_result}'"
        
        proc_space.stdin.write("clear get_status\n")
        proc_space.stdin.flush()
        status_line = json.loads(proc_space.stdout.readline().strip())
        print("Space test status output:", status_line)
        assert status_line == ["exited 1"], f"Expected ['exited 1'], got '{status_line}'"
        
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
