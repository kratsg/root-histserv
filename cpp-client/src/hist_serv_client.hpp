#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include <grpcpp/grpcpp.h>

#include "hist.grpc.pb.h"

class TH1;

namespace histserv_client {

// Thrown on any failed RPC. Carries the raw grpc::StatusCode alongside the
// message so callers can distinguish e.g. ALREADY_EXISTS (a rejected
// duplicate unique_id) from a transport-level failure without string-matching
// what() -- see grpc::StatusCode for the full list of values `code` can take.
class HistServError : public std::runtime_error {
public:
    HistServError(grpc::StatusCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    grpc::StatusCode code() const { return code_; }

private:
    grpc::StatusCode code_;
};

// Standalone HistServ client whose only job is to take an ALREADY-FILLED
// ROOT TH1/TH2/TH3 and ship its current state to the server in one RPC.
// There is deliberately no per-event remote Fill() here, no local
// incremental buffering, and no rebinning logic -- ROOT already fills
// histograms fast and correctly on its own; this class only serializes (see
// root_hist_serialize.hpp) and sends the result.
//
// Two limitations are inherited directly from HistServ's own wire protocol
// and are NOT worked around here (doing so would mean inventing a new
// mechanism, which was explicitly out of scope):
//
//   - Init() has no idempotency protection: hist.proto's InitRequest carries
//     no unique_id field (unlike FillRequest/FillManyRequest). If a response
//     is lost after the server has already created the histogram (a normal
//     gRPC at-least-once failure mode), retrying Init() creates a SECOND,
//     fully duplicated remote histogram. There is no way to detect this from
//     the client side alone.
//
//   - Fill() sends h's CURRENT, full state, not a delta since the last call.
//     Call it once per already-finished, independent contributing histogram
//     (e.g. once at the end of each job) so HistServ's accumulate-in-place
//     merge sums independent contributions correctly. Calling Fill()
//     repeatedly against the SAME TH1* as it keeps accumulating more entries
//     over time will double-count everything sent in a previous call --
//     `unique_id` only rejects a byte-for-byte identical retry, it does not
//     detect or subtract out what a growing histogram already sent before.
class HistServClient {
public:
    explicit HistServClient(std::shared_ptr<grpc::Channel> channel);

    // Registers a brand-new remote histogram, seeded with h's current bin
    // (and, if Sumw2 is active, variance) contents. Returns the
    // server-assigned hist_id. See the class docs above: NOT idempotent.
    std::string Init(const TH1* h, const std::string& token = "");

    // Sends h's CURRENT state as one FillMany RPC, which HistServ accumulates
    // (sums) into whatever remote state already exists for hist_id -- this is
    // the "already-filled TH1 in, over the wire" entry point. Independent
    // contributing histograms (e.g. one per job) each call this once against
    // the same hist_id to merge server-side. See the class docs above for the
    // "full state, not a delta" caveat.
    //
    // `unique_id`, when non-empty, is forwarded to HistServ's existing
    // idempotency mechanism (the same one WasFilledWithUniqueId exposes): a
    // retried call with the same unique_id is rejected (ALREADY_EXISTS,
    // surfaced as HistServError::code()) rather than double-counting. No
    // separate delta-tracking is implemented on top of that -- reuse of the
    // existing mechanism is the point.
    void Fill(const std::string& hist_id, const TH1* h,
              const std::string& token = "", const std::string& unique_id = "");

private:
    std::unique_ptr<HistogrammerService::Stub> stub_;
};

} // namespace histserv_client
