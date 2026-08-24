"""conftest.py — S14 slice d: e2e test fixtures.

Brings up the dockerized meshtasticd (firmware/tools/dev/compose.yml),
launches `ffsim --headless --ctl PORT --connect ... --pack ...` against
it, and tears both down afterward. Every fixture here skips cleanly
(rather than erroring) when its prerequisite isn't available — docker,
the `ffsim` binary, or the `docker compose` plugin — per the task's
"mark them so they're skipped cleanly when docker is unavailable"
requirement.
"""
from __future__ import annotations

import pathlib
import socket
import subprocess
import sys
import time
from typing import Iterator

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ctl_client import CtlClient  # noqa: E402

E2E_DIR = pathlib.Path(__file__).resolve().parent
FIRMWARE_DIR = E2E_DIR.parent.parent
COMPOSE_FILE = FIRMWARE_DIR / "tools" / "dev" / "compose.yml"
FFSIM_BIN = FIRMWARE_DIR / "build" / "ffsim"
FESTPACK_FIXTURE = FIRMWARE_DIR / "festpack" / "tests" / "fixtures" / "lost-lands-2026.festpack.json"
CREW_SIM = FIRMWARE_DIR / "tools" / "dev" / "crew_sim.py"

MESHTASTICD_PORT = 4403
COMPOSE_PROJECT = "ffsim-e2e"


def _have_docker() -> bool:
    try:
        subprocess.run(["docker", "version"], capture_output=True, timeout=10, check=True)
    except (OSError, subprocess.SubprocessError):
        return False
    return True


def _have_compose_plugin() -> bool:
    try:
        subprocess.run(["docker", "compose", "version"], capture_output=True, timeout=10, check=True)
    except (OSError, subprocess.SubprocessError):
        return False
    return True


def _have_meshtastic_package() -> bool:
    try:
        import meshtastic  # noqa: F401
    except ImportError:
        return False
    return True


def _port_open(host: str, port: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1.0):
                return True
        except OSError:
            time.sleep(0.3)
    return False


@pytest.fixture(scope="session")
def docker_ready() -> None:
    if not _have_docker():
        pytest.skip("docker is not available in this environment")
    if not _have_compose_plugin():
        pytest.skip("docker compose plugin is not available in this environment")


@pytest.fixture(scope="session")
def meshtastic_package_ready() -> None:
    if not _have_meshtastic_package():
        pytest.skip("the `meshtastic` pip package is not installed (pip install -r requirements.txt)")


@pytest.fixture(scope="session")
def ffsim_bin() -> pathlib.Path:
    if not FFSIM_BIN.exists():
        pytest.skip(
            f"{FFSIM_BIN} not found — build first: "
            "cmake -S firmware -B firmware/build -DFF_TARGET=sim && cmake --build firmware/build -j8"
        )
    return FFSIM_BIN


@pytest.fixture(scope="session")
def meshtasticd(docker_ready: None) -> Iterator[None]:
    """Brings up firmware/tools/dev/compose.yml's meshtasticd for the
    whole test session (each test starts/stops its own ffsim process
    against it — see the run_ffsim fixture — so a single shared
    meshtasticd instance is enough; see compose.yml/README.md's
    documented single-client-connection limitation for why tests don't
    run concurrent ffsim/crew_sim connections against it)."""
    subprocess.run(
        ["docker", "compose", "-f", str(COMPOSE_FILE), "-p", COMPOSE_PROJECT, "up", "-d"],
        check=True,
        timeout=120,
    )
    try:
        if not _port_open("127.0.0.1", MESHTASTICD_PORT, timeout=60):
            pytest.fail(f"meshtasticd never opened port {MESHTASTICD_PORT}")
        # meshtasticd's TCP listener comes up (accepts a bare connect())
        # a few seconds before its ApiServer/PhoneAPI handler is actually
        # ready to service a client — connecting too early was observed
        # (in this repo's own dev session) to fail the very first write
        # with a "Bad file descriptor" error. A short settle delay avoids
        # that race; there's no cheap health-check endpoint to poll
        # instead (the client API IS the thing we're waiting to become
        # ready).
        time.sleep(8.0)
        yield
    finally:
        subprocess.run(
            ["docker", "compose", "-f", str(COMPOSE_FILE), "-p", COMPOSE_PROJECT, "down", "-v"],
            check=False,
            timeout=60,
        )


