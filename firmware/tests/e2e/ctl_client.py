"""ctl_client.py — a tiny Python client for ffsim's control socket
(firmware/targets/sim/ctl_server.h, protocol doc: firmware/tools/dev/CTL.md).

Used by test_scenarios.py to drive/inspect a running `ffsim --headless
--ctl PORT` process. Deliberately dependency-free (stdlib socket + json
only) — this is test infrastructure, not product code, but it's still
just talking the same newline-JSON protocol any other ctl client would.
"""
from __future__ import annotations

import json
import socket
import time
from typing import Any, Callable


class CtlError(RuntimeError):
    pass


class CtlClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 0, connect_timeout: float = 10.0):
        self.host = host
        self.port = port
        deadline = time.monotonic() + connect_timeout
        last_err: Exception | None = None
        sock: socket.socket | None = None
        while time.monotonic() < deadline:
            try:
                sock = socket.create_connection((host, port), timeout=2.0)
                break
            except OSError as e:  # ctl socket not listening yet
                last_err = e
                time.sleep(0.1)
        if sock is None:
            raise CtlError(f"could not connect to ctl socket {host}:{port}: {last_err}")
        self._sock = sock
        self._file = sock.makefile("rwb", buffering=0)

    def send(self, cmd: dict[str, Any]) -> dict[str, Any]:
        line = json.dumps(cmd) + "\n"
        self._file.write(line.encode("utf-8"))
        raw = self._file.readline()
        if not raw:
            raise CtlError(f"ctl socket closed while waiting for a response to {cmd!r}")
        resp = json.loads(raw.decode("utf-8"))
        return resp

    def state(self) -> dict[str, Any]:
        resp = self.send({"cmd": "state"})
        if not resp.get("ok"):
            raise CtlError(f"state command failed: {resp}")
        return resp["state"]

    def screenshot(self, path: str) -> None:
        resp = self.send({"cmd": "screenshot", "path": path})
        if not resp.get("ok"):
            raise CtlError(f"screenshot command failed: {resp}")

    def wait_for(
        self,
        predicate: Callable[[dict[str, Any]], bool],
        timeout: float = 15.0,
        interval: float = 0.25,
    ) -> dict[str, Any]:
        """Polls `state()` until `predicate(state)` is true, or raises
        CtlError once `timeout` seconds have elapsed (returning the last
        seen state in the exception message for debuggability)."""
        deadline = time.monotonic() + timeout
        last_state: dict[str, Any] = {}
        while time.monotonic() < deadline:
            last_state = self.state()
            if predicate(last_state):
                return last_state
            time.sleep(interval)
        raise CtlError(f"timed out after {timeout}s waiting for state condition; last state: {last_state!r}")

    def quit(self) -> None:
        try:
            self.send({"cmd": "quit"})
        except (CtlError, OSError):
            pass  # best-effort: the process may already be on its way out

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass
