# Remote ROOT Histogram Filling via HistServ — Technical Report

Investigation of `./root`, `./uhi`, `./histserv` to determine the engineering path to a working
remote-histogram-filling prototype. All claims below are backed by file paths, class/function
names, and (where quoted) exact code read from the three repositories.

---

## 1. Repository Map

```
root/                           # Full ROOT source tree (C++)
  hist/hist/inc, hist/hist/src  # Classic histogramming: TH1, TH2, TH3, TAxis, TH1Merger
  hist/histv7/inc/ROOT/*.hxx    # Experimental "ROOT 7" histogram prototype (RHist, RHistEngine, ...)
  hist/histv7util/              # ConvertToTH1 (RHist <-> TH1 interop, 1D only)
  tutorials/hist/histv7/*.C     # hist001-005: RHist usage, incl. concurrent filling, TH1 conversion
  io/io/src/TBufferJSON.cxx     # Generic reflection-based JSON (de)serializer, used by JSROOT
  tree/dataframe/               # RDataFrame Histo1D/2D/3D actions (thread-local-replica-then-merge)
  core/thread/inc/ROOT/TThreadedObject.hxx  # Generic per-thread-slot + Merge() pattern

uhi/                             # Python: histogram *protocol* + typing + serialization schema
  src/uhi/typing/plottable.py   # PlottableHistogram / PlottableAxis / PlottableTraits (read-only Protocols)
  src/uhi/typing/serialization.py  # TypedDict "IR" mirror of the JSON schema
  src/uhi/resources/histogram.schema.json  # The actual wire-format JSON Schema (draft-07)
  src/uhi/schema.py             # fastjsonschema-based validator + CLI
  src/uhi/io/{json,hdf5,zip}.py # Concrete (de)serializers implementing the schema
  src/uhi/tag.py                # Indexing vocabulary: loc, underflow, overflow, at, rebin
  src/uhi/numpy_plottable.py    # NumPyPlottableHistogram + ROOT adapter (PyROOT, read-only)
  src/uhi/testing/indexing.py   # Conformance test mixins for downstream libraries' __getitem__/__setitem__

histserv/                        # Python: the actual gRPC histogram-filling service
  src/histserv/protos/hist.proto        # 11 unary RPCs, wire messages
  src/histserv/service.py               # grpc.aio servicer (Histogrammer)
  src/histserv/chunked_hist.py          # ChunkedHist: categorical-axis-chunked dense storage
  src/histserv/serialize.py             # protobuf <-> ChunkedHist wire codec
  src/histserv/client.py                # Client / RemoteHist / RemoteHistSlice (Python client)
  src/histserv/server.py, __main__.py   # gRPC server lifecycle, CLI
  example/{client.py,client_methods.py,coffea_processor.py}
  tests/test_grpc_integration.py        # Real-socket round-trip tests, local-vs-remote equality
```

Key surprise up front: **HistServ already is almost exactly the Python PoC requested in section 7
of the brief.** It has a real gRPC service, a real Python client, and tests
(`tests/test_grpc_integration.py`) that fill a `hist.Hist` both locally and remotely with identical
input and assert bit-for-bit equality — for regular axes, weighted categorical axes, growable
axes, and partial/sliced retrieval. The work items below are about **filling the specific gaps**
(a C++ client, an explicit wire-format spec, ROOT interop, benchmarking), not building from zero.

---

## 2. ROOT Histogram Architecture

### 2.1 Classic `TH1`/`TAxis`

Storage lives in the concrete subclasses, not `TH1` itself: `TH1C/S/I/F/L/D` each own a typed
`fArray` and override three **per-bin virtual hooks** (`hist/hist/inc/TH1.h`):

```cpp
Double_t RetrieveBinContent(Int_t bin) const override { return Double_t(fArray[bin]); }
void     UpdateBinContent(Int_t bin, Double_t content) override { fArray[bin] = <T>(content); }
void     AddBinContent(Int_t bin) override { ++fArray[bin]; }
void     AddBinContent(Int_t bin, Double_t w) override { fArray[bin] += <T>(w); }
```

`fNcells = ∏(nbins_i + 2)` — **flow bins are baked into the core cell count**, not an add-on.
`fEntries` counts `Fill()` calls, independent of bin content.

`TH1::Fill(x)` / `Fill(x, w)` (`src/TH1.cxx`): find bin via `TAxis::FindBin` (0 = underflow,
`nbins+1` = overflow, NaN routed to overflow), `AddBinContent`, and — critically — accumulate
**weighted raw moments** directly in `Fill()` itself, not through any virtual hook:
`fTsumw, fTsumw2, fTsumwx, fTsumwx2` (+ `fTsumwy*`, `fTsumwxy` for 2D). These back `GetMean()` /
`GetStdDev()` and are **not recoverable from binned contents alone** (bin-center approximation
loses precision) — a remote protocol aiming for exact ROOT statistics must carry them explicitly.

**Sumw2 / errors**: `Sumw2()` allocates `fSumw2` (`TArrayD`, size `fNcells`) and is auto-invoked on
the first weighted `Fill()`. `GetBinError(bin) = sqrt(fSumw2[bin])` if active, else
`sqrt(bincontent)` (Poisson assumption). **Presence/absence of `fSumw2` is itself semantically
meaningful** (unweighted-Poisson-error mode vs. explicit-variance mode) — a tri-state, not just an
optional array.

**Axes**: fixed bins via `fNbins/fXmin/fXmax`; variable bins via `TArrayD fXbins`
(`IsVariableBinSize() == fXbins.GetSize() != 0`), located via `TMath::BinarySearch`. Growable axes
(`TH1::SetCanExtend`) rebin in place via `TAxis::ExtendAxis` rather than flowing.

**Multi-dimensional indexing**: `TH2::Fill(x,y,w)` computes
`bin = biny*(nbinsx+2) + binx` — flow-bin stride is baked into the linear index; each axis's
flow is independent.

**Buffering (`fBuffer`)**: a client-side "accumulate `(w,x)` pairs, then bulk-apply" mechanism
(`TH1::BufferFill`/`BufferEmpty`), whose actual purpose is **auto-ranging** (deferring binning
until axis limits are known via `THLimitsFinder::FindGoodLimits`), not RPC amortization — but it
is concrete existing precedent that "accumulate then bulk-apply" is already a first-class ROOT
histogram state transition.

**Merge**: `TH1::Merge()` delegates to `TH1Merger` (`src/TH1Merger.h/.cxx`), which explicitly
classifies compatibility (`kAllSameAxes`, `kHasNewLimits`, `kAllLabel`, ...) and remaps bins
flow-aware:

```cpp
static Int_t FindFixBinNumber(Int_t ibin, const TAxis &inAxis, const TAxis &outAxis) {
   if (ibin == 0) return 0;                                      // underflow stays underflow
   if (ibin == inAxis.GetNbins()+1) return outAxis.GetNbins()+1;  // overflow stays overflow
   return outAxis.FindFixBin(inAxis.GetBinCenter(ibin));
}
```

This is real, tested, production merge logic covering the hard cases (different ranges, labels,
flow bins) — directly reusable as the conceptual model for any remote "merge partial state" op.

**Serialization** (two independent, both non-protobuf):
- Native ROOT I/O: `TH1::Streamer` — versioned, reflection-driven binary format for `TFile`.
- `TBufferJSON::ToJSON(h)` / `FromJSON` (`io/io/src/TBufferJSON.cxx`) — a *generic* reflection-based
  JSON serializer, already used by JSROOT for exactly the "send a live object to a non-ROOT web
  consumer" use case. Full round trip exists **today**, just not over gRPC.

**Thread safety**: classic `TH1::Fill` is not thread-safe (no mentions of "thread" in `TH1.h`).
ROOT's actual answer to concurrent filling is *replicate-and-merge*: `ROOT::TThreadedObject<TH1D>`
and RDataFrame's `Histo1D`/`Histo2D` actions both give each thread its own local `TH1` replica,
merged via `TH1::Merge()` at the end — the same "accumulate locally, merge later" idiom that
recurs at multiple layers of ROOT (buffer, threaded fill, RDataFrame), and the natural model for
a remote design too.

