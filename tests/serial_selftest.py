#!/usr/bin/env python3
"""Run the firmware's randomized diagnostic through USB serial (requires pyserial).
This exercises the real device code; it does not provide keys to the solver.
"""
import argparse
import re
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument("--port", required=True)
parser.add_argument("--id", default="all", choices=["all"] + [str(i) for i in range(1, 11)])
parser.add_argument("--mode", default="walk", choices=["walk", "random"])
parser.add_argument("--timeout", type=float, default=900)
args = parser.parse_args()

with serial.Serial(args.port, 115200, timeout=0.2) as connection:
    connection.dtr = True
    connection.rts = False

    def receive_until(marker, timeout):
        deadline = time.monotonic() + timeout
        text = ""
        while time.monotonic() < deadline:
            raw = connection.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").replace("\r", "")
            print(line, end="", flush=True)
            text += line
            if "SELFTEST FAILED" in text or "flash save failed" in text:
                raise RuntimeError("Device self-test failed")
            if marker in text:
                return text
        raise TimeoutError(f"No {marker!r} within {timeout} seconds")

    connection.write(b"/status\n")
    before = receive_until("Stored puzzle IDs:", 30)
    before_id = re.search(r"^Puzzle: (\d+)", before, re.M)
    before_attempts = re.search(r"Candidates checked: (\d+)", before)
    connection.write(f"/selftest {args.id} {args.mode}\n".encode())
    result = receive_until("SELFTEST completed.", args.timeout)
    expected = range(1, 11) if args.id == "all" else [int(args.id)]
    for puzzle in expected:
        assert f"SELFTEST PASS puzzle {puzzle}:" in result
    if args.mode == "walk":
        assert "walk attempts" in result and "reference checks:" in result
    else:
        assert "independent random attempts" in result
    connection.write(b"/status\n")
    after = receive_until("Stored puzzle IDs:", 30)
    if before_id:
        assert f"Puzzle: {before_id.group(1)}\n" in after
    if before_attempts:
        match = re.search(r"Candidates checked: (\d+)", after)
        assert match and int(match.group(1)) >= int(before_attempts.group(1))
    connection.write(f"/found {10 if args.id == 'all' else args.id}\n".encode())
    receive_until("Telegram delivery:", 30)
    print("DEVICE SELFTEST PASSED; original puzzle retained and discovered results stored", flush=True)
