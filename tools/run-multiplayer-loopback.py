#!/usr/bin/env python3
"""Run the EDF multiplayer client against the actual local dedicated server."""
from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def unused_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_until_bound(process: subprocess.Popen[bytes], port: int, log_path: Path) -> None:
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            log = log_path.read_text(encoding="utf-8", errors="replace")
            raise RuntimeError(f"server exited with code {process.returncode}:\n{log}")
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.bind(("127.0.0.1", port))
        except OSError:
            return
        time.sleep(0.05)
    raise RuntimeError(f"server did not bind UDP port {port} within 8 seconds")


def run_mode(server: Path, client_test: Path, work_dir: Path, profile_root: Path,
             authoritative: bool) -> None:
    name = "server-authoritative" if authoritative else "client-authoritative"
    mode_dir = work_dir / name
    data_dir = mode_dir / "server-data"
    data_dir.mkdir(parents=True, exist_ok=True)
    # Keep this profile shallow. On Windows, the server pin filename and
    # `EDF2027/server-pins` suffix must fit below the legacy MAX_PATH limit;
    # placing LOCALAPPDATA under a build tree can exceed it.
    client_local_app_data = profile_root / name
    client_local_app_data.mkdir(parents=True, exist_ok=True)
    log_path = mode_dir / "server.log"
    port = unused_udp_port()
    server_args = [
        str(server), "--name", "loopback-test", "--mission", "Mission 1",
        "--bind", "127.0.0.1", "--port", str(port), "--no-discovery",
        "--data-dir", str(data_dir),
    ]
    if authoritative:
        server_args.extend(("--server-authoritative-movement", "--spawn-x", "0", "--spawn-y", "0", "--spawn-z", "0"))

    with log_path.open("wb") as log:
        server_process = subprocess.Popen(
            server_args,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        try:
            wait_until_bound(server_process, port, log_path)
            test_mode = "--server-authoritative-network" if authoritative else "--network"
            result = subprocess.run(
                [str(client_test), test_mode, f"127.0.0.1:{port}"],
                capture_output=True,
                text=True,
                timeout=15,
                check=False,
                env={**os.environ, "LOCALAPPDATA": str(client_local_app_data)},
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            if result.stdout:
                print(result.stdout, end="")
            if result.returncode:
                server_process.poll()
                log.flush()
                server_log = log_path.read_text(encoding="utf-8", errors="replace")
                raise RuntimeError(
                    f"{name} client test failed with code {result.returncode}.\n"
                    f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}\n"
                    f"server log:\n{server_log}"
                )
        finally:
            if server_process.poll() is None:
                server_process.terminate()
                try:
                    server_process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    server_process.kill()
                    server_process.wait(timeout=3)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True, help="edf2027_server executable")
    parser.add_argument("--client-test", type=Path, required=True, help="edf_multiplayer_tests executable")
    parser.add_argument("--work-dir", type=Path, required=True, help="isolated directory for test server data/logs")
    args = parser.parse_args()
    for path in (args.server, args.client_test):
        if not path.is_file():
            parser.error(f"executable does not exist: {path}")
    args.work_dir.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="edf2027-loopback-") as profile_root:
            profile_path = Path(profile_root)
            run_mode(args.server.resolve(), args.client_test.resolve(), args.work_dir.resolve(),
                     profile_path, False)
            run_mode(args.server.resolve(), args.client_test.resolve(), args.work_dir.resolve(),
                     profile_path, True)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"multiplayer loopback failed: {exc}", file=sys.stderr)
        return 1
    print("client-authoritative and server-authoritative two-client ENet loopbacks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
