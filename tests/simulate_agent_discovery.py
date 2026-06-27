#!/usr/bin/env python3
import json
import subprocess
import sys

def send_msg(proc, msg):
    data = json.dumps(msg) + "\n"
    proc.stdin.write(data)
    proc.stdin.flush()

def read_msg(proc):
    line = proc.stdout.readline()
    if not line:
        return None
    return json.loads(line)

def main():
    # Start the termobulator MCP server from the build directory
    binary_path = "./build/termobulator"
    print(f"--- Starting MCP Server: {binary_path} ---")
    proc = subprocess.Popen(
        [binary_path, "--mcp"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True
    )
    
    try:
        # 1. Initialize Handshake
        print("\n[Step 1] Sending 'initialize' request...")
        init_req = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "fresh-agent-simulator", "version": "1.0"}
            }
        }
        send_msg(proc, init_req)
        init_resp = read_msg(proc)
        print("Initialization Response:")
        print(json.dumps(init_resp, indent=2)[:500] + "\n... [truncated] ...")
        
        # Send notifications/initialized
        print("\nSending 'notifications/initialized'...")
        send_msg(proc, {"jsonrpc": "2.0", "method": "notifications/initialized"})
        
        # 2. List Resources
        print("\n[Step 2] Sending 'resources/list' request...")
        list_req = {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "resources/list",
            "params": {}
        }
        send_msg(proc, list_req)
        list_resp = read_msg(proc)
        print("List Resources Response:")
        print(json.dumps(list_resp, indent=2))
        
        # 3. Read Catalog
        print("\n[Step 3] Reading recipes catalog ('termobulator://recipes/catalog')...")
        read_cat_req = {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "resources/read",
            "params": {"uri": "termobulator://recipes/catalog"}
        }
        send_msg(proc, read_cat_req)
        read_cat_resp = read_msg(proc)
        print("Read Catalog Response:")
        print(json.dumps(read_cat_resp, indent=2))
        
        # 4. Read First Contact Recipe
        print("\n[Step 4] Reading 'first_contact' recipe...")
        read_recipe_req = {
            "jsonrpc": "2.0",
            "id": 4,
            "method": "resources/read",
            "params": {"uri": "termobulator://recipes/incidental/first_contact"}
        }
        send_msg(proc, read_recipe_req)
        read_recipe_resp = read_msg(proc)
        print("Read Recipe Response:")
        print(json.dumps(read_recipe_resp, indent=2)[:800] + "\n... [truncated] ...")
        
    finally:
        proc.terminate()
        proc.wait()
        print("\n--- MCP Server stopped ---")

if __name__ == "__main__":
    main()
