#!/usr/bin/env python3
"""
============================================================================
 IoT Bhai - XIAO ESP32-C5 throughput test-file server
----------------------------------------------------------------------------
 Creates a 20 MB test file (once) and serves THIS folder over HTTP so the
 C5 can download it and measure 2.4 GHz vs 5 GHz throughput.

 Run (from a terminal on this PC):
     python serve_testfile.py

 The node code must use:
     http://<this-PC-LAN-IP>:8000/testfile.bin
 (the script prints the exact URL when it starts)

 Stop with Ctrl+C.

 NOTE: the PC and the ESP32-C5 must be on the SAME router/subnet.
       This PC is on 192.168.0.x, so the C5 must also get a 192.168.0.x IP.
============================================================================
"""
import http.server
import socketserver
import os
import socket

PORT    = 8000
SIZE_MB = 30
FILE    = "testfile.bin"


def ensure_testfile():
    """Create the fixed-size test file once; reuse it on later runs."""
    size = SIZE_MB * 1024 * 1024
    if os.path.exists(FILE) and os.path.getsize(FILE) == size:
        print(f"[ok] {FILE} already exists ({SIZE_MB} MB)")
        return
    print(f"[..] creating {FILE} ({SIZE_MB} MB) ...")
    with open(FILE, "wb") as f:
        f.truncate(size)          # zero-filled 20 MB file
    print("[ok] test file ready")


def lan_ip():
    """Best-effort primary LAN IPv4 of this PC."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))   # no traffic sent; just picks the route
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    ensure_testfile()

    ip = lan_ip()
    handler = http.server.SimpleHTTPRequestHandler
    # 0.0.0.0 = listen on every interface so the C5 on Wi-Fi can reach it
    with socketserver.TCPServer(("0.0.0.0", PORT), handler) as httpd:
        print("\n" + "=" * 52)
        print(f"  Serving: http://{ip}:{PORT}/{FILE}")
        print("  Put THAT url in THROUGHPUT_URL in the node code.")
        print("  Ctrl+C to stop.")
        print("=" * 52 + "\n")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[bye] server stopped")
