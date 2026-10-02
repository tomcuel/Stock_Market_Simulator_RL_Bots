//=======================================================================
// Reads Data/Datasets/processed/latest_snapshot.csv (produced by Data/feature_engineering.py) to seed simulated symbols with real prices and volatility instead of arbitrary made-up numbers
// Deliberately a tiny hand-rolled CSV reader rather than a dependency: the file's format is simple (no embedded commas/quotes) and fixed by feature_engineering.py's own output code, so a plain comma-split is both correct and enough
// (this will evolve, to include the real history of each symbol and not the initial snapshot for more advanced bots and simulations)
//=======================================================================
#pragma once

#include <string>
#include <vector>

#include "types.hpp"

namespace sim {

struct SeedSymbol {
    Symbol symbol;
    Price last_price{0.0};
    double rolling_volatility_20{0.0}; // annualized, e.g. 0.25 == 25%/year; 0 if unavailable
};

// Returns one SeedSymbol per usable row in `csv_path`
// Returns an empty vector if the file doesn't exist or has no usable rows: callers should fall back to a hardcoded default symbol list in that case rather than failing to start
std::vector<SeedSymbol> load_market_seed(const std::string& csv_path);

// Deterministically picks up to `max_symbols` entries from `all` (in the order they appear in the CSV), for callers that don't want to run a simulation against every single ticker in the file
std::vector<SeedSymbol> pick_symbols(const std::vector<SeedSymbol>& all, std::size_t max_symbols);

} // namespace sim
