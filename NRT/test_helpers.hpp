#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include "types.hpp"

namespace nrt {

#ifndef NRT_OUTPUT_DIR
#define NRT_OUTPUT_DIR "output" // fallback when compiled without the Makefile's -DNRT_OUTPUT_DIR
#endif

// A fresh, empty directory under NRT/output/ for a test that writes files (reports, snapshots...), 
inline std::filesystem::path fresh_output_dir(const std::string& tag) {
    static std::atomic<int> counter{0};
    std::filesystem::path dir = std::filesystem::path(NRT_OUTPUT_DIR) / (tag + "_" + std::to_string(counter++));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// Builds an OrderRequest with the given basics; STOP/LIMIT_STOP-specific fields (trigger_lower, trigger_upper, expires_in, not_before_in) are left unset here 
// and set directly on the returned struct by tests that need them, since positional parameters for every optional field would be unreadable at call sites
inline sim::OrderRequest make_order(sim::ClientId client, sim::Side side, sim::OrderKind kind, const sim::Symbol& symbol, sim::Quantity qty, sim::Price price = 0.0) {
    sim::OrderRequest request;
    request.client = client;
    request.side = side;
    request.kind = kind;
    request.symbol = symbol;
    request.quantity = qty;
    request.price = price;
    return request;
}

} // namespace nrt
