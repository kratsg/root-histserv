# root-histserv development

## Purpose

A standalone C++ gRPC client that pushes already-filled ROOT histograms (`TH1`/`TH2`/`TH3`) to
[HistServ](https://github.com/kratsg/histserv). It does not reimplement binning or forward
per-event fills — it serializes a histogram's current state into HistServ's existing wire format
and sends it in one RPC. See [`REPORT.md`](REPORT.md) for the full ROOT/UHI/HistServ architecture
investigation that led to this design, and [`cpp-client/README.md`](cpp-client/README.md) for the
client's API and known limitations.

## Project layout

- `pixi.toml` / `pixi.lock`: all dependencies (ROOT, gRPC, protobuf, `histserv`, `nlohmann_json`,
  build tools) and tasks (`configure`, `build`, `test`, `serve`)
- `cpp-client/CMakeLists.txt`: generates C++ protobuf/gRPC stubs from `hist.proto`, read directly
  out of the **installed** `histserv` package (not vendored, not a sibling checkout)
- `cpp-client/src/root_hist_serialize.{hpp,cpp}`: `TH1`/`TH2`/`TH3` → HistServ wire format
- `cpp-client/src/hist_serv_client.{hpp,cpp}`: the gRPC client (`Init`/`Fill`)
- `cpp-client/src/main.cpp`: demo executable
- `cpp-client/test/test_integration.py`: the only test suite — starts a real `histserv` server,
  runs the compiled demo, and cross-checks results against independently-built PyROOT references
- `.github/workflows/ci.yml`: builds and runs the integration test on every PR
- `REPORT.md`: the ROOT / UHI / HistServ architecture report this client's design is derived from

## Facts that must not be re-derived from assumption

These were verified empirically (via `pixi run python` against the real `boost_histogram`/`hist`/
`histserv` packages and a real ROOT `TH2`) before being relied on in code. If anything here seems
to need changing, re-verify empirically rather than trusting a comment or this file:

- HistServ's `hist_json` schema is (for the axis/storage shapes this client emits) structurally
  identical to the JSON `boost_histogram.serialization` produces — regular axis fields
  `type/lower/upper/bins/underflow/overflow/circular`, plus per-axis `metadata: {name, label}`.
- `"double"` storage's dense bytes are a plain packed `float64` array (little-endian, C order).
  `"weighted"` storage's dense bytes are a packed `{double value; double variance;}` struct with
  **no padding** — this matches boost-histogram's `WeightedSumView` dtype exactly.
- ROOT's bin-flow convention (bin `0` = underflow, bin `nbins+1` = overflow) matches
  boost-histogram's flow-inclusive `view(flow=True)` ordering directly — no reindexing needed
  *per axis*.
- **Multi-dimensional histograms DO need reindexing.** ROOT's own `TH2::GetBin(bx,by)` stores the
  *last* axis as slowest-varying (`by*(nx+2)+bx`); boost-histogram's dense view stores the
  *first-declared* axis as slowest-varying. These are transposes of each other. The serializer
  must walk bins via `GetBin()`/`GetBinContent()` (virtual dispatch, correct regardless of axis
  count) rather than copying ROOT's raw internal buffer.
- `TH1::GetBinErrorSqUnchecked` is `protected` and cannot be called from client code — use the
  public `GetSumw2()->At(bin)` instead (see `root_hist_serialize.cpp`). Do not "simplify" variance
  extraction back to `GetBinError(bin) * GetBinError(bin)` — that round-trips through `sqrt`, which
  does not exactly invert for roughly half of all doubles, and a past version of this code had
  exactly that bug (see git history and the integration test's exact-equality variance checks).

## Dev environment

Everything is managed by [pixi](https://pixi.sh) — no separate ROOT/gRPC/protobuf install:

```sh
pixi run build   # cmake configure + build into cpp-client/build/
pixi run test    # build, start a real histserv server, run cpp-client/test/
pixi run serve   # run a histserv server on localhost:50051 (foreground)
```

`pixi.toml` targets both `osx-arm64` and `linux-64` (CI runs on `ubuntu-latest`) — if you add a
dependency, confirm `pixi install` still resolves on both before committing (`pixi.lock` covers
both platforms already).

## Testing instructions

- The only test suite is `cpp-client/test/test_integration.py`, run via `pixi run test`. There is
  no unit-test layer separate from this end-to-end suite — the serializer's correctness is
  fundamentally about matching HistServ's/ROOT's actual byte-level behavior, which is what an
  end-to-end round trip through a live server verifies.
- Follow TDD for changes to `root_hist_serialize.cpp`/`hist_serv_client.cpp`: add or extend a case
  in `test_integration.py` (and the matching block in `main.cpp`, which the test parses via its
  `HIST_ID <label> <id>` stdout lines) that would fail under the bug, confirm it fails, then fix.
- Where a value could plausibly round-trip inexactly (variances, in particular), prefer an exact
  equality assertion over `pytest.approx` — approximate assertions previously masked a real
  precision bug (see the git history for the `GetBinErrorSqUnchecked` fix).
- Never commit `cpp-client/build/` or `__pycache__/` (already gitignored).

## Working on code

- Keep `cpp-client/README.md`'s "Supported" / "Explicitly rejected" / "Known limitations" sections
  in sync with `root_hist_serialize.cpp` and `hist_serv_client.hpp` whenever behavior changes.
- This client intentionally does not touch HistServ's `.proto` or server code — it is meant to
  work against HistServ exactly as published. If a limitation genuinely requires a server/protocol
  change, that belongs in a change to the `histserv` project, not a workaround here.
- Small, focused commits grouped by logical change (CMake scaffold, serializer, client, demo,
  tests, CI, docs), not one large commit — see the existing git history for the intended
  granularity.

## PR instructions

- Commit messages follow Conventional Commits (`feat:`, `fix:`, `test:`, `ci:`, `docs:`, `chore:`),
  with a `cpp-client` scope for anything under that directory.
- Run `pixi run test` (which implies `pixi run build`) before committing — CI runs the same task.
