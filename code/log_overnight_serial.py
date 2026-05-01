#!/usr/bin/env python3
"""Timestamp ESP32 overnight serial logs on macOS.

Usage:
  python3 log_overnight_serial.py /dev/cu.usbmodemXXXX overnight_log.csv

Find the port with:
  ls /dev/cu.*
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
from pathlib import Path
import subprocess
import sys


def configure_serial_port(port: str, baud: int) -> None:
    subprocess.run(
        ["stty", "-f", port, str(baud), "cs8", "-cstopb", "-parenb", "-ixon", "-ixoff"],
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Log ESP32 overnight serial output with computer timestamps.")
    parser.add_argument("port", help="Serial port, for example /dev/cu.usbmodemXXXX")
    parser.add_argument("output", nargs="?", default="overnight_log.csv", help="CSV output file")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    output_path = Path(args.output)
    configure_serial_port(args.port, args.baud)

    with open(args.port, "rb", buffering=0) as serial_file, output_path.open("a", newline="") as out_file:
        writer = csv.writer(out_file)
        if output_path.stat().st_size == 0:
            writer.writerow(["computer_time_utc", "serial_line"])

        print(f"Logging {args.port} to {output_path}. Press Ctrl+C to stop.")
        while True:
            raw_line = serial_file.readline()
            if not raw_line:
                continue
            line = raw_line.decode("utf-8", errors="replace").strip()
            timestamp = datetime.now(timezone.utc).isoformat()
            writer.writerow([timestamp, line])
            out_file.flush()
            print(f"{timestamp},{line}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nStopped logging.")
        raise SystemExit(0)
    except subprocess.CalledProcessError as exc:
        print(f"Failed to configure serial port: {exc}", file=sys.stderr)
        raise SystemExit(1)
