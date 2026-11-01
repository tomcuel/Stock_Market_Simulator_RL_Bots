//=======================================================================
// What a trading strategy can see and do, independently of how it reaches the market
// A strategy (see bot.hpp) only talks to a MarketGateway, so the exact same strategy code runs:
//   in-process  (InProcessGateway, below): direct calls into the MatchingEngine, used by simulation.x
//   over TCP    (WireGateway, net/wire_gateway.hpp): ORDER / CANCEL / MARKET / PORTFOLIO commands through the socket protocol, used by sim_client.x --bot
// Before this interface existed, every strategy was written twice (once in bot.cpp, once as wire commands in sim_client_main.cpp) and the two versions drifted apart
//=======================================================================
#ifndef GATEWAY_HPP
#define GATEWAY_HPP

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "portfolio.hpp"
#include "types.hpp"

namespace sim {

class MatchingEngine;

// top of the book for one symbol, 0 for anything unknown (no trade yet, empty side)
struct Quote {
    Price last{0.0};
    Price best_bid{0.0};
    Price best_ask{0.0};

    // mid of the book when both sides exist, otherwise the last price
    Price mid() const { return (best_bid > 0.0 && best_ask > 0.0) ? (best_bid + best_ask) / 2.0 : last; }
};

class MarketGateway {
public:
    virtual ~MarketGateway() = default;

    virtual ClientId client() const = 0;
    virtual std::vector<Symbol> symbols() = 0;
    virtual Quote quote(const Symbol& symbol) = 0;
    virtual Portfolio portfolio() = 0;
    // request.client is filled in by the gateway
    virtual SubmitResult submit(OrderRequest request) = 0;
    // retracts an open order, whether it rests in the book or still waits for its trigger.
    // false if it no longer exists (already filled, expired or cancelled)
    virtual bool cancel(const Symbol& symbol, OrderId order_id) = 0;

    // what an order of this client has traded so far, wherever it traded (continuous trading, an auction, a released stop),
    // once fills are tracked (track_fills): 0 shares when the gateway can't tell (the wire gateway)
    struct Fill {
        Quantity quantity{0};
        double notional{0.0};
        Price average() const {return quantity > 0 ? notional / static_cast<double>(quantity) : 0.0;}
    };
    virtual void track_fills() {}
    virtual Fill filled(OrderId) {return {};}
};

// direct calls into a MatchingEngine, on behalf of one client
class InProcessGateway : public MarketGateway {
public:
    InProcessGateway(MatchingEngine& engine, ClientId client) : engine_(engine), client_(client) {}
    ~InProcessGateway() override;

    ClientId client() const override {return client_;}
    std::vector<Symbol> symbols() override;
    Quote quote(const Symbol& symbol) override;
    Portfolio portfolio() override;
    SubmitResult submit(OrderRequest request) override;
    bool cancel(const Symbol& symbol, OrderId order_id) override;
    // records this client's fills from the engine's trade events (the RL bots: their exact entry and exit prices).
    // The events arrive through the bus's thread, a moment after the trade
    void track_fills() override;
    Fill filled(OrderId order_id) override;

private:
    MatchingEngine& engine_;
    ClientId client_;
    std::uint64_t subscription_{0};
    bool tracking_{false};
    std::mutex fills_mutex_;
    std::unordered_map<OrderId, Fill> fills_;
};

} // namespace sim

#endif // GATEWAY_HPP
