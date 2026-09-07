#pragma once

class TH1;
#include "hist.pb.h"

namespace histserv_client {

// Converts an already-filled ROOT TH1 into the ChunkedHistPayload HistServ's
// wire protocol expects: a small JSON schema string (axis + storage shape,
// matching the boost-histogram/UHI axis taxonomy HistServ's ChunkedHist
// already deserializes) plus one dense byte chunk carrying the bin contents
// (and, if Sumw2 is active, variances) exactly as they currently stand on h.
//
// No binning happens here and no incremental remote state is tracked -- h is
// assumed to already be fully filled locally (the fast, normal ROOT way);
// this only packages its current state for one RPC.
//
// 1D, 2D, and 3D histograms are supported (TH1/TH2/TH3 and their C/S/I/F/D
// variants), each with either fixed-width or variable-width binning per
// axis. Throws std::invalid_argument for anything else (e.g. THnSparse,
// profile histograms).
ChunkedHistPayload SerializeTH1(const TH1* h);

} // namespace histserv_client
