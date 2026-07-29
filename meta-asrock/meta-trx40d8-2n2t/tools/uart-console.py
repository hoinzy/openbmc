#!/usr/bin/env python3
"""Minimal interactive serial console with an append-only raw capture."""

import argparse
import os
import select
import sys
import termios
import threading
import time
import tty

import serial


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("device")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--log", required=True)
    parser.add_argument(
        "--interrupt-pattern",
        help="start a short space-character burst when this byte string appears",
    )
    args = parser.parse_args()

    serial_port = serial.Serial(
        args.device,
        baudrate=args.baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0,
        write_timeout=1,
        xonxoff=False,
        rtscts=False,
        dsrdtr=False,
    )

    stdin_fd = sys.stdin.fileno()
    old_terminal = termios.tcgetattr(stdin_fd) if os.isatty(stdin_fd) else None
    if old_terminal is not None:
        tty.setraw(stdin_fd)

    try:
        interrupt_pattern = (
            args.interrupt_pattern.encode() if args.interrupt_pattern else None
        )
        recent_output = b""
        interrupt_sent = False
        pending_carriage_return = False

        def write_display(data: bytes) -> None:
            """Render every serial line ending as CRLF while keeping the log raw."""
            nonlocal pending_carriage_return
            rendered = bytearray()
            for byte in data:
                if pending_carriage_return:
                    rendered.extend(b"\r\n")
                    pending_carriage_return = False
                    if byte == 0x0A:
                        continue
                if byte == 0x0D:
                    pending_carriage_return = True
                elif byte == 0x0A:
                    rendered.extend(b"\r\n")
                else:
                    rendered.append(byte)
            if rendered:
                sys.stdout.buffer.write(rendered)
                sys.stdout.buffer.flush()

        with open(args.log, "ab", buffering=0) as capture:
            while True:
                readable, _, _ = select.select(
                    [stdin_fd, serial_port.fileno()], [], [], 0.25
                )
                if serial_port.fileno() in readable:
                    data = serial_port.read(serial_port.in_waiting or 1)
                    if data:
                        capture.write(data)
                        write_display(data)
                        if interrupt_pattern and not interrupt_sent:
                            recent_output = (
                                recent_output + data
                            )[-max(4096, len(interrupt_pattern)) :]
                            if interrupt_pattern in recent_output:
                                interrupt_sent = True
                                def interrupt_autoboot() -> None:
                                    deadline = time.monotonic() + 3
                                    while time.monotonic() < deadline:
                                        serial_port.write(b" ")
                                        serial_port.flush()
                                        time.sleep(0.001)

                                threading.Thread(
                                    target=interrupt_autoboot,
                                    daemon=True,
                                ).start()
                                sys.stderr.write(
                                    "\n[uart-console: autoboot interrupt burst started]\n"
                                )
                                sys.stderr.flush()
                if stdin_fd in readable:
                    data = os.read(stdin_fd, 1024)
                    if not data or b"\x1d" in data:
                        break
                    serial_port.write(data)
    finally:
        if old_terminal is not None:
            termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_terminal)
        serial_port.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
