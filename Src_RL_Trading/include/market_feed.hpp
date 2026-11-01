//=======================================================================
// MarketFeed: streams the trading days to the policy server, ONE BAR PER SYMBOL PER DAY
//
// At every close (simulation.x --days, after the closing auction), the simulation hands the day's bars to push_bars(): 
// for every symbol one OBSERVE with the close, the book when the pre-close started (best bid and ask, the quantity in the top 10 levels of each side), the day's volume, open, high, low and number of trades
// The server keeps these as one rolling stream per symbol: what the RL agents' observations are built from
//=======================================================================
#ifndef MARKET_FEED_HPP
#define MARKET_FEED_HPP

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "market_session.hpp"
#include "policy_client.hpp"

namespace sim {

class MarketFeed {
public:
    MarketFeed() = default;

    MarketFeed(const MarketFeed&) = delete;
    MarketFeed& operator=(const MarketFeed&) = delete;

    bool connect(const std::string& host, int port); // false if the server can't be reached
    void push_bars(const std::vector<DailyBar>& bars); // the day's bars, at its close
    std::uint64_t bars_sent() const {return sent_.load();}
    std::uint64_t days_sent() const {return days_.load();}

private:
    PolicyClient client_;
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> days_{0};
};

} // namespace sim

#endif // MARKET_FEED_HPP
