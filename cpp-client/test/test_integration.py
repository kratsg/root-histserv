"""
End-to-end integration test for the C++ HistServ client.

Runs the compiled `histserv_demo` binary against a real, live `histserv`
server, then independently rebuilds the same histograms in PyROOT (using the
exact same TRandom3 seeds/parameters `main.cpp` uses) and asserts the remote
histogram's bin contents match. This is the automated version of the manual
cross-checks performed while developing this client -- it must stay in sync
with `main.cpp`'s parameters (each test documents exactly which block of
`main.cpp` it mirrors).

Requires `pixi run build` to have produced cpp-client/build/histserv_demo.
"""

from __future__ import annotations

import array
import dataclasses
import socket
import subprocess
import sys
import time
from collections.abc import Iterator
from pathlib import Path

import pytest
import ROOT
from histserv.client import Client

REPO_ROOT = Path(__file__).resolve().parents[2]
DEMO_BINARY = REPO_ROOT / "cpp-client" / "build" / "histserv_demo"


@dataclasses.dataclass
class DemoRun:
    hist_ids: dict[str, str]
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


@pytest.fixture(scope="module")
def demo_run(server_address: str) -> DemoRun:
    if not DEMO_BINARY.exists():
        pytest.skip(f"{DEMO_BINARY} not built -- run `pixi run build` first")
    result = subprocess.run(
        [str(DEMO_BINARY), server_address], capture_output=True, text=True, check=True
    )
    hist_ids: dict[str, str] = {}
    for line in result.stdout.splitlines():
        if line.startswith("HIST_ID "):
            _, label, hist_id = line.split()
            hist_ids[label] = hist_id
    return DemoRun(hist_ids=hist_ids, stdout=result.stdout)


def _snapshot(server_address: str, hist_id: str):
    with Client(address=server_address) as client:
        return client.connect(hist_id).snapshot().to_hist()


def test_merged_1d_weighted(server_address: str, demo_run: DemoRun) -> None:
    """Mirrors main.cpp's job1/job2 block: two independent, weighted,
    fixed-bin TH1Ds merged server-side into one remote histogram."""
    h1 = ROOT.TH1D("job1", "Filled locally by job 1", 20, -5.0, 5.0)
    h1.Sumw2()
    rng1 = ROOT.TRandom3(42)
    for _ in range(20000):
        h1.Fill(rng1.Gaus(0.0, 1.0))

    h2 = ROOT.TH1D("job2", "Filled locally by job 2", 20, -5.0, 5.0)
    h2.Sumw2()
    rng2 = ROOT.TRandom3(1337)
    for _ in range(10000):
        h2.Fill(rng2.Gaus(0.5, 1.5))

    result = _snapshot(server_address, demo_run.hist_ids["merged_1d"])
    view = result.view(flow=True)
    for i in range(h1.GetNbinsX() + 2):
        expected_value = h1.GetBinContent(i) + h2.GetBinContent(i)
        # Unweighted fills -> fSumw2 holds integer bin counts. Compare EXACTLY
        # (not pytest.approx): the client must read variance via GetSumw2(),
        # not GetBinError()**2, since sqrt-then-square does not exactly
        # round-trip for ~half of all doubles and would otherwise make this
        # assertion flaky.
        expected_var = h1.GetSumw2().At(i) + h2.GetSumw2().At(i)
        assert view["value"][i] == pytest.approx(expected_value)
        assert view["variance"][i] == expected_var


def test_merged_1d_retry_rejected(demo_run: DemoRun) -> None:
    """Mirrors main.cpp's ExpectRejected call: retrying Fill with the same
    unique_id must be rejected (ALREADY_EXISTS), not double-counted."""
    assert "Retry with the same unique_id correctly rejected" in demo_run.stdout


def test_variable_bin_unweighted(server_address: str, demo_run: DemoRun) -> None:
    """Mirrors main.cpp's job3 block: unweighted, variable-bin-width TH1D."""
    edges = array.array("d", [-5.0, -1.0, -0.5, 0.0, 0.5, 1.0, 5.0])
    h3 = ROOT.TH1D("job3", "Unweighted, variable-width bins", 6, edges)
    rng3 = ROOT.TRandom3(7)
    for _ in range(5000):
        h3.Fill(rng3.Gaus(0.0, 1.0))

    result = _snapshot(server_address, demo_run.hist_ids["variable_1d"])
    view = result.view(flow=True)
    for i in range(h3.GetNbinsX() + 2):
        assert view[i] == pytest.approx(h3.GetBinContent(i))
    assert list(result.axes[0].edges) == list(edges)


def test_weighted_2d(server_address: str, demo_run: DemoRun) -> None:
    """Mirrors main.cpp's job4 block: 2D weighted TH2D. This specifically
    exercises the x-outer/y-inner reindexing (ROOT's own TH2 buffer order is
    the transpose of this), so a transposition bug would show up here."""
    h4 = ROOT.TH2D("job4", "2D weighted", 8, -4.0, 4.0, 5, -2.5, 2.5)
    h4.Sumw2()
    rng4 = ROOT.TRandom3(99)
    for _ in range(8000):
        h4.Fill(rng4.Gaus(0.0, 1.5), rng4.Gaus(0.0, 1.0), 1.5)

    result = _snapshot(server_address, demo_run.hist_ids["weighted_2d"])
    view = result.view(flow=True)
    nx, ny = h4.GetNbinsX() + 2, h4.GetNbinsY() + 2
    for ix in range(nx):
        for iy in range(ny):
            bin_ = h4.GetBin(ix, iy)
            expected_value = h4.GetBinContent(bin_)
            expected_var = h4.GetSumw2().At(bin_)
            assert view["value"][ix, iy] == pytest.approx(expected_value), (ix, iy)
            assert view["variance"][ix, iy] == expected_var, (ix, iy)


def test_profile_rejected(demo_run: DemoRun) -> None:
    """TProfile's bin content is a mean, not a sum; summing two profiles'
    means server-side would be meaningless, so the client must reject it
    before ever contacting the server (see main.cpp's ExpectProfileRejected).
    No hist_id is created for it, and the rejection message must be present."""
    assert "profile" not in demo_run.hist_ids
    assert "TProfile correctly rejected" in demo_run.stdout
