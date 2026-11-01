#include "market_seed.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace sim {

namespace {

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    std::istringstream iss(line);
    while (std::getline(iss, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

double parse_double_or(const std::string& text, double fallback) {
    if (text.empty()) return fallback;
    try {
        return std::stod(text);
    } catch (...) {
        return fallback;
    }
}

} // namespace

std::vector<SeedSymbol> load_market_seed(const std::string& csv_path) {
    std::vector<SeedSymbol> result;

    std::ifstream in(csv_path);
    if (!in.is_open()) {
        return result; // no file: not an error, caller falls back to defaults
    }

    std::string header_line;
    if (!std::getline(in, header_line)) {
        return result; // empty file
    }
    std::vector<std::string> header = split_csv_line(header_line);

    // locate each column by name rather than assuming a fixed order, so this keeps working if feature_engineering.py's column order ever changes
    auto column_index = [&header](const std::string& name) -> int {
        auto it = std::find(header.begin(), header.end(), name);
        return it == header.end() ? -1 : static_cast<int>(std::distance(header.begin(), it));
    };
    int ticker_col = column_index("ticker");
    int price_col = column_index("latest_price");
    int volatility_col = column_index("rolling_volatility_20");

    if (ticker_col < 0 || price_col < 0) {
        return result; // not the file we expect; refuse to guess
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fields = split_csv_line(line);
        if (static_cast<int>(fields.size()) <= std::max(ticker_col, price_col)) continue;

        SeedSymbol seed;
        seed.symbol = fields[ticker_col];
        seed.last_price = parse_double_or(fields[price_col], 0.0);
        if (volatility_col >= 0 && static_cast<int>(fields.size()) > volatility_col) {
            seed.rolling_volatility_20 = parse_double_or(fields[volatility_col], 0.0);
        }

        if (seed.symbol.empty() || seed.last_price <= 0.0) continue; // unusable row, skip it
        result.push_back(std::move(seed));
    }

    return result;
}

std::vector<SeedSymbol> pick_symbols(const std::vector<SeedSymbol>& all, std::size_t max_symbols) {
    std::vector<SeedSymbol> picked;
    picked.reserve(std::min(max_symbols, all.size()));
    for (std::size_t i = 0; i < all.size() && picked.size() < max_symbols; ++i) {
        picked.push_back(all[i]);
    }
    return picked;
}

} // namespace sim
