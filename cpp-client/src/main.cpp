// Minimal demo/PoC: fill ROOT histograms entirely locally (the normal, fast
// ROOT way), then push their finished state to a running HistServ server
// with HistServClient. Prints one "HIST_ID <label> <hist_id>" line per
// remote histogram created, so cpp-client/test/test_integration.py can
// parse them and cross-check against independently-built ROOT references.
#include <TH1D.h>
#include <TH2D.h>
#include <TProfile.h>
#include <TRandom3.h>

#include <iostream>
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

    const std::string merged_1d_id = client.Init(&h1, /*token=*/"");
    client.Fill(merged_1d_id, &h2, /*token=*/"", /*unique_id=*/"job2-run1");
    ExpectRejected(client, merged_1d_id, &h2, "job2-run1");
    std::cout << "HIST_ID merged_1d " << merged_1d_id << std::endl;

    // --- Unweighted, variable-bin-width 1D histogram ---
    double edges[] = {-5.0, -1.0, -0.5, 0.0, 0.5, 1.0, 5.0};
    TH1D h3("job3", "Unweighted, variable-width bins", 6, edges);
    TRandom3 rng3(7);
    for (int i = 0; i < 5000; ++i) h3.Fill(rng3.Gaus(0.0, 1.0));
    const std::string variable_1d_id = client.Init(&h3, /*token=*/"");
    std::cout << "HIST_ID variable_1d " << variable_1d_id << std::endl;

    // --- 2D, weighted histogram: exercises the multi-dimensional reindexing path ---
    TH2D h4("job4", "2D weighted", 8, -4.0, 4.0, 5, -2.5, 2.5);
    h4.Sumw2();
    TRandom3 rng4(99);
    for (int i = 0; i < 8000; ++i) {
        h4.Fill(rng4.Gaus(0.0, 1.5), rng4.Gaus(0.0, 1.0), 1.5);
    }
    const std::string weighted_2d_id = client.Init(&h4, /*token=*/"");
    std::cout << "HIST_ID weighted_2d " << weighted_2d_id << std::endl;

    ExpectProfileRejected(client);

    return 0;
}
