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
        assert set(tool_names) == {
            "create_session",
            "close_session",
            "list_sessions",
            "set_active_session",
            "execute_dsl",
            "register_watcher",
            "check_watchers",
            "await_watchers"
        }, f"Unexpected tool list: {tool_names}"

        # Test error when calling execute_dsl without active session
        send_msg({
            "jsonrpc": "2.0",
            "id": 99,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "script": "return term.wait_idle(20, 500)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 99
        assert resp.get("result", {}).get("isError", False)
        err_msg = resp["result"]["content"][0]["text"]
        assert "No active session. Please create a session using create_session first." in err_msg, f"Expected active session error message, got {err_msg}"

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
                "name": "execute_dsl",
                "arguments": {
                    "script": "return term.wait_idle(20, 500)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 4
        assert not resp["result"].get("isError", False)
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        assert resp_payload["result"] == "idle", f"Expected 'idle', got {resp_payload['result']}"

        # 6. Call get_screen via DSL
        send_msg({
            "jsonrpc": "2.0",
            "id": 5,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "script": "return term.get_screen(-1)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 5
        assert not resp["result"].get("isError", False)
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        screen_str = resp_payload["result"]
        assert screen_str.startswith("READY"), f"Expected screen to start with READY, got: {screen_str[:20]}"

        # 7. Take a snapshot via DSL
        send_msg({
            "jsonrpc": "2.0",
            "id": 6,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "script": "return term.take_snapshot()"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 6
        assert not resp["result"].get("isError", False)
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        assert resp_payload["result"] == 0, f"Expected snapshot ID 0, got {resp_payload['result']}"

        # Test parameter validation: missing required parameter instructions in execute_dsl
        send_msg({
            "jsonrpc": "2.0",
            "id": 21,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {}
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 21
        assert resp["result"].get("isError") is True
        err_text = resp["result"]["content"][0]["text"]
        assert "Missing required parameter" in err_text or "script" in err_text, f"Expected validation error, got: {err_text}"

        # Test get_cursor via DSL (returns [cursor_x, cursor_y, visible])
        send_msg({
            "jsonrpc": "2.0",
            "id": 22,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "script": "return term.get_cursor(-1)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 22
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        cursor_res = resp_payload["result"]
        assert len(cursor_res) == 3
        assert isinstance(cursor_res[0], int)
        assert isinstance(cursor_res[1], int)
        assert isinstance(cursor_res[2], bool)

        # Test get_cell via DSL
        send_msg({
            "jsonrpc": "2.0",
            "id": 23,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "script": "return term.get_cell(0, 0, -1)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 23
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        cell_data = resp_payload["result"]
        assert "char" in cell_data
        assert "fg" in cell_data
        assert "bg" in cell_data

        # Test get_row via DSL
        send_msg({
            "jsonrpc": "2.0",
            "id": 24,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "script": "return term.get_row(0, -1)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 24
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        row_text = resp_payload["result"]
        assert row_text.startswith("READY"), f"Expected row 0 to start with READY, got: {row_text}"

        # Test find_text via DSL
        send_msg({
            "jsonrpc": "2.0",
            "id": 25,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "script": "return term.find_text('READY', -1)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 25
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        find_data = resp_payload["result"]
        assert len(find_data) > 0
        assert find_data[0]["row"] == 0
        assert find_data[0]["col_start"] == 0

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

        # 13. Query screen of session_2 via execute_dsl specifying session_id
        send_msg({
            "jsonrpc": "2.0",
            "id": 33,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "session_id": "session_2",
                    "script": "term.wait_idle(20, 500); return term.get_screen(-1)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 33
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        screen2 = resp_payload["result"]
        assert "hello_from_session_2" in screen2, f"Expected hello_from_session_2, got: {screen2}"

        # 14. Activate session_2 and run execute_dsl without session_id (should target session_2)
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
                "name": "execute_dsl",
                "arguments": {
                    "script": "return term.get_screen(-1)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 35
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        screen2_default = resp_payload["result"]
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
            "id": 52,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "session_id": "session_custom_env",
                    "script": "term.wait_idle(20, 500); return term.get_screen(-1)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 52
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        screen_custom = resp_payload["result"]
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

        # 8. Send key 'q' followed by a newline via DSL (now back to default session)
        send_msg({
            "jsonrpc": "2.0",
            "id": 7,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "script": "term.send_key('q\\n')"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["id"] == 7

        # 9. Wait for exited status via DSL
        send_msg({
            "jsonrpc": "2.0",
            "id": 8,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "script": "return term.wait_idle(100, 1000)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        res_val = resp_payload["result"]
        assert res_val == "exited", f"Expected 'exited', got {res_val}"

        # 10. Check status via DSL
        send_msg({
            "jsonrpc": "2.0",
            "id": 9,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "script": "return term.get_status()"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        res_val = resp_payload["result"]
        assert res_val == "exited 0", f"Expected 'exited 0', got {res_val}"

        # Create a session to test DSL operations
        send_msg({
            "jsonrpc": "2.0",
            "id": 300,
            "method": "tools/call",
            "params": {
                "name": "create_session",
                "arguments": {
                    "binary": "seq",
                    "arguments": ["1", "30"],
                    "session_id": "sb_test",
                    "scrollback_size": 200
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False)

        # Test execute_dsl tool
        # 1. Basic evaluation
        send_msg({
            "jsonrpc": "2.0",
            "id": 401,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "session_id": "sb_test",
                    "script": "return 123"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False)
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        assert resp_payload["result"] == 123, f"Expected 123, got {resp_payload['result']}"

        # 2. Variable Persistence: store a variable in one call, load in next
        send_msg({
            "jsonrpc": "2.0",
            "id": 402,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "session_id": "sb_test",
                    "script": "vars.var_mcp = 999"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False)
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        assert resp_payload["variables"] == {"var_mcp": 999}

        send_msg({
            "jsonrpc": "2.0",
            "id": 403,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "session_id": "sb_test",
                    "script": "return vars.var_mcp"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False)
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        assert resp_payload["result"] == 999, f"Expected 999, got {resp_payload['result']}"

        # Test structured return values (multiple returns)
        send_msg({
            "jsonrpc": "2.0",
            "id": 410,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "session_id": "sb_test",
                    "script": "return 'hello_structured', ''"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False)
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        assert resp_payload["result"] == ["hello_structured", ""], f"Expected ['hello_structured', ''], got {resp_payload['result']}"

        # Test another structured array check
        send_msg({
            "jsonrpc": "2.0",
            "id": 411,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "session_id": "sb_test",
                    "script": "return {88, 88}"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False)
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        assert resp_payload["result"] == [88, 88], f"Expected [88, 88], got {resp_payload['result']}"

        # Test error diagnostics (syntax error)
        send_msg({
            "jsonrpc": "2.0",
            "id": 412,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "session_id": "sb_test",
                    "script": "1 +"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["result"].get("isError") is True
        err_msg = resp["result"]["content"][0]["text"]
        assert "syntax error" in err_msg or "near" in err_msg or "unexpected" in err_msg

        # Verify empty script doesn't error
        send_msg({
            "jsonrpc": "2.0",
            "id": 413,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "session_id": "sb_test",
                    "script": ""
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False)
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        assert resp_payload["result"] is None

        # Test sleep_ms
        import time
        start_time = time.time()
        send_msg({
            "jsonrpc": "2.0",
            "id": 414,
            "method": "tools/call",
            "params": {
                "name": "execute_dsl",
                "arguments": {
                    "session_id": "sb_test",
                    "script": "term.sleep_ms(150)"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False)
        duration = time.time() - start_time
        assert duration >= 0.14, f"Expected sleep to take at least 150ms, took {duration}s"

        # Test persistent watchers
        # 1. Register a text watcher
        send_msg({
            "jsonrpc": "2.0",
            "id": 450,
            "method": "tools/call",
            "params": {
                "name": "register_watcher",
                "arguments": {
                    "session_id": "sb_test",
                    "watcher_id": "seq_watch",
                    "condition": {
                        "type": "pattern",
                        "pattern": "%d+"
                    }
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False)

        # 2. Check watchers (non-blocking poll)
        # Since 'seq' outputs integers, it should fire immediately
        time.sleep(0.5)
        send_msg({
            "jsonrpc": "2.0",
            "id": 451,
            "method": "tools/call",
            "params": {
                "name": "check_watchers",
                "arguments": {
                    "session_id": "sb_test"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False)
        events = json.loads(resp["result"]["content"][0]["text"])
        assert len(events) > 0
        assert events[0]["watcher_id"] == "seq_watch"
        assert events[0]["matched_text"].isdigit()

        # 3. await_watchers (blocking wait)
        send_msg({
            "jsonrpc": "2.0",
            "id": 452,
            "method": "tools/call",
            "params": {
                "name": "register_watcher",
                "arguments": {
                    "session_id": "sb_test",
                    "watcher_id": "timeout_watch",
                    "condition": {
                        "type": "text",
                        "pattern": "NEVER_EXISTS"
                    }
                }
            }
        })
        resp = read_msg()
        assert resp is not None

        send_msg({
            "jsonrpc": "2.0",
            "id": 453,
            "method": "tools/call",
            "params": {
                "name": "await_watchers",
                "arguments": {
                    "session_id": "sb_test",
                    "timeout_ms": 100,
                    "watcher_ids": ["timeout_watch"]
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        events = json.loads(resp["result"]["content"][0]["text"])
        assert len(events) == 0, f"Expected 0 events on timeout, got {events}"

        # Terminate termobulator
        proc.stdin.close()
        proc.wait(timeout=2.0)
        print("MCP integration test passed successfully!")

    except Exception as e:
        proc.kill()
        traceback.print_exc()
        print("MCP test failed:", e)
        sys.exit(1)

def run_idle_timeout_test():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    
    if os.path.exists("./termobulator"):
        binary_path = "./termobulator"
    elif os.path.exists(os.path.join(project_root, "build", "termobulator")):
        binary_path = os.path.join(project_root, "build", "termobulator")
    else:
        binary_path = "./build/termobulator"
        
    target_path = os.path.join(script_dir, "target.sh")

    print("Running MCP idle timeout test...")
    proc = subprocess.Popen(
        [binary_path, "--mcp", "--idle-timeout-sec", "2"],
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
        # Initialize
        send_msg({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "TestClient", "version": "1.1.0"}
            }
        })
        read_msg()
        send_msg({
            "jsonrpc": "2.0",
            "method": "notifications/initialized"
        })

        # Create session
        send_msg({
            "jsonrpc": "2.0",
            "id": 2,
            "method": "tools/call",
            "params": {
                "name": "create_session",
                "arguments": {
                    "binary": target_path,
                    "session_id": "test_timeout"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False)

        # Confirm session is listed
        send_msg({
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {
                "name": "list_sessions",
                "arguments": {}
            }
        })
        resp = read_msg()
        assert resp is not None
        sess_list = json.loads(resp["result"]["content"][0]["text"])
        assert len(sess_list) == 1
        assert sess_list[0]["session_id"] == "test_timeout"

        # Wait 3 seconds to exceed the 2 second idle timeout
        time.sleep(3.0)

        # Try listing sessions - it should be gone (empty list)
        send_msg({
            "jsonrpc": "2.0",
            "id": 4,
            "method": "tools/call",
            "params": {
                "name": "list_sessions",
                "arguments": {}
            }
        })
        resp = read_msg()
        assert resp is not None
        sess_list = json.loads(resp["result"]["content"][0]["text"])
        assert len(sess_list) == 0, f"Expected session to be cleaned up, but got list: {sess_list}"

        # Clean terminate
        proc.stdin.close()
        proc.wait(timeout=2.0)
        print("MCP idle timeout test passed successfully!")

    except Exception as e:
        proc.kill()
        traceback.print_exc()
        print("MCP idle timeout test failed:", e)
        sys.exit(1)

def run_logging_test():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    
    if os.path.exists("./termobulator"):
        binary_path = "./termobulator"
    elif os.path.exists(os.path.join(project_root, "build", "termobulator")):
        binary_path = os.path.join(project_root, "build", "termobulator")
    else:
        binary_path = "./build/termobulator"
        
    print("Running MCP logging test...")
    proc = subprocess.Popen(
        [binary_path, "--mcp", "--do_log"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True
    )
    
    pid = proc.pid
    log_path = f"/tmp/termobulator-{pid}.log"
    
    if os.path.exists(log_path):
        os.remove(log_path)
        
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
        init_req = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "TestClient", "version": "1.1.0"}
            }
        }
        send_msg(init_req)
        init_resp = read_msg()
        assert init_resp is not None
        
        init_notif = {
            "jsonrpc": "2.0",
            "method": "notifications/initialized"
        }
        send_msg(init_notif)
        
        proc.stdin.close()
        proc.wait(timeout=2.0)
        
        assert os.path.exists(log_path), f"Log file {log_path} was not created"
        
        with open(log_path, "r") as f:
            log_lines = f.read().splitlines()
            
        print("Log lines captured:")
        for line in log_lines:
            print(f"  {line}")
            
        assert len(log_lines) >= 3, f"Expected at least 3 log lines, got {len(log_lines)}"
        
        assert log_lines[0].startswith("[RECV] "), f"Expected line 0 to start with '[RECV] ', got '{log_lines[0]}'"
        log_init_req = json.loads(log_lines[0][7:])
        assert log_init_req["method"] == "initialize"
        
        assert log_lines[1].startswith("[SEND] "), f"Expected line 1 to start with '[SEND] ', got '{log_lines[1]}'"
        log_init_resp = json.loads(log_lines[1][7:])
        assert log_init_resp["id"] == 1
        assert "result" in log_init_resp
        
        assert log_lines[2].startswith("[RECV] "), f"Expected line 2 to start with '[RECV] ', got '{log_lines[2]}'"
        log_init_notif = json.loads(log_lines[2][7:])
        assert log_init_notif["method"] == "notifications/initialized"
        
        os.remove(log_path)
        print("MCP logging test passed successfully!")
        
    except Exception as e:
        proc.kill()
        if os.path.exists(log_path):
            try:
                os.remove(log_path)
            except:
                pass
        traceback.print_exc()
        print("MCP logging test failed:", e)
        sys.exit(1)

def run_workspace_limits_test():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    
    if os.path.exists("./termobulator"):
        binary_path = "./termobulator"
    elif os.path.exists(os.path.join(project_root, "build", "termobulator")):
        binary_path = os.path.join(project_root, "build", "termobulator")
    else:
        binary_path = "./build/termobulator"
        
    target_path = os.path.join(script_dir, "target.sh")
    
    print("Running MCP workspace limits test...")
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
        while True:
            line = proc.stdout.readline()
            if not line:
                return None
            msg = json.loads(line)
            if msg.get("method") == "roots/list":
                send_msg({
                    "jsonrpc": "2.0",
                    "id": msg["id"],
                    "result": {"roots": [
                        {
                            "uri": f"file://{project_root}",
                            "name": "Project Root"
                        }
                    ]}
                })
                continue
            return msg
        
    try:
        # Handshake: initialize with roots (only project_root)
        send_msg({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {
                    "roots": {
                        "listChanged": True
                    }
                },
                "clientInfo": {"name": "TestClientWithWorkspace", "version": "1.1.0"},
                "roots": [
                    {
                        "uri": f"file://{project_root}",
                        "name": "Project Root"
                    }
                ]
            }
        })
        resp = read_msg()
        assert resp is not None
        
        send_msg({
            "jsonrpc": "2.0",
            "method": "notifications/initialized"
        })
        
        # Test 1: Spawning a target inside the workspace (target_path) should succeed
        send_msg({
            "jsonrpc": "2.0",
            "id": 2,
            "method": "tools/call",
            "params": {
                "name": "create_session",
                "arguments": {
                    "binary": target_path,
                    "session_id": "inside_sess"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False), f"Expected inside workspace spawn to succeed, but got error: {resp}"
        
        # Test 2: Spawning a target outside the workspace (/bin/sh) should fail
        send_msg({
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {
                "name": "create_session",
                "arguments": {
                    "binary": "/bin/sh",
                    "session_id": "outside_sess"
                }
            }
        })
        resp = read_msg()
        assert resp is not None
        assert resp["result"].get("isError", False), "Expected outside workspace spawn to fail, but it succeeded!"
        err_msg = resp["result"]["content"][0]["text"]
        assert "lies outside the workspace" in err_msg, f"Expected workspace access denied error, got: {err_msg}"
        
        proc.stdin.close()
        proc.wait(timeout=2.0)
        print("MCP workspace limits test passed successfully!")
        
    except Exception as e:
        proc.kill()
        traceback.print_exc()
        print("MCP workspace limits test failed:", e)
        sys.exit(1)

    print("Running MCP empty roots (CWD fallback) test...")
    proc2 = subprocess.Popen(
        [binary_path, "--mcp"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True
    )
    
    def send_msg2(msg):
        line = json.dumps(msg)
        proc2.stdin.write(line + "\n")
        proc2.stdin.flush()
        
    def read_msg2():
        while True:
            line = proc2.stdout.readline()
            if not line:
                return None
            msg = json.loads(line)
            if msg.get("method") == "roots/list":
                send_msg2({
                    "jsonrpc": "2.0",
                    "id": msg["id"],
                    "result": {"roots": []}
                })
                continue
            return msg
        
    try:
        # Handshake: initialize with roots capability, but empty roots array
        send_msg2({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {
                    "roots": {
                        "listChanged": True
                    }
                },
                "clientInfo": {"name": "TestClientWithEmptyWorkspace", "version": "1.1.0"},
                "roots": []
            }
        })
        resp = read_msg2()
        assert resp is not None
        
        send_msg2({
            "jsonrpc": "2.0",
            "method": "notifications/initialized"
        })
        
        # Spawning ./test_terminal (inside CWD) should succeed
        send_msg2({
            "jsonrpc": "2.0",
            "id": 2,
            "method": "tools/call",
            "params": {
                "name": "create_session",
                "arguments": {
                    "binary": "./test_terminal",
                    "session_id": "inside_sess"
                }
            }
        })
        resp = read_msg2()
        assert resp is not None
        assert not resp["result"].get("isError", False), f"Expected inside CWD spawn to succeed, but got error: {resp}"
        
        # Spawning /bin/sh (outside CWD) should fail
        send_msg2({
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {
                "name": "create_session",
                "arguments": {
                    "binary": "/bin/sh",
                    "session_id": "outside_sess"
                }
            }
        })
        resp = read_msg2()
        assert resp is not None
        assert resp["result"].get("isError", False), "Expected outside CWD spawn to fail, but it succeeded!"
        err_msg = resp["result"]["content"][0]["text"]
        assert "lies outside the workspace" in err_msg, f"Expected workspace access denied error, got: {err_msg}"
        
        proc2.stdin.close()
        proc2.wait(timeout=2.0)
        print("MCP empty roots test passed successfully!")
        
    except Exception as e:
        proc2.kill()
        traceback.print_exc()
        print("MCP empty roots test failed:", e)
        sys.exit(1)

def run_terminal_levels_test():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)

    if os.path.exists("./termobulator"):
        binary_path = "./termobulator"
    elif os.path.exists(os.path.join(project_root, "build", "termobulator")):
        binary_path = os.path.join(project_root, "build", "termobulator")
    else:
        binary_path = "./build/termobulator"

    print("Running terminal levels test...")
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

    def handshake():
        send_msg({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                  "params": {"protocolVersion": "2024-11-05",
                             "capabilities": {},
                             "clientInfo": {"name": "TestClient", "version": "1.0"}}})
        read_msg()
        send_msg({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def create_and_check_term(msg_id, terminal_arg, expected_terminfo):
        """Create a session with the given terminal arg, verify response field and TERM env."""
        args = {"binary": "/bin/sh",
                "arguments": ["-c", "echo TERM=$TERM"],
                "session_id": f"sess_{msg_id}"}
        if terminal_arg is not None:
            args["terminal"] = terminal_arg
        send_msg({"jsonrpc": "2.0", "id": msg_id, "method": "tools/call",
                  "params": {"name": "create_session", "arguments": args}})
        resp = read_msg()
        assert resp is not None
        assert not resp["result"].get("isError", False), \
            f"create_session failed: {resp}"
        create_data = json.loads(resp["result"]["content"][0]["text"])
        assert create_data["terminal"] == expected_terminfo, \
            f"Expected terminal={expected_terminfo!r}, got {create_data['terminal']!r}"

        # Read TERM from the child's environment
        send_msg({"jsonrpc": "2.0", "id": msg_id + 1000, "method": "tools/call",
                  "params": {"name": "execute_dsl",
                             "arguments": {"session_id": f"sess_{msg_id}",
                                           "script": "term.wait_idle(20, 500); return term.get_screen(-1)"}}})
        resp2 = read_msg()
        assert resp2 is not None
        resp_payload = json.loads(resp2["result"]["content"][0]["text"])
        screen = resp_payload["result"]
        assert f"TERM={expected_terminfo}" in screen, \
            f"Expected TERM={expected_terminfo} in screen, got: {screen!r}"

        # Clean up
        send_msg({"jsonrpc": "2.0", "id": msg_id + 2000, "method": "tools/call",
                  "params": {"name": "close_session",
                             "arguments": {"session_id": f"sess_{msg_id}"}}})
        read_msg()
        return create_data

    try:
        handshake()

        # 1. Symbolic name resolution: "basic" -> vt100
        create_and_check_term(10, "basic", "vt100")
        print("  [1] Symbolic name resolution: OK")

        # 2. Compound name: "extended-8color" -> termobulator-extended-8color
        create_and_check_term(20, "extended-8color",
                              "termobulator-extended-8color")
        print("  [2] Compound name: OK")


        # 3. Case insensitivity: "FULL-MONO" -> termobulator-full-mono
        create_and_check_term(30, "FULL-MONO", "termobulator-full-mono")
        print("  [3] Case insensitivity: OK")

        # 4. Alias: "extended-mono" -> vt220 (no warning)
        data = create_and_check_term(40, "extended-mono", "vt220")
        assert "warning" not in data, \
            f"Expected no warning for extended-mono, got: {data.get('warning')}"
        print("  [4] extended-mono alias (no warning): OK")

        # 5. Unknown warning: "my-custom-term" -> passthrough + warning
        send_msg({"jsonrpc": "2.0", "id": 50, "method": "tools/call",
                  "params": {"name": "create_session",
                             "arguments": {"binary": "/bin/sh",
                                           "arguments": ["-c", "echo hi"],
                                           "session_id": "sess_unknown",
                                           "terminal": "my-custom-term"}}})
        resp = read_msg()
        assert not resp["result"].get("isError", False)
        data = json.loads(resp["result"]["content"][0]["text"])
        assert data["terminal"] == "my-custom-term"
        assert "warning" in data, "Expected warning for unknown terminal type"
        assert "my-custom-term" in data["warning"]
        send_msg({"jsonrpc": "2.0", "id": 51, "method": "tools/call",
                  "params": {"name": "close_session",
                             "arguments": {"session_id": "sess_unknown"}}})
        read_msg()
        print("  [5] Unknown terminal warning: OK")

        # 6. resources/list
        send_msg({"jsonrpc": "2.0", "id": 60, "method": "resources/list",
                  "params": {}})
        resp = read_msg()
        assert resp is not None
        assert "result" in resp, f"resources/list error: {resp}"
        resources = resp["result"]["resources"]
        uris = [r["uri"] for r in resources]
        assert "termobulator://docs/terminal-levels" in uris, \
            f"Expected terminal-levels resource, got: {uris}"
        print("  [6] resources/list: OK")

        # 7. resources/read
        send_msg({"jsonrpc": "2.0", "id": 70, "method": "resources/read",
                  "params": {"uri": "termobulator://docs/terminal-levels"}})
        resp = read_msg()
        assert resp is not None
        assert "result" in resp, f"resources/read error: {resp}"
        contents = resp["result"]["contents"]
        assert len(contents) > 0
        text = contents[0]["text"]
        for level in ["minimal", "basic", "extended", "full"]:
            assert level in text, f"Expected level {level!r} in docs text"
        print("  [7] resources/read: OK")

        # 8. Default changed: no terminal param -> xterm-256color
        send_msg({"jsonrpc": "2.0", "id": 80, "method": "tools/call",
                  "params": {"name": "create_session",
                             "arguments": {"binary": "/bin/sh",
                                           "arguments": ["-c", "echo TERM=$TERM"],
                                           "session_id": "sess_default"}}})
        resp = read_msg()
        assert not resp["result"].get("isError", False)
        data = json.loads(resp["result"]["content"][0]["text"])
        assert data["terminal"] == "xterm-256color", \
            f"Expected default terminal=xterm-256color, got {data['terminal']!r}"
        send_msg({"jsonrpc": "2.0", "id": 81, "method": "tools/call",
                  "params": {"name": "execute_dsl",
                             "arguments": {"session_id": "sess_default",
                                           "script": "term.wait_idle(20, 500); return term.get_screen(-1)"}}})
        resp = read_msg()
        resp_payload = json.loads(resp["result"]["content"][0]["text"])
        screen = resp_payload["result"]
        assert "TERM=xterm-256color" in screen, \
            f"Expected TERM=xterm-256color, got: {screen!r}"
        send_msg({"jsonrpc": "2.0", "id": 82, "method": "tools/call",
                  "params": {"name": "close_session",
                             "arguments": {"session_id": "sess_default"}}})
        read_msg()
        print("  [8] Default changed to xterm-256color: OK")

        proc.stdin.close()
        proc.wait(timeout=2.0)
        print("Terminal levels test passed successfully!")

    except Exception as e:
        proc.kill()
        traceback.print_exc()
        print("Terminal levels test failed:", e)
        sys.exit(1)

if __name__ == "__main__":
    run_mcp_test()
    run_idle_timeout_test()
    run_logging_test()
    run_workspace_limits_test()
    run_terminal_levels_test()