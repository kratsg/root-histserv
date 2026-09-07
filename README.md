# root-histserv

A standalone C++ gRPC client that pushes already-filled [ROOT](https://root.cern) histograms
(`TH1`/`TH2`/`TH3`) to [HistServ](https://github.com/kratsg/histserv), a Python gRPC service that
holds histogram state remotely.

It does **not** reimplement histogram binning or forward per-event fills over the network. You
fill a `TH1` locally the normal, fast ROOT way; this client serializes its *current* bin contents
(and variances, if `Sumw2()` is active) into HistServ's existing wire format and sends them in a
single RPC, merged server-side by summation. This is the simplest and most efficient way to
offload histogram storage: no new protocol, no per-event RPC overhead, no rebinning logic
duplicated in C++.

See [`REPORT.md`](REPORT.md) for the full archaeology behind this design (ROOT's `TH1`/`TAxis`
internals, the UHI histogram protocol, HistServ's wire protocol, and why other designs — e.g.
subclassing `TH1`, or forwarding per-event fills — were rejected).

## Quick start

Everything (ROOT, gRPC, protobuf, `histserv`, build tools) is managed by
[pixi](https://pixi.sh); no separate ROOT/gRPC installation is required.

```sh
pixi run build   # builds cpp-client/build/histserv_demo
pixi run test    # builds, starts a real histserv server, and runs the integration suite
pixi run serve   # runs a histserv server on localhost:50051 (foreground)
```

To run the demo by hand against a server started with `pixi run serve`:

```sh
pixi run ./cpp-client/build/histserv_demo localhost:50051
```

It fills a few `TH1D`/`TH2D` histograms in ROOT (weighted and unweighted, fixed- and
variable-width bins, 1D and 2D), pushes them to the server, and prints the resulting `hist_id`s.
You can then fetch one back with the existing Python client:

```python
from histserv.client import Client
with Client(address="localhost:50051") as client:
    print(client.connect("<hist_id>").snapshot().to_hist())
```

## Repo layout

```
cpp-client/            the C++ client itself (see cpp-client/README.md)
  CMakeLists.txt        generates C++ stubs from the installed histserv package's hist.proto
  src/
    root_hist_serialize.*   TH1/TH2/TH3 -> HistServ ChunkedHistPayload serialization
    hist_serv_client.*      the gRPC client (Init/Fill)
    main.cpp                demo executable
  test/
    test_integration.py     end-to-end test: C++ demo -> live server -> PyROOT cross-check
.github/workflows/ci.yml    builds the client and runs the integration test on every PR
REPORT.md               the full ROOT / UHI / HistServ architecture investigation
pixi.toml               dependencies (root, libgrpc, libprotobuf, nlohmann_json, histserv, ...) and tasks
```

## Scope

Supported: 1D/2D/3D histograms (`TH1`/`TH2`/`TH3` and their `C`/`S`/`I`/`F`/`D` variants), fixed-
or variable-width bins per axis, with or without `Sumw2()`. HistServ's own `ChunkedHist` storage
supports multi-dimensional *dense* axes (verified directly against `histserv.chunked_hist` before
implementing this), so this covers the common HEP case directly.

Explicitly rejected: `TProfile`/`TProfile2D`/`TProfile3D` (bin content is a mean, not a sum;
summing means server-side would be meaningless — see `cpp-client/README.md`).

Not supported (and not needed for this PoC): growable ROOT axes (`SetCanExtend`) mapping onto
HistServ's *categorical*-axis growth mechanism, and categorical/labeled axes. `Init()` is also not
idempotent, and `Fill()` sends full state rather than a delta — see `cpp-client/README.md`'s
"Known limitations" section. See `REPORT.md` for why these are separate, harder problems.
