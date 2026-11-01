//=======================================================================
// Trading days: the simulated market opens, trades, closes and reopens the next day, like a real exchange (simulation.x --days)
//
// One day of --day-ms milliseconds:
//   PRE_OPEN   the first --pre-open-ms: orders are collected without matching (the market makers quote, MARKET orders join
//              at their collar), with the GTC orders carried from the day before, then the opening auction sets the open
//              Without it continuous trading started on a book emptied by the close (every DAY quote expired): the first
//              market orders swept it to the 10% collar, through the stop-losses and the take-profits at once
//   OPEN       continuous trading: the bots trade, STOP orders trigger
//   PRE_CLOSE  the last --pre-close-ms: orders are accepted but nothing matches (MatchingEngine auction mode)
//   CLOSED     the closing auction sets each symbol's official close, every DAY order expires, the day's bar is recorded and
//              pushed to the policy server; for --overnight-ms the bots are paused (an exchange is closed at night), the
//              reference prices may gap (--overnight-gap-vol: overnight news), then the next day opens
// GTC orders (the RL bots' stop-losses and take-profits) stay from one day to the next, their clock only counts trading time
//
// The day's bar: open (first trade), high, low, close (the closing auction), volume, VWAP, number of trades, the book just
// before the pre-close (best bid / ask, top-10 depth) and the overnight gap that preceded it: daily_bars_simu.csv
// An RL agent decides once per day on these bars, after the close, and its orders reach the market at the next open
//=======================================================================
#ifndef MARKET_SESSION_HPP
#define MARKET_SESSION_HPP

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "notification.hpp"
#include "types.hpp"

namespace sim {

enum class SessionPhase {PRE_OPEN, OPEN, PRE_CLOSE, CLOSED};

inline const char* to_string(SessionPhase phase) {
    switch (phase) {
        case SessionPhase::PRE_OPEN:  return "PRE_OPEN";
        case SessionPhase::OPEN:      return "OPEN";
        case SessionPhase::PRE_CLOSE: return "PRE_CLOSE";
        case SessionPhase::CLOSED:    return "CLOSED";
    }
    return "?";
}

struct SessionConfig {
    int days{60};                                 // trading days in the run (at least 1)
    std::chrono::milliseconds day{4000};          // one trading day
    std::chrono::milliseconds pre_open{400};      // its first part: orders collected for the opening auction
    std::chrono::milliseconds pre_close{400};     // its last part: orders collected for the closing auction
    std::chrono::milliseconds overnight{100};     // the market closed between two days (bots paused)
    double overnight_gap_vol{0.0};                // standard deviation of the overnight log return (0: the next day opens at the close)
    std::chrono::milliseconds total() const {return days * (day + overnight);}
};

// the market's clock, shared by the simulation (which moves it) and the bots (which read it)
class MarketSession {
public:
    int day() const {return day_.load();}                         // the current (or last) trading day, -1 before the first open
    SessionPhase phase() const {return phase_.load();}
    bool accepting_orders() const {auto p = phase(); return p != SessionPhase::CLOSED;}
    bool continuous_trading() const {return phase() == SessionPhase::OPEN;}
    void set(int day, SessionPhase phase) {day_.store(day); phase_.store(phase);}

private:
    std::atomic<int> day_{-1};
    std::atomic<SessionPhase> phase_{SessionPhase::CLOSED}; // until the first day opens
};

struct DailyBar {
    int day{0};
    Symbol symbol;
    double open_ms{0.0};   // since the start of the run
    double close_ms{0.0};
    Price open{0.0};       // the opening auction, or the first trade of the day (the previous close when nothing traded)
    Price high{0.0};
    Price low{0.0};
    Price close{0.0};      // the closing auction, or the last trade when nothing crossed
    Quantity volume{0};
    double vwap{0.0};
    int trades{0};
    Price bid{0.0};        // the book when the pre-close started
    Price ask{0.0};
    double bid_depth{0.0}; // top 10 levels
    double ask_depth{0.0};
    Quantity auction_volume{0};
    double gap{0.0};       // overnight log return applied before this day opened
};

// builds the day's bars from the trades the bus delivers
class DailyBarBuilder {
public:
    explicit DailyBarBuilder(NotificationBus& bus);
    ~DailyBarBuilder();
    DailyBarBuilder(const DailyBarBuilder&) = delete;
    DailyBarBuilder& operator=(const DailyBarBuilder&) = delete;

    // a new day: `reference` = each symbol's price at the open (the previous close, gapped), `gaps` = the gap applied
    void start_day(int day, double now_ms, const std::unordered_map<Symbol, Price>& reference, const std::unordered_map<Symbol, double>& gaps);
    // the book when the pre-close starts
    void set_book(const Symbol& symbol, Price bid, Price ask, double bid_depth, double ask_depth);
    // after the closing auction (call bus.flush() first, so every trade of the day was counted): the day's bars, also kept in all()
    std::vector<DailyBar> finish_day(double now_ms, const std::unordered_map<Symbol, Price>& closes, const std::unordered_map<Symbol, Quantity>& auction_volumes);
    std::vector<DailyBar> all() const;
    // writes daily_bars<suffix>.csv into dir, false if it can't
    bool write_csv(const std::string& dir, const std::string& suffix) const;

private:
    NotificationBus& bus_;
    SubscriptionId subscription_{0};
    mutable std::mutex mutex_;
    bool in_day_{false};
    std::unordered_map<Symbol, DailyBar> current_;
    std::unordered_map<Symbol, double> notional_;
    std::vector<DailyBar> done_;
};

} // namespace sim

#endif // MARKET_SESSION_HPP
