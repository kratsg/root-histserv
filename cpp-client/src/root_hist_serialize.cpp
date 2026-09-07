#include "root_hist_serialize.hpp"

#include <TArrayD.h>
#include <TAxis.h>
#include <TH1.h>

#include <nlohmann/json.hpp>

#include <array>
#include <stdexcept>
#include <vector>

namespace histserv_client {
namespace {

// HistServ's ChunkedHist requires every axis to be named (it rejects unnamed
// boost-histogram axes at construction). ROOT's default TAxis names are the
// uninformative "xaxis"/"yaxis"/"zaxis", so fall back to a fixed placeholder
// in that case.
nlohmann::json AxisSchema(const TAxis* axis, const std::string& fallback_name) {
    std::string name = axis->GetName();
    if (name.empty() || name == "xaxis" || name == "yaxis" || name == "zaxis") {
        name = fallback_name;
    }

    nlohmann::json j;
    if (axis->IsVariableBinSize()) {
        const TArrayD* xbins = axis->GetXbins();
        std::vector<double> edges(xbins->GetArray(), xbins->GetArray() + xbins->GetSize());
        j["type"] = "variable";
        j["edges"] = edges;
    } else {
        j["type"] = "regular";
        j["lower"] = axis->GetXmin();
        j["upper"] = axis->GetXmax();
        j["bins"] = axis->GetNbins();
    }
    // ROOT's TAxis always has both flow bins addressable (bin 0 / nbins+1),
    // and has no circular-axis concept in the classic TH1 API.
    j["underflow"] = true;
    j["overflow"] = true;
    j["circular"] = false;
    j["metadata"] = {{"name", name}, {"label", std::string(axis->GetTitle())}};
    return j;
}

} // namespace

ChunkedHistPayload SerializeTH1(const TH1* h) {
    // TProfile/TProfile2D/TProfile3D are TH1-derived but GetBinContent()
    // returns a per-bin MEAN, not a sum -- summing two profiles' means
    // server-side (which is all HistServ's accumulate-in-place merge does)
    // would silently produce a meaningless result. HistServ's own storage
    // layer already rejects "mean"/"weighted_mean" storage
    // (ChunkedHist._validate_supported_storage), but this client never tags
    // a profile as such, so that guard would never fire -- reject explicitly
    // here instead.
    if (h->InheritsFrom("TProfile") || h->InheritsFrom("TProfile2D") || h->InheritsFrom("TProfile3D")) {
        throw std::invalid_argument("SerializeTH1 does not support TProfile/TProfile2D/TProfile3D "
                                     "(bin content is a mean, not a sum -- summing means server-side "
                                     "would be meaningless)");
    }

    const int ndim = h->GetDimension();
    if (ndim < 1 || ndim > 3) {
        throw std::invalid_argument("SerializeTH1 only supports 1D/2D/3D histograms (TH1/TH2/TH3)");
    }

    static const std::array<const char*, 3> kFallbackNames = {"x", "y", "z"};
    std::array<const TAxis*, 3> axis = {h->GetXaxis(), nullptr, nullptr};
    if (ndim >= 2) axis[1] = h->GetYaxis();
    if (ndim >= 3) axis[2] = h->GetZaxis();

    // ncells (bins + 2 flow bins) per axis, in declaration order (x, [y, [z]]).
    std::array<int, 3> ncells = {1, 1, 1};
    nlohmann::json axes_json = nlohmann::json::array();
    for (int d = 0; d < ndim; ++d) {
        ncells[d] = axis[d]->GetNbins() + 2;
        axes_json.push_back(AxisSchema(axis[d], kFallbackNames[d]));
    }

    const bool weighted = h->GetSumw2N() > 0;
    nlohmann::json schema;
    schema["uhi_schema"] = 1;
    schema["axes"] = axes_json;
    schema["storage"]["type"] = weighted ? "weighted" : "double";
    schema["metadata"] = {{"name", std::string(h->GetName())}, {"label", std::string(h->GetTitle())}};

    const long total_cells = static_cast<long>(ncells[0]) * ncells[1] * ncells[2];
    const std::size_t doubles_per_cell = weighted ? 2 : 1;
    // Write into a properly-typed double buffer, not a reinterpret_cast'd
    // std::string buffer: writing a double through a pointer reinterpreted
    // from a char-typed (std::string) buffer violates the strict-aliasing
    // rule, and different compilers/platforms are free to (and in practice
    // do) miscompile that differently. Reading a double buffer's bytes back
    // out through a char pointer, done below, is well-defined.
    std::vector<double> buffer(static_cast<std::size_t>(total_cells) * doubles_per_cell, 0.0);
    double* out = buffer.data();

    // Row-major over (i0, i1, i2) with axis 0 slowest-varying -- this matches
    // boost-histogram's dense view for a histogram whose axes were declared
    // in (x, y, z) order, e.g. hist.Hist.new.Reg(x).Reg(y).Reg(z)..., which is
    // the TRANSPOSE of ROOT's OWN internal linear storage: TH2::GetBin(bx,by)
    // = by*(nx+2)+bx stores y as the slower-varying index, not x. Verified
    // empirically (not just by inspection): ROOT's raw TH2 buffer order and a
    // fresh 2D hist.Hist's dense view disagree unless this loop explicitly
    // reindexes via GetBin()/GetBinContent()/GetBinError() rather than
    // copying ROOT's raw buffer.
    for (int i0 = 0; i0 < ncells[0]; ++i0) {
        for (int i1 = 0; i1 < ncells[1]; ++i1) {
            for (int i2 = 0; i2 < ncells[2]; ++i2) {
                const int bin = h->GetBin(i0, i1, i2);
                const long linear = (static_cast<long>(i0) * ncells[1] + i1) * ncells[2] + i2;
                const double content = h->GetBinContent(bin);
                if (weighted) {
                    // weighted == (GetSumw2N() > 0), so fSumw2 is populated here.
                    out[2 * linear] = content;
                    // Read variance from fSumw2 directly via the public
                    // GetSumw2() accessor (this replicates TH1's own
                    // protected GetBinErrorSqUnchecked, which isn't callable
                    // from outside a TH1 subclass) rather than computing
                    // GetBinError(bin) and squaring it: sqrt-then-square is
                    // an unnecessary floating-point round trip that does not
                    // exactly invert for roughly half of all doubles.
                    out[2 * linear + 1] = h->GetSumw2()->At(bin);
                } else {
                    out[linear] = content;
                }
            }
        }
    }

    ChunkedHistPayload payload;
    payload.set_hist_json(schema.dump());
    ChunkPayload* chunk = payload.add_chunks(); // no categorical axes -> one chunk, empty key
    chunk->set_dense_view(reinterpret_cast<const char*>(buffer.data()), buffer.size() * sizeof(double));
    return payload;
}

} // namespace histserv_client
