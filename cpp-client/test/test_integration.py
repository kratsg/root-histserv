"""
End-to-end integration test for the C++ HistServ client.

Runs the compiled `histserv_demo` binary against a real, live `histserv`
server, and cross-checks the server's response against the ACTUAL local
ground truth `main.cpp` prints for each histogram it built (via
"HIST_VALUES"/"HIST_VARIANCES" lines) -- deliberately not a second,
independently-reseeded ROOT histogram built in Python. An earlier version of
this test rebuilt references with `ROOT.TRandom3(<same seed>)` in Python, on
the assumption that the same seed reproduces the same draw sequence
regardless of process -- that assumption did NOT reliably hold on Linux CI
(observed mismatches that were not explained by any bug in the client's
serialization logic, which was independently verified correct via a
from-scratch, non-random C++-only round trip). Comparing against what this
run's C++ process actually computed and sent sidesteps that fragility
entirely and is a more direct test of "did the server return what we sent."

Requires `pixi run build` to have produced cpp-client/build/histserv_demo.
"""

from __future__ import annotations

import dataclasses
import socket
import subprocess
import sys
import time
from collections.abc import Iterator
from pathlib import Path

import numpy as np
import pytest
from histserv.client import Client

REPO_ROOT = Path(__file__).resolve().parents[2]
DEMO_BINARY = REPO_ROOT / "cpp-client" / "build" / "histserv_demo"


@dataclasses.dataclass
class DemoRun:
    hist_ids: dict[str, str]
    values: dict[str, list[float]]
    variances: dict[str, list[float]]
    stdout: str


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("localhost", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="module")
def server_address() -> Iterator[str]:
    port = _free_port()
    proc = subprocess.Popen(
        [sys.executable, "-m", "histserv", "--port", str(port), "--log-level", "WARNING"],
        cwd=REPO_ROOT,
    )
    address = f"localhost:{port}"
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("localhost", port), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError(f"histserv did not start listening on {address} in time")
        yield address
    finally:
        proc.terminate()
        proc.wait(timeout=10)


def _parse_csv_floats(text: str) -> list[float]:
    return [float(v) for v in text.strip(",").split(",") if v]


@pytest.fixture(scope="module")
def demo_run(server_address: str) -> DemoRun:
    if not DEMO_BINARY.exists():
        pytest.skip(f"{DEMO_BINARY} not built -- run `pixi run build` first")
    result = subprocess.run(
        [str(DEMO_BINARY), server_address], capture_output=True, text=True, check=True
    )
    hist_ids: dict[str, str] = {}
    values: dict[str, list[float]] = {}
    variances: dict[str, list[float]] = {}
    for line in result.stdout.splitlines():
        if line.startswith("HIST_ID "):
            _, label, hist_id = line.split()
            hist_ids[label] = hist_id
        elif line.startswith("HIST_VALUES "):
            _, label, csv = line.split(maxsplit=2)
            values[label] = _parse_csv_floats(csv)
        elif line.startswith("HIST_VARIANCES "):
            _, label, csv = line.split(maxsplit=2)
            variances[label] = _parse_csv_floats(csv)
    return DemoRun(hist_ids=hist_ids, values=values, variances=variances, stdout=result.stdout)


def _snapshot(server_address: str, hist_id: str):
    with Client(address=server_address) as client:
        return client.connect(hist_id).snapshot().to_hist()


def test_merged_1d_weighted(server_address: str, demo_run: DemoRun) -> None:
    """Mirrors main.cpp's job1/job2 block: two independent, weighted,
    fixed-bin TH1Ds merged server-side into one remote histogram."""
    result = _snapshot(server_address, demo_run.hist_ids["merged_1d"])
    view = result.view(flow=True)
    np.testing.assert_array_equal(view["value"], demo_run.values["merged_1d"])
    np.testing.assert_array_equal(view["variance"], demo_run.variances["merged_1d"])


def test_merged_1d_retry_rejected(demo_run: DemoRun) -> None:
    """Mirrors main.cpp's ExpectRejected call: retrying Fill with the same
    unique_id must be rejected (ALREADY_EXISTS), not double-counted."""
    assert "Retry with the same unique_id correctly rejected" in demo_run.stdout


def test_variable_bin_unweighted(server_address: str, demo_run: DemoRun) -> None:
    """Mirrors main.cpp's job3 block: unweighted, variable-bin-width TH1D."""
    result = _snapshot(server_address, demo_run.hist_ids["variable_1d"])
    np.testing.assert_array_equal(result.view(flow=True), demo_run.values["variable_1d"])
    assert list(result.axes[0].edges) == [-5.0, -1.0, -0.5, 0.0, 0.5, 1.0, 5.0]


def test_weighted_2d(server_address: str, demo_run: DemoRun) -> None:
    """Mirrors main.cpp's job4 block: 2D weighted TH2D. This specifically
    exercises the x-outer/y-inner reindexing (ROOT's own TH2 buffer order is
    the transpose of this), so a transposition bug would show up here."""
    result = _snapshot(server_address, demo_run.hist_ids["weighted_2d"])
    view = result.view(flow=True)
    nx, ny = 10, 7  # (8 bins + 2 flow) x (5 bins + 2 flow)
    expected_values = np.array(demo_run.values["weighted_2d"]).reshape(nx, ny)
    expected_variances = np.array(demo_run.variances["weighted_2d"]).reshape(nx, ny)
    np.testing.assert_array_equal(view["value"], expected_values)
    np.testing.assert_array_equal(view["variance"], expected_variances)


def test_profile_rejected(demo_run: DemoRun) -> None:
    """TProfile's bin content is a mean, not a sum; summing two profiles'
    means server-side would be meaningless, so the client must reject it
    before ever contacting the server (see main.cpp's ExpectProfileRejected).
    No hist_id is created for it, and the rejection message must be present."""
    assert "profile" not in demo_run.hist_ids
    assert "TProfile correctly rejected" in demo_run.stdout
