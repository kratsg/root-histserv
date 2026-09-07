#include "hist_serv_client.hpp"

#include "root_hist_serialize.hpp"

#include <stdexcept>
#include <utility>

namespace histserv_client {
namespace {

// grpc::ClientContext is neither copyable nor movable, so it can't be
// returned by value (NRVO isn't guaranteed) -- take it by reference instead.
void ApplyToken(grpc::ClientContext& ctx, const std::string& token) {
    if (!token.empty()) {
        ctx.AddMetadata("x-histserv-token", token);
    }
}

} // namespace

HistServClient::HistServClient(std::shared_ptr<grpc::Channel> channel)
    : stub_(HistogrammerService::NewStub(std::move(channel))) {}

std::string HistServClient::Init(const TH1* h, const std::string& token) {
    InitRequest request;
    *request.mutable_payload() = SerializeTH1(h);

    InitResponse response;
    grpc::ClientContext ctx;
    ApplyToken(ctx, token);
    grpc::Status status = stub_->Init(&ctx, request, &response);
    if (!status.ok()) {
        throw HistServError(status.error_code(), "HistServ Init RPC failed: " + status.error_message());
    }
    return response.hist_id();
}

void HistServClient::Fill(const std::string& hist_id, const TH1* h,
                          const std::string& token, const std::string& unique_id) {
    ChunkedHistPayload payload = SerializeTH1(h);

    FillManyRequest request;
    request.set_hist_id(hist_id);
    if (!unique_id.empty()) {
        request.set_unique_id(unique_id);
    }
    for (auto& chunk : *payload.mutable_chunks()) {
        *request.add_chunks() = std::move(chunk);
    }

    FillResponse response;
    grpc::ClientContext ctx;
    ApplyToken(ctx, token);
    grpc::Status status = stub_->FillMany(&ctx, request, &response);
    if (!status.ok()) {
        throw HistServError(status.error_code(), "HistServ FillMany RPC failed: " + status.error_message());
    }
}

} // namespace histserv_client