### 2.2 Experimental RHist v7 (`hist/histv7/inc/ROOT/*.hxx`)

Every header carries the literal warning: *"This is part of the %ROOT 7 prototype! It will change
without notice. It might trigger earthquakes."* Explicitly unstable, not production.

Architecturally **cleaner** than classic TH1 — genuine separation of concerns:
- `RHistEngine<T>` (`final`): storage + bin lookup only. Concrete `std::vector<T> fBinContents`.
- `RHistStats` (`final`): global stats only — up to **4th** raw moment per dimension (richer than
  classic TH1's 2nd-moment-only tracking), mergeable via `AddAtomic`.
- `RHist<T>` (`final`): thin composition, `Fill()` just calls both engine and stats.

**Concurrency**: no thread-local buffering of bin content — `FillAtomic` does in-place atomic
read-modify-write directly on the shared vector element. `SnapshotAtomic()` is a hand-rolled
seqlock (compare-exchange a `fSnapshotInProgress` flag, collect, re-collect until two passes
agree) — the "get a coherent point-in-time copy while concurrent fills continue" primitive.
`RHistConcurrentFiller`/`RHistFillContext` *do* thread-local-buffer, but only **stats**, merging
into the shared `RHistStats` on `Flush()` — this exact shape (local accumulate → flush → merge)
generalizes directly to a remote filler: buffer bin increments locally, then ship-and-merge instead
of atomic-on-shared-memory.

**No pluggable storage**: `RHistEngine` is `final` with a concrete `std::vector<T>` member — no
virtual or template storage-strategy hook exists. **No serialization at all, by design**:

```cpp
// RHistEngine.hxx, RHist.hxx, RHistStats.hxx
void Streamer(TBuffer &) { throw std::runtime_error("unable to store RHistEngine"); }
```

**TH1 interop** (`hist/histv7util`): `ConvertToTH1{C,S,I,L,F,D}` is one-directional, **1D only**
(throws for `NDimensions != 1`), and the engine-only overload loses entries/moments entirely
(documented: *"the number of entries and the total sum of weights will be unset"*). No `TH1→RHist`
conversion exists.

### 2.3 Does ROOT have an abstraction point for a remote backend?

**No clean one, in either stack:**

- **Classic TH1**: `AddBinContent`/`UpdateBinContent`/`RetrieveBinContent` are virtual, but `Fill()`
  itself writes `fTsumw*` moments directly (not through a hook), and dozens of other methods
  (`GetBinContent`, `GetMean`, `Draw`, `Fit`, I/O) read `fArray`/`fSumw2`/`fTsumw*` directly. There
  is no single virtual seam the rest of TH1 is written to respect — overriding the per-bin hooks
  is not sufficient for transparency.
- **RHist v7**: conceptually the right shape (Engine/Stats separation), but concretely blocked:
  `RHistEngine` is `final`, storage is a concrete `std::vector`, and serialization is explicitly
  disabled. It's also pre-1.0 ("will change without notice"), 1D-TH1-convertible only, and has no
  network/multi-process concept — its concurrency model is intra-process shared-memory atomics.

**Conclusion**: the smallest *upstreamable* hook, if ROOT changes were ever wanted, would be a
storage-strategy template parameter on `RHistEngine` (templates don't need a common base class —
just matching member signatures, since everything here is `final`). Given RHist v7's prototype
status, this is a longer-term ROOT research contribution, not a near-term PoC dependency. **For a
PoC, the realistic path is entirely outside both hierarchies**: a standalone client class, using
ROOT only for optional interop at the edges (reconstructing a `TH1D` via `SetBinContent`/`Sumw2`,
or `TBufferJSON` for JSON, or `ConvertToTH1` for RHist).

### 2.4 Minimum state to faithfully reconstruct a ROOT histogram remotely

- Name, title (`TNamed` fields — RHist v7 has no name field at all).
- Per axis: `nbins/xmin/xmax` **or** variable edges array; bin labels if alphanumeric; growable flag.
- Storage type/precision (which of C/S/I/L/F/D).
- Full `fNcells` bin-content array — **flow bins are core state**.
- Sumw2 tri-state (absent vs. present-with-array) — not just an optional array.
- `fEntries` and the raw moment sums (`fTsumw, fTsumw2, fTsumwx, fTsumwx2`, + cross terms per extra
  dimension) — needed for exact `GetMean()`/`GetStdDev()`, not recoverable from bins alone.

---

## 3. UHI Architecture

Location note: package source is `uhi/src/uhi/` (not top-level `uhi/`).

### 3.1 What UHI actually is

Per `README.md` and `AGENTS.md`, verbatim: *"UHI is primarily a **standards + typing + testing**
package, not a runtime dependency (the base package only needs numpy)."* It has four concerns:
(1) `typing/plottable.py` — the read-only Protocols; (2) `tag.py` — indexing vocabulary;
(3) `io/` + `schema.py` + `resources/` — the JSON serialization schema; (4) `numpy_plottable.py` +
`testing/` — adapters and conformance tests.

### 3.2 `PlottableHistogram` — confirmed read-only, no fill API

`src/uhi/typing/plottable.py`:

```python
@runtime_checkable
class PlottableHistogram(Protocol):
    @property
    def axes(self) -> Sequence[PlottableAxis]: ...
    @property
    def kind(self) -> Kind: ...
    def values(self) -> np.typing.NDArray[np.float64]: ...
    def variances(self) -> np.typing.NDArray[np.float64] | None: ...
    def counts(self) -> np.typing.NDArray[np.float64] | None: ...
```

`grep -rn "def fill" src` returns **nothing** in the entire package. `PlottableTraits` exposes only
`circular`/`discrete` booleans (no `growable`, no flow-related trait). `PlottableAxisGeneric`
exposes `traits`, `__getitem__`, `__len__`, `__eq__`, `__iter__` — no `.edges`/`.centers` attribute,
no `name` (deliberately excluded, per comment). Every `.fill(...)` call anywhere in the UHI test
suite is on an *external* `boost_histogram.Histogram` used to build fixtures that are then handed
to UHI's serializers — UHI never triggers a fill itself. **Conclusion, with high confidence: UHI
describes finished histogram *state* for plotting/serialization consumers; it is not, and does not
attempt to be, a filling API or a wire protocol by itself.**

(`uhi/testing/indexing.py`'s `Indexing1D/2D/3D` mixins do test `__setitem__` mutation, but against
the *downstream library's own* histogram object — not part of `PlottableHistogram`, which stays
strictly read-only regardless.)

### 3.3 `histogram.schema.json` — the actual portable wire format

JSON Schema (draft-07), 336 lines. Root is `{name: histogram}` (multiple histograms per document).
Each histogram: `uhi_schema: 1` (version gate — *"a future revision with a backward incompatible
change will bump this to 2, and readers should always error on future schemas"*), optional
`writer_info`/`metadata`, `axes: [...]`, `storage: {...}`.

**5 axis kinds**, each `oneOf`-tagged by `type`:
- `regular`: `lower, upper, bins, underflow, overflow, circular` (all required)
- `variable`: `edges` (array or string path), `underflow, overflow, circular`
- `category_str` / `category_int`: `categories`, `flow`
- `boolean`: no flow concept at all

**5 storage kinds**, each supporting empty / dense / sparse (`index` present) shapes:
`int` (`values`), `double` (`values`), `weighted` (`values, variances`),
`mean` (`counts, values, variances`), `weighted_mean` (`sum_of_weights, sum_of_weights_squared,
values, variances`).

`data_array`/`sparse_array` support **inline JSON array or a string path to an out-of-band file** —
this `oneOf` duality is exactly the shape a protobuf `oneof {repeated double; string uri}` would
express natively.

`typing/serialization.py` mirrors this 1:1 as `TypedDict`s (`HistogramIR`, `AxisIR`, `StorageIR`
unions) plus a single `ToUHIHistogram` Protocol (`_to_uhi_(self) -> dict`) — the hook any producer
implements to become serializable. This file is pure declaration; actual codecs live in `io/`:
`io/json.py` (`default`/`object_hook` for `json.dumps`/`loads`), `io/zip.py` (arrays → `.npy` files
inside a zip), `io/hdf5.py` (arrays → HDF5 datasets, axes linked via `h5py.Reference`).

`docs/serialization.md` states the design rationale plainly: format is *"based heavily on
boost-histogram"*, and **ROOT is an explicitly unimplemented ("todo") serialization target** — no
ROOT *file* format exists in UHI, distinct from the adapter below.

### 3.4 The existing ROOT adapter — read-only, already tested, PyROOT-dependent

`src/uhi/numpy_plottable.py` ships live PyROOT adapter classes:
`ROOTAxis`/`ContinuousROOTAxis`/`DiscreteROOTAxis`, `ROOTPlottableHistBase`,
`ROOTPlottableHistogram`, `ROOTPlottableProfile`, dispatched by `ensure_plottable_histogram(hist)`
via `hist.InheritsFrom("TH1")`. It does a **zero-copy** read of ROOT's internal buffer:

```python
def _roottarray_asnumpy(tarr, shape=None):
    llv = tarr.GetArray()
    arr = np.frombuffer(llv, dtype=llv.typecode, count=tarr.GetSize())
    return np.reshape(arr, shape, order="F") if shape is not None else arr
```

`ROOTPlottableHistogram.values()` strips flow bins via `[slice(1,-1)]*ndim`; `variances()` reads
`GetSumw2()` if present else falls back to `values()` (Poisson); `counts()` implements the exact
effective-count formula (`sumw²/sumw2`) documented in `PlottableHistogram.counts()`'s docstring.
`ROOTPlottableProfile` (`kind=MEAN`) covers `TProfile`/`TProfile2D`/`TProfile3D` but loops
per-cell via `GetBinContent`/`GetBinError` (no zero-copy path).

**Confirmed gap**: both `ContinuousROOTAxis.traits` and `DiscreteROOTAxis.traits` hardcode
`circular=False` unconditionally — never inspect ROOT's actual `TAxis` circularity. Tested only
for `TH1F`/`TH2F` (`tests/test_root.py`, requires real ROOT, run via `nox -s root_tests`) — no test
of `TProfile`, circular axes, or labeled axes against real ROOT.

This adapter is **plotting-oriented and read-only** — not a serialization backend, and it depends
on PyROOT running in the same process. It is not directly usable at a gRPC server boundary unless
PyROOT runs server-side too.

### 3.5 `tag.py` — a separate, non-Protocol indexing spec

`loc(value)`, `underflow`/`overflow` singletons, `at(value)`, `rebin(factor)` — the vocabulary for
`h[...]`/`h[...] = ...` on boost-histogram-family objects. Notably, `loc.__call__` requires
`axis.index()`, which is **not** part of `PlottableAxis` — an acknowledged inconsistency (there's a
literal `# TODO: clarify that .index() is required` comment in `tag.py`). This confirms indexing
semantics are a separate concern from the read-only plotting Protocol, and orthogonal to filling.

### 3.6 Conclusion on UHI's role

UHI is **not** a wire protocol candidate as software (no runtime dependency intended, Python-typing
based), but its **JSON Schema is a genuinely language-neutral, well-scoped candidate IR** for
histogram *shape* (axes + storage taxonomy) — small, closed, versioned, and already the reference
schema boost-histogram's own serialization targets. Its `PlottableHistogram` Protocol is the right
*conceptual* read-side contract (`axes`, `kind`, `values/variances/counts`) to present in Python
once a remote histogram is fetched back — but it is Python-specific and cannot itself be
"implemented" by a C++/gRPC service. Growable axes and name/title are explicitly unstandardized
gaps shared by UHI, ROOT's growable axes, and (as shown below) HistServ's own chunking model.

---

## 4. HistServ Architecture

Version `0.2.1`, classified pre-alpha (`"Development Status :: 1 - Planning"`). Dependencies
(`pyproject.toml`/`pixi.toml`): `grpcio`, `hist`, `boost-histogram`, `uhi>=1.0.0`, `numcodecs`,
`grpcio-tools`; optional `h5py` (HDF5 flush), `fastapi`/`uvicorn`/`httpx` (dashboard).

**UHI's actual role in HistServ today is narrow**: `chunked_hist.py` imports `uhi.io.json` only for
its `default` encoder (numpy-aware JSON fallback); `service.py`'s `Flush` RPC imports
`uhi.io.hdf5.write` only to dump a `hist.Hist` to an HDF5 file server-side. **UHI is not used as
the wire schema or as a typed abstraction layer** — the embedded metadata schema is built from
`boost_histogram.serialization._axis._axis_to_dict` / `_storage_to_dict` (boost-histogram's own
private serialization internals, which independently implement essentially the same axis/storage
taxonomy UHI's schema documents, since boost-histogram is one of UHI's reference-conformant
libraries) — `uhi.io.json.default` is used only as the string encoder for that dict.

### 4.1 The proto — `src/histserv/protos/hist.proto` (complete, 142 lines)

```proto
syntax = "proto3";
import "google/protobuf/timestamp.proto";

service HistogrammerService {
  rpc Init(InitRequest) returns (InitResponse) {}
  rpc Describe(DescribeRequest) returns (DescribeResponse) {}
  rpc Exists(ExistsRequest) returns (ExistsResponse) {}
  rpc WasFilledWithUniqueId(WasFilledWithUniqueIdRequest) returns (WasFilledWithUniqueIdResponse) {}
  rpc Fill(FillRequest) returns (FillResponse) {}
  rpc FillMany(FillManyRequest) returns (FillResponse) {}
  rpc Snapshot(SnapshotRequest) returns (SnapshotResponse) {}
  rpc Delete(DeleteRequest) returns (DeleteResponse) {}
  rpc Reset(ResetRequest) returns (ResetResponse) {}
  rpc Flush(FlushRequest) returns (FlushResponse) {}
  rpc Stats(StatsRequest) returns (StatsResponse) {}
}

message ChunkScalar { oneof value { string string_value = 1; int64 int_value = 2; } }
message ChunkPayload { repeated ChunkScalar chunk_key = 1; bytes dense_view = 2; }
message ChunkedHistPayload {
  string hist_json = 1;
  repeated ChunkPayload chunks = 2;
  optional string dense_view_codec = 3;
}
message InitRequest { ChunkedHistPayload payload = 1; }
message InitResponse { string hist_id = 1; }
message DescribeRequest { string hist_id = 1; }
message DescribeResponse { string hist_json = 1; }
message ExistsRequest { string hist_id = 1; }
message ExistsResponse { bool exists = 1; }
message WasFilledWithUniqueIdRequest { string hist_id = 1; bytes unique_id = 2; }
message WasFilledWithUniqueIdResponse { bool was_filled = 1; }
message FillRequest {
  string hist_id = 1;
  optional bytes unique_id = 2;
  repeated ChunkScalar chunk_key = 3;
  bytes dense_view = 4;
  optional string dense_view_codec = 5;
}
message FillResponse {}
message FillManyRequest {
  string hist_id = 1;
  optional bytes unique_id = 2;
  repeated ChunkPayload chunks = 3;
  optional string dense_view_codec = 4;
}
message ChunkSelector { string axis = 1; repeated ChunkScalar values = 2; }
message SnapshotRequest {
  string hist_id = 1;
  bool delete_from_server = 2;
  repeated ChunkSelector chunk_selectors = 3;
  optional string dense_view_codec = 4;
}
message SnapshotResponse { ChunkedHistPayload payload = 1; }
message DeleteRequest { string hist_id = 1; }
message DeleteResponse {}
message ResetRequest { string hist_id = 1; }
message ResetResponse {}
message FlushRequest { string hist_id = 1; string destination = 2; }
message FlushResponse {}
message StatsRequest {}
message StatsResponse {
  uint64 histogram_count = 1; uint64 histogram_bytes = 2; uint64 active_rpcs = 3;
  string version = 4; uint64 uptime_seconds = 5;
  double user_cpu_seconds = 6; double system_cpu_seconds = 7;
  map<string, uint64> rpc_calls_total = 8;
  google.protobuf.Timestamp observed_at = 9;
  TokenScopedStats token_scoped = 10;
}
message TokenScopedStats {
  uint64 histogram_count = 1; uint64 histogram_bytes = 2;
  map<string, uint64> rpc_calls_total = 3;
}
```

**All 11 RPCs are unary-unary** — zero streaming anywhere (`grep -n "stream"` across the proto and
generated `hist_pb2_grpc.py`: no matches). **No `Axis`/`Regular`/`Variable` protobuf messages exist**
— axis/storage schema is an opaque JSON string (`hist_json`) inside `ChunkedHistPayload`. **No raw
event coordinates or weights cross the wire at all** — the client always pre-bins locally into a
dense NumPy array first; the wire only ever carries `bytes dense_view` (already-binned) plus a
`chunk_key` (categorical scalar values, in native `oneof` fields). **No explicit Merge RPC** —
merging is accumulate-in-place inside `Fill`/`FillMany` handlers. Access control is a plain
`x-histserv-token` metadata-header string match (mismatch → `NOT_FOUND`, not a distinct
"forbidden" — a caller can't even tell an unauthorized histogram exists).

### 4.2 `ChunkedHist` (`chunked_hist.py`) — the server-side storage model

Purpose-built to let histograms with **growable categorical axes**
(`hist.axis.IntCategory`/`StrCategory`, `growth=True`) be filled incrementally across many
independent gRPC calls without knowing the full category set in advance. Splits axes into:
- **chunk axes**: growable `IntCategory`/`StrCategory` — tracked as a growing list of known keys.
- **dense axes**: `Regular`, `Boolean`, `Variable`, `Integer` — everything else.

Each unique combination of chunk-axis values maps to one fixed-shape dense NumPy array (over just
the dense axes, flow-inclusive: `view(flow=True)`). New categorical values grow the **key space**
(a dict key) without ever renegotiating array shape. `fill(**kwargs)` does *real* local binning via
a scratch `hist.Hist.fill()` (genuine boost-histogram semantics — weight handling, storage
accumulation); `add_dense_view(key, dense_view)` is the lower-level primitive the gRPC handlers
actually call — accumulate-in-place on an **already-binned** array, no `hist.Hill.fill()` involved
server-side at all. `to_hist()` rebuilds a real `hist.Hist`, explicitly **dropping the categorical
overflow bin (index -1)** — documented, lossy, and structurally identical to the "growth status
lost on round-trip" caveat UHI's own docs make for growable axes. `copy()`/`chunk_view_copy()`
produce detached copies explicitly for safe handoff to a worker thread while the live dict keeps
mutating on the event loop.

**Validation limits** (`_validate_supported_axis`/`_validate_supported_storage`): rejects `Regular`
axes with a `transform` set, and rejects `Mean`/`WeightedMean` storage — i.e. growable *Regular*
axes and profile-style storage are **not currently chunkable** (only fixed dense axes + growable
categorical axes + Int/Double/Weight storage).

### 4.3 `serialize.py` — the actual wire codec (not JSON end-to-end, not pickle)

```python
_ZSTD = numcodecs.Zstd(level=1); _LZ4 = numcodecs.LZ4(acceleration=1)
```

- Categorical chunk keys → native protobuf `oneof {string_value, int_value}` fields.
- Dense bin data → `np.asarray(array, order="C").tobytes()`, optionally passed through
  `numcodecs.Zstd`/`LZ4`. **Shape/dtype/byte-count are never carried per-message** — they're
  established once at `Init` time and stored server-side (`entry.hist.dense_view_shape` etc.), so
  wire fill messages are opaque byte blobs valid only in the context of a previously negotiated
  schema.
- `serialize_chunked_hist_payload`/`deserialize_chunked_hist_payload` round-trip the full
  `ChunkedHistPayload` (schema JSON + all stored chunk byte blobs) — this is the single entry
  point used by both `Init` and `snapshot()`.

**This tightly couples the wire format to Python/NumPy dtype layout, byte order, and C-order
memory layout on both ends.** A non-Python (e.g. C++/ROOT) client must reproduce this exact
byte-buffer contract — matching dtype sizes/endianness and the JSON schema shape — rather than
relying on native protobuf numeric fields for bin contents. **This is the single most important
finding for the C++ client design**: the contract is currently implicit and must be made explicit.

### 4.4 `service.py` — request trace and concurrency model

`Histogrammer(hist_pb2_grpc.HistogrammerServiceServicer)` is a `grpc.aio` (asyncio) servicer,
single in-process dict `self._entries: dict[str, HistogramEntry]` keyed by `uuid.uuid4().hex`,
shared across *all* connected clients. **Zero explicit locks anywhere** — safety relies entirely on
(a) mutating handlers (`Fill`, `FillMany`, `Reset`) executing synchronously with no `await` between
read and mutate, so concurrent coroutines can't interleave mid-mutation on one event loop, and
(b) `Snapshot`/`Flush` taking a synchronous atomic `.copy()` *before* hopping to `asyncio.to_thread`
for the expensive part — documented in code comments as a load-bearing invariant. There is **no
`ThreadPoolExecutor`/`max_workers` sizing anywhere** — everything is one asyncio loop, one core.
This is the central scalability limit identified in this report (§7).

Full per-RPC trace (`Fill` example): entry lookup+token check → idempotency check via
`unique_id`/`WasFilledWithUniqueId` (SHA-256 hash of the JSON-canonicalized key, one-way) →
deserialize chunk key + dense bytes against the entry's stored shape/dtype → `add_dense_view`
(accumulate-in-place) → empty `FillResponse()`. `FillMany` does the same for a `repeated
ChunkPayload` list in one RPC. `Snapshot` supports full copy, delete-after-snapshot, or
chunk-selector partial retrieval (`entry.hist[selection]`).

### 4.5 `client.py` — existing Python client, and a full traced example

```python
Client(address).init(hist: Hist) -> RemoteHist       # ChunkedHist.from_hist() -> Init RPC
RemoteHist.fill(**kwargs) -> FillResponse             # local hist.Hist.fill() -> Fill RPC
RemoteHist.fill_many(fills) -> FillResponse           # groups by chunk key -> ONE FillMany RPC
RemoteHist.snapshot(delete_from_server=False) -> ChunkedHist
RemoteHist.flush(destination) / .delete() / .reset() / .exists() / .was_filled_with_unique_id()
Client.stats(token=...)
```

`channel`/`stub` are lazy `cached_property`s; `Client.__getstate__` strips both, making `Client`
(and any `RemoteHist` it produced) **pickle-safe for distributed/dask/coffea workers** — proven by
`example/coffea_processor.py`, which round-trips a `Processor` (holding a live `Client`+`RemoteHist`)
through `pickle.dumps`/`loads` before use, and fills the same remote histogram from a worker's
`process()` method across repeated 1,000,000-event batches. **This is the strongest evidence in the
repo for the intended production shape**: many distributed workers, each holding one long-lived
`RemoteHist` handle, each issuing few large batched fills — not one RPC per event.

Traced round trip (matches the README quickstart and `test_remote_fill_matches_local_hist_for_regular_axes`):
```python
H_local = Hist.new.Reg(30, -3, 3, name="x").Double()
with Client(address="[::]:50051") as client:
    H_remote = client.init(H_local)                          # Init RPC
    H_remote.fill(x=np.random.normal(size=1000))              # local bin -> Fill RPC
    H_snapshot = H_remote.snapshot(delete_from_server=True)   # Snapshot RPC -> ChunkedHist
    print(H_snapshot.to_hist())                                # -> real hist.Hist
```

`fill_many()` groups logical per-call fills by resulting chunk key **client-side before
serializing**, proven by `tests/test_client.py::test_fill_many_sends_one_chunk_per_distinct_key`
(4 logical fills over 2 distinct keys → exactly 1 RPC, 2 chunk payloads).

### 4.6 Tests — exact evidence of intended semantics

`tests/test_grpc_integration.py` runs against a **real socket-connected server** (no mocks,
`tests/conftest.py`'s `GrpcServerThread`). Confirmed by direct assertion:
- `test_remote_fill_matches_local_hist_for_regular_axes` — sequential single `fill()` RPCs, then
  `np.testing.assert_equal(remote_snapshot.to_hist().view(flow=True), local_hist.view(flow=True))`.
- `test_remote_fill_many_matches_local_hist_for_regular_axes` — same assertion via one `fill_many()`
  call, proving `Fill`/`FillMany` are numerically interchangeable.
- `test_remote_fill_matches_local_hist_for_weighted_categorical_axes` — parametrized over 1D/2D/3D
  categorical+weighted cases, including a case where a fill introduces a **brand-new** category
  value never in the original axis (growth-via-remote-fill), verified against the local reference.
- `test_remote_fill_reject_with_duped_unique_id` — idempotency: replaying the same `unique_id`
  raises `grpc.RpcError` `ALREADY_EXISTS`.
- `test_remote_slice_snapshot_returns_only_selected_chunks` — partial retrieval by chunk selector.
- `test_snapshot_delete_from_server_removes_hist`, `test_flush_writes_hdf5_file_and_removes_hist`.
- Compression parametrized over `[None, "zstd", "lz4"]` for every fill/snapshot RPC, always checked
  against an uncompressed local reference.

`example/client.py` explicitly demonstrates **axis growth via remote fill** with a category value
(`dataset="ttbar"`) never present in the original histogram definition.

### 4.7 C++ client status

**Confirmed: none exists.** Repo-wide search for `*.cpp/*.cc/*.hpp/*.h/CMakeLists.txt` returns zero
results; no generated `.pb.cc`/`.grpc.pb.cc` files; only Python codegen is documented
(`python -m grpc_tools.protoc ... --python_out ... --grpc_python_out ...`, from `README.md`).
Nothing in the plain-`proto3` `.proto` file prevents C++ codegen — it's simply never been done.

---

## 5. Mapping: ROOT ↔ UHI ↔ HistServ

| Concept | ROOT (classic TH1 / RHist v7) | UHI | HistServ |
|---|---|---|---|
| Fixed-width axis | `TAxis` (`fNbins,fXmin,fXmax`) | `regular` axis schema | dense axis in `ChunkedHist`, built from real `hist.axis.Regular` |
| Variable-width axis | `TAxis` with `fXbins` (`TArrayD`) | `variable` axis schema (`edges`) | supported as a dense axis (rejects only `Regular` with a `transform`) |
| Categorical/labeled axis | `TAxis::SetBinLabel` overlay (heuristically detected by UHI's `ROOTAxis.create`) | `category_str`/`category_int` schema | **first-class**: `ChunkedHist`'s growable chunk axes are exactly `IntCategory`/`StrCategory` |
| Growable axis | `TH1::SetCanExtend` + `ExtendAxis` (rebins Regular axis in place) | not representable — docs admit "growth status will be lost" on round-trip | growable **categorical only** (via chunk-key growth); growable *Regular* axes explicitly rejected by `_validate_supported_axis` |
| Weighted storage / errors | `Sumw2()` → `fSumw2` (`TArrayD`), tri-state | `weighted` storage (`values`,`variances`) | `bh.storage.Weight()`, structured dtype `{value,variance}`, accumulated per-field in `_accumulate_dense_view` — direct match |
| Profile / mean storage | `TProfile` (not covered by classic `TH1`) | `mean`/`weighted_mean` storage | **not supported** — `_validate_supported_storage` explicitly rejects `Mean`/`WeightedMean` |
| Underflow/overflow | flow bins baked into `fNcells`; independent per axis | `underflow`/`overflow`/`flow` booleans per axis | dense arrays are always `view(flow=True)`; **categorical overflow bin (-1) is silently dropped** on `from_hist()`/`to_hist()` — a real, documented fidelity gap |
| Multi-dim indexing | `TH2::Fill` linear index with flow-inclusive stride | `axes: Sequence[PlottableAxis]`, arbitrary length | `ChunkedHist` splits axes into chunk/dense sets; dense sub-array indexing follows boost-histogram/`hist` conventions |
| Merge | `TH1::Merge`/`TH1Merger` — general, handles differing ranges/labels | not modeled | accumulate-in-place only (`+=`), **requires bit-identical axes/storage** — far narrower than `TH1::Merge` |
| Name/title | `TNamed` fields | not standardized — metadata-only, explicitly due to "lack of support from boost-histogram" | `hist_id` is a server-generated UUID; `hist.Hist` name/label live inside the opaque metadata JSON like UHI |
| Circular axis | `TAxis` circularity flag (real, e.g. for φ) | `PlottableTraits.circular` exists in Protocol+schema | not addressed at all in `hist.proto`/`ChunkedHist`; and UHI's own shipped ROOT adapter hardcodes `circular=False` regardless of the real flag |
| Full-histogram JSON | `TBufferJSON::ToJSON`/`FromJSON` (generic reflection) | `histogram.schema.json` (draft-07) | **HistServ's `hist_json` field is built from `boost_histogram.serialization` internals — a different producer of essentially the same UHI-shaped taxonomy**, not `TBufferJSON` and not `uhi.io.json`'s schema builder directly |
| Bulk numeric payload | binary `TBuffer` (ROOT I/O) | inline JSON array, or `.npy` (zip), or HDF5 dataset | **raw `ndarray.tobytes()` in a protobuf `bytes` field, optionally zstd/lz4-compressed** — HistServ effectively adds a 4th, most-compact option beyond UHI's three |
| Read-side Python object | n/a | `PlottableHistogram` (`axes`,`kind`,`values()`,`variances()`,`counts()`) | `RemoteHist.snapshot().to_hist()` returns a real `hist.Hist`, which **already satisfies `PlottableHistogram`** — no adapter needed |
| ROOT read-side object | live `TH1`/`TH2`/`TProfile` | `ROOTPlottableHistogram`/`ROOTPlottableProfile` (PyROOT, zero-copy for `TH1`, per-cell for `TProfile`) | not connected — HistServ never touches PyROOT |

**Key synthesis point**: HistServ did not adopt UHI's JSON serialization wholesale. It made a
deliberate (and sound) engineering split: **UHI-shaped JSON for the small, infrequently-changing
axis/storage *schema*, and raw NumPy bytes for the large, frequently-sent *bulk numeric payload*.**
That split is exactly the right one to preserve for a C++/ROOT client — a C++ client should parse
the small JSON schema (trivial with any JSON library) and match the byte-buffer contract for bulk
data (the part that currently has zero language-neutral documentation and is the one real gap).

---

## 6. Candidate Wire Protocols — Evaluated Against What Already Exists

**A. Individual fills, one RPC per `Fill(x)` call.** This is what `hist.proto`'s `Fill` RPC *is* at
the RPC-count level per Python call — but because the Python client already pre-bins a whole numpy
array locally before sending, one Python `RemoteHist.fill(x=big_array)` call is **one RPC for
however many events are in that array**, not one RPC per event. The real risk case is existing
**C++ ROOT code** that calls `h->Fill(x)` once per event inside a tight loop (the classic,
extremely common ROOT idiom) — mapping that 1:1 to gRPC calls would be the literal "billions of
RPCs" scenario the brief warns about, and is architecturally incompatible with HEP throughput
requirements. This is a reason a C++ client needs its own client-side buffering (mirroring what the
Python client gets for free from vectorized `numpy` input), not a reason to change the protocol.

**B. Batch fills.** Already implemented and tested: `FillMany` (`repeated ChunkPayload` per RPC) and
client-side chunk-key grouping in `RemoteHist.fill_many()`. This is the shipped, recommended shape.

**C. Send already-binned data (`bin += weight`) instead of `Fill(x)`.** **HistServ already made
this choice at the wire level** — `dense_view` is always pre-binned bytes; the server's
`add_dense_view` never calls anything resembling `hist.Hist.fill()`. Binning happens **client-side**
using real `hist.Hist.fill()` semantics (weights, storage, edges) before serialization. For a C++
client, this is very good news: **Boost.Histogram is a real, header-only C++ template library**
(the Python `boost_histogram`/`hist` packages are bindings over it), so a C++ client can link it
directly and reproduce *byte-identical* binning/accumulation logic without reimplementing binning
math from scratch or going through Python at all.

**D. Send serialized histogram state, periodically, and merge server-side.** ROOT already has two
full serialization mechanisms for this (`TH1::Streamer`, `TBufferJSON`) and a real general-purpose
merge (`TH1::Merge`/`TH1Merger`), and RHist v7 has `RHistStats::AddAtomic`. HistServ's own
`add_dense_view` accumulate-in-place is a **narrow instance** of this pattern (identical
axes/storage/shape only — no rebinning, no differing ranges) — this is adequate for the common case
(all workers fill the histogram they were handed, unmodified) but is not as general as
`TH1::Merge`. No wire-format changes are implied by this option; it describes *when* to serialize
(periodically vs. per-event), which the client already controls freely via how large a local buffer
it accumulates before calling `fill()`/`fill_many()`.

**E. Hybrid (`Create`/`FillBatch`/`Merge`/`Get`).** **This is what `hist.proto` already is**:
`Init`≈Create, `Fill`/`FillMany`≈FillBatch, accumulate-in-place≈implicit Merge, `Describe`/
`Snapshot`≈Get — plus production-grade additions the brief's sketch didn't anticipate:
per-fill idempotency (`unique_id`/`WasFilledWithUniqueId`, SHA-256-hashed, `ALREADY_EXISTS` on
replay — essential for safe retries under gRPC's at-least-once unary-call semantics), partial
retrieval (`chunk_selectors`), lifecycle ops (`Exists`/`Delete`/`Reset`), and observability
(`Stats`). **Recommendation: do not invent a new protocol. Reuse `hist.proto` as-is.**

---

## 7. Recommended Protocol and What's Actually Missing

**Recommendation: adopt the existing `HistogrammerService` (`hist.proto`) unchanged as the wire
protocol.** It already embodies the best-supported design point (C: pre-binned dense batches, via
E: a create/fill/snapshot lifecycle with idempotency), it is tested end-to-end against a real
socket-connected server, and — critically — since `hist.proto` is plain `proto3` with no
language-specific options, a C++ client requires **zero server or `.proto` changes**, only codegen.

What is genuinely missing, and needs to be *documented*, not *invented*:

1. **The dense-buffer byte-layout contract is implicit and Python/NumPy-native.** `shape`, `dtype`,
   and `expected_nbytes` are never sent on the wire — they live in server state established at
   `Init`. A second-language client must independently derive the identical shape/dtype/byte-order
   from the same `hist_json` schema string the Python side would produce. This needs a short,
   explicit spec (dtype-name ↔ C++ type mapping, C-order convention, `Weight()`'s structured
   `{value, variance}` pair layout) — a documentation artifact, not a protocol change.
2. **Growable Regular axes and Mean/WeightedMean (profile) storage are not chunkable today**
   (`_validate_supported_axis`/`_validate_supported_storage` explicitly reject them). If a PoC needs
   ROOT-style dynamic-range growth or `TProfile`-equivalent behavior, this is a real HistServ-side
   gap, not just a documentation gap — but it is avoidable for a first PoC by using fixed-range
   axes and `Weight()`/plain-count storage only (which covers the large majority of HEP histogram
   use cases).
3. **No native protobuf `Axis`/`Storage` messages** — schema travels as an opaque string. This is a
   nice-to-have for stronger cross-language type safety later, not a blocker (a C++ client can
   parse the JSON string with any JSON library in a few lines).
4. **No TLS, weak auth (plain string match), no per-histogram locking beyond the single-event-loop
   invariant.** Fine for a same-host/LAN PoC; flagged for anyone taking this toward production.

---

## 8. Python PoC Design

**This PoC essentially already exists.** `histserv/example/client.py` and
`histserv/tests/test_grpc_integration.py` already implement exactly the "local histogram, remote
histogram, identical input, compare" pattern requested. The PoC work is to **assemble and extend**
what's there into a standalone demonstration script, not to design new API:

```python
import numpy as np
from hist import Hist
from histserv import Client

rng = np.random.default_rng(0)
x = rng.normal(size=1_000_000)

local = Hist.new.Reg(50, -5, 5, name="x").Weight()
local.fill(x=x, weight=np.ones_like(x))

with Client(address="[::]:50051") as client:
    remote = client.init(Hist.new.Reg(50, -5, 5, name="x").Weight())
    remote.fill(x=x, weight=np.ones_like(x))          # or fill_many() over sub-batches
    result = remote.snapshot(delete_from_server=True).to_hist()

np.testing.assert_allclose(result.view(flow=True)["value"], local.view(flow=True)["value"])
np.testing.assert_allclose(result.view(flow=True)["variance"], local.view(flow=True)["variance"])
```

Extend this single pattern (no new server/client code required) to cover the brief's checklist —
each is already exercisable with the existing service:
- **Weighted fills**: `storage=hist.storage.Weight()` + `weight=` kwarg (proven by
  `test_remote_fill_matches_local_hist_for_weighted_categorical_axes`).
- **Underflow/overflow**: feed values outside `[lower, upper)`; compare `.view(flow=True)`.
- **Variable bins**: `hist.axis.Variable([...])` as a dense axis — already supported.
- **2D histogram**: two `Regular` axes; `ChunkedHist` handles arbitrary dense-axis combinations.
- **Categorical growth**: fill with a category value not in the original axis (per
  `example/client.py`'s `dataset="ttbar"` pattern) to exercise HistServ's one genuinely novel
  feature relative to plain boost-histogram.

The only *new* code worth writing for Phase 1/2 is the benchmark harness (batch-size vs.
throughput/RPC-count, compression on/off) — see §10 and §13.

---

## 9. C++ PoC Design

### 9.1 What NOT to build, and why

**Do not write `class RemoteTH1D : public TH1D`.** The ROOT archaeology is unambiguous: `TH1::Fill`
writes moment sums (`fTsumw*`) directly, not through any virtual hook; `AddBinContent`/
`UpdateBinContent`/`RetrieveBinContent` are virtual but *only* used for the per-storage-type
`fArray` access, not for the many other methods (`GetMean`, `Draw`, `Fit`, I/O) that read `fArray`/
`fSumw2`/`fTsumw*` directly. Even a subclass that carefully overrides every `Fill` overload
(`Fill(x)`, `Fill(x,w)` — note `TH2::Fill(x,y)` is a *different, non-virtual-override* method due
to signature hiding, not overriding) still leaves `GetBinContent`, `Draw`, `Fit`, and ROOT's own
I/O reading stale/empty local `fArray`, since the real data lives remotely. **There is no single
seam that makes classic `TH1` transparently proxyable.** This matches the ROOT archaeology's
explicit conclusion.

### 9.2 What to build instead — mirror the already-proven Python client shape

The Python client (`Client`/`RemoteHist`) is the validated reference design. Build the C++
equivalent as a **standalone class hierarchy that does not inherit from `TH1`/`RHist` at all**:

```cpp
class RemoteHist {
public:
    static RemoteHist Init(Client& client, /* axis spec */, const std::string& token = "");
    static RemoteHist Connect(Client& client, const std::string& hist_id, const std::string& token = "");

    void Fill(double x);                       // buffers locally
    void Fill(double x, double weight);
    void Fill(double x, double y, double weight = 1.0);
    void FlushBuffer();                        // sends one FillMany RPC for everything buffered

    ChunkedHist Snapshot(bool delete_from_server = false);
    std::unique_ptr<TH1D> ToTH1D() const;       // materializes a REAL ROOT histogram
    std::unique_ptr<TH2D> ToTH2D() const;
};
```

`Fill()` accumulates into a **local Boost.Histogram instance** (`boost::histogram::histogram<...>`
— the real C++ template library underneath `boost_histogram`/`hist`), guaranteeing bit-identical
binning/weight/storage semantics to the Python client's `hist.Hist.fill()`, with no Python
involved. `FlushBuffer()` serializes that local histogram's dense storage to bytes using the
**same byte-layout contract** as `serialize.py` (documented per §7.1) and calls the generated C++
gRPC stub's `Fill`/`FillMany`. `ToTH1D()`/`ToTH2D()` convert a `Snapshot()` result into a real ROOT
histogram via `SetBinContent`/`SetBinError`/`Sumw2()` — the client-side mirror of what Python's
`ChunkedHist.to_hist()` already does, adapted to ROOT instead of `hist.Hist`.

### 9.3 Can `TH1D *h; h->Fill(x);` be transparently redirected?

**No, not without call-site changes**, for the same reason inheritance doesn't work: there is no
polymorphic seam in classic `TH1` that "the rest of ROOT" (Draw, Fit, I/O, GetMean) respects. Code
that holds a real `TH1D*` and calls `->Fill()` cannot be silently redirected to a remote backend
today. The pragmatic migration path is: swap the *declaration* at the call site
(`TH1D*` → `RemoteHist*`, a deliberately narrower Fill-focused type) for the specific histograms
being offloaded, and call `ToTH1D()` only at the point where full `TH1` functionality (plotting,
fitting) is actually needed — not a transparent drop-in, but a small, mechanical, greppable change
per histogram.

### 9.4 Build requirements (all additive, zero HistServ/proto changes)

```
protoc -I histserv/src/histserv/protos \
    --cpp_out=<out> --grpc_out=<out> \
    --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
    histserv/src/histserv/protos/hist.proto
```
This has never been run in this repo (§4.7) but the `.proto` is ordinary `proto3` — no obstacles.
Link against: generated `hist.pb.{h,cc}`/`hist.grpc.pb.{h,cc}`, gRPC C++ runtime, Boost.Histogram
(header-only), a JSON library (e.g. `nlohmann::json`, or ROOT's own JSON facilities) for the small
schema string, and ROOT (`libHist`) only for the `ToTH1D`/`ToTH2D` conversion layer.

---

## 10. ROOT Integration Options — Summary Table

| Option | Verdict | Why |
|---|---|---|
| `class RemoteTH1D : public TH1D` | **Rejected** | `Fill()` writes moments directly (not virtual-hook-mediated); many methods bypass virtuals to touch `fArray`/`fSumw2` directly; `TH2::Fill(x,y)` hides rather than overrides `TH1::Fill(x)` — no consistent seam |
| Standalone `RemoteHist` + explicit `ToTH1D()`/`ToTH2D()` conversion | **Recommended (near-term)** | Mirrors the already-proven Python `Client`/`RemoteHist`/`.to_hist()` design; zero ROOT/HistServ changes; requires call-site edits but those are mechanical and localized |
| Storage-strategy template hook on RHist v7's `RHistEngine` | **Correct long-term shape, not near-term** | Engine/Stats separation is architecturally right, but `RHistEngine` is `final`/concrete/non-pluggable today, serialization is explicitly disabled (`Streamer` throws), and the whole stack is pre-1.0 ("will change without notice") — a ROOT research contribution, not a PoC dependency |
| Modify classic `TH1` to add a storage-abstraction virtual base | **Not recommended** | Would require touching a huge, stable, widely-depended-on class; the RHist v7 line already exists as ROOT's designated place for this kind of experimentation |

---

## 11. Performance Considerations

Grounded in the concrete mechanisms found (not generic claims):

- **One RPC per event is unambiguously unacceptable** for HEP scale. gRPC unary round-trip latency
  is roughly **0.1–1 ms on localhost/LAN**, dominated by event-loop scheduling + (de)serialization
  overhead, not raw network time; WAN adds tens of ms more. At even an optimistic 0.2 ms/RPC:
  10⁶ fills ≈ 200 s of pure RPC overhead; 10⁹ fills ≈ 55 hours — before any actual binning work.
  This is the exact scenario the brief warns about, and it is also the exact scenario the existing
  Python client architecture (local-bin-then-batch) already avoids by construction.
- **Client-side pre-binning + batching (what HistServ already does) changes the bottleneck
  entirely.** Local vectorized binning (`hist.Hist.fill()` / Boost.Histogram) processes on the
  order of **10⁷–10⁸ entries/sec** on a single core. Serialization is a `tobytes()`
  memcpy — microseconds for MB-sized arrays. A 1D histogram with 1,000 `double` bins is 8 KB
  uncompressed; a 2D 100×100 `Weight()`-storage histogram is ~160 KB — trivial to send even
  uncompressed. **RPC count, not RPC cost, is what must be minimized**, and it already is, via
  `fill_many()`'s chunk-key grouping and simply calling `fill()` on large arrays rather than
  per-event.
- **Compression** (`numcodecs.Zstd(level=1)`/`LZ4(acceleration=1)`) is tuned for **speed over
  ratio** — appropriate, since dense payloads are typically not highly compressible after many
  fills (most bins are non-zero), and the goal is to avoid adding CPU-bound latency, not to
  minimize bytes at all cost. Tests parametrize all compression modes against uncompressed local
  references, confirming correctness is compression-independent.
- **Concurrency ceiling**: the server runs **one asyncio event loop, zero explicit locks, no
  `ThreadPoolExecutor`**. All mutating RPCs across *all* histograms and *all* clients are
  serialized through one core. For a realistic HEP workload with many concurrent distributed
  workers (as `example/coffea_processor.py` demonstrates is the intended usage — many pickled
  `Processor`s, each with its own `RemoteHist`, running on separate dask/coffea workers), this
  single-core ceiling is the actual scaling bottleneck, **not** gRPC/serialization overhead.
  Horizontal scaling would require sharding histograms across multiple `HistServ` server processes
  (no built-in clustering exists) — a real, identified limitation worth benchmarking explicitly
  (§13, Phase 6) rather than assuming away.
- **Local-fill-then-periodic-merge** (design D) has an equivalent cost profile to explicit batching
  — it's the same mechanism with a time-based rather than count-based flush trigger, and it is
  exactly what `RemoteHist` already provides if a caller simply chooses a batch/flush cadence.
- **WAN vs. LAN**: nothing in HistServ's design assumes LAN specifically (no shared-memory
  assumptions), but the current single-core, no-TLS, no-retry-hardened server means WAN operation
  primarily changes RPC latency (relevant mainly if batch sizes are small or many round trips are
  chatty, e.g. many small `Snapshot` polls) rather than data-path correctness.

---

## 12. Required Changes by Repository

**Existing functionality usable unchanged:**
- HistServ's entire proto/service/client/`ChunkedHist` stack — proven correct by real-socket
  integration tests; this *is* the Python PoC's backend already.
- `hist`/`boost-histogram`'s Python fill semantics (used identically on both local-reference and
  remote-pre-binning sides).
- UHI's `numpy_plottable.ensure_plottable_histogram` + `ROOTPlottableHistogram`/
  `ROOTPlottableProfile` for turning a *live local* ROOT object into a `values()/variances()`
  array-bearing object (read-side interop, already tested against real ROOT for `TH1F`/`TH2F`).
- `TBufferJSON::ToJSON`/`FromJSON` if ROOT-native JSON round-tripping is ever wanted instead of the
  UHI-shaped schema (independent, already proven mechanism, not currently used by HistServ).
- Boost.Histogram's C++ core, directly linkable from a C++ client for identical binning logic.

**Small adapters (buildable entirely outside ROOT/HistServ, no core changes):**
- A short written spec for the `dense_view` byte-layout contract (dtype↔C++ type map, C-order,
  `Weight()`'s structured-field layout) — currently implicit/Python-native (§7.1).
- The C++ `RemoteHist` client class (§9.2) — new code, but purely additive, using generated stubs.
- `ToTH1D()`/`ToTH2D()` conversion helpers (C++ mirror of `ChunkedHist.to_hist()`), and, if desired,
  a Python convenience wrapper sugar-coating `Client`/`RemoteHist` into the `RemoteHistogram(...)`
  shape sketched in the brief — optional, since the existing client API already covers the PoC.

**HistServ changes — none required for a first PoC; optional later improvements:**
- Document the dense-buffer byte contract explicitly (a docs change, arguably belongs *in* this
  repo even though it requires no code change).
- Support growable `Regular` axes and `Mean`/`WeightedMean` storage in `ChunkedHist`, if
  ROOT-style dynamic ranges or profile histograms are needed beyond the PoC.
- Native protobuf `Axis`/`Storage` messages instead of an opaque JSON string (stronger typing,
  not required for correctness).
- TLS, stronger auth, and a path to horizontal scaling (sharding across processes) before any
  production, multi-worker HEP deployment.

**UHI changes — none required.** UHI is correctly used here as documentation-shape prior art
(its axis/storage taxonomy is effectively what `boost_histogram.serialization` and hence
`ChunkedHist`'s metadata independently implement) and as the read-side Python contract
(`hist.Hist` returned by `ChunkedHist.to_hist()` already satisfies `PlottableHistogram`).

**ROOT changes — none required for the PoC.** If, later, genuine architectural integration is
desired, the smallest upstreamable hook is a storage-strategy template parameter on RHist v7's
`RHistEngine` — explicitly a ROOT-side research contribution given RHist v7's prototype status, not
a dependency of this PoC.

---

## 13. Proposed Architecture

```
                     ROOT / C++ analysis code                    Python analysis (hist / boost-histogram)
                     TH1D *h; h->Fill(x);  (existing,                       h.fill(x)  (existing,
                     unchanged call sites elsewhere)                        unchanged elsewhere)
                              |                                              |
                    [NEW] C++ RemoteHist client               [EXISTING] histserv.Client / RemoteHist
                    local pre-bin via Boost.Histogram          local pre-bin via real hist.Hist.fill()
                    (bit-identical semantics to Python)                     |
                              |                                              |
                              +------------------+      +--------------------+
                                                  |      |
                                        same wire contract:
                                   ChunkedHistPayload { hist_json (UHI-shaped
                                   axis/storage schema, small) +
                                   dense_view (raw NumPy-layout bytes,
                                   optional zstd/lz4) }
                                                  |
                                          gRPC (HistogrammerService,
                                          Init / Fill / FillMany / Snapshot / ...
                                          — EXISTING hist.proto, unchanged)
                                                  |
                                     HistServ server (service.py, EXISTING)
                                     single asyncio loop, in-memory ChunkedHist
                                     store, accumulate-in-place, token auth
                                                  |
                                          Snapshot -> ChunkedHistPayload
                                                  |
                              +-------------------+-------------------+
                              |                                       |
                 [NEW] C++ deserializer                  [EXISTING] ChunkedHist.to_hist()
                 -> TH1D::SetBinContent/                  -> real hist.Hist
                    SetBinError/Sumw2()                   (already PlottableHistogram-conformant)
                              |                                       |
                    real ROOT TH1D/TH2D                    real hist.Hist, usable with
                    (Draw/Fit/etc. work normally)           UHI-consuming plotters, mplhep, etc.
                                                                       |
                                                  (optional) uhi.numpy_plottable adapters
                                                  for a live *local* ROOT object read-side,
                                                  independent of the HistServ path
```

The diagram differs from the brief's sketch in one important way: **"batching" and "serialization"
are not two parallel boxes feeding one gRPC call** — they are the *same* client-side step (local
pre-binning *is* the batching mechanism; there is nothing to batch except already-binned dense
arrays), and HistServ already implements exactly this on the Python side. The work is adding the
matching C++-side box, not redesigning the pipeline shape.

---

## 14. Concrete Implementation Plan

**Phase 1 — Python minimal PoC.** Mostly assembly, not invention: adapt
`tests/test_grpc_integration.py::test_remote_fill_matches_local_hist_for_regular_axes` and
`example/client.py` into one standalone script covering: 1D regular, weighted, underflow/overflow
(out-of-range fills), variable-bin (`hist.axis.Variable`), and 2D — each compared against a locally
filled `hist.Hist` with identical (seeded) input, per §8. No new server/client code required.

**Phase 2 — Python batching/throughput measurement.** Benchmark: single large `fill()` call vs.
many small `fill()` calls vs. `fill_many()` at batch sizes 10³–10⁶ events; measure wall-clock, RPC
count, and payload size with compression off/`zstd`/`lz4`. Pure measurement against the existing
`Client`/`RemoteHist` API — no new server features.

**Phase 3 — C++ client.** (a) `protoc --cpp_out --grpc_out` codegen from the existing, unmodified
`hist.proto`; (b) write the byte-layout spec (§7.1); (c) implement `RemoteHist` (§9.2) using
Boost.Histogram C++ for local pre-binning; (d) validate with a **cross-language golden test**: fill
remotely from C++, read back from Python (or vice versa), and assert equality against a Python
local reference — the single highest-value validation this PoC can produce, since it directly
proves the byte-contract documentation is correct.

**Phase 4 — ROOT adapter.** Implement `ToTH1D()`/`ToTH2D()` (§9.2) using `SetBinContent`/
`SetBinError`/`Sumw2()`. Explicitly do **not** attempt `RemoteTH1D : public TH1D` (§9.1/§10).
Document the call-site migration pattern (§9.3) with a small before/after example. Optionally spin
off a clearly-labeled *speculative* research note on an `RHistEngine` storage-strategy hook for
ROOT's own consideration — separate from, and not blocking, this PoC.

**Phase 5 — serialization/merge demonstration.** Retrieve one remote histogram as (a) a real ROOT
`TH1D` via the Phase 4 converter and (b) a real Python `hist.Hist` via the existing
`ChunkedHist.to_hist()`, and cross-check both against a locally-filled ROOT histogram read through
UHI's existing `ensure_plottable_histogram`/`ROOTPlottableHistogram` adapter — closing the loop
ROOT ⇄ HistServ ⇄ Python/UHI with three independent code paths agreeing.

**Phase 6 — realistic HEP workload benchmark.** Extend `example/coffea_processor.py`'s pattern
(many pickled `Processor`s, each with one long-lived `RemoteHist`, each filling 10⁶+ events per
worker call) to multiple concurrent workers, and benchmark end-to-end wall-clock/CPU against an
equivalent fully-local ROOT/`hist` fill — explicitly measuring the single-asyncio-loop concurrency
ceiling identified in §11 as a function of worker count, not just per-worker throughput.

---

## Appendix: Notable Fidelity Gaps Found (carry into any protocol/spec work)

- Categorical axis overflow bin (index -1) is silently dropped by `ChunkedHist.from_hist()`/
  `to_hist()` (`chunked_hist.py`).
- Growable-axis status is lost on UHI JSON round-trip generally (`docs/serialization.md`), and
  HistServ only supports growable *categorical* axes, not growable *Regular* axes.
- Circular axes are representable in UHI's schema/Protocol but the shipped ROOT adapter
  (`numpy_plottable.py`) hardcodes `circular=False` regardless of the real ROOT flag — and
  `hist.proto`/`ChunkedHist` don't address circularity at all.
- Name/title are unstandardized across all three layers — ROOT's `TNamed`, UHI's
  not-a-schema-field convention, and HistServ's UUID-keyed `hist_id` with name/label buried in
  opaque metadata JSON, are three independent, non-unified conventions.
- `TH1::Merge` (differing ranges, rebinning, labels) is strictly more general than HistServ's
  accumulate-in-place merge (identical shape/dtype only) — worth keeping in mind if a future
  requirement needs merging histograms that weren't defined identically.