class FfsimProcess:
    def __init__(self, proc: subprocess.Popen, ctl: CtlClient):
        self.proc = proc
        self.ctl = ctl

    def shutdown(self) -> None:
        self.ctl.quit()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        self.ctl.close()


@pytest.fixture
def run_ffsim(ffsim_bin: pathlib.Path, meshtasticd: None, tmp_path: pathlib.Path):
    """Factory fixture: `run_ffsim(extra_args)` launches
    `ffsim --headless --ctl PORT --pack <lost-lands fixture> <extra_args>`,
    waits for the ctl socket, and returns an FfsimProcess. Automatically
    shut down (ctl `quit`, then killed if it doesn't exit) at test
    teardown, even on failure."""
    procs: list[FfsimProcess] = []
    next_port = [18400]  # arbitrary, unlikely-to-collide local range

    def _start(extra_args: list[str] | None = None) -> FfsimProcess:
        port = next_port[0]
        next_port[0] += 1
        args = [
            str(ffsim_bin),
            "--headless",
            "--ctl",
            str(port),
            "--pack",
            str(FESTPACK_FIXTURE),
            # S16 slice b2: live mode drives the app shell, which (unlike
            # the retired live.c wiring) enforces the roster trust policy
            # — unpaired senders produce no feed items and no crew slots.
            # --dev-trust-all is the sim-only dev affordance for exactly
            # this harness: it auto-pairs NodeInfo senders, suspends the
            # self filter (the dockerized meshtasticd is a SINGLE node
            # whose id is also ffsim's own — crew_sim.py's "one node
            # plays every role" constraint), and latches the wall clock
            # from the host so the replayed position can be aged honestly
            # (see ff_shell.h's dev-affordances section).
            "--dev-trust-all",
        ] + (extra_args or [])
        log_path = tmp_path / f"ffsim-{port}.log"
        log_file = log_path.open("wb")
        proc = subprocess.Popen(args, stdout=log_file, stderr=subprocess.STDOUT, cwd=str(FIRMWARE_DIR))
        try:
            ctl = CtlClient("127.0.0.1", port, connect_timeout=15.0)
        except Exception:
            proc.kill()
            proc.wait(timeout=5)
            raise
        fp = FfsimProcess(proc, ctl)
        procs.append(fp)
        return fp

    yield _start

    for fp in procs:
        fp.shutdown()


@pytest.fixture
def crew_sim():
    """Factory fixture: `crew_sim(["walk", "--node", "Dana", ...])` runs
    firmware/tools/dev/crew_sim.py against the compose.yml meshtasticd
    (127.0.0.1:4403) and blocks until it exits. Raises on nonzero exit."""

    def _run(args: list[str], timeout: float = 90.0) -> None:
        # 90s default: crew_sim.py's `walk` can legitimately take up to
        # ~45s just for _wait_for_a_position's own timeout (see that
        # function's docstring — meshtasticd's position-update rate
        # limiting and precision truncation, found empirically while
        # fixing PR #19 finding #5, mean waiting for the daemon to
        # actually report a position back can take tens of seconds even
        # on a healthy run), plus the walk's own narration time.
        subprocess.run(
            [sys.executable, str(CREW_SIM), "--host", "127.0.0.1", "--port", str(MESHTASTICD_PORT)] + args,
            check=True,
            timeout=timeout,
            cwd=str(E2E_DIR),
        )

    return _run
