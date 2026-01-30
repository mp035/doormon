#!/usr/bin/env python3
"""
Doormon monitor – discover device via mDNS, poll /status every second,
report trigger events and slow responses (>3s), allow reset via 'r'.

Uses mDNS directly so discovery is fast (no 5s system DNS timeout).
All HTTP requests use the resolved IP, so polling stays fast.

Usage:
  pip install -r scripts/requirements.txt   # once
  python scripts/doormon_monitor.py
"""

import json
import socket
import sys
import threading
import time
import urllib.error
import urllib.request

try:
    from zeroconf import ServiceBrowser, ServiceInfo, Zeroconf
except ImportError:
    print("Install zeroconf: pip install -r scripts/requirements.txt", file=sys.stderr)
    sys.exit(1)

# Match firmware: _http._tcp, instance "Doormon"
SERVICE_TYPE = "_http._tcp.local."
INSTANCE_NAME = "Doormon"
POLL_INTERVAL = 1.0
SLOW_RESPONSE_THRESHOLD = 3.0
DISCOVERY_TIMEOUT = 15.0


def discover_device():
    """Resolve Doormon via mDNS; return (host, port) or None."""
    result = {"info": None}

    class Listener:
        def add_service(self, zc, type_, name):
            if result["info"] is not None:
                return
            info = zc.get_service_info(type_, name)
            if info is None:
                return
            # Prefer instance name match (e.g. "Doormon._http._tcp.local.")
            if INSTANCE_NAME.lower() not in name.lower():
                return
            # Get IPv4 address
            if getattr(info, "addresses", None):
                addrs = info.addresses
            else:
                addrs = [getattr(info, "address", None)] if hasattr(info, "address") else []
            for addr in addrs:
                if addr is not None and len(addr) == 4:
                    result["info"] = (socket.inet_ntoa(addr), info.port or 80)
                    return

        def remove_service(self, zc, type_, name):
            pass

    zc = Zeroconf()
    listener = Listener()
    browser = ServiceBrowser(zc, SERVICE_TYPE, listener)
    deadline = time.monotonic() + DISCOVERY_TIMEOUT
    while result["info"] is None and time.monotonic() < deadline:
        time.sleep(0.2)
    browser.cancel()
    zc.close()
    return result["info"]


def get_status(host, port, timeout=4.0):
    """GET /status; return (triggered, response_time_sec) or (None, time) on error."""
    url = f"http://{host}:{port}/status"
    req = urllib.request.Request(url)
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = resp.read().decode()
            elapsed = time.monotonic() - t0
            obj = json.loads(data)
            return (obj.get("triggered", False), elapsed)
    except Exception:
        elapsed = time.monotonic() - t0
        return (None, elapsed)


def post_reset(host, port, timeout=4.0):
    """POST /reset; return True on success."""
    url = f"http://{host}:{port}/reset"
    req = urllib.request.Request(url, method="POST", data=b"")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.getcode() == 200
    except Exception:
        return False


def main():
    print("Discovering Doormon via mDNS (_http._tcp)...")
    addr = discover_device()
    if not addr:
        print("No Doormon device found on the LAN.", file=sys.stderr)
        sys.exit(1)
    host, port = addr
    print(f"Found Doormon at http://{host}:{port}")
    print("Polling /status every second. Press 'r' + Enter to reset. Ctrl+C to quit.")
    print()

    prev_triggered = None
    reset_requested = threading.Event()
    input_buffer = []

    def input_thread():
        while True:
            try:
                line = sys.stdin.readline()
                if not line:
                    break
                if line.strip().lower() in ("r", "reset"):
                    reset_requested.set()
            except (KeyboardInterrupt, EOFError):
                break

    thread = threading.Thread(target=input_thread, daemon=True)
    thread.start()

    try:
        while True:
            if reset_requested.is_set():
                reset_requested.clear()
                if post_reset(host, port):
                    print("[Reset] Triggered state cleared.")
                    prev_triggered = False
                else:
                    print("[Reset] Request failed.")

            triggered, elapsed = get_status(host, port)
            if triggered is None:
                print(f"[Error] No response (took {elapsed:.2f}s)")
            else:
                if elapsed > SLOW_RESPONSE_THRESHOLD:
                    print(f"[Slow] Response took {elapsed:.2f}s (>{SLOW_RESPONSE_THRESHOLD}s)")
                if prev_triggered is False and triggered is True:
                    print("Triggered!")
                prev_triggered = triggered

            time.sleep(POLL_INTERVAL)
    except KeyboardInterrupt:
        print("\nBye.")


if __name__ == "__main__":
    main()
