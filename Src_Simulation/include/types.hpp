//=======================================================================
// Core value types shared across the whole simulation engine.
// This header has no dependency other than the standard library on purpose:
// Src_Simulation is meant to be a fast, self-contained C++ matching engine
// with no SQL/network dependency, so it can be unit tested in isolation (see NRT/).
//=======================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace sim {

using OrderId = std::uint64_t;
using ClientId = std::int64_t;
using Symbol = std::string;
using Quantity = std::int64_t;
using Price = double;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class Side {BUY, SELL};

// MARKET: Immediate-Or-Cancel, matches against the best available price(s) right away: any quantity that cannot be filled immediately is discarded rather than resting in the book
// LIMIT:  Rests in the book at its own price until filled, cancelled or expired
// STOP:   Does not enter the book on submission. It waits for the symbol's reference (last traded) price to reach a trigger, then enters the book as a LIMIT order at `price`
//         Side-aware, like on a real exchange: a SELL stop (stop-loss) triggers when the price falls to or below the trigger: a buy stop (breakout / short cover) triggers when the price RISES to or above it. 
//         (Note: Src_SQL's STOP still uses a single "price <= trigger" rule for both sides)
// LIMIT_STOP: like STOP, but with an explicit [release_lower, release_upper] band instead of a single trigger, and a `price` that can differ from the band: a bracket/band order
//
// None of these ever use a placeholder numeric price to represent "no price yet" or "any price":
// - MARKET's `price` field is simply never consulted (see order_book.cpp), 
// - STOP/LIMIT_STOP use -infinity/+infinity for "no bound on this side", which can never be mistaken for a real price and can never leak into a settlement price
enum class OrderKind {MARKET, LIMIT, STOP, LIMIT_STOP};

inline const char* to_string(Side side) {
    return side == Side::BUY ? "BUY" : "SELL";
}

inline const char* to_string(OrderKind kind) {
    switch (kind) {
        case OrderKind::MARKET:     return "MARKET";
        case OrderKind::LIMIT:      return "LIMIT";
        case OrderKind::STOP:       return "STOP";
        case OrderKind::LIMIT_STOP: return "LIMIT_STOP";
    }
    return "?";
}

// A single order: `price` is the resting/limit price once the order is live in the book (unused for MARKET)
// `release_lower`/`release_upper` define the band the symbol's reference price must fall in before the order is allowed to enter the book at all: 
// - MARKET and plain LIMIT orders default to an unbounded band (released immediately on submission, matching prior behavior)
// - STOP/LIMIT_STOP narrow this band to implement trigger semantics. 
// - `not_before` is this order's "start date": it is held back even if its price band is already satisfied. 
// `expires_at` is its "expiry date": it is dropped if it hasn't been released into the book by then. 
struct Order {
    OrderId id{0};
    ClientId client{0};
    Side side{Side::BUY};
    OrderKind kind{OrderKind::LIMIT};
    Symbol symbol;
    Quantity quantity{0};
    Price price{0.0};
    Price release_lower{-std::numeric_limits<Price>::infinity()};
    Price release_upper{std::numeric_limits<Price>::infinity()};
    TimePoint submitted_at{};
    std::optional<TimePoint> not_before; // optional
    std::optional<TimePoint> expires_at; // optional

    bool release_band_contains(Price reference_price) const {
        return reference_price >= release_lower && reference_price <= release_upper;
    }
};

// Everything needed to submit a new order
struct OrderRequest {
    ClientId client{0};
    Side side{Side::BUY};
    OrderKind kind{OrderKind::LIMIT};
    Symbol symbol;
    Quantity quantity{0};
    Price price{0.0};                                  // required for LIMIT/STOP/LIMIT_STOP, ignored for MARKET
    std::optional<Price> trigger_lower;                // LIMIT_STOP only: release band lower bound
    std::optional<Price> trigger_upper;                // STOP/LIMIT_STOP: release band upper bound (STOP's trigger)
    std::optional<std::chrono::seconds> expires_in;    // relative "expiry date" from submission time
    std::optional<std::chrono::seconds> not_before_in; // relative "start date" from submission time
};

// A completed match between two orders. `price` is always the resting (book) order's real limit price, always a meaningful, tradable price (never a placeholder)
struct Trade {
    OrderId buy_order_id{0};
    OrderId sell_order_id{0};
    ClientId buyer{0};
    ClientId seller{0};
    Symbol symbol;
    Quantity quantity{0};
    Price price{0.0};
    TimePoint timestamp{};
};

// Outcome of MatchingEngine::submit_order(): whether the order was accepted by pre-trade risk checks,
// whether it matched immediately or is waiting for its release band/start date, its assigned id, and any trades it immediately produced
struct SubmitResult {
    bool accepted{false};
    bool queued{false}; // true if the order passed risk checks but is waiting (not yet in the book)
    std::string reject_reason;
    OrderId order_id{0};
    Quantity filled_quantity{0};
    std::vector<Trade> trades;
};

} // namespace sim
