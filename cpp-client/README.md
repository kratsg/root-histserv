# HistServ C++ client

A standalone C++ gRPC client for [HistServ](https://github.com/kratsg/histserv). It does **not**
reimplement any binning or forward per-event fills over the network. Instead:

1. You fill a ROOT `TH1`/`TH2`/`TH3` locally, the normal fast way.
2. `HistServClient::Init`/`Fill` serializes that histogram's *current* bin contents (and, if
   `Sumw2()` is active, variances) into the exact wire format HistServ's `ChunkedHist` already
   expects, and sends it in a single RPC.

No new server-side protocol or `.proto` changes were needed — this talks to the existing,
unmodified `HistogrammerService`, and `hist.proto` is read straight out of the installed
`histserv` Python package (a pixi dependency of this workspace), not vendored here.

## API

```cpp
histserv_client::HistServClient client(grpc::CreateChannel("host:port", grpc::InsecureChannelCredentials()));

// Register a new remote histogram, seeded from an already-filled TH1's current state.
std::string hist_id = client.Init(&h);

// Merge another already-filled TH1's current state into it (e.g. from a different job).
// unique_id reuses HistServ's existing idempotency mechanism (FillRequest.unique_id /
// WasFilledWithUniqueId) -- a retried call with the same id is rejected (ALREADY_EXISTS,
// surfaced as HistServError::code()) rather than double-counted. No separate delta-tracking
// is implemented on top of that.
client.Fill(hist_id, &h2, /*token=*/"", /*unique_id=*/"job2-run1");
```

Supported: 1D/2D/3D histograms (`TH1`/`TH2`/`TH3` and their `C`/`S`/`I`/`F`/`D` variants), fixed-
or variable-width bins per axis, with or without `Sumw2()`.

Explicitly rejected (`std::invalid_argument`): `TProfile`/`TProfile2D`/`TProfile3D` — their bin
content is a per-bin *mean*, not a sum, and HistServ's merge is a plain accumulate-in-place sum;
summing two profiles' means server-side would silently produce a meaningless result.

### Known limitations (inherited from HistServ's own wire protocol, not worked around here)

- **`Init()` is not idempotent.** `InitRequest` carries no `unique_id` field (unlike
  `FillRequest`/`FillManyRequest`). If the response is lost after the server already created the
  histogram — a normal gRPC at-least-once failure mode — retrying `Init()` creates a second, fully
  duplicated remote histogram, with no way to detect this from the client alone.
- **`Fill()` sends the TH1's full current state, not a delta.** Call it once per already-finished,
  independent contributing histogram (e.g. once at the end of each job). Calling it repeatedly
  against the *same*, still-growing `TH1*` will double-count whatever was already sent in a
  previous call — `unique_id` only rejects a byte-for-byte identical retry, it does not detect or
  subtract out prior partial sends.
- **`fEntries` and the raw statistical moments (`fTsumw*`) are not transmitted.** Only bin contents
  and (optionally) variances cross the wire, so `GetMean()`/`GetStdDev()` on the reconstructed
  histogram are not preserved exactly (they can still be approximated from bin centers).

## Building

Everything needed (ROOT, `libgrpc`, `libprotobuf`, `protoc`, `grpc_cpp_plugin`, `nlohmann_json`,
`cmake`, `ninja`, compilers, and `histserv` itself) comes from the workspace's `pixi.toml` — from
the repo root:

```sh
pixi run build       # configures (if needed) + builds into cpp-client/build/
pixi run test        # builds, starts a real histserv server, and runs the integration suite
```

## Running the demo

```sh
pixi run serve &                              # starts HistServ on localhost:50051
pixi run ./cpp-client/build/histserv_demo      # fills histograms in ROOT, pushes them, prints hist_ids
```

The demo fills, in ROOT: two weighted, fixed-bin `TH1D`s representing independent "jobs" merged
server-side into one remote histogram; an unweighted, variable-bin-width `TH1D`; a weighted 2D
`TH2D` (exercising the axis-reindexing needed because ROOT's own internal bin storage order is the
transpose of boost-histogram's); and a `TProfile`, which it confirms is rejected. It also
demonstrates that retrying `Fill` with the same `unique_id` is rejected rather than
double-counted.

To check a result against the real Python client:

```python
from histserv.client import Client
with Client(address="localhost:50051") as client:
    print(client.connect("<hist_id printed above>").snapshot().to_hist())
```

See [`test/test_integration.py`](test/test_integration.py) for the automated version of this
check, run against independently-built PyROOT reference histograms.
