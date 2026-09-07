// Minimal demo/PoC: fill ROOT histograms entirely locally (the normal, fast
// ROOT way), then push their finished state to a running HistServ server
// with HistServClient. Prints "HIST_ID <label> <hist_id>" and
// "HIST_VALUES <label> v0,v1,..." (and "HIST_VARIANCES <label> ..." for
// weighted histograms) lines so cpp-client/test/test_integration.py can
// cross-check the server's response against the ACTUAL local ground truth --
// deliberately not a second, independently-reseeded ROOT histogram: an
// earlier version of this test rebuilt references with a fresh
// ROOT.TRandom3(same seed) in Python, which turned out NOT to reliably
// reproduce the same draw sequence as this process's TRandom3 (observed
// mismatching, non-flaky-looking-until-investigated results on Linux CI --
// see git history). Dumping this process's own bin contents sidesteps that
// entirely and is a strictly more direct test of "did the server receive and
// return what we actually sent."
#include <TH1D.h>
#include <TH2D.h>
#include <TProfile.h>
#include <TRandom3.h>

#include <array>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "hist_serv_client.hpp"

namespace {

void ExpectRejected(histserv_client::HistServClient& client, const std::string& hist_id,
                     const TH1* h, const std::string& unique_id) {
    try {
        client.Fill(hist_id, h, /*token=*/"", unique_id);
        std::cerr << "ERROR: duplicate unique_id was not rejected" << std::endl;
        std::exit(1);
    } catch (const histserv_client::HistServError& e) {
        if (e.code() != grpc::StatusCode::ALREADY_EXISTS) {
            std::cerr << "ERROR: expected ALREADY_EXISTS, got a different status: " << e.what() << std::endl;
            std::exit(1);
        }
        std::cout << "Retry with the same unique_id correctly rejected: " << e.what() << std::endl;
    }
}

void ExpectProfileRejected(histserv_client::HistServClient& client) {
    TProfile profile("profile", "TProfile is not supported", 10, 0.0, 10.0);
    profile.Fill(1.0, 5.0);
    try {
        client.Init(&profile);
        std::cerr << "ERROR: TProfile was not rejected" << std::endl;
        std::exit(1);
    } catch (const std::invalid_argument& e) {
        std::cout << "TProfile correctly rejected: " << e.what() << std::endl;
    }
}

// Prints this histogram's own bin contents (and variances, if Sumw2 is
// active) in exactly the row-major, axis-0-outermost order SerializeTH1
// writes them in, so the test can compare against the flattened server
// response without needing to independently reconstruct the same data.
void DumpBins(const std::string& label, const TH1* h) {
    const int ndim = h->GetDimension();
    std::array<const TAxis*, 3> axis = {h->GetXaxis(), nullptr, nullptr};
    if (ndim >= 2) axis[1] = h->GetYaxis();
    if (ndim >= 3) axis[2] = h->GetZaxis();

    std::array<int, 3> ncells = {1, 1, 1};
    for (int d = 0; d < ndim; ++d) ncells[d] = axis[d]->GetNbins() + 2;

    const bool weighted = h->GetSumw2N() > 0;
    std::ostringstream values;
    std::ostringstream variances;
    char buf[64];
    for (int i0 = 0; i0 < ncells[0]; ++i0) {
        for (int i1 = 0; i1 < ncells[1]; ++i1) {
            for (int i2 = 0; i2 < ncells[2]; ++i2) {
                const int bin = h->GetBin(i0, i1, i2);
                std::snprintf(buf, sizeof(buf), "%.17g", h->GetBinContent(bin));
                values << buf << ",";
                if (weighted) {
                    std::snprintf(buf, sizeof(buf), "%.17g", h->GetSumw2()->At(bin));
                    variances << buf << ",";
                }
            }
        }
    }
    std::cout << "HIST_VALUES " << label << " " << values.str() << std::endl;
    if (weighted) {
        std::cout << "HIST_VARIANCES " << label << " " << variances.str() << std::endl;
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string target = argc > 1 ? argv[1] : "localhost:50051";

    auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    histserv_client::HistServClient client(channel);

    // --- Two independent "jobs" merging into one weighted, fixed-bin 1D histogram ---
    TH1D h1("job1", "Filled locally by job 1", 20, -5.0, 5.0);
    h1.Sumw2();
    TRandom3 rng1(42);
    for (int i = 0; i < 20000; ++i) h1.Fill(rng1.Gaus(0.0, 1.0));

    TH1D h2("job2", "Filled locally by job 2", 20, -5.0, 5.0);
    h2.Sumw2();
    TRandom3 rng2(1337);
    for (int i = 0; i < 10000; ++i) h2.Fill(rng2.Gaus(0.5, 1.5));

    TH1D merged(h1);
    merged.Add(&h2); // TH1::Add sums bin contents AND fSumw2 (variances) correctly

    const std::string merged_1d_id = client.Init(&h1, /*token=*/"");
    client.Fill(merged_1d_id, &h2, /*token=*/"", /*unique_id=*/"job2-run1");
    ExpectRejected(client, merged_1d_id, &h2, "job2-run1");
    std::cout << "HIST_ID merged_1d " << merged_1d_id << std::endl;
    DumpBins("merged_1d", &merged);

    // --- Unweighted, variable-bin-width 1D histogram ---
    double edges[] = {-5.0, -1.0, -0.5, 0.0, 0.5, 1.0, 5.0};
    TH1D h3("job3", "Unweighted, variable-width bins", 6, edges);
    TRandom3 rng3(7);
    for (int i = 0; i < 5000; ++i) h3.Fill(rng3.Gaus(0.0, 1.0));
    const std::string variable_1d_id = client.Init(&h3, /*token=*/"");
    std::cout << "HIST_ID variable_1d " << variable_1d_id << std::endl;
    DumpBins("variable_1d", &h3);

    // --- 2D, weighted histogram: exercises the multi-dimensional reindexing path ---
    TH2D h4("job4", "2D weighted", 8, -4.0, 4.0, 5, -2.5, 2.5);
    h4.Sumw2();
    TRandom3 rng4(99);
    for (int i = 0; i < 8000; ++i) {
        h4.Fill(rng4.Gaus(0.0, 1.5), rng4.Gaus(0.0, 1.0), 1.5);
    }
    const std::string weighted_2d_id = client.Init(&h4, /*token=*/"");
    std::cout << "HIST_ID weighted_2d " << weighted_2d_id << std::endl;
    DumpBins("weighted_2d", &h4);

    ExpectProfileRejected(client);

    return 0;
}
