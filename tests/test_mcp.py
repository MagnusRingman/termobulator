#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The Termobulator Authors.

import subprocess
import json
import time
import sys
import os
import traceback

def run_mcp_test():
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
        print(f"Error: Executable {binary_path} not found.")
        sys.exit(1)
        
    os.chmod(target_path, 0o755)

    print("Running MCP integration test...")
    proc = subprocess.Popen(
        [binary_path, "--mcp"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True
    )

    def send_msg(msg):
        line = json.dumps(msg)
        proc.stdin.write(line + "\n")
        proc.stdin.flush()

    def read_msg():
        line = proc.stdout.readline()
        if not line:
            return None
        return json.loads(line)

    try:
        # 1. Try to list tools before initialization (should error)
        send_msg({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/list",
            "params": {}
        })
        resp = read_msg()
        assert resp is not None, "No response received"
        assert "error" in resp, "Expected error before initialization"
        assert resp["error"]["code"] == -32002, f"Expected error code -32002, got {resp['error']['code']}"

        # 2. Handshake: initialize
        send_msg({
            "jsonrpc": "2.0",
            "id": 2,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "TestClient", "version": "1.1.0"}
            }
        })
        resp = read_msg()
        assert resp is not None, "No initialize response received"
        assert resp["id"] == 2
        assert "result" in resp
        assert resp["result"]["protocolVersion"] == "2024-11-05"
        assert "tools" in resp["result"]["capabilities"]

        # Test protocol conformance: tools/list after initialize but before notifications/initialized must fail
        send_msg({
            "jsonrpc": "2.0",
            "id": 20,
            "method": "tools/list",
            "params": {}
        })
        resp = read_msg()
        assert resp is not None
        assert "error" in resp, "Expected error before notifications/initialized"
        assert resp["error"]["code"] == -32002, f"Expected error code -32002, got {resp['error']['code']}"

        # 3. Handshake: initialized
        send_msg({
            "jsonrpc": "2.0",
            "method": "notifications/initialized"
        })

        # 4. List tools
        send_msg({
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/list",
            "params": {}
        })
        resp = read_msg()
        assert resp is not None, "No tools/list response received"
        assert resp["id"] == 3
        assert "result" in resp
        tools = resp["result"]["tools"]
        tool_names = [t["name"] for t in tools]
        print("Exposed tools:", tool_names)
        assert "get_screen" in tool_names
        assert "send_key" in tool_names
        assert "wait_idle" in tool_names
        assert "find_text" in tool_names
        assert "get_row" in tool_names
        assert "create_session" in tool_names
        assert "close_session" in tool_names
        assert "list_sessions" in tool_names
        assert "set_active_session" in tool_names

        # 4b. Create default session running target_path
        send_msg({
            "jsonrpc": "2.0",
            "id": 100,
            "method": "tools/call",
            "params": {
                "name": "create_session",
                "arguments": {
                    "binary": target_path,
                    "session_id": "default"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 100
        assert not resp["result"].get("isError", False)
        create_res = json.loads(resp["result"]["content"][0]["text"])
        assert create_res["session_id"] == "default"

        # 5. Call wait_idle to wait for target.sh to become idle
        send_msg({
            "jsonrpc": "2.0",
            "id": 4,
            "method": "tools/call",
            "params": {
                "name": "wait_idle",
                "arguments": {
                    "quiet_ms": 20,
                    "deadline_ms": 500
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 4
        assert not resp["result"].get("isError", False)
        text_content = resp["result"]["content"][0]["text"]
        assert text_content == "wait: idle", f"Expected 'wait: idle', got {text_content}"

        # 6. Call get_screen
        send_msg({
            "jsonrpc": "2.0",
            "id": 5,
            "method": "tools/call",
            "params": {
                "name": "get_screen",
                "arguments": {}
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 5
        text_content = resp["result"]["content"][0]["text"]
        assert text_content.startswith("READY"), f"Expected screen to start with READY, got: {text_content[:20]}"

        # 7. Take a snapshot
        send_msg({
            "jsonrpc": "2.0",
            "id": 6,
            "method": "tools/call",
            "params": {
                "name": "take_snapshot",
                "arguments": {}
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 6
        text_content = resp["result"]["content"][0]["text"]
        snap_data = json.loads(text_content)
        assert snap_data["snapshot_id"] == 0, f"Expected snapshot_id to be 0, got {snap_data}"

        # Test parameter validation: missing required parameter keyname in send_special_key
        send_msg({
            "jsonrpc": "2.0",
            "id": 21,
            "method": "tools/call",
            "params": {
                "name": "send_special_key",
                "arguments": {}
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 21
        assert resp["result"].get("isError") is True
        err_text = resp["result"]["content"][0]["text"]
        assert "Missing required parameter" in err_text, f"Expected validation error, got: {err_text}"

        # Test get_cursor returns JSON
        send_msg({
            "jsonrpc": "2.0",
            "id": 22,
            "method": "tools/call",
            "params": {
                "name": "get_cursor",
                "arguments": {}
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 22
        cursor_data = json.loads(resp["result"]["content"][0]["text"])
        assert "col" in cursor_data
        assert "row" in cursor_data
        assert "visible" in cursor_data

        # Test get_cell returns JSON
        send_msg({
            "jsonrpc": "2.0",
            "id": 23,
            "method": "tools/call",
            "params": {
                "name": "get_cell",
                "arguments": {"x": 0, "y": 0}
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 23
        cell_data = json.loads(resp["result"]["content"][0]["text"])
        assert "char" in cell_data
        assert "fg" in cell_data
        assert "bg" in cell_data

        # Test get_row
        send_msg({
            "jsonrpc": "2.0",
            "id": 24,
            "method": "tools/call",
            "params": {
                "name": "get_row",
                "arguments": {"row": 0}
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 24
        row_text = resp["result"]["content"][0]["text"]
        assert row_text.startswith("READY"), f"Expected row 0 to start with READY, got: {row_text}"

        # Test find_text
        send_msg({
            "jsonrpc": "2.0",
            "id": 25,
            "method": "tools/call",
            "params": {
                "name": "find_text",
                "arguments": {"text": "READY"}
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 25
        find_data = json.loads(resp["result"]["content"][0]["text"])
        assert len(find_data) > 0
        assert find_data[0]["row"] == 0
        assert find_data[0]["col_start"] == 0

        # Test get_attributes without snapshot_id but with attribute_id (should fail)
        send_msg({
            "jsonrpc": "2.0",
            "id": 26,
            "method": "tools/call",
            "params": {
                "name": "get_attributes",
                "arguments": {"attribute_id": 0}
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 26
        assert resp["result"].get("isError") is True
        assert "snapshot_id is required" in resp["result"]["content"][0]["text"]

        # Test JSON-RPC id field type validation
        send_msg({
            "jsonrpc": "2.0",
            "id": [1, 2],
            "method": "tools/list",
            "params": {}
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] is None
        assert "error" in resp
        assert "id must be string, number, or null" in resp["error"]["message"]

        # Test multi-session capabilities:
        # 11. Create a second session
        send_msg({
            "jsonrpc": "2.0",
            "id": 30,
            "method": "tools/call",
            "params": {
                "name": "create_session",
                "arguments": {
                    "binary": "/bin/sh",
                    "arguments": ["-c", "echo hello_from_session_2"],
                    "session_id": "session_2"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 30
        assert not resp["result"].get("isError", False)
        sess2_data = json.loads(resp["result"]["content"][0]["text"])
        assert sess2_data["session_id"] == "session_2"

        # 12. List sessions
        send_msg({
            "jsonrpc": "2.0",
            "id": 31,
            "method": "tools/call",
            "params": {
                "name": "list_sessions",
                "arguments": {}
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 31
        sess_list = json.loads(resp["result"]["content"][0]["text"])
        sess2_item = next((s for s in sess_list if s["session_id"] == "session_2"), None)
        assert sess2_item is not None
        default_item = next((s for s in sess_list if s["session_id"] == "default"), None)
        assert default_item is not None

        # 13. Query screen of session_2
        send_msg({
            "jsonrpc": "2.0",
            "id": 32,
            "method": "tools/call",
            "params": {
                "name": "wait_idle",
                "arguments": {
                    "session_id": "session_2",
                    "quiet_ms": 20,
                    "deadline_ms": 500
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 32

        send_msg({
            "jsonrpc": "2.0",
            "id": 33,
            "method": "tools/call",
            "params": {
                "name": "get_screen",
                "arguments": {
                    "session_id": "session_2"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 33
        screen2 = resp["result"]["content"][0]["text"]
        assert "hello_from_session_2" in screen2, f"Expected hello_from_session_2, got: {screen2}"

        # 14. Activate session_2 and run get_screen without session_id (should target session_2)
        send_msg({
            "jsonrpc": "2.0",
            "id": 34,
            "method": "tools/call",
            "params": {
                "name": "set_active_session",
                "arguments": {
                    "session_id": "session_2"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 34
        assert not resp["result"].get("isError", False)

        send_msg({
            "jsonrpc": "2.0",
            "id": 35,
            "method": "tools/call",
            "params": {
                "name": "get_screen",
                "arguments": {}
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 35
        screen2_default = resp["result"]["content"][0]["text"]
        assert "hello_from_session_2" in screen2_default

        # 15. Close session_2
        send_msg({
            "jsonrpc": "2.0",
            "id": 36,
            "method": "tools/call",
            "params": {
                "name": "close_session",
                "arguments": {
                    "session_id": "session_2"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 36
        assert not resp["result"].get("isError", False)

        # 16. Verify session_2 is gone from list_sessions
        send_msg({
            "jsonrpc": "2.0",
            "id": 37,
            "method": "tools/call",
            "params": {
                "name": "list_sessions",
                "arguments": {}
            }
        })
        resp = read_msg()
        assert resp is not None
        sess_list_post = json.loads(resp["result"]["content"][0]["text"])
        sess2_item_post = next((s for s in sess_list_post if s["session_id"] == "session_2"), None)
        assert sess2_item_post is None, "Expected session_2 to be deleted"

        # Test create_session with custom terminal and locale
        send_msg({
            "jsonrpc": "2.0",
            "id": 50,
            "method": "tools/call",
            "params": {
                "name": "create_session",
                "arguments": {
                    "binary": "/bin/sh",
                    "arguments": ["-c", "echo TERM=$TERM LANG=$LANG"],
                    "session_id": "session_custom_env",
                    "terminal": "custom-terminal-123",
                    "locale": "en_GB.UTF-8"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 50
        assert not resp["result"].get("isError", False)

        send_msg({
            "jsonrpc": "2.0",
            "id": 51,
            "method": "tools/call",
            "params": {
                "name": "wait_idle",
                "arguments": {
                    "session_id": "session_custom_env",
                    "quiet_ms": 20,
                    "deadline_ms": 500
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 51

        send_msg({
            "jsonrpc": "2.0",
            "id": 52,
            "method": "tools/call",
            "params": {
                "name": "get_screen",
                "arguments": {
                    "session_id": "session_custom_env"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 52
        screen_custom = resp["result"]["content"][0]["text"]
        assert "TERM=custom-terminal-123" in screen_custom, f"Expected custom terminal env, got: {screen_custom}"
        assert "LANG=en_GB.UTF-8" in screen_custom, f"Expected custom locale env, got: {screen_custom}"

        send_msg({
            "jsonrpc": "2.0",
            "id": 53,
            "method": "tools/call",
            "params": {
                "name": "close_session",
                "arguments": {
                    "session_id": "session_custom_env"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 53
        assert not resp["result"].get("isError", False)

        # Switch active session back to default
        send_msg({
            "jsonrpc": "2.0",
            "id": 38,
            "method": "tools/call",
            "params": {
                "name": "set_active_session",
                "arguments": {
                    "session_id": "default"
                }
            }
        })
        resp = read_msg()
        assert resp is not None

        # 8. Send key 'q' followed by a newline (now back to default session)
        send_msg({
            "jsonrpc": "2.0",
            "id": 7,
            "method": "tools/call",
            "params": {
                "name": "send_key",
                "arguments": {
                    "keys": "q\\n"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 7

        # 9. Wait for exited status
        send_msg({
            "jsonrpc": "2.0",
            "id": 8,
            "method": "tools/call",
            "params": {
                "name": "wait_idle",
                "arguments": {
                    "quiet_ms": 100,
                    "deadline_ms": 1000
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        text_content = resp["result"]["content"][0]["text"]
        assert text_content == "wait: exited", f"Expected 'wait: exited', got {text_content}"

        # 10. Check status
        send_msg({
            "jsonrpc": "2.0",
            "id": 9,
            "method": "tools/call",
            "params": {
                "name": "get_status",
                "arguments": {}
            }
        })
        resp = read_msg()
        assert resp is not None
        text_content = resp["result"]["content"][0]["text"]
        assert text_content == "exited 0", f"Expected 'exited 0', got {text_content}"

        # Terminate termobulator
        proc.stdin.close()
        proc.wait(timeout=2.0)
        print("MCP integration test passed successfully!")

    except Exception as e:
        proc.kill()
        traceback.print_exc()
        print("MCP test failed:", e)
        sys.exit(1)

if __name__ == "__main__":
    run_mcp_test()
